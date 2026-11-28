/* gl_pipeline.c -- render pipeline state objects and the GL state shadow
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
#include "gl_pipeline.h"
#ifdef USE_SDL_GPU
#include "rg_device.h"
#endif

/* Every GL call in this module goes through RP_GL.  Under -gpu (#340 P1c)
 * there is no GL context: the shadow below IS the draw state, and the
 * SDL_GPU backend builds each draw's pipeline object from it
 * (R_PipelineState), so the call is skipped and only the shadow moves.
 * tools/test_rg_sources.py holds that no GL call here escapes it. */
#ifdef USE_SDL_GPU
#define RP_GL(call)	do { if (!R_GPU ()) { call; } } while (0)
#else
#define RP_GL(call)	do { call; } while (0)
#endif

/* glBlendFunci is GL 4.0 and is not in WebGL2, where gl_func.h makes it a
 * no-op function-like macro that cannot be tested as a value.  Same shape as
 * HW_OIT_HAS_BLEND_FUNCI in gl_postprocess.c. */
#ifdef USE_GLES
#define RP_HAS_BLEND_FUNCI	0
#else
#define RP_HAS_BLEND_FUNCI	(glBlendFunci_fp != NULL)
#endif

/* The shadow.  Initialised to the GL spec defaults so it is already truthful
 * on a fresh context; R_PipelineResetState re-asserts them anyway so that a
 * context which arrived in some other state is dragged into agreement rather
 * than silently disagreeing.  The type is in gl_pipeline.h. */
static rpipestate_t rp = {
	false,					/* blend */
	{ GL_ONE, GL_ONE },			/* blend_src */
	{ GL_ZERO, GL_ZERO },			/* blend_dst */
	{ GL_ONE, GL_ONE },			/* blend_eff_src */
	{ GL_ZERO, GL_ZERO },			/* blend_eff_dst */
	false,					/* depth_test */
	true,					/* depth_mask */
	GL_LESS,				/* depth_func */
	0.0, 1.0,				/* depth range */
	false,					/* cull */
	GL_BACK,				/* cull_face */
	GL_FILL,				/* polygon_mode */
	false, false,				/* polygon offset fill / line */
	0.0f, 0.0f,				/* polygon offset factor / units */
	false,					/* stencil_test */
	GL_ALWAYS, 0, ~0u,			/* stencil func / ref / mask */
	GL_KEEP, GL_KEEP, GL_KEEP,		/* stencil op */
	{ true, true, true, true },		/* color mask */
	false,					/* alpha_to_coverage */
	0,					/* program */
	true					/* program_known */
};

/* ------------------------------------------------------------------ */
/* Blending                                                            */
/* ------------------------------------------------------------------ */

void R_SetBlend (qboolean enable)
{
	if (rp.blend == !!enable)
		return;
	rp.blend = enable ? true : false;
	if (rp.blend)
	{
		RP_GL (glEnable_fp (GL_BLEND));
		/* Some drivers reset the per-draw-buffer blend functions to the
		 * global pair when GL_BLEND is enabled, which silently breaks the
		 * WBOIT revealage buffer -- every translucent surface disappears.
		 * The old code worked around it by re-issuing glBlendFunci before
		 * every draw inside the OIT pass, which a redundancy filter would
		 * have thrown away.  Model the quirk instead: enabling blend makes
		 * the indexed funcs unknown, so the next R_SetBlendFuncIndexed
		 * really does reach GL, and the ones after it still fold away. */
		rp.blend_src[0] = rp.blend_dst[0] = RP_ENUM_UNKNOWN;
		rp.blend_src[1] = rp.blend_dst[1] = RP_ENUM_UNKNOWN;
	}
	else
	{
		RP_GL (glDisable_fp (GL_BLEND));
	}
}

void R_SetBlendFunc (GLenum src, GLenum dst)
{
	int i;

	if (rp.blend_src[0] == src && rp.blend_dst[0] == dst &&
	    rp.blend_src[1] == src && rp.blend_dst[1] == dst)
		return;

	RP_GL (glBlendFunc_fp (src, dst));
	/* glBlendFunc is the un-indexed entry point: it sets every draw
	 * buffer, so the whole indexed shadow follows it. */
	for (i = 0; i < RP_MAX_BLEND_TARGETS; i++)
	{
		rp.blend_src[i] = rp.blend_eff_src[i] = src;
		rp.blend_dst[i] = rp.blend_eff_dst[i] = dst;
	}
}

