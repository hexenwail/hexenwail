/* gl_draw.c
 * this is the only file outside the refresh that touches the vid buffer
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2005-2012  O.Sezer <sezero@users.sourceforge.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "quakedef.h"
#include "hashindex.h"
#include "img_load.h"
#include "gl_shader.h"
#include "gl_pipeline.h"
#include "gl_vbo.h"
#include "gl_matrix.h"
#include "image.h"	/* imagedump's writer */

#ifdef __SSE2__
#include <emmintrin.h>
#endif

/* The console charset: a 16x16 grid of 8x8 glyphs, stored as raw paletted
 * pixels with no header.  Fixed by the format, not by the file. */
#define	CONCHARS_W	256
#define	CONCHARS_H	128
#define	CONCHARS_BYTES	(CONCHARS_W * CONCHARS_H)

#if ENDIAN_RUNTIME_DETECT
/* initialized by VID_Init() */
unsigned int	MASK_r;
unsigned int	MASK_g;
unsigned int	MASK_b;
unsigned int	MASK_a;
unsigned int	MASK_rgb;
unsigned int	SHIFT_r;
unsigned int	SHIFT_g;
unsigned int	SHIFT_b;
unsigned int	SHIFT_a;
#endif

qboolean	draw_reinit = false;

static cvar_t	gl_picmip = {"gl_picmip", "0", CVAR_NONE};
#ifdef __SSE2__
/* Ironwail's r_simd, uhexen2-a5nn.11.  Only exists on a build that HAS a SIMD
 * path to switch off, which upstream also does (#if defined(USE_SIMD)); a cvar
 * that could only ever read 0 would be worse than absent.
 *
 * Our one SIMD path is the mipmap reduction in GL_MipMap_W / GL_MipMap_H, and
 * the two implementations are meant to be bit-identical: _mm_avg_epu8 computes
 * (a + b + 1) >> 1 per byte, which is exactly what the scalar loops below it
 * write out.  So this is a bisection tool -- if turning it off changes a
 * rendered pixel, the SIMD path has a bug -- not a quality or compatibility
 * setting, and there is no reason for a player to touch it.  Read at upload
 * time, so set it before the map loads. */
cvar_t		r_simd = {"r_simd", "1", CVAR_NONE};
#endif
static cvar_t	r_embeddedmipmaps = {"r_embeddedmipmaps", "0", CVAR_ARCHIVE};
static cvar_t	gl_constretch = {"gl_constretch", "0", CVAR_ARCHIVE};
static cvar_t	gl_texturemode = {"gl_texturemode", "", CVAR_ARCHIVE};
cvar_t	gl_texture_anisotropy = {"gl_texture_anisotropy", "8", CVAR_ARCHIVE};
/* Compress engine-generated textures to BC7 on upload.  Default off, matching
 * Ironwail: the codec is lossy, the driver does the encoding at load time (so
 * it costs startup for a VRAM win rather than the other way round), and the
 * quality varies by driver.  uhexen2-8dks. */
cvar_t	gl_compress_textures = {"gl_compress_textures", "0", CVAR_ARCHIVE};
/* Per-texture LOD bias for mipmapped textures (uhexen2-dax2).  Numeric
 * value applied directly; the special string "auto" scales by current
 * MSAA sample count: 0/1 → 0, 2x → -0.25, 4x → -0.5, 8x → -0.75,
 * 16x → -1.0 (each MSAA doubling shifts mipmap pickup -0.25 toward
 * sharper levels, on the theory that MSAA's extra coverage samples
 * can absorb the higher-frequency mip without aliasing). */
cvar_t	gl_lodbias = {"gl_lodbias", "0", CVAR_ARCHIVE};

#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS	0x8501
#endif

/* 0 = auto (1x at 480p, 2x at 960p, ...). Non-zero overrides. */
cvar_t	scr_sbarscale       = {"scr_sbarscale",       "0", CVAR_ARCHIVE};
cvar_t	scr_menuscale       = {"scr_menuscale",       "0", CVAR_ARCHIVE};
cvar_t	scr_crosshairscale  = {"scr_crosshairscale",  "0", CVAR_ARCHIVE};
/* The debug readouts (showfps, showclock, showspeed) get their own scale so
 * they can be legible at 4K without the HUD growing with them, and so they stay
 * put when a player changes scr_sbarscale.  Ironwail cc03300c7, uhexen2-r9qj. */
cvar_t	scr_infoscale       = {"scr_infoscale",       "0", CVAR_ARCHIVE};
/* Console background tweaks (Ironwail parity).
 * scr_conalpha caps the fully-down console alpha (1.0 = opaque,
 * 0.5 = semi-transparent), scr_conbrightness multiplies the conback
 * RGB so users can darken or lighten it independent of alpha. */
cvar_t	scr_conalpha        = {"scr_conalpha",        "1", CVAR_ARCHIVE};
cvar_t	scr_conbrightness   = {"scr_conbrightness",   "1", CVAR_ARCHIVE};

/* Hexen II statusbar logical dimensions (see sbar.c).
   The CANVAS_SBAR canvas reserves room for the top bumps above the
   main bar and for the lower info bar that drops down on showinfo. */
#define SBAR_CANVAS_W		320
#define SBAR_CANVAS_TOP_H	46		/* main bar height (BAR_TOP_HEIGHT) */
#define SBAR_CANVAS_BUMP_H	23		/* bumps above the main bar */
#define SBAR_CANVAS_BOT_H	98		/* lower bar height (BAR_BOTTOM_HEIGHT) */

static GLuint		menuplyr_textures[MAX_PLAYER_CLASS];	// player textures in multiplayer config screens
static GLuint		draw_backtile;
static GLuint		conback;
static GLuint		char_texture;
static GLuint		cs_texture;	// crosshair texture
static GLuint		char_smalltexture;
static GLuint		char_menufonttexture;

/* Glyph batcher: consecutive Draw_*Character calls with the same font
 * texture (and identical state — color, no blend toggle, same canvas)
 * accumulate quads in the GL_Imm buffer and flush as a single draw. A
 * full HUD frame can issue 100+ glyphs; immediate-mode emitted one
 * draw per glyph, which dominated 2D cost on the GL 4.3 pipeline. */
#define MAX_CHAR_BATCH_QUADS	((GL_IMM_MAX_VERTS / 4) - 2)
static int		char_batch_count;
static GLuint		char_batch_tex;
static float		char_batch_alpha = 1.0f;	/* per-quad alpha for the glyph batcher; see Draw_SetCharacterAlpha */

// Crosshair texture is a 32x32 alpha map with 8 levels of alpha.
// The format is similar to an X11 pixmap, but not the same.
// 7 is 100% solid, 0 and any other characters are transparent.
static const char	*cs_data = {
/* This is actually the QuakeWorld crosshair
   which Raven didn't bother changing. It is
   possible to make class-based crosshairs. */
	"................................"
	"................................"
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"................................"
	"................................"
	"................................"
	"................................"
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..7777....7777....7777....7777.."
	"..7777....7777....7777....7777.."
	"..7777....7777....7777....7777.."
	"..7777....7777....7777....7777.."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"................................"
	"................................"
	"................................"
	"................................"
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"..............7777.............."
	"................................"
	"................................"
};

int		gl_filter_idx = 5; /* Trilinear */

gltexture_t	gltextures[MAX_GLTEXTURES];
int			numgltextures;
hashindex_t	hash_gltextures;

/* Set when GL_LoadTexture retires a slot, cleared by the first scan that finds
 * nothing left to reclaim, so the common path never walks the array. */
static qboolean	gl_stale_gltextures;

static GLuint GL_LoadPixmap (const char *name, const char *data);
static void GL_Upload32 (unsigned int *data, gltexture_t *glt);
static void GL_Upload8 (byte *data, gltexture_t *glt);
static void GL_UploadCompressed (const imgreplace_t *r, unsigned int glformat, gltexture_t *glt);
static void GL_SetTextureFilter (const gltexture_t *glt);


//=============================================================================
/* Support Routines */

cachepic_t	menu_cachepics[MAX_CACHED_PICS];
int			menu_numcachepics;
hashindex_t	hash_cachepics;

/*
 * Geometry for the player/skin selection screen image.
 */
#define	PLAYER_PIC_WIDTH	68
#define	PLAYER_PIC_HEIGHT	114
#define	PLAYER_DEST_WIDTH	128
#define	PLAYER_DEST_HEIGHT	128

static byte	menuplyr_pixels[MAX_PLAYER_CLASS][PLAYER_PIC_WIDTH*PLAYER_PIC_HEIGHT];


static void Draw_PicCheckError (void *ptr, const char *name)
{
	if (!ptr)
		Sys_Error ("Failed to load %s", name);
}


qpic_t *Draw_PicFromFile (const char *name)
{
	qpic_t	*p;
	glpic_t	gl;

	p = (qpic_t *)FS_LoadHunkFile (name, NULL);
	if (!p)
		return NULL;

	SwapPic (p);

	// Try external texture override — keep original LMP dimensions
	if (r_texture_external_hud.integer)
	{
		char	extname[MAX_QPATH];
		int		ext_w, ext_h, len;
		qboolean has_alpha;
		byte	*ext_data;

		q_strlcpy (extname, name, sizeof(extname));
		len = (int)strlen(extname);
		if (len > 4 && !strcmp(extname + len - 4, ".lmp"))
			extname[len - 4] = '\0';

		ext_data = IMG_LoadExternalTexture (extname, &ext_w, &ext_h, &has_alpha);
		if (ext_data)
		{
			unsigned int flags = TEX_RGBA | TEX_NEAREST | TEX_UNCOMPRESSED;
			if (has_alpha)
				flags |= TEX_ALPHA;

			gl.texnum = GL_LoadTexture (extname, ext_data, ext_w, ext_h, flags);
			gl.sl = 0;
			gl.sh = 1;
			gl.tl = 0;
			gl.th = 1;
			memcpy (p->data, &gl, sizeof(glpic_t));

			free (ext_data);
			return p;
		}
	}

	gl.texnum = GL_LoadPicTexture (p);
	gl.sl = 0;
	gl.sh = 1;
	gl.tl = 0;
	gl.th = 1;
	memcpy (p->data, &gl, sizeof(glpic_t));

	return p;
}

qpic_t *Draw_PicFromWad (const char *name)
{
	qpic_t	*p;
	glpic_t	gl;

	p = (qpic_t *) W_GetLumpName (name);

	gl.texnum = GL_LoadPicTexture (p);
	gl.sl = 0;
	gl.sh = 1;
	gl.tl = 0;
	gl.th = 1;
	memcpy (p->data, &gl, sizeof(glpic_t));

	return p;
}


/*
================
Draw_CachePic
================
*/
qpic_t	*Draw_CachePic (const char *path)
{
	cachepic_t	*pic;
	int			i, key;
	qpic_t		*dat;
	glpic_t		gl;

	key = Hash_GenerateKeyString (&hash_cachepics, path, true);
	for (i = Hash_First(&hash_cachepics, key); i != -1; i = Hash_Next(&hash_cachepics, i))
	{
		pic = &menu_cachepics[i];
		if (!strcmp (path, pic->name))
			return &pic->pic;
	}

	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
	Hash_Add (&hash_cachepics, key, menu_numcachepics);
	pic = &menu_cachepics[menu_numcachepics];
	menu_numcachepics++;
	q_strlcpy (pic->name, path, MAX_QPATH);

//
// load the pic from disk first to get original dimensions
//
	dat = (qpic_t *)FS_LoadTempFile (path, NULL);
	Draw_PicCheckError (dat, path);
	SwapPic (dat);

//
// try external texture override (e.g. gfx/menu/conback.tga)
// keep original LMP dimensions for layout, use hi-res for GL texture
//
	if (r_texture_external_hud.integer)
	{
		char	extname[MAX_QPATH];
		int		ext_w, ext_h;
		qboolean has_alpha;
		byte	*ext_data;

		// Strip .lmp extension if present
		q_strlcpy (extname, path, sizeof(extname));
		i = (int)strlen(extname);
		if (i > 4 && !strcmp(extname + i - 4, ".lmp"))
			extname[i - 4] = '\0';

		ext_data = IMG_LoadExternalTexture (extname, &ext_w, &ext_h, &has_alpha);
		if (ext_data)
		{
			unsigned int flags = TEX_RGBA | TEX_NEAREST | TEX_UNCOMPRESSED;
			if (has_alpha)
				flags |= TEX_ALPHA;

			// Use original LMP dimensions for layout
			pic->pic.width = dat->width;
			pic->pic.height = dat->height;

			gl.texnum = GL_LoadTexture (extname, ext_data, ext_w, ext_h, flags);
			gl.sl = 0;
			gl.sh = 1;
			gl.tl = 0;
			gl.th = 1;
			memcpy (pic->pic.data, &gl, sizeof(glpic_t));

			free (ext_data);

			if (developer.value >= 2)
				Con_Printf ("Loaded external pic: %s (%dx%d)\n", extname, ext_w, ext_h);

			return &pic->pic;
		}
	}

	// HACK HACK HACK --- we need to keep the bytes for
	// the translatable player picture just for the menu
	// configuration dialog
	/* garymct */
	if (!strcmp (path, "gfx/menu/netp1.lmp"))
		memcpy (menuplyr_pixels[0], dat->data, dat->width*dat->height);
	else if (!strcmp (path, "gfx/menu/netp2.lmp"))
		memcpy (menuplyr_pixels[1], dat->data, dat->width*dat->height);
	else if (!strcmp (path, "gfx/menu/netp3.lmp"))
		memcpy (menuplyr_pixels[2], dat->data, dat->width*dat->height);
	else if (!strcmp (path, "gfx/menu/netp4.lmp"))
		memcpy (menuplyr_pixels[3], dat->data, dat->width*dat->height);
	else if (!strcmp (path, "gfx/menu/netp5.lmp"))
		memcpy (menuplyr_pixels[4], dat->data, dat->width*dat->height);
#if defined (H2W)
	else if (!strcmp (path, "gfx/menu/netp6.lmp"))
		memcpy (menuplyr_pixels[5], dat->data, dat->width*dat->height);
#endif

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;

	gl.texnum = GL_LoadPicTexture (dat);
	gl.sl = 0;
	gl.sh = 1;
	gl.tl = 0;
	gl.th = 1;
	memcpy (pic->pic.data, &gl, sizeof(glpic_t));

	return &pic->pic;
}

