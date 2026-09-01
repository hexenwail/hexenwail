/*
 * img_load.c - External image loading (PCX, TGA, PNG)
 *
 * Supports loading external textures to override internal BSP textures.
 * Decoded formats live here (.pcx, .tga, .png); the block-compressed
 * containers (.dds, .ktx) are parsed in img_dds.c and preferred over these
 * when both are present.  Search order for a world texture is
 *
 *     textures/<mapname>/<texname>.{dds,ktx,png,tga,pcx}
 *     textures/<texname>.{dds,ktx,png,tga,pcx}
 *
 * Copyright (C) 2025 uHexen2 project
 */

#include "quakedef.h"
#include "img_load.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/*
=================
IMG_LoadPCX

Load an 8-bit PCX file and convert to 32-bit RGBA.
Palette index 255 is treated as transparent (alpha = 0).
Returns allocated buffer, caller must free.
=================
*/
byte *IMG_LoadPCX (const char *filename, int *width, int *height)
{
	fshandle_t	fh;
	byte	*rawdata;
	byte	*palette;
	byte	*rgba;
	int		x, y, i;
	int		x_min, y_min, x_max, y_max;
	int		size, bytes_per_scanline;
	byte	pixel;
	int	run_length;
	int		src, dst;

	size = FS_OpenFileHandle_Silent (filename, &fh, NULL);
	if (size < 0)
		return NULL;

	if (size < 128 + 769)	// minimum: header + 1 pixel + palette
	{
		FS_fclose(&fh);
		return NULL;
	}

	rawdata = (byte *) malloc (size);
	if (!rawdata)
	{
		FS_fclose(&fh);
		return NULL;
	}

	FS_fread(rawdata, 1, size, &fh);
	FS_fclose(&fh);

	// Verify PCX header
	if (rawdata[0] != 0x0A)
	{
		free (rawdata);
		return NULL;
	}

	// Get dimensions
	x_min = rawdata[4] | (rawdata[5] << 8);
	y_min = rawdata[6] | (rawdata[7] << 8);
	x_max = rawdata[8] | (rawdata[9] << 8);
	y_max = rawdata[10] | (rawdata[11] << 8);

	*width = x_max - x_min + 1;
	*height = y_max - y_min + 1;
	bytes_per_scanline = rawdata[66] | (rawdata[67] << 8);

	if (*width <= 0 || *height <= 0 || *width > 4096 || *height > 4096)
	{
		free (rawdata);
		return NULL;
	}

	// Find palette (256-color VGA palette is at end-768, preceded by 0x0C)
	palette = NULL;
	if (size >= 769)
	{
		if (rawdata[size - 769] == 0x0C)
			palette = &rawdata[size - 768];
		else if (size > 769 && rawdata[size - 768] == 0x0C)
			palette = &rawdata[size - 767];
	}

	// If no embedded palette, use the game's palette
	if (!palette)
		palette = (byte *)d_8to24table;	// Use the global palette table

	// Allocate output buffer
	rgba = (byte *) malloc (*width * *height * 4);
	if (!rgba)
	{
		free (rawdata);
		return NULL;
	}

	// Decode RLE image data
	src = 128;	// PCX data starts after 128-byte header

	for (y = 0; y < *height; y++)
	{
		int	x = 0;
		int	scan_start = src;

		while (x < bytes_per_scanline && src < size - 768)
		{
			pixel = rawdata[src++];

			if ((pixel & 0xC0) == 0xC0)	// RLE run
			{
				run_length = pixel & 0x3F;
				if (src >= size - 768)
					break;
				pixel = rawdata[src++];

				for (i = 0; i < run_length && x < *width; i++)
				{
					int dst_offset = (y * *width + x) * 4;
					if (pixel == 255 && palette != (byte *)d_8to24table)
					{
						// Transparent pixel (index 255)
						rgba[dst_offset + 0] = 0;
						rgba[dst_offset + 1] = 0;
						rgba[dst_offset + 2] = 0;
						rgba[dst_offset + 3] = 0;
					}
					else
					{
						// Use palette color
						unsigned int palcolor;
						if (palette == (byte *)d_8to24table)
							palcolor = d_8to24table[pixel];
						else
						{
							// Embedded palette (RGB bytes)
							int pal_idx = pixel * 3;
							if (pal_idx + 2 < 768)
							{
								rgba[dst_offset + 0] = palette[pal_idx + 0];
								rgba[dst_offset + 1] = palette[pal_idx + 1];
								rgba[dst_offset + 2] = palette[pal_idx + 2];
								rgba[dst_offset + 3] = (pixel == 255) ? 0 : 255;
								x++;
								continue;
							}
							else
								palcolor = d_8to24table[pixel];
						}
						rgba[dst_offset + 0] = (palcolor >> 0) & 0xFF;
						rgba[dst_offset + 1] = (palcolor >> 8) & 0xFF;
						rgba[dst_offset + 2] = (palcolor >> 16) & 0xFF;
						rgba[dst_offset + 3] = (pixel == 255) ? 0 : 255;
					}
					x++;
				}
			}
			else	// Literal pixel
			{
				if (x < *width)
				{
					int dst_offset = (y * *width + x) * 4;
					if (pixel == 255 && palette != (byte *)d_8to24table)
					{
						rgba[dst_offset + 0] = 0;
						rgba[dst_offset + 1] = 0;
						rgba[dst_offset + 2] = 0;
						rgba[dst_offset + 3] = 0;
					}
					else
					{
						unsigned int palcolor;
						if (palette == (byte *)d_8to24table)
							palcolor = d_8to24table[pixel];
						else
						{
							int pal_idx = pixel * 3;
							if (pal_idx + 2 < 768)
							{
								rgba[dst_offset + 0] = palette[pal_idx + 0];
								rgba[dst_offset + 1] = palette[pal_idx + 1];
								rgba[dst_offset + 2] = palette[pal_idx + 2];
								rgba[dst_offset + 3] = (pixel == 255) ? 0 : 255;
								x++;
								continue;
							}
							else
								palcolor = d_8to24table[pixel];
						}
						rgba[dst_offset + 0] = (palcolor >> 0) & 0xFF;
						rgba[dst_offset + 1] = (palcolor >> 8) & 0xFF;
						rgba[dst_offset + 2] = (palcolor >> 16) & 0xFF;
						rgba[dst_offset + 3] = (pixel == 255) ? 0 : 255;
					}
				}
				x++;
			}
		}

		// Skip to next scanline if needed
		while (x < bytes_per_scanline && src < size - 768)
		{
			pixel = rawdata[src++];
			if ((pixel & 0xC0) == 0xC0)
			{
				src++;	// skip the value byte
				x += (pixel & 0x3F);
			}
			else
				x++;
		}
	}

	free (rawdata);
	return rgba;
}

