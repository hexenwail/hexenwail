/* gl_uniforms.c -- the two std140 uniform blocks shared by every file-backed
 * draw program, the alias family's AliasFrag and AliasVert, the world
 * family's WorldFrag, and the OIT twins' OitFrag.  See
 * gl_uniforms.h for the layout and the reasoning.
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
#include "gl_matrix.h"
#include "gl_shader.h"		/* UBO_BINDING_VERT / _FRAG / _ALIAS_FRAG / _ALIAS_VERT / _WORLD_FRAG / _OIT_FRAG */
#include "gl_uniforms.h"
#ifdef USE_SDL_GPU
#include "rg_device.h"
#endif

#ifndef GL_UNIFORM_BLOCK_DATA_SIZE
#define GL_UNIFORM_BLOCK_DATA_SIZE	0x8A40
#endif

/* On the ES tier these are real symbols rather than loader-table pointers, so
 * they cannot be tested as values at all -- the comparison is not merely
 * always-true, it does not compile as written. */
#ifdef USE_GLES
#define RU_HAVE_UBO_BIND	1
#define RU_HAVE_UBO_QUERY	1
#else
#define RU_HAVE_UBO_BIND	(glGetUniformBlockIndex_fp != NULL && \
				 glUniformBlockBinding_fp != NULL)
#define RU_HAVE_UBO_QUERY	(glGetActiveUniformBlockiv_fp != NULL)
#endif

#define RU_NAME_VERT	"VertParams"
#define RU_NAME_FRAG	"FragParams"
#define RU_NAME_AFRAG	"AliasFrag"
#define RU_NAME_AVERT	"AliasVert"
#define RU_NAME_WFRAG	"WorldFrag"
#define RU_NAME_OFRAG	"OitFrag"

static GLuint		ubo_vert;
static GLuint		ubo_frag;
static GLuint		ubo_afrag;
static GLuint		ubo_avert;
static GLuint		ubo_wfrag;
static GLuint		ubo_ofrag;

static r_vertparams_t	vertparams;
static r_fragparams_t	fragparams;
static r_aliasfrag_t	aliasfrag;
static r_aliasvert_t	aliasvert;
static r_worldfrag_t	worldfrag;
static r_oitfrag_t	oitfrag;

/* The world frame record (#450 W2): the per-frame half of what the world
 * fragment stages read, kept here, not in any shadow, until R_SetWorldFrag
 * copies it into one.  worldframe has WorldFrag's layout; the warp's xy is
 * never read from it (R_SetWorldFrag takes the liquid warp per draw). */
static r_worldfrag_t	worldframe;
static float		worldframe_caustics[2];
static float		worldframe_bicubic;
static float		worldframe_debug[2];
static float		worldframe_onesided;	/* the frame's, for R_SetWorldLiquid (false) */

static qboolean		vert_dirty;
static qboolean		frag_dirty;
static qboolean		afrag_dirty;
static qboolean		avert_dirty;
static qboolean		wfrag_dirty;
static qboolean		ofrag_dirty;
static qboolean		bindings_dirty;
static qboolean		layout_checked;
static qboolean		afrag_layout_checked;
static qboolean		avert_layout_checked;
static qboolean		wfrag_layout_checked;
static qboolean		ofrag_layout_checked;

/* The mirrors are only trustworthy if they are the size std140 says they are.
 * All are built from vec4/mat4/ivec4 only, so this is arithmetic rather than
 * hope: 4 mat4 + 6 vec4 + 1 ivec4, 9 vec4, 1 mat4 + 5 vec4, 1 mat4 +
 * 2 vec4, 1 mat4 + 6 vec4, and 1 ivec4. */