#if !defined(DRAW_PROGRESSBARS)
/*
================
Draw_CacheLoadingPic
like Draw_CachePic() but only for loading.lmp
with its progress bars eliminated.
================
*/
static const char ls_path[] = "gfx/menu/loading.lmp";
qpic_t	*Draw_CacheLoadingPic (void)
{
	cachepic_t	*pic;
	int			i, key;
	qpic_t		*dat;
	glpic_t		gl;

	key = Hash_GenerateKeyString (&hash_cachepics, ls_path, true);
	for (i = Hash_First(&hash_cachepics, key); i != -1; i = Hash_Next(&hash_cachepics, i))
	{
		pic = &menu_cachepics[i];
		if (!strcmp (ls_path, pic->name))
			return &pic->pic;
	}

	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");

	dat = (qpic_t *)FS_LoadTempFile (ls_path, NULL);
	Draw_PicCheckError (dat, ls_path);
	SwapPic (dat);
	if (fs_filesize != 17592 || dat->width != 157 || dat->height != 112)
		return Draw_CachePic(ls_path);

	Hash_Add (&hash_cachepics, key, menu_numcachepics);
	pic = &menu_cachepics[menu_numcachepics];
	q_strlcpy (pic->name, ls_path, MAX_QPATH);
	menu_numcachepics++;

	/* kill the progress slot pixels between rows [85:103] */
	memmove(dat->data + 157*85, dat->data + 157*104, 157*(112 - 104));
	dat->height -= (104 - 85);

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;

	gl.texnum = GL_LoadPicTexture (dat);
	gl.sl = 0;
	gl.sh = 1;
	gl.tl = 0;
	gl.th = 1;
	memcpy (pic->pic.data, &gl, sizeof(glpic_t));

	return &pic->pic;
}
#endif	/* !DRAW_PROGRESSBARS */

glmode_t gl_texmodes[NUM_GL_FILTERS] =
{
	{ "GL_NEAREST",			GL_NEAREST,			GL_NEAREST },	/* point sampled	*/
	{ "GL_NEAREST_MIPMAP_NEAREST",	GL_NEAREST_MIPMAP_NEAREST,	GL_NEAREST },	/* nearest, 1 mipmap	*/
	{ "GL_NEAREST_MIPMAP_LINEAR",	GL_NEAREST_MIPMAP_LINEAR,	GL_NEAREST },	/* nearest, 2 mipmaps	*/
	{ "GL_LINEAR",			GL_LINEAR,			GL_LINEAR  },	/* Bilinear, no mipmaps	*/
	{ "GL_LINEAR_MIPMAP_NEAREST",	GL_LINEAR_MIPMAP_NEAREST,	GL_LINEAR  },	/* Bilinear, 1 mipmap	*/
	{ "GL_LINEAR_MIPMAP_LINEAR",	GL_LINEAR_MIPMAP_LINEAR,	GL_LINEAR  }	/* Trilinear: 2 mipmaps	*/
};

/*
===============
Draw_TextureMode_f
===============
*/
static void Draw_TouchAllFilterModes (void)
{
	gltexture_t	*glt;
	int	i;

	for (i = 0, glt = gltextures; i < numgltextures; i++, glt++)
	{
		if (glt->texnum == GL_UNUSED_TEXTURE) continue;	/* skip retired slots */
		if (glt->flags & (TEX_NEAREST|TEX_LINEAR))	/* TEX_MIPMAP mustn't be set in this case */
			continue;
		GL_Bind (glt->texnum);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_texmodes[gl_filter_idx].maximize);
		if (glt->flags & TEX_MIPMAP)
		{
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_texmodes[gl_filter_idx].minimize);
			if (gl_max_anisotropy >= 2)
				glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, gl_texture_anisotropy.value);
		}
		else	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_texmodes[gl_filter_idx].maximize);
	}
}

static void Draw_TextureMode_f (cvar_t *var)
{
	int	i;

	/* GL_NEAREST is the one entry whose minification filter has no mipmap
	 * component, so a minified texture point-samples level 0 and every
	 * screen pixel lands on whichever texel it happens to hit.  On dense
	 * distant geometry that sparkles: capture_white_tipped_trees.rdc caught
	 * SoT mill.bsp's pine crowns speckled with the brightest texels of
	 * models/pine_1.tga, 2.55% of visible pixels above 100 in all channels
	 * against 0.37% for the same skin box-filtered.  Ironwail's classic mode
	 * is GL_NEAREST_MIPMAP_LINEAR for exactly this reason and it offers no
	 * un-mipmapped rung at all.
	 *
	 * Magnification is GL_NEAREST in both modes, so the crunchy up-close
	 * look is untouched -- only minification changes.  Redirect rather than
	 * reject so an existing config keeps loading, and because the resolved
	 * name is written back to the cvar the setting heals itself on the next
	 * config write.  Nothing in the menus or the display presets can reach
	 * this entry; only a hand-set cvar gets here.  uhexen2-kl6w. */
	if (!q_strcasecmp (gl_texmodes[0].name, var->string))
	{
		Con_Printf ("%s has no mipmapped minification (distant detail "
			    "sparkles); using %s\n",
			    gl_texmodes[0].name, gl_texmodes[2].name);
		Cvar_SetQuick (var, gl_texmodes[2].name);
		return;
	}

	for (i = 0; i < NUM_GL_FILTERS; i++)
	{
		if (!strcmp (gl_texmodes[i].name, var->string))
		{
			if (gl_filter_idx != i)
			{
				gl_filter_idx = i;
				// change all the existing mipmap texture objects
				Draw_TouchAllFilterModes ();
			}
			return;
		}
	}

	for (i = 0; i < NUM_GL_FILTERS; i++)
	{
		if (!q_strcasecmp (gl_texmodes[i].name, var->string))
		{
			Cvar_SetQuick (var, gl_texmodes[i].name);
			return;
		}
	}

	Con_Printf ("bad filter name\n");
	Cvar_SetQuick (var, gl_texmodes[gl_filter_idx].name);
}

/*
================
GL_DescribeTextureModes_f

Ironwail's gl_describetexturemodes.  gl_texturemode takes a GL enum spelling
rather than an index, so without this the only way to learn the accepted
strings is to read gl_texmodes[] in the source -- which is not a thing a field
tester can do.  Marks the active one, because the pair of questions is always
"what can I set it to" and "what is it now".
================
*/
static void GL_DescribeTextureModes_f (void)
{
	int	i;

	for (i = 0; i < NUM_GL_FILTERS; i++)
		Con_SafePrintf ("%s %d: %s\n",
				(i == gl_filter_idx) ? "->" : "  ",
				i + 1, gl_texmodes[i].name);

	Con_Printf ("%d modes; gl_texturemode is %s\n",
		    NUM_GL_FILTERS, gl_texmodes[gl_filter_idx].name);
}

/*
================
GL_TextureFlagString

The flag column for `imagelist`, one letter per flag that changed how the
texture was uploaded or is sampled.  Fixed width and fixed order so a few
hundred rows can be scanned for the odd one out, which is what the command is
for.  Returns a static buffer: one call per printf argument list.
================
*/
static const char *GL_TextureFlagString (int flags)
{
	static char	buf[8];

	buf[0] = (flags & TEX_MIPMAP)	? 'm' : '-';
	buf[1] = (flags & TEX_ALPHA)	? 'a' : '-';
	buf[2] = (flags & TEX_RGBA)	? 'r' : '-';
	buf[3] = (flags & TEX_FENCE)	? 'f' : '-';
	buf[4] = (flags & TEX_HOLEY)	? 'h' : '-';
	buf[5] = '\0';

	return buf;
}

/*
================
GL_ImageList_f

Ironwail's imagelist, over gltextures[] instead of its linked list.  Takes an
optional name prefix, which upstream's does not: model skins and world
textures carry their path in the identifier and outnumber everything else
several hundred to one, so `imagelist models/` is the difference between an
answer and four screens of scrollback.  (Menu and HUD pics are the exception
-- they are identified by lump name, "conback", "+0pyr" -- which is why the
prefix is matched, not required.)

Two things this has to get right that upstream's does not.  numgltextures is a
high-water mark over the slot array, not a live count -- GL_LoadTextureEx
retires a slot by setting texnum to GL_UNUSED_TEXTURE and clearing the
identifier, leaving it in place for GL_ClaimStaleTexture to recycle -- so
retired slots are skipped and the live count is counted, not read off.  And the
totals cover every live texture even when the listing is filtered, because the
question the totals answer ("how much texture memory is this map costing")
is not the question the filter asks.
================
*/
static void GL_ImageList_f (void)
{
	const char	*prefix = NULL;
	size_t		preLen = 0;
	gltexture_t	*glt;
	double		texels = 0;
	int		i, live = 0, shown = 0;

	if (Cmd_Argc() > 1)
	{
		prefix = Cmd_Argv(1);
		preLen = strlen (prefix);
	}

	for (i = 0, glt = gltextures; i < numgltextures; i++, glt++)
	{
		if (glt->texnum == GL_UNUSED_TEXTURE || !glt->identifier[0])
			continue;	/* retired slot awaiting reuse */

		live++;
		/* A full mip chain is 4/3 of the base level, the same estimate
		 * upstream makes.  Compressed uploads occupy less than this;
		 * the number is a texel count, not a VRAM measurement. */
		texels += (glt->flags & TEX_MIPMAP)
				? (double)glt->width * glt->height * 4.0 / 3.0
				: (double)glt->width * glt->height;

		if (preLen && q_strncasecmp (prefix, glt->identifier, preLen) != 0)
			continue;

		shown++;
		Con_SafePrintf ("%6u %s %4i x%4i  %s\n", (unsigned int)glt->texnum,
				GL_TextureFlagString (glt->flags),
				glt->width, glt->height, glt->identifier);
	}

	/* texels stays a double all the way to the printf: upstream truncates
	 * it to int, which wraps somewhere north of two billion texels, and a
	 * heavily retextured mod is not that far off. */
	Con_Printf ("%d of %d textures listed; %.0f texels, %.1f megabytes\n",
		    shown, live, texels, texels * 4 / 0x100000);
	Con_Printf ("flags: m=mipmap a=alpha r=rgba f=fence h=holey\n");
}

/*
================
GL_ImageDump_f

Ironwail's imagedump: every live texture written out as an image, for when a
report is "the wrong texture is on that wall" and you need to see what the
engine actually uploaded rather than what is on disk.

PNG rather than upstream's TGA because Image_WriteTGA prefixes fs_gamedir_nopath
-- a bare gamedir NAME in this tree, so it writes CWD-relative -- while
Image_WritePNG takes a full path, which is what lets the dump land beside the
screenshots in the userdir instead of in the install.  Level 0 comes back in
upload order, i.e. top-down, which is the writer's upsidedown=true.
================
*/
#if defined(USE_GLES)
static void GL_ImageDump_f (void)
{
	/* glGetTexImage does not exist in GL ES 3.0 / WebGL2.  Reproducing it
	 * would mean drawing each texture into an FBO and glReadPixels'ing that
	 * back, which is a real feature and not a debug command's worth of work.
	 * Say so rather than silently writing nothing. */
	Con_Printf ("imagedump needs glGetTexImage, which this GL ES build has no equivalent of\n");
}
#else
static void GL_ImageDump_f (void)
{
	char		dirpath[MAX_OSPATH], filepath[MAX_OSPATH];
	char		safename[MAX_QPATH];
	gltexture_t	*glt;
	byte		*buffer;
	char		*c;
	int		i, written = 0, failed = 0;

	FS_MakePath_BUF (FS_USERDIR, NULL, dirpath, sizeof(dirpath), "imagedump");
	Sys_mkdir (dirpath, false);

	for (i = 0, glt = gltextures; i < numgltextures; i++, glt++)
	{
		if (glt->texnum == GL_UNUSED_TEXTURE || !glt->identifier[0])
			continue;	/* retired slot awaiting reuse */
		if (glt->width <= 0 || glt->height <= 0)
			continue;

		/* Identifiers are paths, and some are synthesised with ':' or
		 * '*' in them (the warp textures, the per-face skybox names).
		 * Flatten every separator to '_' so the result is one filename
		 * in one directory on every platform.  Two identifiers that
		 * differ only in a flattened character therefore land on the
		 * same file and the second wins; that is upstream's behaviour
		 * too, and the alternative -- a directory tree mirroring the
		 * gamedir -- is worse to grep through, which is what the dump
		 * is for. */
		q_strlcpy (safename, glt->identifier, sizeof(safename));
		for (c = safename; *c; c++)
		{
			if (strchr ("/\\:*?\"<>|", *c))
				*c = '_';
		}

		q_snprintf (filepath, sizeof(filepath), "%s/%s.png", dirpath, safename);

		buffer = (byte *) malloc ((size_t)glt->width * glt->height * 4);
		if (!buffer)
		{
			Con_Printf ("imagedump: out of memory\n");
			break;
		}

		GL_Bind (glt->texnum);
		glPixelStorei_fp (GL_PACK_ALIGNMENT, 1);
		glGetTexImage_fp (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);

		if (Image_WritePNG (filepath, buffer, glt->width, glt->height, 32, true))
			written++;
		else
			failed++;

		free (buffer);
	}

	Con_Printf ("imagedump: wrote %d texture%s to %s\n",
		    written, (written == 1) ? "" : "s", dirpath);
	if (failed)
		Con_Printf ("imagedump: %d could not be written\n", failed);
}
#endif	/* USE_GLES */

static void Draw_TouchMipmapFilterModes (void)
{
	gltexture_t	*glt;
	int	i;

	for (i = 0, glt = gltextures; i < numgltextures; i++, glt++)
	{
		if (glt->texnum == GL_UNUSED_TEXTURE) continue;	/* skip retired slots */
		if (glt->flags & TEX_MIPMAP)
		{
			GL_Bind (glt->texnum);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_texmodes[gl_filter_idx].maximize);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_texmodes[gl_filter_idx].minimize);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, gl_texture_anisotropy.value);
		}
	}
}

static float GL_ResolveLodBias (void)
{
	extern cvar_t vid_config_fsaa;
	const char *s = gl_lodbias.string;
	if (s && (s[0] == 'a' || s[0] == 'A') && q_strcasecmp(s, "auto") == 0)
	{
		int n = vid_config_fsaa.integer;
		switch (n)
		{
		case 0: case 1:	return  0.0f;
		case 2:		return -0.25f;
		case 4:		return -0.5f;
		case 8:		return -0.75f;
		case 16:	return -1.0f;
		default:	return  0.0f;
		}
	}
	return gl_lodbias.value;
}

