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
#include "draw.h"

/* GL defines that may be missing on some platforms (MinGW, ES3) */
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 0x8CE1
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_TEXTURE_2D_MULTISAMPLE
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#endif

/* OIT needs per-draw-buffer blend equations (glBlendFunci, GL 4.0), which
 * WebGL2 does not expose.  There glBlendFunci_fp is a no-op function-like
 * macro, so it cannot be tested as a value — go through this helper instead. */
#ifdef USE_GLES
#define HW_OIT_HAS_BLEND_FUNCI	0
#else
#define HW_OIT_HAS_BLEND_FUNCI	(glBlendFunci_fp != NULL)
#endif

/* GLSL version headers, mirroring gl_shader.c.  Every shader in this file used
 * to be hardcoded to "#version 430 core", so the entire post-process chain --
 * gamma/contrast, FXAA, bloom, motion blur, softemu palettization and the OIT
 * resolve -- failed to compile on the ES tier and silently disabled itself.
 * That went unnoticed because only the browser could reach the tier and nobody
 * was reading its shader log; a desktop -DUSE_GLES=ON build (uhexen2-0py6)
 * makes it a visible crash instead.  uhexen2-7wdv.
 *
 * GLSL ES 3.00 declares no default float precision in the fragment language,
 * so it must be stated.  int is declared mediump there, which is as little as
 * 16 bits -- not enough for bayer16's 32-bit shifts below -- hence highp int.
 * sampler3D likewise has no default (only sampler2D and samplerCube do), and
 * softemu's palette LUT is a sampler3D whose .r is scaled by 255 to index the
 * palette, so it wants the full range too. */
#ifdef USE_GLES
#define PP_ES_PRECISION	"precision highp float;\nprecision highp int;\nprecision highp sampler3D;\n"
#define PP_VERT_HEADER	"#version 300 es\n" PP_ES_PRECISION
#define PP_FRAG_HEADER	"#version 300 es\n" PP_ES_PRECISION
/* bitfieldReverse is GLSL 4.00 / ES 3.10.  Only bayer16 uses it, and only on
 * a value whose meaningful bits are the low 16, but a full 32-bit reversal is
 * what the desktop builtin does and what the following ">> 24" expects. */
#define PP_BITFIELD_REVERSE \
	"uint bitfieldReverse(uint v) {\n" \
	"    v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);\n" \
	"    v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);\n" \
	"    v = ((v & 0x0F0F0F0Fu) << 4) | ((v >> 4) & 0x0F0F0F0Fu);\n" \
	"    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);\n" \
	"    return (v << 16) | (v >> 16);\n" \
	"}\n"
#else
#define PP_VERT_HEADER	"#version 430 core\n"
#define PP_FRAG_HEADER	"#version 430 core\n"
#define PP_BITFIELD_REVERSE	""
#endif

/* FBO state — scaled 3D scene */
static GLuint	pp_fbo;		/* render target (may be multisampled) */
static GLuint	pp_color_rb;	/* multisampled color renderbuffer (0 if no MSAA) */
static GLuint	pp_depth_rb;	/* multisampled depth/stencil renderbuffer (0 if no MSAA) */
static GLuint	pp_depth_tex;	/* non-MSAA depth/stencil texture, sampler-readable for Hi-Z (uhexen2-xd87) */
static GLuint	pp_resolve_fbo;	/* resolve target (non-multisampled, for shader blit) */
static GLuint	pp_color_tex;	/* resolved color texture */
static int	pp_width, pp_height;
static int	pp_samples;	/* MSAA sample count (0 or 1 = no MSAA) */
static qboolean	pp_fbo_failed;	/* true if FBO creation failed — don't retry every frame */
static GLuint	pp_copyback_tex;/* fallback texture for copyback mode (no FBO) */
static int	pp_copyback_w, pp_copyback_h;

/* Soft-particle depth snapshot (uhexen2-mf9u).  Sprites need to read the
 * depth of the opaque scene behind them, but at sprite-draw time that depth
 * is the bound framebuffer's own attachment — sampling it is a rendering
 * feedback loop, undefined under the GL 4.3 spec.  Blit a private copy once,
 * after the opaque pass and before any translucency, and sample that.
 * Snapshotting there also makes the fade opaque-only by construction, which
 * is what it should be: a sprite ought to dissolve into the wall behind it,
 * not into whichever translucent entity happened to sort earlier. */
static GLuint	soft_depth_fbo;
static GLuint	soft_depth_tex;
static int	soft_depth_w, soft_depth_h;
static GLenum	soft_depth_fmt;		/* internal format the texture was built with */
static qboolean	soft_depth_valid;	/* captured successfully this frame */

/* FBO state — native res for 2D composite */
static GLuint	pp_native_fbo;
static GLuint	pp_native_color_tex;
static GLuint	pp_native_depth_rb;
static int	pp_native_w, pp_native_h;
static GLenum	pp_native_fmt;		/* colour format the native FBO was built with */

/* Bloom post-process FBO pyramid (1/2, 1/4, 1/8, 1/16 res) */
#define BLOOM_LEVELS 4
static GLuint	bloom_fbo[BLOOM_LEVELS];
static GLuint	bloom_tex[BLOOM_LEVELS];
static int	bloom_w[BLOOM_LEVELS], bloom_h[BLOOM_LEVELS];

/* Bloom shader programs */
static GLuint	bloom_bright_prog;	/* threshold extraction */
static GLuint	bloom_down_prog;	/* downsample 4-tap box */
static GLuint	bloom_up_prog;		/* upsample 3x3 tent + additive blend */

/* Bloom shader uniform locations */
static GLint	bloom_bright_loc_scene, bloom_bright_loc_threshold, bloom_bright_loc_rcpframe;
static GLint	bloom_down_loc_scene, bloom_down_loc_rcpframe;
static GLint	bloom_up_loc_scene, bloom_up_loc_prev, bloom_up_loc_rcpframe;
static GLuint	bloom_dummy_vao;		/* dummy VAO required for glDrawArrays in GL 4.3 core */

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
static GLint	pp_loc_waterwarp;
static GLint	pp_loc_time;
static GLint	pp_loc_fxaa;
static GLint	pp_loc_rcpframe;
static GLint	pp_loc_motionblur;
static GLint	pp_loc_viewdelta;
static GLint	pp_loc_hdr_exposure;
static GLint	pp_loc_bloom_tex;
static GLint	pp_loc_bloom_strength;

/* Palette LUT state */
static GLuint	pp_palette_lut;		/* 32x32x32 3D texture, nearest-colour (r_softemu 1/2) */
static GLuint	pp_colormap_lut;	/* 32x32x32 3D texture, via gfx/colormap.lmp (r_softemu 3) */
static qboolean	pp_lut_built;

static qboolean	pp_initialized;
static qboolean	pp_active;		/* true when scene is being rendered to FBO this frame */
static qboolean	pp_native_active;	/* true when End3D transferred 3D → composite FBO; EndFrame skips warp/blur */
static float	pp_prev_yaw, pp_prev_pitch;	/* view angle tracking for motionblur delta */
static vec3_t	pp_prev_origin;			/* position tracking for motionblur delta */
static int	pp_saved_glwidth, pp_saved_glheight;	/* original viewport dims */
static float	pp_waterwarp_preview_end;	/* cl.time when waterwarp preview should end */

cvar_t	r_scale = {"r_scale", "1", CVAR_ARCHIVE};
cvar_t	r_softemu = {"r_softemu", "0", CVAR_ARCHIVE};
cvar_t	r_dither = {"r_dither", "0.5", CVAR_ARCHIVE};	/* dither strength (0-2), reduced to avoid AMD noise artifacts */
cvar_t	r_hdr = {"r_hdr", "0", CVAR_ARCHIVE};		/* 0=off, 1=ACES tonemap */
cvar_t	r_hdr_exposure = {"r_hdr_exposure", "1.0", CVAR_ARCHIVE};
cvar_t	r_oit = {"r_oit", "0", CVAR_ARCHIVE};
cvar_t	r_bloom = {"r_bloom", "0", CVAR_ARCHIVE};		/* bloom post-process */
cvar_t	r_bloom_intensity = {"r_bloom_intensity", "1.0", CVAR_ARCHIVE};	/* bloom glow strength */
cvar_t	r_bloom_threshold = {"r_bloom_threshold", "1.0", CVAR_ARCHIVE};	/* luminance threshold */
/* Soft particles (uhexen2-mf9u).  Default off until it has been looked at on
 * real hardware; costs one full-res depth blit per frame plus a depth fetch
 * per sprite fragment when on.
 *
 * The scale is the fade distance in world units, and 8 is a measured choice,
 * not a guess.  sm_expld.spr — the sprite the original report came in about —
 * is 48 units across and detonates centred on the impact surface, so the fade
 * distance trades artifact removal against washing the sprite out.  Compared
 * headless on demo1 (Necromancer, raven staff, explosions on the plinth): 24
 * dissolves most of the flame and visibly drains the explosion; 8 removes the
 * hard rectangular cut while leaving the flame at close to full brightness. */
cvar_t	r_softparticles = {"r_softparticles", "0", CVAR_ARCHIVE};
cvar_t	r_softparticles_scale = {"r_softparticles_scale", "8", CVAR_ARCHIVE};

/* ------------------------------------------------------------------ */
/* Order-Independent Transparency (McGuire & Bavoil WBOIT)            */
/* ------------------------------------------------------------------ */

extern cvar_t	vid_config_fsaa;	/* MSAA sample count (drives the GL context) */

static GLuint	oit_accum_tex;		/* RGBA16F accumulation */
static GLuint	oit_revealage_tex;	/* RGBA16F revealage (.r used) */
static GLuint	oit_fbo;		/* MRT FBO sharing scene depth/stencil */
static GLuint	oit_resolve_prog;	/* fullscreen resolve shader (sampler2D) */
static GLuint	oit_resolve_prog_msaa;	/* per-sample resolve (sampler2DMS) */
static GLuint	oit_resolve_vao;	/* dummy VAO required for glDrawArrays
					 * in GL 4.3 core profile — without one
					 * bound, the resolve draw was a silent
					 * GL_INVALID_OPERATION and the entire OIT
					 * composite was being discarded. */
static GLint	oit_resolve_loc_accum;
static GLint	oit_resolve_loc_reveal;
static GLint	oit_resolve_msaa_loc_accum;
static GLint	oit_resolve_msaa_loc_reveal;
static int	oit_samples;		/* >1 when accum/revealage are multisampled */
static int	oit_width, oit_height;
static qboolean	oit_available;		/* true if FBO + shader created OK */
static qboolean	oit_in_pass;		/* true between Begin/EndTranslucency */

