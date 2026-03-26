/*
 * img_load.c - External image loading (PCX, TGA, PNG)
 *
 * Supports loading external textures to override internal BSP textures.
 * Checks for files in textures/ directory with extensions: .pcx, .tga, .png
 *
 * Copyright (C) 2025 uHexen2 project
 */

#include "quakedef.h"

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
	FILE	*f;
	byte	*rawdata;
	byte	*palette;
	byte	*rgba;
	int		x, y, i;
	int		x_min, y_min, x_max, y_max;
	int		size, bytes_per_scanline;
	byte	pixel;
	int	run_length;
	int		src, dst;

	size = FS_OpenFile_Silent (filename, &f, NULL);
	if (!f || size < 0)
		return NULL;

	if (size < 128 + 769)	// minimum: header + 1 pixel + palette
	{
		fclose (f);
		return NULL;
	}

	rawdata = (byte *) malloc (size);
	if (!rawdata)
	{
		fclose (f);
		return NULL;
	}

	fread (rawdata, 1, size, f);
	fclose (f);

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
	FILE	*f;
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

	size = FS_OpenFile_Silent (filename, &f, NULL);
	if (!f || size < 0)
		return NULL;

	if (size < 18)
	{
		fclose (f);
		return NULL;
	}

	// Read TGA header
	id_len = fgetc (f);
	cmap_type = fgetc (f);
	image_type = fgetc (f);

	// Skip colormap spec (5 bytes) and origin (4 bytes) = 9 bytes total
	fseek (f, 9, SEEK_CUR);

	// Now at byte 12: width, height, bpp, descriptor
	img_w = fgetc (f); img_w |= fgetc (f) << 8;
	img_h = fgetc (f); img_h |= fgetc (f) << 8;
	bpp = fgetc (f);
	descriptor = fgetc (f);

	if (img_w <= 0 || img_h <= 0 || img_w > 4096 || img_h > 4096)
	{
		fclose (f);
		return NULL;
	}

	*width = img_w;
	*height = img_h;

	// Check image type - support uncompressed RGB/RGBA
	if (image_type != 2)	// 2 = uncompressed RGB
	{
		fclose (f);
		return NULL;
	}

	if (bpp != 24 && bpp != 32)
	{
		fclose (f);
		return NULL;
	}

	pixel_size = bpp / 8;
	flip_vert = !(descriptor & 0x20);	// bit 5 = top-to-bottom

	data = (byte *) malloc (img_w * img_h * pixel_size);
	if (!data)
	{
		fclose (f);
		return NULL;
	}

	// Skip image ID and colormap if present
	if (id_len > 0)
		fseek (f, id_len, SEEK_CUR);

	fread (data, 1, img_w * img_h * pixel_size, f);
	fclose (f);

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
	FILE	*f;
	byte	*file_data;
	long	size;

	size = FS_OpenFile_Silent (filename, &f, NULL);
	if (!f || size < 0)
		return NULL;

	file_data = (byte *) malloc (size);
	if (!file_data)
	{
		fclose (f);
		return NULL;
	}

	fread (file_data, 1, size, f);
	fclose (f);

	rgba = stbi_load_from_memory (file_data, size, width, height, &channels, 4);
	free (file_data);

	if (!rgba)
		return NULL;

	*has_alpha = (channels == 4 || channels == 2);
	return rgba;
}

