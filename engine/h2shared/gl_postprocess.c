/* gl_postprocess.c -- GLSL post-process gamma/contrast correction
 *
 * Renders the scene into an FBO, then blits to the default framebuffer
 * with a shader that applies gamma and contrast.  When both gamma and
 * contrast are 1.0 (identity), the FBO is not used and there is zero
 * performance cost.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "sdl_inc.h"
#include "gl_postprocess.h"
#include "gl_matrix.h"
#include "gl_shader.h"
#include "gl_vbo.h"

/* FBO state — scaled 3D scene */
static GLuint	pp_fbo;		/* render target (may be multisampled) */
static GLuint	pp_color_rb;	/* multisampled color renderbuffer (0 if no MSAA) */
static GLuint	pp_depth_rb;
static GLuint	pp_resolve_fbo;	/* resolve target (non-multisampled, for shader blit) */
static GLuint	pp_color_tex;	/* resolved color texture */
static int	pp_width, pp_height;
static int	pp_samples;	/* MSAA sample count (0 or 1 = no MSAA) */

/* FBO state — native res for 2D composite */
static GLuint	pp_native_fbo;
static GLuint	pp_native_color_tex;
static GLuint	pp_native_depth_rb;
static int	pp_native_w, pp_native_h;

/* Shader state */
static GLuint	pp_program;
static GLint	pp_loc_scene;
static GLint	pp_loc_gamma;
static GLint	pp_loc_contrast;
static GLint	pp_loc_mvp;
static GLint	pp_loc_softemu;
static GLint	pp_loc_dither;
static GLint	pp_loc_paletteLUT;
static GLint	pp_loc_palette;
static GLint	pp_loc_scale;

/* Palette LUT state */
static GLuint	pp_palette_lut;	/* 32x32x32 3D texture */
static qboolean	pp_lut_built;

static qboolean	pp_initialized;
static qboolean	pp_active;	/* true when scene is being rendered to FBO this frame */
static int	pp_saved_glwidth, pp_saved_glheight;	/* original viewport dims */

cvar_t	r_scale = {"r_scale", "1", CVAR_ARCHIVE};
cvar_t	r_softemu = {"r_softemu", "0", CVAR_ARCHIVE};
cvar_t	r_dither = {"r_dither", "1.0", CVAR_ARCHIVE};	/* dither strength (0-2) */

/* ------------------------------------------------------------------ */

static qboolean PP_NeedsPostProcess (void)
{
	if (!pp_initialized)
		return false;
	if (v_gamma.value != 1.0f || v_contrast.value != 1.0f)
		return true;
	if (r_scale.value < 1.0f)
		return true;
	if (r_softemu.value > 0)
		return true;
	return false;
}

/* ------------------------------------------------------------------ */
/* FBO management                                                      */
/* ------------------------------------------------------------------ */

static void PP_DeleteFBO (void)
{
	if (pp_color_tex)   { glDeleteTextures_fp(1, &pp_color_tex); pp_color_tex = 0; }
	if (pp_color_rb)    { glDeleteRenderbuffers_fp(1, &pp_color_rb); pp_color_rb = 0; }
	if (pp_depth_rb)    { glDeleteRenderbuffers_fp(1, &pp_depth_rb); pp_depth_rb = 0; }
	if (pp_resolve_fbo) { glDeleteFramebuffers_fp(1, &pp_resolve_fbo); pp_resolve_fbo = 0; }
	if (pp_fbo)         { glDeleteFramebuffers_fp(1, &pp_fbo); pp_fbo = 0; }
	pp_width = pp_height = pp_samples = 0;
}

static void PP_DeleteNativeFBO (void)
{
	if (pp_native_color_tex) { glDeleteTextures_fp(1, &pp_native_color_tex); pp_native_color_tex = 0; }
	if (pp_native_depth_rb)  { glDeleteRenderbuffers_fp(1, &pp_native_depth_rb); pp_native_depth_rb = 0; }
	if (pp_native_fbo)       { glDeleteFramebuffers_fp(1, &pp_native_fbo); pp_native_fbo = 0; }
	pp_native_w = pp_native_h = 0;
}

