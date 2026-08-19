/*
 * draw.h
 * these are the only functions outside the refresh
 * allowed to touch the vid buffer
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
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

#ifndef __HX2_DRAW_H
#define __HX2_DRAW_H

#define MAX_DISC 18

extern	qboolean	draw_reinit;

void Draw_Init (void);
void Draw_ReInit (void);

qpic_t *Draw_PicFromWad (const char *name);
qpic_t *Draw_PicFromFile (const char *name);

qpic_t *Draw_CachePic (const char *path);
#if !defined(DRAW_PROGRESSBARS)
qpic_t *Draw_CacheLoadingPic (void);	/* without the progress bars. */
#else
#define Draw_CacheLoadingPic ()		Draw_CachePic ("gfx/menu/loading.lmp")
#endif	/* DRAW_PROGRESSBARS */

void Draw_Pic (int x, int y, qpic_t *pic);
void Draw_PicCropped (int x, int y, qpic_t *pic);
void Draw_SubPic (int x, int y, qpic_t *pic, int srcx, int srcy, int width, int height);
void Draw_SubPicCropped (int x, int y, int h, qpic_t *pic);
void Draw_TransPic (int x, int y, qpic_t *pic);
void Draw_TransPicCropped (int x, int y, qpic_t *pic);
void Draw_ConsoleBackground (int lines);
void Draw_Crosshair (void);

/* The web builds publish the same extended 2D API as the GL build: the WebGL2
 * configuration because it *is* the GL renderer, the software configuration
 * because draw_soft_web.c reimplements it on the 8bpp framebuffer.  That is
 * what keeps console.c / menu.c / sbar.c free of renderer #ifdefs. */
#if defined(GLQUAKE) || defined(WEBQUAKE)
void Draw_AlphaPic (int x, int y, qpic_t *pic, float alpha);
#endif	/*  GLQUAKE || WEBQUAKE */

void Draw_TransPicTranslate (int x, int y, qpic_t *pic, byte *translation, int p_class);
					/* Only used for the player color selection menu */

#if FULLSCREEN_INTERMISSIONS
# if defined(GLQUAKE) || defined(WEBQUAKE)
qpic_t *Draw_CachePicNoTrans (const char *path);
void Draw_IntermissionPic (qpic_t *pic);
# else	/* !GLQUAKE && !WEBQUAKE */
qpic_t *Draw_CachePicResize (const char *path, int targetWidth, int targetHeight);
# endif	/*  GLQUAKE || WEBQUAKE */
#endif	/*  FULLSCREEN_INTERMISSIONS */

#if defined(GLQUAKE) || defined(WEBQUAKE)
#undef DRAW_LOADINGSKULL
#endif

#if !defined(DRAW_LOADINGSKULL)
#define Draw_BeginDisc()
#define Draw_EndDisc()
#else
void Draw_BeginDisc (void);
void Draw_EndDisc (void);
#endif

void Draw_TileClear (int x, int y, int w, int h);
void Draw_Fill (int x, int y, int w, int h, int c);
void Draw_FadeScreen (void);
#if defined(GLQUAKE) || defined(WEBQUAKE)
void Draw_FillAlpha (int x, int y, int w, int h, float r, float g, float b, float a);
void Draw_MenuBackdrop (void);	/* full-screen conback for main-menu backdrop */
#else
#define Draw_MenuBackdrop()
#endif

void Draw_Character (int x, int y, unsigned int num);
void Draw_SetCharacterAlpha (float a);	/* per-quad alpha for Draw_Character/String; caller restores to 1 */
void Draw_String (int x, int y, const char *str);
void Draw_SmallCharacter (int x, int y, int num);
void Draw_SmallString (int x, int y, const char *str);
void Draw_RedString (int x, int y, const char *str);
void Draw_BigCharacter (int x, int y, int num);

#if defined(GLQUAKE) || defined(WEBQUAKE)
/* Flush any pending batched glyph quads. Called automatically by every
 * other Draw_* function and at the 2D-phase boundary; rarely needed
 * directly. */
void Draw_FlushCharBatch (void);
#else
#define Draw_FlushCharBatch()
#endif

/* game/engine name to draw on the console */
#define GAME_MOD_NAME		ENGINE_NAME
#define ENGINE_WATERMARK	GAME_MOD_NAME " " HW_VERSION " (" PLATFORM_STRING ")"

#if defined(GLQUAKE) || defined(WEBQUAKE)
/* Multi-canvas 2D scaling (Ironwail-parity).
   Each canvas has its own ortho projection and viewport so HUD,
   menu and crosshair can be scaled independently for high-DPI. */
typedef enum {
	CANVAS_NONE,
	CANVAS_DEFAULT,
	CANVAS_SBAR,
	CANVAS_MENU,
	CANVAS_CROSSHAIR,
	CANVAS_INFO,
	CANVAS_INVALID = -1
} canvastype;

void GL_SetCanvas (canvastype newcanvas);

/* Logical size of the fixed 320-wide UI canvases, in canvas units.
   CANVAS_SBAR is anchored to the bottom of the screen and horizontally
   centred; CANVAS_MENU is anchored to the top and horizontally centred.
   The status bar canvas is tall enough for the bumps above the main bar
   and for the lower info bar that drops down on showinfo -- it must match
   BAR_BUMP_HEIGHT + BAR_TOP_HEIGHT + BAR_BOTTOM_HEIGHT in sbar.c. */
#define UI_CANVAS_WIDTH		320
#define UI_SBAR_CANVAS_HEIGHT	(23 + 46 + 98)

extern cvar_t scr_sbarscale;
extern cvar_t scr_menuscale;
extern cvar_t scr_crosshairscale;
extern cvar_t scr_infoscale;
extern cvar_t scr_conalpha;
extern cvar_t scr_conbrightness;

float SCR_CalcUIScale (cvar_t *user);

/* CANVAS_INFO's logical size, and the scale it resolved to.  Either pointer may
 * be NULL.  The debug readouts anchor to this canvas' edges, so they have to ask
 * for its size rather than assume vid.width/vid.height. */
float SCR_InfoCanvasSize (int *w, int *h);
#endif	/* GLQUAKE || WEBQUAKE */

#endif	/* __HX2_DRAW_H */

