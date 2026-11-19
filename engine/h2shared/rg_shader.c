/* rg_shader.c -- SDL_GPU backend: shaders and programs (#340 P1c)
 *
 * A program is an rprog_id_t (gl_progid.h).  Here it becomes one
 * SDL_GPUShader per stage, created from the stage's offline Vulkan SPIR-V
 * with the resource counts gl_progid_refl.h carries, and a GL-shaped name
 * the renderer binds with R_UseProgram like any other program.
 *
 * WHERE THE SPIR-V COMES FROM (decision 6 on #340, still open).  The blobs
 * are read at run time from a directory, not compiled into the binary:
 *
 *   1. -gpushaders <dir> on the command line;
 *   2. <the executable's directory>/spirv;
 *   3. <basedir>/spirv;
 *   4. the build tree's own generated/spirv, compiled in as RG_SPIRV_DIR when
 *      CMake configured with USE_SDL_GPU (a dev build that ran
 *      `make shaders_spirv` finds its blobs with no flag).
 *
 * That keeps glslangValidator out of every build -- the shaders_spirv target
 * stays opt-in, exactly as before -- and costs nothing to reverse: embedding
 * them later is a different answer to "find blob X", not a different
 * backend.  Without them, RG_CreateDevice refuses -gpu and the engine runs
 * GL.
 *
 * Each blob is reflected here as it loads: the sampler and uniform-block
 * bindings, with their names, read from the SPIR-V itself.  The sampler and
 * uniform-buffer counts are checked against gl_progid_refl.h, which the
 * binary was built with; a directory of blobs from another build fails that
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

static char		rg_spvdir[MAX_OSPATH];
static qboolean		rg_spvdir_found;

/* One allocation per program: a recorded draw holds its rgprog_t * until
 * the segment replays, and a program created mid-frame must not move it. */
static rgprog_t		**rg_progs;
static int		rg_numprogs, rg_maxprogs;

/* ------------------------------------------------------------------ */
/* Finding the blobs                                                   */
/* ------------------------------------------------------------------ */

/* The blob every program set has; its presence marks a usable directory. */
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

static void RG_FindShaderDir (void)
{
	char		path[MAX_OSPATH];
	const char	*base;
	int		i;

	rg_spvdir_found = false;
	i = COM_CheckParm ("-gpushaders");
	if (i && i + 1 < com_argc)
	{
		if (RG_DirHasBlobs (com_argv[i + 1]))
			goto found;
		Con_SafePrintf ("SDL_GPU: -gpushaders %s has no %s.vk.spv\n", com_argv[i + 1], RG_PROBE_BLOB);
	}
	base = SDL_GetBasePath ();
	if (base)
	{
		q_snprintf (path, sizeof(path), "%sspirv", base);
		if (RG_DirHasBlobs (path))
			goto found;
	}
	q_snprintf (path, sizeof(path), "%s/spirv", FS_GetBasedir ());
	if (RG_DirHasBlobs (path))
		goto found;
#ifdef RG_SPIRV_DIR
	if (RG_DirHasBlobs (RG_SPIRV_DIR))
		goto found;
#endif
	return;
found:
	rg_spvdir_found = true;
}

const char *RG_ShaderDir (void)
{
	return rg_spvdir_found ? rg_spvdir : NULL;
}

/* ------------------------------------------------------------------ */
/* Reflection                                                          */
/* ------------------------------------------------------------------ */

enum {
	SPV_OpName = 5,
	SPV_OpTypeImage = 25,
	SPV_OpTypeSampledImage = 27,
	SPV_OpTypeStruct = 30,
	SPV_OpTypePointer = 32,
	SPV_OpVariable = 59,
	SPV_OpDecorate = 71,
	SPV_DecBlock = 2,
	SPV_DecBinding = 33,
	SPV_DecDescriptorSet = 34,
	SPV_ScUniformConstant = 0,
	SPV_ScUniform = 2,
	SPV_Dim2D = 1,
	SPV_Dim3D = 2,
	SPV_DimCube = 3
};

typedef struct {
	Uint32		op;		/* the instruction that defines the id */
	Uint32		a, b, c;	/* its first operands after the result id */
	Uint32		set, binding;
	qboolean	has_set, has_binding, block;
	const char	*name;
} spvid_t;