static qboolean PP_CreateNativeFBO (int width, int height)
{
	GLenum status;

	if (width == pp_native_w && height == pp_native_h && pp_native_fbo)
		return true;	/* already correct size */

	PP_DeleteNativeFBO();

	glGenTextures_fp(1, &pp_native_color_tex);
	glBindTexture_fp(GL_TEXTURE_2D, pp_native_color_tex);
	glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture_fp(GL_TEXTURE_2D, 0);

	glGenRenderbuffers_fp(1, &pp_native_depth_rb);
	glBindRenderbuffer_fp(GL_RENDERBUFFER, pp_native_depth_rb);
	glRenderbufferStorage_fp(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glBindRenderbuffer_fp(GL_RENDERBUFFER, 0);

	glGenFramebuffers_fp(1, &pp_native_fbo);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_native_fbo);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_TEXTURE_2D, pp_native_color_tex, 0);
	glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				     GL_RENDERBUFFER, pp_native_depth_rb);

	status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		PP_DeleteNativeFBO();
		return false;
	}

	pp_native_w = width;
	pp_native_h = height;
	return true;
}

static qboolean PP_CreateFBO (int width, int height)
{
	GLenum status;
	int samples = (int)Cvar_VariableValue("vid_config_fsaa");

	PP_DeleteFBO();

	/* resolve texture (always non-multisampled — this is what the shader reads) */
	glGenTextures_fp(1, &pp_color_tex);
	glBindTexture_fp(GL_TEXTURE_2D, pp_color_tex);
	glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glBindTexture_fp(GL_TEXTURE_2D, 0);

	if (samples > 1 && glRenderbufferStorageMultisample_fp && glBlitFramebuffer_fp)
	{
		/* multisampled render FBO */
		glGenRenderbuffers_fp(1, &pp_color_rb);
		glBindRenderbuffer_fp(GL_RENDERBUFFER, pp_color_rb);
		glRenderbufferStorageMultisample_fp(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);

		glGenRenderbuffers_fp(1, &pp_depth_rb);
		glBindRenderbuffer_fp(GL_RENDERBUFFER, pp_depth_rb);
		glRenderbufferStorageMultisample_fp(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width, height);
		glBindRenderbuffer_fp(GL_RENDERBUFFER, 0);

		glGenFramebuffers_fp(1, &pp_fbo);
		glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_fbo);
		glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					     GL_RENDERBUFFER, pp_color_rb);
		glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
					     GL_RENDERBUFFER, pp_depth_rb);

		status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			Con_Printf("PostProcess: MSAA FBO incomplete (status 0x%x), falling back\n", status);
			PP_DeleteFBO();
			samples = 0;
			/* fall through to non-MSAA path below */
		}
		else
		{
			/* resolve FBO (non-multisampled texture) */
			glGenFramebuffers_fp(1, &pp_resolve_fbo);
			glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_resolve_fbo);
			glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
						  GL_TEXTURE_2D, pp_color_tex, 0);

			status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
			glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);

			if (status != GL_FRAMEBUFFER_COMPLETE)
			{
				Con_Printf("PostProcess: resolve FBO incomplete\n");
				PP_DeleteFBO();
				return false;
			}

			pp_samples = samples;
			pp_width = width;
			pp_height = height;
			Con_DPrintf("PostProcess: %dx%d FBO with %dx MSAA\n", width, height, samples);
			return true;
		}
	}

	/* non-MSAA path */
	glGenRenderbuffers_fp(1, &pp_depth_rb);
	glBindRenderbuffer_fp(GL_RENDERBUFFER, pp_depth_rb);
	glRenderbufferStorage_fp(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glBindRenderbuffer_fp(GL_RENDERBUFFER, 0);

	glGenFramebuffers_fp(1, &pp_fbo);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_fbo);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_TEXTURE_2D, pp_color_tex, 0);
	glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				     GL_RENDERBUFFER, pp_depth_rb);

	status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_Printf("PostProcess: FBO incomplete (status 0x%x)\n", status);
		PP_DeleteFBO();
		return false;
	}

	pp_samples = 0;
	pp_width = width;
	pp_height = height;
	return true;
}

