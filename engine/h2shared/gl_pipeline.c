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
 * than silently disagreeing. */
typedef struct {
	qboolean	blend;
	GLenum		blend_src[RP_MAX_BLEND_TARGETS];
	GLenum		blend_dst[RP_MAX_BLEND_TARGETS];
	qboolean	depth_test;
	qboolean	depth_mask;
	GLenum		depth_func;
	double		depth_near, depth_far;
	qboolean	cull;
	GLenum		cull_face;
	qboolean	poly_offset_fill;
	qboolean	poly_offset_line;
	float		poly_factor, poly_units;
	qboolean	stencil_test;
	GLenum		stencil_func;
	GLint		stencil_ref;
	GLuint		stencil_mask;
	GLenum		stencil_sfail, stencil_dpfail, stencil_dppass;
	qboolean	color_mask[4];
	qboolean	alpha_to_coverage;
	GLuint		program;
	qboolean	program_known;
} rpipestate_t;

static rpipestate_t rp = {
	false,					/* blend */
	{ GL_ONE, GL_ONE },			/* blend_src */
	{ GL_ZERO, GL_ZERO },			/* blend_dst */
	false,					/* depth_test */
	true,					/* depth_mask */
	GL_LESS,				/* depth_func */
	0.0, 1.0,				/* depth range */
	false,					/* cull */
	GL_BACK,				/* cull_face */
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
		glEnable_fp (GL_BLEND);
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
		glDisable_fp (GL_BLEND);
	}
}

void R_SetBlendFunc (GLenum src, GLenum dst)
{
	int i;

	if (rp.blend_src[0] == src && rp.blend_dst[0] == dst &&
	    rp.blend_src[1] == src && rp.blend_dst[1] == dst)
		return;

	glBlendFunc_fp (src, dst);
	/* glBlendFunc is the un-indexed entry point: it sets every draw
	 * buffer, so the whole indexed shadow follows it. */
	for (i = 0; i < RP_MAX_BLEND_TARGETS; i++)
	{
		rp.blend_src[i] = src;
		rp.blend_dst[i] = dst;
	}
}

void R_SetBlendFuncIndexed (GLuint buf, GLenum src, GLenum dst)
{
	if (!RP_HAS_BLEND_FUNCI)
		return;
	if (buf < RP_MAX_BLEND_TARGETS &&
	    rp.blend_src[buf] == src && rp.blend_dst[buf] == dst)
		return;

	glBlendFunci_fp (buf, src, dst);
	if (buf < RP_MAX_BLEND_TARGETS)
	{
		rp.blend_src[buf] = src;
		rp.blend_dst[buf] = dst;
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
		glEnable_fp (GL_DEPTH_TEST);
	else
		glDisable_fp (GL_DEPTH_TEST);
}

void R_SetDepthMask (qboolean enable)
{
	if (rp.depth_mask == !!enable)
		return;
	rp.depth_mask = enable ? true : false;
	glDepthMask_fp (rp.depth_mask ? GL_TRUE : GL_FALSE);
}

void R_SetDepthFunc (GLenum func)
{
	if (rp.depth_func == func)
		return;
	rp.depth_func = func;
	glDepthFunc_fp (func);
}

void R_SetDepthRange (double znear, double zfar)
{
	if (rp.depth_near == znear && rp.depth_far == zfar)
		return;
	rp.depth_near = znear;
	rp.depth_far = zfar;
	glDepthRange_fp (znear, zfar);
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
		glEnable_fp (GL_CULL_FACE);
	else
		glDisable_fp (GL_CULL_FACE);
}

void R_SetCullFace (GLenum face)
{
	if (rp.cull_face == face)
		return;
	rp.cull_face = face;
	glCullFace_fp (face);
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
			glEnable_fp (GL_POLYGON_OFFSET_FILL);
		else
			glDisable_fp (GL_POLYGON_OFFSET_FILL);
	}
	if (rp.poly_factor != factor || rp.poly_units != units)
	{
		rp.poly_factor = factor;
		rp.poly_units = units;
		glPolygonOffset_fp (factor, units);
	}
}