void R_SetBlendFuncIndexed (GLuint buf, GLenum src, GLenum dst)
{
	if (!RP_HAS_BLEND_FUNCI)
		return;
	if (buf < RP_MAX_BLEND_TARGETS &&
	    rp.blend_src[buf] == src && rp.blend_dst[buf] == dst)
		return;

	RP_GL (glBlendFunci_fp (buf, src, dst));
	if (buf < RP_MAX_BLEND_TARGETS)
	{
		rp.blend_src[buf] = rp.blend_eff_src[buf] = src;
		rp.blend_dst[buf] = rp.blend_eff_dst[buf] = dst;
	}
}

/* ------------------------------------------------------------------ */
/* Depth                                                               */
/* ------------------------------------------------------------------ */

void R_SetDepthTest (qboolean enable)
{
	if (rp.depth_test == !!enable)
		return;
	rp.depth_test = enable ? true : false;
	if (rp.depth_test)
		RP_GL (glEnable_fp (GL_DEPTH_TEST));
	else
		RP_GL (glDisable_fp (GL_DEPTH_TEST));
}

void R_SetDepthMask (qboolean enable)
{
	if (rp.depth_mask == !!enable)
		return;
	rp.depth_mask = enable ? true : false;
	RP_GL (glDepthMask_fp (rp.depth_mask ? GL_TRUE : GL_FALSE));
}

void R_SetDepthFunc (GLenum func)
{
	if (rp.depth_func == func)
		return;
	rp.depth_func = func;
	RP_GL (glDepthFunc_fp (func));
}

void R_SetDepthRange (double znear, double zfar)
{
	if (rp.depth_near == znear && rp.depth_far == zfar)
		return;
	rp.depth_near = znear;
	rp.depth_far = zfar;
	RP_GL (glDepthRange_fp (znear, zfar));
}

/* ------------------------------------------------------------------ */
/* Rasteriser                                                          */
/* ------------------------------------------------------------------ */

void R_SetCull (qboolean enable)
{
	if (rp.cull == !!enable)
		return;
	rp.cull = enable ? true : false;
	if (rp.cull)
		RP_GL (glEnable_fp (GL_CULL_FACE));
	else
		RP_GL (glDisable_fp (GL_CULL_FACE));
}

void R_SetCullFace (GLenum face)
{
	if (rp.cull_face == face)
		return;
	rp.cull_face = face;
	RP_GL (glCullFace_fp (face));
}

qboolean R_GetCull (void)
{
	return rp.cull;
}

qboolean R_GetBlend (void)
{
	return rp.blend;
}

qboolean R_GetDepthMask (void)
{
	return rp.depth_mask;
}

void R_GetDepthRange (double *znear, double *zfar)
{
	if (znear) *znear = rp.depth_near;
	if (zfar)  *zfar  = rp.depth_far;
}

qboolean R_GetDepthTest (void)
{
	return rp.depth_test;
}

GLenum R_GetDepthFunc (void)
{
	return rp.depth_func;
}

GLenum R_GetCullFace (void)
{
	return rp.cull_face;
}

qboolean R_GetStencilTest (void)
{
	return rp.stencil_test;
}

qboolean R_GetPolygonOffsetFill (float *factor, float *units)
{
	if (factor) *factor = rp.poly_factor;
	if (units)  *units  = rp.poly_units;
	return rp.poly_offset_fill;
}

/* The disable paths also zero factor/units, matching what the scattered
 * clusters did: every glDisable(GL_POLYGON_OFFSET_FILL) in the engine was
 * paired with a glPolygonOffset(0,0) on the next line. */
void R_SetPolygonOffsetFill (qboolean enable, float factor, float units)
{
	if (!enable)
	{
		factor = 0.0f;
		units = 0.0f;
	}

	if (rp.poly_offset_fill != !!enable)
	{
		rp.poly_offset_fill = enable ? true : false;
		if (rp.poly_offset_fill)
			RP_GL (glEnable_fp (GL_POLYGON_OFFSET_FILL));
		else
			RP_GL (glDisable_fp (GL_POLYGON_OFFSET_FILL));
	}
	if (rp.poly_factor != factor || rp.poly_units != units)
	{
		rp.poly_factor = factor;
		rp.poly_units = units;
		RP_GL (glPolygonOffset_fp (factor, units));
	}
}

