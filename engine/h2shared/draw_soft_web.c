/*
 * draw_soft_web.c -- extended 2D API for the web software renderer.
 *
 * The shared client code in hexenwail was written against the extended
 * (Ironwail-parity) 2D API that the WebGL2 renderer publishes: alpha pics,
 * alpha fills, glyph batching, multi-canvas UI scaling and full-screen
 * intermission art. draw.c -- the classic 8bpp rasteriser's 2D layer --
 * predates all of that.
 *
 * Rather than sprinkle #ifdefs through console.c / menu.c / sbar.c /
 * pr_csqc.c, this file implements the extended API on top of the 8bpp
 * framebuffer. Alpha is expressed the way the software renderer always
 * expressed it: ordered stipple and colormap darkening.
 *
 * See docs/web/SOFTWARE_RENDERER.md.
 *
 * Copyright (C) 2025-2026  Hexenwail contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "r_local.h"

/* Screen extents, mirrored from vid so shared client code can use the
 * historical names. Refreshed on every Draw_SoftWebInit()/present. */
int	glwidth, glheight;

cvar_t	scr_sbarscale = {"scr_sbarscale", "1", CVAR_ARCHIVE};
cvar_t	scr_menuscale = {"scr_menuscale", "1", CVAR_ARCHIVE};
cvar_t	scr_crosshairscale = {"scr_crosshairscale", "1", CVAR_ARCHIVE};
cvar_t	scr_conalpha = {"scr_conalpha", "0.8", CVAR_ARCHIVE};
cvar_t	scr_conbrightness = {"scr_conbrightness", "1", CVAR_ARCHIVE};

static float	draw_charalpha = 1.0f;

/*
================
Draw_SoftWebInit

Registers the UI-scaling cvars that shared client code reads. Called from
Draw_Init() so the software and WebGL2 builds expose the same cvar set.
================
*/
void Draw_SoftWebInit (void)
{
	static qboolean registered = false;

	glwidth = vid.width;
	glheight = vid.height;

	if (registered)
		return;
	registered = true;

	Cvar_RegisterVariable (&scr_sbarscale);
	Cvar_RegisterVariable (&scr_menuscale);
	Cvar_RegisterVariable (&scr_crosshairscale);
	Cvar_RegisterVariable (&scr_conalpha);
	Cvar_RegisterVariable (&scr_conbrightness);
}

/*
================
GL_SetCanvas

The software renderer has exactly one canvas: the framebuffer itself. The
WebGL2 backend's GL_SetCanvas is likewise a no-op that simply records the
full framebuffer extents, so both renderers agree.
================
*/
void GL_SetCanvas (canvastype newcanvas)
{
	(void) newcanvas;
	glwidth = vid.width;
	glheight = vid.height;
}

/*
================
SCR_CalcUIScale

The 2D layer draws unscaled into a framebuffer that is itself already a
low resolution upscaled by the presenter, so the effective UI scale is
always 1. Returning anything else would move HUD/menu hit-testing away
from where the glyphs actually land.
================
*/
float SCR_CalcUIScale (cvar_t *user)
{
	(void) user;
	return 1.0f;
}

/*
================
Draw_FlushCharBatch

No glyph batching in the software rasteriser -- characters are written
straight to the framebuffer.
================
*/
void Draw_FlushCharBatch (void)
{
}

/*
================
Draw_SetCharacterAlpha

Recorded so callers that fade text out stop drawing it entirely once it is
mostly transparent. Partial alpha uses the same ordered stipple as
Draw_AlphaPic.
================
*/
void Draw_SetCharacterAlpha (float a)
{
	draw_charalpha = a;
}

float Draw_GetCharacterAlpha (void)
{
	return draw_charalpha;
}

/*
=============================================================================

	stipple helpers

	Classic software-renderer translucency: a 4x4 ordered dither
	decides which pixels survive. Cheap, deterministic, and it looks
	like the era it comes from.

=============================================================================
*/

static const byte stipple4x4[16] =
{
	 0,  8,  2, 10,
	12,  4, 14,  6,
	 3, 11,  1,  9,
	15,  7, 13,  5
};

static inline qboolean Stipple_Passes (int x, int y, int threshold)
{
	return stipple4x4[((y & 3) << 2) | (x & 3)] < threshold;
}

/* alpha (0..1) -> 0..16 stipple threshold */
static int Stipple_Threshold (float alpha)
{
	int t;

	if (alpha >= 1.0f)
		return 16;
	if (alpha <= 0.0f)
		return 0;
	t = (int)(alpha * 16.0f + 0.5f);
	if (t < 0) t = 0;
	if (t > 16) t = 16;
	return t;
}

/*
================
Draw_AlphaPic

Draws pic with stipple translucency, skipping TRANSPARENT_COLOR texels.
================
*/
void Draw_AlphaPic (int x, int y, qpic_t *pic, float alpha)
{
	byte	*source, *dest;
	int	u, v, threshold;
	byte	tbyte;

	threshold = Stipple_Threshold (alpha);
	if (threshold <= 0)
		return;
	if (threshold >= 16)
	{
		Draw_TransPic (x, y, pic);
		return;
	}

	if (x < 0 || (x + pic->width) > vid.width ||
	    y < 0 || (y + pic->height) > vid.height)
	{
		Sys_Error ("%s: bad coordinates", __thisfunc__);
	}

	source = pic->data;
	dest = vid.buffer + y * vid.rowbytes + x;

	for (v = 0; v < pic->height; v++)
	{
		for (u = 0; u < pic->width; u++)
		{
			tbyte = source[u];
			if (tbyte == TRANSPARENT_COLOR)
				continue;
			if (!Stipple_Passes (x + u, y + v, threshold))
				continue;
			dest[u] = tbyte;
		}
		dest += vid.rowbytes;
		source += pic->width;
	}
}

