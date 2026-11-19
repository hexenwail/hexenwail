/* rg_pipeline.c -- SDL_GPU backend: graphics pipeline cache (#340 P1c)
 *
 * SDL_GPU bakes the draw state into an immutable pipeline object; the
 * engine sets it piece by piece through gl_pipeline.c, whose shadow
 * (R_PipelineState) is therefore the whole of it.  Each draw looks up the
 * pipeline for (shadow, program, primitive, vertex layout, target formats)
 * and creates it on first use.  SDL_GPU has no pipeline cache of its own; a
 * first-use hitch is acceptable for the 2D set, and pre-warming is decision 7
 * on #340.
 *
 * Mapping notes:
 *   - front face.  Every Vulkan vertex stage negates gl_Position.y
 *     (VK_FLIP_Y, layout.inc), which reverses winding: GL's counter-clockwise
 *     front is SDL_GPU's clockwise.  GL's glFrontFace is never changed from
 *     GL_CCW in this engine.
 *   - GL_FRONT_AND_BACK culling draws nothing in GL and has no SDL_GPU mode,
 *     so it returns NULL and the draw is dropped, as GL drops it.
 *   - blend.  The effective funcs (blend_eff_*), never the shadow's
 *     RP_ENUM_UNKNOWN after R_SetBlend(true): that sentinel models a driver
 *     quirk for the redundancy filter, not the state a draw sees.
 *   - depth clip.  GL clips to [-1,1] NDC depth and SDL_GPU to [0,1]; with
 *     clipping off SDL_GPU clamps instead, which keeps the 2D canvas's z = 0
 *     (GL_Ortho -99999..99999) inside.  Fixing 3D depth to [0,1] is P2.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef USE_SDL_GPU
#error rg_pipeline.c is the SDL_GPU backend; CMake compiles it only with USE_SDL_GPU
#endif

#include "quakedef.h"
#include "rg_internal.h"
#include "gl_shader.h"		/* ATTR_* */

/* Everything a pipeline object is made from, zero-padded so the key can be
 * hashed and compared as bytes. */
typedef struct {
	GLuint		program;
	Uint32		prim, layout, color_fmt, depth_fmt;
	Uint32		blend, blend_src, blend_dst;
	Uint32		depth_test, depth_write, depth_func;
	Uint32		cull_face;		/* 0 = no culling */
	Uint32		fill;
	Uint32		color_mask;
	Uint32		poly_offset;
	float		poly_factor, poly_units;
	Uint32		stencil, stencil_func, stencil_mask, sfail, dpfail, dppass;
} rgpipekey_t;

typedef struct rgpipe_s {
	struct rgpipe_s			*next;
	rgpipekey_t			key;
	SDL_GPUGraphicsPipeline		*pipe;
} rgpipe_t;

#define RG_PIPE_BUCKETS	256
static rgpipe_t		*rg_pipes[RG_PIPE_BUCKETS];
static int		rg_numpipes;

