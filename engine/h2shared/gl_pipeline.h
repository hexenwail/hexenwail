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
 * and nowhere else.  Two exceptions, both harmless: one-time context setup in
 * gl_vidsdl.c for state this module does not track (multisample, debug
 * output), and three #if 0 fragments of the old fixed-function zfix hack
 * (gl_vidsdl.c GL_Init, gl_rsurf.c R_DrawBrushModel) which do not compile and
 * so cannot desync anything.
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
void	R_SetStencilTest (qboolean enable);
void	R_SetStencilFunc (GLenum func, GLint ref, GLuint mask);
void	R_SetStencilOp (GLenum sfail, GLenum dpfail, GLenum dppass);
void	R_SetColorMask (qboolean r, qboolean g, qboolean b, qboolean a);
void	R_SetAlphaToCoverage (qboolean enable);
void	R_UseProgram (GLuint program);

/* Deleting a program invalidates the bound-program shadow: GL reuses names, so
 * a freshly created program can land on the number the shadow still believes
 * is current and the next R_UseProgram would fold itself away.  Call this from
 * anything that destroys programs that may have been bound. */
void	R_PipelineForgetProgram (void);

/* Shadow readback, for the handful of save-and-restore sites.  Exact by
 * construction and free, unlike the glIsEnabled round trip it replaces. */
qboolean R_GetCull (void);

/* Draw entry points.  Every one flushes the std140 uniform blocks first, which
 * is the only reason no draw path has to remember to -- a missed flush would
 * render a batch with the previous batch's matrices, which is exactly the kind
 * of bug that shows up on one map and not the next.  They are also the seam a
 * second backend needs: SDL_GPU records draws into a command buffer rather
 * than issuing them, so the call sites must not name glDraw* directly.
 * uhexen2-p4ln.2. */
void	R_DrawArrays (GLenum mode, GLint first, GLsizei count);
void	R_DrawElements (GLenum mode, GLsizei count, GLenum type,
			const void *indices);
void	R_DrawElementsInstanced (GLenum mode, GLsizei count, GLenum type,
				 const void *indices, GLsizei primcount);
void	R_DrawElementsIndirect (GLenum mode, GLenum type, const void *indirect);
void	R_MultiDrawElementsIndirect (GLenum mode, GLenum type,
				     const void *indirect, GLsizei drawcount,
				     GLsizei stride);

#endif	/* GL_PIPELINE_H */