/*
=================
IMG_LoadTGA

Load a TGA file (24-bit RGB or 32-bit RGBA).
Returns allocated buffer, caller must free.
=================
*/
byte *IMG_LoadTGA (const char *filename, int *width, int *height, int *has_alpha)
{
	fshandle_t	fh;
	byte	*data;
	byte	*rgba = NULL;
	int		x, y, i;
	int		id_len, cmap_type, image_type;
	int		cmap_first, cmap_len, cmap_entry_size;
	int		origin_x, origin_y, img_w, img_h;
	int		bpp, descriptor;
	int		pixel_size;
	int		flip_vert;
	long	size;

	size = FS_OpenFileHandle_Silent (filename, &fh, NULL);
	if (size < 0)
		return NULL;

	if (size < 18)
	{
		FS_fclose(&fh);
		return NULL;
	}

	// Read TGA header
	id_len = FS_fgetc(&fh);
	cmap_type = FS_fgetc(&fh);
	image_type = FS_fgetc(&fh);

	// Skip colormap spec (5 bytes) and origin (4 bytes) = 9 bytes total
	FS_fseek(&fh, 9, SEEK_CUR);

	// Now at byte 12: width, height, bpp, descriptor
	img_w = FS_fgetc(&fh); img_w |= FS_fgetc(&fh) << 8;
	img_h = FS_fgetc(&fh); img_h |= FS_fgetc(&fh) << 8;
	bpp = FS_fgetc(&fh);
	descriptor = FS_fgetc(&fh);

	if (img_w <= 0 || img_h <= 0 || img_w > 4096 || img_h > 4096)
	{
		FS_fclose(&fh);
		return NULL;
	}

	*width = img_w;
	*height = img_h;

	// Check image type - support uncompressed RGB/RGBA
	if (image_type != 2)	// 2 = uncompressed RGB
	{
		FS_fclose(&fh);
		return NULL;
	}

	if (bpp != 24 && bpp != 32)
	{
		FS_fclose(&fh);
		return NULL;
	}

	pixel_size = bpp / 8;
	flip_vert = !(descriptor & 0x20);	// bit 5 = top-to-bottom

	data = (byte *) malloc (img_w * img_h * pixel_size);
	if (!data)
	{
		FS_fclose(&fh);
		return NULL;
	}

	// Skip image ID and colormap if present
	if (id_len > 0)
		FS_fseek(&fh, id_len, SEEK_CUR);

	FS_fread(data, 1, img_w * img_h * pixel_size, &fh);
	FS_fclose(&fh);

	// Convert to RGBA
	rgba = (byte *) malloc (img_w * img_h * 4);
	if (!rgba)
	{
		free (data);
		return NULL;
	}

	*has_alpha = (bpp == 32);

	for (y = 0; y < img_h; y++)
	{
		int	src_y = flip_vert ? (img_h - 1 - y) : y;

		for (x = 0; x < img_w; x++)
		{
			int	src_offset = (src_y * img_w + x) * pixel_size;
			int	dst_offset = (y * img_w + x) * 4;

			rgba[dst_offset + 0] = data[src_offset + 2];	// BGR -> RGB
			rgba[dst_offset + 1] = data[src_offset + 1];
			rgba[dst_offset + 2] = data[src_offset + 0];
			rgba[dst_offset + 3] = (bpp == 32) ? data[src_offset + 3] : 255;
		}
	}

	free (data);
	return rgba;
}