/* OIT resolve shaders */
static const char oit_resolve_vert[] =
	PP_VERT_HEADER
	"void main() {\n"
	"    ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
	"    gl_Position = vec4(vec2(v) * 4.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/* No stencil gate, no early_fragment_tests. WBOIT handles empty pixels
 * via math: accum=0, revealage=1 → alpha=0 → transparent → scene unchanged. */
static const char oit_resolve_frag[] =
	PP_FRAG_HEADER
	"uniform sampler2D TexAccum;\n"
	"uniform sampler2D TexReveal;\n"
	"layout(location=0) out vec4 out_fragcolor;\n"
	"float max3(vec3 v) { return max(max(v.x, v.y), v.z); }\n"
	"void main() {\n"
	"    ivec2 coords = ivec2(gl_FragCoord.xy);\n"
	"    float revealage = texelFetch(TexReveal, coords, 0).r;\n"
	"    vec4 accumulation = texelFetch(TexAccum, coords, 0);\n"
	"    if (isinf(max3(abs(accumulation.rgb))))\n"
	"        accumulation.rgb = vec3(accumulation.a);\n"
	"    vec3 average_color = accumulation.rgb / max(accumulation.a, 1e-5);\n"
	"    out_fragcolor = vec4(average_color, 1.0 - revealage);\n"
	"}\n";

/* MSAA variant: per-sample resolve via sampler2DMS + gl_SampleID, used when
 * the OIT accum/revealage targets are GL_TEXTURE_2D_MULTISAMPLE.
 *
 * Absent on the ES tier: sampler2DMS and gl_SampleID are GLSL ES 3.10, and
 * glTexImage2DMultisample_fp is a no-op there, so oit_samples never exceeds 1
 * and this program could never be selected.  It used to be compiled anyway on
 * the theory that a failed build was harmless; it is not -- a driver handed a
 * shader it just rejected is a driver in a state nothing tests. */
#ifndef USE_GLES
static const char oit_resolve_frag_msaa[] =
	PP_FRAG_HEADER
	"uniform sampler2DMS TexAccum;\n"
	"uniform sampler2DMS TexReveal;\n"
	"layout(location=0) out vec4 out_fragcolor;\n"
	"float max3(vec3 v) { return max(max(v.x, v.y), v.z); }\n"
	"void main() {\n"
	"    ivec2 coords = ivec2(gl_FragCoord.xy);\n"
	"    float revealage = texelFetch(TexReveal, coords, gl_SampleID).r;\n"
	"    vec4 accumulation = texelFetch(TexAccum, coords, gl_SampleID);\n"
	"    if (isinf(max3(abs(accumulation.rgb))))\n"
	"        accumulation.rgb = vec3(accumulation.a);\n"
	"    vec3 average_color = accumulation.rgb / max(accumulation.a, 1e-5);\n"
	"    out_fragcolor = vec4(average_color, 1.0 - revealage);\n"
	"}\n";
#endif /* !USE_GLES */

/* ------------------------------------------------------------------ */
/* Bloom post-process shaders                                          */
/* ------------------------------------------------------------------ */

static const char bloom_vert_src[] =
	PP_VERT_HEADER
	"out vec2 v_uv;\n"
	"void main() {\n"
	"    ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
	"    vec2 pos = vec2(v) * 4.0 - 1.0;\n"
	"    v_uv = pos * 0.5 + 0.5;\n"
	"    gl_Position = vec4(pos, 0.0, 1.0);\n"
	"}\n";

static const char bloom_bright_frag_src[] =
	PP_FRAG_HEADER
	"uniform sampler2D u_scene;\n"
	"uniform float u_threshold;\n"
	"uniform vec2 u_rcpframe;\n"
	"in vec2 v_uv;\n"
	"layout(location=0) out vec4 fragColor;\n"
	"void main() {\n"
	"    vec3 c = texture(u_scene, v_uv).rgb;\n"
	"    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
	"    float w = max(0.0, lum - u_threshold);\n"
	"    fragColor = vec4(c * (w / max(lum, 0.001)), 1.0);\n"
	"}\n";

static const char bloom_down_frag_src[] =
	PP_FRAG_HEADER
	"uniform sampler2D u_scene;\n"
	"uniform vec2 u_rcpframe;\n"
	"in vec2 v_uv;\n"
	"layout(location=0) out vec4 fragColor;\n"
	"void main() {\n"
	"    vec2 h = u_rcpframe * 0.5;\n"
	"    vec4 s = texture(u_scene, v_uv + vec2(-h.x,-h.y))\n"
	"           + texture(u_scene, v_uv + vec2( h.x,-h.y))\n"
	"           + texture(u_scene, v_uv + vec2(-h.x, h.y))\n"
	"           + texture(u_scene, v_uv + vec2( h.x, h.y));\n"
	"    fragColor = s * 0.25;\n"
	"}\n";

static const char bloom_up_frag_src[] =
	PP_FRAG_HEADER
	"uniform sampler2D u_scene;\n"
	"uniform vec2 u_rcpframe;\n"
	"in vec2 v_uv;\n"
	"layout(location=0) out vec4 fragColor;\n"
	"void main() {\n"
	"    vec2 h = u_rcpframe;\n"
	"    vec4 s = texture(u_scene, v_uv + vec2(-h.x, 0)) * 2.0\n"
	"           + texture(u_scene, v_uv + vec2( h.x, 0)) * 2.0\n"
	"           + texture(u_scene, v_uv + vec2(0, -h.y)) * 2.0\n"
	"           + texture(u_scene, v_uv + vec2(0,  h.y)) * 2.0\n"
	"           + texture(u_scene, v_uv + vec2(-h.x,-h.y))\n"
	"           + texture(u_scene, v_uv + vec2( h.x,-h.y))\n"
	"           + texture(u_scene, v_uv + vec2(-h.x, h.y))\n"
	"           + texture(u_scene, v_uv + vec2( h.x, h.y));\n"
	"    s /= 12.0;\n"
	"    fragColor = s;\n"
	"}\n";

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
	if (gl_fxaa.value > 0)
		return true;
	if (r_waterwarp.value > 0)
		return true;
	if (Cvar_VariableValue("r_motionblur") > 0)
		return true;
	if (r_hdr.integer)
		return true;
	/* OIT's accum/revealage FBO is built as a sibling of the scene FBO
	 * in PP_CreateFBO and shares its depth/stencil attachment, so the
	 * postprocess pipeline must be up for any WBOIT to happen.  Without
	 * this, setting r_oit 1 at runtime left OIT_Active() false because
	 * oit_fbo was never created — and the user had to enable an
	 * unrelated effect (FXAA, HDR, …) to "wake" the pipeline. */
	if (r_oit.integer)
		return true;
	if (r_bloom.integer)
		return true;
	return false;
}

/* ------------------------------------------------------------------ */
/* FBO management                                                      */
/* ------------------------------------------------------------------ */

static void PP_DeleteBloomFBOs (void);	/* forward decl to avoid implicit declaration */

/* r_hdr changes the FBO colour format and r_bloom decides whether the
 * pyramid exists at all, but the per-frame gate only rebuilds on a size
 * change -- so a runtime toggle used to do nothing until vid_restart.
 * The callbacks can fire mid-frame from the console or the menu, so they
 * only raise a flag; PP_BeginFrame does the reallocation at the same safe
 * point the size-change rebuild already uses.  uhexen2-3p6p. */
static qboolean	pp_rebuild_pending;

static void PP_FormatChanged (cvar_t *var)
{
	(void) var;
	pp_rebuild_pending = true;
}

static void PP_DeleteFBO (void)
{
	if (pp_color_tex)   { glDeleteTextures_fp(1, &pp_color_tex); pp_color_tex = 0; }
	if (pp_color_rb)    { glDeleteRenderbuffers_fp(1, &pp_color_rb); pp_color_rb = 0; }
	if (pp_depth_rb)    { glDeleteRenderbuffers_fp(1, &pp_depth_rb); pp_depth_rb = 0; }
	if (pp_depth_tex)   { glDeleteTextures_fp(1, &pp_depth_tex); pp_depth_tex = 0; }
	if (pp_resolve_fbo) { glDeleteFramebuffers_fp(1, &pp_resolve_fbo); pp_resolve_fbo = 0; }
	if (pp_fbo)         { glDeleteFramebuffers_fp(1, &pp_fbo); pp_fbo = 0; }
	PP_DeleteBloomFBOs();
	pp_width = pp_height = pp_samples = 0;
}

static void PP_DeleteNativeFBO (void)
{
	if (pp_native_color_tex) { glDeleteTextures_fp(1, &pp_native_color_tex); pp_native_color_tex = 0; }
	if (pp_native_depth_rb)  { glDeleteRenderbuffers_fp(1, &pp_native_depth_rb); pp_native_depth_rb = 0; }
	if (pp_native_fbo)       { glDeleteFramebuffers_fp(1, &pp_native_fbo); pp_native_fbo = 0; }
	pp_native_w = pp_native_h = 0;
	pp_native_fmt = 0;
}

static void PP_DeleteBloomFBOs (void)
{
	int i;
	for (i = 0; i < BLOOM_LEVELS; i++) {
		if (bloom_tex[i]) { glDeleteTextures_fp(1, &bloom_tex[i]); bloom_tex[i] = 0; }
		if (bloom_fbo[i]) { glDeleteFramebuffers_fp(1, &bloom_fbo[i]); bloom_fbo[i] = 0; }
		bloom_w[i] = bloom_h[i] = 0;
	}
}

