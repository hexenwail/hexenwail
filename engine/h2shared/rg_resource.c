/* rg_resource.c -- SDL_GPU backend: textures and samplers (#340 P1c)
 *
 * The engine keeps its GL-shaped texture names (decision 1 on #340); this
 * table maps each name to an SDL_GPUTexture and the sampler its GL sampling
 * state describes.  GL and SDL_GPU disagree about when a texture exists:
 *
 *   GL       a name is defined level by level -- glTexImage2D(level 0),
 *            then (level 1) ... -- and can be resized or given more levels at
 *            any time;
 *   SDL_GPU  a texture is created once, with its size, format and number of
 *            levels fixed.
 *
 * So the definition is recorded as it arrives and the SDL_GPUTexture is
 * created as late as possible: at the first draw that samples it, or at the
 * first copy pass that has data for it, whichever comes first.  Every level
 * an engine loader defines arrives in one call chain, before either, so the
 * level count is known by then.  A level added after creation, or a level 0
 * redefined at another size or format, re-creates the texture: what older
 * levels held is lost, which is counted (`gpu_unrouted`) rather than hidden.
 *
 * Uploads are copied out of the caller's memory immediately (the caller is
 * free to reuse it, as with glTexImage2D), converted to the SDL format, and
 * queued for rg_frame.c's next copy pass.  A copy pass runs ahead of its
 * segment's draws, so an upload into a texture that a draw already recorded
 * in the open segment reads would change what that draw sees: the upload
 * first flushes the segment (RG_Frame_Flush).
 *
 * Format expansion follows the WebGL2 tier's rules: SDL_GPU has no RGB8, so
 * an RGB / RGB8 texture is stored RGBA8 with alpha forced to 255 at upload,
 * which is what GL's RGB8 returns from the sampler; BGRA source data is
 * swizzled to RGBA.  Anything else unsupported is counted and left
 * undefined, and a draw that samples it gets the white placeholder.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef USE_SDL_GPU
#error rg_resource.c is the SDL_GPU backend; CMake compiles it only with USE_SDL_GPU
#endif

#include "quakedef.h"
#include "rg_internal.h"
#include "gl_texbind.h"		/* RT_MAX_TEXTURE_UNITS */

/* Queued texture data, already in the texture's SDL format, tightly packed. */
typedef struct rgupload_s {
	struct rgupload_s	*next;
	Uint32		level, layer;
	Uint32		x, y, z, w, h, d;
	Uint32		size;
	Uint32		staged;		/* offset in the transfer buffer, once staged */
	Uint8		data[1];
} rgupload_t;

typedef struct {
	qboolean		alive;		/* name handed out and not deleted */
	/* the definition */
	rg_textype_t		type;
	SDL_GPUTextureFormat	fmt;
	SDL_GPUTextureUsageFlags usage;
	int			bpp;		/* bytes per texel in fmt */
	qboolean		rgb;		/* RGB internal format: alpha reads 1 */
	int			w, h, d;	/* level 0; d is depth (3D) or layers */
	int			levels;		/* defined: highest level + 1 */
	qboolean		defined;
	/* what exists */
	SDL_GPUTexture		*tex;
	int			tex_levels;
	/* sampling state, GL-shaped */
	GLint			min_filter, mag_filter;
	GLint			wrap_s, wrap_t, wrap_r;
	GLint			max_level;
	GLint			compare_mode, compare_func;
	float			lod_bias, anisotropy;
	SDL_GPUSampler		*sampler;	/* NULL: rebuild from the above */
	/* queue and hazard */
	rgupload_t		*uploads, **uploads_tail;
	unsigned int		read_segment;	/* RG_Frame_Segment () of the last draw */
	qboolean		read_ever;
} rgtex_t;

static rgtex_t		*rgt;
static int		rgt_count;	/* table size; name n is rgt[n] */
static int		rgt_used;	/* highest name ever handed out */
static GLuint		*rgt_free;	/* deleted names, reused like GL reuses them */
static int		rgt_numfree, rgt_maxfree;

/* Per unit, per dimensionality: RG_BindTexture's exact record. */
static GLuint		rgt_units[RT_MAX_TEXTURE_UNITS][RG_TT_COUNT];

/* Released only after the frame's command buffer is submitted: a recorded
 * draw can still name one when the delete or re-creation happens. */
static SDL_GPUTexture	**rgt_retired;
static int		rgt_numretired, rgt_maxretired;

/* Placeholders, one per dimensionality: opaque white, as gl_solid_white. */
static SDL_GPUTexture	*rgt_white[RG_TT_COUNT];
static SDL_GPUSampler	*rgt_white_sampler;
static qboolean		rgt_white_uploaded;

/* Samplers are few and shared; keyed by their whole create-info. */
#define RG_MAX_SAMPLERS	64
typedef struct {
	SDL_GPUSamplerCreateInfo	info;
	SDL_GPUSampler			*sampler;
} rgsamplerentry_t;
static rgsamplerentry_t	rgs_cache[RG_MAX_SAMPLERS];
static int		rgs_count;

static Uint32		rgt_pending_bytes;
static unsigned long	rgt_created, rgt_recreated, rgt_uploads, rgt_upload_bytes;

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