/*
=================
IMG_LoadPNG

Load a PNG file using stb_image.
Returns allocated buffer, caller must free.
=================
*/
byte *IMG_LoadPNG (const char *filename, int *width, int *height, int *has_alpha)
{
	int		channels;
	byte	*rgba;
	fshandle_t	fh;
	byte	*file_data;
	long	size;

	size = FS_OpenFileHandle_Silent (filename, &fh, NULL);
	if (size < 0)
		return NULL;

	file_data = (byte *) malloc (size);
	if (!file_data)
	{
		FS_fclose(&fh);
		return NULL;
	}

	FS_fread(file_data, 1, size, &fh);
	FS_fclose(&fh);

	rgba = stbi_load_from_memory (file_data, size, width, height, &channels, 4);
	free (file_data);

	if (!rgba)
		return NULL;

	*has_alpha = (channels == 4 || channels == 2);
	return rgba;
}

/*
=================
IMG_SafeName

Sanitize a texture name for filesystem use: '*' marks liquid textures in the
BSP (e.g. *lowlight) but is not a legal Windows filename character, so it
becomes '#' per Quake convention.
=================
*/
static void IMG_SafeName (char *dst, size_t dstsize, const char *name)
{
	int	i;

	q_strlcpy (dst, name, dstsize);
	for (i = 0; dst[i]; i++)
	{
		if (dst[i] == '*')
			dst[i] = '#';
	}
}

/*
=================
IMG_MapBaseName

Reduce a model path ("maps/demo1.bsp") to the bare map name ("demo1") used as
the per-map override directory.  Returns false when there is nothing usable,
in which case the caller simply skips the per-map candidate.
=================
*/
static qboolean IMG_MapBaseName (char *dst, size_t dstsize, const char *modelname)
{
	const char	*slash, *dot;
	size_t		len;

	if (!modelname || !*modelname)
		return false;

	slash = strrchr (modelname, '/');
	slash = slash ? slash + 1 : modelname;

	dot = strrchr (slash, '.');
	len = dot ? (size_t)(dot - slash) : strlen (slash);

	if (len < 1 || len >= dstsize)
		return false;

	memcpy (dst, slash, len);
	dst[len] = '\0';
	return true;
}