static void Draw_LodBias_f (cvar_t *var)
{
	gltexture_t	*glt;
	int		i;
	float		bias = GL_ResolveLodBias();
	(void)var;
	for (i = 0, glt = gltextures; i < numgltextures; i++, glt++)
	{
		if (glt->texnum == GL_UNUSED_TEXTURE) continue;	/* skip retired slots */
		if (glt->flags & TEX_MIPMAP)
		{
			GL_Bind (glt->texnum);
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, bias);
		}
	}
}

static void Draw_Anisotropy_f (cvar_t *var)
{
	if (var->value < 1)
	{
		Cvar_SetQuick (var, "1");
	}
	else if (var->value > gl_max_anisotropy)
	{
		Cvar_SetValueQuick (var, gl_max_anisotropy);
	}
	else
	{
		if (gl_max_anisotropy >= 2)
			Draw_TouchMipmapFilterModes ();
	}
}

/*
===============
Draw_ClearAllModels
Callback for Draw_ReInit() and Draw_ReinitTextures():
Clear all models along with their textures.
===============
*/
static void Draw_ClearAllModels (void)
{
	int	temp = gl_purge_maptex.integer;
	flush_textures = true;
	Cvar_Set ("gl_purge_maptex", "1");
	Mod_ClearAll ();
	Cvar_SetValue ("gl_purge_maptex", temp);
}

#if 0
/*
===============
Draw_ReinitTextures
Delete and reload all textures
===============
*/
static void Draw_ReinitTextures (void)
{
	int	temp;

	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;
	draw_reinit = true;

	D_ClearOpenGLTextures(0);
	memset (lightmap_textures, 0, sizeof(lightmap_textures));
	// make sure all of alias models are cleared
	if (cls.state != ca_active)
		Draw_ClearAllModels ();

	// Reload pre-map pics, fonts, console, etc
	W_LoadWadFile ("gfx.wad");
	Draw_Init();
	SCR_Init();
	Sbar_Init();
	// Reload the particle texture
	R_InitParticleTexture();
	R_InitExtraTextures ();
#ifdef H2W
	R_InitNetgraphTexture();
#endif

	// Reload the map's textures
	if (cls.state == ca_active)
	{
		Mod_ReloadTextures();
		GL_BuildLightmaps();
	}

	draw_reinit = false;
	scr_disabled_for_loading = temp;
}
#endif

/*
===============
Draw_ReInit
Delete and reload textures that read during engine's
init phase which may be changed by mods and purge all
others, i.e. map/model textures.
Should NEVER be called when a map is active: Only
intended to be called upon a game directory change.
===============
*/
void Draw_ReInit (void)
{
	int	temp;

	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;
	draw_reinit = true;

	D_ClearOpenGLTextures(0);
	if (lightmap_textures[0])
		glDeleteTextures_fp (MAX_LIGHTMAPS, lightmap_textures);
	memset (lightmap_textures, 0, sizeof(lightmap_textures));
	// make sure all of alias models are cleared
	Draw_ClearAllModels ();

	// Reload pre-map pics, fonts, console, etc
	W_LoadWadFile ("gfx.wad");
	Draw_Init();
	SCR_Init();
	Sbar_Init();
	// Reload the particle texture
	R_InitParticleTexture();
	R_InitExtraTextures ();
#ifdef H2W
	R_InitNetgraphTexture();
#endif

	draw_reinit = false;
	scr_disabled_for_loading = temp;
}

/*
================
SCR_AutoScale_f

Ironwail's scr_autoscale: one gesture that makes the 2D interface a sensible
size for the resolution.  It means something slightly different here, and the
difference is in our favour.

Upstream's four scale cvars are plain numbers with no auto, so its command
computes one from the mode and writes it into all four -- a snapshot, correct
until the player changes resolution and then quietly wrong.  Ours read zero as
"auto" and re-derive from glheight every time they are asked (SCR_CalcUIScale),
so putting them back to zero is both what the command is for and strictly
better than baking the number.

The console is the exception: it has no auto, only an absolute width or a
multiplier, so that one does get a number, computed by upstream's formula in
VID_AutoConScale.  uhexen2-a5nn.33
================
*/
static void SCR_AutoScale_f (void)
{
	Cvar_SetValueQuick (&scr_sbarscale, 0);
	Cvar_SetValueQuick (&scr_menuscale, 0);
	Cvar_SetValueQuick (&scr_crosshairscale, 0);
	Cvar_SetValueQuick (&scr_infoscale, 0);
	VID_AutoConScale ();
}

/*
===============
Draw_Init
===============
*/

void Draw_Init (void)
{
	qpic_t		*p;
	byte		*chars;
	int		i;
	int		charsofs, charsavail;
	/* Static rather than on the stack: 32 KB is more than this thread's
	 * frame should carry, and GL_LoadTexture may hold the pointer for a
	 * later reload — the temp-file buffer it used to be handed is a good
	 * deal more volatile than this. */
	static byte	charbuf[CONCHARS_BYTES];

	if (!draw_reinit)
	{
		Cvar_RegisterVariable (&gl_picmip);
#ifdef __SSE2__
		Cvar_RegisterVariable (&r_simd);
#endif
		Cvar_RegisterVariable (&r_embeddedmipmaps);
		Cvar_RegisterVariable (&gl_constretch);
		gl_texturemode.string = gl_texmodes[gl_filter_idx].name;
		Cvar_RegisterVariable (&gl_texturemode);
		Cvar_RegisterVariable (&gl_texture_anisotropy);
		Cvar_RegisterVariable (&gl_compress_textures);
		Cvar_RegisterVariable (&gl_lodbias);
		Cvar_RegisterVariable (&scr_sbarscale);
		Cvar_RegisterVariable (&scr_menuscale);
		Cvar_RegisterVariable (&scr_crosshairscale);
		Cvar_RegisterVariable (&scr_infoscale);
		Cvar_RegisterVariable (&scr_conalpha);
		Cvar_RegisterVariable (&scr_conbrightness);
		Cvar_SetCallback (&gl_texturemode, Draw_TextureMode_f);
		Cvar_SetCallback (&gl_texture_anisotropy, Draw_Anisotropy_f);
		Cvar_SetCallback (&gl_lodbias, Draw_LodBias_f);
		Cmd_AddCommand ("scr_autoscale", SCR_AutoScale_f);
		Cmd_AddCommand ("imagelist", GL_ImageList_f);
		Cmd_AddCommand ("imagedump", GL_ImageDump_f);
		Cmd_AddCommand ("gl_describetexturemodes", GL_DescribeTextureModes_f);
		Hash_Allocate (&hash_cachepics, MAX_CACHED_PICS);
		Hash_Allocate (&hash_gltextures, MAX_GLTEXTURES);
	}

	// load the charset: 8*8 graphic characters
	chars = FS_LoadTempFile ("gfx/menu/conchars.lmp", NULL);
	Draw_PicCheckError (chars, "gfx/menu/conchars.lmp");

	/* conchars.lmp is raw 256x128 pixels with no header of any kind, but
	 * WAD tools that do not know that rewrite it as a qpic: an 8-byte
	 * width/height header followed by the same pixels.  A headered lump and
	 * an oversize raw one both land on 32776 bytes, so size alone cannot
	 * separate them and reading the wrong one shifts the whole atlas by 8
	 * pixels.  uhexen2-52gf.
	 *
	 * The header test alone is not enough either.  Measured against the real
	 * files (uhexen2-dmm2): karma2, SoT and portals all ship a 32776-byte
	 * RAW charset that opens with 00 01 00 00 80 00 00 00 — pixels the mod
	 * author drew into glyph 0, which read as exactly the 256/128 header we
	 * were sniffing for.  Those three matched vanilla far better at offset 0
	 * (79%) than at offset 8 (61%), so they are raw with 8 trailing bytes,
	 * not headered.
	 *
	 * What does separate them: after a real header the image begins, and
	 * glyph 0's first row is blank in every charset derived from the stock
	 * one (vanilla opens with eight zero bytes).  So require both the header
	 * AND a blank row behind it.  When in doubt, treat the lump as raw —
	 * that is the reading every observed file wants. */
	charsofs = 0;
	if (fs_filesize >= 8 + CONCHARS_BYTES &&
	    LittleLong(((int *)chars)[0]) == CONCHARS_W &&
	    LittleLong(((int *)chars)[1]) == CONCHARS_H &&
	    !memcmp(chars + 8, "\0\0\0\0\0\0\0\0", 8))
	{
		Con_DPrintf ("conchars.lmp: qpic header found, skipping 8 bytes\n");
		charsofs = 8;
	}
	else if (fs_filesize > CONCHARS_BYTES)
	{
		Con_DPrintf ("conchars.lmp: larger file (%ld bytes), using first %d\n",
			     fs_filesize, CONCHARS_BYTES);
	}

	/* Copy into a fixed-size buffer and pad any shortfall instead of
	 * calling Sys_Error.  A malformed charset in a mod pak used to kill
	 * the engine during Draw_Init — before there was a console to print
	 * the reason to — so it reached users as a bare fatal "bad size" with
	 * no way to see what was wrong or to reach a command line and fix it.
	 * Missing characters draw blank; everything else still works. */
	charsavail = (int)fs_filesize - charsofs;
	if (charsavail < 0)
		charsavail = 0;
	if (charsavail > CONCHARS_BYTES)
		charsavail = CONCHARS_BYTES;
	if (charsavail < CONCHARS_BYTES)
		Con_Printf ("WARNING: gfx/menu/conchars.lmp is short (%d of %d "
			    "bytes) — some characters will be blank\n",
			    charsavail, CONCHARS_BYTES);

	memcpy (charbuf, chars + charsofs, charsavail);
	memset (charbuf + charsavail, 255, CONCHARS_BYTES - charsavail);
	chars = charbuf;

	for (i = 0; i < CONCHARS_BYTES; i++)
	{
		if (chars[i] == 0)
			chars[i] = 255;	// proper transparent color
	}
	char_texture = GL_LoadTexture ("charset", chars, CONCHARS_W, CONCHARS_H,
				       TEX_ALPHA|TEX_NEAREST|TEX_UNCOMPRESSED);
	/* Set CLAMP_TO_EDGE on charset atlas to prevent edge sampling artifacts */
	GL_Bind (char_texture);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// load the small characters for status bar
	chars = (byte *) W_GetLumpName("tinyfont");
	for (i = 0; i < 128*32; i++)
	{
		if (chars[i] == 0)
			chars[i] = 255;	// proper transparent color
	}
	char_smalltexture = GL_LoadTexture ("smallcharset", chars, 128, 32,
					    TEX_ALPHA|TEX_NEAREST|TEX_UNCOMPRESSED);
	GL_Bind (char_smalltexture);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// load the big menu font
	// Note: old version of demo has bigfont.lmp, not bigfont2.lmp
	p = (qpic_t *)FS_LoadTempFile("gfx/menu/bigfont2.lmp", NULL);
	if (!p) p = (qpic_t *)FS_LoadTempFile("gfx/menu/bigfont.lmp", NULL);
	Draw_PicCheckError (p, "gfx/menu/bigfont2.lmp");
	SwapPic (p);
	for (i = 0; i < p->width * p->height; i++)	// MUST be 160 * 80
	{
		if (p->data[i] == 0)
			p->data[i] = 255;	// proper transparent color
	}
	char_menufonttexture = GL_LoadTexture ("menufont", p->data, p->width, p->height,
					       TEX_ALPHA|TEX_LINEAR|TEX_UNCOMPRESSED);
	GL_Bind (char_menufonttexture);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// load the console background
	p = (qpic_t *)FS_LoadTempFile ("gfx/menu/conback.lmp", NULL);
	Draw_PicCheckError (p, "gfx/menu/conback.lmp");
	SwapPic (p);
	conback = GL_LoadTexture ("conback", p->data, p->width, p->height,
				  TEX_LINEAR|TEX_UNCOMPRESSED);

	// load the backtile
	p = (qpic_t *)FS_LoadTempFile ("gfx/menu/backtile.lmp", NULL);
	Draw_PicCheckError (p, "gfx/menu/backtile.lmp");
	SwapPic (p);
	draw_backtile = GL_LoadPicTexture (p);

	// load the crosshair texture
	cs_texture = GL_LoadPixmap ("crosshair", cs_data);

	// initialize the player texnums for multiplayer config screens
	glGenTextures_fp(MAX_PLAYER_CLASS, menuplyr_textures);
}


/*
================
Draw_FlushCharBatch

Emit any pending glyph quads as a single draw. Called automatically by
every non-glyph Draw_* function, by GL_SetCanvas, and at the 2D-phase
boundary in GL_PostProcess_EndFrame.
================
*/
void Draw_FlushCharBatch (void)
{
	if (char_batch_count == 0)
		return;
	/* Draw_CachePic may GL_LoadTexture mid-batch and leave the new pic
	 * bound; re-bind the atlas so the flush samples the right one. */
	GL_Bind (char_batch_tex);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);
	char_batch_count = 0;
	char_batch_tex = 0;
}

/* Append one glyph quad to the batch. (Re)binds the font texture and
 * starts a new batch on texture change or buffer-fill. Font textures
 * have CLAMP_TO_EDGE set once at Draw_Init and nothing else mutates
 * their wrap state, so we don't need to re-set it here. */
static void Draw_AddCharQuad (GLuint tex, int x, int y, int w, int h,
			      float fcol, float frow, float xsize, float ysize)
{
	if (char_batch_tex != tex || char_batch_count >= MAX_CHAR_BATCH_QUADS)
	{
		Draw_FlushCharBatch ();
		GL_Bind (tex);
		GL_ImmBegin ();
		char_batch_tex = tex;
	}

	GL_ImmColor4f (1, 1, 1, char_batch_alpha);
	GL_ImmTexCoord2f (fcol, frow);
	GL_ImmVertex2f (x, y);
	GL_ImmTexCoord2f (fcol + xsize, frow);
	GL_ImmVertex2f (x + w, y);
	GL_ImmTexCoord2f (fcol + xsize, frow + ysize);
	GL_ImmVertex2f (x + w, y + h);
	GL_ImmTexCoord2f (fcol, frow + ysize);
	GL_ImmVertex2f (x, y + h);
	char_batch_count++;
}

/*
================
Draw_SetCharacterAlpha

Set the per-quad alpha used by subsequent Draw_Character / Draw_String
calls.  Callers must restore to 1.0 when done (no auto-reset).
================
*/
void Draw_SetCharacterAlpha (float a)
{
	if (a < 0.0f) a = 0.0f;
	else if (a > 1.0f) a = 1.0f;
	char_batch_alpha = a;
}

