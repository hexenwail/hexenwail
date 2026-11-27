/* rg_compute.c -- SDL_GPU backend: compute dispatches (#340 P5a-2)
 *
 * GL runs a dispatch where it is issued, against the program R_UseProgram
 * bound, the image units glBindImageTexture set, the textures on their units
 * and the storage buffers at their binding points, and orders it against what
 * follows with glMemoryBarrier.  SDL_GPU wants a compute pass, and fixes its
 * read-write resources when the pass begins.  So each dispatch is a pass of
 * its own:
 *
 *   1. what came before lands first (RG_Frame_ComputeBegin): the open
 *      segment's draws in their render pass, or, when it has none, a copy
 *      pass of the queued uploads alone, a pending clear kept for the next
 *      render pass;
 *   2. the pass begins with the stage's set 1: storage image N from image
 *      unit N (IMGBIND takes one number for GL and Vulkan), each read-write
 *      buffer from the GL binding point its block names
 *      (RG_ComputeStoragePoint);
 *   3. set 0 is bound as a draw's is: each sampler its unit's texture or a
 *      placeholder (rg_resource.c), each read-only buffer its point's or the
 *      zeroed stand-in; set 2 is each uniform block's flushed mirror;
 *   4. the dispatch, and the pass ends.
 *
 * Passes are ordered and SDL inserts the barriers between them, so every
 * glMemoryBarrier is already met and RG_MemoryBarrier only counts.  Two
 * dispatches in one pass would race; a later phase that wants them together
 * has to prove they share no resource (P5f).
 *
 * Cycling.  A read-write buffer never cycles: the world cull's indirect
 * buffer keeps the first index written at creation, its dedup buffer the
 * frame count, and cycling would hand the pass a buffer without them.  An
 * image cycles only when the dispatch overwrites all of it (RG_Res_
 * StorageImage), which spares the pass waiting on a frame still sampling it.
 * That rule takes a write-only image whose dispatch covers every texel to
 * store every texel: a pass that covers the texture but stores only some of
 * it, keeping the rest, must bind it GL_READ_WRITE (P5b on, the contract).
 *
 * Sampling.  A dispatch that samples a texture first submits what is
 * recorded and waits (RG_Frame_ComputeBegin): SDL 3.4.12 syncs a texture's
 * return to SAMPLER for the graphics stages alone.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef USE_SDL_GPU
#error rg_compute.c is the SDL_GPU backend; CMake compiles it only with USE_SDL_GPU
#endif

#include "quakedef.h"
#include "rg_internal.h"

static unsigned long	rgc_dispatches, rgc_dropped, rgc_barriers, rgc_cycled;

/* The bindings of one dispatch; false when one cannot be made (counted by
 * whoever refused), and the dispatch is dropped. */
static qboolean RGC_Bind (const rgstage_t *st, const Uint32 cover[3],
			  SDL_GPUStorageTextureReadWriteBinding *img,
			  SDL_GPUStorageBufferReadWriteBinding *rw,
			  SDL_GPUTextureSamplerBinding *smp, SDL_GPUBuffer **ro)
{
	int	i;
	void	*read;

	for (i = 0; i < st->num_images; i++)
	{
		const rgimage_t	*im = &st->images[i];

		if (!RG_Res_StorageImage (im->binding, (rg_textype_t) im->type, cover, &img[im->binding]))
			return false;
	}
	/* RG_Res_StorageImageWritten once the dispatch is recorded */
	for (i = 0; i < st->num_rwssbos; i++)
	{
		const rgssbo_t	*sb = &st->rwssbos[i];
		SDL_GPUBuffer	*buf = RG_Buf_Storage (sb->point, false, &read);

		if (!buf)
			return false;
		SDL_zero (rw[sb->slot]);
		rw[sb->slot].buffer = buf;
		rw[sb->slot].cycle = false;
	}
	for (i = 0; i < st->num_samplers; i++)
	{
		const rgsampler_t *s = &st->samplers[i];

		if (s->ms)
		{
			RG_NoteUnhandled ("multisample sampler in a compute stage (dispatch dropped)");
			return false;
		}
		if (s->unit == RG_UNIT_NONE)
			RG_NoteUnhandled ("sampler with no GL unit mapping (bound to a placeholder)");
		if (!RG_Res_ForDraw (s->unit, (rg_textype_t) s->type, (rg_sampclass_t) s->sclass,
				     s->shadow, &smp[s->binding]))
			return false;
	}
	for (i = 0; i < st->num_ssbos; i++)
	{
		const rgssbo_t	*sb = &st->ssbos[i];
		SDL_GPUBuffer	*buf = RG_Buf_Storage (sb->point, false, &read);

		ro[sb->slot] = buf ? buf : RG_Frame_Standin ();
	}
	return true;
}