/* The sampler and uniform-block bindings of one stage, by name. */
static qboolean RG_Reflect (const Uint32 *w, size_t nwords, int sampler_set, int ubo_set,
			    rgstage_t *out, const char *blob)
{
	spvid_t		*ids;
	Uint32		bound, id;
	size_t		p;

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
				else if (w[p + 2] == SPV_DecBinding && wc >= 4)
					d->binding = w[p + 3], d->has_binding = true;
				else if (w[p + 2] == SPV_DecDescriptorSet && wc >= 4)
					d->set = w[p + 3], d->has_set = true;
			}
			break;
		case SPV_OpTypeImage:		/* result, sampled type, dim, depth, arrayed */
			if (wc >= 6 && w[p + 1] < bound)
			{
				ids[w[p + 1]].op = op;
				ids[w[p + 1]].a = w[p + 3];
				ids[w[p + 1]].b = w[p + 5];
			}
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
			s->type = (img->a == SPV_Dim3D) ? RG_TT_3D :
				  (img->a == SPV_DimCube) ? RG_TT_CUBE :
				  (img->b) ? RG_TT_2D_ARRAY : RG_TT_2D;
			/* The GL unit: u_textureN reads unit N, the convention every
			 * 2D, flat, sky and particle stage follows.  The world's and
			 * alias family's other samplers (light grid, lightstyle
			 * layers, MBOIT, shadow maps) take their units from
			 * glUniform1i in GL_InitProgramSplit; mapping those is P2. */
			if (!strncmp (nm, "u_texture", 9) && nm[9] >= '0' && nm[9] <= '9' && !nm[10])
				s->unit = (Uint8)(nm[9] - '0');
			else
			{
				s->unit = s->binding;
				RG_NoteUnhandled ("sampler with no GL unit mapping yet (bound by its binding)");
			}
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

static void *RG_LoadBlob (const char *blob, size_t *size)
{
	char	path[MAX_OSPATH];
	void	*code;

	if (!rg_spvdir_found)
		return NULL;
	q_snprintf (path, sizeof(path), "%s/%s.vk.spv", rg_spvdir, blob);
	code = SDL_LoadFile (path, size);
	if (!code)
		Con_Printf ("SDL_GPU: cannot read %s (%s)\n", path, SDL_GetError ());
	return code;
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
	if (out->num_samplers != refl->samplers || out->num_ubos != refl->uniform_buffers)
	{
		Con_Printf ("SDL_GPU: %s/%s.vk.spv has %d samplers and %d uniform blocks, this build "
			    "expects %d and %d -- the blobs are from another build\n",
			    rg_spvdir, blob, out->num_samplers, out->num_ubos,
			    refl->samplers, refl->uniform_buffers);
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
	void			*code = RG_LoadBlob (blob, &size);

	if (!code)
		return NULL;
	if (!RG_CheckStage (id, stage, code, size,
			    kind == SDL_GPU_SHADERSTAGE_VERTEX ? 0 : 2,
			    kind == SDL_GPU_SHADERSTAGE_VERTEX ? 1 : 3, out))
	{
		SDL_free (code);
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
	SDL_free (code);
	return sh;
}

static SDL_GPUComputePipeline *RG_MakeCompute (rprog_id_t id, rgstage_t *out)
{
	const rprog_refl_t		*refl = R_Program_Reflection (id, RPROG_STAGE_COMPUTE);
	const char			*blob = R_Program_Blob (id, RPROG_STAGE_COMPUTE);
	SDL_GPUComputePipelineCreateInfo ci;
	SDL_GPUComputePipeline		*cp;
	size_t				size;
	void				*code = RG_LoadBlob (blob, &size);

	if (!code)
		return NULL;
	/* Compute: set 0 samplers, set 2 uniform buffers. */
	if (!RG_CheckStage (id, RPROG_STAGE_COMPUTE, code, size, 0, 2, out))
	{
		SDL_free (code);
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
	SDL_free (code);
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

void RG_Shader_Init (void)
{
	RG_FindShaderDir ();
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