void R_SetPolygonOffsetLine (qboolean enable, float factor, float units)
{
	if (!enable)
	{
		factor = 0.0f;
		units = 0.0f;
	}

	if (rp.poly_offset_line != !!enable)
	{
		rp.poly_offset_line = enable ? true : false;
		if (rp.poly_offset_line)
			glEnable_fp (GL_POLYGON_OFFSET_LINE);
		else
			glDisable_fp (GL_POLYGON_OFFSET_LINE);
	}
	if (rp.poly_factor != factor || rp.poly_units != units)
	{
		rp.poly_factor = factor;
		rp.poly_units = units;
		glPolygonOffset_fp (factor, units);
	}
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
		glEnable_fp (GL_STENCIL_TEST);
	else
		glDisable_fp (GL_STENCIL_TEST);
}

void R_SetStencilFunc (GLenum func, GLint ref, GLuint mask)
{
	if (rp.stencil_func == func && rp.stencil_ref == ref &&
	    rp.stencil_mask == mask)
		return;
	rp.stencil_func = func;
	rp.stencil_ref = ref;
	rp.stencil_mask = mask;
	glStencilFunc_fp (func, ref, mask);
}

void R_SetStencilOp (GLenum sfail, GLenum dpfail, GLenum dppass)
{
	if (rp.stencil_sfail == sfail && rp.stencil_dpfail == dpfail &&
	    rp.stencil_dppass == dppass)
		return;
	rp.stencil_sfail = sfail;
	rp.stencil_dpfail = dpfail;
	rp.stencil_dppass = dppass;
	glStencilOp_fp (sfail, dpfail, dppass);
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
	glColorMask_fp (r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE,
			b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
}

void R_SetAlphaToCoverage (qboolean enable)
{
	if (rp.alpha_to_coverage == !!enable)
		return;
	rp.alpha_to_coverage = enable ? true : false;
	if (rp.alpha_to_coverage)
		glEnable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	else
		glDisable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
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
	glUseProgram_fp (program);
}

void R_PipelineForgetProgram (void)
{
	rp.program_known = false;
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

	R_SetAlphaToCoverage (p->alpha_to_coverage);
}

/* ------------------------------------------------------------------ */
/* Context boundaries                                                  */
/* ------------------------------------------------------------------ */

void R_PipelineResetState (void)
{
	int i;

	glDisable_fp (GL_BLEND);
	glBlendFunc_fp (GL_ONE, GL_ZERO);
	glDisable_fp (GL_DEPTH_TEST);
	glDepthMask_fp (GL_TRUE);
	glDepthFunc_fp (GL_LESS);
	glDepthRange_fp (0.0, 1.0);
	glDisable_fp (GL_CULL_FACE);
	glCullFace_fp (GL_BACK);
	glDisable_fp (GL_POLYGON_OFFSET_FILL);
	glPolygonOffset_fp (0.0f, 0.0f);
	glDisable_fp (GL_STENCIL_TEST);
	glStencilFunc_fp (GL_ALWAYS, 0, ~0u);
	glStencilOp_fp (GL_KEEP, GL_KEEP, GL_KEEP);
	glColorMask_fp (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable_fp (GL_SAMPLE_ALPHA_TO_COVERAGE);
	glUseProgram_fp (0);

	rp.blend = false;
	for (i = 0; i < RP_MAX_BLEND_TARGETS; i++)
	{
		rp.blend_src[i] = GL_ONE;
		rp.blend_dst[i] = GL_ZERO;
	}
	rp.depth_test = false;
	rp.depth_mask = true;
	rp.depth_func = GL_LESS;
	rp.depth_near = 0.0;
	rp.depth_far = 1.0;
	rp.cull = false;
	rp.cull_face = GL_BACK;
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