/* ------------------------------------------------------------------ */
/* Shader                                                              */
/* ------------------------------------------------------------------ */

static const char pp_vert_src[] =
	"#version 430 core\n"
	"layout(location = 0) in vec3 a_position;\n"
	"layout(location = 1) in vec2 a_texcoord;\n"
	"out vec2 v_texcoord;\n"
	"uniform mat4 u_mvp;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char pp_frag_src[] =
	"#version 430 core\n"
	"uniform sampler2D scene;\n"
	"uniform float gamma;\n"
	"uniform float contrast;\n"
	"uniform int softemu;\n"
	"uniform float dither;\n"
	"uniform sampler3D paletteLUT;\n"
	"uniform vec3 palette[256];\n"
	"uniform float scale;\n"
	"in vec2 v_texcoord;\n"
	"out vec4 fragColor;\n"
	"\n"
	"float bayer16(ivec2 c) {\n"
	"    c &= 15;\n"
	"    c.y ^= c.x;\n"
	"    uint v = uint(c.y | (c.x << 8));\n"
	"    v = (v ^ (v << 2)) & 0x3333u;\n"
	"    v = (v ^ (v << 1)) & 0x5555u;\n"
	"    v |= v >> 7;\n"
	"    v = bitfieldReverse(v) >> 24;\n"
	"    return float(v) / 256.0 - 0.5;\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    vec4 color = texture(scene, v_texcoord);\n"
	"    color.rgb = (color.rgb - 0.5) * contrast + 0.5;\n"
	/* gamma cvar: 1.0 = no change, <1.0 = brighter (matches
	 * the original Hexen2 brightness slider convention where
	 * lower gamma values mean a brighter picture). */
	"    color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(gamma));\n"
	"    color.rgb = clamp(color.rgb, 0.0, 1.0);\n"
	"\n"
	"    if (softemu > 0) {\n"
	"        vec3 c = color.rgb;\n"
	"        if (softemu == 1) {\n"
	"            float d = bayer16(ivec2(gl_FragCoord.xy * scale));\n"
	"            c += d * dither / 16.0;\n"
	"        }\n"
	"        ivec3 idx = ivec3(clamp(c, 0.0, 1.0) * 31.0 + 0.5);\n"
	"        int palIdx = int(texelFetch(paletteLUT, idx, 0).r * 255.0);\n"
	"        color.rgb = palette[palIdx];\n"
	"    }\n"
	"\n"
	"    fragColor = color;\n"
	"}\n";

/* These helpers are already in gl_vidsdl.c but static.  We re-declare
 * local copies here to stay self-contained. */
static GLuint PP_CompileShader (GLenum type, const char *source)
{
	GLuint shader;
	GLint status;
	char log[512];

	shader = glCreateShader_fp(type);
	glShaderSource_fp(shader, 1, &source, NULL);
	glCompileShader_fp(shader);
	glGetShaderiv_fp(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		glGetShaderInfoLog_fp(shader, sizeof(log), NULL, log);
		Con_Printf("PostProcess shader compile error: %s\n", log);
		glDeleteShader_fp(shader);
		return 0;
	}
	return shader;
}

static GLuint PP_LinkProgram (GLuint vert, GLuint frag)
{
	GLuint prog;
	GLint status;
	char log[512];

	prog = glCreateProgram_fp();
	glAttachShader_fp(prog, vert);
	glAttachShader_fp(prog, frag);
	glLinkProgram_fp(prog);
	glGetProgramiv_fp(prog, GL_LINK_STATUS, &status);
	if (!status)
	{
		glGetProgramInfoLog_fp(prog, sizeof(log), NULL, log);
		Con_Printf("PostProcess shader link error: %s\n", log);
		glDeleteProgram_fp(prog);
		return 0;
	}
	return prog;
}