static rgtex_t *RGT_Get (GLuint name)
{
	if (name == 0 || (int)name > rgt_used || !rgt[name].alive)
		return NULL;
	return &rgt[name];
}

static void RGT_Reset (rgtex_t *t)
{
	memset (t, 0, sizeof(*t));
	t->alive = true;
	t->uploads_tail = &t->uploads;
	/* GL's defaults for a new texture object */
	t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
	t->mag_filter = GL_LINEAR;
	t->wrap_s = t->wrap_t = t->wrap_r = GL_REPEAT;
	t->max_level = 1000;
	t->compare_mode = GL_NONE;
	t->compare_func = GL_LEQUAL;
	t->anisotropy = 1.0f;
}

void RG_GenTextures (GLsizei n, GLuint *textures)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
	{
		GLuint	name;

		if (rgt_numfree > 0)
			name = rgt_free[--rgt_numfree];
		else
		{
			if (rgt_used + 1 >= rgt_count)
			{
				int	newcount = rgt_count ? rgt_count * 2 : 1024;

				rgt = (rgtex_t *) realloc (rgt, newcount * sizeof(rgtex_t));
				if (!rgt)
					Sys_Error ("SDL_GPU: out of memory for %d texture names", newcount);
				memset (rgt + rgt_count, 0, (newcount - rgt_count) * sizeof(rgtex_t));
				rgt_count = newcount;
			}
			name = (GLuint) ++rgt_used;
		}
		RGT_Reset (&rgt[name]);
		textures[i] = name;
	}
}

static void RGT_Retire (SDL_GPUTexture *tex)
{
	if (!tex)
		return;
	if (rgt_numretired == rgt_maxretired)
	{
		rgt_maxretired = rgt_maxretired ? rgt_maxretired * 2 : 64;
		rgt_retired = (SDL_GPUTexture **) realloc (rgt_retired, rgt_maxretired * sizeof(*rgt_retired));
		if (!rgt_retired)
			Sys_Error ("SDL_GPU: out of memory retiring textures");
	}
	rgt_retired[rgt_numretired++] = tex;
}

static void RGT_DropUploads (rgtex_t *t)
{
	rgupload_t	*u, *next;

	for (u = t->uploads; u; u = next)
	{
		next = u->next;
		rgt_pending_bytes -= u->size;
		free (u);
	}
	t->uploads = NULL;
	t->uploads_tail = &t->uploads;
}

/* A draw recorded in the open segment reads t, so its copy pass has not run:
 * anything that changes what that pass would put in t -- a new upload, or
 * dropping or re-targeting the queued ones -- has to end the segment first. */
static void RGT_FlushIfRead (const rgtex_t *t)
{
	if (t->read_ever && t->read_segment == RG_Frame_Segment () && RG_Frame_SegmentHasDraws ())
		RG_Frame_Flush ();
}

void RG_DeleteTextures (GLsizei n, const GLuint *textures)
{
	GLsizei	i;
	int	u, k;

	for (i = 0; i < n; i++)
	{
		rgtex_t	*t = RGT_Get (textures[i]);

		if (!t)
			continue;
		if (t->uploads)
			RGT_FlushIfRead (t);
		RGT_DropUploads (t);
		RGT_Retire (t->tex);
		t->alive = false;
		/* GL: deleting a bound texture unbinds it from every unit. */
		for (u = 0; u < RT_MAX_TEXTURE_UNITS; u++)
			for (k = 0; k < RG_TT_COUNT; k++)
				if (rgt_units[u][k] == textures[i])
					rgt_units[u][k] = 0;
		if (rgt_numfree == rgt_maxfree)
		{
			rgt_maxfree = rgt_maxfree ? rgt_maxfree * 2 : 256;
			rgt_free = (GLuint *) realloc (rgt_free, rgt_maxfree * sizeof(GLuint));
			if (!rgt_free)
				Sys_Error ("SDL_GPU: out of memory freeing texture names");
		}
		rgt_free[rgt_numfree++] = textures[i];
	}
}

/* ------------------------------------------------------------------ */
/* Targets and formats                                                 */
/* ------------------------------------------------------------------ */

/* The texture's dimensionality, and for a cube face which layer. */
static qboolean RGT_Target (GLenum target, rg_textype_t *type, int *layer)
{
	*layer = 0;
	switch (target)
	{
	case GL_TEXTURE_2D:		*type = RG_TT_2D; return true;
	case GL_TEXTURE_3D:		*type = RG_TT_3D; return true;
	case GL_TEXTURE_2D_ARRAY:	*type = RG_TT_2D_ARRAY; return true;
	case GL_TEXTURE_CUBE_MAP:	*type = RG_TT_CUBE; return true;
	}
	if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
	{
		*type = RG_TT_CUBE;
		*layer = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
		return true;
	}
	return false;
}