/*
=================
IMG_BuildCandidates

Fill in the extension-less paths to probe for a replacement, in search order,
and return how many there are.

Names that already carry their own directory (models/, gfx/, particles/) are
used as-is -- they are full paths from the game directory and were never part
of the flat textures/ pool.  Everything else is a world texture out of the BSP
miptex lump, which gets the per-map directory first so two maps can disagree
about what "wall01" looks like, then the shared pool.
=================
*/
static int IMG_BuildCandidates (const char *name, const char *modelname,
				char paths[IMG_MAX_CANDIDATES][MAX_OSPATH])
{
	char	safename[MAX_QPATH];
	char	mapname[MAX_QPATH];
	int	n = 0;

	IMG_SafeName (safename, sizeof(safename), name);

	if (!strncmp (safename, "models/", 7) ||
	    !strncmp (safename, "gfx/", 4) ||
	    !strncmp (safename, "particles/", 10))
	{
		q_snprintf (paths[n++], MAX_OSPATH, "%s", safename);
		return n;
	}

	if (IMG_MapBaseName (mapname, sizeof(mapname), modelname))
		q_snprintf (paths[n++], MAX_OSPATH, "textures/%s/%s", mapname, safename);

	q_snprintf (paths[n++], MAX_OSPATH, "textures/%s", safename);

	return n;
}

/*
=================
IMG_TryDecoded

Probe one base path for a decoded image, newest format first.

pathout receives the file that actually won.  The base path alone does not
say which extension resolved, and that filename is the whole point of the
diagnostic in IMG_LogReplacement -- so it is threaded back out rather than
logged here, where the caller may still reject the hit.
=================
*/
static byte *IMG_TryDecoded (const char *base, int *width, int *height, qboolean *has_alpha,
			     char *pathout, size_t pathoutsize)
{
	char	path[MAX_OSPATH];
	byte	*data;
	int	alpha;

	q_snprintf (path, sizeof(path), "%s.png", base);
	data = IMG_LoadPNG (path, width, height, &alpha);
	if (data)
	{
		*has_alpha = alpha ? true : false;
		q_strlcpy (pathout, path, pathoutsize);
		return data;
	}

	q_snprintf (path, sizeof(path), "%s.tga", base);
	data = IMG_LoadTGA (path, width, height, &alpha);
	if (data)
	{
		*has_alpha = alpha ? true : false;
		q_strlcpy (pathout, path, pathoutsize);
		return data;
	}

	q_snprintf (path, sizeof(path), "%s.pcx", base);
	data = IMG_LoadPCX (path, width, height);
	if (data)
	{
		*has_alpha = true;	/* PCX uses index 255 for transparency */
		q_strlcpy (pathout, path, pathoutsize);
		return data;
	}

	return NULL;
}

/*
=================
IMG_CompressedFits

Whether any level of the chain is small enough for this GPU to accept.

Block data cannot be resampled the way GL_Upload32 rescales RGBA, so the only
way to fit an oversized compressed texture is to start further down its mip
chain.  A container that ships one huge level and no mips has nowhere to go:
uploading it would fail with GL_INVALID_VALUE and leave the surface black.
Rejecting it here instead lets resolution carry on to the decoded image in the
same directory, which can be rescaled.
=================
*/
static qboolean IMG_CompressedFits (const imgreplace_t *r)
{
	int	i;

	for (i = 0; i < r->nummips; i++)
	{
		if (r->mipw[i] <= gl_max_size && r->miph[i] <= gl_max_size)
			return true;
	}

	return false;
}