COMPILE_TIME_ASSERT(vertparams_size, sizeof(r_vertparams_t) == 4 * 64 + 7 * 16);
COMPILE_TIME_ASSERT(fragparams_size, sizeof(r_fragparams_t) == 9 * 16);
COMPILE_TIME_ASSERT(aliasfrag_size, sizeof(r_aliasfrag_t) == 64 + 5 * 16);
COMPILE_TIME_ASSERT(aliasvert_size, sizeof(r_aliasvert_t) == 64 + 2 * 16);
COMPILE_TIME_ASSERT(worldfrag_size, sizeof(r_worldfrag_t) == 64 + 6 * 16);
COMPILE_TIME_ASSERT(oitfrag_size, sizeof(r_oitfrag_t) == 16);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static qboolean RU_Set4 (float *dst, float a, float b, float c, float d)
{
	if (dst[0] == a && dst[1] == b && dst[2] == c && dst[3] == d)
		return false;
	dst[0] = a;
	dst[1] = b;
	dst[2] = c;
	dst[3] = d;
	return true;
}

static qboolean RU_SetMat (float *dst, const float *m16)
{
	if (!memcmp(dst, m16, 16 * sizeof(float)))
		return false;
	memcpy(dst, m16, 16 * sizeof(float));
	return true;
}

/* ------------------------------------------------------------------ */
/* VertParams setters                                                  */
/* ------------------------------------------------------------------ */

void R_SetMVP (const float *m16)
{
	if (RU_SetMat(vertparams.mvp, m16))
		vert_dirty = true;
}

void R_SetModelView (const float *m16)
{
	if (RU_SetMat(vertparams.modelview, m16))
		vert_dirty = true;
}

void R_SetModelMatrix (const float *m16)
{
	if (RU_SetMat(vertparams.model, m16))
		vert_dirty = true;
}

void R_SetViewProj (const float *m16)
{
	if (RU_SetMat(vertparams.viewproj, m16))
		vert_dirty = true;
}

void R_SetParticleBasis (const float *pup, const float *pright,
			 const float *vpn, const float *origin, float ptime)
{
	int changed = 0;

	changed |= RU_Set4(vertparams.pup, pup[0], pup[1], pup[2], 0.0f);
	changed |= RU_Set4(vertparams.pright, pright[0], pright[1], pright[2], 0.0f);
	changed |= RU_Set4(vertparams.vpn, vpn[0], vpn[1], vpn[2], 0.0f);
	changed |= RU_Set4(vertparams.porigin, origin[0], origin[1], origin[2], ptime);
	if (changed)
		vert_dirty = true;
}

void R_SetSkelPose (int base, int base2, float blend)
{
	if (vertparams.posei[0] != base || vertparams.posei[3] != base2
	 || vertparams.alias[1] != blend)
	{
		vertparams.posei[0] = base;
		vertparams.posei[3] = base2;
		vertparams.alias[1] = blend;
		vert_dirty = true;
	}
}

void R_SetInstBase (int v)
{
	if (vertparams.posei[1] != v)
	{
		vertparams.posei[1] = v;
		vert_dirty = true;
	}
}

void R_SetPoseVertType (int v)
{
	if (vertparams.posei[2] != v)
	{
		vertparams.posei[2] = v;
		vert_dirty = true;
	}
}

/* salias_vert's u_alias_dlight (#450): the blocklight -> vertex-colour scale,
 * 0 when the clustered dynamic-light path is off for the batch.  Desktop
 * only in the shader (the ES arm of salias_vert does not read it), but the
 * member exists on both tiers so the layout never varies. */
void R_SetAliasDlight (float scale)
{
	if (vertparams.alias[0] != scale)
	{
		vertparams.alias[0] = scale;
		vert_dirty = true;
	}
}

/* ------------------------------------------------------------------ */
/* Duplicated into both blocks                                         */
/* ------------------------------------------------------------------ */

void R_SetEyePos (const float *xyz)
{
	if (RU_Set4(vertparams.scene, xyz[0], xyz[1], xyz[2], vertparams.scene[3]))
		vert_dirty = true;
	if (RU_Set4(fragparams.scene, xyz[0], xyz[1], xyz[2], fragparams.scene[3]))
		frag_dirty = true;
}

