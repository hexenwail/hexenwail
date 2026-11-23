/* rg_shader.c -- SDL_GPU backend: shaders and programs (#340 P1c)
 *
 * A program is an rprog_id_t (gl_progid.h).  Here it becomes one
 * SDL_GPUShader per stage, created from the stage's offline Vulkan SPIR-V
 * with the resource counts gl_progid_refl.h carries, and a GL-shaped name
 * the renderer binds with R_UseProgram like any other program.
 *
 * WHERE THE SPIR-V COMES FROM (decision 6 on #340).  The blobs are compiled
 * into the binary: the build turns every <blob>.vk.spv into rg_spirv[]
 * (rg_spirv.h, engine/cmake/EmbedSpirv.cmake).  One source serves a whole
 * run, picked once:
 *
 *   1. -gpushaders <dir>, if <dir> has the probe blob: every blob is read
 *      from <dir>/<blob>.vk.spv.  The developer override -- point it at
 *      <build>/engine/generated/spirv after `make shaders_spirv` to try an
 *      edited shader without relinking.  A <dir> without the probe blob is
 *      reported and ignored.
 *   2. the embedded table.
 *
 * Nothing else on disk is looked at.  P1c also searched <exe>/spirv,
 * <basedir>/spirv and the build tree; ahead of the table those would let a
 * stale directory beside an installed binary replace the blobs it was built
 * with -- the drift embedding exists to rule out -- and behind it they could
 * never be reached, since the table always has the probe blob.
 *
 * Each blob is reflected here as it loads: the sampler and uniform-block
 * bindings, with their names, read from the SPIR-V itself.  The sampler and
 * uniform-buffer counts are checked against gl_progid_refl.h, which the
 * binary was built with; an override directory from another build fails that
 * check loudly instead of binding the wrong slots.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef USE_SDL_GPU
#error rg_shader.c is the SDL_GPU backend; CMake compiles it only with USE_SDL_GPU
#endif

#include "quakedef.h"
#include "rg_internal.h"
#include "rg_spirv.h"
#include "gl_texbind.h"		/* RT_MAX_TEXTURE_UNITS */
#include "gl_lightcluster.h"	/* LIGHT_SSBO_BINDING */

/* This run's blob source (see the top): a -gpushaders directory, or the
 * embedded table. */
static char		rg_spvdir[MAX_OSPATH];
static char		rg_spvsource[MAX_OSPATH + 32];
static qboolean		rg_spv_fromdir, rg_spv_found;

/* One allocation per program: a recorded draw holds its rgprog_t * until
 * the segment replays, and a program created mid-frame must not move it. */
static rgprog_t		**rg_progs;
static int		rg_numprogs, rg_maxprogs;

/* ------------------------------------------------------------------ */
/* Finding the blobs                                                   */
/* ------------------------------------------------------------------ */

/* The blob every program set has; its presence marks a usable source. */
#define RG_PROBE_BLOB	"s2d.vert"

static qboolean RG_DirHasBlobs (const char *dir)
{
	char	path[MAX_OSPATH];
	FILE	*f;

	if (!dir || !*dir)
		return false;
	q_snprintf (path, sizeof(path), "%s/%s.vk.spv", dir, RG_PROBE_BLOB);
	f = fopen (path, "rb");
	if (!f)
		return false;
	fclose (f);
	q_strlcpy (rg_spvdir, dir, sizeof(rg_spvdir));
	return true;
}

static const rg_spirv_t *RG_EmbeddedBlob (const char *blob)
{
	int	i;

	for (i = 0; i < rg_spirv_count; i++)
	{
		if (!strcmp (rg_spirv[i].name, blob))
			return &rg_spirv[i];
	}
	return NULL;
}

static void RG_FindShaders (void)
{
	int	i;

	rg_spv_found = rg_spv_fromdir = false;
	i = COM_CheckParm ("-gpushaders");
	if (i && i + 1 < com_argc)
	{
		if (RG_DirHasBlobs (com_argv[i + 1]))
		{
			rg_spv_fromdir = rg_spv_found = true;
			q_snprintf (rg_spvsource, sizeof(rg_spvsource), "-gpushaders %s", rg_spvdir);
			return;
		}
		Con_SafePrintf ("SDL_GPU: -gpushaders %s has no %s.vk.spv; using the embedded blobs\n",
				com_argv[i + 1], RG_PROBE_BLOB);
	}
	if (RG_EmbeddedBlob (RG_PROBE_BLOB))
	{
		rg_spv_found = true;
		q_snprintf (rg_spvsource, sizeof(rg_spvsource), "the embedded table (%d blobs)",
			    rg_spirv_count);
	}
}