static qboolean PP_InitShader (void)
{
	GLuint vs, fs;
	GLint loc;

	vs = PP_CompileShader(GL_VERTEX_SHADER, pp_vert_src);
	if (!vs) return false;
	fs = PP_CompileShader(GL_FRAGMENT_SHADER, pp_frag_src);
	if (!fs) { glDeleteShader_fp(vs); return false; }

	pp_program = PP_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);
	if (!pp_program) return false;

	pp_loc_scene = glGetUniformLocation_fp(pp_program, "scene");
	pp_loc_gamma = glGetUniformLocation_fp(pp_program, "gamma");
	pp_loc_contrast = glGetUniformLocation_fp(pp_program, "contrast");
	pp_loc_mvp = glGetUniformLocation_fp(pp_program, "u_mvp");
	pp_loc_softemu = glGetUniformLocation_fp(pp_program, "softemu");
	pp_loc_dither = glGetUniformLocation_fp(pp_program, "dither");
	pp_loc_paletteLUT = glGetUniformLocation_fp(pp_program, "paletteLUT");
	pp_loc_palette = glGetUniformLocation_fp(pp_program, "palette");
	pp_loc_scale = glGetUniformLocation_fp(pp_program, "scale");

	/* bind samplers once */
	glUseProgram_fp(pp_program);
	loc = pp_loc_scene;
	if (loc >= 0) glUniform1i_fp(loc, 0);	/* texture unit 0 */
	loc = pp_loc_paletteLUT;
	if (loc >= 0) glUniform1i_fp(loc, 1);	/* texture unit 1 */
	glUseProgram_fp(0);

	return true;
}

/* ------------------------------------------------------------------ */
/* Palette LUT                                                         */
/* ------------------------------------------------------------------ */

extern unsigned int d_8to24table[256];