void R_SetFrameTime (float t)
{
	if (vertparams.scene[3] != t)
	{
		vertparams.scene[3] = t;
		vert_dirty = true;
	}
	if (fragparams.scene[3] != t)
	{
		fragparams.scene[3] = t;
		frag_dirty = true;
	}
}

/* ------------------------------------------------------------------ */
/* FragParams setters                                                  */
/* ------------------------------------------------------------------ */

void R_SetFog (float density, const float *rgb)
{
	if (RU_Set4(fragparams.fog, rgb[0], rgb[1], rgb[2], density))
		frag_dirty = true;
}

void R_SetAlphaMask (float threshold, float force_opaque)
{
	if (fragparams.mask[0] != threshold || fragparams.mask[1] != force_opaque)
	{
		fragparams.mask[0] = threshold;
		fragparams.mask[1] = force_opaque;
		frag_dirty = true;
	}
}

void R_SetOverbright (float v)
{
	if (fragparams.mask[2] != v)
	{
		fragparams.mask[2] = v;
		frag_dirty = true;
	}
}

void R_SetLightmapBicubic (float v)
{
	if (fragparams.mask[3] != v)
	{
		fragparams.mask[3] = v;
		frag_dirty = true;
	}
}

void R_SetWorldCaustics (float intensity, float time)
{
	if (fragparams.caustics[0] != intensity || fragparams.caustics[1] != time)
	{
		fragparams.caustics[0] = intensity;
		fragparams.caustics[1] = time;
		frag_dirty = true;
	}
}

void R_SetAliasCausticsU (float intensity, float time)
{
	if (fragparams.caustics[2] != intensity || fragparams.caustics[3] != time)
	{
		fragparams.caustics[2] = intensity;
		fragparams.caustics[3] = time;
		frag_dirty = true;
	}
}

void R_SetSoftParams (float x, float y, float z)
{
	if (RU_Set4(fragparams.soft, x, y, z, 0.0f))
		frag_dirty = true;
}

void R_SetSkyFog (float r, float g, float b, float a)
{
	if (RU_Set4(fragparams.skyfog, r, g, b, a))
		frag_dirty = true;
}

void R_SetSkyWind (float u, float v)
{
	if (RU_Set4(fragparams.sky, u, v, 0.0f, 0.0f))
		frag_dirty = true;
}

void R_SetSkyCubeWind (const float *dir3, float yaw)
{
	if (RU_Set4(fragparams.skydir, dir3[0], dir3[1], dir3[2], yaw))
		frag_dirty = true;
}

void R_SetLightDebug (float fullbright, float lightmapdbg)
{
	if (RU_Set4(fragparams.debug, fullbright, lightmapdbg, 0.0f, 0.0f))
		frag_dirty = true;
}

/* ------------------------------------------------------------------ */
/* AliasFrag setters                                                   */
/* ------------------------------------------------------------------ */

void R_SetAliasLightGrid (const float *view16, const float *grid4,
			  float zlogscale, float zlogbias)
{
	int changed = 0;

	changed |= RU_SetMat(aliasfrag.lightview, view16);
	changed |= RU_Set4(aliasfrag.grid, grid4[0], grid4[1], grid4[2], grid4[3]);
	changed |= RU_Set4(aliasfrag.gridz, zlogscale, zlogbias, 0.0f, 0.0f);
	if (changed)
		afrag_dirty = true;
	/* The instanced vertex stage's copy of the same matrix (#450 S3).  One
	 * call writes both, so the stage that builds v_eyepos and the one that
	 * looks it up in the grid cannot drift apart. */
	if (RU_SetMat(aliasvert.lightview, view16))
		avert_dirty = true;
}

void R_SetAliasTurb (float amplitude, float time)
{
	if (aliasfrag.warp[0] != amplitude || aliasfrag.warp[1] != time)
	{
		aliasfrag.warp[0] = amplitude;
		aliasfrag.warp[1] = time;
		afrag_dirty = true;
	}
}

