/*
 * img_load.h - External image loading interface
 */

#ifndef __IMG_LOAD_H
#define __IMG_LOAD_H

#include "quakedef.h"

/* Upper bound on mip levels carried by a block-compressed container.
 * 16 covers 32768x32768, far past anything gl_max_size will accept. */
#define IMG_MAX_MIPS	16

/* Extension-less paths probed for one texture name: the per-map override
 * directory, then the shared textures/ pool. */
#define IMG_MAX_CANDIDATES	2

/*
 * A replacement image resolved from disk.  Exactly one of rgba / blocks is
 * non-NULL: rgba for the PNG/TGA/PCX path, blocks for a pre-compressed DDS or
 * KTX container whose block payload is handed to the driver untouched.
 *
 * Free with IMG_FreeReplacement(), never free() the members directly.
 */
typedef struct imgreplace_s
{
	int		width, height;		/* dimensions of mip 0 */
	qboolean	has_alpha;

	byte		*rgba;			/* width*height*4, or NULL */

	byte		*blocks;		/* all mips back to back, or NULL */
	unsigned int	glformat;		/* GL internal format for blocks */
	int		nummips;		/* levels present in blocks */
	int		mipw[IMG_MAX_MIPS];
	int		miph[IMG_MAX_MIPS];
	size_t		mipofs[IMG_MAX_MIPS];	/* offset into blocks */
	size_t		mipsize[IMG_MAX_MIPS];
} imgreplace_t;

/*
 * Resolve a replacement image for a texture name, preferring a compressed
 * container over a decoded one and a per-map override over the shared pool.
 * modelname is the owning model's path (e.g. "maps/demo1.bsp") and may be
 * NULL; it only selects the per-map search directory.
 *
 * Pass allow_compressed = false for a texture the engine will alpha-test.
 * BC1/BC3/BC7 interpolate alpha inside each 4x4 block, so a cutout mask comes
 * back out of the codec with intermediate values along every edge and the
 * alpha test cuts a ragged line through them -- the fringing Ironwail 1011ff8
 * and 74d8e74 avoid by never compressing these.  The engine cannot re-encode
 * what the pack shipped, so the only lever is to decline the container and
 * take the decoded image instead; that decision has to happen here, because
 * this is the last point where the decoded alternative is still reachable.
 * A '{' fence name is recognized without being told.  uhexen2-r7zu.
 *
 * Returns false and leaves *out untouched when nothing is on disk.
 */
qboolean IMG_LoadReplacement (const char *name, const char *modelname,
			      qboolean allow_compressed, imgreplace_t *out);

/*
 * True when a decoded replacement carries an authored translucency ramp rather
 * than a cutout mask: most texels partially transparent and essentially none
 * fully opaque.  Lets an EF_HOLEY model whose replacement skin is really soft
 * escape the binarize-and-alpha-test cutout path.  False for compressed
 * containers, which have no texels to count yet.  uhexen2-5zv5.
 */
qboolean IMG_ReplacementHasSoftAlpha (const imgreplace_t *r);

/* Same, for the fullbright/glow sidecar of a texture: <name>_glow, then
 * <name>_luma.  Kept separate so callers do not have to build the suffix. */
qboolean IMG_LoadReplacementGlow (const char *name, const char *modelname, imgreplace_t *out);

void IMG_FreeReplacement (imgreplace_t *r);

/* Load external texture with automatic format detection (PNG/TGA/PCX only).
 * Returns allocated RGBA buffer, caller must free().
 * Retained for the 2D/HUD and particle callers, which never want a
 * block-compressed source. */
byte *IMG_LoadExternalTexture (const char *name, int *width, int *height, qboolean *has_alpha);

// Individual format loaders (also available for direct use)
byte *IMG_LoadPCX (const char *filename, int *width, int *height);
byte *IMG_LoadTGA (const char *filename, int *width, int *height, int *has_alpha);
byte *IMG_LoadPNG (const char *filename, int *width, int *height, int *has_alpha);

/* Block-compressed container loaders (img_dds.c).  Both return false unless
 * the file exists, parses, and uses a format this GL context supports. */
qboolean IMG_LoadDDS (const char *filename, imgreplace_t *out);
qboolean IMG_LoadKTX (const char *filename, imgreplace_t *out);

#endif /* __IMG_LOAD_H */