/*
================
Draw_Character

Draws one 8*8 graphics character with 0 being transparent.
It can be clipped to the top of the screen to allow the console to be
smoothly scrolled off.
================
*/
void Draw_Character (int x, int y, unsigned int num)
{
	int		row, col;
	float	frow, fcol, xsize,ysize;

	num &= 511;

	if (num == 32)
		return;		// space

	if (y <= -8)
		return;		// totally off screen

	row = num >> 5;
	col = num & 31;

	xsize = 0.03125;
	ysize = 0.0625;
	fcol = col*xsize;
	frow = row*ysize;

	Draw_AddCharQuad (char_texture, x, y, 8, 8, fcol, frow, xsize, ysize);
}

/*
================
Draw_String
================
*/
void Draw_String (int x, int y, const char *str)
{
	while (*str)
	{
		Draw_Character (x, y, *str);
		str++;
		x += 8;
	}
}

void Draw_RedString (int x, int y, const char *str)
{
	while (*str)
	{
		Draw_Character (x, y, ((unsigned char)(*str))+256);
		str++;
		x += 8;
	}
}

/*
================
Draw_Crosshair

Drawn inside CANVAS_CROSSHAIR so scr_crosshairscale enlarges it
on high-DPI displays without affecting the rest of the HUD.

Restores CANVAS_DEFAULT on the way out.  This is load-bearing: it is the first
call in SCR_UpdateScreen's have_world block, and the overlays after it draw at
CANVAS_DEFAULT coordinates without naming a canvas.  Leaving CANVAS_CROSSHAIR
active would put them off-screen with no error and nothing in the log
(uhexen2-ffdy).
================
*/
void Draw_Crosshair (void)
{
	int		x, y;
	unsigned char	*pColor;
	float		s;
	int		canvas_w, canvas_h;

	GL_SetCanvas (CANVAS_CROSSHAIR);

	/* The CROSSHAIR canvas uses framebuffer-aligned coords scaled by
	 * scr_crosshairscale, so map scr_vrect from vid space into the
	 * canvas's logical pixels. */
	s = SCR_CalcUIScale (&scr_crosshairscale);
	{
		/* Same numbers GL_SetCanvas just used for CANVAS_CROSSHAIR; they
		 * have to agree or the crosshair leaves the view centre.
		 * uhexen2-a5nn.37 */
		int	gw, gh;
		SCR_GuiSize (&gw, &gh);
		s = q_min (s, (float)gw / 32.0f);
		s = q_min (s, (float)gh / 32.0f);
		canvas_w = (int)(gw / s);
		canvas_h = (int)(gh / s);
	}

	x = (scr_vrect.x + scr_vrect.width/2)  * canvas_w / vid.width  + (int)cl_crossx.value;
	y = (scr_vrect.y + scr_vrect.height/2) * canvas_h / vid.height + (int)cl_crossy.value;

	if (crosshair.integer == 2)
	{
		pColor = (unsigned char *) &d_8to24table[(byte) crosshaircolor.integer];

		GL_Bind (cs_texture);

		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		// Our crosshair is now 32x32, but we're drawing 16x16 here
		// to have a smaller pic. If, in the pixmap, the pixels are
		// not drawn in doubles, the final image on the screen may
		// have some of the pixels missing. Sigh...
		GL_ImmBegin();
		GL_ImmColor4ubv (pColor);
		GL_ImmTexCoord2f (0, 0);
		GL_ImmVertex2f (x - 7, y - 7);
		GL_ImmTexCoord2f (1, 0);
		GL_ImmVertex2f (x + 9, y - 7);
		GL_ImmTexCoord2f (1, 1);
		GL_ImmVertex2f (x + 9, y + 9);
		GL_ImmTexCoord2f (0, 1);
		GL_ImmVertex2f (x - 7, y + 9);
		GL_ImmEnd (GL_QUADS, &gl_shader_2d);
	}
	else if (crosshair.integer)
	{
		Draw_Character (x - 4, y - 4, '+');
	}

	GL_SetCanvas (CANVAS_DEFAULT);
}


//==========================================================================
//
// Draw_SmallCharacter
//
// Draws a small character that is clipped at the bottom edge of the
// screen.
//
//==========================================================================
void Draw_SmallCharacter (int x, int y, int num)
{
	int		row, col;
	float	frow, fcol, xsize,ysize;

	if (num < 32)
	{
		num = 0;
	}
	else if (num >= 'a' && num <= 'z')
	{
		num -= 64;
	}
	else if (num > '_')
	{
		num = 0;
	}
	else
	{
		num -= 32;
	}

	if (num == 0)
		return;

	if (y <= -8 || y >= vid.height)
		return; 	// totally off screen

	row = num >> 4;
	col = num & 15;

	xsize = 0.0625;
	ysize = 0.25;
	fcol = col*xsize;
	frow = row*ysize;

	Draw_AddCharQuad (char_smalltexture, x, y, 8, 8, fcol, frow, xsize, ysize);
}

//==========================================================================
//
// Draw_SmallString
//
//==========================================================================
void Draw_SmallString (int x, int y, const char *str)
{
	while (*str)
	{
		Draw_SmallCharacter (x, y, *str);
		str++;
		x += 6;
	}
}

//==========================================================================
//
// Draw_BigCharacter
//
// Callback for M_DrawBigCharacter() of menu.c
//
//==========================================================================
void Draw_BigCharacter (int x, int y, int num)
{
	int		row, col;
	float	frow, fcol, xsize, ysize;

	row = num / 8;
	col = num % 8;

	xsize = 0.125;
	ysize = 0.25;
	fcol = col*xsize;
	frow = row*ysize;

	Draw_AddCharQuad (char_menufonttexture, x, y, 20, 20, fcol, frow, xsize, ysize);
}