/*
=================
IMG_TryCompressed

Probe one base path for a block-compressed container.

pathout receives the file that won, for the same reason as IMG_TryDecoded.
It is written only on success -- a container rejected for size is not the
texture the surface ended up with.
=================
*/
static qboolean IMG_TryCompressed (const char *base, imgreplace_t *out,
				   char *pathout, size_t pathoutsize)
{
	char	path[MAX_OSPATH];

	q_snprintf (path, sizeof(path), "%s.dds", base);
	if (IMG_LoadDDS (path, out))
	{
		if (IMG_CompressedFits (out))
		{
			q_strlcpy (pathout, path, pathoutsize);
			return true;
		}
		Con_DPrintf ("%s: no mip level fits %d texels, ignoring\n",
			     path, (int) gl_max_size);
		IMG_FreeReplacement (out);
	}

	q_snprintf (path, sizeof(path), "%s.ktx", base);
	if (IMG_LoadKTX (path, out))
	{
		if (IMG_CompressedFits (out))
		{
			q_strlcpy (pathout, path, pathoutsize);
			return true;
		}
		Con_DPrintf ("%s: no mip level fits %d texels, ignoring\n",
			     path, (int) gl_max_size);
		IMG_FreeReplacement (out);
	}

	return false;
}

/*
=================
IMG_LogReplacement

Name the file a texture actually came from.

An external image silently displaces whatever the BSP or MDL embedded, and
nothing downstream can say which file won -- so a bad pack presents exactly
like a renderer bug.  uhexen2-nqwl spent a full RenderDoc capture teardown
proving that a black slime surface was a near-black #lowlight6.tga sitting in
the game's own pack; this one line would have named it in seconds.

Con_DPrintf, so an ordinary session stays silent and a field tester can be
asked for a `developer 1` log instead of a capture.  Deliberately reports
only -- rejecting a replacement that "looks wrong" would be a heuristic
overriding legitimate authoring, and dark or low-res replacements are both
legal.  uhexen2-0q4f.
=================
*/
static void IMG_LogReplacement (const char *name, const char *path,
				int width, int height, qboolean compressed)
{
	Con_DPrintf ("IMG: \"%s\" replaced by %s (%dx%d%s)\n",
		     name, path, width, height, compressed ? ", compressed" : "");
}

/*
=================
IMG_LoadReplacement

Resolve a replacement image, preferring a per-map override over the shared
pool and, within one directory, a compressed container over a decoded one.

The ordering is deliberate: a pack that ships both textures/e1m1/wall01.png
and textures/wall01.dds means the first to win for e1m1, because per-map is
the more specific statement.  Only when two files sit in the *same* directory
does format preference decide, and there the pre-compressed one is strictly
better -- no CPU decode, a quarter of the VRAM, and its own mip chain.
=================
*/
qboolean IMG_LoadReplacement (const char *name, const char *modelname,
			      qboolean allow_compressed, imgreplace_t *out)
{
	char		paths[IMG_MAX_CANDIDATES][MAX_OSPATH];
	char		winner[MAX_OSPATH];
	int		n, i;
	qboolean	is_fence = (name[0] == '{');

	memset (out, 0, sizeof(*out));

	/* A '{' name is alpha-tested by definition, so it never wants a
	 * block-compressed source whatever the caller passed.  uhexen2-r7zu. */
	if (is_fence)
		allow_compressed = false;

	/* This gate was r_texture_external, which used to cover only the Quake II
	 * .wal loader that h2config.h #undefs -- a dead cvar whose name promised
	 * exactly the feature people ask about, defaulting to 0.  uhexen2-dbnh
	 * gave it teeth and raised the default, but it was CVAR_ARCHIVE, so every
	 * config.cfg already holding the old meaningless 0 silently lost every
	 * replacement texture in that gamedir.  Renamed here, because an archived
	 * value cannot be told apart from a deliberate one.  uhexen2-yz1b */
	if (!r_external_textures.integer)
	{
		/* Say so, once.  The predecessor of this cvar spent seven months
		 * silently refusing every replacement pack on installs whose
		 * config.cfg had archived a then-meaningless 0, and the absence of
		 * any message is what made that expensive to diagnose rather than
		 * the value itself.  uhexen2-yz1b */
		static qboolean	told = false;

		if (!told)
		{
			told = true;
			Con_Printf ("r_external_textures is 0: replacement textures are disabled,\n"
				    "  so models and world textures will use their embedded art.\n"
				    "  Set r_external_textures 1 to load an installed HD pack.\n");
		}
		return false;
	}

	n = IMG_BuildCandidates (name, modelname, paths);

	for (i = 0; i < n; i++)
	{
		if (allow_compressed && IMG_TryCompressed (paths[i], out, winner, sizeof(winner)))
		{
			if (is_fence)
				out->has_alpha = true;
			IMG_LogReplacement (name, winner, out->width, out->height, true);
			return true;
		}

		out->rgba = IMG_TryDecoded (paths[i], &out->width, &out->height, &out->has_alpha,
					    winner, sizeof(winner));
		if (out->rgba)
		{
			if (is_fence)
				out->has_alpha = true;
			IMG_LogReplacement (name, winner, out->width, out->height, false);
			return true;
		}
	}

	memset (out, 0, sizeof(*out));
	return false;
}