const char *RG_ShaderSource (void)
{
	return rg_spv_found ? rg_spvsource : NULL;
}

/* ------------------------------------------------------------------ */
/* Reflection                                                          */
/* ------------------------------------------------------------------ */

enum {
	SPV_OpName = 5,
	SPV_OpTypeInt = 21,
	SPV_OpTypeFloat = 22,
	SPV_OpTypeImage = 25,
	SPV_OpTypeSampledImage = 27,
	SPV_OpTypeStruct = 30,
	SPV_OpTypePointer = 32,
	SPV_OpVariable = 59,
	SPV_OpDecorate = 71,
	SPV_DecBlock = 2,
	SPV_DecBufferBlock = 3,
	SPV_DecBinding = 33,
	SPV_DecDescriptorSet = 34,
	SPV_ScUniformConstant = 0,
	SPV_ScUniform = 2,
	SPV_ScStorageBuffer = 12,
	SPV_Dim2D = 1,
	SPV_Dim3D = 2,
	SPV_DimCube = 3
};

typedef struct {
	Uint32		op;		/* the instruction that defines the id */
	Uint32		a, b, c, d, e;	/* its first operands after the result id */
	Uint32		set, binding;
	qboolean	has_set, has_binding, block, bufblock;
	const char	*name;
} spvid_t;

/* Each storage block's GL binding point, by block name (#340 P2e).  The
 * Vulkan SPIR-V carries only the SDL_GPU slot (VERT_SSBO / FRAG_SSBO's `vk`
 * in layout.inc); the point is the `gl` beside it, which the C side binds
 * with GL_BindBufferBase / Range.  A name keeps one point in every stage
 * that declares it, so the name is the key: tools/test_rg_sources.py holds
 * this table to every storage block in engine/shaders.  The kind is when
 * nothing bound there is expected rather than a routing gap (#520). */
static const struct {
	const char	*block;
	int		point;
	rg_ssbokind_t	kind;
	int		format;		/* RG_SSBO_POSE: the u_poseverttype it is read at */
} rg_storage_points[] = {
	{ "InstanceBuffer",	0, RG_SSBO_NEEDED, -1 },	/* R_DrawAliasInstanced's GL_Upload */
	{ "PoseBuffer",		1, RG_SSBO_POSE, PV_QUAKE1 },	/* gl_mesh.c ssbo_pose */
	{ "ShadeDots",		2, RG_SSBO_NEEDED, -1 },	/* gl_shader.c ubo_shadedots */
	{ "MD3PoseBuffer",	3, RG_SSBO_POSE, PV_MD3 },	/* gl_mesh.c ssbo_pose_md3 */
	{ "BoneBuffer",		4, RG_SSBO_POSE, PV_IQM },	/* gl_mesh.c ssbo_bones */
	{ "GpuParticleBuffer",	0, RG_SSBO_NEEDED, -1 },	/* r_part.c, never drawn */
	{ "LightBuffer",	LIGHT_SSBO_BINDING, RG_SSBO_COMPUTE_FED, -1 },	/* gl_lightcluster.c, compute: P5 */
};

static int RG_StorageRow (const char *block)
{
	int	i;

	for (i = 0; i < (int)(sizeof(rg_storage_points) / sizeof(rg_storage_points[0])); i++)
	{
		if (!strcmp (rg_storage_points[i].block, block))
			return i;
	}
	return -1;
}

int RG_StoragePoint (const char *block)
{
	int	i = RG_StorageRow (block);

	return (i < 0) ? -1 : rg_storage_points[i].point;
}

rg_ssbokind_t RG_StorageKind (const char *block, int *format)
{
	int	i = RG_StorageRow (block);

	*format = (i < 0) ? -1 : rg_storage_points[i].format;
	return (i < 0) ? RG_SSBO_NEEDED : rg_storage_points[i].kind;
}

/* An OpTypeImage's Dim and Arrayed operands as the texture kind a unit
 * holds.  Anything else (1D, rect, buffer) reads as 2D; no stage in
 * engine/shaders declares one. */
