/* gl_pipeline.h -- render pipeline state objects and the GL state shadow
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

/* Every draw-affecting GL state change goes through this module.  Two things
 * come out of that:
 *
 *   1. Redundant calls are dropped.  The engine issues its state as scattered
 *      set-then-draw clusters, so the same glDisable(GL_BLEND) /
 *      glDepthMask(1) pair is re-asserted dozens of times per frame; the
 *      shadow below turns those into nothing.
 *
 *   2. There is one place that knows the whole draw state, which is what a
 *      second backend needs -- Vulkan/D3D12/Metal bake this into an immutable
 *      pipeline object at creation time and cannot accept it as loose
 *      per-draw toggles.  rpipeline_t is that bundle, expressed in GL terms
 *      for now.
 *
 * The shadow is only correct as long as nothing writes these states behind
 * its back, so the raw glEnable/glDepthMask/glBlendFunc/... calls belong here
 * and nowhere else.  Scissor and sample shading are not shadowed (scissor is
 * dynamic command-buffer state in SDL_GPU, and SDL_GPU has no sample-shading
 * API at all), but their calls still live here, as the GL_Set* entry points.
 * Exceptions, all deliberate: context-wide switches set at context or
 * allocation time and never per draw -- debug output, multisample and clip
 * control in gl_vidsdl.c GL_Init, seamless cube sampling when gl_shadow.c
 * allocates its maps; viewport and clear values, which are not draw state; and the web_canvas_gl2.c presentation canvas (a separate
 * renderer path, raw GLES by design).  tools/test_draw_state_routing.py holds
 * that list and fails on any other draw-state call outside this module.
 * GitHub #337.
 */

#ifndef GL_PIPELINE_H
#define GL_PIPELINE_H

/* Draw buffers whose blend function is tracked independently.  Only the WBOIT
 * pass uses more than one (0 = accum, 1 = revealage). */
#define RP_MAX_BLEND_TARGETS	2

/* Immutable draw-state bundle.  A field set to RP_KEEP_ENUM is not touched by
 * R_BindPipeline, which is how the reversed-Z depth compare stays expressible:
 * it is GL_GEQUAL or GL_LEQUAL depending on gl_clipcontrol_able, so it cannot
 * be baked into a static table. */
#define RP_KEEP_ENUM	0

/* Shadow sentinel for "this value is not known".  Cannot be 0: GL_ZERO is 0
 * and is a real blend factor (the WBOIT revealage buffer uses it). */
#define RP_ENUM_UNKNOWN	0xFFFFFFFFu

typedef struct rpipeline_s {
	GLuint		program;
	qboolean	blend;
	GLenum		blend_src;
	GLenum		blend_dst;
	qboolean	depth_test;
	qboolean	depth_write;
	GLenum		depth_func;		/* RP_KEEP_ENUM to leave alone */
	qboolean	cull;
	GLenum		cull_face;		/* RP_KEEP_ENUM to leave alone */
	GLenum		polygon_mode;		/* RP_KEEP_ENUM to leave alone */
	qboolean	alpha_to_coverage;
} rpipeline_t;

/* Apply a whole bundle.  No draw site uses this yet: converting one means
 * deciding its depth-write value explicitly, and the depth mask is the one
 * piece of state the scattered clusters leave implicit, so each conversion
 * needs a look at the real frame rather than a mechanical rewrite.  The type
 * and the entry point land here because the SDL_GPU backend and the descriptor
 * work are written against them. */
void	R_BindPipeline (const rpipeline_t *p);

/* Force GL to a known state and seed the shadow from it.  Called once per GL
 * context, i.e. at startup and after every vid_restart. */
void	R_PipelineResetState (void);

/* Individual state.  Each is a no-op when the shadow already agrees. */
void	R_SetBlend (qboolean enable);
void	R_SetBlendFunc (GLenum src, GLenum dst);
void	R_SetBlendFuncIndexed (GLuint buf, GLenum src, GLenum dst);
void	R_SetDepthTest (qboolean enable);
void	R_SetDepthMask (qboolean enable);
void	R_SetDepthFunc (GLenum func);
void	R_SetDepthRange (double znear, double zfar);
void	R_SetCull (qboolean enable);
void	R_SetCullFace (GLenum face);
void	R_SetPolygonOffsetFill (qboolean enable, float factor, float units);
void	R_SetPolygonOffsetLine (qboolean enable, float factor, float units);
void	R_SetPolygonMode (GLenum mode);
void	R_SetStencilTest (qboolean enable);
void	R_SetStencilFunc (GLenum func, GLint ref, GLuint mask);
void	R_SetStencilOp (GLenum sfail, GLenum dpfail, GLenum dppass);
void	R_SetColorMask (qboolean r, qboolean g, qboolean b, qboolean a);
void	R_SetAlphaToCoverage (qboolean enable);

/* Point size, scissor and sample shading.  Unshadowed (see gl_pipeline.c):
 * every caller puts back what it changed -- set/reset pairs, or for
 * gl_shadow.c's passes reset then restore the saved scissor.  #453, #337. */
void	GL_SetPointSize (float size);
void	GL_SetScissor (int x, int y, int width, int height);
void	GL_ResetScissor (void);
void	GL_SetSampleShading (float frac);	/* <= 0 turns it off */
void	R_UseProgram (GLuint program);

/* Deleting a program invalidates the bound-program shadow: GL reuses names, so
 * a freshly created program can land on the number the shadow still believes
 * is current and the next R_UseProgram would fold itself away.  Call this from
 * anything that destroys programs that may have been bound. */
void	R_PipelineForgetProgram (void);

/* Shadow readback, for the handful of save-and-restore sites.  Exact by
 * construction and free, unlike the glIsEnabled round trip it replaces. */
qboolean R_GetCull (void);
qboolean R_GetBlend (void);
qboolean R_GetDepthMask (void);
void	R_GetDepthRange (double *znear, double *zfar);
/* For gl_shadow.c, which draws its maps in the middle of R_SetupGL and puts
 * back everything it changed (#97).  The two enums may be RP_ENUM_UNKNOWN. */
qboolean R_GetDepthTest (void);
GLenum	R_GetDepthFunc (void);
GLenum	R_GetCullFace (void);
qboolean R_GetStencilTest (void);
qboolean R_GetPolygonOffsetFill (float *factor, float *units);

#endif	/* GL_PIPELINE_H */