/*
=================
IMG_ReplacementHasSoftAlpha

True when a decoded replacement carries an authored translucency ramp rather
than a cutout mask.

The test is deliberately narrow, because getting it wrong in the permissive
direction turns a cutout into a blended surface and changes how it sorts.  A
cutout -- foliage, a grate, a fence -- is mostly solid interior with a thin
antialiased rim, so it has a large population at alpha 255.  An authored
translucent skin has the opposite shape: most texels partially transparent and
essentially nothing fully opaque.  Requiring both conditions separates them
with a wide margin rather than a tuned cutoff.  SoT's mist reads 85% partial /
0% opaque; a cutout reads a few percent partial against a solid core.

Compressed containers are not inspected -- there are no texels to count until
the codec runs -- so those keep the cutout treatment they had.  In practice the
caller has already declined compression for anything it means to alpha-test.
uhexen2-5zv5.
=================
*/
qboolean IMG_ReplacementHasSoftAlpha (const imgreplace_t *r)
{
	int	i, n, partial = 0, opaque = 0;

	if (!r || !r->rgba || !r->has_alpha)
		return false;

	n = r->width * r->height;
	if (n <= 0)
		return false;

	for (i = 0; i < n; i++)
	{
		byte a = r->rgba[i*4 + 3];
		if (a == 255)
			opaque++;
		else if (a != 0)
			partial++;
	}

	return (partial * 10 >= n) && (opaque * 20 <= n);
}

/*
=================
IMG_LoadReplacementGlow

Resolve the fullbright sidecar for a texture.

The engine's own fullbright masks come from palette indices >= vid.fullbright,
which an RGBA replacement no longer has -- so once a texture is replaced, its
glow can only come from a companion file.  Without this, dropping a HD pack in
puts out every torch, rune and monster eye in the game (uhexen2-0vgo.1).

_glow is our name; _luma is what FTE and jsHexen2 packs ship.  Both are
accepted so an existing pack works unmodified.
=================
*/
qboolean IMG_LoadReplacementGlow (const char *name, const char *modelname, imgreplace_t *out)
{
	char	buf[MAX_QPATH];

	/* Compression stays allowed on a glow sidecar even when its parent is
	 * alpha-tested: the mask is sampled for an ADDITIVE pass, never for an
	 * alpha test, so a block-interpolated edge dims a fringe rather than
	 * punching a ragged hole in the silhouette.  uhexen2-r7zu. */
	q_snprintf (buf, sizeof(buf), "%s_glow", name);
	if (IMG_LoadReplacement (buf, modelname, true, out))
		return true;

	q_snprintf (buf, sizeof(buf), "%s_luma", name);
	return IMG_LoadReplacement (buf, modelname, true, out);
}

