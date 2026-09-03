/*
 * vid_soft_web.c -- video backend for the 8bpp software renderer on the
 * web platform.
 *
 * Owns the palettized software framebuffer, the software resolution ladder
 * and the aspect policy; hands the finished frame to the accelerated
 * presentation canvas (web_canvas.h).  See docs/web/SOFTWARE_RENDERER.md.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2026  Hexenwail contributors
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
#include "d_local.h"
#include "web_canvas.h"
#include "sdl_inc.h"

#include <emscripten/emscripten.h>

viddef_t	vid;			// global video state
modestate_t	modestate = MS_FULLSCREEN;
qboolean	in_mode_set;

/* The SDL3 window and GL context.  Unlike alextnewman/hexenwail 0b1906add,
 * which replaced SDL with a bespoke browser platform layer, only the renderer
 * is swapped here: in_sdl.c, sys_sdl.c and snd_sdl.c are shared with every
 * other target, so the window has to be the one they already talk to.  The
 * presenter (web_canvas_gl2.c) draws into this context. */
static SDL_Window	*sdl_window;
static SDL_GLContext	sdl_glcontext;

cvar_t		_enable_mouse = {"_enable_mouse", "1", CVAR_ARCHIVE};

byte		globalcolormap[VID_GRADES * 256];
byte		lastglobalcolor = 0;
byte		*lastsourcecolormap = NULL;
unsigned short	d_8to16table[256];
unsigned int	d_8to24table[256];

/*
 * Software resolution ladder.
 *
 * Every entry is 4:3 (683x512 is 4:3 to within half a pixel) and stays
 * inside the software rasterizer's MAXWIDTH/MAXHEIGHT limits (1280x1024,
 * see r_shared.h).  The list is deliberately short: these are render
 * resolutions, not display modes -- the presentation canvas always runs at
 * the panel's full device resolution and scales this image up on the GPU.
 *
 * Entries flagged as "panel aligned" divide an M1 iPad Pro panel exactly:
 *   683x512  x4 = 2732x2048  (12.9-inch, exact)
 *   1024x768 x2 = 2048x1536  (fits both panels with a small border)
 */
typedef struct
{
	int		width;
	int		height;
	const char	*desc;
} softmode_t;

static const softmode_t softmodes[] = {
	{  320, 240, "320 x 240" },
	{  400, 300, "400 x 300" },
	{  512, 384, "512 x 384" },
	{  640, 480, "640 x 480" },
	{  683, 512, "683 x 512" },
	{  800, 600, "800 x 600" },
	{  960, 720, "960 x 720" },
	{ 1024, 768, "1024 x 768" },
	{ 1152, 864, "1152 x 864" },
	{ 1280, 960, "1280 x 960" },
};

#define NUM_SOFTMODES	((int)(sizeof(softmodes) / sizeof(softmodes[0])))

/* Auto mode never picks anything heavier than this. The software rasterizer
 * is single-threaded on the main WASM thread, so pixel count is the frame
 * budget. 1024x768 is the highest rung an M1 class core sustains at 60 Hz
 * with room left for the server, sound and QuakeC. */
#define AUTO_MAX_PIXELS		(1024 * 768)
/* Auto mode refuses to upscale by less than this: below it the software
 * image is so close to the panel resolution that it costs a lot and looks
 * no better than the rung below. */
#define AUTO_MIN_SCALE		2.0f

/* 0 = pick automatically from the canvas size, 1..NUM_SOFTMODES = ladder index. */
static cvar_t	vid_soft_mode = {"vid_soft_mode", "0", CVAR_ARCHIVE};
/* 0 = nearest neighbour (classic crunchy pixels), 1 = pixel-art antialiasing. */
static cvar_t	vid_soft_filter = {"vid_soft_filter", "0", CVAR_ARCHIVE};
/* 0 = keep the 4:3 render aspect and pillarbox, 1 = stretch to fill. */
static cvar_t	vid_soft_stretch = {"vid_soft_stretch", "0", CVAR_ARCHIVE};

static byte	*vid_framebuffer;
static int	vid_framebuffer_size;
static byte	*vid_surfcache;
static int	vid_surfcachesize;
static int	vid_highhunkmark;
static int	canvas_width = 960, canvas_height = 720;
static int	vid_current_mode = -1;
static int	vid_menu_mode;
static int	lockcount;
static byte	vid_curpal[768];
static qboolean	vid_initialized;