static void PP_BuildPaletteLUT (void)
{
	unsigned char lut[32 * 32 * 32];
	int r, g, b, i;

	if (!glTexImage3D_fp)
		return;

	/* for each point in the 32^3 grid, find the nearest palette color */
	for (b = 0; b < 32; b++)
	{
		for (g = 0; g < 32; g++)
		{
			for (r = 0; r < 32; r++)
			{
				int tr = (r * 255) / 31;
				int tg = (g * 255) / 31;
				int tb = (b * 255) / 31;
				int best = 0;
				int bestdist = 0x7fffffff;

				/* skip index 255 (transparent / fullbright) */
				for (i = 0; i < 255; i++)
				{
					unsigned int pal = d_8to24table[i];
					int pr = (pal >>  0) & 0xff;
					int pg = (pal >>  8) & 0xff;
					int pb = (pal >> 16) & 0xff;
					int dr = tr - pr;
					int dg = tg - pg;
					int db = tb - pb;
					int dist = dr*dr + dg*dg + db*db;
					if (dist < bestdist)
					{
						bestdist = dist;
						best = i;
					}
				}
				lut[b * 32 * 32 + g * 32 + r] = (unsigned char)best;
			}
		}
	}

	glGenTextures_fp(1, &pp_palette_lut);
	glActiveTextureARB_fp(GL_TEXTURE0_ARB + 1);
	glBindTexture_fp(GL_TEXTURE_3D, pp_palette_lut);
	glTexImage3D_fp(GL_TEXTURE_3D, 0, GL_R8, 32, 32, 32, 0,
			GL_RED, GL_UNSIGNED_BYTE, lut);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glBindTexture_fp(GL_TEXTURE_3D, 0);
	glActiveTextureARB_fp(GL_TEXTURE0_ARB);

	pp_lut_built = true;
	Con_SafePrintf("PostProcess: palette LUT built\n");
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void GL_PostProcess_Init (void)
{
	Cvar_RegisterVariable(&r_scale);
	Cvar_RegisterVariable(&r_softemu);
	Cvar_RegisterVariable(&r_dither);

	pp_initialized = false;
	pp_active = false;
	pp_fbo = 0;
	pp_color_tex = 0;
	pp_depth_rb = 0;
	pp_program = 0;
	pp_width = pp_height = 0;

	/* GL 4.3: shaders always available */

	/* check for FBO function pointers */
	glGenFramebuffers_fp = (glGenFramebuffers_f) SDL_GL_GetProcAddress("glGenFramebuffers");
	glDeleteFramebuffers_fp = (glDeleteFramebuffers_f) SDL_GL_GetProcAddress("glDeleteFramebuffers");
	glBindFramebuffer_fp = (glBindFramebuffer_f) SDL_GL_GetProcAddress("glBindFramebuffer");
	glFramebufferTexture2D_fp = (glFramebufferTexture2D_f) SDL_GL_GetProcAddress("glFramebufferTexture2D");
	glFramebufferRenderbuffer_fp = (glFramebufferRenderbuffer_f) SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
	glCheckFramebufferStatus_fp = (glCheckFramebufferStatus_f) SDL_GL_GetProcAddress("glCheckFramebufferStatus");
	glGenRenderbuffers_fp = (glGenRenderbuffers_f) SDL_GL_GetProcAddress("glGenRenderbuffers");
	glDeleteRenderbuffers_fp = (glDeleteRenderbuffers_f) SDL_GL_GetProcAddress("glDeleteRenderbuffers");
	glBindRenderbuffer_fp = (glBindRenderbuffer_f) SDL_GL_GetProcAddress("glBindRenderbuffer");
	glRenderbufferStorage_fp = (glRenderbufferStorage_f) SDL_GL_GetProcAddress("glRenderbufferStorage");
	glRenderbufferStorageMultisample_fp = (glRenderbufferStorageMultisample_f) SDL_GL_GetProcAddress("glRenderbufferStorageMultisample");
	glBlitFramebuffer_fp = (glBlitFramebuffer_f) SDL_GL_GetProcAddress("glBlitFramebuffer");

	if (!glGenFramebuffers_fp || !glDeleteFramebuffers_fp ||
	    !glBindFramebuffer_fp || !glFramebufferTexture2D_fp ||
	    !glFramebufferRenderbuffer_fp || !glCheckFramebufferStatus_fp ||
	    !glGenRenderbuffers_fp || !glDeleteRenderbuffers_fp ||
	    !glBindRenderbuffer_fp || !glRenderbufferStorage_fp)
	{
		Con_SafePrintf("PostProcess: FBO functions not available\n");
		return;
	}

	/* 3D texture and uniform functions for softemu */
	glTexImage3D_fp = (glTexImage3D_f) SDL_GL_GetProcAddress("glTexImage3D");
	glUniform3fv_fp = (glUniform3fv_f) SDL_GL_GetProcAddress("glUniform3fv");

	if (!PP_InitShader())
	{
		Con_SafePrintf("PostProcess: shader init failed\n");
		return;
	}

	/* build the palette LUT for software rendering emulation */
	PP_BuildPaletteLUT();

	pp_initialized = true;
	Con_SafePrintf("PostProcess: gamma/contrast shader ready\n");
}

void GL_PostProcess_Shutdown (void)
{
	PP_DeleteFBO();
	PP_DeleteNativeFBO();
	if (pp_program)
	{
		glDeleteProgram_fp(pp_program);
		pp_program = 0;
	}
	if (pp_palette_lut)
	{
		glDeleteTextures_fp(1, &pp_palette_lut);
		pp_palette_lut = 0;
	}
	pp_lut_built = false;
	pp_initialized = false;
	pp_active = false;
}

void GL_PostProcess_BeginFrame (void)
{
	int w, h;
	float scale;

	pp_active = false;

	if (!PP_NeedsPostProcess())
		return;

	/* clamp render scale */
	scale = r_scale.value;
	if (scale < 0.25f) scale = 0.25f;
	if (scale > 1.0f) scale = 1.0f;

	/* get current viewport size, apply scale */
	w = (int)(glwidth * scale);
	h = (int)(glheight * scale);
	if (w <= 0 || h <= 0)
		return;

	/* (re)create FBO if size changed — MSAA changes go through
	 * vid_restart which destroys and recreates everything */
	if (w != pp_width || h != pp_height)
	{
		if (!PP_CreateFBO(w, h))
			return;
	}

	/* save original viewport and override with scaled resolution */
	pp_saved_glwidth = glwidth;
	pp_saved_glheight = glheight;
	glwidth = w;
	glheight = h;

	/* bind scene FBO */
	glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_fbo);
	pp_active = true;
}