static Uint32 RG_KeyHash (const rgpipekey_t *k)
{
	const Uint8	*b = (const Uint8 *) k;
	Uint32		h = 2166136261u;
	size_t		i;

	for (i = 0; i < sizeof(*k); i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

static SDL_GPUBlendFactor RG_BlendFactor (GLenum f)
{
	switch (f)
	{
	case GL_ZERO:			return SDL_GPU_BLENDFACTOR_ZERO;
	case GL_ONE:			return SDL_GPU_BLENDFACTOR_ONE;
	case GL_SRC_COLOR:		return SDL_GPU_BLENDFACTOR_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR:	return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
	case GL_DST_COLOR:		return SDL_GPU_BLENDFACTOR_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR:	return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA:		return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA:	return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA:		return SDL_GPU_BLENDFACTOR_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA:	return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
	case GL_CONSTANT_COLOR:		return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
	case GL_ONE_MINUS_CONSTANT_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
	case GL_SRC_ALPHA_SATURATE:	return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
	}
	return SDL_GPU_BLENDFACTOR_INVALID;
}

static SDL_GPUCompareOp RG_CompareOp (GLenum f)
{
	switch (f)
	{
	case GL_NEVER:		return SDL_GPU_COMPAREOP_NEVER;
	case GL_LESS:		return SDL_GPU_COMPAREOP_LESS;
	case GL_EQUAL:		return SDL_GPU_COMPAREOP_EQUAL;
	case GL_LEQUAL:		return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	case GL_GREATER:	return SDL_GPU_COMPAREOP_GREATER;
	case GL_NOTEQUAL:	return SDL_GPU_COMPAREOP_NOT_EQUAL;
	case GL_GEQUAL:		return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
	case GL_ALWAYS:		return SDL_GPU_COMPAREOP_ALWAYS;
	}
	return SDL_GPU_COMPAREOP_INVALID;
}

static SDL_GPUStencilOp RG_StencilOp (GLenum op)
{
	switch (op)
	{
	case GL_KEEP:		return SDL_GPU_STENCILOP_KEEP;
	case GL_ZERO:		return SDL_GPU_STENCILOP_ZERO;
	case GL_REPLACE:	return SDL_GPU_STENCILOP_REPLACE;
	case GL_INCR:		return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
	case GL_DECR:		return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
	case GL_INVERT:		return SDL_GPU_STENCILOP_INVERT;
	case GL_INCR_WRAP:	return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
	case GL_DECR_WRAP:	return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
	}
	return SDL_GPU_STENCILOP_INVALID;
}

/* The key for the current state; false for a state SDL_GPU cannot draw. */
static qboolean RG_MakeKey (rgpipekey_t *k, const rgprog_t *prog, SDL_GPUPrimitiveType prim,
			    rg_vlayout_t layout, SDL_GPUTextureFormat color,
			    SDL_GPUTextureFormat depth)
{
	const rpipestate_t	*s = R_PipelineState ();

	memset (k, 0, sizeof(*k));
	k->program = prog->name;
	k->prim = prim;
	k->layout = layout;
	k->color_fmt = color;
	k->depth_fmt = depth;
	if (s->blend)
	{
		k->blend = 1;
		k->blend_src = s->blend_eff_src[0];
		k->blend_dst = s->blend_eff_dst[0];
	}
	k->depth_test = s->depth_test;
	k->depth_write = s->depth_mask;
	k->depth_func = (s->depth_func == RP_ENUM_UNKNOWN) ? GL_LESS : s->depth_func;
	if (s->cull)
	{
		if (s->cull_face == GL_FRONT_AND_BACK)
			return false;
		k->cull_face = (s->cull_face == GL_FRONT) ? GL_FRONT : GL_BACK;
	}
	k->fill = (s->polygon_mode == GL_LINE) ? 1 : 0;
	if (s->polygon_mode == GL_POINT)
	{
		RG_NoteUnhandled ("glPolygonMode GL_POINT (drawn filled)");
		k->fill = 0;
	}
	k->color_mask = (s->color_mask[0] ? 1 : 0) | (s->color_mask[1] ? 2 : 0) |
			(s->color_mask[2] ? 4 : 0) | (s->color_mask[3] ? 8 : 0);
	if (s->poly_offset_fill || (s->poly_offset_line && k->fill))
	{
		k->poly_offset = 1;
		k->poly_factor = s->poly_factor;
		k->poly_units = s->poly_units;
	}
	if (s->stencil_test)
	{
		k->stencil = 1;
		k->stencil_func = s->stencil_func;
		k->stencil_mask = s->stencil_mask & 0xFF;
		k->sfail = s->stencil_sfail;
		k->dpfail = s->stencil_dpfail;
		k->dppass = s->stencil_dppass;
	}
	return true;
}

static SDL_GPUGraphicsPipeline *RG_CreatePipeline (const rgpipekey_t *k, const rgprog_t *prog)
{
	SDL_GPUGraphicsPipelineCreateInfo	ci;
	SDL_GPUColorTargetDescription		ct;
	SDL_GPUVertexBufferDescription		vb;
	SDL_GPUVertexAttribute			va[4];
	SDL_GPUGraphicsPipeline			*pipe;

	SDL_zero (ci);
	SDL_zero (ct);
	SDL_zero (vb);
	ci.vertex_shader = prog->vs;
	ci.fragment_shader = prog->fs;

	/* RG_VL_IMM: immvert_t.  Every attribute is supplied whether or not the
	 * stage reads it, as the GL VAO enables all four. */
	vb.slot = 0;
	vb.pitch = 11 * sizeof(float);
	vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	va[0].location = ATTR_POSITION; va[0].buffer_slot = 0;
	va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; va[0].offset = 0;
	va[1].location = ATTR_TEXCOORD; va[1].buffer_slot = 0;
	va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; va[1].offset = 3 * sizeof(float);
	va[2].location = ATTR_LMCOORD; va[2].buffer_slot = 0;
	va[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; va[2].offset = 5 * sizeof(float);
	va[3].location = ATTR_COLOR; va[3].buffer_slot = 0;
	va[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[3].offset = 7 * sizeof(float);
	ci.vertex_input_state.vertex_buffer_descriptions = &vb;
	ci.vertex_input_state.num_vertex_buffers = 1;
	ci.vertex_input_state.vertex_attributes = va;
	ci.vertex_input_state.num_vertex_attributes = 4;

	ci.primitive_type = (SDL_GPUPrimitiveType) k->prim;

	ci.rasterizer_state.fill_mode = k->fill ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
	ci.rasterizer_state.cull_mode = (k->cull_face == GL_FRONT) ? SDL_GPU_CULLMODE_FRONT :
					(k->cull_face == GL_BACK) ? SDL_GPU_CULLMODE_BACK :
					SDL_GPU_CULLMODE_NONE;
	ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;	/* GL_CCW, Y-flipped */
	if (k->poly_offset)
	{
		ci.rasterizer_state.enable_depth_bias = true;
		ci.rasterizer_state.depth_bias_constant_factor = k->poly_units;
		ci.rasterizer_state.depth_bias_slope_factor = k->poly_factor;
	}
	ci.rasterizer_state.enable_depth_clip = false;

	ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

	ci.depth_stencil_state.enable_depth_test = k->depth_test ? true : false;
	ci.depth_stencil_state.enable_depth_write = (k->depth_test && k->depth_write) ? true : false;
	ci.depth_stencil_state.compare_op = k->depth_test ? RG_CompareOp (k->depth_func)
							  : SDL_GPU_COMPAREOP_ALWAYS;
	if (k->stencil)
	{
		SDL_GPUStencilOpState	st;

		st.compare_op = RG_CompareOp (k->stencil_func);
		st.fail_op = RG_StencilOp (k->sfail);
		st.depth_fail_op = RG_StencilOp (k->dpfail);
		st.pass_op = RG_StencilOp (k->dppass);
		ci.depth_stencil_state.front_stencil_state = st;
		ci.depth_stencil_state.back_stencil_state = st;
		ci.depth_stencil_state.enable_stencil_test = true;
		ci.depth_stencil_state.compare_mask = (Uint8) k->stencil_mask;
		ci.depth_stencil_state.write_mask = 0xFF;
	}

	ct.format = (SDL_GPUTextureFormat) k->color_fmt;
	if (k->blend)
	{
		SDL_GPUBlendFactor	src = RG_BlendFactor (k->blend_src);
		SDL_GPUBlendFactor	dst = RG_BlendFactor (k->blend_dst);

		if (src == SDL_GPU_BLENDFACTOR_INVALID || dst == SDL_GPU_BLENDFACTOR_INVALID)
		{
			RG_NoteUnhandled ("blend factor with no SDL_GPU equivalent");
			return NULL;
		}
		ct.blend_state.enable_blend = true;
		ct.blend_state.src_color_blendfactor = ct.blend_state.src_alpha_blendfactor = src;
		ct.blend_state.dst_color_blendfactor = ct.blend_state.dst_alpha_blendfactor = dst;
		ct.blend_state.color_blend_op = ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	}
	ct.blend_state.enable_color_write_mask = true;
	ct.blend_state.color_write_mask = (SDL_GPUColorComponentFlags)
		(((k->color_mask & 1) ? SDL_GPU_COLORCOMPONENT_R : 0) |
		 ((k->color_mask & 2) ? SDL_GPU_COLORCOMPONENT_G : 0) |
		 ((k->color_mask & 4) ? SDL_GPU_COLORCOMPONENT_B : 0) |
		 ((k->color_mask & 8) ? SDL_GPU_COLORCOMPONENT_A : 0));
	ci.target_info.color_target_descriptions = &ct;
	ci.target_info.num_color_targets = 1;
	if (k->depth_fmt != SDL_GPU_TEXTUREFORMAT_INVALID)
	{
		ci.target_info.has_depth_stencil_target = true;
		ci.target_info.depth_stencil_format = (SDL_GPUTextureFormat) k->depth_fmt;
	}

	pipe = SDL_CreateGPUGraphicsPipeline (rg_dev, &ci);
	if (!pipe)
		Con_Printf ("SDL_GPU: pipeline for program %s failed (%s)\n",
			    R_Program_Name (prog->id), SDL_GetError ());
	return pipe;
}

SDL_GPUGraphicsPipeline *RG_PipelineFor (const rgprog_t *prog, SDL_GPUPrimitiveType prim,
					 rg_vlayout_t layout, SDL_GPUTextureFormat color,
					 SDL_GPUTextureFormat depth)
{
	rgpipekey_t	k;
	rgpipe_t	*p;
	Uint32		b;

	if (!prog || prog->compute)
		return NULL;
	if (!RG_MakeKey (&k, prog, prim, layout, color, depth))
		return NULL;
	b = RG_KeyHash (&k) & (RG_PIPE_BUCKETS - 1);
	for (p = rg_pipes[b]; p; p = p->next)
	{
		if (!memcmp (&p->key, &k, sizeof(k)))
			return p->pipe;
	}
	p = (rgpipe_t *) malloc (sizeof(rgpipe_t));
	if (!p)
		Sys_Error ("SDL_GPU: out of memory for pipelines");
	p->key = k;
	/* A failure is cached too (NULL), so it is reported once, not per draw. */
	p->pipe = RG_CreatePipeline (&k, prog);
	p->next = rg_pipes[b];
	rg_pipes[b] = p;
	rg_numpipes++;
	return p->pipe;
}

int RG_PipelineCount (void)
{
	return rg_numpipes;
}

void RG_Pipeline_Init (void)
{
	memset (rg_pipes, 0, sizeof(rg_pipes));
	rg_numpipes = 0;
}

void RG_Pipeline_Shutdown (void)
{
	int		i;
	rgpipe_t	*p, *next;

	for (i = 0; i < RG_PIPE_BUCKETS; i++)
	{
		for (p = rg_pipes[i]; p; p = next)
		{
			next = p->next;
			if (p->pipe && rg_dev)
				SDL_ReleaseGPUGraphicsPipeline (rg_dev, p->pipe);
			free (p);
		}
		rg_pipes[i] = NULL;
	}
	rg_numpipes = 0;
}