void R_SetAliasSpans (float cell)
{
	if (aliasfrag.warp[2] != cell)
	{
		aliasfrag.warp[2] = cell;
		afrag_dirty = true;
	}
}

void R_SetAliasMaterialU (float normal, float gloss, float glossexp)
{
	if (RU_Set4(aliasfrag.material, normal, gloss, glossexp, 0.0f))
		afrag_dirty = true;
}

void R_SetAliasLightDirU (const float *dir3)
{
	if (RU_Set4(aliasfrag.lightdir, dir3[0], dir3[1], dir3[2], 0.0f))
		afrag_dirty = true;
}

/* ------------------------------------------------------------------ */
/* AliasVert setters (R_SetAliasLightGrid above writes its lightview)  */
/* ------------------------------------------------------------------ */

void R_SetSkelLight (const float *shadevec3, const float *light4, float fullbright)
{
	int changed = 0;

	changed |= RU_Set4(aliasvert.shade, shadevec3[0], shadevec3[1], shadevec3[2], fullbright);
	changed |= RU_Set4(aliasvert.light, light4[0], light4[1], light4[2], light4[3]);
	if (changed)
		avert_dirty = true;
}

/* ------------------------------------------------------------------ */
/* The world frame record and WorldFrag (#450 W2)                      */
/* ------------------------------------------------------------------ */

void R_SetWorldFrame (float caustics_intensity, float caustics_time, float bicubic,
		      float fullbright, float lightmapdbg,
		      const float *material3, const float *softemu4)
{
	worldframe_caustics[0] = caustics_intensity;
	worldframe_caustics[1] = caustics_time;
	worldframe_bicubic = bicubic;
	worldframe_debug[0] = fullbright;
	worldframe_debug[1] = lightmapdbg;
	RU_Set4 (worldframe.material, material3[0], material3[1], material3[2], 0.0f);
	RU_Set4 (worldframe.softemu, softemu4[0], softemu4[1], softemu4[2], softemu4[3]);
}

void R_SetWorldSpans (float cell)
{
	worldframe.warp[2] = cell;
}

void R_SetWorldDlight (float scale, const float *view16, const float *grid4,
		       float zlogscale, float zlogbias, float onesided)
{
	RU_SetMat (worldframe.lightview, view16);
	RU_Set4 (worldframe.grid, grid4[0], grid4[1], grid4[2], grid4[3]);
	RU_Set4 (worldframe.gridz, zlogscale, zlogbias, scale, onesided);
	worldframe_onesided = onesided;
}

void R_SetWorldLiquid (qboolean liquid)
{
	worldframe.gridz[3] = liquid ? 0.0f : worldframe_onesided;
}

void R_SetWorldLMStyles (float on, float shift)
{
	RU_Set4 (worldframe.lmstyles, on, shift, 0.0f, 0.0f);
}

/* The whole of WorldFrag is replaced from the frame record on every call, and
 * every FragParams member the world stages read is written through its own
 * setter, so no member can keep a value some other program's batch left. */
void R_SetWorldFrag (float fog_density, const float *fog_rgb,
		     float alpha_threshold, float force_opaque, float overbright,
		     float turb_amplitude, float turb_time)
{
	r_worldfrag_t	w = worldframe;

	R_SetFog (fog_density, fog_rgb);
	R_SetAlphaMask (alpha_threshold, force_opaque);
	R_SetOverbright (overbright);
	R_SetLightmapBicubic (worldframe_bicubic);
	R_SetWorldCaustics (worldframe_caustics[0], worldframe_caustics[1]);
	R_SetLightDebug (worldframe_debug[0], worldframe_debug[1]);

	w.warp[0] = turb_amplitude;
	w.warp[1] = turb_time;
	if (memcmp (&worldfrag, &w, sizeof(w)))
	{
		worldfrag = w;
		wfrag_dirty = true;
	}
}