/*
=============
Draw_Pic
=============
*/
void Draw_Pic (int x, int y, qpic_t *pic)
{
	glpic_t			*gl;

	Draw_FlushCharBatch ();
	gl = (glpic_t *)pic->data;
	GL_Bind (gl->texnum);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin();
	GL_ImmColor4f (1, 1, 1, 1);
	GL_ImmTexCoord2f (gl->sl, gl->tl);
	GL_ImmVertex2f (x, y);
	GL_ImmTexCoord2f (gl->sh, gl->tl);
	GL_ImmVertex2f (x+pic->width, y);
	GL_ImmTexCoord2f (gl->sh, gl->th);
	GL_ImmVertex2f (x+pic->width, y+pic->height);
	GL_ImmTexCoord2f (gl->sl, gl->th);
	GL_ImmVertex2f (x, y+pic->height);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

/*
=============
Draw_AlphaPic
=============
*/
void Draw_AlphaPic (int x, int y, qpic_t *pic, float alpha)
{
	glpic_t			*gl;

	Draw_FlushCharBatch ();
	gl = (glpic_t *)pic->data;
	R_SetBlend (true);
	R_SetCullFace (GL_FRONT);
	GL_Bind (gl->texnum);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin();
	GL_ImmColor4f (1, 1, 1, alpha);
	GL_ImmTexCoord2f (gl->sl, gl->tl);
	GL_ImmVertex2f (x, y);
	GL_ImmTexCoord2f (gl->sh, gl->tl);
	GL_ImmVertex2f (x+pic->width, y);
	GL_ImmTexCoord2f (gl->sh, gl->th);
	GL_ImmVertex2f (x+pic->width, y+pic->height);
	GL_ImmTexCoord2f (gl->sl, gl->th);
	GL_ImmVertex2f (x, y+pic->height);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);
	R_SetBlend (false);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

#if FULLSCREEN_INTERMISSIONS
/*
================
Draw_CachePicNoTrans

Pa3PyX: Function added to cache pics ignoring transparent
colors (e.g. in intermission screens)
================
*/
qpic_t *Draw_CachePicNoTrans (const char *path)
{
	cachepic_t	*pic;
	int		i, key;
	qpic_t		*dat;
	glpic_t		gl;

	key = Hash_GenerateKeyString (&hash_cachepics, path, true);
	for (i = Hash_First(&hash_cachepics, key); i != -1; i = Hash_Next(&hash_cachepics, i))
	{
		pic = &menu_cachepics[i];
		if (!strcmp (path, pic->name))
			return &pic->pic;
	}

	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
	Hash_Add (&hash_cachepics, key, menu_numcachepics);
	pic = &menu_cachepics[menu_numcachepics];
	menu_numcachepics++;
	q_strlcpy (pic->name, path, MAX_QPATH);

//
// load the pic from disk
//
	dat = (qpic_t *)FS_LoadTempFile (path, NULL);
	Draw_PicCheckError (dat, path);
	SwapPic (dat);

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;

	// Get rid of transparencies
	for (i = 0; i < dat->width * dat->height; i++)
	{
		if (dat->data[i] == 255)
			dat->data[i] = 31; // pal(31) == pal(255) == FCFCFC (white)
	}
	gl.texnum = GL_LoadPicTexture (dat);

	gl.sl = 0;
	gl.sh = 1;
	gl.tl = 0;
	gl.th = 1;
	memcpy (pic->pic.data, &gl, sizeof(glpic_t));

	return &pic->pic;
}

/*
=============
Draw_IntermissionPic

Pa3PyX: this new function introduced to draw the intermission screen only
=============
*/
void Draw_IntermissionPic (qpic_t *pic)
{
	glpic_t			*gl;

	Draw_FlushCharBatch ();
	gl = (glpic_t *)pic->data;
	GL_Bind (gl->texnum);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin();
	GL_ImmColor4f (1, 1, 1, 1);
	GL_ImmTexCoord2f (0.0f, 0.0f);
	GL_ImmVertex2f (0.0f, 0.0f);
	GL_ImmTexCoord2f (1.0f, 0.0f);
	GL_ImmVertex2f (vid.width, 0.0f);
	GL_ImmTexCoord2f (1.0f, 1.0f);
	GL_ImmVertex2f (vid.width, vid.height);
	GL_ImmTexCoord2f (0.0f, 1.0f);
	GL_ImmVertex2f (0.0f, vid.height);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}
#endif	/* FULLSCREEN_INTERMISSIONS */


void Draw_SubPic (int x, int y, qpic_t *pic, int srcx, int srcy, int width, int height)
{
	glpic_t			*gl;
	float	newsl, newtl, newsh, newth;
	float	oldglwidth, oldglheight;

	Draw_FlushCharBatch ();
	gl = (glpic_t *)pic->data;

	oldglwidth = gl->sh - gl->sl;
	oldglheight = gl->th - gl->tl;

	newsl = gl->sl + (srcx*oldglwidth)/pic->width;
	newsh = newsl + (width*oldglwidth)/pic->width;

	newtl = gl->tl + (srcy*oldglheight)/pic->height;
	newth = newtl + (height*oldglheight)/pic->height;

	GL_Bind (gl->texnum);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin();
	GL_ImmColor4f (1, 1, 1, 1);
	GL_ImmTexCoord2f (newsl, newtl);
	GL_ImmVertex2f (x, y);
	GL_ImmTexCoord2f (newsh, newtl);
	GL_ImmVertex2f (x+width, y);
	GL_ImmTexCoord2f (newsh, newth);
	GL_ImmVertex2f (x+width, y+height);
	GL_ImmTexCoord2f (newsl, newth);
	GL_ImmVertex2f (x, y+height);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void Draw_PicCropped (int x, int y, qpic_t *pic)
{
	int		height;
	glpic_t 	*gl;
	float		th, tl;

	if ((x < 0) || (x+pic->width > vid.width))
	{
		Sys_Error("%s: bad coordinates", __thisfunc__);
	}

	if (y >= vid.height || y+pic->height < 0)
		return;		// totally off screen

	Draw_FlushCharBatch ();
	gl = (glpic_t *)pic->data;

	// rjr	tl/th need to be computed based upon pic->tl and pic->th
	//	cuz the piece may come from the scrap
	if (y+pic->height > vid.height)
	{
		height = vid.height-y;
		tl = 0;
		th = (height-0.01)/pic->height;
	}
	else if (y < 0)
	{
		height = pic->height+y;
		y = -y;
		tl = (y-0.01)/pic->height;
		th = (pic->height-0.01)/pic->height;
		y = 0;
	}
	else
	{
		height = pic->height;
		tl = gl->tl;
		th = gl->th;//(height-0.01)/pic->height;
	}

	GL_Bind (gl->texnum);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin();
	GL_ImmColor4f (1, 1, 1, 1);
	GL_ImmTexCoord2f (gl->sl, tl);
	GL_ImmVertex2f (x, y);
	GL_ImmTexCoord2f (gl->sh, tl);
	GL_ImmVertex2f (x+pic->width, y);
	GL_ImmTexCoord2f (gl->sh, th);
	GL_ImmVertex2f (x+pic->width, y+height);
	GL_ImmTexCoord2f (gl->sl, th);
	GL_ImmVertex2f (x, y+height);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void Draw_SubPicCropped (int x, int y, int h, qpic_t *pic)
{
	int		height;
	glpic_t 	*gl;
	float		th,tl;

	if ((x < 0) || (x+pic->width > vid.width))
	{
		Sys_Error("%s: bad coordinates", __thisfunc__);
	}

	if (y >= vid.height || y+h < 0)
		return;		// totally off screen

	Draw_FlushCharBatch ();
	gl = (glpic_t *)pic->data;

	// rjr	tl/th need to be computed based upon pic->tl and pic->th
	//	cuz the piece may come from the scrap
	if (y+pic->height > vid.height)
	{
		height = vid.height-y;
		tl = 0;
		th = (height-0.01)/pic->height;
	}
	else if (y < 0)
	{
		height = pic->height+y;
		y = -y;
		tl = (y-0.01)/pic->height;
		th = (pic->height-0.01)/pic->height;
		y = 0;
	}
	else
	{
		height = pic->height;
		tl = gl->tl;
		th = gl->th;//(height-0.01)/pic->height;
	}

	if (height > h)
	{
		height = h;
	}

	GL_Bind (gl->texnum);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin();
	GL_ImmColor4f (1, 1, 1, 1);
	GL_ImmTexCoord2f (gl->sl, tl);
	GL_ImmVertex2f (x, y);
	GL_ImmTexCoord2f (gl->sh, tl);
	GL_ImmVertex2f (x+pic->width, y);
	GL_ImmTexCoord2f (gl->sh, th);
	GL_ImmVertex2f (x+pic->width, y+height);
	GL_ImmTexCoord2f (gl->sl, th);
	GL_ImmVertex2f (x, y+height);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

/*
=============
Draw_TransPic
=============
*/
void Draw_TransPic (int x, int y, qpic_t *pic)
{
	if (!pic)
		return;

	if (x < 0 || (x + pic->width) > vid.width ||
	    y < 0 || (y + pic->height) > vid.height)
	{
		Sys_Error ("%s: bad coordinates", __thisfunc__);
	}

	Draw_Pic (x, y, pic);
}

void Draw_TransPicCropped(int x, int y, qpic_t *pic)
{
	Draw_PicCropped (x, y, pic);
}

/*
=============
Draw_TransPicTranslate

Only used for the player color selection menu
=============
*/
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation, int p_class)
{
	int		i, j, v, u;
	unsigned int	trans[PLAYER_DEST_WIDTH * PLAYER_DEST_HEIGHT], *dest;
	byte		*src;
	int		p;

	Draw_FlushCharBatch ();
	GL_Bind(menuplyr_textures[p_class-1]);
	dest = trans;
	for (v = 0; v < 64; v++, dest += 64)
	{
		src = &menuplyr_pixels[p_class-1][((v*pic->height)>>6) * pic->width];
		for (u = 0; u < 64; u++)
		{
			p = src[(u*pic->width)>>6];
			dest[u] = (p == 255) ? 255 : d_8to24table[translation[p]];
		}
	}

	for (i = 0; i < PLAYER_PIC_WIDTH; i++)
	{
		for (j = 0; j < PLAYER_PIC_HEIGHT; j++)
		{
			int p = menuplyr_pixels[p_class-1][j * PLAYER_PIC_WIDTH + i];
			/* Index 255 is the transparent colorkey globally, but portrait
			 * images use it for dark background pixels that must be opaque. */
			trans[j * PLAYER_DEST_WIDTH + i] = (p == 255)
				? (d_8to24table[255] | MASK_a)
				: d_8to24table[translation[p]];
		}
	}

	glTexImage2D_fp (GL_TEXTURE_2D, 0, gl_alpha_format, PLAYER_DEST_WIDTH, PLAYER_DEST_HEIGHT,
			 0, GL_RGBA, GL_UNSIGNED_BYTE, trans);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin();
	GL_ImmColor3f (1, 1, 1);
	GL_ImmTexCoord2f (0, 0);
	GL_ImmVertex2f (x, y);
	GL_ImmTexCoord2f (( float )PLAYER_PIC_WIDTH / PLAYER_DEST_WIDTH, 0);
	GL_ImmVertex2f (x+pic->width, y);
	GL_ImmTexCoord2f (( float )PLAYER_PIC_WIDTH / PLAYER_DEST_WIDTH, ( float )PLAYER_PIC_HEIGHT / PLAYER_DEST_HEIGHT);
	GL_ImmVertex2f (x+pic->width, y+pic->height);
	GL_ImmTexCoord2f (0, ( float )PLAYER_PIC_HEIGHT / PLAYER_DEST_HEIGHT);
	GL_ImmVertex2f (x, y+pic->height);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);
}


/*
================
Draw_ConsoleBackground

================
*/
static void Draw_ConsolePic (int lines, float ofs, GLuint num, float alpha)
{
	float bright = scr_conbrightness.value;
	if (bright < 0.0f) bright = 0.0f;
	if (bright > 4.0f) bright = 4.0f;	/* sane upper bound */

	Draw_FlushCharBatch ();
	R_SetBlend (true);
	R_SetCullFace (GL_FRONT);
	GL_Bind (num);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin();
	GL_ImmColor4f (bright, bright, bright, alpha);
	GL_ImmTexCoord2f (0, 0 + ofs);
	GL_ImmVertex2f (0, 0);
	GL_ImmTexCoord2f (1, 0 + ofs);
	GL_ImmVertex2f (vid.width, 0);
	GL_ImmTexCoord2f (1, 1);
	GL_ImmVertex2f (vid.width, lines);
	GL_ImmTexCoord2f (0, 1);
	GL_ImmVertex2f (0, lines);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);

	R_SetBlend (false);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

static void Draw_ConsoleVersionInfo (int lines);	/* forward */

/*
================
Draw_MenuBackdrop

Full-screen conback as the main-menu backdrop when no demo / game
world is loaded. Bypasses Draw_ConsoleBackground's alpha cap and
console-height stretch math so the splash always paints, opaque, edge
to edge. Caller must be in CANVAS_DEFAULT.

Renders the version watermark at the bottom-right after the splash so
boot-time identification still works on the main menu.
================
*/
void Draw_MenuBackdrop (void)
{
	Draw_FlushCharBatch ();
	R_SetBlend (false);
	GL_Bind (conback);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	GL_ImmBegin();
	GL_ImmColor4f (1, 1, 1, 1);
	GL_ImmTexCoord2f (0, 0);
	GL_ImmVertex2f (0, 0);
	GL_ImmTexCoord2f (1, 0);
	GL_ImmVertex2f (vid.width, 0);
	GL_ImmTexCoord2f (1, 1);
	GL_ImmVertex2f (vid.width, vid.height);
	GL_ImmTexCoord2f (0, 1);
	GL_ImmVertex2f (0, vid.height);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	/* Pin watermark to the bottom-right of the visible canvas. */
	Draw_ConsoleVersionInfo (vid.height);
}

static void Draw_ConsoleVersionInfo (int lines)
{
	static const char ver[] = ENGINE_WATERMARK;
	const char *ptr = ver;
	/* CANVAS_DEFAULT coordinates: vid.width/vid.height, which carry the
	 * scr_pixelaspect squash; conwidth/conheight are the console's own
	 * unsquashed geometry and are not a canvas extent.  uhexen2-a5nn.37 */
	int x = vid.width - (strlen(ver) * 8 + 11);
	int y = lines - 14;
	for (; *ptr; ++ptr)
		Draw_Character (x + (int)(ptr - ver) * 8, y, *ptr | 0x100);
}

void Draw_ConsoleBackground (int lines)
{
	int	y;
	float	ofs, alpha, cap;

	y = (vid.height * 3) >> 2;
	ofs = (gl_constretch.integer) ? 0.0f : (vid.height - lines) / (float) vid.height;
	alpha = (lines > y) ? 1.0f : 1.1f * lines / y;

	/* User-configurable cap on fully-down alpha. Clamp to a sane range
	 * so the console is at least faintly visible. */
	cap = scr_conalpha.value;
	if (cap < 0.0f) cap = 0.0f;
	if (cap > 1.0f) cap = 1.0f;
	alpha *= cap;

	Draw_ConsolePic (lines, ofs, conback, alpha);

#if defined(H2W)
	if (cls.download)
		return;
#endif	/* H2W */

	Draw_ConsoleVersionInfo (lines);
}


/*
=============
Draw_TileClear

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void Draw_TileClear (int x, int y, int w, int h)
{
	Draw_FlushCharBatch ();
	GL_Bind (draw_backtile);
	GL_ImmBegin();
	GL_ImmColor3f (1, 1, 1);
	GL_ImmTexCoord2f (x/64.0, y/64.0);
	GL_ImmVertex2f (x, y);
	GL_ImmTexCoord2f ( (x+w)/64.0, y/64.0);
	GL_ImmVertex2f (x+w, y);
	GL_ImmTexCoord2f ( (x+w)/64.0, (y+h)/64.0);
	GL_ImmVertex2f (x+w, y+h);
	GL_ImmTexCoord2f (x/64.0, (y+h)/64.0);
	GL_ImmVertex2f (x, y+h);
	GL_ImmEnd (GL_QUADS, &gl_shader_2d);
}


/*
=============
Draw_Fill

Fills a box of pixels with a single color
=============
*/
void Draw_Fill (int x, int y, int w, int h, int c)
{
	Draw_FlushCharBatch ();
	GL_ImmBegin();
	GL_ImmColor3f (host_basepal[c*3]/255.0,
				host_basepal[c*3+1]/255.0,
				host_basepal[c*3+2]/255.0);
	GL_ImmVertex2f (x,y);
	GL_ImmVertex2f (x+w, y);
	GL_ImmVertex2f (x+w, y+h);
	GL_ImmVertex2f (x, y+h);
	GL_ImmEnd (GL_QUADS, &gl_shader_flat);
}

//=============================================================================

/*
================
Draw_FadeScreen

================
*/
void Draw_FadeScreen (void)
{
	extern cvar_t	scr_menubgalpha;
	int		bx, by, ex, ey;
	int		c;
	float		dim = scr_menubgalpha.value;

	/* Nothing to draw at all rather than forty transparent quads.  Also the
	 * only place the cvar can express "no dim" without the caller having to
	 * know about it, which matters because Draw_FadeScreen has callers other
	 * than the menu (the y/n modal draws through it too). */
	if (dim <= 0.0f)
		return;
	if (dim > 1.0f)
		dim = 1.0f;

	Draw_FlushCharBatch ();
	R_SetBlend (true);

	GL_ImmBegin();
	GL_ImmColor4f (248.0/255.0, 220.0/255.0, 120.0/255.0, 0.1 * dim);
	GL_ImmVertex2f (0,0);
	GL_ImmVertex2f (vid.width, 0);
	GL_ImmVertex2f (vid.width, vid.height);
	GL_ImmVertex2f (0, vid.height);
	GL_ImmEnd (GL_QUADS, &gl_shader_flat);

	for (c = 0 ; c < 40 ; c++)
	{
		bx = (rand() % vid.width) - 20;
		by = (rand() % vid.height) - 20;
		ex = bx + (rand() % 40) + 20;
		ey = by + (rand() % 40) + 20;
		if (bx < 0)
			bx = 0;
		if (by < 0)
			by = 0;
		if (ex > vid.width)
			ex = vid.width;
		if (ey > vid.height)
			ey = vid.height;

		GL_ImmBegin();
		GL_ImmColor4f (248.0/255.0, 220.0/255.0, 120.0/255.0, 0.018 * dim);
		GL_ImmVertex2f (bx, by);
		GL_ImmVertex2f (ex, by);
		GL_ImmVertex2f (ex, ey);
		GL_ImmVertex2f (bx, ey);
		GL_ImmEnd (GL_QUADS, &gl_shader_flat);
	}

	R_SetBlend (false);

	Sbar_Changed();
}

/*
================
Draw_FillAlpha
Draws a filled rectangle with color and alpha
================
*/
void Draw_FillAlpha (int x, int y, int w, int h, float r, float g, float b, float a)
{
	Draw_FlushCharBatch ();
	R_SetBlend (true);

	GL_ImmBegin();
	GL_ImmColor4f (r, g, b, a);
	GL_ImmVertex2f (x, y);
	GL_ImmVertex2f (x + w, y);
	GL_ImmVertex2f (x + w, y + h);
	GL_ImmVertex2f (x, y + h);
	GL_ImmEnd (GL_QUADS, &gl_shader_flat);

	R_SetBlend (false);
}

//=============================================================================

/*
================
SCR_GuiSize

The framebuffer squashed by scr_pixelaspect -- the logical extent the four
scaled canvases size themselves against, where they used to use glwidth and
glheight directly.  Ironwail's vid.guiwidth / vid.guiheight, computed here
rather than cached because gl_postprocess.c reassigns glwidth for the duration
of a scaled 3D pass and a cached copy would disagree with it.  uhexen2-a5nn.37

At the default aspect of 1 both are exactly glwidth and glheight, so every
caller below is unchanged at the default -- which is the property the whole
change is verified on.
================
*/
void SCR_GuiSize (int *w, int *h)
{
	float	a = vid.guipixelaspect;

	if (a <= 0.0f)
		a = 1.0f;
	*w = (int)((float)glwidth  / q_max (a, 1.0f));
	*h = (int)((float)glheight * q_min (a, 1.0f));
	if (*w < 1) *w = 1;
	if (*h < 1) *h = 1;
}

/*
================
SCR_CalcUIScale

Resolves a UI scale cvar. Zero (default) means "auto" — picks an
integer multiple based on framebuffer height so the UI stays a
reasonable size on high-DPI displays. Non-zero is taken verbatim
but clamped to keep the canvas on-screen.
================
*/
float SCR_CalcUIScale (cvar_t *user)
{
	float s = (user) ? user->value : 0.0f;
	if (s <= 0.0f)
	{
		/* auto: 1x up to 480p, 2x at 960p, 3x at 1440p, ...
		 * Off the SQUASHED height, so a stretched interface does not keep
		 * a scale chosen for the room it no longer has.  Upstream's scale
		 * clamps read vid.guiheight for the same reason.  uhexen2-a5nn.37 */
		int	gw, gh;
		SCR_GuiSize (&gw, &gh);
		(void) gw;
		s = floorf ((float)gh / 480.0f);
		if (s < 1.0f) s = 1.0f;
	}
	return s;
}


/*
================
SCR_InfoCanvasSize

Resolves scr_infoscale into CANVAS_INFO's logical size, and returns the scale it
settled on.  Callers need the size as well as the scale because the readouts on
this canvas are corner-anchored: they position themselves from its right and
bottom edges, which move with the scale.

The clamp keeps a canvas that can still hold a line of text -- 40 characters at
8x8 -- so a big scr_infoscale in a small window degrades to "as large as fits"
rather than to a readout with its right half off-screen.
================
*/
float SCR_InfoCanvasSize (int *w, int *h)
{
	float	s = SCR_CalcUIScale (&scr_infoscale);
	int	gw, gh;

	SCR_GuiSize (&gw, &gh);

	s = q_min (s, (float)gw / 320.0f);
	s = q_min (s, (float)gh / 240.0f);
	if (s < 1.0f)
		s = 1.0f;

	if (w)
		*w = (int)((float)gw / s);
	if (h)
		*h = (int)((float)gh / s);
	return s;
}

/*
================
GL_SetCanvas

Switches the active 2D canvas. Each canvas owns its ortho
projection and viewport. CANVAS_DEFAULT is the legacy full-screen
1:1 canvas used by everything that hasn't been ported yet.
================
*/
static canvastype currentcanvas = CANVAS_INVALID;

void GL_SetCanvas (canvastype newcanvas)
{
	float	s;
	int	w, h;
	/* The canvas's logical extent is the framebuffer squashed by
	 * scr_pixelaspect; its VIEWPORT is still in real pixels.  px/py convert
	 * between the two, and are 1 at the default aspect.  uhexen2-a5nn.37 */
	int	gw, gh;
	float	px, py;

	if (newcanvas == currentcanvas)
		return;

	SCR_GuiSize (&gw, &gh);
	px = (float)glwidth  / (float)gw;
	py = (float)glheight / (float)gh;

	/* Flush any pending glyph quads under the OLD projection before
	 * we reload the matrices for the new canvas. */
	Draw_FlushCharBatch ();

	currentcanvas = newcanvas;

	GL_MatrixMode(GL_MAT_PROJECTION);
	GL_LoadIdentity();

	switch (newcanvas)
	{
	case CANVAS_DEFAULT:
		glViewport_fp (glx, gly, glwidth, glheight);
		GL_Ortho (0, vid.width, vid.height, 0, -99999, 99999);
		break;
	case CANVAS_SBAR:
		s = SCR_CalcUIScale (&scr_sbarscale);
		/* clamp so the canvas always fits horizontally */
		s = q_min (s, (float)gw / (float)SBAR_CANVAS_W);
		w = (int)((float)SBAR_CANVAS_W * s * px);
		h = (int)((float)(SBAR_CANVAS_BUMP_H + SBAR_CANVAS_TOP_H + SBAR_CANVAS_BOT_H) * s * py);
		/* viewport anchored to the bottom of the screen, centered horizontally */
		glViewport_fp (glx + (glwidth - w) / 2, gly, w, h);
		/* logical y: [0, total_h]. y=0 is the top of the canvas (top of bumps
		   when the lower info bar is fully extended), y=total_h is the screen
		   bottom. All-positive coords keep Draw_PicCropped's "y < 0 means
		   off-screen-top" cropping path from triggering on the top bumps. */
		GL_Ortho (0, SBAR_CANVAS_W,
			  SBAR_CANVAS_BUMP_H + SBAR_CANVAS_TOP_H + SBAR_CANVAS_BOT_H,
			  0, -99999, 99999);
		break;
	case CANVAS_MENU:
		s = SCR_CalcUIScale (&scr_menuscale);
		/* Clamp horizontally so the 320-wide canvas fits the screen. */
		s = q_min (s, (float)gw / 320.0f);
		w = (int)(320.0f * s * px);
		/* Canvas fills the screen vertically — same convention as the
		 * pre-canvas menu (logical y maps 1:1 onto pixel y at scale=1,
		 * grows with scale). Logical height = glheight / s so items
		 * at any y < glheight/s stay visible.
		 *
		 * Horizontally centered. In GL viewport coords (origin bottom-
		 * left) the canvas covers the full screen height. */
		h = glheight;
		glViewport_fp (glx + (glwidth - w) / 2, gly, w, h);
		GL_Ortho (0, 320, (float)gh / s, 0, -99999, 99999);
		break;
	case CANVAS_INFO:
		SCR_InfoCanvasSize (&w, &h);
		glViewport_fp (glx, gly, glwidth, glheight);
		GL_Ortho (0, w, h, 0, -99999, 99999);
		break;
	case CANVAS_CROSSHAIR:
		s = SCR_CalcUIScale (&scr_crosshairscale);
		s = q_min (s, (float)gw / 32.0f);
		s = q_min (s, (float)gh / 32.0f);
		w = (int)(gw / s);
		h = (int)(gh / s);
		glViewport_fp (glx, gly, glwidth, glheight);
		/* keep crosshair coords aligned with vrect (3D viewport) */
		GL_Ortho (0, w, h, 0, -99999, 99999);
		break;
	case CANVAS_NONE:
	case CANVAS_INVALID:
	default:
		glViewport_fp (glx, gly, glwidth, glheight);
		GL_Ortho (0, vid.width, vid.height, 0, -99999, 99999);
		break;
	}

	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_LoadIdentity();
}

/*
================
GL_Set2D

Setup as if the screen was 320*200
================
*/
void GL_Set2D (void)
{
	currentcanvas = CANVAS_INVALID;	/* force re-set */
	GL_SetCanvas (CANVAS_DEFAULT);

	R_SetDepthTest (false);
	R_SetCull (false);
	R_SetBlend (false);

	/* enable alpha test for 2D draws (menu text, HUD) */
	GL_SetAlphaThreshold(0.666f);
}

//====================================================================

/*
================
GL_FindTexture
================
*/
#if 0	/* seems to have no users */
int GL_FindTexture (const char *identifier)
{
	int		i;
	gltexture_t	*glt;

	for (i = 0, glt = gltextures; i < numgltextures; i++, glt++)
	{
		if (!strcmp (identifier, glt->identifier))
			return gltextures[i].texnum;
	}

	return -1;
}
#endif

/*
================
GL_ResampleTexture
================
*/
static void GL_ResampleTexture (unsigned int *in, int inwidth, int inheight, unsigned int *out, int outwidth, int outheight)
{
	int		i, j, mark;
	unsigned int	*inrow, *inrow2;
	unsigned int	frac, fracstep;
	unsigned int	*p1, *p2;
	byte		*pix1, *pix2, *pix3, *pix4;

	fracstep = inwidth * 0x10000 / outwidth;

	mark = Hunk_LowMark();
	p1 = (unsigned int *) Hunk_Alloc (outwidth * sizeof(unsigned int));
	p2 = (unsigned int *) Hunk_Alloc (outwidth * sizeof(unsigned int));

	frac = fracstep >> 2;
	for (i = 0; i < outwidth; i++)
	{
		p1[i] = 4*(frac>>16);
		frac += fracstep;
	}
	frac = 3 * (fracstep >> 2);
	for (i = 0; i < outwidth; i++)
	{
		p2[i] = 4 * (frac >> 16);
		frac += fracstep;
	}

	for (i = 0; i < outheight; i++, out += outwidth)
	{
		inrow = in + inwidth*(int)((i+0.25)*inheight/outheight);
		inrow2 = in + inwidth*(int)((i+0.75)*inheight/outheight);

		frac = fracstep >> 1;
		for (j = 0 ; j < outwidth; j++)
		{
			pix1 = (byte *)inrow + p1[j];
			pix2 = (byte *)inrow + p2[j];
			pix3 = (byte *)inrow2 + p1[j];
			pix4 = (byte *)inrow2 + p2[j];
			((byte *)(out+j))[0] = (pix1[0] + pix2[0] + pix3[0] + pix4[0])>>2;
			((byte *)(out+j))[1] = (pix1[1] + pix2[1] + pix3[1] + pix4[1])>>2;
			((byte *)(out+j))[2] = (pix1[2] + pix2[2] + pix3[2] + pix4[2])>>2;
			((byte *)(out+j))[3] = (pix1[3] + pix2[3] + pix3[3] + pix4[3])>>2;
		}
	}

	Hunk_FreeToLowMark(mark);
}

/*
================
GL_MipMap_W

Horizontally halve an RGBA8 image in-place — average each pair of
adjacent input pixels into one output pixel.  SSE2 fast-path processes
4 output pixels (8 input pixels) per iteration.
================
*/
static void GL_MipMap_W (byte *data, int width, int height)
{
	int		total, i;
	byte		*out = data, *in = data;

	total = (width >> 1) * height;	/* output pixel count */

#ifdef __SSE2__
	while (r_simd.integer && total >= 4)
	{
		__m128i v0 = _mm_loadu_si128 ((const __m128i *) in);
		__m128i v1 = _mm_loadu_si128 ((const __m128i *) (in + 16));
		__m128i v2, v3;

		v0 = _mm_shuffle_epi32 (v0, _MM_SHUFFLE (3, 1, 2, 0));
		v1 = _mm_shuffle_epi32 (v1, _MM_SHUFFLE (3, 1, 2, 0));
		v2 = _mm_unpacklo_epi64 (v0, v1);
		v3 = _mm_unpackhi_epi64 (v0, v1);
		v0 = _mm_avg_epu8 (v2, v3);
		_mm_storeu_si128 ((__m128i *) out, v0);

		total -= 4;
		in += 32;
		out += 16;
	}
#endif

	for (i = 0; i < total; i++, in += 8, out += 4)
	{
		out[0] = (byte) ((in[0] + in[4] + 1) >> 1);
		out[1] = (byte) ((in[1] + in[5] + 1) >> 1);
		out[2] = (byte) ((in[2] + in[6] + 1) >> 1);
		out[3] = (byte) ((in[3] + in[7] + 1) >> 1);
	}
}

/*
================
GL_MipMap_H

Vertically halve an RGBA8 image in-place — average each pair of
adjacent input rows into one output row.  SSE2 fast-path averages
16 bytes (4 pixels) per iteration of the inner loop.
================
*/
static void GL_MipMap_H (byte *data, int width, int height)
{
	int		row_bytes = width * 4;
	int		half_h = height >> 1;
	int		i, j;
	byte		*out = data, *in = data;

	for (i = 0; i < half_h; i++)
	{
		j = 0;
#ifdef __SSE2__
		while (r_simd.integer && j + 16 <= row_bytes)
		{
			__m128i a = _mm_loadu_si128 ((const __m128i *) (in + j));
			__m128i b = _mm_loadu_si128 ((const __m128i *) (in + row_bytes + j));
			__m128i v = _mm_avg_epu8 (a, b);
			_mm_storeu_si128 ((__m128i *) (out + j), v);
			j += 16;
		}
#endif
		for (; j < row_bytes; j += 4)
		{
			out[j+0] = (byte) ((in[j+0] + in[row_bytes+j+0] + 1) >> 1);
			out[j+1] = (byte) ((in[j+1] + in[row_bytes+j+1] + 1) >> 1);
			out[j+2] = (byte) ((in[j+2] + in[row_bytes+j+2] + 1) >> 1);
			out[j+3] = (byte) ((in[j+3] + in[row_bytes+j+3] + 1) >> 1);
		}
		in += row_bytes * 2;
		out += row_bytes;
	}
}

/*
================
GL_MipMap

Quarters the size of the texture.  Operates in-place — both `in` and
`out` are expected to alias the same buffer (preserved from the prior
Darkplaces API for caller compatibility).  Width and height update
in place to reflect the new dimensions.

SSE2 fast-paths in GL_MipMap_W / GL_MipMap_H.  Bit pattern matches
Ironwail (`(a + b + 1) >> 1` per channel) — not bit-identical to the
prior `(a + b + c + d) >> 2` two-row box on a single pass, but
visually equivalent, and faster for both single-axis and combined
downsamples.  Odd width/height discards the last row/column (same as
before).
================
*/
static void GL_MipMap (const byte *in, byte *out, int *width, int *height, int destwidth, int destheight)
{
	(void) in;	/* in-place: only `out` is touched */

	if (*width > destwidth)
	{
		GL_MipMap_W (out, *width, *height);
		*width >>= 1;
	}
	if (*height > destheight)
	{
		GL_MipMap_H (out, *width, *height);
		*height >>= 1;
	}
}

/*
===============
GL_InternalFormat

Internal format for an engine-generated texture: BC7 when compression is on
and this texture is eligible, otherwise the caller's uncompressed choice.
The driver does the encoding -- we hand it ordinary RGBA and it compresses --
so nothing else about the upload changes.

Exemptions, following Ironwail 1011ff8 and 74d8e74 (uhexen2-r7zu):

  - Alpha-tested skins (TEX_FENCE / TEX_HOLEY).  BC7 interpolates alpha
    within each 4x4 block, so the binary mask GL_Upload32 works to keep
    crisp -- it re-binarizes alpha at every mip level for exactly this
    reason -- comes back out of the codec with intermediate values along
    every edge, and the alpha test then cuts a ragged line through them.
    Checked here rather than at the call sites so a new cutout caller
    cannot forget the flag.

  - 2D and HUD art (TEX_UNCOMPRESSED, set by the conchars / menu font /
    conback loaders).  These are drawn unscaled at 1:1, where block
    artifacts are not hidden by minification the way a world texture's are.

  - Lightmaps never reach this function; gl_rsurf.c uploads them directly
    with lightmap_internalformat.

BC7 is core GL 4.2 and this engine requires 4.3, so gl_have_bptc is true on
any context that got this far -- but it is still checked, because the ES tier
shares this file and has no BPTC at all.
===============
*/
static int GL_InternalFormat (const gltexture_t *glt, int uncompressed)
{
	if (!gl_compress_textures.value)
		return uncompressed;
	if (!gl_have_bptc)
		return uncompressed;
	if (glt->flags & (TEX_UNCOMPRESSED | TEX_FENCE | TEX_HOLEY))
		return uncompressed;

	return GL_COMPRESSED_RGBA_BPTC_UNORM;
}

/*
===============
GL_Upload32
===============
*/
static void GL_Upload32 (unsigned int *data, gltexture_t *glt)
{
	int		samples;
	unsigned int	*scaled;
	int		mark = 0;
	int		scaled_width, scaled_height;

	scaled_width = glt->width >> gl_picmip.integer;
	scaled_height = glt->height >> gl_picmip.integer;

	if (scaled_width < 1)
		scaled_width = 1;

	if (scaled_height < 1)
		scaled_height = 1;

	if (scaled_width > gl_max_size)
		scaled_width = gl_max_size;

	if (scaled_height > gl_max_size)
		scaled_height = gl_max_size;

	samples = (glt->flags & TEX_ALPHA) ? gl_alpha_format : gl_solid_format;

	// Alpha-bleed RGBA textures with transparency to prevent black fringes:
	// bilinear filter blends RGB across alpha=0 pixels, so if their RGB is
	// black (common in PNG cutouts) the fringe darkens. Flood RGB from
	// opaque neighbors first. For cutout textures (TEX_FENCE / TEX_HOLEY +
	// RGBA), also binarize alpha so alpha-test reads a clean 0/255 mask.
	// TEX_ALPHA-only (translucent PNG alias skins) gets the bleed without
	// binarization — keeps intermediate alpha intact. uhexen2-pcd1.
	{
		int is_cutout = (glt->flags & TEX_FENCE) ||
				((glt->flags & TEX_HOLEY) && (glt->flags & TEX_RGBA));
		int needs_bleed = is_cutout ||
				((glt->flags & TEX_ALPHA) && (glt->flags & TEX_RGBA));

	if (needs_bleed)
	{
		int i, s, x, y, w, h;
		byte *rgba;
		unsigned int *cleaned_data;
		unsigned short *bleed_dist;

		if (!mark)
			mark = Hunk_LowMark();
		cleaned_data = (unsigned int *) Hunk_AllocName(glt->width * glt->height * sizeof(unsigned int), "texbuf_fence");
		memcpy(cleaned_data, data, glt->width * glt->height * sizeof(unsigned int));

		w = glt->width;
		h = glt->height;
		s = w * h;
		rgba = (byte *)cleaned_data;

		// Step 1: Binarize alpha at threshold 128 (cutout textures only)
		if (is_cutout)
		{
			for (i = 0; i < s; i++)
			{
				int offset = i * 4;
				rgba[offset + 3] = (rgba[offset + 3] >= 128) ? 255 : 0;
			}
		}

		/* Step 2: flood RGB from the opaque texels into *every* transparent
		 * texel, nearest donor first.  Filtering that samples across a
		 * cutout boundary picks up whatever RGB sits under alpha=0, and in
		 * Hexen II that is usually white -- palette index 255, the
		 * transparent index, is (252,252,252), and an exported replacement
		 * skin inherits it (SoT's models/pine_1.tga carries 40850 pure-white
		 * alpha=0 texels right against the frond art).  Every texel the
		 * flood fails to reach bleeds white into the mip chain and frosts
		 * the tips of distant foliage.
		 *
		 * The loop this replaces ran four passes but only ever accepted
		 * alpha==255 donors, and a flooded texel keeps alpha 0 -- so it
		 * never became a donor and passes 2-4 recomputed the same one-texel
		 * ring.  Real reach was one texel, against white regions dozens of
		 * texels across.
		 *
		 * Two-pass city-block distance transform instead: the forward scan
		 * carries each texel's nearest donor down and right, the backward
		 * scan up and left, which is exact for 4-neighbour L1 distance in
		 * O(w*h) with no pass count to tune.  Texels with fractional alpha
		 * are visible, so they neither donate nor receive -- they act as
		 * walls and the flood goes around them.  uhexen2-nkj1, pcd1. */
		bleed_dist = (unsigned short *) Hunk_AllocName(s * sizeof(unsigned short), "texbuf_bleed");
		for (i = 0; i < s; i++)
			bleed_dist[i] = (rgba[i * 4 + 3] == 255) ? 0 : 0xFFFF;

#define BLEED_FROM(neighbor)						\
		do {							\
			int n_ = (neighbor);				\
			if (bleed_dist[n_] + 1 < bleed_dist[i])		\
			{						\
				bleed_dist[i] = bleed_dist[n_] + 1;	\
				rgba[i * 4 + 0] = rgba[n_ * 4 + 0];	\
				rgba[i * 4 + 1] = rgba[n_ * 4 + 1];	\
				rgba[i * 4 + 2] = rgba[n_ * 4 + 2];	\
			}						\
		} while (0)

		for (y = 0; y < h; y++)
		{
			for (x = 0; x < w; x++)
			{
				i = y * w + x;
				if (rgba[i * 4 + 3] != 0)
					continue;	/* donor or wall */
				if (y > 0)
					BLEED_FROM(i - w);
				if (x > 0)
					BLEED_FROM(i - 1);
			}
		}

		for (y = h - 1; y >= 0; y--)
		{
			for (x = w - 1; x >= 0; x--)
			{
				i = y * w + x;
				if (rgba[i * 4 + 3] != 0)
					continue;
				if (y < h - 1)
					BLEED_FROM(i + w);
				if (x < w - 1)
					BLEED_FROM(i + 1);
			}
		}

#undef BLEED_FROM

		data = cleaned_data;
	}
	}

	// Force GL_RGBA8 (0x8058) sized internal format for alpha-tested textures.
	// The legacy internal format '4' may not reliably store alpha on
	// some Mesa/Intel drivers. GL_RGBA8 explicitly requires 8-bit RGBA.
	if (glt->flags & (TEX_FENCE | TEX_HOLEY))
		samples = 0x8058; /* GL_RGBA8 */

	/* After the alpha-tested override, not before: GL_InternalFormat
	 * exempts those textures anyway, so the order only matters if that
	 * ever changes -- and this way the exemption wins either way. */
	samples = GL_InternalFormat (glt, samples);

	if (scaled_width == glt->width && scaled_height == glt->height)
	{
		scaled = data;
	}
	else
	{
		if (!mark)
			mark = Hunk_LowMark();
		scaled = (unsigned int *) Hunk_AllocName(scaled_width * scaled_height * sizeof(unsigned int), "texbuf_upload32");
		GL_ResampleTexture (data, glt->width, glt->height, scaled, scaled_width, scaled_height);
	}

	glTexImage2D_fp (GL_TEXTURE_2D, 0, samples, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, scaled);

	if (glt->flags & TEX_MIPMAP)
	{
		int		miplevel;

		miplevel = 0;
		while (scaled_width > 1 || scaled_height > 1)
		{
			GL_MipMap ((byte *)scaled, (byte *)scaled, &scaled_width, &scaled_height, 1, 1);
			miplevel++;

			/* Re-binarize alpha in mipmaps for alpha-tested textures.
			 * GL_MipMap averages alpha values, creating intermediate
			 * values that would leak through the alpha test. */
			if (glt->flags & (TEX_FENCE | TEX_HOLEY))
			{
				int ii, mipsize = scaled_width * scaled_height;
				byte *miprgba = (byte *)scaled;
				for (ii = 0; ii < mipsize; ii++)
					miprgba[ii * 4 + 3] = (miprgba[ii * 4 + 3] >= 128) ? 255 : 0;
			}

			glTexImage2D_fp (GL_TEXTURE_2D, miplevel, samples, scaled_width, scaled_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, scaled);
		}
	}

	GL_SetTextureFilter (glt);

	if (mark)
		Hunk_FreeToLowMark(mark);
}

/*
===============
GL_SetTextureFilter

Sampler state for a freshly uploaded texture: min/mag filters, anisotropy,
LOD bias and the alpha swizzle.  Split out of GL_Upload32 so the
block-compressed path (GL_UploadCompressed) applies exactly the same rules --
a texture that came from a DDS should sample identically to the same art
shipped as a PNG, and two copies of this would not stay that way.

The texture must already be bound.
===============
*/
static void GL_SetTextureFilter (const gltexture_t *glt)
{
	if (glt->flags & TEX_NEAREST)
	{
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	}
	else if (glt->flags & TEX_LINEAR)
	{
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
	else if (glt->flags & TEX_MIPMAP)
	{
		/* TEX_HOLEY / TEX_FENCE cutout textures get no special MIN_FILTER
		 * treatment here -- they take gl_texturemode like everything else.
		 * uhexen2-khsa r29 used to demote GL_LINEAR_MIPMAP_LINEAR to
		 * GL_LINEAR_MIPMAP_NEAREST for them, on the theory that cross-LOD
		 * interpolation of the pre-binarized mip alpha (see the
		 * re-binarization loop above) was what flickered through the 0.5
		 * discard and produced Mathuzzz's screen-door.  Removed in
		 * uhexen2-izii for three reasons:
		 *
		 *   - The theory is dead.  The 2026-06-22 repro detail (always the
		 *     same entity, shifts between builds, re-running the same map
		 *     fixes it, a different map in between breaks it again) pointed
		 *     at a stale resource across a map transition, and the whole
		 *     alpha/discard/mipmap/A2C family was ruled out with it.  r29
		 *     was never confirmed to fix anything.
		 *
		 *   - It would not have worked anyway.  MIPMAP_NEAREST only picks
		 *     one mip; sampling *within* that mip is still bilinear, so
		 *     every cutout edge still yields the full 0..255 fractional
		 *     alpha range, and anisotropic filtering still multi-taps.  It
		 *     removed one of two sources of an effect that remains.
		 *
		 *   - It was not free.  Every FENCE/HOLEY texture paid visible
		 *     mip-LOD popping for it -- world brush grates and fences, not
		 *     just the alias models the report was about.
		 *
		 * The mip alpha re-binarization above is the principled half and
		 * stays.  Do not re-add the demotion without a repro that a
		 * MIN_FILTER change actually moves. */
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_texmodes[gl_filter_idx].minimize);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_texmodes[gl_filter_idx].maximize);
		if (gl_max_anisotropy >= 2)
			glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, gl_texture_anisotropy.value);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, GL_ResolveLodBias());
	}
	else
	{
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_texmodes[gl_filter_idx].maximize);
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_texmodes[gl_filter_idx].maximize);
	}

	/* uhexen2-khsa r11: opaque textures get GL_TEXTURE_SWIZZLE_A=GL_ONE so
	 * the sampler returns 1.0 for alpha regardless of what's actually in
	 * the texture.  GL_RGB8 is not a real hardware storage format on any
	 * modern GPU — drivers promote it to GL_RGBA8 and may preserve the
	 * source alpha, which for index-255 texels is 0 (d_8to24table[255]
	 * &= MASK_rgb in gl_vidsdl.c).  That zero would leak into the alias
	 * shader's tex.a and fire the production salias_frag discard line.
	 * Forcing the swizzle on the sampler is cheaper than rewriting the
	 * source buffer and works regardless of how the driver allocates
	 * internal storage.  TEX_ALPHA paths (FENCE, HOLEY, TRANSPARENT,
	 * SPECIAL_TRANS, translucent PNG skins) explicitly reset to
	 * GL_ALPHA so re-uploads of the same texture name stay correct. */
	glTexParameteri_fp(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A,
			   (glt->flags & TEX_ALPHA) ? GL_ALPHA : GL_ONE);
}