/*
================
VID_DestRect

Where the software image lands on the canvas, in device pixels. Keeps the
render aspect unless vid_soft_stretch is set, and centres what is left.
================
*/
static void VID_DestRect (int src_w, int src_h, int *x, int *y, int *w, int *h)
{
	float	src_aspect;

	if (vid_soft_stretch.integer)
	{
		*x = *y = 0;
		*w = canvas_width;
		*h = canvas_height;
		return;
	}

	src_aspect = (float)src_w / (float)src_h;
	if ((float)canvas_width / (float)canvas_height > src_aspect)
	{
		*h = canvas_height;
		*w = (int)(canvas_height * src_aspect + 0.5f);
	}
	else
	{
		*w = canvas_width;
		*h = (int)(canvas_width / src_aspect + 0.5f);
	}
	if (*w < 1) *w = 1;
	if (*h < 1) *h = 1;
	*x = (canvas_width - *w) / 2;
	*y = (canvas_height - *h) / 2;
}

/*
================
VID_AutoMode

Picks the ladder rung that upscales most cleanly onto the current canvas.
An exact integer upscale is always preferred because it is the only way to
keep the software renderer's pixels perfectly square and crisp; otherwise
the largest affordable rung wins.
================
*/
static int VID_AutoMode (void)
{
	int	i, best = -1, largest = -1;

	for (i = 0; i < NUM_SOFTMODES; ++i)
	{
		int	w = softmodes[i].width;
		int	h = softmodes[i].height;
		int	dx, dy, dw, dh;
		float	scale, rounded;

		if (w * h > AUTO_MAX_PIXELS)
			continue;

		VID_DestRect (w, h, &dx, &dy, &dw, &dh);
		scale = (float)dh / (float)h;
		if (scale < AUTO_MIN_SCALE)
			continue;

		largest = i;

		rounded = (float)floor(scale + 0.5);
		if (fabs(scale - rounded) <= 0.01 * rounded)
			best = i;	/* exact integer upscale */
	}

	if (best >= 0)
		return best;
	if (largest >= 0)
		return largest;
	return 0;
}

static int VID_WantedMode (void)
{
	int	mode = vid_soft_mode.integer;

	if (mode >= 1 && mode <= NUM_SOFTMODES)
		return mode - 1;
	return VID_AutoMode ();
}

/*
================
VID_AllocBuffers

Framebuffer, z-buffer and surface cache for one software resolution. The
z-buffer and surface cache live on the high hunk exactly like the classic
backends so a mode change can release them in one shot.
================
*/
static void VID_AllocBuffers (int width, int height)
{
	int	zbuffersize, cachesize, pixels;

	pixels = width * height;
	zbuffersize = pixels * (int)sizeof(*d_pzbuffer);
	cachesize = D_SurfaceCacheForRes (width, height);

	if (vid_framebuffer_size < pixels)
	{
		byte *buf = (byte *) realloc (vid_framebuffer, pixels);
		if (!buf)
			Sys_Error ("Not enough memory for a %dx%d software framebuffer", width, height);
		vid_framebuffer = buf;
		vid_framebuffer_size = pixels;
	}
	memset (vid_framebuffer, 0, pixels);

	if (d_pzbuffer)
	{
		D_FlushCaches ();
		Hunk_FreeToHighMark (vid_highhunkmark);
		d_pzbuffer = NULL;
	}

	vid_highhunkmark = Hunk_HighMark ();
	d_pzbuffer = (short *) Hunk_HighAllocName (zbuffersize + cachesize, "video");
	vid_surfcache = (byte *)d_pzbuffer + zbuffersize;
	vid_surfcachesize = cachesize;
}

/*
================
VID_SetSoftMode
================
*/
static void VID_SetSoftMode (int mode)
{
	int	width, height;

	if (mode < 0 || mode >= NUM_SOFTMODES)
		mode = 0;

	width = softmodes[mode].width;
	height = softmodes[mode].height;

	in_mode_set = true;
	VID_AllocBuffers (width, height);

	vid.width = vid.conwidth = width;
	vid.height = vid.conheight = height;
	vid.rowbytes = vid.conrowbytes = width;
	vid.buffer = vid.conbuffer = vid.direct = vid_framebuffer;
	vid.aspect = ((float)height / (float)width) * (320.0f / 240.0f);
	vid.numpages = 1;
	vid.recalc_refdef = 1;

	D_InitCaches (vid_surfcache, vid_surfcachesize);
	WebCanvas_SetSource (width, height);

	/* Shared client code (menu.c) sizes its canvas from these. */
	glwidth = width;
	glheight = height;

	vid_current_mode = mode;
	vid_menu_mode = mode;
	in_mode_set = false;
}