void RG_DispatchCompute (GLuint groups_x, GLuint groups_y, GLuint groups_z)
{
	const rpipestate_t			*ps = R_PipelineState ();
	const rgprog_t				*prog = RG_Program (ps->program);
	const rprog_refl_t			*refl;
	const rgstage_t				*st;
	SDL_GPUStorageTextureReadWriteBinding	img[RG_MAX_STAGE_IMAGES];
	SDL_GPUStorageBufferReadWriteBinding	rw[RG_MAX_STAGE_SSBOS];
	SDL_GPUTextureSamplerBinding		smp[RG_MAX_STAGE_SAMPLERS];
	SDL_GPUBuffer				*ro[RG_MAX_STAGE_SSBOS];
	SDL_GPUCommandBuffer			*cmd;
	SDL_GPUComputePass			*pass;
	Uint32					cover[3];
	qboolean				target = false;
	int					i;

	if (!prog || !prog->compute || !prog->cs)
	{
		RG_NoteUnhandled ("dispatch with no SDL_GPU compute program bound (dropped)");
		rgc_dropped++;
		return;
	}
	if (!groups_x || !groups_y || !groups_z)
		return;
	refl = R_Program_Reflection (prog->id, RPROG_STAGE_COMPUTE);
	st = &prog->stage[RPROG_STAGE_COMPUTE];
	if (!refl)
	{
		rgc_dropped++;
		return;
	}
	cover[0] = groups_x * refl->threads[0];
	cover[1] = groups_y * refl->threads[1];
	cover[2] = groups_z * refl->threads[2];

	for (i = 0; i < st->num_images; i++)
		target |= RG_Res_ImageIsTarget (st->images[i].binding);
	cmd = RG_Frame_ComputeBegin (target, st->num_samplers > 0);
	if (!cmd)
	{
		rgc_dropped++;
		return;
	}
	if (!RGC_Bind (st, cover, img, rw, smp, ro))
	{
		rgc_dropped++;
		RG_Frame_ComputeEnd (false);
		return;
	}

	pass = SDL_BeginGPUComputePass (cmd, img, (Uint32) st->num_images, rw, (Uint32) st->num_rwssbos);
	if (!pass)
	{
		Con_DPrintf ("SDL_GPU: no compute pass (%s)\n", SDL_GetError ());
		rgc_dropped++;
		RG_Frame_ComputeEnd (false);
		return;
	}
	SDL_BindGPUComputePipeline (pass, prog->cs);
	if (st->num_samplers)
		SDL_BindGPUComputeSamplers (pass, 0, smp, (Uint32) st->num_samplers);
	if (st->num_ssbos)
		SDL_BindGPUComputeStorageBuffers (pass, 0, ro, (Uint32) st->num_ssbos);
	for (i = 0; i < st->num_ubos; i++)
	{
		rgubo_t		*u = (rgubo_t *) &st->ubos[i];
		const void	*data;
		int		size = 0;

		if (u->block == -2)
		{
			u->block = RG_Frame_BlockIndex (u->name);
			if (u->block < 0)
				RG_NoteUnhandled ("uniform block no mirror is registered for (left unpushed)");
		}
		data = RG_Frame_BlockFlushed (u->block, &size);
		if (data)
			SDL_PushGPUComputeUniformData (cmd, u->binding, data, (Uint32) size);
	}
	SDL_DispatchGPUCompute (pass, groups_x, groups_y, groups_z);
	SDL_EndGPUComputePass (pass);
	for (i = 0; i < st->num_images; i++)
	{
		rgc_cycled += img[i].cycle;
		RG_Res_StorageImageWritten (st->images[i].binding);
	}
	rgc_dispatches++;
	RG_Frame_ComputeEnd (true);
}

void RG_MemoryBarrier (GLbitfield barriers)
{
	/* Met by the pass boundary every dispatch already has: see the top. */
	(void) barriers;
	rgc_barriers++;
}

void RG_Compute_Status (void)
{
	Con_Printf ("[RENDERER] SDL_GPU compute since boot: %lu dispatches (%lu images cycled), "
		    "%lu dropped, %lu barriers met by pass boundaries\n",
		    rgc_dispatches, rgc_cycled, rgc_dropped, rgc_barriers);
}