/* SDL format for a GL internal format; usage it needs beyond SAMPLER. */
static qboolean RGT_Format (GLint internalformat, SDL_GPUTextureFormat *fmt, int *bpp,
			    qboolean *rgb, qboolean *depth)
{
	*rgb = false;
	*depth = false;
	switch (internalformat)
	{
	case GL_RGBA: case GL_RGBA8: case 4:
		*fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; *bpp = 4; return true;
	case GL_RGB: case GL_RGB8: case 3:
		*fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; *bpp = 4; *rgb = true; return true;
	case GL_SRGB8_ALPHA8:
		*fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB; *bpp = 4; return true;
	case GL_SRGB8:
		*fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB; *bpp = 4; *rgb = true; return true;
	case GL_RED: case GL_R8: case 1:
		*fmt = SDL_GPU_TEXTUREFORMAT_R8_UNORM; *bpp = 1; return true;
	case GL_RG: case GL_RG8: case 2:
		*fmt = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM; *bpp = 2; return true;
	case GL_RGB10_A2:
		*fmt = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM; *bpp = 4; return true;
	case GL_RGBA16F:
		*fmt = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT; *bpp = 8; return true;
	case GL_RGBA32F:
		*fmt = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT; *bpp = 16; return true;
	case GL_R16F:
		*fmt = SDL_GPU_TEXTUREFORMAT_R16_FLOAT; *bpp = 2; return true;
	case GL_RG16F:
		*fmt = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT; *bpp = 4; return true;
	case GL_R32F:
		*fmt = SDL_GPU_TEXTUREFORMAT_R32_FLOAT; *bpp = 4; return true;
	case GL_R16UI:
		*fmt = SDL_GPU_TEXTUREFORMAT_R16_UINT; *bpp = 2; return true;
	case GL_R32UI:
		*fmt = SDL_GPU_TEXTUREFORMAT_R32_UINT; *bpp = 4; return true;
	case GL_RGBA32UI:
		*fmt = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT; *bpp = 16; return true;
	case GL_DEPTH_COMPONENT16:
		*fmt = SDL_GPU_TEXTUREFORMAT_D16_UNORM; *bpp = 2; *depth = true; return true;
	case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT24:
		*fmt = SDL_GPU_TEXTUREFORMAT_D24_UNORM; *bpp = 4; *depth = true; return true;
	case GL_DEPTH_COMPONENT32F:
		*fmt = SDL_GPU_TEXTUREFORMAT_D32_FLOAT; *bpp = 4; *depth = true; return true;
	case GL_DEPTH24_STENCIL8:
		*fmt = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT; *bpp = 4; *depth = true; return true;
	}
	return false;
}

/* Source texel size for (format, type); 0 for a pair this does not read. */
static int RGT_SourceBpp (GLenum format, GLenum type)
{
	int	comps, size;

	switch (format)
	{
	case GL_RED: case GL_RED_INTEGER: case GL_DEPTH_COMPONENT:	comps = 1; break;
	case GL_RG:						comps = 2; break;
	case GL_RGB: case GL_BGR:				comps = 3; break;
	case GL_RGBA: case GL_BGRA: case GL_RGBA_INTEGER:	comps = 4; break;
	default:						return 0;
	}
	switch (type)
	{
	case GL_UNSIGNED_BYTE:			size = 1; break;
	case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT:	size = 2; break;
	case GL_UNSIGNED_INT: case GL_FLOAT:	size = 4; break;
	case GL_UNSIGNED_INT_8_8_8_8_REV:	return (comps == 4) ? 4 : 0;
	default:				return 0;
	}
	return comps * size;
}

/* True if (format, type) client data is already fmt's texels byte for byte. */
static qboolean RGT_RawOk (SDL_GPUTextureFormat fmt, GLenum format, GLenum type)
{
	switch (fmt)
	{
	case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
		return format == GL_RED && type == GL_UNSIGNED_BYTE;
	case SDL_GPU_TEXTUREFORMAT_R8G8_UNORM:
		return format == GL_RG && type == GL_UNSIGNED_BYTE;
	case SDL_GPU_TEXTUREFORMAT_R16_UINT:
		return format == GL_RED_INTEGER && type == GL_UNSIGNED_SHORT;
	case SDL_GPU_TEXTUREFORMAT_R32_UINT:
		return format == GL_RED_INTEGER && type == GL_UNSIGNED_INT;
	case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT:
		return format == GL_RGBA_INTEGER && type == GL_UNSIGNED_INT;
	case SDL_GPU_TEXTUREFORMAT_R32_FLOAT:
		return format == GL_RED && type == GL_FLOAT;
	case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT:
		return format == GL_RGBA && type == GL_FLOAT;
	case SDL_GPU_TEXTUREFORMAT_R16_FLOAT:
		return format == GL_RED && type == GL_HALF_FLOAT;
	case SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT:
		return format == GL_RG && type == GL_HALF_FLOAT;
	case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
		return format == GL_RGBA && type == GL_HALF_FLOAT;
	default:
		return false;
	}
}

/* Copy one w x h x d box of GL client memory into tightly packed texels of
 * t's format.  `row_length` is the source's row pitch in pixels; rows are
 * padded to GL_UNPACK_ALIGNMENT, which the engine leaves at GL's 4. */
