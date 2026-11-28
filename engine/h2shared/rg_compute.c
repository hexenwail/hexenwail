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
 * return to SAMPLER for the graphics stages alone.  A sampler its unit hands
 * a placeholder needs none of that: the placeholders were written at boot
 * (P5c, the world cull's Hi-Z pyramid while Hi-Z is off).
 *
 * Read-only storage textures (#340 P5f).  A stage that reads a texture with
 * texelFetch through a texture2D and no sampler (a Vulkan arm of its own;
 * GL's program keeps its sampler) gets it bound with
 * SDL_BindGPUComputeStorageTextures, which moves it to its read layout with
 * a barrier whose second scope is the compute stage: ordered after what
 * wrote it, with no wait.  That is how the Hi-Z reduce reads the level below
 * and the cull reads the pyramid, which took 11 waits a frame before.  The
 * texture needs COMPUTE_STORAGE_READ usage, declared before it is made
 * (RG_Res_StorageRead); a level the stage also writes goes through a
 * scratch, as one a sampler reads does, since the bind moves every level.
 *
 * Sampled and written.  A stage that samples a texture and writes one of its
 * levels as a storage image (the Hi-Z reduce: mip N-1 in, mip N out, #340
 * P5d) has the written level in the storage layout while the sampler's view,
 * which takes in every level, is declared read-only: Vulkan faults the
 * dispatch (VUID-vkCmdDispatch-imageLayout-00344), whatever level the shader
 * actually reads.  So that image is written through a scratch texture of the
 * level's size (RG_Res_StorageScratch), copied into the level in a copy pass
 * right after.  GL binds the level itself, and is unchanged.
 *
 * Dropped.  A dispatch that cannot be made is counted, and the frame goes on;
 * RG_ComputeDropped lets a caller that draws from the pass's output see it
 * went missing (the world cull, P5c), and renderer_status shows the frame's.
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

static unsigned long	rgc_dispatches, rgc_dropped, rgc_barriers, rgc_cycled, rgc_scratched;

/* A storage image the dispatch writes through a scratch texture: where the
 * scratch is copied back to once the pass ends (RG_Res_StorageScratch). */
typedef struct {
	SDL_GPUTextureLocation	scratch, home;
	Uint32			size[3];
	qboolean		preload;
} rgcscratch_t;

/* Whether storage image `im` holds a texture one of the stage's samplers
 * reads: the reduce of a mip chain, sampling the levels it reads from and
 * writing the next one (the Hi-Z pyramid, #340 P5d). */
static qboolean RGC_ImageSampled (const rgstage_t *st, const rgimage_t *im)
{
	int	i;

	for (i = 0; i < st->num_samplers; i++)
	{
		if (RG_Res_ImageSampled (im->binding, st->samplers[i].unit, (rg_textype_t) st->samplers[i].type))
			return true;
	}
	/* or as a read-only storage texture: SDL moves the whole texture to
	 * that layout when it is bound, the written level with it (P5f) */
	for (i = 0; i < st->num_rotex; i++)
	{
		if (RG_Res_ImageSampled (im->binding, st->rotex[i].unit, (rg_textype_t) st->rotex[i].type))
			return true;
	}
	return false;
}

/* The bindings of one dispatch; false when one cannot be made (counted by
 * whoever refused), and the dispatch is dropped. */
static qboolean RGC_Bind (const rgstage_t *st, const Uint32 cover[3],
			  SDL_GPUStorageTextureReadWriteBinding *img,
			  SDL_GPUStorageBufferReadWriteBinding *rw,
			  SDL_GPUTextureSamplerBinding *smp, SDL_GPUBuffer **ro,
			  SDL_GPUTexture **rot, rgcscratch_t *sc, int *nsc)
{
	int	i;
	void	*read;

	*nsc = 0;
	RG_Res_ScratchBegin ();
	for (i = 0; i < st->num_images; i++)
	{
		const rgimage_t	*im = &st->images[i];
		SDL_GPUStorageTextureReadWriteBinding *b = &img[im->binding];

		if (!RG_Res_StorageImage (im->binding, (rg_textype_t) im->type, cover, b))
			return false;
		if (!RGC_ImageSampled (st, im))
			continue;
		if (!RG_Res_StorageScratch (im->binding, cover, b, &sc[*nsc].home, sc[*nsc].size,
					    &sc[*nsc].preload))
			return false;
		SDL_zero (sc[*nsc].scratch);
		sc[*nsc].scratch.texture = b->texture;
		(*nsc)++;
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
	for (i = 0; i < st->num_rotex; i++)
	{
		const rgsampler_t *s = &st->rotex[i];

		if (s->unit == RG_UNIT_NONE)
			RG_NoteUnhandled ("read-only storage texture with no GL unit mapping (placeholder bound)");
		if (!RG_Res_StorageRead (s->unit, (rg_textype_t) s->type, (rg_sampclass_t) s->sclass,
					 &rot[s->binding]))
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
	SDL_GPUTexture				*rot[RG_MAX_STAGE_SAMPLERS];
	rgcscratch_t				sc[RG_MAX_STAGE_IMAGES];
	SDL_GPUCommandBuffer			*cmd;
	SDL_GPUComputePass			*pass;
	SDL_GPUCopyPass				*copy;
	Uint32					cover[3];
	qboolean				target = false, samples = false;
	int					i, nsc;

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
	/* Only a texture of the engine's can have been written in this command
	 * buffer; a placeholder was written at boot (#340 P5c: the world cull
	 * declares the Hi-Z pyramid, and samples it only with Hi-Z on). */
	for (i = 0; i < st->num_samplers; i++)
	{
		samples |= RG_Res_UnitHolds (st->samplers[i].unit, (rg_textype_t) st->samplers[i].type);
		/* the scene's depth, read where its framebuffer is still bound
		 * (the Hi-Z copy at the post chain's end): a clear pending on
		 * it lands first (#340 P5d) */
		target |= RG_Res_UnitIsTarget (st->samplers[i].unit, (rg_textype_t) st->samplers[i].type);
	}
	/* A read-only storage texture is no sample: SDL's barrier into its
	 * layout has the compute stage as its second scope (#340 P5f). */
	for (i = 0; i < st->num_rotex; i++)
		target |= RG_Res_UnitIsTarget (st->rotex[i].unit, (rg_textype_t) st->rotex[i].type);
	cmd = RG_Frame_ComputeBegin (target, samples);
	if (!cmd)
	{
		rgc_dropped++;
		return;
	}
	if (!RGC_Bind (st, cover, img, rw, smp, ro, rot, sc, &nsc))
	{
		rgc_dropped++;
		RG_Frame_ComputeEnd (false);
		return;
	}
	for (i = 0; i < nsc; i++)
	{
		if (!sc[i].preload)
			continue;
		copy = SDL_BeginGPUCopyPass (cmd);
		RG_Frame_CountCopyPass ();
		if (!copy)
		{
			rgc_dropped++;
			RG_Frame_ComputeEnd (false);
			return;
		}
		SDL_CopyGPUTextureToTexture (copy, &sc[i].home, &sc[i].scratch, sc[i].size[0], sc[i].size[1],
					     sc[i].size[2], false);
		SDL_EndGPUCopyPass (copy);
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
	if (st->num_rotex)
		SDL_BindGPUComputeStorageTextures (pass, 0, rot, (Uint32) st->num_rotex);
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
	if (nsc)
	{
		/* The level lands where GL would have stored it.  A copy pass
		 * that cannot begin leaves it as it was: counted, as a drop. */
		copy = SDL_BeginGPUCopyPass (cmd);
		RG_Frame_CountCopyPass ();
		if (!copy)
			rgc_dropped++;
		for (i = 0; copy && i < nsc; i++)
			SDL_CopyGPUTextureToTexture (copy, &sc[i].scratch, &sc[i].home, sc[i].size[0],
						     sc[i].size[1], sc[i].size[2], false);
		if (copy)
		{
			SDL_EndGPUCopyPass (copy);
			rgc_scratched += nsc;
		}
	}
	for (i = 0; i < st->num_images; i++)
	{
		rgc_cycled += img[i].cycle;
		RG_Res_StorageImageWritten (st->images[i].binding);
	}
	rgc_dispatches++;
	RG_Frame_ComputeEnd (true);
}

unsigned long RG_ComputeDropped (void)
{
	return rgc_dropped;
}

void RG_MemoryBarrier (GLbitfield barriers)
{
	/* Met by the pass boundary every dispatch already has: see the top. */
	(void) barriers;
	rgc_barriers++;
}

void RG_Compute_Status (void)
{
	Con_Printf ("[RENDERER] SDL_GPU compute since boot: %lu dispatches (%lu images cycled, %lu "
		    "written through a scratch), %lu dropped, %lu barriers met by pass boundaries\n",
		    rgc_dispatches, rgc_cycled, rgc_scratched, rgc_dropped, rgc_barriers);
}