/*
=============================================================================

	colour matching

	Draw_FillAlpha takes float RGB, which the paletted renderer has to
	resolve to an index. Results are memoised in a small direct-mapped
	cache keyed on the quantised colour, because callers repeat the
	same handful of colours every frame.

=============================================================================
*/

#define RGBCACHE_BITS	8
#define RGBCACHE_SIZE	(1 << RGBCACHE_BITS)

static struct {
	unsigned int	key;
	byte		index;
	qboolean	valid;
} rgbcache[RGBCACHE_SIZE];

static byte Draw_PaletteIndexForRGB (int r, int g, int b)
{
	const byte	*pal;
	unsigned int	key, slot;
	int		i, bestdist, bestindex;

	key = ((unsigned int)(r & 0xF8) << 16) |
	      ((unsigned int)(g & 0xF8) << 8) |
	      ((unsigned int)(b & 0xF8));
	slot = (key ^ (key >> 11) ^ (key >> 22)) & (RGBCACHE_SIZE - 1);
	if (rgbcache[slot].valid && rgbcache[slot].key == key)
		return rgbcache[slot].index;

	pal = host_basepal;
	bestdist = 0x7FFFFFFF;
	bestindex = 0;
	/* index 255 is the transparency key, never a fill colour */
	for (i = 0; i < 255; i++)
	{
		int dr = r - pal[i * 3 + 0];
		int dg = g - pal[i * 3 + 1];
		int db = b - pal[i * 3 + 2];
		int dist = dr * dr + dg * dg + db * db;

		if (dist < bestdist)
		{
			bestdist = dist;
			bestindex = i;
			if (!dist)
				break;
		}
	}

	rgbcache[slot].key = key;
	rgbcache[slot].index = (byte) bestindex;
	rgbcache[slot].valid = true;
	return (byte) bestindex;
}

static int Draw_Unorm8 (float v)
{
	int i = (int)(v * 255.0f + 0.5f);
	if (i < 0) i = 0;
	if (i > 255) i = 255;
	return i;
}

/*
================
Draw_FillAlpha

Rectangle fill with stipple translucency.
================
*/
void Draw_FillAlpha (int x, int y, int w, int h, float r, float g, float b, float a)
{
	byte	*dest;
	byte	color;
	int	u, v, threshold;
	int	x2, y2;

	threshold = Stipple_Threshold (a);
	if (threshold <= 0 || w <= 0 || h <= 0)
		return;

	/* clamp instead of Sys_Error: callers derive rects from cvars */
	x2 = x + w;
	y2 = y + h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x2 > vid.width) x2 = vid.width;
	if (y2 > vid.height) y2 = vid.height;
	if (x >= x2 || y >= y2)
		return;

	color = Draw_PaletteIndexForRGB (Draw_Unorm8 (r), Draw_Unorm8 (g), Draw_Unorm8 (b));

	if (threshold >= 16)
	{
		Draw_Fill (x, y, x2 - x, y2 - y, color);
		return;
	}

	dest = vid.buffer + y * vid.rowbytes + x;
	for (v = y; v < y2; v++)
	{
		for (u = x; u < x2; u++)
		{
			if (Stipple_Passes (u, v, threshold))
				dest[u - x] = color;
		}
		dest += vid.rowbytes;
	}
}

/*
================
Draw_MenuBackdrop

Full-screen conback, drawn when there is no world behind the menu.
================
*/
void Draw_MenuBackdrop (void)
{
	Draw_ConsoleBackground (vid.height);
}

#if FULLSCREEN_INTERMISSIONS
/*
================
Draw_CachePicNoTrans

The 8bpp path has no load-time transparency to suppress -- transparency is
a palette index resolved at blit time -- so this is just Draw_CachePic.
================
*/
qpic_t *Draw_CachePicNoTrans (const char *path)
{
	return Draw_CachePic (path);
}

/*
================
Draw_IntermissionPic

Nearest-neighbour stretch of pic across the whole framebuffer. Point
sampling is deliberate: it keeps the chunky look consistent with the rest
of the software renderer instead of introducing a lone filtered surface.
================
*/
void Draw_IntermissionPic (qpic_t *pic)
{
	byte	*source, *dest;
	int	u, v;
	int	fracx, fracy, stepx, stepy;

	if (!pic || pic->width <= 0 || pic->height <= 0)
		return;

	if (pic->width == vid.width && pic->height == vid.height)
	{
		Draw_Pic (0, 0, pic);
		return;
	}

	stepx = (pic->width << 16) / vid.width;
	stepy = (pic->height << 16) / vid.height;

	dest = vid.buffer;
	fracy = 0;
	for (v = 0; v < vid.height; v++)
	{
		source = pic->data + (fracy >> 16) * pic->width;
		fracx = 0;
		for (u = 0; u < vid.width; u++)
		{
			dest[u] = source[fracx >> 16];
			fracx += stepx;
		}
		fracy += stepy;
		dest += vid.rowbytes;
	}
}
#endif	/* FULLSCREEN_INTERMISSIONS */