static qboolean PP_CreateNativeFBO (int width, int height)
{
	GLenum status;

	/* Size alone is not enough: r_hdr selects the colour format, so a
	 * runtime toggle must reallocate even at an unchanged resolution.
	 * uhexen2-3p6p. */
	if (width == pp_native_w && height == pp_native_h && pp_native_fbo &&
	    pp_native_fmt == (GLenum)(r_hdr.integer ? GL_RGBA16F : GL_RGBA8))
		return true;	/* already correct size and format */

	PP_DeleteNativeFBO();

	{
	GLenum native_fmt = r_hdr.integer ? GL_RGBA16F : GL_RGBA8;
	GLenum native_type = r_hdr.integer ? GL_FLOAT : GL_UNSIGNED_BYTE;
	glGenTextures_fp(1, &pp_native_color_tex);
	glBindTexture_fp(GL_TEXTURE_2D, pp_native_color_tex);
	glTexImage2D_fp(GL_TEXTURE_2D, 0, native_fmt, width, height, 0,
			GL_RGBA, native_type, NULL);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture_fp(GL_TEXTURE_2D, 0);

	glGenRenderbuffers_fp(1, &pp_native_depth_rb);
	glBindRenderbuffer_fp(GL_RENDERBUFFER, pp_native_depth_rb);
	glRenderbufferStorage_fp(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
	glBindRenderbuffer_fp(GL_RENDERBUFFER, 0);

	glGenFramebuffers_fp(1, &pp_native_fbo);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_native_fbo);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_TEXTURE_2D, pp_native_color_tex, 0);
	glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
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
	pp_native_fmt = native_fmt;
	}  /* end native_fmt scope */
	return true;
}

/* Forward declaration — OIT FBO created after scene FBO */
static qboolean OIT_CreateFBO (int width, int height, GLuint depth_stencil_rb, GLuint depth_stencil_tex, int samples);
static void OIT_DeleteFBO (void);

/* Forward declaration — bloom pyramid FBOs */
static qboolean PP_CreateBloomFBOs (int width, int height);

static qboolean PP_CreateBloomFBOs (int width, int height)
{
	GLenum color_fmt, color_type, status;
	int i;

	/* Delete first so a runtime r_bloom 0 actually releases the pyramid
	 * rather than leaving it allocated but unused.  uhexen2-3p6p. */
	PP_DeleteBloomFBOs();

	if (!r_bloom.integer)
		return true;	/* bloom disabled, skip allocation */

	color_fmt = r_hdr.integer ? GL_RGBA16F : GL_RGBA8;
	color_type = r_hdr.integer ? GL_FLOAT : GL_UNSIGNED_BYTE;

	for (i = 0; i < BLOOM_LEVELS; i++)
	{
		bloom_w[i] = (width >> (i + 1)) > 1 ? (width >> (i + 1)) : 1;		/* /2, /4, /8, /16 */
		bloom_h[i] = (height >> (i + 1)) > 1 ? (height >> (i + 1)) : 1;

		/* Bloom texture */
		glGenTextures_fp(1, &bloom_tex[i]);
		glBindTexture_fp(GL_TEXTURE_2D, bloom_tex[i]);
		glTexImage2D_fp(GL_TEXTURE_2D, 0, color_fmt, bloom_w[i], bloom_h[i], 0,
				GL_RGBA, color_type, NULL);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture_fp(GL_TEXTURE_2D, 0);

		/* Bloom FBO */
		glGenFramebuffers_fp(1, &bloom_fbo[i]);
		glBindFramebuffer_fp(GL_FRAMEBUFFER, bloom_fbo[i]);
		glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					  GL_TEXTURE_2D, bloom_tex[i], 0);

		status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
		glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);

		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			Con_DPrintf("Bloom: FBO %d incomplete (status 0x%x, %dx%d)\n",
				    i, status, bloom_w[i], bloom_h[i]);
			PP_DeleteBloomFBOs();
			return false;
		}
	}

	return true;
}

static qboolean PP_CreateFBO (int width, int height)
{
	GLenum status;
	/* OIT now follows Ironwail: when MSAA is active the accum/revealage
	 * targets are created as GL_TEXTURE_2D_MULTISAMPLE and resolved
	 * per-sample, so they share the multisampled depth/stencil cleanly. */
	int samples = (vid_config_fsaa.integer > 1) ? vid_config_fsaa.integer : 0;

	/* Clamp to what the driver can actually allocate.  config.cfg is a plain
	 * text file and survives GPU swaps, so an inherited 16 on an 8-sample
	 * driver is an ordinary case, not a corrupt one -- clamp it rather than
	 * letting the allocation fail into the fallback path on every restart.
	 * gl_max_samples is the engine's one GL_MAX_SAMPLES query (gl_vidsdl.c,
	 * populated post-context-init and already the bound the Options menu picks
	 * against); the cap cannot change within a context, so re-querying it here
	 * would only risk the two clamps drifting apart. */
	if (samples > gl_max_samples)
		samples = gl_max_samples;
	if (samples < 2)
		samples = 0;

	PP_DeleteFBO();

	/* resolve texture (always non-multisampled — this is what the shader reads) */
	{
		GLenum color_fmt = r_hdr.integer ? GL_RGBA16F : GL_RGBA8;
		GLenum color_type = r_hdr.integer ? GL_FLOAT : GL_UNSIGNED_BYTE;
	glGenTextures_fp(1, &pp_color_tex);
	glBindTexture_fp(GL_TEXTURE_2D, pp_color_tex);
	glTexImage2D_fp(GL_TEXTURE_2D, 0, color_fmt, width, height, 0,
			GL_RGBA, color_type, NULL);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture_fp(GL_TEXTURE_2D, 0);

	if (samples > 1 && glRenderbufferStorageMultisample_fp && glBlitFramebuffer_fp)
	{
		/* multisampled render FBO */
		glGenRenderbuffers_fp(1, &pp_color_rb);
		glBindRenderbuffer_fp(GL_RENDERBUFFER, pp_color_rb);
		glRenderbufferStorageMultisample_fp(GL_RENDERBUFFER, samples, color_fmt, width, height);

		glGenRenderbuffers_fp(1, &pp_depth_rb);
		glBindRenderbuffer_fp(GL_RENDERBUFFER, pp_depth_rb);
		glRenderbufferStorageMultisample_fp(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
		glBindRenderbuffer_fp(GL_RENDERBUFFER, 0);

		glGenFramebuffers_fp(1, &pp_fbo);
		glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_fbo);
		glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					     GL_RENDERBUFFER, pp_color_rb);
		glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
					     GL_RENDERBUFFER, pp_depth_rb);

		status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			Con_Printf("PostProcess: MSAA FBO incomplete (status 0x%x), falling back\n", status);
			/* clean up only MSAA resources, keep pp_color_tex for non-MSAA fallback */
			glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);
			if (pp_fbo)       { glDeleteFramebuffers_fp(1, &pp_fbo); pp_fbo = 0; }
			if (pp_color_rb)  { glDeleteRenderbuffers_fp(1, &pp_color_rb); pp_color_rb = 0; }
			if (pp_depth_rb)  { glDeleteRenderbuffers_fp(1, &pp_depth_rb); pp_depth_rb = 0; }
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
				pp_fbo_failed = true;
				return false;
			}

			pp_samples = samples;
			pp_width = width;
			pp_height = height;
			Con_DPrintf("PostProcess: %dx%d FBO with %dx MSAA\n", width, height, samples);
			OIT_CreateFBO(width, height, pp_depth_rb, 0, samples);
			/* Bloom samples the resolved texture, so it applies under MSAA
			 * exactly as it does without.  Omitting this left r_bloom 1 a
			 * silent no-op whenever vid_config_fsaa >= 2.  uhexen2-3p6p. */
			PP_CreateBloomFBOs(width, height);
			return true;
		}
	}

	/* non-MSAA path — depth/stencil as a sampler-readable texture so the
	 * Hi-Z compute pass (uhexen2-xd87) can read it. */
	glGenTextures_fp(1, &pp_depth_tex);
	glBindTexture_fp(GL_TEXTURE_2D, pp_depth_tex);
	glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height, 0,
			GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	/* GL_TEXTURE_COMPARE_MODE defaults to NONE so sampler2D returns the
	 * raw depth value rather than a comparison result. */
	glBindTexture_fp(GL_TEXTURE_2D, 0);

	glGenFramebuffers_fp(1, &pp_fbo);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_fbo);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  GL_TEXTURE_2D, pp_color_tex, 0);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
				  GL_TEXTURE_2D, pp_depth_tex, 0);

	status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_Printf("PostProcess: FBO incomplete (status 0x%x, %dx%d, tex=%u, depth_tex=%u)\n",
			   status, width, height, pp_color_tex, pp_depth_tex);
		PP_DeleteFBO();
		pp_fbo_failed = true;
		return false;
	}

	pp_samples = 0;
	pp_width = width;
	pp_height = height;
	}  /* end color_fmt scope */
	OIT_CreateFBO(width, height, 0, pp_depth_tex, 0);
	PP_CreateBloomFBOs(width, height);
	return true;
}

/* ------------------------------------------------------------------ */
/* Shader                                                              */
/* ------------------------------------------------------------------ */

static const char pp_vert_src[] =
	PP_VERT_HEADER
	"layout(location = 0) in vec3 a_position;\n"
	"layout(location = 1) in vec2 a_texcoord;\n"
	"out vec2 v_texcoord;\n"
	"uniform mat4 u_mvp;\n"
	"void main() {\n"
	"    v_texcoord = a_texcoord;\n"
	"    gl_Position = u_mvp * vec4(a_position, 1.0);\n"
	"}\n";