void GL_PostProcess_End3D (void)
{
	int native_w, native_h;
	GLuint read_fbo;

	if (!pp_active)
		return;
	if (r_scale.value >= 1.0f)
		return;	/* no scaling, nothing to do */

	/* determine which FBO to read from */
	if (pp_samples > 1)
	{
		/* resolve multisampled FBO first */
		glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, pp_fbo);
		glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, pp_resolve_fbo);
		glBlitFramebuffer_fp(0, 0, pp_width, pp_height,
				      0, 0, pp_width, pp_height,
				      GL_COLOR_BUFFER_BIT, GL_NEAREST);
		read_fbo = pp_resolve_fbo;
	}
	else
	{
		read_fbo = pp_fbo;
	}

	/* restore native resolution */
	glwidth = pp_saved_glwidth;
	glheight = pp_saved_glheight;
	native_w = glwidth;
	native_h = glheight;

	/* ensure native FBO exists */
	if (!PP_CreateNativeFBO(native_w, native_h))
	{
		pp_active = false;
		glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);
		return;
	}

	/* blit scaled 3D scene to native FBO with nearest filtering (crunchy upscale) */
	glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, read_fbo);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, pp_native_fbo);
	glBlitFramebuffer_fp(0, 0, pp_width, pp_height,
			      0, 0, native_w, native_h,
			      GL_COLOR_BUFFER_BIT, GL_NEAREST);

	/* switch to native FBO for 2D draws */
	glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_native_fbo);
	glViewport_fp(0, 0, native_w, native_h);
	glEnable_fp(GL_DEPTH_TEST);
}