static void VID_CheckMode (void)
{
	int	wanted = VID_WantedMode ();

	if (wanted != vid_current_mode)
	{
		VID_SetSoftMode (wanted);
		Con_DPrintf ("software resolution: %s (canvas %dx%d, %s)\n",
			softmodes[vid_current_mode].desc, canvas_width, canvas_height,
			WebCanvas_BackendName ());
	}
}

/*
================
Hexenwail_ResizeCanvas

Called from the PWA launcher whenever the visual viewport changes. Sizes are
CSS pixels; the canvas backing store runs at full device resolution so the
GPU upscale of the software image stays as sharp as the panel allows.

Same export name and same contract as the GL build's copy in gl_vidsdl.c --
web/app.js calls one symbol and must not care which renderer answers.
================
*/
EMSCRIPTEN_KEEPALIVE void Hexenwail_ResizeCanvas (int css_width, int css_height)
{
	int	dw, dh;

	if (!sdl_window || css_width <= 0 || css_height <= 0)
		return;

	SDL_SetWindowSize (sdl_window, css_width, css_height);
	SDL_SyncWindow (sdl_window);
	SDL_GetWindowSizeInPixels (sdl_window, &dw, &dh);
	if (dw <= 0 || dh <= 0)
		return;
	if (dw == canvas_width && dh == canvas_height)
		return;

	canvas_width = dw;
	canvas_height = dh;

	if (vid_initialized)
		VID_CheckMode ();
}

/*
================
VID_QueryCanvasSize

The canvas backing store is the SDL window's pixel size; SDL sizes it from
the CSS size and the device pixel ratio, which is the same arithmetic the
GL build relies on.
================
*/
static void VID_QueryCanvasSize (void)
{
	int	dw = 0, dh = 0;

	if (sdl_window)
		SDL_GetWindowSizeInPixels (sdl_window, &dw, &dh);
	if (dw <= 0 || dh <= 0)
	{
		dw = 1024;
		dh = 768;
	}
	canvas_width = dw;
	canvas_height = dh;
}

static void VID_SoftMode_f (void)
{
	int	i;

	if (Cmd_Argc () < 2)
	{
		Con_Printf ("software resolutions (vid_soft_mode):\n");
		Con_Printf ("  0 : auto (currently %s)\n", softmodes[VID_AutoMode ()].desc);
		for (i = 0; i < NUM_SOFTMODES; ++i)
			Con_Printf ("%s%2d : %s\n", (i == vid_current_mode) ? "* " : "  ",
				i + 1, softmodes[i].desc);
		return;
	}
	Cvar_SetValueQuick (&vid_soft_mode, atoi (Cmd_Argv (1)));
	VID_CheckMode ();
}