static const char pp_frag_src[] =
	PP_FRAG_HEADER
	"uniform sampler2D scene;\n"
	"uniform float gamma;\n"
	"uniform float contrast;\n"
	"uniform int softemu;\n"
	"uniform float dither;\n"
	"uniform sampler3D paletteLUT;\n"
	"uniform vec3 palette[256];\n"
	"uniform float scale;\n"
	"uniform float waterwarp;\n"
	"uniform float time;\n"
	"uniform float fxaa_on;\n"
	"uniform vec2 rcpFrame;\n"
	"uniform float motionblur;\n"
	"uniform float hdr_exposure;\n"
	"uniform vec2 viewdelta;\n"
	"uniform sampler2D u_bloom;\n"
	"uniform float u_bloom_strength;\n"
	"in vec2 v_texcoord;\n"
	"out vec4 fragColor;\n"
	"\n"
	"vec4 fxaa(sampler2D tex, vec2 uv, vec2 rcp) {\n"
	"    #define LM(c) ((c).g)\n"
	"    vec3 rN=texture(tex,uv+vec2(0,-1)*rcp).rgb,rS=texture(tex,uv+vec2(0,1)*rcp).rgb;\n"
	"    vec3 rE=texture(tex,uv+vec2(1,0)*rcp).rgb,rW=texture(tex,uv+vec2(-1,0)*rcp).rgb;\n"
	"    vec3 rM=texture(tex,uv).rgb;\n"
	"    float lN=LM(rN),lS=LM(rS),lE=LM(rE),lW=LM(rW),lM=LM(rM);\n"
	"    float mn=min(lM,min(min(lN,lS),min(lW,lE)));\n"
	"    float mx=max(lM,max(max(lN,lS),max(lW,lE)));\n"
	"    float rng=mx-mn;\n"
	"    if(rng<max(0.0625,mx*0.166)) return vec4(rM,1.0);\n"
	"    float lNW=LM(texture(tex,uv+vec2(-1,-1)*rcp).rgb);\n"
	"    float lNE=LM(texture(tex,uv+vec2(1,-1)*rcp).rgb);\n"
	"    float lSW=LM(texture(tex,uv+vec2(-1,1)*rcp).rgb);\n"
	"    float lSE=LM(texture(tex,uv+vec2(1,1)*rcp).rgb);\n"
	"    float eH=abs(lNW+lNE-2.0*lN)+abs(lW+lE-2.0*lM)*2.0+abs(lSW+lSE-2.0*lS);\n"
	"    float eV=abs(lNW+lSW-2.0*lW)+abs(lN+lS-2.0*lM)*2.0+abs(lNE+lSE-2.0*lE);\n"
	"    bool hz=(eH>=eV);\n"
	"    float l1=hz?lS:lE,l2=hz?lN:lW;\n"
	"    float st=hz?rcp.y:rcp.x;\n"
	"    if(abs(l1-lM)<abs(l2-lM))st=-st;\n"
	"    float avg=(lN+lS+lE+lW)*0.25;\n"
	"    float sp=clamp(abs(avg-lM)/rng,0.0,0.75);\n"
	"    vec2 p=uv;if(hz)p.y+=st*0.5;else p.x+=st*0.5;\n"
	"    return vec4(mix(texture(tex,p).rgb,rM,1.0-sp),1.0);\n"
	"    #undef LM\n"
	"}\n"
	"\n"
	PP_BITFIELD_REVERSE
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
	"    vec2 uv = v_texcoord;\n"
	"    if (waterwarp > 0.0) {\n"
	"        uv.x += sin(uv.y * 10.0 + time * 1.5) * 0.015 * waterwarp;\n"
	"        uv.y += sin(uv.x * 10.0 + time * 2.0) * 0.015 * waterwarp;\n"
	"    }\n"
	"    vec4 color = (fxaa_on > 0.0) ? fxaa(scene, uv, rcpFrame) : texture(scene, uv);\n"
	"    if (motionblur > 0.0) {\n"
	"        vec2 vel = viewdelta * motionblur;\n"
	"        color.rgb = color.rgb * 0.4;\n"
	"        color.rgb += texture(scene, uv + vel * 0.25).rgb * 0.2;\n"
	"        color.rgb += texture(scene, uv + vel * 0.50).rgb * 0.15;\n"
	"        color.rgb += texture(scene, uv + vel * 0.75).rgb * 0.125;\n"
	"        color.rgb += texture(scene, uv + vel * 1.00).rgb * 0.125;\n"
	"    }\n"
	"    if (u_bloom_strength > 0.0) {\n"
	"        color.rgb += texture(u_bloom, uv).rgb * u_bloom_strength;\n"
	"    }\n"
	"    /* HDR tonemapping (ACES filmic) */\n"
	"    if (hdr_exposure > 0.0) {\n"
	"        color.rgb *= hdr_exposure;\n"
	"        vec3 x = color.rgb;\n"
	"        color.rgb = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);\n"
	"    }\n"
	"    if (contrast != 1.0)\n"
	"        color.rgb = (color.rgb - 0.5) * contrast + 0.5;\n"
	"    if (gamma != 1.0)\n"
	"        color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(gamma));\n"
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
	pp_loc_waterwarp = glGetUniformLocation_fp(pp_program, "waterwarp");
	pp_loc_time = glGetUniformLocation_fp(pp_program, "time");
	pp_loc_fxaa = glGetUniformLocation_fp(pp_program, "fxaa_on");
	pp_loc_rcpframe = glGetUniformLocation_fp(pp_program, "rcpFrame");
	pp_loc_motionblur = glGetUniformLocation_fp(pp_program, "motionblur");
	pp_loc_viewdelta = glGetUniformLocation_fp(pp_program, "viewdelta");
	pp_loc_hdr_exposure = glGetUniformLocation_fp(pp_program, "hdr_exposure");

	/* bind samplers once */
	glUseProgram_fp(pp_program);
	loc = pp_loc_scene;
	if (loc >= 0) glUniform1i_fp(loc, 0);	/* texture unit 0 */
	loc = pp_loc_paletteLUT;
	if (loc >= 0) glUniform1i_fp(loc, 1);	/* texture unit 1 */
	pp_loc_bloom_tex = glGetUniformLocation_fp(pp_program, "u_bloom");
	pp_loc_bloom_strength = glGetUniformLocation_fp(pp_program, "u_bloom_strength");
	if (pp_loc_bloom_tex >= 0) glUniform1i_fp(pp_loc_bloom_tex, 2);	/* texture unit 2 */
	glUseProgram_fp(0);

	/* Bloom shaders */
	vs = PP_CompileShader(GL_VERTEX_SHADER, bloom_vert_src);
	if (!vs) return true;	/* bloom optional */

	fs = PP_CompileShader(GL_FRAGMENT_SHADER, bloom_bright_frag_src);
	if (!fs) { glDeleteShader_fp(vs); return true; }
	bloom_bright_prog = PP_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);
	if (!bloom_bright_prog) return true;

	bloom_bright_loc_scene = glGetUniformLocation_fp(bloom_bright_prog, "u_scene");
	bloom_bright_loc_threshold = glGetUniformLocation_fp(bloom_bright_prog, "u_threshold");
	bloom_bright_loc_rcpframe = glGetUniformLocation_fp(bloom_bright_prog, "u_rcpframe");

	vs = PP_CompileShader(GL_VERTEX_SHADER, bloom_vert_src);
	if (!vs) return true;
	fs = PP_CompileShader(GL_FRAGMENT_SHADER, bloom_down_frag_src);
	if (!fs) { glDeleteShader_fp(vs); return true; }
	bloom_down_prog = PP_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);
	if (!bloom_down_prog) return true;

	bloom_down_loc_scene = glGetUniformLocation_fp(bloom_down_prog, "u_scene");
	bloom_down_loc_rcpframe = glGetUniformLocation_fp(bloom_down_prog, "u_rcpframe");

	vs = PP_CompileShader(GL_VERTEX_SHADER, bloom_vert_src);
	if (!vs) return true;
	fs = PP_CompileShader(GL_FRAGMENT_SHADER, bloom_up_frag_src);
	if (!fs) { glDeleteShader_fp(vs); return true; }
	bloom_up_prog = PP_LinkProgram(vs, fs);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);
	if (!bloom_up_prog) return true;

	bloom_up_loc_scene = glGetUniformLocation_fp(bloom_up_prog, "u_scene");
	bloom_up_loc_prev = glGetUniformLocation_fp(bloom_up_prog, "u_prev");
	bloom_up_loc_rcpframe = glGetUniformLocation_fp(bloom_up_prog, "u_rcpframe");

	/* GL 4.3 core profile requires a VAO bound for any draw call. */
	glGenVertexArrays_fp(1, &bloom_dummy_vao);

	return true;
}

/* ------------------------------------------------------------------ */
/* Palette LUT                                                         */
/* ------------------------------------------------------------------ */

extern unsigned int d_8to24table[256];

/* ITU-R BT.601 luma, fixed point: (r*77 + g*151 + b*28) >> 8 spans 0..255. */
#define PP_LUMA(r,g,b)	((((r) * 77) + ((g) * 151) + ((b) * 28)) >> 8)

static GLuint PP_UploadLUT (const unsigned char *lut)
{
	GLuint tex = 0;

	glGenTextures_fp(1, &tex);
	glActiveTexture_fp(GL_TEXTURE0 + 1);
	glBindTexture_fp(GL_TEXTURE_3D, tex);
	glTexImage3D_fp(GL_TEXTURE_3D, 0, GL_R8, 32, 32, 32, 0,
			GL_RED, GL_UNSIGNED_BYTE, lut);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	glBindTexture_fp(GL_TEXTURE_3D, 0);
	glActiveTexture_fp(GL_TEXTURE0);

	return tex;
}

/* r_softemu 3 -- quantize the way the 1997 rasterizer did.
 *
 * The software renderer never chose a colour by RGB distance.  Every lit pixel
 * was colormap[light * 256 + texel]: an authored table whose darkening ramps
 * take a deliberately non-linear path through the palette, with hue shifts at
 * the dark end that a Euclidean nearest-colour search cannot reproduce.
 *
 * The post-process only ever sees the product of texel and light, so splitting
 * one back into the other is an inference.  The table's own calibration makes
 * the split: row ~30 is the identity row (colormap[30*256 + i] == i for most
 * i), so a fully lit texel sits mid-ramp and the rows above it are Raven's
 * overbright headroom.  Luminance is therefore the light axis -- for a
 * candidate texel, the light level is the row of that texel's ramp whose
 * luminance lands nearest the incoming pixel's.  Pinning the light level that
 * way leaves exactly one reachable colour per texel, and the texel is then
 * whichever of those 255 candidates is closest in RGB.
 *
 * So brightness picks the light level and chroma picks the texel: the
 * rasterizer's own split, run backwards.  The result is whatever Raven's table
 * says lives at that pair, which is why it is not the nearest palette entry --
 * 27.8% of the 32^3 grid lands elsewhere, and the disagreement concentrates in
 * the dark ranges (56% of dark grid points move, mean delta 40/255, against
 * 25% and 11/255 in the midtones) where the authored ramps bend.
 *
 * Ramp luminance is not quite monotonic in light level (90 of the 256 ramps
 * wobble), so the level search has to be exhaustive rather than a bisection.
 */
