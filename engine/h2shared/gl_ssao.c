/*
 * gl_ssao.c -- r_ssao: entity contact occlusion (GitHub #188)
 *
 * A port of vkQuake's Quake/r_ssao.c (GPL-2.0-or-later; shaders in
 * engine/shaders/ssao_*.glsl, whose GTAO core is Intel's MIT-licensed XeGTAO,
 * notice in ssao.inc).  vkQuake calls it "experimental screen-space contact
 * occlusion: entity occluders, world receivers", and that scope is the point:
 * the world's own corners are already shaded by its baked lightmaps, so a
 * general SSAO would darken them twice.  What the lightmaps cannot know is a
 * monster, an item or a door standing on that world.  So GTAO is evaluated
 * twice per world pixel -- against the world's depth alone and against the
 * scene's -- and only the ratio, the occlusion the entities ADD, is applied,
 * and only to world pixels.
 *
 * The frame, inside R_RenderScene:
 *   R_DrawWorld          -> R_SSAO_CaptureWorld: snapshot depth, "prepare"
 *   R_DrawEntitiesOnList -> R_SSAO_Apply: snapshot depth again, then
 *                           mip, evaluate, filter x2, composite
 *
 * Where it departs from vkQuake:
 *   - depth reaches the compute stages as two blits into single-sample
 *     DEPTH24_STENCIL8 textures, the scene's own format.  A world pixel is a
 *     receiver when the two are bitwise equal, which replaces vkQuake's
 *     stencil entity mask.  Under MSAA the blit resolves one sample, which an
 *     occlusion estimate tolerates (soft particles, which need the exact depth
 *     the test compared against, decline MSAA instead);
 *   - depth is linearised as z = zb / (za - d), the R_SoftParticleParams form,
 *     so forward-Z, reversed-Z and split depth ranges all work;
 *   - rows: see ssao.inc;
 *   - no subgroups (see ssao_mip_comp.glsl), no FP16, no debug views.
 * Desktop GL 4.3 only: compute and image load/store.  The ES/WebGL2 tier
 * compiles the stubs at the bottom.
 */

#include "quakedef.h"
#include "gl_shader.h"
#include "gl_pipeline.h"
#include "gl_postprocess.h"
#include "gl_matrix.h"
#include "gl_ssao.h"

#ifndef CLAMP
#define CLAMP(min,val,max) ((val) < (min) ? (min) : ((val) > (max) ? (max) : (val)))
#endif
extern float	r_fog_density;	/* gl_fog.c: the world shader's, this frame */

/* 0 off; 1 low (half resolution), 2 medium, 3 high -- vkQuake's ladder. */
cvar_t	r_ssao = {"r_ssao", "0", CVAR_ARCHIVE};
static cvar_t	r_ssao_radius = {"r_ssao_radius", "32", CVAR_ARCHIVE};
static cvar_t	r_ssao_strength = {"r_ssao_strength", "1.0", CVAR_ARCHIVE};
/* vkQuake's ENTITY_SSAO_DEBUG views, kept in the release build: 1 the
 * occlusion multiplier alone (white = none), 2 the receiver mask (white =
 * an entity's own pixel, which takes no occlusion). */
static cvar_t	r_ssao_debug = {"r_ssao_debug", "0", CVAR_NONE};

void R_SSAO_Init (void)
{
	Cvar_RegisterVariable (&r_ssao);
	Cvar_RegisterVariable (&r_ssao_radius);
	Cvar_RegisterVariable (&r_ssao_strength);
	Cvar_RegisterVariable (&r_ssao_debug);
}

#ifndef USE_GLES

#include "shaders_gen.h"

#define SSAO_UBO_BINDING	3	/* 0 Hi-Z, 1 and 2 the shared vert/frag blocks */
#define SSAO_TEX_UNIT		8	/* the shaders' TEXBIND(8..11): clear of the units draws cache */
#define SSAO_PYRAMID_LEVELS	5
#define SSAO_VERSION		"#version 430 core\n"
/* Tile tags: one bit per 128x128 AO tile, 8x4 tiles per texel.  16x16 texels
 * covers a 16384x8192 AO target; R_SSAO_Apply clears them from a buffer this
 * size. */
#define SSAO_MISMATCH_MAX	16