void R_SetPolygonOffsetLine (qboolean enable, float factor, float units)
{
#ifdef USE_GLES
	/* The ES tier has no polygon offset for line rasterization -- unlike
	 * glBlendFunci above, which gl_func.h can stub, GL_POLYGON_OFFSET_LINE
	 * is not even an enum in <GLES3/gl3.h>, so this has to be compiled out
	 * rather than skipped at runtime.  Nothing calls this yet; it exists
	 * for the desktop wireframe paths. */
	(void) enable;
	(void) factor;
	(void) units;
#else
	if (!enable)
	{
		factor = 0.0f;
		units = 0.0f;
	}

	if (rp.poly_offset_line != !!enable)
	{
		rp.poly_offset_line = enable ? true : false;
		if (rp.poly_offset_line)
			RP_GL (glEnable_fp (GL_POLYGON_OFFSET_LINE));
		else
			RP_GL (glDisable_fp (GL_POLYGON_OFFSET_LINE));
	}
	if (rp.poly_factor != factor || rp.poly_units != units)
	{
		rp.poly_factor = factor;
		rp.poly_units = units;
		RP_GL (glPolygonOffset_fp (factor, units));
	}
#endif	/* USE_GLES */
}

void R_SetPolygonMode (GLenum mode)
{
#ifdef USE_GLES
	/* No glPolygonMode on the ES tier, and the showtris pass returns
	 * early there anyway -- same shape as R_SetPolygonOffsetLine above,
	 * whose enum is not even in <GLES3/gl3.h>. */
	(void) mode;
#else
	if (rp.polygon_mode == mode)
		return;
	rp.polygon_mode = mode;
	RP_GL (glPolygonMode_fp (GL_FRONT_AND_BACK, mode));
#endif	/* USE_GLES */
}

/* ------------------------------------------------------------------ */
/* Stencil                                                             */
/* ------------------------------------------------------------------ */

void R_SetStencilTest (qboolean enable)
{
	if (rp.stencil_test == !!enable)
		return;
	rp.stencil_test = enable ? true : false;
	if (rp.stencil_test)
		RP_GL (glEnable_fp (GL_STENCIL_TEST));
	else
		RP_GL (glDisable_fp (GL_STENCIL_TEST));
}

void R_SetStencilFunc (GLenum func, GLint ref, GLuint mask)
{
	if (rp.stencil_func == func && rp.stencil_ref == ref &&
	    rp.stencil_mask == mask)
		return;
	rp.stencil_func = func;
	rp.stencil_ref = ref;
	rp.stencil_mask = mask;
	RP_GL (glStencilFunc_fp (func, ref, mask));
}

void R_SetStencilOp (GLenum sfail, GLenum dpfail, GLenum dppass)
{
	if (rp.stencil_sfail == sfail && rp.stencil_dpfail == dpfail &&
	    rp.stencil_dppass == dppass)
		return;
	rp.stencil_sfail = sfail;
	rp.stencil_dpfail = dpfail;
	rp.stencil_dppass = dppass;
	RP_GL (glStencilOp_fp (sfail, dpfail, dppass));
}

/* ------------------------------------------------------------------ */
/* Framebuffer writes                                                  */
/* ------------------------------------------------------------------ */

void R_SetColorMask (qboolean r, qboolean g, qboolean b, qboolean a)
{
	if (rp.color_mask[0] == !!r && rp.color_mask[1] == !!g &&
	    rp.color_mask[2] == !!b && rp.color_mask[3] == !!a)
		return;
	rp.color_mask[0] = r ? true : false;
	rp.color_mask[1] = g ? true : false;
	rp.color_mask[2] = b ? true : false;
	rp.color_mask[3] = a ? true : false;
	RP_GL (glColorMask_fp (r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE,
			b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE));
}