static qboolean PP_BuildColormapLUT (void)
{
	/* static: ~114 KB of tables, too much for the stack on Windows */
	static unsigned char lut[32 * 32 * 32];
	static unsigned char rampluma[VID_GRADES][256];	/* [light][texel] -> luma */
	static unsigned char bestlevel[256][256];	/* [texel][target luma] -> light */
	const byte *cmap = host_colormap;
	int r, g, b, i, l, y;

	if (!glTexImage3D_fp || !cmap)
		return false;

	for (l = 0; l < VID_GRADES; l++)
	{
		for (i = 0; i < 256; i++)
		{
			unsigned int c = d_8to24table[cmap[l * 256 + i]];
			rampluma[l][i] = (unsigned char)
				PP_LUMA((int)((c >> 0) & 0xff),
					(int)((c >> 8) & 0xff),
					(int)((c >> 16) & 0xff));
		}
	}

	for (i = 0; i < 256; i++)
	{
		for (y = 0; y < 256; y++)
		{
			int best = 0;
			int bestdist = 0x7fffffff;

			for (l = 0; l < VID_GRADES; l++)
			{
				int d = (int)rampluma[l][i] - y;
				if (d < 0)
					d = -d;
				if (d < bestdist)
				{
					bestdist = d;
					best = l;
				}
			}
			bestlevel[i][y] = (unsigned char)best;
		}
	}

	for (b = 0; b < 32; b++)
	{
		for (g = 0; g < 32; g++)
		{
			for (r = 0; r < 32; r++)
			{
				int tr = (r * 255) / 31;
				int tg = (g * 255) / 31;
				int tb = (b * 255) / 31;
				int ty = PP_LUMA(tr, tg, tb);
				int best = 0;
				int bestdist = 0x7fffffff;

				/* skip texel 255 (transparent / fullbright), as the
				 * nearest-colour LUT does.  No other texel's ramp
				 * resolves to 255 in the retail colormap, so mode 3
				 * never emits it either. */
				for (i = 0; i < 255; i++)
				{
					int entry = cmap[bestlevel[i][ty] * 256 + i];
					unsigned int pal = d_8to24table[entry];
					int dr = tr - (int)((pal >>  0) & 0xff);
					int dg = tg - (int)((pal >>  8) & 0xff);
					int db = tb - (int)((pal >> 16) & 0xff);
					int dist = dr*dr + dg*dg + db*db;
					if (dist < bestdist)
					{
						bestdist = dist;
						best = entry;
					}
				}
				lut[b * 32 * 32 + g * 32 + r] = (unsigned char)best;
			}
		}
	}

	pp_colormap_lut = PP_UploadLUT(lut);
	return pp_colormap_lut != 0;
}

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

	pp_palette_lut = PP_UploadLUT(lut);

	pp_lut_built = true;
	Con_SafePrintf("PostProcess: palette LUT built\n");

	if (PP_BuildColormapLUT())
		Con_SafePrintf("PostProcess: colormap LUT built\n");
	else
		Con_SafePrintf("PostProcess: no colormap, r_softemu 3 falls back to nearest-colour\n");
}

/* ------------------------------------------------------------------ */
/* OIT FBO + resolve                                                   */
/* ------------------------------------------------------------------ */

static void OIT_DeleteFBO (void)
{
	if (oit_accum_tex) { glDeleteTextures_fp(1, &oit_accum_tex); oit_accum_tex = 0; }
	if (oit_revealage_tex) { glDeleteTextures_fp(1, &oit_revealage_tex); oit_revealage_tex = 0; }
	if (oit_fbo) { glDeleteFramebuffers_fp(1, &oit_fbo); oit_fbo = 0; }
	oit_samples = 0;
	oit_available = false;
}

static qboolean OIT_CreateFBO (int width, int height, GLuint depth_stencil_rb, GLuint depth_stencil_tex, int samples)
{
	GLenum status;
	GLenum drawbufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	GLenum textarget = (samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;

	OIT_DeleteFBO();
	oit_samples = (samples > 1) ? samples : 0;

	/* Accumulation texture (RGBA16F). When MSAA is active these must be
	 * multisampled to share the scene's multisampled depth/stencil. */
	glGenTextures_fp(1, &oit_accum_tex);
	glBindTexture_fp(textarget, oit_accum_tex);
	if (oit_samples > 1)
		glTexImage2DMultisample_fp(GL_TEXTURE_2D_MULTISAMPLE, oit_samples,
					   GL_RGBA16F, width, height, GL_TRUE);
	else
	{
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
				GL_RGBA, GL_FLOAT, NULL);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	glGenTextures_fp(1, &oit_revealage_tex);
	glBindTexture_fp(textarget, oit_revealage_tex);
	if (oit_samples > 1)
		glTexImage2DMultisample_fp(GL_TEXTURE_2D_MULTISAMPLE, oit_samples,
					   GL_RGBA16F, width, height, GL_TRUE);
	else
	{
		glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0,
				GL_RGBA, GL_FLOAT, NULL);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	/* FBO with both color attachments + shared depth/stencil. The scene
	 * FBO supplies depth as either a multisampled renderbuffer (MSAA
	 * branch) or a sampler-readable texture (non-MSAA, lets Hi-Z compute
	 * read it). */
	glGenFramebuffers_fp(1, &oit_fbo);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, oit_fbo);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  textarget, oit_accum_tex, 0);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
				  textarget, oit_revealage_tex, 0);
	if (depth_stencil_tex)
		glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
					  GL_TEXTURE_2D, depth_stencil_tex, 0);
	else
		glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
					     GL_RENDERBUFFER, depth_stencil_rb);
	glDrawBuffers_fp(2, drawbufs);

	status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_SafePrintf("OIT: FBO incomplete (status 0x%x)\n", status);
		OIT_DeleteFBO();
		return false;
	}

	oit_width = width;
	oit_height = height;
	oit_available = true;
	Con_SafePrintf("OIT: %dx%d FBO ready\n", width, height);
	return true;
}

static qboolean OIT_InitShader (void)
{
	GLuint vs, fs, prog;
	GLint linked;

	vs = GL_CompileShader(GL_VERTEX_SHADER, oit_resolve_vert);
	if (!vs) return false;
	fs = GL_CompileShader(GL_FRAGMENT_SHADER, oit_resolve_frag);
	if (!fs) { glDeleteShader_fp(vs); return false; }

	prog = glCreateProgram_fp();
	glAttachShader_fp(prog, vs);
	glAttachShader_fp(prog, fs);
	glLinkProgram_fp(prog);
	glDeleteShader_fp(vs);
	glDeleteShader_fp(fs);

	glGetProgramiv_fp(prog, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		char log[512];
		glGetProgramInfoLog_fp(prog, sizeof(log), NULL, log);
		Con_SafePrintf("OIT resolve link failed: %s\n", log);
		glDeleteProgram_fp(prog);
		return false;
	}

	oit_resolve_prog = prog;
	oit_resolve_loc_accum = glGetUniformLocation_fp(prog, "TexAccum");
	oit_resolve_loc_reveal = glGetUniformLocation_fp(prog, "TexReveal");

#ifndef USE_GLES
	/* MSAA per-sample resolve variant — best-effort. If it fails to build
	 * the MSAA OIT path is simply never selected, since oit_samples stays 0
	 * then.  Not built at all on the ES tier — see oit_resolve_frag_msaa. */
	vs = GL_CompileShader(GL_VERTEX_SHADER, oit_resolve_vert);
	fs = vs ? GL_CompileShader(GL_FRAGMENT_SHADER, oit_resolve_frag_msaa) : 0;
	if (vs && fs)
	{
		prog = glCreateProgram_fp();
		glAttachShader_fp(prog, vs);
		glAttachShader_fp(prog, fs);
		glLinkProgram_fp(prog);
		glGetProgramiv_fp(prog, GL_LINK_STATUS, &linked);
		if (linked)
		{
			oit_resolve_prog_msaa = prog;
			oit_resolve_msaa_loc_accum = glGetUniformLocation_fp(prog, "TexAccum");
			oit_resolve_msaa_loc_reveal = glGetUniformLocation_fp(prog, "TexReveal");
		}
		else
			glDeleteProgram_fp(prog);
	}
	if (vs) glDeleteShader_fp(vs);
	if (fs) glDeleteShader_fp(fs);
#endif /* !USE_GLES */

	/* GL 4.3 core profile requires a VAO bound for any draw call. */
	glGenVertexArrays_fp(1, &oit_resolve_vao);

	Con_SafePrintf("OIT: resolve shader OK\n");
	return true;
}

void OIT_BeginTranslucency (void)
{
	static const float zeroes[4] = {0.f, 0.f, 0.f, 0.f};
	static const float ones[4] = {1.f, 1.f, 1.f, 1.f};

	if (!oit_available || !r_oit.integer || !HW_OIT_HAS_BLEND_FUNCI)
		return;

	oit_in_pass = true;
	glBindFramebuffer_fp(GL_FRAMEBUFFER, oit_fbo);
	glClearBufferfv_fp(GL_COLOR, 0, zeroes);
	glClearBufferfv_fp(GL_COLOR, 1, ones);

	/* Per-buffer blending for WBOIT */
	glEnable_fp(GL_BLEND);
	glBlendFunci_fp(0, GL_ONE, GL_ONE);			/* accum: additive */
	glBlendFunci_fp(1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);	/* revealage: multiplicative */

	/* Translucent geometry reads depth but doesn't write it */
	glDepthMask_fp(0);
}