typedef struct
{
	float	viewport[4];	/* xy AO size, zw ndc -> view multiplier */
	float	projection[4];	/* xy ndc -> view offset, w AO scale */
	float	settings[4];	/* x radius, y strength, z fog density / final flag, w slices */
	float	depthlin[4];	/* x za, y zb, z clear depth, w full-res height */
} ssao_params_t;

static GLuint	ss_world_tex, ss_scene_tex, ss_world_fbo, ss_scene_fbo;
static GLuint	ss_pyramid, ss_ao, ss_filtered, ss_edges, ss_mismatch, ss_hilbert;
static GLuint	ss_prep_prog, ss_mip_prog, ss_eval_prog[3], ss_filter_prog, ss_comp_prog;
static GLuint	ss_ubo, ss_vao;
static int	ss_w, ss_h, ss_scale, ss_aow, ss_aoh, ss_levels;
static int	ss_mismatch_w, ss_mismatch_h;
static qboolean	ss_failed;		/* a program or target could not be built; until restart */
static qboolean	ss_world_valid;		/* CaptureWorld ran this frame */
static ssao_params_t	ss_params;

static void SS_DeleteTargets (void)
{
	GLuint	*tex[] = { &ss_world_tex, &ss_scene_tex, &ss_pyramid, &ss_ao, &ss_filtered,
			   &ss_edges, &ss_mismatch, &ss_hilbert };
	size_t	i;

	for (i = 0; i < sizeof(tex) / sizeof(tex[0]); i++)
	{
		if (*tex[i])
		{
			glDeleteTextures_fp (1, tex[i]);
			*tex[i] = 0;
		}
	}
	if (ss_world_fbo) { glDeleteFramebuffers_fp (1, &ss_world_fbo); ss_world_fbo = 0; }
	if (ss_scene_fbo) { glDeleteFramebuffers_fp (1, &ss_scene_fbo); ss_scene_fbo = 0; }
	ss_w = ss_h = ss_scale = 0;
	currenttexture = GL_UNUSED_TEXTURE;
}

void R_SSAO_Shutdown (void)
{
	int	i;

	SS_DeleteTargets ();
	R_PipelineForgetProgram ();	/* GL reuses program names */
	if (ss_prep_prog) { glDeleteProgram_fp (ss_prep_prog); ss_prep_prog = 0; }
	if (ss_mip_prog) { glDeleteProgram_fp (ss_mip_prog); ss_mip_prog = 0; }
	for (i = 0; i < 3; i++)
		if (ss_eval_prog[i]) { glDeleteProgram_fp (ss_eval_prog[i]); ss_eval_prog[i] = 0; }
	if (ss_filter_prog) { glDeleteProgram_fp (ss_filter_prog); ss_filter_prog = 0; }
	if (ss_comp_prog) { glDeleteProgram_fp (ss_comp_prog); ss_comp_prog = 0; }
	if (ss_ubo) { GL_ForgetBuffer (ss_ubo); glDeleteBuffers_fp (1, &ss_ubo); ss_ubo = 0; }
	if (ss_vao) { glDeleteVertexArrays_fp (1, &ss_vao); ss_vao = 0; }
	ss_failed = false;
	ss_world_valid = false;
}

static void SS_BindBlock (GLuint prog)
{
	GLuint	blk;

	if (!prog || !glGetUniformBlockIndex_fp || !glUniformBlockBinding_fp)
		return;
	blk = glGetUniformBlockIndex_fp (prog, "SsaoParams");
	if (blk != GL_INVALID_INDEX)
		glUniformBlockBinding_fp (prog, blk, SSAO_UBO_BINDING);
}