static qboolean RGT_Convert (const rgtex_t *t, GLenum format, GLenum type, const void *pixels,
			     int w, int h, int d, int row_length, Uint8 *dst)
{
	const Uint8	*src = (const Uint8 *) pixels;
	int		sbpp = RGT_SourceBpp (format, type);
	size_t		spitch, slab;
	int		x, y, z;
	qboolean	ubyte = (type == GL_UNSIGNED_BYTE || type == GL_UNSIGNED_INT_8_8_8_8_REV);

	if (!sbpp)
		return false;
	spitch = ((size_t)row_length * sbpp + 3) & ~(size_t)3;
	slab = spitch * h;

	for (z = 0; z < d; z++)
	{
		for (y = 0; y < h; y++)
		{
			const Uint8	*s = src + z * slab + y * spitch;
			Uint8		*o = dst + ((size_t)z * h + y) * w * t->bpp;

			if (t->fmt == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ||
			    t->fmt == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB)
			{
				if (!ubyte)
					return false;
				if (format == GL_RGBA || format == GL_BGRA)
				{
					qboolean	swap = (format == GL_BGRA);

					for (x = 0; x < w; x++, s += 4, o += 4)
					{
						o[0] = swap ? s[2] : s[0];
						o[1] = s[1];
						o[2] = swap ? s[0] : s[2];
						o[3] = t->rgb ? 255 : s[3];
					}
				}
				else if (format == GL_RGB || format == GL_BGR)
				{
					qboolean	swap = (format == GL_BGR);

					for (x = 0; x < w; x++, s += 3, o += 4)
					{
						o[0] = swap ? s[2] : s[0];
						o[1] = s[1];
						o[2] = swap ? s[0] : s[2];
						o[3] = 255;
					}
				}
				else
					return false;
			}
			else if (sbpp == t->bpp && RGT_RawOk (t->fmt, format, type))
			{
				/* The client layout is the texel format: R8 from
				 * RED/UBYTE, R32UI from RED_INTEGER/UINT, ... */
				memcpy (o, s, (size_t)w * t->bpp);
			}
			else
				return false;
		}
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Definition and upload                                               */
/* ------------------------------------------------------------------ */

static void RGT_QueueUpload (rgtex_t *t, int level, int layer, int x, int y, int z,
			     int w, int h, int d, GLenum format, GLenum type,
			     const void *pixels, int row_length)
{
	rgupload_t	*u;
	Uint32		size = (Uint32)w * h * d * t->bpp;

	if (!pixels || w <= 0 || h <= 0 || d <= 0)
		return;
	/* An upload a recorded draw would see too early (see the header). */
	RGT_FlushIfRead (t);

	u = (rgupload_t *) malloc (sizeof(rgupload_t) + size);
	if (!u)
		Sys_Error ("SDL_GPU: out of memory queueing a %u-byte texture upload", (unsigned int)size);
	if (!RGT_Convert (t, format, type, pixels, w, h, d, row_length, u->data))
	{
		free (u);
		RG_NoteUnhandled ("texture upload: source format/type has no conversion");
		return;
	}
	u->next = NULL;
	u->level = level;
	u->layer = layer;
	u->x = x; u->y = y; u->z = z;
	u->w = w; u->h = h; u->d = d;
	u->size = size;
	u->staged = 0;
	*t->uploads_tail = u;
	t->uploads_tail = &u->next;
	rgt_pending_bytes += size;
	rgt_uploads++;
	rgt_upload_bytes += size;

	/* A level load (a map's worth of textures) is not held in client
	 * memory until the next frame. */
	if (rgt_pending_bytes > 64u * 1024u * 1024u)
		RG_Frame_Flush ();
}

/* Levels in t's full mip chain: SDL_GPU, like Vulkan, refuses more. */
static int RGT_FullChain (const rgtex_t *t)
{
	int	maxdim = q_max (t->w, t->h), full;

	if (t->type == RG_TT_3D)
		maxdim = q_max (maxdim, t->d);
	for (full = 1; maxdim > 1; maxdim >>= 1)
		full++;
	return full;
}

/* Levels t is created with.  A sampled texture gets its whole chain up
 * front: GL defines levels one call at a time, and a copy pass can run
 * between two of them (the 64 MB flush, a hazard flush), which would
 * otherwise re-create the texture mid-chain and strand the levels already
 * uploaded in the retired object.  The sampler's max_lod keeps the levels
 * never defined out of reach.  Render targets keep what they declared. */
static int RGT_CreateLevels (const rgtex_t *t)
{
	int	full = RGT_FullChain (t);

	if (t->usage & (SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
		return q_min (t->levels, full);
	return full;
}

/* Make t's SDL_GPUTexture match its definition. */
static void RGT_Realize (rgtex_t *t)
{
	SDL_GPUTextureCreateInfo	ci;

	if (!t->defined)
		return;
	if (t->tex && t->tex_levels >= q_min (t->levels, RGT_FullChain (t)))
		return;
	if (t->tex)
	{
		/* A level added after the texture was created: the older levels'
		 * contents are gone with the old object. */
		RG_NoteUnhandled ("texture level defined after first use (re-created, contents lost)");
		RGT_Retire (t->tex);
		t->tex = NULL;
		rgt_recreated++;
	}

	SDL_zero (ci);
	switch (t->type)
	{
	case RG_TT_3D:		ci.type = SDL_GPU_TEXTURETYPE_3D; break;
	case RG_TT_CUBE:	ci.type = SDL_GPU_TEXTURETYPE_CUBE; break;
	case RG_TT_2D_ARRAY:	ci.type = SDL_GPU_TEXTURETYPE_2D_ARRAY; break;
	default:		ci.type = SDL_GPU_TEXTURETYPE_2D; break;
	}
	ci.format = t->fmt;
	ci.usage = t->usage;
	ci.width = t->w;
	ci.height = t->h;
	ci.layer_count_or_depth = (t->type == RG_TT_CUBE) ? 6 : t->d;
	ci.num_levels = RGT_CreateLevels (t);
	ci.sample_count = SDL_GPU_SAMPLECOUNT_1;

	if (!SDL_GPUTextureSupportsFormat (rg_dev, ci.format, ci.type, ci.usage))
	{
		RG_NoteUnhandled ("texture format/usage the device does not support");
		t->defined = false;
		RGT_DropUploads (t);
		return;
	}
	t->tex = SDL_CreateGPUTexture (rg_dev, &ci);
	if (!t->tex)
	{
		Con_Printf ("SDL_GPU: texture %dx%dx%d create failed (%s)\n",
			    t->w, t->h, t->d, SDL_GetError ());
		RG_NoteUnhandled ("texture create failed");
		t->defined = false;
		RGT_DropUploads (t);
		return;
	}
	t->tex_levels = ci.num_levels;
	t->sampler = NULL;	/* max_lod follows the levels that exist */
	rgt_created++;
}

void RG_TexImage (GLenum target, GLuint tex, GLint level, GLint internalformat,
		  GLsizei width, GLsizei height, GLsizei depth, GLenum format,
		  GLenum type, const void *pixels)
{
	rgtex_t			*t = RGT_Get (tex);
	rg_textype_t		ttype;
	int			layer, lw, lh, ld;
	SDL_GPUTextureFormat	fmt;
	int			bpp;
	qboolean		rgb, depthfmt;

	if (!t)
	{
		RG_NoteUnhandled ("texture upload to a name never generated");
		return;
	}
	if (!RGT_Target (target, &ttype, &layer))
	{
		RG_NoteUnhandled ("texture target with no SDL_GPU type");
		return;
	}
	if (!RGT_Format (internalformat, &fmt, &bpp, &rgb, &depthfmt))
	{
		RG_NoteUnhandled ("texture internal format with no SDL_GPU format");
		return;
	}
	if (ttype == RG_TT_CUBE)
		depth = 1;
	/* Level 0's size, from this level's. */
	lw = width << level;
	lh = height << level;
	ld = (ttype == RG_TT_3D) ? (depth << level) : depth;

	if (level == 0 || !t->defined)
	{
		if (t->defined && (t->fmt != fmt || t->type != ttype ||
				   (level == 0 && (t->w != lw || t->h != lh || t->d != ld))))
		{
			/* Redefined: a new object, whatever was queued for the old
			 * one is for a texture that no longer exists -- unless a
			 * recorded draw reads the old one, which still needs it. */
			RGT_FlushIfRead (t);
			RGT_DropUploads (t);
			RGT_Retire (t->tex);
			t->tex = NULL;
			t->tex_levels = 0;
			t->levels = 0;
			rgt_recreated++;
		}
		t->type = ttype;
		t->fmt = fmt;
		t->bpp = bpp;
		t->rgb = rgb;
		t->w = lw;
		t->h = lh;
		t->d = ld;
		t->usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		/* Storage with no data is a render target in this engine
		 * (gl_postprocess.c, gl_worldcull.c); P4 binds them. */
		if (depthfmt)
			t->usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		else if (!pixels && ttype == RG_TT_2D)
			t->usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		t->defined = true;
	}
	/* A level past what exists re-creates the texture at the next
	 * RGT_Realize, and the queued uploads then land in the new one: a draw
	 * recorded against the old one gets them first. */
	if (t->tex && level >= t->tex_levels)
		RGT_FlushIfRead (t);
	if (level + 1 > t->levels)
	{
		t->levels = level + 1;
		t->sampler = NULL;	/* max_lod follows the defined levels */
	}
	if (depthfmt && pixels)
	{
		RG_NoteUnhandled ("depth texture upload");
		return;
	}
	if (ttype == RG_TT_2D_ARRAY && pixels)
	{
		/* One region per layer: SDL_GPU's 2D-array upload is a layer
		 * with depth 1, not a box. */
		size_t	slab = (((size_t)width * RGT_SourceBpp (format, type) + 3) & ~(size_t)3) * height;
		int	i;

		for (i = 0; i < depth; i++)
			RGT_QueueUpload (t, level, i, 0, 0, 0, width, height, 1, format, type,
					 (const Uint8 *) pixels + i * slab, width);
		return;
	}
	RGT_QueueUpload (t, level, layer, 0, 0, 0, width, height,
			 (ttype == RG_TT_3D) ? depth : 1, format, type, pixels, width);
}

void RG_TexStorage2D (GLenum target, GLuint tex, GLsizei levels, GLenum internalformat,
		      GLsizei width, GLsizei height)
{
	rgtex_t	*t;

	RG_TexImage (target, tex, 0, (GLint)internalformat, width, height, 1, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	t = RGT_Get (tex);
	if (t && t->defined && levels > t->levels)
		t->levels = levels;
}

void RG_TexSubImage (GLenum target, GLuint tex, GLint level, GLint x, GLint y,
		     GLsizei width, GLsizei height, GLint row_length,
		     GLenum format, GLenum type, const void *pixels)
{
	rgtex_t		*t = RGT_Get (tex);
	rg_textype_t	ttype;
	int		layer;

	if (!t || !t->defined)
	{
		RG_NoteUnhandled ("texture sub-upload to an undefined texture");
		return;
	}
	if (!RGT_Target (target, &ttype, &layer) || ttype != t->type || level >= t->levels)
	{
		RG_NoteUnhandled ("texture sub-upload outside the texture's definition");
		return;
	}
	if (x < 0 || y < 0 || x + width > q_max (1, t->w >> level) || y + height > q_max (1, t->h >> level))
	{
		RG_NoteUnhandled ("texture sub-upload outside the level");
		return;
	}
	RGT_QueueUpload (t, level, layer, x, y, 0, width, height, 1, format, type, pixels,
			 row_length > 0 ? row_length : width);
}

void RG_TexParameter (GLenum target, GLuint tex, GLenum pname, GLint ivalue, GLfloat fvalue)
{
	rgtex_t	*t = RGT_Get (tex);

	(void) target;
	if (!t)
		return;
	switch (pname)
	{
	case GL_TEXTURE_MIN_FILTER:	t->min_filter = ivalue; break;
	case GL_TEXTURE_MAG_FILTER:	t->mag_filter = ivalue; break;
	case GL_TEXTURE_WRAP_S:		t->wrap_s = ivalue; break;
	case GL_TEXTURE_WRAP_T:		t->wrap_t = ivalue; break;
	case GL_TEXTURE_WRAP_R:		t->wrap_r = ivalue; break;
	case GL_TEXTURE_MAX_LEVEL:	t->max_level = ivalue; break;
	case GL_TEXTURE_LOD_BIAS:	t->lod_bias = fvalue; break;
	case GL_TEXTURE_MAX_ANISOTROPY_EXT:	t->anisotropy = fvalue; break;
	case GL_TEXTURE_COMPARE_MODE:	t->compare_mode = ivalue; break;
	case GL_TEXTURE_COMPARE_FUNC:	t->compare_func = ivalue; break;
	case GL_TEXTURE_BASE_LEVEL:
		if (ivalue != 0)
			RG_NoteUnhandled ("GL_TEXTURE_BASE_LEVEL above 0");
		return;
	case GL_TEXTURE_SWIZZLE_A:
		/* gl_renderer_caps.texture_swizzle is off under -gpu, so the
		 * loaders already bake alpha into the data, as on WebGL2. */
		if (ivalue != GL_ALPHA)
			RG_NoteUnhandled ("GL_TEXTURE_SWIZZLE_A (no swizzle in SDL_GPU)");
		return;
	default:
		RG_NoteUnhandled ("texture parameter with no SDL_GPU sampler field");
		return;
	}
	t->sampler = NULL;
}

/* ------------------------------------------------------------------ */
/* Samplers                                                            */
/* ------------------------------------------------------------------ */

static SDL_GPUSamplerAddressMode RGT_Wrap (GLint wrap)
{
	switch (wrap)
	{
	case GL_REPEAT:			return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
	case GL_MIRRORED_REPEAT:	return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
	case GL_CLAMP_TO_BORDER:
		RG_NoteUnhandled ("GL_CLAMP_TO_BORDER (sampled as clamp-to-edge)");
		return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	default:			return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	}
}

static SDL_GPUCompareOp RGT_Compare (GLint func)
{
	switch (func)
	{
	case GL_NEVER:		return SDL_GPU_COMPAREOP_NEVER;
	case GL_LESS:		return SDL_GPU_COMPAREOP_LESS;
	case GL_EQUAL:		return SDL_GPU_COMPAREOP_EQUAL;
	case GL_LEQUAL:		return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
	case GL_GREATER:	return SDL_GPU_COMPAREOP_GREATER;
	case GL_NOTEQUAL:	return SDL_GPU_COMPAREOP_NOT_EQUAL;
	case GL_GEQUAL:		return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
	default:		return SDL_GPU_COMPAREOP_ALWAYS;
	}
}

static SDL_GPUSampler *RGT_SamplerFor (const SDL_GPUSamplerCreateInfo *info)
{
	int	i;

	for (i = 0; i < rgs_count; i++)
	{
		if (!memcmp (&rgs_cache[i].info, info, sizeof(*info)))
			return rgs_cache[i].sampler;
	}
	if (rgs_count == RG_MAX_SAMPLERS)
	{
		RG_NoteUnhandled ("sampler cache full (sampled with the first sampler)");
		return rgs_cache[0].sampler;
	}
	rgs_cache[rgs_count].info = *info;
	rgs_cache[rgs_count].sampler = SDL_CreateGPUSampler (rg_dev, info);
	if (!rgs_cache[rgs_count].sampler)
	{
		Con_Printf ("SDL_GPU: sampler create failed (%s)\n", SDL_GetError ());
		return rgs_count ? rgs_cache[0].sampler : NULL;
	}
	return rgs_cache[rgs_count++].sampler;
}

/* GL's sampling rules, for the state a texture holds: a non-mipmap minify
 * filter samples the base level only; a mipmap one samples up to
 * GL_TEXTURE_MAX_LEVEL and the levels that exist. */
static SDL_GPUSampler *RGT_Sampler (rgtex_t *t)
{
	SDL_GPUSamplerCreateInfo	info;
	qboolean			mip = true;

	if (t->sampler)
		return t->sampler;
	SDL_zero (info);
	switch (t->min_filter)
	{
	case GL_NEAREST:
		info.min_filter = SDL_GPU_FILTER_NEAREST; mip = false; break;
	case GL_LINEAR:
		info.min_filter = SDL_GPU_FILTER_LINEAR; mip = false; break;
	case GL_NEAREST_MIPMAP_NEAREST:
		info.min_filter = SDL_GPU_FILTER_NEAREST;
		info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST; break;
	case GL_LINEAR_MIPMAP_NEAREST:
		info.min_filter = SDL_GPU_FILTER_LINEAR;
		info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST; break;
	case GL_NEAREST_MIPMAP_LINEAR:
		info.min_filter = SDL_GPU_FILTER_NEAREST;
		info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR; break;
	default:
		info.min_filter = SDL_GPU_FILTER_LINEAR;
		info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR; break;
	}
	info.mag_filter = (t->mag_filter == GL_NEAREST) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
	info.address_mode_u = RGT_Wrap (t->wrap_s);
	info.address_mode_v = RGT_Wrap (t->wrap_t);
	info.address_mode_w = RGT_Wrap (t->wrap_r);
	info.mip_lod_bias = t->lod_bias;
	info.min_lod = 0.0f;
	/* Only the levels GL defined: a sampled texture exists with its whole
	 * chain (RGT_CreateLevels), the rest of which nothing uploaded. */
	info.max_lod = mip ? (float) q_min (t->max_level, q_max (q_min (t->levels, t->tex_levels), 1) - 1)
			   : 0.0f;
	if (mip && t->anisotropy > 1.0f)
	{
		info.enable_anisotropy = true;
		info.max_anisotropy = t->anisotropy;
	}
	if (t->compare_mode == GL_COMPARE_REF_TO_TEXTURE)
	{
		info.enable_compare = true;
		info.compare_op = RGT_Compare (t->compare_func);
	}
	t->sampler = RGT_SamplerFor (&info);
	return t->sampler;
}

/* ------------------------------------------------------------------ */
/* Binding and drawing                                                 */
/* ------------------------------------------------------------------ */

void RG_BindTexture (GLuint unit, GLenum target, GLuint tex)
{
	rg_textype_t	type;
	int		layer;

	if (unit >= RT_MAX_TEXTURE_UNITS)
	{
		RG_NoteUnhandled ("texture unit above 15");
		return;
	}
	if (!RGT_Target (target, &type, &layer))
	{
		/* GL_TEXTURE_2D_MULTISAMPLE / CUBE_MAP_ARRAY: P6. */
		RG_NoteUnhandled ("texture bind to a target with no SDL_GPU type yet");
		return;
	}
	rgt_units[unit][type] = tex;
}

static void RGT_CreatePlaceholders (void)
{
	SDL_GPUTextureCreateInfo	ci;
	SDL_GPUSamplerCreateInfo	si;
	int				i;

	for (i = 0; i < RG_TT_COUNT; i++)
	{
		SDL_zero (ci);
		ci.type = (i == RG_TT_3D) ? SDL_GPU_TEXTURETYPE_3D :
			  (i == RG_TT_CUBE) ? SDL_GPU_TEXTURETYPE_CUBE :
			  (i == RG_TT_2D_ARRAY) ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D;
		ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ci.width = ci.height = 1;
		ci.layer_count_or_depth = (i == RG_TT_CUBE) ? 6 : 1;
		ci.num_levels = 1;
		rgt_white[i] = SDL_CreateGPUTexture (rg_dev, &ci);
		if (!rgt_white[i])
			Sys_Error ("SDL_GPU: cannot create the placeholder texture (%s)", SDL_GetError ());
	}
	SDL_zero (si);
	rgt_white_sampler = RGT_SamplerFor (&si);
	rgt_white_uploaded = false;
}

void RG_Res_ForDraw (GLuint unit, rg_textype_t type, SDL_GPUTextureSamplerBinding *out)
{
	rgtex_t	*t = (unit < RT_MAX_TEXTURE_UNITS) ? RGT_Get (rgt_units[unit][type]) : NULL;

	if (t && t->defined && t->type == type)
	{
		RGT_Realize (t);
		if (t->tex)
		{
			t->read_segment = RG_Frame_Segment ();
			t->read_ever = true;
			out->texture = t->tex;
			out->sampler = RGT_Sampler (t);
			if (out->sampler)
				return;
		}
	}
	out->texture = rgt_white[type];
	out->sampler = rgt_white_sampler;
}

/* ------------------------------------------------------------------ */
/* The copy pass                                                       */
/* ------------------------------------------------------------------ */

/* The placeholders' one texel each (2D, 3D, six cube faces, 2D array) is
 * the first upload ever staged, padded like every other to 16 bytes. */
#define RG_WHITE_BYTES	(4 * (1 + 1 + 6 + 1))
#define RG_WHITE_STAGED	((RG_WHITE_BYTES + 15) & ~15u)

Uint32 RG_Res_PendingBytes (void)
{
	/* Each upload is staged 16-byte aligned. */
	Uint32	n = rgt_white_uploaded ? 0 : RG_WHITE_STAGED;
	int	i;

	if (!rgt_pending_bytes)
		return n;
	for (i = 1; i <= rgt_used; i++)
	{
		rgupload_t	*u;

		if (!rgt[i].alive)
			continue;
		for (u = rgt[i].uploads; u; u = u->next)
			n += (u->size + 15) & ~15u;
	}
	return n;
}

static Uint32	rgt_white_staged;

void RG_Res_StageUploads (Uint8 *dst, Uint32 base)
{
	Uint32	off = 0;
	int	i;

	if (!rgt_white_uploaded)
	{
		memset (dst, 255, RG_WHITE_BYTES);
		rgt_white_staged = base;
		off = RG_WHITE_STAGED;
	}
	if (!rgt_pending_bytes)
		return;
	for (i = 1; i <= rgt_used; i++)
	{
		rgupload_t	*u;

		if (!rgt[i].alive)
			continue;
		for (u = rgt[i].uploads; u; u = u->next)
		{
			memcpy (dst + off, u->data, u->size);
			u->staged = base + off;
			off += (u->size + 15) & ~15u;
		}
	}
}

void RG_Res_IssueUploads (SDL_GPUCopyPass *copy, SDL_GPUTransferBuffer *tb)
{
	SDL_GPUTextureTransferInfo	src;
	SDL_GPUTextureRegion		dst;
	int				i, k;

	if (!rgt_white_uploaded)
	{
		Uint32	off = rgt_white_staged;

		for (i = 0; i < RG_TT_COUNT; i++)
		{
			int	layers = (i == RG_TT_CUBE) ? 6 : 1;

			for (k = 0; k < layers; k++, off += 4)
			{
				SDL_zero (src);
				src.transfer_buffer = tb;
				src.offset = off;
				SDL_zero (dst);
				dst.texture = rgt_white[i];
				dst.layer = k;
				dst.w = dst.h = dst.d = 1;
				SDL_UploadToGPUTexture (copy, &src, &dst, false);
			}
		}
		rgt_white_uploaded = true;
	}
	if (!rgt_pending_bytes)
		return;
	for (i = 1; i <= rgt_used; i++)
	{
		rgtex_t		*t = &rgt[i];
		rgupload_t	*u;

		if (!t->alive || !t->uploads)
			continue;
		RGT_Realize (t);
		for (u = t->uploads; t->tex && u; u = u->next)
		{
			if ((int)u->level >= t->tex_levels)
				continue;	/* beyond what the chain can hold */
			SDL_zero (src);
			src.transfer_buffer = tb;
			src.offset = u->staged;
			src.pixels_per_row = u->w;
			src.rows_per_layer = u->h;
			SDL_zero (dst);
			dst.texture = t->tex;
			dst.mip_level = u->level;
			dst.layer = u->layer;
			dst.x = u->x; dst.y = u->y; dst.z = u->z;
			dst.w = u->w; dst.h = u->h; dst.d = u->d;
			SDL_UploadToGPUTexture (copy, &src, &dst, false);
		}
		RGT_DropUploads (t);
	}
}

void RG_Res_EndFrame (void)
{
	int	i;

	for (i = 0; i < rgt_numretired; i++)
		SDL_ReleaseGPUTexture (rg_dev, rgt_retired[i]);
	rgt_numretired = 0;
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

void RG_Res_Init (void)
{
	memset (rgt_units, 0, sizeof(rgt_units));
	rgs_count = 0;
	RGT_CreatePlaceholders ();
}

void RG_Res_Shutdown (void)
{
	int	i;

	if (!rg_dev)
		return;
	for (i = 1; i <= rgt_used; i++)
	{
		if (!rgt[i].alive)
			continue;
		RGT_DropUploads (&rgt[i]);
		if (rgt[i].tex)
			SDL_ReleaseGPUTexture (rg_dev, rgt[i].tex);
	}
	RG_Res_EndFrame ();
	for (i = 0; i < RG_TT_COUNT; i++)
	{
		if (rgt_white[i])
			SDL_ReleaseGPUTexture (rg_dev, rgt_white[i]);
		rgt_white[i] = NULL;
	}
	for (i = 0; i < rgs_count; i++)
	{
		if (rgs_cache[i].sampler)
			SDL_ReleaseGPUSampler (rg_dev, rgs_cache[i].sampler);
	}
	rgs_count = 0;
	free (rgt); rgt = NULL;
	free (rgt_free); rgt_free = NULL;
	free (rgt_retired); rgt_retired = NULL;
	rgt_count = rgt_used = rgt_numfree = rgt_maxfree = rgt_numretired = rgt_maxretired = 0;
	rgt_pending_bytes = 0;
}

void RG_Res_Status (void)
{
	int	i, alive = 0, made = 0;

	for (i = 1; i <= rgt_used; i++)
	{
		alive += rgt[i].alive;
		made += (rgt[i].alive && rgt[i].tex);
	}
	Con_Printf ("[RENDERER] SDL_GPU textures: %d names, %d created (%lu creates, %lu re-creates), "
		    "%d samplers, %lu uploads / %lu KB, %u bytes queued\n",
		    alive, made, rgt_created, rgt_recreated, rgs_count,
		    rgt_uploads, rgt_upload_bytes / 1024, (unsigned int)rgt_pending_bytes);
}