void OIT_EndTranslucency (GLuint scene_fbo)
{
	GLenum textarget;
	GLuint prog;
	GLint loc_accum, loc_reveal;
	GLboolean cull_was_on;
	GLint saved_viewport[4];

	if (!oit_available || !r_oit.integer || !HW_OIT_HAS_BLEND_FUNCI)
		return;

	textarget = (oit_samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
	prog = (oit_samples > 1) ? oit_resolve_prog_msaa : oit_resolve_prog;
	loc_accum = (oit_samples > 1) ? oit_resolve_msaa_loc_accum : oit_resolve_loc_accum;
	loc_reveal = (oit_samples > 1) ? oit_resolve_msaa_loc_reveal : oit_resolve_loc_reveal;

	oit_in_pass = false;

	cull_was_on = glIsEnabled_fp(GL_CULL_FACE);
	glGetIntegerv_fp(GL_VIEWPORT, saved_viewport);

	glBindFramebuffer_fp(GL_FRAMEBUFFER, scene_fbo);

	glUseProgram_fp(prog);

	glBlendFunc_fp(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable_fp(GL_BLEND);
	glDepthMask_fp(0);
	glDisable_fp(GL_DEPTH_TEST);

	glActiveTexture_fp(GL_TEXTURE0);
	glBindTexture_fp(textarget, oit_accum_tex);
	glActiveTexture_fp(GL_TEXTURE1);
	glBindTexture_fp(textarget, oit_revealage_tex);

	if (loc_accum >= 0) glUniform1i_fp(loc_accum, 0);
	if (loc_reveal >= 0) glUniform1i_fp(loc_reveal, 1);

	/* Full-buffer for the resolve triangle, but SAVED and put back below.
	 * The scene's viewport is not the whole buffer: R_SetupGL() derives it
	 * from r_refdef.vrect, and SCR_CalcRefdef() gives the status bar 36 lines
	 * whenever viewsize < 110, so the 3D view is (0,36,W,H-36).  Leaving the
	 * full-buffer viewport behind put everything drawn after this -- glows,
	 * viewmodel, mirror, the bbox/pointfile debug draws -- through a different
	 * viewport transform than the world it is meant to sit on: same clip
	 * coords, so a downward shift plus a vertical stretch.  Measured at
	 * 640x480 viewsize 100, the mana-orb glows landed 24 px below their orbs
	 * while the world stayed pixel-identical.  uhexen2-osya.
	 *
	 * Same reasoning as the cull-state save/restore below, and the same
	 * mid-frame reason it has to be a restore and not a re-assert: this runs
	 * between R_RenderScene and those passes, none of which set a viewport of
	 * their own.  Queried rather than recomputed so a caller that arrives with
	 * some other viewport gets its own back. */
	glViewport_fp(0, 0, pp_width, pp_height);
	glColorMask_fp(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	/* The resolve triangle is counter-clockwise in NDC, i.e. front-facing,
	 * and this engine culls FRONT faces (gl_vidsdl.c :: GL_Init(), and
	 * R_SetupGL re-asserts it every frame when gl_cull is on).  Leave culling
	 * enabled and the whole fullscreen triangle is thrown away before
	 * rasterisation: accum and revealage are written correctly and then
	 * composited nowhere, so every translucent thing routed through OIT --
	 * sprites, particles, translucent water -- renders as absolutely nothing.
	 * That is uhexen2-z4r1, and uhexen2-a0hp / uhexen2-mex9 before it; it
	 * looked hardware-specific only because nobody had reproduced it on a
	 * second stack.  PP_BlitWith3DEffects and the final blit both disable
	 * culling before their fullscreen draws; this one has to as well.
	 *
	 * Restored rather than left off, because unlike those two this runs
	 * mid-frame -- R_DrawAllGlows, R_DrawViewModel and R_Mirror all draw
	 * after it and expect the scene's cull state.  Queried rather than
	 * assumed, since gl_cull 0 legitimately leaves it off. */
	glDisable_fp(GL_CULL_FACE);

	/* GL 4.3 core profile requires a VAO bound for glDrawArrays. */
	glBindVertexArray_fp(oit_resolve_vao);
	glDrawArrays_fp(GL_TRIANGLES, 0, 3);
	glBindVertexArray_fp(0);

	if (cull_was_on)
		glEnable_fp(GL_CULL_FACE);

	glViewport_fp(saved_viewport[0], saved_viewport[1],
		      saved_viewport[2], saved_viewport[3]);

	glActiveTexture_fp(GL_TEXTURE1);
	glBindTexture_fp(textarget, 0);
	glActiveTexture_fp(GL_TEXTURE0);
	glUseProgram_fp(0);
	glEnable_fp(GL_DEPTH_TEST);
	glDepthMask_fp(1);
	glBlendFunc_fp(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

qboolean OIT_Active (void)
{
	return oit_available && r_oit.integer && HW_OIT_HAS_BLEND_FUNCI;
}

qboolean OIT_InPass (void)
{
	return oit_in_pass;
}

GLuint GL_GetSceneFBO (void)
{
	return pp_fbo;	/* 0 if no postprocess FBO active */
}

GLuint GL_PostProcess_GetSceneDepthTex (void)
{
	return pp_depth_tex;	/* 0 in the MSAA branch or when no FBO is up */
}

/* ------------------------------------------------------------------ */
/* Soft-particle depth snapshot (uhexen2-mf9u)                         */
/* ------------------------------------------------------------------ */

static void PP_DeleteSoftDepth (void)
{
	if (soft_depth_tex) { glDeleteTextures_fp(1, &soft_depth_tex); soft_depth_tex = 0; }
	if (soft_depth_fbo) { glDeleteFramebuffers_fp(1, &soft_depth_fbo); soft_depth_fbo = 0; }
	soft_depth_w = soft_depth_h = 0;
	soft_depth_fmt = 0;
	soft_depth_valid = false;
}

void GL_SoftDepth_Capture (void)
{
	GLenum	fmt, attach, status;
	GLint	prev_draw = 0, prev_read = 0;
	int	w = glwidth, h = glheight;	/* pp rebinds these to the scaled scene size */

	soft_depth_valid = false;

	if (!r_softparticles.integer || !glBlitFramebuffer_fp || w <= 0 || h <= 0)
		return;

	/* MSAA scene: resolving a multisampled depth buffer down to one sample
	 * is implementation-defined, so the snapshot would not reliably match
	 * what the depth test actually compared against.  Skip and let sprites
	 * keep the hard cut rather than fade against a guess.  Mirrors how
	 * R_BuildHiZForNextFrame declines the same path. */
	if (pp_fbo && pp_samples > 1)
		return;

	/* glBlitFramebuffer wants matching depth formats.  The scene FBO carries
	 * packed depth/stencil; the default-framebuffer case mirrors the format
	 * the Hi-Z standalone resolve has been blitting from fb 0 since
	 * uhexen2-9912. */
	if (pp_fbo)	{ fmt = GL_DEPTH24_STENCIL8;  attach = GL_DEPTH_STENCIL_ATTACHMENT; }
	else		{ fmt = GL_DEPTH_COMPONENT24; attach = GL_DEPTH_ATTACHMENT; }

	if (soft_depth_tex && soft_depth_fbo &&
	    soft_depth_w == w && soft_depth_h == h && soft_depth_fmt == fmt)
		goto blit;

	PP_DeleteSoftDepth();

	glGenTextures_fp(1, &soft_depth_tex);
	glBindTexture_fp(GL_TEXTURE_2D, soft_depth_tex);
	glTexImage2D_fp(GL_TEXTURE_2D, 0, fmt, w, h, 0,
			(fmt == GL_DEPTH24_STENCIL8) ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
			(fmt == GL_DEPTH24_STENCIL8) ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_INT,
			NULL);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture_fp(GL_TEXTURE_2D, 0);

	glGetIntegerv_fp(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
	glGenFramebuffers_fp(1, &soft_depth_fbo);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, soft_depth_fbo);
	glFramebufferTexture2D_fp(GL_DRAW_FRAMEBUFFER, attach,
				  GL_TEXTURE_2D, soft_depth_tex, 0);
	/* Depth-only FBO needs DRAW_BUFFER = NONE for completeness on strict
	 * drivers; it is only ever a blit destination. */
	{
		GLenum none = GL_NONE;
		glDrawBuffers_fp(1, &none);
	}
	status = glCheckFramebufferStatus_fp(GL_DRAW_FRAMEBUFFER);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_DPrintf("Soft particles: depth FBO incomplete (0x%x)\n", status);
		PP_DeleteSoftDepth();
		return;
	}

	soft_depth_w = w;
	soft_depth_h = h;
	soft_depth_fmt = fmt;

blit:
	/* Read from whatever the scene is currently drawing into — pp_fbo when
	 * postprocess is up, the default framebuffer otherwise, and either one
	 * again on the mirror pass.  GL_NEAREST is the only legal depth filter. */
	glGetIntegerv_fp(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
	glGetIntegerv_fp(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
	glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, (GLuint)prev_draw);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, soft_depth_fbo);
	glBlitFramebuffer_fp(0, 0, w, h, 0, 0, w, h,
			     GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
	glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, (GLuint)prev_read);

	soft_depth_valid = true;
}

GLuint GL_SoftDepth_GetTex (void)
{
	return soft_depth_valid ? soft_depth_tex : 0;
}

qboolean GL_PostProcess_GetSceneSize (int *w, int *h)
{
	if (!pp_initialized || !pp_fbo || pp_width <= 0 || pp_height <= 0)
		return false;
	if (w) *w = pp_width;
	if (h) *h = pp_height;
	return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void GL_PostProcess_Init (void)
{
	Cvar_RegisterVariable(&r_scale);
	Cvar_RegisterVariable(&r_softemu);
	Cvar_RegisterVariable(&r_dither);
	Cvar_RegisterVariable(&r_hdr);
	Cvar_RegisterVariable(&r_hdr_exposure);
	Cvar_RegisterVariable(&r_oit);
	Cvar_RegisterVariable(&r_bloom);
	Cvar_RegisterVariable(&r_bloom_intensity);
	Cvar_RegisterVariable(&r_bloom_threshold);
	Cvar_RegisterVariable(&r_softparticles);
	Cvar_RegisterVariable(&r_softparticles_scale);

	Cvar_SetCallback(&r_hdr, PP_FormatChanged);
	Cvar_SetCallback(&r_bloom, PP_FormatChanged);

	/* r_oit used to be force-reset to 0 here, discarding an archived 1 on
	 * every startup, because OIT rendered nothing at all.  That was
	 * uhexen2-z4r1: the resolve's fullscreen triangle was front-face culled,
	 * so correctly filled accum/revealage buffers were composited nowhere.
	 * Fixed in 8dcbc6284, so the workaround is now hiding a fixed bug and
	 * stopping anybody from accumulating playtime on the feature.
	 *
	 * Still defaults to 0 -- WBOIT is an approximation, not a free upgrade,
	 * and this only lets the setting persist for someone who opts in.
	 * uhexen2-hhs1. */

	pp_initialized = false;
	pp_active = false;
	pp_fbo = 0;
	pp_color_tex = 0;
	pp_depth_rb = 0;
	pp_depth_tex = 0;
	pp_program = 0;
	pp_width = pp_height = 0;
	/* Context is new — drop the old names rather than deleting them. */
	soft_depth_fbo = 0;
	soft_depth_tex = 0;
	soft_depth_w = soft_depth_h = 0;
	soft_depth_fmt = 0;
	soft_depth_valid = false;

	/* GL 4.3: shaders always available */

	/* check for FBO function pointers */
#ifndef USE_GLES
	/* On desktop GL, dynamically load function pointers */
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
	glTexImage2DMultisample_fp = (glTexImage2DMultisample_f) SDL_GL_GetProcAddress("glTexImage2DMultisample");
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

	/* GL functions loaded at runtime */
	glTexImage3D_fp = (glTexImage3D_f) SDL_GL_GetProcAddress("glTexImage3D");
	glUniform3fv_fp = (glUniform3fv_f) SDL_GL_GetProcAddress("glUniform3fv");
	if (!glUniform2f_fp)
		glUniform2f_fp = (glUniform2f_f) SDL_GL_GetProcAddress("glUniform2f");
#endif /* !USE_GLES */

	if (!PP_InitShader())
	{
		Con_SafePrintf("PostProcess: shader init failed\n");
		return;
	}

	/* build the palette LUT for software rendering emulation */
	PP_BuildPaletteLUT();

	pp_initialized = true;
	Con_SafePrintf("PostProcess: gamma/contrast shader ready\n");

	/* Init OIT resolve shader (FBO created lazily when scene FBO is ready) */
	if (HW_OIT_HAS_BLEND_FUNCI && glDrawBuffers_fp && glClearBufferfv_fp)
		OIT_InitShader();
}

void GL_PostProcess_Shutdown (void)
{
	OIT_DeleteFBO();
	if (oit_resolve_prog) { glDeleteProgram_fp(oit_resolve_prog); oit_resolve_prog = 0; }
	if (oit_resolve_prog_msaa) { glDeleteProgram_fp(oit_resolve_prog_msaa); oit_resolve_prog_msaa = 0; }
	PP_DeleteFBO();
	PP_DeleteNativeFBO();
	PP_DeleteSoftDepth();
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
	if (pp_colormap_lut)
	{
		glDeleteTextures_fp(1, &pp_colormap_lut);
		pp_colormap_lut = 0;
	}
	pp_lut_built = false;
	pp_initialized = false;
	pp_active = false;
	pp_native_active = false;
	pp_prev_yaw = pp_prev_pitch = 0.0f;
	pp_fbo_failed = false;
	if (pp_copyback_tex) { glDeleteTextures_fp(1, &pp_copyback_tex); pp_copyback_tex = 0; }
	pp_copyback_w = pp_copyback_h = 0;
}

void GL_PostProcess_BeginFrame (void)
{
	int w, h;
	float scale;

	pp_active = false;
	pp_native_active = false;

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
	if (pp_fbo_failed)
	{
		static qboolean warned;
		if (!warned)
		{
			Con_Printf("PostProcess: FBO failed, using copyback fallback for gamma/contrast\n");
			warned = true;
		}
		/* No FBO — scene renders to default framebuffer.
		 * EndFrame will copy backbuffer to texture and apply shader. */
		pp_active = true;
		pp_saved_glwidth = glwidth;
		pp_saved_glheight = glheight;
		return;
	}
	if (w != pp_width || h != pp_height || pp_rebuild_pending)
	{
		pp_rebuild_pending = false;
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

/* Apply warp+blur shader to src_tex into the currently-bound framebuffer.
 * Uses identity gamma/contrast/fxaa — 3D-scene effects only. */
static void PP_BlitWith3DEffects (GLuint src_tex, int w, int h, float warp, float blur, float scale)
{
	glDisable_fp(GL_DEPTH_TEST);
	glDisable_fp(GL_BLEND);
	glDisable_fp(GL_CULL_FACE);

	GL_MatrixMode(GL_MAT_PROJECTION);
	GL_PushMatrix();
	GL_LoadIdentity();
	GL_Ortho(0, 1, 0, 1, -1, 1);
	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_PushMatrix();
	GL_LoadIdentity();

	glBindTexture_fp(GL_TEXTURE_2D, src_tex);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glUseProgram_fp(pp_program);
	if (pp_loc_mvp >= 0)
	{
		float mvp[16];
		GL_GetMVP(mvp);
		glUniformMatrix4fv_fp(pp_loc_mvp, 1, GL_FALSE, mvp);
	}
	if (pp_loc_gamma >= 0)     glUniform1f_fp(pp_loc_gamma,    1.0f);
	if (pp_loc_contrast >= 0)  glUniform1f_fp(pp_loc_contrast, 1.0f);
	if (pp_loc_hdr_exposure >= 0) glUniform1f_fp(pp_loc_hdr_exposure, 0.0f);
	if (pp_loc_softemu >= 0)   glUniform1i_fp(pp_loc_softemu,  0);
	if (pp_loc_dither >= 0)    glUniform1f_fp(pp_loc_dither,   0.0f);
	if (pp_loc_fxaa >= 0)      glUniform1f_fp(pp_loc_fxaa,     0.0f);
	if (pp_loc_scale >= 0)     glUniform1f_fp(pp_loc_scale,    scale);
	if (pp_loc_waterwarp >= 0) glUniform1f_fp(pp_loc_waterwarp, warp);
	if (pp_loc_time >= 0)      glUniform1f_fp(pp_loc_time,     (float)realtime);
	if (pp_loc_rcpframe >= 0 && glUniform2f_fp)
		glUniform2f_fp(pp_loc_rcpframe, 1.0f / w, 1.0f / h);
	if (pp_loc_motionblur >= 0)
	{
		float yaw   = cl.viewangles[1];
		float pitch = cl.viewangles[0];
		float dy = (yaw   - pp_prev_yaw)   * 0.002f;
		float dp = (pitch - pp_prev_pitch) * 0.002f;

		/* position-based velocity: project movement onto screen axes */
		{
			vec3_t delta;
			float move_h, move_v;
			VectorSubtract(r_origin, pp_prev_origin, delta);
			move_h = DotProduct(delta, vright) * 0.0004f;
			move_v = DotProduct(delta, vup)    * 0.0004f;
			dy += move_h;
			dp += move_v;
			VectorCopy(r_origin, pp_prev_origin);
		}

		if (dy >  0.03f) dy =  0.03f; else if (dy < -0.03f) dy = -0.03f;
		if (dp >  0.03f) dp =  0.03f; else if (dp < -0.03f) dp = -0.03f;
		glUniform1f_fp(pp_loc_motionblur, blur);
		if (pp_loc_viewdelta >= 0 && glUniform2f_fp)
			glUniform2f_fp(pp_loc_viewdelta, dy, dp);
		pp_prev_yaw   = yaw;
		pp_prev_pitch = pitch;
	}

	GL_ImmBegin();
	GL_ImmColor4f(1, 1, 1, 1);
	GL_ImmTexCoord2f(0, 0); GL_ImmVertex2f(0, 0);
	GL_ImmTexCoord2f(1, 0); GL_ImmVertex2f(1, 0);
	GL_ImmTexCoord2f(1, 1); GL_ImmVertex2f(1, 1);
	GL_ImmTexCoord2f(0, 1); GL_ImmVertex2f(0, 1);
	GL_ImmDraw(GL_QUADS);

	glUseProgram_fp(0);

	GL_MatrixMode(GL_MAT_PROJECTION);
	GL_PopMatrix();
	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_PopMatrix();

	glEnable_fp(GL_DEPTH_TEST);
}

void GL_PostProcess_End3D (void)
{
	int native_w, native_h;
	float warp, blur, scale;
	extern mleaf_t *r_viewleaf;

	if (!pp_active)
		return;

	if (pp_fbo_failed)
	{
		/* Copyback path: 3D scene is in the default framebuffer.
		 * Apply warp/blur now so 2D draws on top of the processed scene. */
		int w = pp_saved_glwidth;
		int h = pp_saved_glheight;

		warp = 0.0f;
		if (r_waterwarp.value && r_viewleaf && r_viewleaf->contents <= CONTENTS_WATER)
			warp = r_waterwarp.value;
		else if (cl.time < pp_waterwarp_preview_end)
			warp = r_waterwarp.value;	/* show preview even if not in water */
		blur = Cvar_VariableValue("r_motionblur");

		if (warp > 0.0f || blur > 0.0f)
		{
			/* Grab the 3D scene from the backbuffer */
			if (!pp_copyback_tex || w != pp_copyback_w || h != pp_copyback_h)
			{
				if (pp_copyback_tex)
					glDeleteTextures_fp(1, &pp_copyback_tex);
				glGenTextures_fp(1, &pp_copyback_tex);
				glBindTexture_fp(GL_TEXTURE_2D, pp_copyback_tex);
				glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGB10_A2, w, h, 0,
						GL_RGBA, GL_UNSIGNED_BYTE, NULL);
				glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				pp_copyback_w = w;
				pp_copyback_h = h;
			}
			glBindTexture_fp(GL_TEXTURE_2D, pp_copyback_tex);
			glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

			/* Apply warp+blur, writing the processed 3D back into the default framebuffer */
			glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);
			glViewport_fp(0, 0, w, h);
			scale = r_scale.value;
			if (scale < 0.25f) scale = 0.25f;
			if (scale > 1.0f)  scale = 1.0f;
			PP_BlitWith3DEffects(pp_copyback_tex, w, h, warp, blur, scale);

			/* 2D will now draw on top of the already-warped 3D scene.
			 * Tell EndFrame to skip warp/blur and only apply gamma/FXAA. */
			pp_native_active = true;
		}
		return;
	}

	/* Resolve MSAA render buffer → pp_color_tex so the shader can sample it */
	if (pp_samples > 1)
	{
		glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, pp_fbo);
		glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, pp_resolve_fbo);
		glBlitFramebuffer_fp(0, 0, pp_width, pp_height,
				      0, 0, pp_width, pp_height,
				      GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	/* Scene depth is final and pp_depth_tex is sampler-readable: build the
	 * Hi-Z pyramid that next frame's cull dispatch consumes (uhexen2-xd87). */
#ifndef USE_GLES
	R_BuildHiZForNextFrame();
#endif

	/* Restore native viewport */
	glwidth = pp_saved_glwidth;
	glheight = pp_saved_glheight;
	native_w = glwidth;
	native_h = glheight;

	/* Ensure composite FBO exists at native resolution */
	if (!PP_CreateNativeFBO(native_w, native_h))
	{
		pp_active = false;
		glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);
		return;
	}

	/* Bind composite FBO — 2D will draw into this after we return */
	glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_native_fbo);
	glViewport_fp(0, 0, native_w, native_h);

	/* Compute 3D-only effect strengths */
	warp = 0.0f;
	if (r_waterwarp.value && r_viewleaf && r_viewleaf->contents <= CONTENTS_WATER)
		warp = r_waterwarp.value;
	else if (cl.time < pp_waterwarp_preview_end)
		warp = r_waterwarp.value;	/* show preview even if not in water */
	blur = Cvar_VariableValue("r_motionblur");
	scale = r_scale.value;
	if (scale < 0.25f) scale = 0.25f;
	if (scale > 1.0f)  scale = 1.0f;

	if (warp > 0.0f || blur > 0.0f)
	{
		/* Shader blit: apply warp+blur, upscale to native res.
		 * Identity gamma/contrast/fxaa so only 3D effects are baked in. */
		PP_BlitWith3DEffects(pp_color_tex, native_w, native_h, warp, blur, scale);
	}
	else
	{
		/* Simple blit: upscale (or same-size copy) with no shader overhead */
		GLuint src = (pp_samples > 1) ? pp_resolve_fbo : pp_fbo;
		glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, src);
		glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, pp_native_fbo);
		glBlitFramebuffer_fp(0, 0, pp_width, pp_height,
				      0, 0, native_w, native_h,
				      GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer_fp(GL_FRAMEBUFFER, pp_native_fbo);
	}

	/* pp_native_fbo is now bound; 2D draws will composite on top of the 3D scene.
	 * EndFrame will read pp_native_color_tex and apply gamma/contrast/FXAA only. */
	pp_native_active = true;
}

void GL_PostProcess_EndFrame (void)
{
	GLuint blit_tex;
	int blit_w, blit_h;

	/* Drain any pending HUD glyph quads before we touch FBOs and shaders. */
	Draw_FlushCharBatch ();

	if (!pp_active)
		return;

	pp_active = false;

	/* Copyback fallback: no FBO, copy backbuffer to texture */
	if (pp_fbo_failed)
	{
		int w = pp_saved_glwidth;
		int h = pp_saved_glheight;
		if (!pp_copyback_tex || w != pp_copyback_w || h != pp_copyback_h)
		{
			if (pp_copyback_tex)
				glDeleteTextures_fp(1, &pp_copyback_tex);
			glGenTextures_fp(1, &pp_copyback_tex);
			glBindTexture_fp(GL_TEXTURE_2D, pp_copyback_tex);
			glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
					GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			pp_copyback_w = w;
			pp_copyback_h = h;
		}
		glBindTexture_fp(GL_TEXTURE_2D, pp_copyback_tex);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
		blit_tex = pp_copyback_tex;
		blit_w = w;
		blit_h = h;
		goto apply_shader;
	}

	/* determine which texture has the composited scene */
	if (pp_native_active)
	{
		/* End3D transferred 3D → composite FBO (with warp/blur already applied);
		 * 2D then drew on top — read the composite from here. */
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

	/* Bloom post-process pass */
	if (r_bloom.integer && bloom_fbo[0] && bloom_bright_prog)
	{
		GLuint bloom_src = pp_native_active ? pp_native_color_tex : pp_color_tex;
		int bloom_src_w = pp_native_active ? pp_native_w : pp_width;
		int bloom_src_h = pp_native_active ? pp_native_h : pp_height;
		int i;

		/* GL 4.3 core profile requires a VAO bound for glDrawArrays. */
		glBindVertexArray_fp(bloom_dummy_vao);

		/* Bright pass: extract overbright pixels */
		glViewport_fp(0, 0, bloom_w[0], bloom_h[0]);
		glBindFramebuffer_fp(GL_FRAMEBUFFER, bloom_fbo[0]);
		glUseProgram_fp(bloom_bright_prog);
		glActiveTexture_fp(GL_TEXTURE0);
		glBindTexture_fp(GL_TEXTURE_2D, bloom_src);
		if (bloom_bright_loc_scene >= 0) glUniform1i_fp(bloom_bright_loc_scene, 0);
		if (bloom_bright_loc_threshold >= 0) glUniform1f_fp(bloom_bright_loc_threshold, r_bloom_threshold.value);
		if (bloom_bright_loc_rcpframe >= 0) glUniform2f_fp(bloom_bright_loc_rcpframe, 1.0f / bloom_src_w, 1.0f / bloom_src_h);
		glDrawArrays_fp(GL_TRIANGLES, 0, 3);

		/* Downsample chain: level 0 → 1 → 2 → 3 */
		for (i = 1; i < BLOOM_LEVELS; i++)
		{
			glViewport_fp(0, 0, bloom_w[i], bloom_h[i]);
			glBindFramebuffer_fp(GL_FRAMEBUFFER, bloom_fbo[i]);
			glUseProgram_fp(bloom_down_prog);
			glActiveTexture_fp(GL_TEXTURE0);
			glBindTexture_fp(GL_TEXTURE_2D, bloom_tex[i-1]);
			if (bloom_down_loc_scene >= 0) glUniform1i_fp(bloom_down_loc_scene, 0);
			if (bloom_down_loc_rcpframe >= 0) glUniform2f_fp(bloom_down_loc_rcpframe, 1.0f / bloom_w[i-1], 1.0f / bloom_h[i-1]);
			glDrawArrays_fp(GL_TRIANGLES, 0, 3);
		}

		/* Upsample + additive blend: level 3 → 2 → 1 → 0.
		 * The upsample shader outputs the tent-filtered coarser level
		 * only; GL_ONE/GL_ONE blending accumulates it into the
		 * downsampled content already in bloom_tex[i] via the ROP,
		 * avoiding the feedback loop that arose from sampling
		 * bloom_tex[i] in the shader while also rendering to it. */
		glEnable_fp(GL_BLEND);
		glBlendFunc_fp(GL_ONE, GL_ONE);
		for (i = BLOOM_LEVELS - 2; i >= 0; i--)
		{
			glViewport_fp(0, 0, bloom_w[i], bloom_h[i]);
			glBindFramebuffer_fp(GL_FRAMEBUFFER, bloom_fbo[i]);
			glUseProgram_fp(bloom_up_prog);
			glActiveTexture_fp(GL_TEXTURE0);
			glBindTexture_fp(GL_TEXTURE_2D, bloom_tex[i + 1]);
			if (bloom_up_loc_scene >= 0) glUniform1i_fp(bloom_up_loc_scene, 0);
			if (bloom_up_loc_rcpframe >= 0) glUniform2f_fp(bloom_up_loc_rcpframe, 1.0f / bloom_w[i+1], 1.0f / bloom_h[i+1]);
			glDrawArrays_fp(GL_TRIANGLES, 0, 3);
		}
		glDisable_fp(GL_BLEND);

		glBindVertexArray_fp(0);
		/* bloom_tex[0] now contains the final bloom result at 1/2 scene res */
	}

	/* unbind scene FBO -- render to default framebuffer */
	glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);

apply_shader:
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
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	/* bind palette LUT on texture unit 1 if softemu is active */
	if ((int)r_softemu.value > 0 && pp_lut_built)
	{
		/* mode 3 swaps in the colormap-derived LUT; the shader is identical
		 * either way, only the table it reads changes */
		GLuint lut = ((int)r_softemu.value == 3 && pp_colormap_lut) ?
				pp_colormap_lut : pp_palette_lut;
		glActiveTexture_fp(GL_TEXTURE0 + 1);
		glBindTexture_fp(GL_TEXTURE_3D, lut);
		glActiveTexture_fp(GL_TEXTURE0);
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
	if (pp_loc_hdr_exposure >= 0)
		glUniform1f_fp(pp_loc_hdr_exposure, r_hdr.integer ? r_hdr_exposure.value : 0.0f);

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

	/* Warp and blur: already baked into pp_native_color_tex when pp_native_active.
	 * Apply here only for the copyback fallback path (pp_native_active=false). */
	if (pp_loc_waterwarp >= 0)
	{
		float warp = 0;
		if (!pp_native_active)
		{
			extern mleaf_t *r_viewleaf;
			if (r_waterwarp.value && r_viewleaf &&
			    r_viewleaf->contents <= CONTENTS_WATER)
				warp = r_waterwarp.value;
			else if (cl.time < pp_waterwarp_preview_end)
				warp = r_waterwarp.value;	/* show preview even if not in water */
		}
		glUniform1f_fp(pp_loc_waterwarp, warp);
	}
	if (pp_loc_time >= 0)
		glUniform1f_fp(pp_loc_time, (float)realtime);
	if (pp_loc_fxaa >= 0)
		glUniform1f_fp(pp_loc_fxaa, Cvar_VariableValue("gl_fxaa"));
	if (pp_loc_rcpframe >= 0 && glUniform2f_fp)
		glUniform2f_fp(pp_loc_rcpframe, 1.0f / glwidth, 1.0f / glheight);
	if (pp_loc_motionblur >= 0)
	{
		float blur = 0, dy = 0, dp = 0;
		if (!pp_native_active)
		{
			float yaw   = cl.viewangles[1];
			float pitch = cl.viewangles[0];
			blur = Cvar_VariableValue("r_motionblur");
			dy = (yaw   - pp_prev_yaw)   * 0.0005f;
			dp = (pitch - pp_prev_pitch) * 0.0005f;

			/* position-based velocity: project movement onto screen axes */
			{
				vec3_t delta;
				float move_h, move_v;
				VectorSubtract(r_origin, pp_prev_origin, delta);
				move_h = DotProduct(delta, vright) * 0.0001f;
				move_v = DotProduct(delta, vup)    * 0.0001f;
				dy += move_h;
				dp += move_v;
				VectorCopy(r_origin, pp_prev_origin);
			}

			if (dy >  0.03f) dy =  0.03f; else if (dy < -0.03f) dy = -0.03f;
			if (dp >  0.03f) dp =  0.03f; else if (dp < -0.03f) dp = -0.03f;
			pp_prev_yaw   = yaw;
			pp_prev_pitch = pitch;
		}
		glUniform1f_fp(pp_loc_motionblur, blur);
		if (pp_loc_viewdelta >= 0 && glUniform2f_fp)
			glUniform2f_fp(pp_loc_viewdelta, dy, dp);
	}

	/* bloom composite texture */
	if (pp_loc_bloom_tex >= 0)
	{
		glActiveTexture_fp(GL_TEXTURE0 + 2);
		glBindTexture_fp(GL_TEXTURE_2D, (r_bloom.integer && bloom_tex[0]) ? bloom_tex[0] : 0);
		glUniform1i_fp(pp_loc_bloom_tex, 2);
		glActiveTexture_fp(GL_TEXTURE0);
	}
	if (pp_loc_bloom_strength >= 0)
		glUniform1f_fp(pp_loc_bloom_strength, r_bloom.integer ? r_bloom_intensity.value : 0.0f);

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
		glActiveTexture_fp(GL_TEXTURE0 + 1);
		glBindTexture_fp(GL_TEXTURE_3D, 0);
		glActiveTexture_fp(GL_TEXTURE0);
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

void GL_PostProcess_RequestWaterwarpPreview (float duration)
{
	pp_waterwarp_preview_end = cl.time + duration;
}

void GL_PostProcess_ResetWaterwarpPreview (void)
{
	pp_waterwarp_preview_end = -1.0f;  /* Clear preview timer on level change */
}