/*
=================
IMG_HeightToNormal

Convert a decoded greyscale height map in place into a tangent-space normal
map, keeping the height in the alpha channel.

This exists because _bump ships a HEIGHT field, not a normal, and the shader
wants a normal.  DarkPlaces does the same conversion at load
(Mod_LoadTextures -> the "bumpmap" branch) rather than asking pack authors to
pre-convert, and packs in the wild rely on that -- a _bump-only pack is common
because height maps are what a painter naturally produces.

Central-difference rather than a full Sobel: the 3x3 Sobel's diagonal taps buy
nothing on a field that is already a low-frequency height, and the 4-tap form
keeps this O(n) with two texel reads per axis.  Wrap-around addressing, because
a world texture tiles -- clamping would put a seam of flat normal down two
edges of every wall.

The alpha channel keeps the original height.  DarkPlaces documents that
convention (darkplaces.txt:212-235: "_norm ... alpha channel may carry bumpmap
height for offset/relief mapping"), so preserving it here means a later offset
or relief mapping pass needs no second sidecar, and a real _norm file that
already carries height in alpha takes the same path.
=================
*/
/* Height-to-normal gradient gain.  A central difference over adjacent texels
 * gets weaker as a height map gets bigger -- the same relief spread over more
 * texels means a smaller step between neighbours -- so the raw slope alone
 * produces almost-flat normals on the 512px maps packs actually ship.
 * DarkPlaces solves it with r_shadow_bumpscale_bumpmap, default 4; matching
 * that number is what makes a DP-authored _bump look here the way its author
 * saw it.  Relief strength stays adjustable at runtime through
 * r_normalmap_intensity, which scales the decoded normal in the shader --
 * doing it there rather than here means the knob is live instead of needing
 * a map reload to re-convert. */
#define IMG_BUMPSCALE	4.0f

static void IMG_HeightToNormal (byte *rgba, int width, int height, float scale)
{
	byte	*out;
	int	x, y;

	if (width < 2 || height < 2)
		return;		/* no gradient to take */

	out = (byte *) malloc ((size_t)width * height * 4);
	if (!out)
		return;		/* leave the height map alone rather than half-convert */

	for (y = 0; y < height; y++)
	{
		int	yn = (y - 1 + height) % height;
		int	yp = (y + 1) % height;

		for (x = 0; x < width; x++)
		{
			int	xn = (x - 1 + width) % width;
			int	xp = (x + 1) % width;
			const byte *l = rgba + (((size_t)y  * width + xn) * 4);
			const byte *r = rgba + (((size_t)y  * width + xp) * 4);
			const byte *u = rgba + (((size_t)yn * width + x ) * 4);
			const byte *d = rgba + (((size_t)yp * width + x ) * 4);
			const byte *c = rgba + (((size_t)y  * width + x ) * 4);
			byte	*o = out + (((size_t)y * width + x) * 4);
			float	hl, hr, hu, hd, nx, ny, nz, inv;

			/* Rec.601 luma.  A height map is normally already grey,
			 * but a pack that saved one as a colour PNG must not
			 * come out with a different relief per channel. */
			hl = (0.299f*l[0] + 0.587f*l[1] + 0.114f*l[2]) * (1.0f/255.0f);
			hr = (0.299f*r[0] + 0.587f*r[1] + 0.114f*r[2]) * (1.0f/255.0f);
			hu = (0.299f*u[0] + 0.587f*u[1] + 0.114f*u[2]) * (1.0f/255.0f);
			hd = (0.299f*d[0] + 0.587f*d[1] + 0.114f*d[2]) * (1.0f/255.0f);

			/* Gradient points downhill, so the surface normal takes
			 * its negation in X and Y.  scale sets how pronounced
			 * the relief is; 1.0 keeps the raw slope. */
			nx = -(hr - hl) * scale;
			ny = -(hd - hu) * scale;
			nz = 1.0f;

			inv = 1.0f / sqrt (nx*nx + ny*ny + nz*nz);
			nx *= inv; ny *= inv; nz *= inv;

			/* Encode [-1,1] -> [0,255]. */
			o[0] = (byte) q_max (0, q_min (255, (int)((nx * 0.5f + 0.5f) * 255.0f + 0.5f)));
			o[1] = (byte) q_max (0, q_min (255, (int)((ny * 0.5f + 0.5f) * 255.0f + 0.5f)));
			o[2] = (byte) q_max (0, q_min (255, (int)((nz * 0.5f + 0.5f) * 255.0f + 0.5f)));
			o[3] = (byte) q_max (0, q_min (255, (int)((0.299f*c[0] + 0.587f*c[1] + 0.114f*c[2]) + 0.5f)));
		}
	}

	memcpy (rgba, out, (size_t)width * height * 4);
	free (out);
}