/*
===============
GL_UploadCompressed

Hand a pre-compressed block payload straight to the driver, one mip level per
call.  No decode, no resample, no mip generation -- the pack already did all
three, which is the entire point of shipping DDS/KTX.

gl_picmip and gl_max_size are honoured by dropping whole levels off the top of
the chain rather than by rescaling; for a mip chain that is not an
approximation of what the RGBA path does, it is the same answer for free.

uhexen2-0vgo.5
===============
*/
static void GL_UploadCompressed (const imgreplace_t *r, unsigned int glformat, gltexture_t *glt)
{
	int	i, first, levels;

	first = gl_picmip.integer;
	if (first < 0)
		first = 0;
	if (first > r->nummips - 1)
		first = r->nummips - 1;

	while (first < r->nummips - 1 &&
	       (r->mipw[first] > gl_max_size || r->miph[first] > gl_max_size))
		first++;

	levels = 0;
	for (i = first; i < r->nummips; i++)
	{
		glCompressedTexImage2D_fp (GL_TEXTURE_2D, levels, (GLenum)glformat,
					   r->mipw[i], r->miph[i], 0,
					   (GLsizei) r->mipsize[i],
					   r->blocks + r->mipofs[i]);
		levels++;

		if (!(glt->flags & TEX_MIPMAP))
			break;
	}

	/* A container is free to ship a single level, and plenty do.  Under a
	 * mipmapping min filter that leaves the texture incomplete, which
	 * samples as white or black depending on the driver -- so cap the chain
	 * at what was actually uploaded instead of leaving GL's default of
	 * 1000.  Set explicitly rather than only when short: this texture name
	 * is fresh from glGenTextures, but being explicit costs nothing and
	 * survives any future slot reuse. */
	glTexParameteri_fp (GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri_fp (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels > 0 ? levels - 1 : 0);

	GL_SetTextureFilter (glt);
}

/*
===============
GL_Upload8_EmbeddedMips

Upload a BSP texture using its embedded mip levels (mip0-mip3) instead
of regenerating mipmaps via box filter.  Only used for simple (non-alpha)
indexed textures.  Remaining levels below mip3 are box-filtered as usual.
===============
*/
static void GL_Upload8_EmbeddedMips (byte *data, gltexture_t *glt)
{
	int		samples, m, i, ms, gl_level, start_mip;
	int		cur_w, cur_h;
	byte		*mip_ptr;
	unsigned int	*conv;
	int		mark;

	samples = GL_InternalFormat (glt, (glt->flags & TEX_ALPHA) ? gl_alpha_format : gl_solid_format);
	start_mip = gl_picmip.integer;
	if (start_mip < 0) start_mip = 0;
	if (start_mip > 3) start_mip = 3;

	/* walk to starting mip data pointer and dimensions */
	mip_ptr = data;
	cur_w = glt->width;
	cur_h = glt->height;
	for (m = 0; m < start_mip; m++)
	{
		mip_ptr += cur_w * cur_h;
		if (cur_w > 1) cur_w >>= 1;
		if (cur_h > 1) cur_h >>= 1;
	}

	mark = Hunk_LowMark();
	conv = (unsigned int *) Hunk_AllocName(cur_w * cur_h * sizeof(unsigned int), "emip");
	gl_level = 0;

	/* upload embedded BSP mip levels */
	for (m = start_mip; m < MIPLEVELS; m++)
	{
		ms = cur_w * cur_h;
		for (i = 0; i < ms; i++)
			conv[i] = d_8to24table[mip_ptr[i]];

		glTexImage2D_fp(GL_TEXTURE_2D, gl_level, samples, cur_w, cur_h, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, conv);
		gl_level++;

		if (cur_w == 1 && cur_h == 1)
			break;

		mip_ptr += ms;
		if (cur_w > 1) cur_w >>= 1;
		if (cur_h > 1) cur_h >>= 1;
	}

	/* generate remaining levels below mip3 using box filter */
	while (cur_w > 1 || cur_h > 1)
	{
		GL_MipMap((byte *)conv, (byte *)conv, &cur_w, &cur_h, 1, 1);
		glTexImage2D_fp(GL_TEXTURE_2D, gl_level, samples, cur_w, cur_h, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, conv);
		gl_level++;
	}

	/* set mipmap filtering */
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_texmodes[gl_filter_idx].minimize);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_texmodes[gl_filter_idx].maximize);
	if (gl_max_anisotropy >= 2)
		glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, gl_texture_anisotropy.value);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, GL_ResolveLodBias());

	/* uhexen2-khsa r11: see GL_Upload32 comment.  Embedded-mip BSP path
	 * only uploads non-alpha indexed textures, so the swizzle is always
	 * GL_ONE here — guarded on the flag anyway for safety. */
	glTexParameteri_fp(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A,
			   (glt->flags & TEX_ALPHA) ? GL_ALPHA : GL_ONE);

	Hunk_FreeToLowMark(mark);
}