/*
=================
IMG_LoadExternalTexture

Try to load an external texture file.
Checks for .png, .tga, .pcx extensions in order.
Returns allocated RGBA buffer, caller must free after uploading to GL.
=================
*/
byte *IMG_LoadExternalTexture (const char *name, int *width, int *height, qboolean *has_alpha)
{
	char	path[MAX_OSPATH];
	char	safename[MAX_QPATH];
	byte	*data;
	int		alpha;
	int		i;

	*has_alpha = false;

	// Sanitize texture name for filesystem use:
	// '*' is used in BSP for liquid textures (e.g. *lowlight) but is
	// invalid in Windows filenames. Replace with '#' per Quake convention.
	q_strlcpy (safename, name, sizeof(safename));
	for (i = 0; safename[i]; i++)
	{
		if (safename[i] == '*')
			safename[i] = '#';
	}

	// For model skins (names starting with "models/"), try direct path first
	if (!strncmp(safename, "models/", 7))
	{
		// Try PNG first
		q_snprintf (path, sizeof(path), "%s.png", safename);
		data = IMG_LoadPNG (path, width, height, &alpha);
		if (data)
		{
			*has_alpha = alpha;
			if (developer.value >= 2)
				Con_Printf ("Loaded external skin: %s\n", path);
			return data;
		}

		// Try TGA
		q_snprintf (path, sizeof(path), "%s.tga", safename);
		data = IMG_LoadTGA (path, width, height, &alpha);
		if (data)
		{
			*has_alpha = alpha;
			return data;
		}

		// Try PCX
		q_snprintf (path, sizeof(path), "%s.pcx", safename);
		data = IMG_LoadPCX (path, width, height);
		if (data)
		{
			*has_alpha = true;	// PCX uses index 255 for transparency
			if (developer.value >= 2)
				Con_Printf ("Loaded external skin: %s\n", path);
			return data;
		}
	}
	// For GFX/HUD pics (names starting with "gfx/"), try direct path
	else if (!strncmp(safename, "gfx/", 4))
	{
		q_snprintf (path, sizeof(path), "%s.png", safename);
		data = IMG_LoadPNG (path, width, height, &alpha);
		if (data)
		{
			*has_alpha = alpha;
			if (developer.value >= 2)
				Con_Printf ("Loaded external gfx: %s\n", path);
			return data;
		}

		q_snprintf (path, sizeof(path), "%s.tga", safename);
		data = IMG_LoadTGA (path, width, height, &alpha);
		if (data)
		{
			*has_alpha = alpha;
			return data;
		}

		q_snprintf (path, sizeof(path), "%s.pcx", safename);
		data = IMG_LoadPCX (path, width, height);
		if (data)
		{
			*has_alpha = true;
			return data;
		}
	}
	// For particles/ directory (e.g., "particles/blood")
	else if (!strncmp(safename, "particles/", 10))
	{
		// Direct path for particles
		q_snprintf (path, sizeof(path), "%s.png", safename);
		data = IMG_LoadPNG (path, width, height, &alpha);
		if (data)
		{
			*has_alpha = alpha;
			if (developer.value >= 2)
				Con_Printf ("Loaded external texture: %s\n", path);
			return data;
		}

		q_snprintf (path, sizeof(path), "%s.tga", safename);
		data = IMG_LoadTGA (path, width, height, &alpha);
		if (data)
		{
			*has_alpha = alpha;
			if (developer.value >= 2)
				Con_Printf ("Loaded external texture: %s\n", path);
			return data;
		}

		q_snprintf (path, sizeof(path), "%s.pcx", safename);
		data = IMG_LoadPCX (path, width, height);
		if (data)
		{
			*has_alpha = true;
			if (developer.value >= 2)
				Con_Printf ("Loaded external texture: %s\n", path);
			return data;
		}
	}
	else
	{
		// Check for fence textures (texture names starting with '{')
		// These use alpha testing with magenta (255,0,255) as transparent
		qboolean is_fence = (safename[0] == '{');

		// Try textures/ directory for world textures
		q_snprintf (path, sizeof(path), "textures/%s.png", safename);
		data = IMG_LoadPNG (path, width, height, &alpha);
		if (data)
		{
			*has_alpha = alpha || is_fence;  // Force alpha for fence textures
			if (developer.value >= 2)
				Con_Printf ("Loaded external texture: %s (alpha=%d, fence=%d)\n", path, alpha, is_fence);
			return data;
		}

		// Try TGA
		q_snprintf (path, sizeof(path), "textures/%s.tga", safename);
		data = IMG_LoadTGA (path, width, height, &alpha);
		if (data)
		{
			*has_alpha = alpha || is_fence;  // Force alpha for fence textures
			if (developer.value >= 2)
				Con_Printf ("Loaded external texture: %s (alpha=%d, fence=%d)\n", path, alpha, is_fence);
			return data;
		}

		// Try PCX
		q_snprintf (path, sizeof(path), "textures/%s.pcx", safename);
		data = IMG_LoadPCX (path, width, height);
		if (data)
		{
			*has_alpha = true;	// PCX uses index 255 for transparency (always alpha for fence or PCX)
			if (developer.value >= 2)
				Con_Printf ("Loaded external texture: %s\n", path);
			return data;
		}
	}

	return NULL;
}