/*
=================
IMG_LoadReplacementNormal

Resolve the normal-map sidecar for a texture: <name>_norm, else <name>_bump
converted from height.

The precedence is not ours to choose.  DarkPlaces documents it
(darkplaces.txt:212-235) and FTE follows it: _norm is a real normal map and
wins outright; _bump is a height map and is "not loaded if a normal map is
present".  Every pack in the wild was authored against that rule, so probing
_bump first -- or merging the two -- would render existing packs wrong.

_norm takes the compressed path.  Block compression of a normal map is
lossier than of a diffuse (the codec interpolates X and Y independently and
the result is no longer unit length), but the shader renormalises, and
declining a DDS the pack deliberately shipped would cost four times the VRAM
to second-guess the author.  _bump cannot: the height-to-normal conversion
needs texels, and a block payload has none until the driver has it.
=================
*/
qboolean IMG_LoadReplacementNormal (const char *name, const char *modelname, imgreplace_t *out)
{
	char	buf[MAX_QPATH];

	q_snprintf (buf, sizeof(buf), "%s_norm", name);
	if (IMG_LoadReplacement (buf, modelname, true, out))
		return true;

	q_snprintf (buf, sizeof(buf), "%s_bump", name);
	if (!IMG_LoadReplacement (buf, modelname, false, out))
		return false;

	/* allow_compressed was false, so this is decoded RGBA by construction.
	 * Assert the invariant rather than trusting it: a future change to
	 * IMG_LoadReplacement that returned blocks here would otherwise walk
	 * a NULL rgba. */
	if (!out->rgba)
	{
		IMG_FreeReplacement (out);
		return false;
	}

	IMG_HeightToNormal (out->rgba, out->width, out->height, IMG_BUMPSCALE);
	out->has_alpha = true;		/* alpha now carries height */
	return true;
}

/*
=================
IMG_LoadReplacementGloss

Resolve the specular sidecar: <name>_gloss.  One suffix, no alias -- DarkPlaces
(model_shared.c:2459, darkplaces.txt:264) and FTE both spell it this way and
nothing else is in circulation.

Compression is allowed.  A gloss map is a low-frequency mask feeding a
multiply, which is the case block compression damages least.
=================
*/
qboolean IMG_LoadReplacementGloss (const char *name, const char *modelname, imgreplace_t *out)
{
	char	buf[MAX_QPATH];

	q_snprintf (buf, sizeof(buf), "%s_gloss", name);
	return IMG_LoadReplacement (buf, modelname, true, out);
}

/*
=================
IMG_FreeReplacement
=================
*/
void IMG_FreeReplacement (imgreplace_t *r)
{
	if (!r)
		return;

	if (r->rgba)
		free (r->rgba);
	if (r->blocks)
		free (r->blocks);

	memset (r, 0, sizeof(*r));
}

/*
=================
IMG_LoadExternalTexture

Decoded-only replacement lookup, for callers that cannot use a compressed
source: the 2D/HUD pics and the particle sprite, which are uploaded without
mipmaps and are exactly the surfaces block compression damages most.

Returns allocated RGBA buffer, caller must free after uploading to GL.
=================
*/
byte *IMG_LoadExternalTexture (const char *name, int *width, int *height, qboolean *has_alpha)
{
	char	paths[IMG_MAX_CANDIDATES][MAX_OSPATH];
	char	winner[MAX_OSPATH];
	int	n, i;
	byte	*data;

	*has_alpha = false;

	n = IMG_BuildCandidates (name, NULL, paths);

	for (i = 0; i < n; i++)
	{
		data = IMG_TryDecoded (paths[i], width, height, has_alpha,
				       winner, sizeof(winner));
		if (data)
		{
			if (name[0] == '{')
				*has_alpha = true;	/* fence textures are always cutouts */

			/* A HUD pic or the particle sprite is displaced just as
			 * silently as a world texture, so it gets the same line.
			 * uhexen2-0q4f. */
			IMG_LogReplacement (name, winner, *width, *height, false);
			return data;
		}
	}

	return NULL;
}