/*
===============
GL_Upload8

modes:
0 - standard
1 - color 0 transparent, odd - translucent, even - full value
2 - color 0 transparent
3 - special (particle translucency table)
===============
*/
static void GL_Upload8 (byte *data, gltexture_t *glt)
{
	unsigned int		*trans;
	int			mark;
	int			i, p, s;

	s = glt->width * glt->height;
	mark = Hunk_LowMark();
	trans = (unsigned int *) Hunk_AllocName(s * sizeof(unsigned int), "texbuf_upload8");

	if (glt->flags & (TEX_ALPHA|TEX_TRANSPARENT|TEX_HOLEY|TEX_SPECIAL_TRANS))
	{
		// if there are no transparent pixels, make it a 3 component
		// texture even if it was flagged as TEX_ALPHA.
		qboolean noalpha = !(glt->flags & (TEX_TRANSPARENT|TEX_HOLEY|TEX_SPECIAL_TRANS));

		for (i = 0; i < s; i++)
		{
			p = data[i];
			trans[i] = d_8to24table[p];

			if (p == 255)
			{
				if (noalpha)
					noalpha = false;

				/* transparent, so scan around for another color
				 * to avoid alpha fringes */
				/* this is a replacement from Quake II for Raven's
				 * "neighboring colors" code */
				if (i > glt->width && data[i-glt->width] != 255)
					p = data[i-glt->width];
				else if (i < s-glt->width && data[i+glt->width] != 255)
					p = data[i+glt->width];
				else if (i > 0 && data[i-1] != 255)
					p = data[i-1];
				else if (i < s-1 && data[i+1] != 255)
					p = data[i+1];
				else
					p = 0;
				/* copy rgb components */
				((byte *)&trans[i])[0] = ((byte *)&d_8to24table[p])[0];
				((byte *)&trans[i])[1] = ((byte *)&d_8to24table[p])[1];
				((byte *)&trans[i])[2] = ((byte *)&d_8to24table[p])[2];
			}

			if (glt->flags & TEX_TRANSPARENT)
			{
				p = data[i];
				if (p == 0)
				{
					trans[i] &= MASK_rgb;
				}
				else if (p & 1)
				{
					p = (int)(255 * r_wateralpha.value) & 0xff;
					trans[i] &= MASK_rgb;
					trans[i] |= p << SHIFT_a;
				}
				else
				{
					trans[i] |= MASK_a;
				}
			}
			else if (glt->flags & TEX_HOLEY)
			{
				p = data[i];
				if (p == 0)
				{
					// Index 0 is transparent - flood RGB like we do for index 255
					if (i > glt->width && data[i-glt->width] != 0)
						p = data[i-glt->width];
					else if (i < s-glt->width && data[i+glt->width] != 0)
						p = data[i+glt->width];
					else if (i > 0 && data[i-1] != 0)
						p = data[i-1];
					else if (i < s-1 && data[i+1] != 0)
						p = data[i+1];
					/* copy rgb components from neighbor */
					((byte *)&trans[i])[0] = ((byte *)&d_8to24table[p])[0];
					((byte *)&trans[i])[1] = ((byte *)&d_8to24table[p])[1];
					((byte *)&trans[i])[2] = ((byte *)&d_8to24table[p])[2];
					/* set alpha to 0 */
					trans[i] &= MASK_rgb;
				}
			}
			else if (glt->flags & TEX_SPECIAL_TRANS)
			{
				p = data[i];
				trans[i] = d_8to24table[ColorIndex[p>>4]] & MASK_rgb;
				/* ColorPercent is a TRANSPARENCY -- higher means less of
				 * the model survives.  Store its complement so this
				 * channel means opacity, which is what every consumer of
				 * an alpha channel already assumes: the ordinary blend
				 * func, and weighted-blended OIT, which has no func to
				 * reverse and so silently composited these models by
				 * their inverse.  The non-OIT result is unchanged --
				 * src*(1-CP/255) + dst*(CP/255) either way, see the
				 * matching note in R_DrawAliasModel.  uhexen2-3z3e */
				trans[i] |= ((255 - (( int )ColorPercent[p&15] & 0xff)) & 0xff) << SHIFT_a;
			}
		}

		if (noalpha)
			glt->flags &= ~TEX_ALPHA;
		if (glt->flags & (TEX_TRANSPARENT|TEX_HOLEY|TEX_SPECIAL_TRANS))
			glt->flags |= TEX_ALPHA;
	}
	else
	{
		// Handle non-multiple-of-4 texture sizes (for SoT mod compatibility)
		for (i = 0; i < s; i++)
		{
			trans[i] = d_8to24table[data[i]];
		}
	}

	/* Use embedded BSP mipmaps for simple textures when enabled.
	 * Alpha/fence/transparent textures need special per-pixel processing
	 * that the embedded mip path doesn't handle, so they fall through. */
	if (r_embeddedmipmaps.integer
	    && (glt->flags & TEX_EMBEDDED_MIPS)
	    && (glt->flags & TEX_MIPMAP)
	    && !(glt->flags & (TEX_ALPHA|TEX_TRANSPARENT|TEX_HOLEY|TEX_SPECIAL_TRANS|TEX_FENCE)))
	{
		Hunk_FreeToLowMark(mark);
		GL_Upload8_EmbeddedMips(data, glt);
		return;
	}

	GL_Upload32 (trans, glt);
	Hunk_FreeToLowMark(mark);
}