static qboolean SS_BuildPrograms (void)
{
	char	header[64];
	int	q;

	if (ss_comp_prog)
		return true;

	ss_prep_prog = GL_LoadComputeProgram (SSAO_VERSION, ssao_prepare_comp, "ssao_prepare");
	ss_mip_prog = GL_LoadComputeProgram (SSAO_VERSION, ssao_mip_comp, "ssao_mip");
	for (q = 0; q < 3; q++)
	{
		q_snprintf (header, sizeof(header), SSAO_VERSION "#define QUALITY %d\n", q + 1);
		ss_eval_prog[q] = GL_LoadComputeProgram (header, ssao_evaluate_comp, "ssao_evaluate");
	}
	ss_filter_prog = GL_LoadComputeProgram (SSAO_VERSION, ssao_filter_comp, "ssao_filter");
	ss_comp_prog = GL_LoadProgramHeaders (SSAO_VERSION, ssao_composite_vert,
					      SSAO_VERSION, ssao_composite_frag);

	if (!ss_prep_prog || !ss_mip_prog || !ss_eval_prog[0] || !ss_eval_prog[1] ||
	    !ss_eval_prog[2] || !ss_filter_prog || !ss_comp_prog)
	{
		Con_Printf ("r_ssao: a shader failed to build, disabling until vid_restart\n");
		return false;
	}
	SS_BindBlock (ss_prep_prog);
	SS_BindBlock (ss_mip_prog);
	for (q = 0; q < 3; q++)
		SS_BindBlock (ss_eval_prog[q]);
	SS_BindBlock (ss_filter_prog);
	SS_BindBlock (ss_comp_prog);

	glGenBuffers_fp (1, &ss_ubo);
	GL_BindBuffer (GL_UNIFORM_BUFFER, ss_ubo);
	glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(ssao_params_t), NULL, GL_DYNAMIC_DRAW);
	GL_BindBuffer (GL_UNIFORM_BUFFER, 0);
	glGenVertexArrays_fp (1, &ss_vao);
	return true;
}