/* ------------------------------------------------------------------ */
/* OitFrag (#450 W3)                                                   */
/* ------------------------------------------------------------------ */

void R_SetOITMode (int mode, qboolean ms)
{
	int	m = ms ? 1 : 0;

	if (oitfrag.mode[0] != mode || oitfrag.mode[1] != m)
	{
		oitfrag.mode[0] = mode;
		oitfrag.mode[1] = m;
		ofrag_dirty = true;
	}
}

/* ------------------------------------------------------------------ */
/* Upload                                                              */
/* ------------------------------------------------------------------ */

#ifdef USE_SDL_GPU
/* R_FlushUniforms' SDL_GPU arm: each dirty mirror is handed to the backend,
 * which snapshots it.  Table-driven, one row per block, in the GL arm's
 * order. */
static void RU_FlushToBackend (void)
{
	struct { qboolean *dirty; const char *name; } const rows[] = {
		{ &vert_dirty, RU_NAME_VERT },
		{ &frag_dirty, RU_NAME_FRAG },
		{ &afrag_dirty, RU_NAME_AFRAG },
		{ &avert_dirty, RU_NAME_AVERT },
		{ &wfrag_dirty, RU_NAME_WFRAG },
		{ &ofrag_dirty, RU_NAME_OFRAG }
	};
	size_t	i;

	bindings_dirty = false;
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
	{
		if (*rows[i].dirty)
		{
			*rows[i].dirty = false;
			RG_UniformBlockFlushed (rows[i].name);
		}
	}
}
#endif

/*
===============
R_FlushUniforms

One push per dirty block, whole block, called immediately before the draw that
reads it.

The buffers are permanent and bound once with GL_BindBufferBase, rather than
streamed through GL_Upload's frame ring the way vertex data is.  That is what
makes skipping an unchanged block safe: a ring range that was bound two draws
ago may since have been handed to somebody else, so "nothing changed, so do not
push" would be reading recycled storage.  A named buffer still holds what was
last written to it however long ago that was.  The ring is also a no-op stub on
the ES tier (gl_buffer.c), which would have made this draw nothing at all in
the browser and on macOS.

glBufferData rather than glBufferSubData: respecifying the whole store orphans
the old one, so a flush never waits on a draw still reading the previous
contents.  Both blocks are a few hundred bytes, so the respecify costs nothing
next to the stall it avoids.
===============
*/
void R_FlushUniforms (void)
{
#ifdef USE_SDL_GPU
	/* No buffers under -gpu: a flush tells the backend which mirrors to
	 * snapshot, and it pushes the snapshot into whatever slot each draw's
	 * program declares that block at (SDL_Push*UniformData).  Same contract
	 * as the GL arm: a draw sees what was last FLUSHED, not what a setter
	 * wrote since. */
	if (R_GPU ())
	{
		RU_FlushToBackend ();
		return;
	}
#endif
	if (bindings_dirty)
	{
		bindings_dirty = false;
		GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_VERT, ubo_vert);
		GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_FRAG, ubo_frag);
		GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_ALIAS_FRAG, ubo_afrag);
		GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_ALIAS_VERT, ubo_avert);
		GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_WORLD_FRAG, ubo_wfrag);
		GL_BindBufferBase (GL_UNIFORM_BUFFER, UBO_BINDING_OIT_FRAG, ubo_ofrag);
	}

	if (vert_dirty)
	{
		vert_dirty = false;
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_vert);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(vertparams), &vertparams,
				 GL_STREAM_DRAW);
	}
	if (frag_dirty)
	{
		frag_dirty = false;
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_frag);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(fragparams), &fragparams,
				 GL_STREAM_DRAW);
	}
	if (afrag_dirty)
	{
		afrag_dirty = false;
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_afrag);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(aliasfrag), &aliasfrag,
				 GL_STREAM_DRAW);
	}
	if (avert_dirty)
	{
		avert_dirty = false;
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_avert);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(aliasvert), &aliasvert,
				 GL_STREAM_DRAW);
	}
	if (wfrag_dirty)
	{
		wfrag_dirty = false;
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_wfrag);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(worldfrag), &worldfrag,
				 GL_STREAM_DRAW);
	}
	if (ofrag_dirty)
	{
		ofrag_dirty = false;
		GL_BindBuffer (GL_UNIFORM_BUFFER, ubo_ofrag);
		glBufferData_fp (GL_UNIFORM_BUFFER, sizeof(oitfrag), &oitfrag,
				 GL_STREAM_DRAW);
	}
}