void R_SetAlphaToCoverage (qboolean enable)
{
	if (rp.alpha_to_coverage == !!enable)
		return;
	rp.alpha_to_coverage = enable ? true : false;
	if (rp.alpha_to_coverage)
		RP_GL (glEnable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE));
	else
		RP_GL (glDisable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE));
}

/* ------------------------------------------------------------------ */
/* Point size, scissor, sample shading — unshadowed state              */
/* ------------------------------------------------------------------ */

/* Deliberately outside the shadow above: each is always set as a balanced
 * pair at the call site (point size 2.0 -> 1.0 around the square-particle
 * batch, scissor enable -> disable around the CSQC clip area and off ->
 * saved state around gl_shadow.c's passes, sample shading on -> off around
 * the 3D pass), so there is nothing for the shadow to deduplicate and no
 * state for a later draw to inherit unknowingly.  They live here, not at the
 * call sites, so no draw-state call sits outside this module: a second
 * backend (SDL_GPU, #447) needs one place that knows the whole draw state.
 * GitHub #453, #337. */
void GL_SetPointSize (float size)
{
	/* No glPointSize on the ES tier (gl_func.h stubs it): point size comes
	 * from the vertex shader's gl_PointSize output, so square-particle mode
	 * draws at 1px there.  The call is still spelled the same on both tiers. */
	RP_GL (glPointSize_fp (size));
}

void R_SetViewport (int x, int y, int width, int height)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_SetViewport (x, y, width, height);
		return;
	}
#endif
	glViewport_fp (x, y, width, height);
}

/* glGetIntegerv (GL_VIEWPORT): x, y, width, height (#340 P5a-1). */
void R_GetViewport (GLint vp[4])
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_GetViewport (vp);
		return;
	}
#endif
	glGetIntegerv_fp (GL_VIEWPORT, vp);
}

/* The values glClear writes, GL's defaults until set.  Not draw state, so
 * nothing is folded: every GL arm below is the bare call the call site used
 * to make.  Under -gpu they are what the next render pass clears to
 * (#340 P2b). */
static float	rp_clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static double	rp_clear_depth = 1.0;
static GLint	rp_clear_stencil = 0;

void R_SetClearColor (float r, float g, float b, float a)
{
	rp_clear_color[0] = r;
	rp_clear_color[1] = g;
	rp_clear_color[2] = b;
	rp_clear_color[3] = a;
	RP_GL (glClearColor_fp (r, g, b, a));
}

void R_SetClearDepth (double depth)
{
	rp_clear_depth = depth;
	RP_GL (glClearDepth_fp (depth));
}

void R_SetClearStencil (GLint s)
{
	rp_clear_stencil = s;
	RP_GL (glClearStencil_fp (s));
}

void R_ClearBuffers (GLbitfield mask)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_Clear (mask, rp_clear_color, (float) rp_clear_depth, rp_clear_stencil);
		return;
	}
#endif
	glClear_fp (mask);
}

void GL_SetScissor (int x, int y, int width, int height)
{
	/* Coordinates are GL-native (origin bottom-left); the caller flips from
	 * Quake's top-left origin.  The SDL_GPU arm takes them unchanged: its
	 * render targets hold GL's rows in GL's order (VK_FLIP_Y, layout.inc). */
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_SetScissor (true, x, y, width, height);
		return;
	}
#endif
	glEnable_fp (GL_SCISSOR_TEST);
	glScissor_fp (x, y, width, height);
}

void GL_ResetScissor (void)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_SetScissor (false, 0, 0, 0, 0);
		return;
	}
#endif
	glDisable_fp (GL_SCISSOR_TEST);
}

qboolean R_GetScissor (GLint box[4])
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		return RG_GetScissor (box);
	}
#endif
	if (!glIsEnabled_fp (GL_SCISSOR_TEST))
		return false;
	glGetIntegerv_fp (GL_SCISSOR_BOX, box);
	return true;
}

/* frac <= 0 disables.  Desktop only -- the ES tier has no
 * glMinSampleShading, and the callers are compiled out there. */
void GL_SetSampleShading (float frac)
{
#ifndef USE_GLES
	if (frac > 0.0f)
	{
		RP_GL (glEnable_fp (GL_SAMPLE_SHADING));
		RP_GL (glMinSampleShading_fp (frac));
	}
	else
	{
		RP_GL (glDisable_fp (GL_SAMPLE_SHADING));
	}
#else
	(void) frac;
#endif
}