static rg_textype_t RG_ImageType (Uint32 dim, Uint32 arrayed)
{
	if (dim == SPV_Dim3D)
		return RG_TT_3D;
	if (dim == SPV_DimCube)
		return arrayed ? RG_TT_CUBE_ARRAY : RG_TT_CUBE;
	return arrayed ? RG_TT_2D_ARRAY : RG_TT_2D;
}

/* The sampler and uniform-block bindings of one stage, by name. */
static qboolean RG_Reflect (const Uint32 *w, size_t nwords, int sampler_set, int ubo_set,
			    rgstage_t *out, const char *blob)
{
	spvid_t		*ids;
	Uint32		bound, id;
	size_t		p;
	int		nssbo = 0;

	memset (out, 0, sizeof(*out));
	if (nwords < 5 || w[0] != 0x07230203u)
	{
		Con_Printf ("SDL_GPU: %s is not SPIR-V\n", blob);
		return false;
	}
	bound = w[3];
	if (bound == 0 || bound > 1u << 22)
		return false;
	ids = (spvid_t *) calloc (bound, sizeof(spvid_t));
	if (!ids)
		Sys_Error ("SDL_GPU: out of memory reflecting %s", blob);

	for (p = 5; p < nwords; )
	{
		Uint32	op = w[p] & 0xFFFF, wc = w[p] >> 16;

		if (wc == 0 || p + wc > nwords)
			break;
		switch (op)
		{
		case SPV_OpName:
			if (wc >= 3 && w[p + 1] < bound)
				ids[w[p + 1]].name = (const char *) &w[p + 2];
			break;
		case SPV_OpDecorate:
			if (wc >= 3 && w[p + 1] < bound)
			{
				spvid_t	*d = &ids[w[p + 1]];

				if (w[p + 2] == SPV_DecBlock)
					d->block = true;
				else if (w[p + 2] == SPV_DecBufferBlock)
					d->bufblock = true;
				else if (w[p + 2] == SPV_DecBinding && wc >= 4)
					d->binding = w[p + 3], d->has_binding = true;
				else if (w[p + 2] == SPV_DecDescriptorSet && wc >= 4)
					d->set = w[p + 3], d->has_set = true;
			}
			break;
		case SPV_OpTypeImage:		/* result, sampled type, dim, depth, arrayed, ms */
			if (wc >= 7 && w[p + 1] < bound)
			{
				ids[w[p + 1]].op = op;
				ids[w[p + 1]].a = w[p + 3];	/* dim */
				ids[w[p + 1]].b = w[p + 5];	/* arrayed */
				ids[w[p + 1]].c = w[p + 4];	/* depth: 1 a shadow sampler */
				ids[w[p + 1]].d = w[p + 2];	/* sampled type */
				ids[w[p + 1]].e = w[p + 6];	/* multisampled */
			}
			break;
		case SPV_OpTypeInt:		/* result, width, signedness */
			if (wc >= 4 && w[p + 1] < bound)
			{
				ids[w[p + 1]].op = op;
				ids[w[p + 1]].b = w[p + 3];
			}
			break;
		case SPV_OpTypeFloat:		/* result, width */
			if (wc >= 3 && w[p + 1] < bound)
				ids[w[p + 1]].op = op;
			break;
		case SPV_OpTypeSampledImage:	/* result, image */
		case SPV_OpTypeStruct:
			if (wc >= 2 && w[p + 1] < bound)
			{
				ids[w[p + 1]].op = op;
				ids[w[p + 1]].a = (wc >= 3) ? w[p + 2] : 0;
			}
			break;
		case SPV_OpTypePointer:		/* result, storage class, type */
			if (wc >= 4 && w[p + 1] < bound)
			{
				ids[w[p + 1]].op = op;
				ids[w[p + 1]].a = w[p + 2];
				ids[w[p + 1]].b = w[p + 3];
			}
			break;
		case SPV_OpVariable:		/* result type, result, storage class */
			if (wc >= 4 && w[p + 2] < bound)
			{
				ids[w[p + 2]].op = op;
				ids[w[p + 2]].a = w[p + 1];
				ids[w[p + 2]].b = w[p + 3];
			}
			break;
		}
		p += wc;
	}

	for (id = 1; id < bound; id++)
	{
		spvid_t	*v = &ids[id], *ptr, *ty;

		if (v->op != SPV_OpVariable || v->a >= bound)
			continue;
		ptr = &ids[v->a];
		if (ptr->op != SPV_OpTypePointer || ptr->b >= bound)
			continue;
		ty = &ids[ptr->b];
		if (v->b == SPV_ScUniformConstant && ty->op == SPV_OpTypeSampledImage && ty->a < bound)
		{
			spvid_t		*img = &ids[ty->a];
			rgsampler_t	*s;
			const char	*nm = v->name ? v->name : "";

			if (out->num_samplers == RG_MAX_STAGE_SAMPLERS || !v->has_binding ||
			    v->binding >= RG_MAX_STAGE_SAMPLERS || (int)v->set != sampler_set)
			{
				Con_Printf ("SDL_GPU: %s: sampler %s outside set %d's 0..%d\n",
					    blob, nm, sampler_set, RG_MAX_STAGE_SAMPLERS - 1);
				free (ids);
				return false;
			}
			s = &out->samplers[out->num_samplers++];
			q_strlcpy (s->name, nm, sizeof(s->name));
			s->binding = (Uint8) v->binding;
			s->type = RG_ImageType (img->a, img->b);
			s->sclass = (img->d < bound && ids[img->d].op == SPV_OpTypeInt)
				    ? (ids[img->d].b ? RG_SC_SINT : RG_SC_UINT) : RG_SC_FLOAT;
			s->shadow = (img->c == 1);
			s->ms = (img->e != 0);
			/* The GL unit: u_textureN reads unit N, the convention every
			 * 2D, flat, sky and particle stage follows and the unit its
			 * builder gives it.  Every other sampler (light grid,
			 * lightstyle layers, MBOIT, shadow maps, soft depth) reads
			 * the unit R_ProgramSamplerUnit names, and none until then:
			 * its binding is a slot in the SPIR-V, not a GL unit. */
			if (!strncmp (nm, "u_texture", 9) && nm[9] >= '0' && nm[9] <= '9' && !nm[10])
				s->unit = (Uint8)(nm[9] - '0');
			else
				s->unit = RG_UNIT_NONE;
		}
		else if (ty->op == SPV_OpTypeStruct &&
			 ((v->b == SPV_ScStorageBuffer && ty->block) || (v->b == SPV_ScUniform && ty->bufblock)))
		{
			/* A storage block: StorageBuffer + Block from SPIR-V 1.3, Uniform
			 * + BufferBlock before.  Only the read-only set's (the samplers'
			 * set) are bound by slot here; a compute stage's read-write set
			 * is P5's.  `slot` holds the raw binding until RG_CheckStage
			 * knows how many textures come first. */
			rgssbo_t	*sb;
			int		format;

			if (!v->has_binding || (int)v->set != sampler_set)
				continue;
			if (nssbo == RG_MAX_STAGE_SSBOS || v->binding > 255)
			{
				Con_Printf ("SDL_GPU: %s: more than %d storage blocks, or a binding past 255\n",
					    blob, RG_MAX_STAGE_SSBOS);
				free (ids);
				return false;
			}
			sb = &out->ssbos[nssbo++];
			q_strlcpy (sb->name, ty->name ? ty->name : "", sizeof(sb->name));
			sb->slot = (Uint8) v->binding;
			sb->point = RG_StoragePoint (sb->name);
			sb->kind = (Uint8) RG_StorageKind (sb->name, &format);
			sb->format = (Sint8) format;
		}
		else if (v->b == SPV_ScUniform && ty->op == SPV_OpTypeStruct && ty->block)
		{
			rgubo_t		*u;

			if (out->num_ubos == RG_MAX_STAGE_UBOS || !v->has_binding ||
			    v->binding >= RG_MAX_STAGE_UBOS || (int)v->set != ubo_set)
			{
				Con_Printf ("SDL_GPU: %s: uniform block %s outside set %d's 0..%d\n",
					    blob, ty->name ? ty->name : "?", ubo_set, RG_MAX_STAGE_UBOS - 1);
				free (ids);
				return false;
			}
			u = &out->ubos[out->num_ubos++];
			q_strlcpy (u->name, ty->name ? ty->name : "", sizeof(u->name));
			u->binding = (Uint8) v->binding;
			u->block = -2;	/* resolved on first draw */
		}
	}
	free (ids);
	out->num_ssbos = nssbo;
	/* SDL_GPU numbers each class densely from 0, and the recorder indexes
	 * its binding arrays with these: a hole or a repeat would hand SDL a
	 * NULL texture or write past the array. */
	{
		Uint32	seen = 0;
		int	i;

		for (i = 0; i < out->num_samplers; i++)
		{
			Uint8	b = out->samplers[i].binding;

			if (b >= out->num_samplers || (seen & (1u << b)))
			{
				Con_Printf ("SDL_GPU: %s: sampler bindings are not 0..%d, each once\n",
					    blob, out->num_samplers - 1);
				return false;
			}
			seen |= 1u << b;
		}
		seen = 0;
		for (i = 0; i < out->num_ubos; i++)
		{
			Uint8	b = out->ubos[i].binding;

			if (b >= out->num_ubos || (seen & (1u << b)))
			{
				Con_Printf ("SDL_GPU: %s: uniform block slots are not 0..%d, each once\n",
					    blob, out->num_ubos - 1);
				return false;
			}
			seen |= 1u << b;
		}
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Programs                                                            */
/* ------------------------------------------------------------------ */

/* *owned is what the caller SDL_frees: the file read from an override
 * directory, or NULL for a blob in the embedded table. */
static const void *RG_LoadBlob (const char *blob, size_t *size, void **owned)
{
	char			path[MAX_OSPATH];
	const rg_spirv_t	*e;

	*owned = NULL;
	if (!rg_spv_found)
		return NULL;
	if (rg_spv_fromdir)
	{
		q_snprintf (path, sizeof(path), "%s/%s.vk.spv", rg_spvdir, blob);
		*owned = SDL_LoadFile (path, size);
		if (!*owned)
			Con_Printf ("SDL_GPU: cannot read %s (%s)\n", path, SDL_GetError ());
		return *owned;
	}
	e = RG_EmbeddedBlob (blob);
	if (!e)
	{
		Con_Printf ("SDL_GPU: %s.vk.spv is not in the embedded table\n", blob);
		return NULL;
	}
	*size = e->size;
	return e->code;
}

/* Reflect a stage and hold it to the counts this binary was built with. */
static qboolean RG_CheckStage (rprog_id_t id, int stage, const void *code, size_t size,
			       int sampler_set, int ubo_set, rgstage_t *out)
{
	const rprog_refl_t	*refl = R_Program_Reflection (id, stage);
	const char		*blob = R_Program_Blob (id, stage);

	if (!refl)
	{
		Con_Printf ("SDL_GPU: %s has no reflection row\n", blob);
		return false;
	}
	if (!RG_Reflect ((const Uint32 *) code, size / 4, sampler_set, ubo_set, out, blob))
		return false;
	if (refl->ro_storage_buffers > RG_MAX_STAGE_SSBOS)
	{
		Con_Printf ("SDL_GPU: %s has %d read-only storage buffers, the backend binds %d\n",
			    blob, refl->ro_storage_buffers, RG_MAX_STAGE_SSBOS);
		return false;
	}
	if (out->num_ssbos != refl->ro_storage_buffers)
	{
		Con_Printf ("SDL_GPU: %s.vk.spv from %s has %d read-only storage blocks, this build "
			    "expects %d\n", blob, rg_spvsource, out->num_ssbos, refl->ro_storage_buffers);
		return false;
	}
	/* In the read-only set the storage buffers follow the sampled and the
	 * storage textures: the SDL_GPU slot is the binding past those, and
	 * the slots are 0..n-1, each once, like every other class. */
	{
		Uint32	seen = 0;
		int	i, first = refl->samplers + refl->ro_storage_textures;

		for (i = 0; i < out->num_ssbos; i++)
		{
			int	slot = (int) out->ssbos[i].slot - first;

			if (slot < 0 || slot >= out->num_ssbos || (seen & (1u << slot)))
			{
				Con_Printf ("SDL_GPU: %s: storage block %s at binding %d is not one of slots "
					    "0..%d after the %d textures\n", blob, out->ssbos[i].name,
					    out->ssbos[i].slot, out->num_ssbos - 1, first);
				return false;
			}
			seen |= 1u << slot;
			out->ssbos[i].slot = (Uint8) slot;
		}
	}
	if (out->num_samplers != refl->samplers || out->num_ubos != refl->uniform_buffers)
	{
		Con_Printf ("SDL_GPU: %s.vk.spv from %s has %d samplers and %d uniform blocks, this "
			    "build expects %d and %d -- %s\n",
			    blob, rg_spvsource, out->num_samplers, out->num_ubos,
			    refl->samplers, refl->uniform_buffers,
			    rg_spv_fromdir ? "the blobs are from another build"
					   : "gl_progid_refl.h is stale; regenerate it with "
					     "tools/gen_program_reflection.py");
		return false;
	}
	return true;
}

static SDL_GPUShader *RG_MakeShader (rprog_id_t id, int stage, SDL_GPUShaderStage kind,
				     rgstage_t *out)
{
	const rprog_refl_t	*refl = R_Program_Reflection (id, stage);
	const char		*blob = R_Program_Blob (id, stage);
	SDL_GPUShaderCreateInfo	ci;
	SDL_GPUShader		*sh;
	size_t			size;
	void			*owned;
	const void		*code = RG_LoadBlob (blob, &size, &owned);

	if (!code)
		return NULL;
	if (!RG_CheckStage (id, stage, code, size,
			    kind == SDL_GPU_SHADERSTAGE_VERTEX ? 0 : 2,
			    kind == SDL_GPU_SHADERSTAGE_VERTEX ? 1 : 3, out))
	{
		SDL_free (owned);
		return NULL;
	}
	SDL_zero (ci);
	ci.code = (const Uint8 *) code;
	ci.code_size = size;
	ci.entrypoint = "main";
	ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
	ci.stage = kind;
	ci.num_samplers = refl->samplers;
	ci.num_storage_textures = refl->ro_storage_textures;
	ci.num_storage_buffers = refl->ro_storage_buffers;
	ci.num_uniform_buffers = refl->uniform_buffers;
	sh = SDL_CreateGPUShader (rg_dev, &ci);
	if (!sh)
		Con_Printf ("SDL_GPU: shader %s failed (%s)\n", blob, SDL_GetError ());
	SDL_free (owned);
	return sh;
}

static SDL_GPUComputePipeline *RG_MakeCompute (rprog_id_t id, rgstage_t *out)
{
	const rprog_refl_t		*refl = R_Program_Reflection (id, RPROG_STAGE_COMPUTE);
	const char			*blob = R_Program_Blob (id, RPROG_STAGE_COMPUTE);
	SDL_GPUComputePipelineCreateInfo ci;
	SDL_GPUComputePipeline		*cp;
	size_t				size;
	void				*owned;
	const void			*code = RG_LoadBlob (blob, &size, &owned);

	if (!code)
		return NULL;
	/* Compute: set 0 samplers, set 2 uniform buffers. */
	if (!RG_CheckStage (id, RPROG_STAGE_COMPUTE, code, size, 0, 2, out))
	{
		SDL_free (owned);
		return NULL;
	}
	SDL_zero (ci);
	ci.code = (const Uint8 *) code;
	ci.code_size = size;
	ci.entrypoint = "main";
	ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
	ci.num_samplers = refl->samplers;
	ci.num_readonly_storage_textures = refl->ro_storage_textures;
	ci.num_readonly_storage_buffers = refl->ro_storage_buffers;
	ci.num_readwrite_storage_textures = refl->rw_storage_textures;
	ci.num_readwrite_storage_buffers = refl->rw_storage_buffers;
	ci.num_uniform_buffers = refl->uniform_buffers;
	ci.threadcount_x = refl->threads[0];
	ci.threadcount_y = refl->threads[1];
	ci.threadcount_z = refl->threads[2];
	cp = SDL_CreateGPUComputePipeline (rg_dev, &ci);
	if (!cp)
		Con_Printf ("SDL_GPU: compute pipeline %s failed (%s)\n", blob, SDL_GetError ());
	SDL_free (owned);
	return cp;
}

GLuint RG_ProgramCreate (rprog_id_t id)
{
	rgprog_t	p;
	const char	*name = R_Program_Name (id);

	memset (&p, 0, sizeof(p));
	p.id = id;
	if (!R_Program_Blob (id, 1))
	{
		p.compute = true;
		p.cs = RG_MakeCompute (id, &p.stage[RPROG_STAGE_COMPUTE]);
		if (!p.cs)
			return 0;
	}
	else
	{
		p.vs = RG_MakeShader (id, RPROG_STAGE_VERTEX, SDL_GPU_SHADERSTAGE_VERTEX,
				      &p.stage[RPROG_STAGE_VERTEX]);
		p.fs = p.vs ? RG_MakeShader (id, RPROG_STAGE_FRAGMENT, SDL_GPU_SHADERSTAGE_FRAGMENT,
					     &p.stage[RPROG_STAGE_FRAGMENT]) : NULL;
		if (!p.vs || !p.fs)
		{
			if (p.vs)
				SDL_ReleaseGPUShader (rg_dev, p.vs);
			Con_Printf ("[SHADER] %s: no SDL_GPU program\n", name ? name : "?");
			return 0;
		}
	}
	if (rg_numprogs == rg_maxprogs)
	{
		rg_maxprogs = rg_maxprogs ? rg_maxprogs * 2 : 64;
		rg_progs = (rgprog_t **) realloc (rg_progs, rg_maxprogs * sizeof(rgprog_t *));
		if (!rg_progs)
			Sys_Error ("SDL_GPU: out of memory for programs");
	}
	p.name = (GLuint)(rg_numprogs + 1);
	rg_progs[rg_numprogs] = (rgprog_t *) malloc (sizeof(rgprog_t));
	if (!rg_progs[rg_numprogs])
		Sys_Error ("SDL_GPU: out of memory for programs");
	*rg_progs[rg_numprogs++] = p;
	if (developer.integer)
		Con_SafePrintf ("[SHADER] program '%s' built on SDL_GPU (id=%u)\n", name, p.name);
	return p.name;
}

const rgprog_t *RG_Program (GLuint name)
{
	if (name == 0 || (int)name > rg_numprogs)
		return NULL;
	return rg_progs[name - 1];
}

int RG_ProgramCount (void)
{
	return rg_numprogs;
}

GLint RG_ProgramBlock (GLuint program, const char *block)
{
	const rgprog_t	*p = RG_Program (program);
	int		s, i;

	if (!p)
		return -1;
	for (s = 0; s < RPROG_MAX_STAGES; s++)
	{
		for (i = 0; i < p->stage[s].num_ubos; i++)
		{
			if (!strcmp (p->stage[s].ubos[i].name, block))
				return p->stage[s].ubos[i].binding;
		}
	}
	return -1;
}

GLint RG_ProgramSampler (GLuint program, const char *name)
{
	const rgprog_t	*p = RG_Program (program);
	int		s, i;

	if (!p)
		return -1;
	for (s = 0; s < RPROG_MAX_STAGES; s++)
	{
		for (i = 0; i < p->stage[s].num_samplers; i++)
		{
			if (!strcmp (p->stage[s].samplers[i].name, name))
				return p->stage[s].samplers[i].binding;
		}
	}
	return -1;
}

void RG_ProgramSamplerUnit (GLuint program, const char *name, GLint unit)
{
	rgprog_t	*p;
	int		s, i;

	if (program == 0 || (int)program > rg_numprogs)
		return;
	if (unit < 0 || unit >= RT_MAX_TEXTURE_UNITS)
	{
		RG_NoteUnhandled ("sampler pointed at a texture unit above 15 (left unmapped)");
		return;
	}
	p = rg_progs[program - 1];
	for (s = 0; s < RPROG_MAX_STAGES; s++)
	{
		for (i = 0; i < p->stage[s].num_samplers; i++)
		{
			if (!strcmp (p->stage[s].samplers[i].name, name))
				p->stage[s].samplers[i].unit = (Uint8) unit;
		}
	}
}

void RG_Shader_Init (void)
{
	RG_FindShaders ();
}

void RG_Shader_Shutdown (void)
{
	int	i;

	for (i = 0; i < rg_numprogs; i++)
	{
		if (rg_progs[i]->vs)
			SDL_ReleaseGPUShader (rg_dev, rg_progs[i]->vs);
		if (rg_progs[i]->fs)
			SDL_ReleaseGPUShader (rg_dev, rg_progs[i]->fs);
		if (rg_progs[i]->cs)
			SDL_ReleaseGPUComputePipeline (rg_dev, rg_progs[i]->cs);
		free (rg_progs[i]);
	}
	free (rg_progs);
	rg_progs = NULL;
	rg_numprogs = rg_maxprogs = 0;
}