static GLuint SS_Tex2D (GLsizei levels, GLenum fmt, int w, int h, GLenum min_filter)
{
	GLuint	t;

	glGenTextures_fp (1, &t);
	glBindTexture_fp (GL_TEXTURE_2D, t);
	glTexStorage2D_fp (GL_TEXTURE_2D, levels, fmt, w, h);
	glTexParameteri_fp (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
	glTexParameteri_fp (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri_fp (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri_fp (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

/* A depth snapshot: the scene's own DEPTH24_STENCIL8, so glBlitFramebuffer
 * accepts it, behind a depth-only FBO it is the blit destination of. */
static qboolean SS_DepthTarget (int w, int h, GLuint *tex, GLuint *fbo)
{
	GLenum	none = GL_NONE, status;
	GLint	prev = 0;

	*tex = SS_Tex2D (1, GL_DEPTH24_STENCIL8, w, h, GL_NEAREST);
	glGetIntegerv_fp (GL_DRAW_FRAMEBUFFER_BINDING, &prev);
	glGenFramebuffers_fp (1, fbo);
	glBindFramebuffer_fp (GL_DRAW_FRAMEBUFFER, *fbo);
	glFramebufferTexture2D_fp (GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, *tex, 0);
	glDrawBuffers_fp (1, &none);
	status = glCheckFramebufferStatus_fp (GL_DRAW_FRAMEBUFFER);
	glBindFramebuffer_fp (GL_DRAW_FRAMEBUFFER, (GLuint)prev);
	return status == GL_FRAMEBUFFER_COMPLETE;
}

/* vkQuake's 64x64 Hilbert-curve index: the spatial noise of the evaluate
 * pass, repeating every 64 pixels. */
static void SS_UploadHilbert (void)
{
	static unsigned short	data[64 * 64];
	unsigned int		x, y;

	for (y = 0; y < 64; ++y)
		for (x = 0; x < 64; ++x)
		{
			unsigned int px = x, py = y, index = 0, level;
			for (level = 32; level; level /= 2)
			{
				unsigned int rx = (px & level) != 0, ry = (py & level) != 0;
				index += level * level * ((3 * rx) ^ ry);
				if (!ry)
				{
					unsigned int temp;
					if (rx)
					{
						px = 63 - px;
						py = 63 - py;
					}
					temp = px;
					px = py;
					py = temp;
				}
			}
			data[y * 64 + x] = (unsigned short)index;
		}
	ss_hilbert = SS_Tex2D (1, GL_R16UI, 64, 64, GL_NEAREST);
	glTexSubImage2D_fp (GL_TEXTURE_2D, 0, 0, 0, 64, 64, GL_RED_INTEGER, GL_UNSIGNED_SHORT, data);
}

static qboolean SS_EnsureTargets (int w, int h, int scale)
{
	int	m;

	if (ss_pyramid && w == ss_w && h == ss_h && scale == ss_scale)
		return true;
	SS_DeleteTargets ();

	ss_w = w;
	ss_h = h;
	ss_scale = scale;
	ss_aow = (w + scale - 1) / scale;
	ss_aoh = (h + scale - 1) / scale;
	for (ss_levels = 1, m = q_max (ss_aow, ss_aoh); m > 1 && ss_levels < SSAO_PYRAMID_LEVELS; m >>= 1)
		ss_levels++;
	ss_mismatch_w = (ss_aow + 1023) / 1024;
	ss_mismatch_h = (ss_aoh + 511) / 512;
	if (ss_mismatch_w > SSAO_MISMATCH_MAX || ss_mismatch_h > SSAO_MISMATCH_MAX)
	{
		Con_Printf ("r_ssao: %dx%d is past the tile-tag limit, disabling\n", w, h);
		return false;
	}

	if (!SS_DepthTarget (w, h, &ss_world_tex, &ss_world_fbo) ||
	    !SS_DepthTarget (w, h, &ss_scene_tex, &ss_scene_fbo))
	{
		Con_Printf ("r_ssao: depth snapshot FBO incomplete, disabling\n");
		SS_DeleteTargets ();
		return false;
	}
	ss_pyramid = SS_Tex2D (ss_levels, GL_RG16F, ss_aow, ss_aoh, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri_fp (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, ss_levels - 1);
	ss_ao = SS_Tex2D (1, GL_R8, ss_aow, ss_aoh, GL_NEAREST);
	ss_filtered = SS_Tex2D (1, GL_R8, ss_aow, ss_aoh, GL_NEAREST);
	ss_edges = SS_Tex2D (1, GL_R8, ss_aow, ss_aoh, GL_NEAREST);
	ss_mismatch = SS_Tex2D (1, GL_R32UI, ss_mismatch_w, ss_mismatch_h, GL_NEAREST);
	SS_UploadHilbert ();
	glBindTexture_fp (GL_TEXTURE_2D, 0);
	currenttexture = GL_UNUSED_TEXTURE;
	return true;
}

static void SS_Blit (GLuint dst_fbo)
{
	GLint	prev_draw = 0, prev_read = 0;

	glGetIntegerv_fp (GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
	glGetIntegerv_fp (GL_READ_FRAMEBUFFER_BINDING, &prev_read);
	glBindFramebuffer_fp (GL_READ_FRAMEBUFFER, (GLuint)prev_draw);
	glBindFramebuffer_fp (GL_DRAW_FRAMEBUFFER, dst_fbo);
	glBlitFramebuffer_fp (0, 0, ss_w, ss_h, 0, 0, ss_w, ss_h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer_fp (GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
	glBindFramebuffer_fp (GL_READ_FRAMEBUFFER, (GLuint)prev_read);
}

static void SS_Upload (void)
{
	GL_BindBuffer (GL_UNIFORM_BUFFER, ss_ubo);
	glBufferSubData_fp (GL_UNIFORM_BUFFER, 0, sizeof(ss_params), &ss_params);
	GL_BindBuffer (GL_UNIFORM_BUFFER, 0);
	GL_BindBufferBase (GL_UNIFORM_BUFFER, SSAO_UBO_BINDING, ss_ubo);
}

/* Drop every image binding a pass made.  A texture left bound as a writable
 * image while a later pass samples it reads as a feedback hazard to the
 * driver. */
static void SS_UnbindImages (int count)
{
	int	i;

	for (i = 0; i < count; i++)
		glBindImageTexture_fp (i, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8);
}

static void SS_BindTex (int unit, GLuint tex)
{
	glActiveTexture_fp (GL_TEXTURE0 + SSAO_TEX_UNIT + unit);
	glBindTexture_fp (GL_TEXTURE_2D, tex);
}

static void SS_UnbindTex (int count)
{
	int	i;

	for (i = 0; i < count; i++)
		SS_BindTex (i, 0);
	glActiveTexture_fp (GL_TEXTURE0);
}

static qboolean SS_Active (void)
{
	return r_ssao.integer > 0 && !ss_failed &&
	       gl_renderer_caps.compute_shaders && glBindImageTexture_fp &&
	       glBlitFramebuffer_fp && glTexStorage2D_fp && GL_GetSceneFBO () != 0;
}

/* The R_SoftParticleParams linearisation, from the matrices in force for the
 * world pass, plus the value a cleared pixel holds. */
static qboolean SS_DepthLinearisation (float *za, float *zb, float *clear_depth)
{
	float	proj[16], a, b, range;

	GL_GetProjection (proj);
	if (proj[11] == 0.0f || proj[14] == 0.0f)
		return false;	/* orthographic: no 1/z to invert */
	if (gl_clipcontrol_able)
	{
		a = -proj[10];
		b = -proj[14];
	}
	else
	{
		a = 0.5f - 0.5f * proj[10];
		b = -0.5f * proj[14];
	}
	range = gldepthmax - gldepthmin;
	if (range == 0.0f)
		return false;
	*za = a * range + gldepthmin;
	*zb = b * range;
	*clear_depth = gl_clipcontrol_able ? 0.0f : 1.0f;
	return true;
}

void R_SSAO_CaptureWorld (void)
{
	int	w, h, quality, scale;
	float	za, zb, clear_depth;

	ss_world_valid = false;
	if (!SS_Active ())
		return;
	if (!GL_PostProcess_GetSceneSize (&w, &h) || w <= 0 || h <= 0)
		return;
	if (!SS_DepthLinearisation (&za, &zb, &clear_depth))
		return;
	if (!SS_BuildPrograms () || !SS_EnsureTargets (w, h, (r_ssao.integer <= 1) ? 2 : 1))
	{
		ss_failed = true;
		return;
	}
	quality = (int) CLAMP (1, r_ssao.integer, 3);
	scale = ss_scale;

	SS_Blit (ss_world_fbo);

	memset (&ss_params, 0, sizeof(ss_params));
	ss_params.viewport[0] = (float)ss_aow;
	ss_params.viewport[1] = (float)ss_aoh;
	ss_params.projection[3] = (float)scale;
	ss_params.settings[0] = CLAMP (1.0f, r_ssao_radius.value, 128.0f);
	ss_params.settings[1] = CLAMP (0.0f, r_ssao_strength.value, 1.0f);
	ss_params.settings[3] = (float) q_max (quality, 2);
	ss_params.depthlin[0] = za;
	ss_params.depthlin[1] = zb;
	ss_params.depthlin[2] = clear_depth;
	ss_params.depthlin[3] = (float)h;

	/* The view rectangle, for the ndc -> view mapping, top-down as the AO
	 * textures are (ssao.inc); vkQuake's r_scene_vrect.  The vertical term
	 * takes -P5: GL's projection keeps view-up as NDC-up where Vulkan's
	 * flips it, and the ported shaders assume Vulkan's. */
	{
		GLint	vp[4];
		float	proj[16], vx, vtop, vw, vh;

		glGetIntegerv_fp (GL_VIEWPORT, vp);
		GL_GetProjection (proj);
		vx = (float)vp[0];
		vw = (float)q_max (vp[2], 1);
		vh = (float)q_max (vp[3], 1);
		vtop = (float)h - (float)(vp[1] + vp[3]);
		ss_params.viewport[2] = 2.0f * ss_aow * scale / vw / proj[0];
		ss_params.viewport[3] = 2.0f * ss_aoh * scale / vh / -proj[5];
		ss_params.projection[0] = (-1.0f - (2.0f * vx + scale - 1) / vw) / proj[0];
		ss_params.projection[1] = (-1.0f - (2.0f * vtop + scale - 1) / vh) / -proj[5];
	}
	SS_Upload ();

	SS_BindTex (0, ss_world_tex);
	glBindImageTexture_fp (0, ss_pyramid, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RG16F);
	R_UseProgram (ss_prep_prog);
	glDispatchCompute_fp ((ss_aow + 7) / 8, (ss_aoh + 7) / 8, 1);
	glMemoryBarrier_fp (GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	SS_UnbindImages (1);
	SS_UnbindTex (1);
	R_UseProgram (0);
	ss_world_valid = true;
}

void R_SSAO_Apply (void)
{
	static const GLuint	zeros[SSAO_MISMATCH_MAX * SSAO_MISMATCH_MAX] = { 0 };
	int			quality, level;
	qboolean		cull, depthmask, blend, depthtest;

	if (!ss_world_valid || !SS_Active ())
		return;
	ss_world_valid = false;
	quality = (int) CLAMP (1, r_ssao.integer, 3);

	SS_Blit (ss_scene_fbo);

	/* The tile tags accumulate with atomics, so start them clear.  At most
	 * a handful of texels: glClearTexImage is GL 4.4, this is not. */
	glBindTexture_fp (GL_TEXTURE_2D, ss_mismatch);
	glTexSubImage2D_fp (GL_TEXTURE_2D, 0, 0, 0, ss_mismatch_w, ss_mismatch_h,
			    GL_RED_INTEGER, GL_UNSIGNED_INT, zeros);
	glBindTexture_fp (GL_TEXTURE_2D, 0);
	currenttexture = GL_UNUSED_TEXTURE;
	glMemoryBarrier_fp (GL_TEXTURE_UPDATE_BARRIER_BIT);

	/* mip: scene depth in, the whole pyramid and the tile tags out */
	SS_BindTex (0, ss_scene_tex);
	for (level = 0; level < SSAO_PYRAMID_LEVELS; level++)
		glBindImageTexture_fp (level, ss_pyramid, q_min (level, ss_levels - 1), GL_FALSE, 0,
				       level == 0 ? GL_READ_WRITE : GL_WRITE_ONLY, GL_RG16F);
	glBindImageTexture_fp (5, ss_mismatch, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
	R_UseProgram (ss_mip_prog);
	glDispatchCompute_fp ((ss_aow + 15) / 16, (ss_aoh + 15) / 16, 1);
	glMemoryBarrier_fp (GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	SS_UnbindImages (6);

	/* evaluate: AO and receiver edges */
	SS_BindTex (0, ss_pyramid);
	SS_BindTex (1, ss_hilbert);
	SS_BindTex (2, ss_mismatch);
	glBindImageTexture_fp (0, ss_ao, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
	glBindImageTexture_fp (1, ss_edges, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
	R_UseProgram (ss_eval_prog[quality - 1]);
	glDispatchCompute_fp ((ss_aow + 7) / 8, (ss_aoh + 7) / 8, 1);
	glMemoryBarrier_fp (GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	SS_UnbindImages (2);

	/* filter twice: ao -> filtered, then filtered -> ao with the final
	 * weighting (settings.z) */
	R_UseProgram (ss_filter_prog);
	SS_BindTex (1, ss_edges);
	ss_params.settings[2] = 0.0f;
	SS_Upload ();
	SS_BindTex (0, ss_ao);
	glBindImageTexture_fp (0, ss_filtered, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
	glDispatchCompute_fp ((ss_aow + 15) / 16, (ss_aoh + 7) / 8, 1);
	glMemoryBarrier_fp (GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	SS_UnbindImages (1);
	ss_params.settings[2] = 1.0f;
	SS_Upload ();
	SS_BindTex (0, ss_filtered);
	glBindImageTexture_fp (0, ss_ao, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
	glDispatchCompute_fp ((ss_aow + 15) / 16, (ss_aoh + 7) / 8, 1);
	glMemoryBarrier_fp (GL_TEXTURE_FETCH_BARRIER_BIT);
	SS_UnbindImages (1);

	/* composite into the scene, world receivers only */
	ss_params.settings[2] = r_fog_density;
	ss_params.settings[3] = (float) CLAMP (0, r_ssao_debug.integer, 2);
	SS_Upload ();
	SS_BindTex (0, ss_world_tex);
	SS_BindTex (1, ss_scene_tex);
	SS_BindTex (2, ss_ao);
	SS_BindTex (3, ss_pyramid);
	glActiveTexture_fp (GL_TEXTURE0);

	cull = R_GetCull ();
	blend = R_GetBlend ();
	depthtest = glIsEnabled_fp (GL_DEPTH_TEST) ? true : false;
	depthmask = R_GetDepthMask ();
	R_SetCull (false);
	R_SetDepthTest (false);
	R_SetDepthMask (false);
	R_SetColorMask (true, true, true, false);	/* the scene's alpha is not ours */
	R_SetBlend (true);
	R_SetBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	R_UseProgram (ss_comp_prog);
	glBindVertexArray_fp (ss_vao);
	glDrawArrays_fp (GL_TRIANGLES, 0, 3);
	glBindVertexArray_fp (0);

	R_SetBlend (blend);
	R_SetColorMask (true, true, true, true);
	R_SetDepthMask (depthmask);
	R_SetDepthTest (depthtest);
	R_SetCull (cull);
	SS_UnbindTex (4);
	R_UseProgram (0);
}

#else	/* USE_GLES: no compute, no image load/store */

void R_SSAO_Shutdown (void) {}
void R_SSAO_CaptureWorld (void) {}
void R_SSAO_Apply (void) {}

#endif
