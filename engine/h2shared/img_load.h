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
 * Returns false and leaves *out untouched when nothing is on disk.
 */
qboolean IMG_LoadReplacement (const char *name, const char *modelname, imgreplace_t *out);

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