/*
================
GL_ClaimStaleTexture

Hand back a retired gltexture_t slot for reuse, or -1 if there is none.

A retired slot (see GL_LoadTexture below) has already had its GL name deleted
and its hash entry removed, and carries GL_UNUSED_TEXTURE as its texnum to say
so -- glGenTextures never issues that value, and the two callers that load a
texture under an empty identifier are live entries, so the name alone is not a
reliable marker.  Nothing outside gltextures[] can still be pointed at a
retired slot, which is what makes reuse safe where reusing the *live* slot in
GL_LoadTexture would not be (uhexen2-0fsq).

Only slots at or above gl_texlevel are offered.  D_ClearOpenGLTextures purges
by index range, so a map texture handed a slot below the static-texture
watermark would quietly survive every map change.  uhexen2-owyq.
================
*/
static int GL_ClaimStaleTexture (void)
{
	int	i;

	if (!gl_stale_gltextures)
		return -1;

	for (i = gl_texlevel; i < numgltextures; i++)
	{
		if (gltextures[i].texnum == GL_UNUSED_TEXTURE)
		{
			if (gl_log_texgen.integer)
				Con_Printf ("[texgen] f=%d reclaim slot %d (of %d)\n",
					    host_framecount, i, numgltextures);
			return i;
		}
	}

	/* nothing reclaimable: either a flush took them, or the only retired
	 * slots are below the watermark, where we must not touch them. */
	gl_stale_gltextures = false;
	return -1;
}

/*
================
GL_LoadTextureEx

Shared body of GL_LoadTexture and GL_LoadReplacement.

cimg is NULL for the ordinary indexed/RGBA path.  When it is set, data is
ignored and the texture comes from that container's block payload in cformat
instead.  Everything around the upload -- the identifier hash, stale-slot
reuse, and the mismatch detection that retires an entry whose properties
changed mid-session (uhexen2-0fsq) -- is identical either way, which is why
the two entry points share this rather than keeping separate copies of it.
================
*/
static GLuint GL_LoadTextureEx (const char *identifier, byte *data, int width, int height,
				int flags, const imgreplace_t *cimg, unsigned int cformat)
{
	int		i, slot, size, key;
	unsigned short	crc;
	gltexture_t	*glt;

#if !defined (H2W)
	if (cls.state == ca_dedicated)
		return GL_UNUSED_TEXTURE;
#endif

	if (cimg)
	{
		/* CRC the block payload, not width*height: the dimensions say
		 * nothing about how many bytes a compressed level occupies, and
		 * the cache must still be able to tell two same-sized textures
		 * that share an identifier apart. */
		size = (int)(cimg->mipofs[cimg->nummips - 1] + cimg->mipsize[cimg->nummips - 1]);
		crc = CRC_Block (cimg->blocks, size);
	}
	else
	{
		size = width * height;
		if (flags & TEX_RGBA)
			size *= 4;
		crc = CRC_Block (data, size);
	}

	key = Hash_GenerateKeyString (&hash_gltextures, identifier, true);
	if (identifier[0])
	{
		/* texture already present? */
		for (i = Hash_First(&hash_gltextures, key); i != -1; i = Hash_Next(&hash_gltextures, i))
		{
			glt = &gltextures[i];
			if (!strcmp (identifier, glt->identifier))
			{
				if (crc != glt->crc ||
				    (glt->flags & TEX_MIPMAP) != (flags & TEX_MIPMAP) ||
				    (glt->flags & (TEX_ALPHA|TEX_HOLEY|TEX_FENCE)) != (flags & (TEX_ALPHA|TEX_HOLEY|TEX_FENCE)) ||
				    width  != glt->width || height != glt->height)
				{ /* not the same, mark stale and create new entry.
				   * uhexen2-0fsq: texture properties changed mid-session.
				   * Instead of reusing the gltexture_t slot (which would
				   * reissue an invalid handle to code that cached the old
				   * value), mark this entry stale so lookups skip it and
				   * fall through to create a fresh entry below. */
					GLuint old_texnum = glt->texnum;
					if (gl_log_texgen.integer)
						Con_Printf ("[texgen] f=%d rebind '%s' old=%lu (%dx%d crc=%u flags=0x%x) "
							    "-> new (%dx%d crc=%u flags=0x%x)\n",
							    host_framecount, identifier,
							    (unsigned long)old_texnum,
							    glt->width, glt->height, (unsigned)glt->crc, glt->flags,
							    width, height, (unsigned)crc, flags);
					else
						Con_DPrintf ("Texture cache mismatch: %lu, %s, creating new entry\n",
								    (unsigned long)old_texnum, identifier);
					glDeleteTextures_fp (1, &old_texnum);
					if (currenttexture == old_texnum)
						currenttexture = GL_UNUSED_TEXTURE;
					Hash_Remove (&hash_gltextures, key, i);
					glt->identifier[0] = '\0';	/* mark entry stale */
					/* GL_UNUSED_TEXTURE is the marker GL_ClaimStaleTexture
					 * reads, and it also keeps D_ClearOpenGLTextures from
					 * re-deleting a name the driver may have recycled to a
					 * live texture by then.  uhexen2-owyq. */
					glt->texnum = GL_UNUSED_TEXTURE;
					gl_stale_gltextures = true;
					break;	/* exit loop, continue to new-entry creation below */
				}
				else	return glt->texnum;	/* the same is present. */
			}
		}
	}

	slot = GL_ClaimStaleTexture ();
	if (slot < 0)
	{
		if (numgltextures >= MAX_GLTEXTURES)
			Sys_Error ("%s: cache full, max is %i textures.", __thisfunc__, MAX_GLTEXTURES);
		slot = numgltextures++;
	}

	Hash_Add (&hash_gltextures, key, slot);
	glt = &gltextures[slot];
	q_strlcpy (glt->identifier, identifier, MAX_QPATH);

	glGenTextures_fp(1, &glt->texnum);
	glt->width = width;
	glt->height = height;
	glt->flags = flags;
	glt->crc = crc;

	GL_Bind (glt->texnum);
	if (cimg)
		GL_UploadCompressed (cimg, cformat, glt);
	else if (flags & TEX_RGBA)
		GL_Upload32 ((unsigned int *)data, glt);
	else	GL_Upload8 (data, glt);

	return glt->texnum;
}

/*
================
GL_LoadTexture
================
*/
GLuint GL_LoadTexture (const char *identifier, byte *data, int width, int height, int flags)
{
	return GL_LoadTextureEx (identifier, data, width, height, flags, NULL, 0);
}

/*
================
GL_LoadReplacement

Upload a replacement image resolved by img_load.c, taking whichever path the
file on disk calls for.  Callers pass only what they know about the surface
(TEX_MIPMAP, and TEX_FENCE / TEX_HOLEY for cutouts); TEX_ALPHA and TEX_RGBA
come from the image.
================
*/
GLuint GL_LoadReplacement (const char *identifier, const struct imgreplace_s *r, int extraflags)
{
	int		flags = extraflags;
	unsigned int	format;

	if (r->has_alpha)
		flags |= TEX_ALPHA;

	if (!r->blocks || r->nummips < 1)
		return GL_LoadTextureEx (identifier, r->rgba, r->width, r->height,
					 flags | TEX_RGBA, NULL, 0);

	format = r->glformat;

	/* A DXT1 file that did not set DDPF_ALPHAPIXELS decodes with alpha
	 * forced to 1.0.  That is right for an opaque wall and wrong for a
	 * cutout, where the holes are the entire point of the texture -- and
	 * most DDS writers never set the flag.  Where the caller has already
	 * established this is a cutout (a '{' texture name, or EF_HOLEY on a
	 * model), believe the caller over the file. */
	if ((flags & (TEX_FENCE | TEX_HOLEY)) && format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT)
	{
		format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
		flags |= TEX_ALPHA;
	}

	return GL_LoadTextureEx (identifier, NULL, r->width, r->height, flags, r, format);
}

/*
===============
GL_LoadPixmap
from LordHavoc's Twilight (DarkPlaces) project

Loads a string into a named 32x32 greyscale OpenGL texture, suitable for
crosshairs or pointers. The data string is in a format similar to an X11
pixmap.  '0'-'7' are brightness levels, any other character is considered
transparent. Remember, NO error checking is performed on the input string.
*/
static GLuint GL_LoadPixmap (const char *name, const char *data)
{
	int		i;
	unsigned char	pixels[32*32][4];

	for (i = 0; i < 32*32; i++)
	{
		if (data[i] >= '0' && data[i] < '8')
		{
			pixels[i][0] = 255;
			pixels[i][1] = 255;
			pixels[i][2] = 255;
			pixels[i][3] = (data[i] - '0') * 32;
		}
		else
		{
			pixels[i][0] = 255;
			pixels[i][1] = 255;
			pixels[i][2] = 255;
			pixels[i][3] = 0;
		}
	}

	return GL_LoadTexture (name, (unsigned char *) pixels, 32, 32, TEX_ALPHA | TEX_RGBA | TEX_LINEAR);
}

/*
================
GL_LoadPicTexture
================
*/
GLuint GL_LoadPicTexture (qpic_t *pic)
{
	/* Every HUD/menu pic funnels through here, and all of them are drawn
	 * unscaled -- so they get the same compression exemption the named 2D
	 * textures above do.  Ironwail's commits name only conchars and
	 * conback, but the reason applies to the whole 2D layer. */
	return GL_LoadTexture ("", pic->data, pic->width, pic->height,
			       TEX_ALPHA|TEX_NEAREST|TEX_UNCOMPRESSED);
}