void R_InvalidateUniforms (void)
{
	vert_dirty = true;
	frag_dirty = true;
	afrag_dirty = true;
	avert_dirty = true;
	wfrag_dirty = true;
	ofrag_dirty = true;
	bindings_dirty = true;
}

/* ------------------------------------------------------------------ */
/* Program hookup                                                      */
/* ------------------------------------------------------------------ */

/* Ask the driver where it actually put things, once, and complain rather than
 * render garbage if it disagrees with the C mirror.  A std140 block built only
 * from vec4/mat4/ivec4 leaves the driver no room to differ, which is the point
 * -- but a layout bug here is invisible on screen except as wrong values, so
 * it is worth one query at startup to turn that into a console line. */
static void RU_CheckBlockSize (GLuint program, const char *name, size_t expect)
{
	GLuint	idx;
	GLint	size = 0;

#ifdef USE_SDL_GPU
	/* The SPIR-V's own std140 layout is what SDL_GPU reads; no driver to
	 * ask.  tools/test_sdlgpu_bindings.py checks the blobs offline. */
	if (R_GPU ())
		return;
#endif
	if (!RU_HAVE_UBO_QUERY)
		return;
	idx = glGetUniformBlockIndex_fp (program, name);
	if (idx == GL_INVALID_INDEX)
		return;
	glGetActiveUniformBlockiv_fp (program, idx, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
	if (size != (GLint)expect)
		Con_Printf ("WARNING: uniform block %s is %d bytes on this driver, "
			    "C mirror is %d -- uniforms will be wrong\n",
			    name, (int)size, (int)expect);
	else
		Con_SafePrintf ("  uniform block %s: %d bytes\n", name, (int)size);
}

static GLint RU_BindOne (GLuint program, const char *name, GLuint binding)
{
	GLuint	index;

#ifdef USE_SDL_GPU
	/* The block's slot from the program's SPIR-V; no binding point to pin,
	 * because SDL_GPU numbers uniform slots per stage and the blob fixes
	 * them (VERT_UBO / FRAG_UBO in layout.inc). */
	if (R_GPU ())
	{
		(void) binding;
		return RG_ProgramBlock (program, name);
	}
#endif
	index = glGetUniformBlockIndex_fp (program, name);

	if (index == GL_INVALID_INDEX)
		return -1;
	glUniformBlockBinding_fp (program, index, binding);
	return (GLint) index;
}

void R_BindProgramBlocks (GLuint program, GLint *out_vert, GLint *out_frag)
{
	GLint	vidx = -1, fidx = -1;

	if (program && RU_HAVE_UBO_BIND)
	{
		vidx = RU_BindOne (program, RU_NAME_VERT, UBO_BINDING_VERT);
		fidx = RU_BindOne (program, RU_NAME_FRAG, UBO_BINDING_FRAG);

		/* First program that declares either block is the one we ask
		 * about; a size the driver disagrees with is a property of the
		 * layout, not of the program. */
		if (!layout_checked && (vidx >= 0 || fidx >= 0))
		{
			layout_checked = true;
			RU_CheckBlockSize (program, RU_NAME_VERT, sizeof(r_vertparams_t));
			RU_CheckBlockSize (program, RU_NAME_FRAG, sizeof(r_fragparams_t));
		}
	}
	else if (program)
	{
		static qboolean warned = false;

		if (!warned)
		{
			warned = true;
			Con_Printf ("[SHADER] no uniform block entry points; "
				    "block-backed shaders will not render\n");
		}
	}

	if (out_vert) *out_vert = vidx;
	if (out_frag) *out_frag = fidx;
}

GLint R_BindAliasFragBlock (GLuint program)
{
	GLint	idx;

	if (!program || !RU_HAVE_UBO_BIND)
		return -1;
	idx = RU_BindOne (program, RU_NAME_AFRAG, UBO_BINDING_ALIAS_FRAG);
	if (idx >= 0 && !afrag_layout_checked)
	{
		afrag_layout_checked = true;
		RU_CheckBlockSize (program, RU_NAME_AFRAG, sizeof(r_aliasfrag_t));
	}
	return idx;
}

GLint R_BindAliasVertBlock (GLuint program)
{
	GLint	idx;

	if (!program || !RU_HAVE_UBO_BIND)
		return -1;
	idx = RU_BindOne (program, RU_NAME_AVERT, UBO_BINDING_ALIAS_VERT);
	if (idx >= 0 && !avert_layout_checked)
	{
		avert_layout_checked = true;
		RU_CheckBlockSize (program, RU_NAME_AVERT, sizeof(r_aliasvert_t));
	}
	return idx;
}

GLint R_BindWorldFragBlock (GLuint program)
{
	GLint	idx;

	if (!program || !RU_HAVE_UBO_BIND)
		return -1;
	idx = RU_BindOne (program, RU_NAME_WFRAG, UBO_BINDING_WORLD_FRAG);
	if (idx >= 0 && !wfrag_layout_checked)
	{
		wfrag_layout_checked = true;
		RU_CheckBlockSize (program, RU_NAME_WFRAG, sizeof(r_worldfrag_t));
	}
	return idx;
}

GLint R_BindOITFragBlock (GLuint program)
{
	GLint	idx;

	if (!program || !RU_HAVE_UBO_BIND)
		return -1;
	idx = RU_BindOne (program, RU_NAME_OFRAG, UBO_BINDING_OIT_FRAG);
	if (idx >= 0 && !ofrag_layout_checked)
	{
		ofrag_layout_checked = true;
		RU_CheckBlockSize (program, RU_NAME_OFRAG, sizeof(r_oitfrag_t));
	}
	return idx;
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

void R_Uniforms_Init (void)
{
	float	ident[16];

	memset (&vertparams, 0, sizeof(vertparams));
	memset (&fragparams, 0, sizeof(fragparams));
	memset (&aliasfrag, 0, sizeof(aliasfrag));
	memset (&aliasvert, 0, sizeof(aliasvert));
	memset (&worldfrag, 0, sizeof(worldfrag));
	memset (&oitfrag, 0, sizeof(oitfrag));
	memset (&worldframe, 0, sizeof(worldframe));
	worldframe_caustics[0] = worldframe_caustics[1] = 0.0f;
	worldframe_bicubic = 0.0f;
	worldframe_debug[0] = worldframe_debug[1] = 0.0f;
	worldframe_onesided = 0.0f;
	layout_checked = false;
	afrag_layout_checked = false;
	avert_layout_checked = false;
	wfrag_layout_checked = false;
	ofrag_layout_checked = false;

	/* GL zero-initialises nothing useful here.  An all-zero model matrix
	 * collapses every vertex to world (0,0) (uhexen2-0gn3).  The loose
	 * u_alias_model and the per-program identity upload that guarded it are
	 * gone (#450 S5), so this seed is now the only thing between that
	 * collapse and a VertParams draw issued before any R_SetModelMatrix;
	 * GL_ImmPushBlocks' feed, imm_alias_model, starts at identity for the
	 * same reason.  mvp and modelview are overwritten before any draw, but
	 * start sane too. */
	Mat4_Identity (ident);
	memcpy (vertparams.mvp, ident, sizeof(ident));
	memcpy (vertparams.modelview, ident, sizeof(ident));
	memcpy (vertparams.model, ident, sizeof(ident));
	memcpy (vertparams.viewproj, ident, sizeof(ident));
	/* The resting material and light direction imm_* holds, (0,0,1): xy 0
	 * is "no material maps", and a unit vector keeps the direction sane for
	 * anything that reads it before its first write.  The loose uniforms
	 * started at GL's zeros instead; both switch the material branch off,
	 * which is the only thing either value decides. */
	aliasfrag.material[2] = 1.0f;
	aliasfrag.lightdir[2] = 1.0f;
	memcpy (aliasfrag.lightview, ident, sizeof(ident));
	memcpy (aliasvert.lightview, ident, sizeof(ident));
	/* The world's, for the same reason; read only once a dlight scale makes
	 * the froxel lookup live, and that write brings its own view. */
	memcpy (worldfrag.lightview, ident, sizeof(ident));
	memcpy (worldframe.lightview, ident, sizeof(ident));

#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_UniformBlockRegister (RU_NAME_VERT, &vertparams, sizeof(vertparams));
		RG_UniformBlockRegister (RU_NAME_FRAG, &fragparams, sizeof(fragparams));
		RG_UniformBlockRegister (RU_NAME_AFRAG, &aliasfrag, sizeof(aliasfrag));
		RG_UniformBlockRegister (RU_NAME_AVERT, &aliasvert, sizeof(aliasvert));
		RG_UniformBlockRegister (RU_NAME_WFRAG, &worldfrag, sizeof(worldfrag));
		RG_UniformBlockRegister (RU_NAME_OFRAG, &oitfrag, sizeof(oitfrag));
	}
	else
#endif
	{
		glGenBuffers_fp (1, &ubo_vert);
		glGenBuffers_fp (1, &ubo_frag);
		glGenBuffers_fp (1, &ubo_afrag);
		glGenBuffers_fp (1, &ubo_avert);
		glGenBuffers_fp (1, &ubo_wfrag);
		glGenBuffers_fp (1, &ubo_ofrag);
	}

	/* Seed both stores before anything binds them, so a program that draws
	 * before its first setter call reads zeros and an identity rather than
	 * an unspecified store. */
	vert_dirty = true;
	frag_dirty = true;
	afrag_dirty = true;
	avert_dirty = true;
	wfrag_dirty = true;
	ofrag_dirty = true;
	bindings_dirty = true;
	R_FlushUniforms ();
}

void R_Uniforms_Shutdown (void)
{
	/* Under -gpu there are no buffers: every ubo_* stays 0, so each branch
	 * below is skipped and no GL call is made. */
	if (ubo_vert)
	{
		GL_ForgetBuffer (ubo_vert);
		glDeleteBuffers_fp (1, &ubo_vert);
		ubo_vert = 0;
	}
	if (ubo_frag)
	{
		GL_ForgetBuffer (ubo_frag);
		glDeleteBuffers_fp (1, &ubo_frag);
		ubo_frag = 0;
	}
	if (ubo_afrag)
	{
		GL_ForgetBuffer (ubo_afrag);
		glDeleteBuffers_fp (1, &ubo_afrag);
		ubo_afrag = 0;
	}
	if (ubo_avert)
	{
		GL_ForgetBuffer (ubo_avert);
		glDeleteBuffers_fp (1, &ubo_avert);
		ubo_avert = 0;
	}
	if (ubo_wfrag)
	{
		GL_ForgetBuffer (ubo_wfrag);
		glDeleteBuffers_fp (1, &ubo_wfrag);
		ubo_wfrag = 0;
	}
	if (ubo_ofrag)
	{
		GL_ForgetBuffer (ubo_ofrag);
		glDeleteBuffers_fp (1, &ubo_ofrag);
		ubo_ofrag = 0;
	}
	layout_checked = false;
	afrag_layout_checked = false;
	avert_layout_checked = false;
	wfrag_layout_checked = false;
	ofrag_layout_checked = false;
	bindings_dirty = true;
}