/*
================
VID_CreateWindow

The presenter needs a WebGL2 context, and in_sdl.c needs an SDL window to
read events from -- one SDL window with an ES 3.0 context satisfies both.
================
*/
static void VID_CreateWindow (void)
{
	SDL_GL_SetAttribute (SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute (SDL_GL_ALPHA_SIZE, 8);
	/* The presenter blits one opaque textured triangle: no depth, no
	 * stencil, no multisample resolve. */
	SDL_GL_SetAttribute (SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute (SDL_GL_STENCIL_SIZE, 0);
	SDL_GL_SetAttribute (SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute (SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

	if ((SDL_WasInit (SDL_INIT_VIDEO)) == 0)
	{
		if (!SDL_InitSubSystem (SDL_INIT_VIDEO))
			Sys_Error ("Couldn't init video: %s", SDL_GetError());
	}

	sdl_window = SDL_CreateWindow ("Hexen II", 1024, 768,
				SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!sdl_window)
		Sys_Error ("Couldn't create the game window: %s", SDL_GetError());

	sdl_glcontext = SDL_GL_CreateContext (sdl_window);
	if (!sdl_glcontext)
	{
		SDL_DestroyWindow (sdl_window);
		sdl_window = NULL;
		Sys_Error ("Couldn't create gl context: %s", SDL_GetError());
	}
}

void VID_Init (const unsigned char *palette)
{
	Cvar_RegisterVariable (&_enable_mouse);
	Cvar_RegisterVariable (&vid_soft_mode);
	Cvar_RegisterVariable (&vid_soft_filter);
	Cvar_RegisterVariable (&vid_soft_stretch);
	Cmd_AddCommand ("vid_softmode", VID_SoftMode_f);
	/* Before Draw_Init()/R_Init() so config execution sees the same cvar
	 * set the GL build publishes, whichever renderer is linked. */
	R_SoftWebInitCvars ();

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

	VID_CreateWindow ();
	VID_QueryCanvasSize ();
	WebCanvas_Init ();
	WebCanvas_SetFilter (vid_soft_filter.integer != 0);
	VID_SetPalette (palette);
	VID_SetSoftMode (VID_WantedMode ());

	vid_initialized = true;
	Con_SafePrintf ("Software renderer: %s on %s canvas %dx%d\n",
		softmodes[vid_current_mode].desc, WebCanvas_BackendName (),
		canvas_width, canvas_height);
}

void VID_Shutdown (void)
{
	vid_initialized = false;
	WebCanvas_Shutdown ();
	free (vid_framebuffer);
	vid_framebuffer = NULL;
	vid_framebuffer_size = 0;
	vid.buffer = vid.conbuffer = vid.direct = NULL;

	if (sdl_glcontext)
	{
		SDL_GL_DestroyContext (sdl_glcontext);
		sdl_glcontext = NULL;
	}
	if (sdl_window)
	{
		SDL_DestroyWindow (sdl_window);
		sdl_window = NULL;
	}
}

void VID_Update (vrect_t *rects)
{
	int	dx, dy, dw, dh;
	static int last_filter = -1;

	(void) rects;	/* the whole frame is uploaded: GPU scaling is per-frame anyway */

	if (!vid_initialized || !vid_framebuffer)
		return;

	if (last_filter != vid_soft_filter.integer)
	{
		last_filter = vid_soft_filter.integer;
		WebCanvas_SetFilter (last_filter != 0);
	}

	/* The launcher resizes the canvas through SDL, so pick the new backing
	 * store size up here rather than trusting a cached value. */
	{
		int	dw2 = 0, dh2 = 0;

		SDL_GetWindowSizeInPixels (sdl_window, &dw2, &dh2);
		if (dw2 > 0 && dh2 > 0)
		{
			canvas_width = dw2;
			canvas_height = dh2;
		}
	}

	VID_CheckMode ();
	VID_DestRect (vid.width, vid.height, &dx, &dy, &dw, &dh);
	WebCanvas_Present (vid_framebuffer, vid.rowbytes,
		canvas_width, canvas_height, dx, dy, dw, dh);
	SDL_GL_SwapWindow (sdl_window);
}

void VID_SetPalette (const unsigned char *palette)
{
	int	i;

	if (!memcmp (vid_curpal, palette, sizeof(vid_curpal)))
		return;
	memcpy (vid_curpal, palette, sizeof(vid_curpal));

	for (i = 0; i < 256; ++i)
	{
		d_8to24table[i] = palette[i * 3] | (palette[i * 3 + 1] << 8) |
			(palette[i * 3 + 2] << 16) | 0xff000000u;
		d_8to16table[i] = (unsigned short)
			(((palette[i * 3] >> 3) << 11) |
			 ((palette[i * 3 + 1] >> 2) << 5) |
			  (palette[i * 3 + 2] >> 3));
	}
	d_8to24table[255] &= 0x00ffffffu;

	WebCanvas_SetPalette (palette);
}

void VID_ShiftPalette (const unsigned char *palette)
{
	VID_SetPalette (palette);
}

void VID_LockBuffer (void)
{
	if (++lockcount > 1)
		return;

	vid.buffer = vid.conbuffer = vid.direct = vid_framebuffer;
	vid.rowbytes = vid.conrowbytes = vid.width;

	if (r_dowarp)
	{
		d_viewbuffer = r_warpbuffer;
		screenwidth = WARP_WIDTH;
	}
	else
	{
		d_viewbuffer = vid.buffer;
		screenwidth = vid.rowbytes;
	}
}

void VID_UnlockBuffer (void)
{
	--lockcount;
	if (lockcount < 0)
		Sys_Error ("Unbalanced unlock");
}

void VID_HandlePause (qboolean paused)
{
	if (_enable_mouse.integer)
	{
		if (paused)
			IN_DeactivateMouse ();
		else
			IN_ActivateMouse ();
	}
}

/* Fullscreen on the web is the browser's business: web/app.js drives the
 * Fullscreen API on the canvas element. */
void VID_ToggleFullscreen (void) {}
/* No mode cvars on this renderer, so nothing to hold back. */
void VID_Lock (void) {}
void D_ShowLoadingSize (void) {}
void VID_InitMouseCursors (void) {}
void VID_SetMouseCursor (mousecursor_t cursor) { (void)cursor; }

SDL_Window *VID_GetWindow (void)
{
	return sdl_window;
}

qboolean VID_HasMouseOrInputFocus (void)
{
	if (!sdl_window)
		return false;
	return (SDL_GetWindowFlags (sdl_window) &
		(SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_INPUT_FOCUS)) != 0;
}

qboolean VID_IsMinimized (void)
{
	if (!sdl_window)
		return false;
	return (SDL_GetWindowFlags (sdl_window) & SDL_WINDOW_MINIMIZED) != 0;
}

/* The console shares the software framebuffer, so there is no separate
 * console size to scale -- these exist for the shared menu code. */
void VID_ChangeConsize (int dir) { (void)dir; }
float VID_ReportConsize (void) { return (float)vid.conwidth; }
/* Nothing to derive: this target picks its console straight off the resolution
 * ladder and has no width or multiplier cvar to put a number into.  Present so
 * the declaration in vid.h holds for every target that includes it. */
void VID_AutoConScale (void) { }

void VID_MenuInit (void) { vid_menu_mode = vid_current_mode; }
qboolean VID_MenuNeedApply (void) { return false; }
void VID_MenuApply (void) {}
void VID_MenuReset (void) { vid_menu_mode = vid_current_mode; }

const char *VID_MenuGetResolution (qboolean *is_current)
{
	static char	desc[48];

	if (is_current)
		*is_current = true;
	if (vid_soft_mode.integer == 0)
		q_snprintf (desc, sizeof(desc), "auto (%s)", softmodes[vid_current_mode].desc);
	else
		q_strlcpy (desc, softmodes[vid_current_mode].desc, sizeof(desc));
	return desc;
}

const char *VID_MenuGetAspect (void)
{
	return vid_soft_stretch.integer ? "Stretch" : "4:3";
}

int VID_MenuGetWindowMode (void) { return 1; }

int VID_MenuGetMultisample (qboolean *is_current, qboolean *available)
{
	if (is_current) *is_current = true;
	if (available) *available = false;
	return 0;
}

int VID_MenuGetVSync (void) { return 1; }

qboolean VID_MenuGetTexFilter (void) { return vid_soft_filter.integer != 0; }

int VID_MenuGetAnisotropy (qboolean *available)
{
	if (available) *available = false;
	return 1;
}

void VID_MenuAdjustWindowMode (int dir) { (void)dir; }

void VID_MenuAdjustAspect (int dir)
{
	(void) dir;
	Cvar_SetValueQuick (&vid_soft_stretch, vid_soft_stretch.integer ? 0 : 1);
	vid.recalc_refdef = 1;
}

void VID_MenuAdjustResolution (int dir)
{
	int	mode = vid_soft_mode.integer + (dir > 0 ? 1 : -1);

	if (mode < 0)
		mode = NUM_SOFTMODES;
	else if (mode > NUM_SOFTMODES)
		mode = 0;
	Cvar_SetValueQuick (&vid_soft_mode, mode);
	VID_CheckMode ();
}

void VID_MenuAdjustMultisample (int dir) { (void)dir; }
void VID_MenuAdjustVSync (int dir) { (void)dir; }

void VID_MenuAdjustTexFilter (void)
{
	Cvar_SetValueQuick (&vid_soft_filter, vid_soft_filter.integer ? 0 : 1);
}

void VID_MenuAdjustAnisotropy (int dir) { (void)dir; }