void GL_PostProcess_EndFrame (void)
{
	GLuint blit_tex;
	int blit_w, blit_h;

	if (!pp_active)
		return;

	pp_active = false;

	/* determine which texture has the composited scene */
	if (pp_native_fbo && r_scale.value < 1.0f)
	{
		/* End3D blitted 3D to native FBO, then 2D drew into it */
		blit_tex = pp_native_color_tex;
		blit_w = pp_native_w;
		blit_h = pp_native_h;
	}
	else if (pp_samples > 1)
	{
		/* MSAA: resolve to pp_color_tex */
		glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, pp_fbo);
		glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, pp_resolve_fbo);
		glBlitFramebuffer_fp(0, 0, pp_width, pp_height,
				      0, 0, pp_width, pp_height,
				      GL_COLOR_BUFFER_BIT, GL_NEAREST);
		blit_tex = pp_color_tex;
		blit_w = pp_width;
		blit_h = pp_height;
	}
	else
	{
		blit_tex = pp_color_tex;
		blit_w = pp_width;
		blit_h = pp_height;
	}

	/* restore original viewport dimensions */
	glwidth = pp_saved_glwidth;
	glheight = pp_saved_glheight;

	/* unbind scene FBO -- render to default framebuffer */
	glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);

	/* set full viewport for the blit */
	glViewport_fp(0, 0, glwidth, glheight);

	/* disable depth test, blending, etc. for the blit */
	glDisable_fp(GL_DEPTH_TEST);
	glDisable_fp(GL_BLEND);
	glDisable_fp(GL_CULL_FACE);

	/* set up orthographic projection for full-screen quad */
	GL_MatrixMode(GL_MAT_PROJECTION);
	GL_PushMatrix();
	GL_LoadIdentity();
	GL_Ortho(0, 1, 0, 1, -1, 1);
	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_PushMatrix();
	GL_LoadIdentity();

	/* bind composited scene texture */
	glBindTexture_fp(GL_TEXTURE_2D, blit_tex);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	/* bind palette LUT on texture unit 1 if softemu is active */
	if ((int)r_softemu.value > 0 && pp_lut_built)
	{
		glActiveTextureARB_fp(GL_TEXTURE0_ARB + 1);
		glBindTexture_fp(GL_TEXTURE_3D, pp_palette_lut);
		glActiveTextureARB_fp(GL_TEXTURE0_ARB);
	}

	/* activate shader and set all uniforms before drawing */
	glUseProgram_fp(pp_program);
	if (pp_loc_mvp >= 0)
	{
		float mvp[16];
		GL_GetMVP(mvp);
		glUniformMatrix4fv_fp(pp_loc_mvp, 1, GL_FALSE, mvp);
	}
	if (pp_loc_gamma >= 0)
		glUniform1f_fp(pp_loc_gamma, v_gamma.value);
	if (pp_loc_contrast >= 0)
		glUniform1f_fp(pp_loc_contrast, v_contrast.value);

	/* softemu uniforms */
	if (pp_loc_softemu >= 0)
		glUniform1i_fp(pp_loc_softemu, (int)r_softemu.value);
	if (pp_loc_dither >= 0)
		glUniform1f_fp(pp_loc_dither, r_dither.value);
	if (pp_loc_scale >= 0)
	{
		float scale = r_scale.value;
		if (scale < 0.25f) scale = 0.25f;
		if (scale > 1.0f) scale = 1.0f;
		glUniform1f_fp(pp_loc_scale, scale);
	}
	if ((int)r_softemu.value > 0 && pp_loc_palette >= 0 && glUniform3fv_fp)
	{
		float pal[256 * 3];
		int i;
		for (i = 0; i < 256; i++)
		{
			unsigned int c = d_8to24table[i];
			pal[i * 3 + 0] = ((c >>  0) & 0xff) / 255.0f;
			pal[i * 3 + 1] = ((c >>  8) & 0xff) / 255.0f;
			pal[i * 3 + 2] = ((c >> 16) & 0xff) / 255.0f;
		}
		glUniform3fv_fp(pp_loc_palette, 256, pal);
	}

	/* draw full-screen quad using streaming VBO (shader already active) */
	GL_ImmBegin();
	GL_ImmColor4f(1, 1, 1, 1);
	GL_ImmTexCoord2f(0, 0); GL_ImmVertex2f(0, 0);
	GL_ImmTexCoord2f(1, 0); GL_ImmVertex2f(1, 0);
	GL_ImmTexCoord2f(1, 1); GL_ImmVertex2f(1, 1);
	GL_ImmTexCoord2f(0, 1); GL_ImmVertex2f(0, 1);
	GL_ImmDraw(GL_QUADS);	/* draw without changing shader */

	glUseProgram_fp(0);

	/* unbind palette LUT from unit 1 to avoid interfering with lightmap binds */
	if ((int)r_softemu.value > 0 && pp_lut_built)
	{
		glActiveTextureARB_fp(GL_TEXTURE0_ARB + 1);
		glBindTexture_fp(GL_TEXTURE_3D, 0);
		glActiveTextureARB_fp(GL_TEXTURE0_ARB);
	}

	/* restore matrices */
	GL_MatrixMode(GL_MAT_PROJECTION);
	GL_PopMatrix();
	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_PopMatrix();

	/* re-enable depth test (it's normally on) */
	glEnable_fp(GL_DEPTH_TEST);
}

qboolean GL_PostProcess_Active (void)
{
	return pp_active;
}