/* ------------------------------------------------------------------ */
/* Program                                                             */
/* ------------------------------------------------------------------ */

void R_UseProgram (GLuint program)
{
	if (rp.program_known && rp.program == program)
		return;
	rp.program = program;
	rp.program_known = true;
	RP_GL (glUseProgram_fp (program));
}

void R_PipelineForgetProgram (void)
{
	rp.program_known = false;
}

const rpipestate_t *R_PipelineState (void)
{
	return &rp;
}

/* ------------------------------------------------------------------ */
/* Bundles                                                             */
/* ------------------------------------------------------------------ */

void R_BindPipeline (const rpipeline_t *p)
{
	R_UseProgram (p->program);

	R_SetBlend (p->blend);
	if (p->blend)
		R_SetBlendFunc (p->blend_src, p->blend_dst);

	R_SetDepthTest (p->depth_test);
	R_SetDepthMask (p->depth_write);
	if (p->depth_func != RP_KEEP_ENUM)
		R_SetDepthFunc (p->depth_func);

	if (p->cull_face != RP_KEEP_ENUM)
		R_SetCullFace (p->cull_face);
	R_SetCull (p->cull);

	if (p->polygon_mode != RP_KEEP_ENUM)
		R_SetPolygonMode (p->polygon_mode);

	R_SetAlphaToCoverage (p->alpha_to_coverage);
}

/* ------------------------------------------------------------------ */
/* Context boundaries                                                  */
/* ------------------------------------------------------------------ */

void R_PipelineResetState (void)
{
	int i;

	RP_GL (glDisable_fp (GL_BLEND));
	RP_GL (glBlendFunc_fp (GL_ONE, GL_ZERO));
	RP_GL (glDisable_fp (GL_DEPTH_TEST));
	RP_GL (glDepthMask_fp (GL_TRUE));
	RP_GL (glDepthFunc_fp (GL_LESS));
	RP_GL (glDepthRange_fp (0.0, 1.0));
	RP_GL (glDisable_fp (GL_CULL_FACE));
	RP_GL (glCullFace_fp (GL_BACK));
	RP_GL (glPolygonMode_fp (GL_FRONT_AND_BACK, GL_FILL));
	RP_GL (glDisable_fp (GL_POLYGON_OFFSET_FILL));
	RP_GL (glPolygonOffset_fp (0.0f, 0.0f));
	RP_GL (glDisable_fp (GL_STENCIL_TEST));
	RP_GL (glStencilFunc_fp (GL_ALWAYS, 0, ~0u));
	RP_GL (glStencilOp_fp (GL_KEEP, GL_KEEP, GL_KEEP));
	RP_GL (glColorMask_fp (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
	RP_GL (glDisable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE));
	RP_GL (glUseProgram_fp (0));

	rp.blend = false;
	for (i = 0; i < RP_MAX_BLEND_TARGETS; i++)
	{
		rp.blend_src[i] = rp.blend_eff_src[i] = GL_ONE;
		rp.blend_dst[i] = rp.blend_eff_dst[i] = GL_ZERO;
	}
	rp.depth_test = false;
	rp.depth_mask = true;
	rp.depth_func = GL_LESS;
	rp.depth_near = 0.0;
	rp.depth_far = 1.0;
	rp.cull = false;
	rp.cull_face = GL_BACK;
	rp.polygon_mode = GL_FILL;
	rp.poly_offset_fill = false;
	rp.poly_offset_line = false;
	rp.poly_factor = 0.0f;
	rp.poly_units = 0.0f;
	rp.stencil_test = false;
	rp.stencil_func = GL_ALWAYS;
	rp.stencil_ref = 0;
	rp.stencil_mask = ~0u;
	rp.stencil_sfail = GL_KEEP;
	rp.stencil_dpfail = GL_KEEP;
	rp.stencil_dppass = GL_KEEP;
	rp.color_mask[0] = true;
	rp.color_mask[1] = true;
	rp.color_mask[2] = true;
	rp.color_mask[3] = true;
	rp.alpha_to_coverage = false;
	rp.program = 0;
	rp.program_known = true;
}
