/*
 * gl_vidsdl.c -- SDL GL vid component
 * Select window size and mode and init SDL in GL mode.
 *
 * Changed 7/11/04 by S.A.
 * - Fixed fullscreen opengl mode, window sizes
 * - Options are now: -fullscreen, -height, -width, -bpp
 * - The "-mode" option has been removed
 *
 * Changed 7/01/06 by O.S
 * - Added video modes enumeration via SDL
 * - Added video mode changing on the fly.
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
 * Copyright (C) 1997-1998  Raven Software Corp.
 * Copyright (C) 2004-2005  Steven Atkinson <stevenaaus@yahoo.com>
 * Copyright (C) 2005-2016  O.Sezer <sezero@users.sourceforge.net>
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

#define	__GL_FUNC_EXTERN

#include "quakedef.h"
#include "cfgfile.h"
#include "bgmusic.h"
#include "cdaudio.h"
#include "sdl_inc.h"
#include "gl_postprocess.h"
#include "gl_shader.h"
#include "gl_vbo.h"
#include "filenames.h"


#define WARP_WIDTH		320
#define WARP_HEIGHT		200
#define MAXWIDTH		10000
#define MAXHEIGHT		10000
#define MIN_WIDTH		320
//#define MIN_HEIGHT		200
#define MIN_HEIGHT		240
#define MAX_DESC		33

typedef struct {
	modestate_t	type;
	int			width;
	int			height;
	int			modenum;
	int			fullscreen;
	int			bpp;
	int			halfscreen;
	char		modedesc[MAX_DESC];
} vmode_t;

typedef struct {
	int	width;
	int	height;
} stdmode_t;

#define RES_640X480	3
static const stdmode_t	std_modes[] = {
// NOTE: keep this list in order
	{320, 240},	// 0
	{400, 300},	// 1
	{512, 384},	// 2
	{640, 480},	// 3 == RES_640X480, this is our default, below
			//		this is the lowresmodes region.
			//		either do not change its order,
			//		or change the above define, too
	{800,  600},	// 4, RES_640X480 + 1
	{1024, 768},	// 5, RES_640X480 + 2
	{1280, 1024},	// 6
	{1600, 1200}	// 7
};

#define MAX_MODE_LIST	128
#define MAX_STDMODES	(sizeof(std_modes) / sizeof(std_modes[0]))
#define NUM_LOWRESMODES	(RES_640X480)
static vmode_t	fmodelist[MAX_MODE_LIST+1];	// list of enumerated fullscreen modes
static vmode_t	wmodelist[MAX_STDMODES +1];	// list of standart 4:3 windowed modes
static vmode_t	*modelist;	// modelist in use, points to one of the above lists

static int	num_fmodes;
static int	num_wmodes;
static int	*nummodes;
static int	bpp = 16;

#if defined(H2W)
#	define WM_TITLEBAR_TEXT	"HexenWorld"
#	define WM_ICON_TEXT	"HexenWorld"
//#elif defined(H2MP)
//#	define WM_TITLEBAR_TEXT	"Hexen II+"
//#	define WM_ICON_TEXT	"HEXEN2MP"
#else
#	define WM_TITLEBAR_TEXT	"Hexen II"
#	define WM_ICON_TEXT	"HEXEN2"
#endif

typedef struct {
	int	red,
		green,
		blue,
		alpha,
		depth,
		stencil;
} attributes_t;
static attributes_t	vid_attribs;

static SDL_Window	*window;
static SDL_GLContext	glcontext;
static qboolean	vid_menu_fs;
static qboolean	fs_toggle_works = true;

// vars for vid state
viddef_t	vid;			// global video state
modestate_t	modestate = MS_UNINIT;
static int	vid_default = -1;	// modenum of 640x480 as a safe default
static int	vid_modenum = -1;	// current video mode, set after mode setting succeeds
static int	vid_maxwidth = 640, vid_maxheight = 480;
static qboolean	vid_conscale = false;
static int	WRHeight, WRWidth;

static qboolean	vid_initialized = false;
qboolean	in_mode_set = false;

// cvar vid_mode must be set before calling
// VID_SetMode, VID_ChangeVideoMode or VID_Restart_f
static cvar_t	vid_mode = {"vid_mode", "0", CVAR_NONE};
static cvar_t	vid_config_consize = {"vid_config_consize", "640", CVAR_ARCHIVE};
static cvar_t	vid_config_glx = {"vid_config_glx", "640", CVAR_ARCHIVE};
static cvar_t	vid_config_gly = {"vid_config_gly", "480", CVAR_ARCHIVE};
static cvar_t	vid_config_fscr= {"vid_config_fscr", "1", CVAR_ARCHIVE};
static cvar_t	vid_window_x = {"vid_window_x", "-1", CVAR_ARCHIVE};
static cvar_t	vid_window_y = {"vid_window_y", "-1", CVAR_ARCHIVE};
static cvar_t	vid_vsync = {"vid_vsync", "1", CVAR_ARCHIVE};	/* 0=off, 1=on, -1=adaptive */
extern cvar_t	gl_texture_anisotropy;	/* defined in gl_draw.c */
static cvar_t	vid_borderless = {"vid_borderless", "0", CVAR_ARCHIVE};
// cvars for compatibility with the software version
static cvar_t	vid_config_swx = {"vid_config_swx", "320", CVAR_ARCHIVE};
static cvar_t	vid_config_swy = {"vid_config_swy", "240", CVAR_ARCHIVE};

byte		globalcolormap[VID_GRADES*256];
float		RTint[256], GTint[256], BTint[256];
unsigned short	d_8to16table[256];
unsigned int	d_8to24table[256];
unsigned int	d_8to24TranslucentTable[256];
unsigned char	*inverse_pal;

// gl stuff
static void GL_Init (void);

#ifdef GL_DLSYM
static const char	*gl_library;
#endif

static const char	*gl_vendor;
static const char	*gl_renderer;
static const char	*gl_version;

GLint		gl_max_size = 256;
GLfloat		gl_max_anisotropy;
float		gldepthmin, gldepthmax;

/* Gamma stuff */
#define	USE_GAMMA_RAMPS			0
static qboolean	gammaworks = false;	// whether hw-gamma works

// multisampling
static int	multisample = 0; // do not set this if SDL cannot multisample
static qboolean	sdl_has_multisample = false;
static cvar_t	vid_config_fsaa = {"vid_config_fsaa", "4", CVAR_ARCHIVE};

// stencil buffer
qboolean	have_stencil = false;

// this is useless: things aren't like those in quake
//static qboolean	fullsbardraw = false;

// menu drawing
static void VID_MenuDraw (void);
static void VID_MenuKey (int key);

// input stuff
static void ClearAllStates (void);
static int	enable_mouse;
cvar_t		_enable_mouse = {"_enable_mouse", "1", CVAR_ARCHIVE};


//====================================

//====================================

void VID_LockBuffer (void)
{
// nothing to do
}

void VID_UnlockBuffer (void)
{
// nothing to do
}

void VID_HandlePause (qboolean paused)
{
	if (_enable_mouse.integer/* && (modestate == MS_WINDOWED)*/)
	{
		// for consistency, don't show pointer - S.A
		if (paused)
		{
			IN_DeactivateMouse ();
			// IN_ShowMouse ();
		}
		else
		{
			IN_ActivateMouse ();
			// IN_HideMouse ();
		}
	}
}

SDL_Window *VID_GetWindow (void)
{
	return window;
}

qboolean VID_HasMouseOrInputFocus (void)
{
	return (SDL_GetWindowFlags(window) & (SDL_WINDOW_MOUSE_FOCUS | SDL_WINDOW_INPUT_FOCUS)) != 0;
}

qboolean VID_IsMinimized (void)
{
	/* SDL3: SDL_WINDOW_SHOWN removed; check SDL_WINDOW_MINIMIZED instead */
	return (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
}


//====================================

static void VID_SetIcon (void)
{
/* from Kristian Duske:  "AFAIK, the application icon must be present in
 * Contents/Resources and it must be set in the Info.plist file. It will
 * then be used by Finder and Dock as well as for individual windows
 * unless overridden by a document icon. So SDL_WM_SetIcon() is probably
 * not necessary, and will likely use a low-res image anyway." */
#if !defined(PLATFORM_OSX)

#	include "xbm_icon.h"	/* the xbm data */
	SDL_Surface	*icon;
	SDL_Color	color;
	Uint8		*ptr;
	int		i, mask;
	SDL_Palette	*palette;

	icon = SDL_CreateSurface(hx2icon_width, hx2icon_height, SDL_PIXELFORMAT_INDEX8);
	if (icon == NULL)
		return;

	SDL_SetSurfaceColorKey(icon, true, 0);

	palette = SDL_CreatePalette(256);
	if (palette)
	{
		color.r = 255;
		color.g = 255;
		color.b = 255;
		color.a = 255;
		SDL_SetPaletteColors(palette, &color, 0, 1);	/* just in case */
		color.r = 192;
		color.g = 0;
		color.b = 0;
		color.a = 255;
		SDL_SetPaletteColors(palette, &color, 1, 1);
		SDL_SetSurfacePalette(icon, palette);
		SDL_DestroyPalette(palette);
	}

	ptr = (Uint8 *)icon->pixels;
	/* one bit represents a pixel, black or white:  each
	 * byte in the xbm array contains data for 8 pixels. */
	for (i = 0; i < (int) sizeof(hx2icon_bits); i++)
	{
		for (mask = 1; mask != 0x100; mask <<= 1)
		{
			*ptr = (hx2icon_bits[i] & mask) ? 1 : 0;
			ptr++;
		}
	}

	SDL_SetWindowIcon(window, icon);
	SDL_DestroySurface(icon);
#endif /* !OSX */
}

static void VID_ConWidth (int modenum)
{
	int	w, h;

	if (!vid_conscale)
	{
		Cvar_SetValueQuick (&vid_config_consize, modelist[modenum].width);
		return;
	}

	w = vid_config_consize.integer;
	w &= ~7; /* make it a multiple of eight */
	if (w < MIN_WIDTH)
		w = MIN_WIDTH;
	else if (w > modelist[modenum].width)
		w = modelist[modenum].width;

	h = w * modelist[modenum].height / modelist[modenum].width;
	if (h < 200 /* MIN_HEIGHT */ ||
	    h > modelist[modenum].height || w > modelist[modenum].width)
	{
		vid_conscale = false;
		Cvar_SetValueQuick (&vid_config_consize, modelist[modenum].width);
		return;
	}
	vid.width = vid.conwidth = w;
	vid.height = vid.conheight = h;
	if (w != modelist[modenum].width)
		vid_conscale = true;
	else	vid_conscale = false;
}

void VID_ChangeConsize (int dir)
{
	int	w, h;

	switch (dir)
	{
	case -1: /* smaller text */
		w = ((float)vid.conwidth/(float)vid.width + 0.05f) * vid.width; /* use 0.10f increment ?? */
		w &= ~7; /* make it a multiple of eight */
		if (w > modelist[vid_modenum].width)
			w = modelist[vid_modenum].width;
		break;

	case 1: /* bigger text */
		w = ((float)vid.conwidth/(float)vid.width - 0.05f) * vid.width;
		w &= ~7; /* make it a multiple of eight */
		if (w < MIN_WIDTH)
			w = MIN_WIDTH;
		break;

	default:	/* bad key */
		return;
	}

	h = w * modelist[vid_modenum].height / modelist[vid_modenum].width;
	if (h < 200)
		return;
	vid.width = vid.conwidth = w;
	vid.height = vid.conheight = h;
	Cvar_SetValueQuick (&vid_config_consize, vid.conwidth);
	vid.recalc_refdef = 1;
	if (vid.conwidth != modelist[vid_modenum].width)
		vid_conscale = true;
	else	vid_conscale = false;
}

float VID_ReportConsize(void)
{
	return (float)modelist[vid_modenum].width/vid.conwidth;
}


static qboolean VID_SetMode (int modenum)
{
	int	i, is_fullscreen;
	SDL_DisplayID	display_id;
	const SDL_DisplayMode	*desktop_mode;
	int	screen_w, screen_h, drawable_w, drawable_h;
	int	scaling_factor = 100;

	in_mode_set = true;

	// setup the attributes
	if (bpp >= 32)
	{
		SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 8 );
		SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 8 );
		SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 8 );
		SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE, 8 );
		SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
		SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
	}
	else
	{
		SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 5 );
		SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 5 );
		SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 5 );
		SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 16 );
	}
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

	/* request OpenGL 4.3 compatibility profile — still have some
	 * legacy GL state/enum usage that doesn't work in core profile */
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 4 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 3 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY );

	if (multisample)
	{
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, multisample);
	}

	Con_SafePrintf ("Requested mode %d: %dx%dx%d\n", modenum, modelist[modenum].width, modelist[modenum].height, bpp);

	// SDL3: SDL_CreateWindow no longer takes x,y position params
	{
		Uint32 wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
		if (vid_borderless.integer && !vid_config_fscr.integer)
			wflags |= SDL_WINDOW_BORDERLESS;
		window = SDL_CreateWindow("",
					  modelist[modenum].width, modelist[modenum].height,
					  wflags);
	}

	if (!window)
	{
		if (!multisample)
			Sys_Error ("Couldn't set video mode: %s", SDL_GetError());
		else
		{
			Con_SafePrintf ("multisample window failed\n");
			multisample = 0;
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, multisample);
			window = SDL_CreateWindow("",
						  modelist[modenum].width, modelist[modenum].height,
						  SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
			if (!window)
				Sys_Error ("Couldn't set video mode: %s", SDL_GetError());
		}
	}

	// Restore saved window position, or center on first run
	if (!vid_config_fscr.integer && vid_window_x.integer >= 0 && vid_window_y.integer >= 0)
		SDL_SetWindowPosition(window, vid_window_x.integer, vid_window_y.integer);
	else
		SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

	// Check HiDPI scaling
	SDL_GetWindowSize(window, &screen_w, &screen_h);
	SDL_GetWindowSizeInPixels(window, &drawable_w, &drawable_h);
	scaling_factor = (100 * drawable_w) / screen_w;
	if (scaling_factor != 100)
		Con_Printf ("High DPI scaling in effect! (%d%%)\n", scaling_factor);

	// Handle fullscreen after window creation
	if (vid_config_fscr.integer)
	{
		// Check if resolution matches desktop
		// (SDL3 fullscreen uses borderless by default when matching desktop res)
		display_id = SDL_GetDisplayForWindow(window);
		desktop_mode = SDL_GetDesktopDisplayMode(display_id);
		if (desktop_mode && screen_w == desktop_mode->w && screen_h == desktop_mode->h)
			Con_Printf ("Fullscreen res matches desktop\n");

		// SDL3: just use SDL_WINDOW_FULLSCREEN, no more FULLSCREEN_DESKTOP
		SDL_SetWindowFullscreen(window, true);
		SDL_SyncWindow(window);
	}

	glcontext = SDL_GL_CreateContext(window);
	if (!glcontext)
	{
		SDL_DestroyWindow(window);
		window = NULL;
		Sys_Error ("Couldn't create gl context: %s", SDL_GetError());
	}

	VID_SetIcon();
	SDL_SetWindowTitle(window, WM_TITLEBAR_TEXT);

	// Apply vsync: 0=off, 1=on, -1=adaptive
	if (!SDL_GL_SetSwapInterval(vid_vsync.integer))
	{
		if (vid_vsync.integer == -1)	// adaptive failed, try normal
			SDL_GL_SetSwapInterval(1);
	}

	// SDL3: only SDL_WINDOW_FULLSCREEN, no FULLSCREEN_DESKTOP
	is_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) ? 1 : 0;

	/* Use drawable pixel size for rendering dimensions.
	 * SDL3 HiDPI: logical size can differ from actual framebuffer size. */
	SDL_GetWindowSizeInPixels(window, &drawable_w, &drawable_h);
	WRWidth = drawable_w;
	WRHeight = drawable_h;

	// set vid_modenum properly and adjust other vars
	vid_modenum = modenum;
	modestate = (is_fullscreen) ? MS_FULLDIB : MS_WINDOWED;
	Cvar_SetValueQuick (&vid_config_glx, WRWidth);
	Cvar_SetValueQuick (&vid_config_gly, WRHeight);
	Cvar_SetValueQuick (&vid_config_fscr, is_fullscreen);
	vid.width = vid.conwidth = WRWidth;
	vid.height = vid.conheight = WRHeight;

	// setup the effective console width
	VID_ConWidth(modenum);

	SDL_GL_GetAttribute(SDL_GL_BUFFER_SIZE, &i);
	Con_SafePrintf ("Video Mode Set : %dx%dx%d\n", modelist[modenum].width, modelist[modenum].height, i);
	if (multisample)
	{
		SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &multisample);
		Con_SafePrintf ("multisample buffer with %i samples\n", multisample);
	}
	Cvar_SetValueQuick (&vid_config_fsaa, multisample);

	IN_HideMouse ();

	in_mode_set = false;

	return true;
}


//====================================

static void VID_InitGamma (void)
{
	gammaworks = false;
	/* SDL2 removed standalone SDL_SetGamma/SDL_GetGammaRamp;
	 * For now, hw-gamma via SDL is not supported. */

	if (!gammaworks)
		Con_SafePrintf("gamma adjustment not available\n");
}

static void VID_ShutdownGamma (void)
{
	/* hw-gamma restore via SDL not supported */
}

static void VID_SetGamma (void)
{
	/* hw-gamma via SDL not supported */
}

void VID_ShiftPalette (const unsigned char *palette)
{
	VID_SetGamma();
}


/* GL_CompileShader and GL_LinkProgram are now in gl_shader.c */

static void GL_LoadFunctionPointers (void)
{
	/* load shader function pointers */
	glCreateShader_fp = (glCreateShader_f) SDL_GL_GetProcAddress("glCreateShader");
	glDeleteShader_fp = (glDeleteShader_f) SDL_GL_GetProcAddress("glDeleteShader");
	glShaderSource_fp = (glShaderSource_f) SDL_GL_GetProcAddress("glShaderSource");
	glCompileShader_fp = (glCompileShader_f) SDL_GL_GetProcAddress("glCompileShader");
	glGetShaderiv_fp = (glGetShaderiv_f) SDL_GL_GetProcAddress("glGetShaderiv");
	glGetShaderInfoLog_fp = (glGetShaderInfoLog_f) SDL_GL_GetProcAddress("glGetShaderInfoLog");
	glCreateProgram_fp = (glCreateProgram_f) SDL_GL_GetProcAddress("glCreateProgram");
	glDeleteProgram_fp = (glDeleteProgram_f) SDL_GL_GetProcAddress("glDeleteProgram");
	glAttachShader_fp = (glAttachShader_f) SDL_GL_GetProcAddress("glAttachShader");
	glLinkProgram_fp = (glLinkProgram_f) SDL_GL_GetProcAddress("glLinkProgram");
	glUseProgram_fp = (glUseProgram_f) SDL_GL_GetProcAddress("glUseProgram");
	glGetProgramiv_fp = (glGetProgramiv_f) SDL_GL_GetProcAddress("glGetProgramiv");
	glGetProgramInfoLog_fp = (glGetProgramInfoLog_f) SDL_GL_GetProcAddress("glGetProgramInfoLog");
	glGetUniformLocation_fp = (glGetUniformLocation_f) SDL_GL_GetProcAddress("glGetUniformLocation");
	glUniform1i_fp = (glUniform1i_f) SDL_GL_GetProcAddress("glUniform1i");
	glUniform1f_fp = (glUniform1f_f) SDL_GL_GetProcAddress("glUniform1f");
	glUniform3f_fp = (glUniform3f_f) SDL_GL_GetProcAddress("glUniform3f");
	glUniform4f_fp = (glUniform4f_f) SDL_GL_GetProcAddress("glUniform4f");
	glUniformMatrix4fv_fp = (glUniformMatrix4fv_f) SDL_GL_GetProcAddress("glUniformMatrix4fv");
	glBindAttribLocation_fp = (glBindAttribLocation_f) SDL_GL_GetProcAddress("glBindAttribLocation");

	/* VBO/VAO functions */
	glGenBuffers_fp = (glGenBuffers_f) SDL_GL_GetProcAddress("glGenBuffers");
	glDeleteBuffers_fp = (glDeleteBuffers_f) SDL_GL_GetProcAddress("glDeleteBuffers");
	glBindBuffer_fp = (glBindBuffer_f) SDL_GL_GetProcAddress("glBindBuffer");
	glBufferData_fp = (glBufferData_f) SDL_GL_GetProcAddress("glBufferData");
	glBufferSubData_fp = (glBufferSubData_f) SDL_GL_GetProcAddress("glBufferSubData");
	glGenVertexArrays_fp = (glGenVertexArrays_f) SDL_GL_GetProcAddress("glGenVertexArrays");
	glDeleteVertexArrays_fp = (glDeleteVertexArrays_f) SDL_GL_GetProcAddress("glDeleteVertexArrays");
	glBindVertexArray_fp = (glBindVertexArray_f) SDL_GL_GetProcAddress("glBindVertexArray");
	glVertexAttribPointer_fp = (glVertexAttribPointer_f) SDL_GL_GetProcAddress("glVertexAttribPointer");
	glEnableVertexAttribArray_fp = (glEnableVertexAttribArray_f) SDL_GL_GetProcAddress("glEnableVertexAttribArray");
	glDrawArrays_fp = (glDrawArrays_f) SDL_GL_GetProcAddress("glDrawArrays");
	glDrawElements_fp = (glDrawElements_f) SDL_GL_GetProcAddress("glDrawElements");

	if (!glCreateShader_fp || !glShaderSource_fp || !glCompileShader_fp ||
	    !glCreateProgram_fp || !glAttachShader_fp || !glLinkProgram_fp ||
	    !glUseProgram_fp || !glGetUniformLocation_fp || !glUniform1i_fp ||
	    !glUniform1f_fp)
	{
		Sys_Error("Required GL 4.3 shader functions not found");
	}
}


#ifdef GL_DLSYM
static qboolean GL_OpenLibrary (const char *name)
{
	int	ret;
	char	gl_liblocal[MAX_OSPATH];

	ret = SDL_GL_LoadLibrary(name);

	if (ret == -1)
	{
		// In case of user-specified gl library, look for it under the
		// installation directory, too: the user may forget providing
		// a valid path information. In that case, make sure it doesnt
		// contain any path information and exists in our own basedir,
		// then try loading it
		if (name != NULL &&
		    FIND_FIRST_DIRSEP(name) == NULL && !HAS_DRIVE_SPEC(name))
		{
			FS_MakePath_BUF (FS_BASEDIR, NULL, gl_liblocal, MAX_OSPATH, name);
			if (! (Sys_FileType(gl_liblocal) & FS_ENT_FILE))
				return false;

			Con_SafePrintf ("Failed loading gl library %s\n"
					"Trying to load %s\n", name, gl_liblocal);

			ret = SDL_GL_LoadLibrary(gl_liblocal);
			if (ret == -1)
				return false;

			Con_SafePrintf("Using GL library: %s\n", gl_liblocal);
			return true;
		}

		return false;
	}

	if (name)
		Con_SafePrintf("Using GL library: %s\n", name);
	else
		Con_SafePrintf("Using system GL library\n");

	return true;
}
#endif	/* GL_DLSYM */


#ifdef GL_DLSYM
static void GL_Init_Functions (void)
{
#define GL_FUNCTION(ret, func, params)				\
    do {							\
	func##_fp = (func##_f) SDL_GL_GetProcAddress(#func);	\
	if (func##_fp == NULL)					\
		Sys_Error("%s not found in GL library", #func);	\
    } while (0);
#define GL_FUNCTION_OPT(ret, func, params)
#include "gl_func.h"
}
#endif	/* GL_DLSYM */

static void GL_ResetFunctions (void)
{
#ifdef	GL_DLSYM
#define GL_FUNCTION(ret, func, params)	\
	func##_fp = NULL;
#endif
#define GL_FUNCTION_OPT(ret, func, params) \
	func##_fp = NULL;
#include "gl_func.h"

	have_stencil = false;
}

/*
===============
GL_Init
===============
*/
static void GL_Init (void)
{
#ifdef GL_DLSYM
	// initialize gl function pointers
	GL_Init_Functions();
#endif

	// collect the visual attributes
	memset (&vid_attribs, 0, sizeof(attributes_t));
	SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &vid_attribs.red);
	SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &vid_attribs.green);
	SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &vid_attribs.blue);
	SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &vid_attribs.alpha);
	SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &vid_attribs.depth);
	SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &vid_attribs.stencil);
	Con_SafePrintf ("R:%d G:%d B:%d A:%d, Z:%d, S:%d\n",
			vid_attribs.red, vid_attribs.green, vid_attribs.blue, vid_attribs.alpha,
			vid_attribs.depth, vid_attribs.stencil);

	gl_vendor = (const char *)glGetString_fp (GL_VENDOR);
	Con_SafePrintf ("GL_VENDOR: %s\n", gl_vendor);
	gl_renderer = (const char *)glGetString_fp (GL_RENDERER);
	Con_SafePrintf ("GL_RENDERER: %s\n", gl_renderer);

	gl_version = (const char *)glGetString_fp (GL_VERSION);
	Con_SafePrintf ("GL_VERSION: %s\n", gl_version);

	glGetIntegerv_fp(GL_MAX_TEXTURE_SIZE, &gl_max_size);
	if (gl_max_size < 256)	// Refuse to work when less than 256
		Sys_Error ("hardware capable of min. 256k opengl texture size needed");
	Con_SafePrintf("OpenGL max.texture size: %i\n", (int) gl_max_size);

	/* GL 4.3: multitexture is always available */
	glActiveTextureARB_fp = (glActiveTextureARB_f) SDL_GL_GetProcAddress("glActiveTexture");
	if (!glActiveTextureARB_fp)
		Sys_Error("glActiveTexture not found");
	glActiveTextureARB_fp(GL_TEXTURE0_ARB);

	/* GL 4.3: anisotropic filtering is always available */
	gl_max_anisotropy = 1;
	glGetFloatv_fp(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &gl_max_anisotropy);
	Con_SafePrintf("Anisotropic filtering: max %.1f\n", gl_max_anisotropy);

	/* NPOT textures: always available on GL 4.3 */
	Con_SafePrintf("NPOT textures enabled\n");

	/* stencil buffer */
	have_stencil = !!vid_attribs.stencil;

	/* load shader and VBO function pointers */
	GL_LoadFunctionPointers();
	GL_Shaders_Init();
	GL_VBO_Init();
	GL_PostProcess_Init();

//	glClearColor_fp(1,0,0,0);
	glCullFace_fp(GL_FRONT);
#if 0 /* causes side effects at least in 16 bpp.  */
	/* Get rid of Z-fighting for textures by offsetting the
	 * drawing of entity models compared to normal polygons.
	 * (See: R_DrawBrushModel.) */
	glPolygonOffset_fp(0.05f, 25.0f);
#endif /* #if 0 */

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glBlendFunc_fp (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	if (multisample)
	{
		glEnable_fp (GL_MULTISAMPLE_ARB);
		Con_SafePrintf ("enabled %i sample fsaa\n", multisample);
	}
}

/*
=================
GL_BeginRendering
=================
*/
void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
	*x = *y = 0;
	*width = WRWidth;
	*height = WRHeight;

//	glViewport_fp (*x, *y, *width, *height);
}


void GL_EndRendering (void)
{
	if (!scr_skipupdate)
		SDL_GL_SwapWindow(window);

// handle the mouse state when windowed if that's changed
	if (_enable_mouse.integer != enable_mouse /*&& modestate == MS_WINDOWED*/)
	{
		if (_enable_mouse.integer)
			IN_ActivateMouse ();
		else	IN_DeactivateMouse ();

		enable_mouse = _enable_mouse.integer;
	}

//	if (fullsbardraw)
//		Sbar_Changed();
}


const int ColorIndex[16] = {
	0, 31, 47, 63, 79, 95, 111, 127, 143, 159, 175, 191, 199, 207, 223, 231
};

const unsigned int ColorPercent[16] = {
	25, 51, 76, 102, 114, 127, 140, 153, 165, 178, 191, 204, 216, 229, 237, 247
};

#define	INVERSE_PALNAME	"gfx/invpal.lmp"
static int ConvertTrueColorToPal (const unsigned char *true_color, const unsigned char *palette)
{
	int	i;
	long	min_dist;
	int	min_index;
	long	r, g, b;

	min_dist = 256 * 256 + 256 * 256 + 256 * 256;
	min_index = -1;
	r = (long) true_color[0];
	g = (long) true_color[1];
	b = (long) true_color[2];

	for (i = 0; i < 256; i++)
	{
		long palr, palg, palb, dist;
		long dr, dg, db;

		palr = palette[3*i];
		palg = palette[3*i+1];
		palb = palette[3*i+2];
		dr = palr - r;
		dg = palg - g;
		db = palb - b;
		dist = dr * dr + dg * dg + db * db;
		if (dist < min_dist)
		{
			min_dist = dist;
			min_index = i;
		}
	}
	return min_index;
}

static void VID_CreateInversePalette (const unsigned char *palette)
{
	long	r, g, b;
	long	idx = 0;
	unsigned char	true_color[3];

	Con_SafePrintf ("Creating inverse palette\n");

	for (r = 0; r < (1 << INVERSE_PAL_R_BITS); r++)
	{
		for (g = 0; g < (1 << INVERSE_PAL_G_BITS); g++)
		{
			for (b = 0; b < (1 << INVERSE_PAL_B_BITS); b++)
			{
				true_color[0] = (unsigned char)(r << (8 - INVERSE_PAL_R_BITS));
				true_color[1] = (unsigned char)(g << (8 - INVERSE_PAL_G_BITS));
				true_color[2] = (unsigned char)(b << (8 - INVERSE_PAL_B_BITS));
				inverse_pal[idx] = ConvertTrueColorToPal(true_color, palette);
				idx++;
			}
		}
	}

	FS_CreatePath(FS_MakePath(FS_USERDIR, NULL, INVERSE_PALNAME));
	FS_WriteFile (INVERSE_PALNAME, inverse_pal, INVERSE_PAL_SIZE);
}

static void VID_InitPalette (const unsigned char *palette)
{
	const unsigned char	*pal;
	unsigned short	r, g, b;
	unsigned short	i, p, c;
	unsigned int	v, *table;
	int		mark;

#if ENDIAN_RUNTIME_DETECT
	switch (host_byteorder)
	{
	case BIG_ENDIAN:	/* R G B A */
		MASK_r	=	0xff000000;
		MASK_g	=	0x00ff0000;
		MASK_b	=	0x0000ff00;
		MASK_a	=	0x000000ff;
		SHIFT_r	=	24;
		SHIFT_g	=	16;
		SHIFT_b	=	8;
		SHIFT_a	=	0;
		break;
	case LITTLE_ENDIAN:	/* A B G R */
		MASK_r	=	0x000000ff;
		MASK_g	=	0x0000ff00;
		MASK_b	=	0x00ff0000;
		MASK_a	=	0xff000000;
		SHIFT_r	=	0;
		SHIFT_g	=	8;
		SHIFT_b	=	16;
		SHIFT_a	=	24;
		break;
	default:
		break;
	}
	MASK_rgb	=	(MASK_r|MASK_g|MASK_b);
#endif	/* ENDIAN_RUNTIME_DETECT */

//
// 8 8 8 encoding
//
	pal = palette;
	table = d_8to24table;
	for (i = 0; i < 256; i++)
	{
		r = pal[0];
		g = pal[1];
		b = pal[2];
		pal += 3;

		v = (255 << SHIFT_a) + (r << SHIFT_r) + (g << SHIFT_g) + (b << SHIFT_b);
		*table++ = v;
	}

	d_8to24table[255] &= MASK_rgb;	// 255 is transparent

	pal = palette;
	table = d_8to24TranslucentTable;

	for (i = 0; i < 16; i++)
	{
		c = ColorIndex[i] * 3;

		r = pal[c];
		g = pal[c + 1];
		b = pal[c + 2];

		for (p = 0; p < 16; p++)
		{
			v = (ColorPercent[15 - p] << SHIFT_a) + (r << SHIFT_r) + (g << SHIFT_g) + (b << SHIFT_b);
			*table++ = v;

			RTint[i*16 + p] = ((float)r) / ((float)ColorPercent[15-p]);
			GTint[i*16 + p] = ((float)g) / ((float)ColorPercent[15-p]);
			BTint[i*16 + p] = ((float)b) / ((float)ColorPercent[15-p]);
		}
	}

	// Initialize the palettized textures data
	mark = Hunk_LowMark ();
	inverse_pal = (unsigned char *) FS_LoadHunkFile (INVERSE_PALNAME, NULL);
	if (inverse_pal != NULL && fs_filesize != INVERSE_PAL_SIZE)
	{
		Hunk_FreeToLowMark (mark);
		inverse_pal = NULL;
	}
	if (inverse_pal == NULL)
	{
		inverse_pal = (unsigned char *) Hunk_AllocName (INVERSE_PAL_SIZE + 1, INVERSE_PALNAME);
		VID_CreateInversePalette (palette);
	}
}

void VID_SetPalette (const unsigned char *palette)
{
// nothing to do
}


/*
===================================================================

MAIN WINDOW

===================================================================
*/

/*
================
ClearAllStates
================
*/
static void ClearAllStates (void)
{
	Key_ClearStates ();
	IN_ClearStates ();
}

/*
=================
VID_ChangeVideoMode
intended only as a callback for VID_Restart_f
=================
*/
static void VID_ChangeVideoMode (int newmode)
{
	int	temp;

	if (!window)
		return;

	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;

	// restore gamma, reset gamma function pointers
	VID_ShutdownGamma();
	CDAudio_Pause ();
	BGM_Pause ();
	S_ClearBuffer ();

	// Shut down GPU resources before destroying GL context
	GL_PostProcess_Shutdown();
	GL_VBO_Shutdown();
	GL_Shaders_Shutdown();

	// Unload all textures and reset texture counts
	D_ClearOpenGLTextures(0);
	memset (lightmap_textures, 0, sizeof(lightmap_textures));

	// reset all opengl function pointers
	GL_ResetFunctions();

	// Avoid re-registering commands and re-allocating memory
	draw_reinit = true;

	// temporarily disable input devices
	IN_DeactivateMouse();
	IN_ShowMouse ();

	// Kill device and rendering contexts
	SDL_GL_DestroyContext(glcontext);
	SDL_DestroyWindow(window);
	SDL_QuitSubSystem(SDL_INIT_VIDEO);	// also unloads the opengl driver

	// re-init sdl_video, set the mode and re-init opengl
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
		Sys_Error ("Couldn't init video: %s", SDL_GetError());
#ifdef GL_DLSYM
	if (!GL_OpenLibrary(gl_library))
		Sys_Error ("Unable to load GL library %s", (gl_library != NULL) ? gl_library : SDL_GetError());
#endif
	VID_SetMode (newmode);

	// Reload graphics wad file (Draw_PicFromWad writes glpic_t data (sizes,
	// texnums) right on top of the original pic data, so the pic data will
	// be dirty after gl textures are loaded the first time; we need to load
	// a clean version)
	W_LoadWadFile ("gfx.wad");

	// Initialize extensions and default OpenGL parameters
	GL_Init();
	VID_InitGamma();

	// Reload pre-map pics, fonts, console, etc
	Draw_Init();
	SCR_Init();
	// R_Init() stuff:
	R_InitParticleTexture();
	R_InitExtraTextures ();
#if defined(H2W)
	R_InitNetgraphTexture();
#endif	/* H2W */
	Sbar_Init();
	vid.recalc_refdef = 1;

	IN_ReInit ();
	ClearAllStates ();
	CDAudio_Resume ();
	BGM_Resume ();

	// Reload model textures and player skins
	Mod_ReloadTextures();
	// rebuild the lightmaps
	GL_BuildLightmaps();
	// finished reloading all images
	draw_reinit = false;
	scr_disabled_for_loading = temp;
	// apply our gamma
	VID_ShiftPalette(NULL);
}

static void VID_Restart_f (void)
{
	if (vid_mode.integer < 0 || vid_mode.integer >= *nummodes)
	{
		Con_Printf ("Bad video mode %d\n", vid_mode.integer);
		Cvar_SetValueQuick (&vid_mode, vid_modenum);
		return;
	}

	Con_Printf ("Re-initializing video:\n");
	VID_ChangeVideoMode (vid_mode.integer);
}

static int sort_modes (const void *arg1, const void *arg2)
{
	const vmode_t *a1, *a2;
	a1 = (vmode_t *) arg1;
	a2 = (vmode_t *) arg2;

#if 0
	/* low to high bpp ? */
	if (a1->bpp != a2->bpp)
		return a1->bpp - a2->bpp;
#endif
	/* lowres to highres */
	if (a1->width == a2->width)
		return a1->height - a2->height;
	return a1->width - a2->width;
}

static void VID_PrepareModes (void)
{
	int	i, j, k;
	qboolean	not_multiple;

	num_fmodes = 0;
	num_wmodes = 0;

	// Add the standart 4:3 modes to the windowed modes list
	// In an unlikely case that we receive no fullscreen modes,
	// this will be our modes list (kind of...)
	for (i = 0; i < (int)MAX_STDMODES; i++)
	{
		wmodelist[num_wmodes].width = std_modes[i].width;
		wmodelist[num_wmodes].height = std_modes[i].height;
		wmodelist[num_wmodes].halfscreen = 0;
		wmodelist[num_wmodes].fullscreen = 0;
		wmodelist[num_wmodes].bpp = 16;
		q_snprintf (wmodelist[num_wmodes].modedesc, MAX_DESC,
				"%d x %d", std_modes[i].width, std_modes[i].height);
		num_wmodes++;
	}

	// Build fullscreen mode list from SDL3 display mode enumeration
	{
		SDL_DisplayID	*displays;
		int		num_displays = 0;

		displays = SDL_GetDisplays(&num_displays);
		if (displays)
		{
			for (i = 0; i < num_displays; i++)
			{
				int			mode_count = 0;
				SDL_DisplayMode		**modes;

				modes = SDL_GetFullscreenDisplayModes(displays[i], &mode_count);
				if (!modes) continue;

				for (j = 0; j < mode_count && num_fmodes < MAX_MODE_LIST; j++)
				{
					const SDL_DisplayMode *mode = modes[j];

					// avoid multiple listings of the same dimension
					not_multiple = true;
					for (k = 0; k < num_fmodes; ++k)
					{
						if (fmodelist[k].width == mode->w && fmodelist[k].height == mode->h)
						{
							not_multiple = false;
							break;
						}
					}

					// avoid resolutions < 320x240
					if (not_multiple && mode->w >= MIN_WIDTH && mode->h >= MIN_HEIGHT)
					{
						fmodelist[num_fmodes].width = mode->w;
						fmodelist[num_fmodes].height = mode->h;
						fmodelist[num_fmodes].halfscreen = 0;
						fmodelist[num_fmodes].fullscreen = 1;
						fmodelist[num_fmodes].bpp = 16;
						q_snprintf (fmodelist[num_fmodes].modedesc, MAX_DESC, "%d x %d", mode->w, mode->h);
						num_fmodes++;
					}
				}
				SDL_free(modes);
			}
			SDL_free(displays);
		}
	}

	if (!num_fmodes)
	{
		Con_SafePrintf ("No fullscreen video modes available\n");
		num_wmodes = RES_640X480 + 1;
		modelist = wmodelist;
		nummodes = &num_wmodes;
		vid_default = RES_640X480;
		Cvar_SetValueQuick (&vid_config_glx, modelist[vid_default].width);
		Cvar_SetValueQuick (&vid_config_gly, modelist[vid_default].height);
		return;
	}

	// At his point, we have a list of valid fullscreen modes:
	// Let's bind to it and use it for windowed modes, as well.
	// The only downside is that if SDL doesn't report any low
	// resolutions to us, we shall not have any for windowed
	// rendering where they would be perfectly legitimate...
	// Since our fullscreen/windowed toggling is instant and
	// doesn't require a vid_restart, switching lists won't be
	// feasible, either. The -width/-height commandline args
	// remain as the user's trusty old friends here.
	nummodes = &num_fmodes;
	modelist = fmodelist;

	// SDL versions older than 1.2.8 have sorting problems
	if (num_fmodes > 1)
		qsort(fmodelist, num_fmodes, sizeof fmodelist[0], sort_modes);

	vid_maxwidth = fmodelist[num_fmodes-1].width;
	vid_maxheight = fmodelist[num_fmodes-1].height;

	// find the 640x480 default resolution. this shouldn't fail
	// at all (for any adapter suporting the VGA/XGA legacy).
	for (i = 0; i < num_fmodes; i++)
	{
		if (fmodelist[i].width == 640 && fmodelist[i].height == 480)
		{
			vid_default = i;
			break;
		}
	}

	if (vid_default < 0)
	{
		// No 640x480? Unexpected, at least today..
		// Easiest thing is to set the default mode
		// as the highest reported one.
		Con_SafePrintf ("WARNING: 640x480 not found in fullscreen modes\n"
				"Using the largest reported dimension as default\n");
		vid_default = num_fmodes-1;
	}

	// limit the windowed (standart) modes list to desktop dimensions
	for (i = 0; i < num_wmodes; i++)
	{
		if (wmodelist[i].width > vid_maxwidth || wmodelist[i].height > vid_maxheight)
			break;
	}
	if (i < num_wmodes)
		num_wmodes = i;

	Cvar_SetValueQuick (&vid_config_glx, modelist[vid_default].width);
	Cvar_SetValueQuick (&vid_config_gly, modelist[vid_default].height);
}

static void VID_ListModes_f (void)
{
	int	i;

	Con_Printf ("Maximum allowed mode: %d x %d\n", vid_maxwidth, vid_maxheight);
	Con_Printf ("Windowed modes enabled:\n");
	for (i = 0; i < num_wmodes; i++)
		Con_Printf ("%2d:  %d x %d\n", i, wmodelist[i].width, wmodelist[i].height);
	Con_Printf ("Fullscreen modes enumerated:");
	if (num_fmodes)
	{
		Con_Printf ("\n");
		for (i = 0; i < num_fmodes; i++)
			Con_Printf ("%2d:  %d x %d\n", i, fmodelist[i].width, fmodelist[i].height);
	}
	else
	{
		Con_Printf (" None\n");
	}
}

static void VID_NumModes_f (void)
{
	Con_Printf ("%d video modes in current list\n", *nummodes);
}

/*
===================
VID_Init
===================
*/
void	VID_Init (const unsigned char *palette)
{
#ifndef __MORPHOS__
	static char nvidia_env_vsync[32] = "__GL_SYNC_TO_VBLANK=1";
#endif
	int	i, temp, width, height;
	const char	*read_vars[] = {
				"vid_config_fscr",
				"vid_config_fsaa",
				"vid_config_glx",
				"vid_config_gly",
				"vid_config_consize",
				"gl_lightmapfmt" };
#define num_readvars	( sizeof(read_vars)/sizeof(read_vars[0]) )

	Cvar_RegisterVariable (&vid_config_fsaa);
	Cvar_RegisterVariable (&vid_config_fscr);
	Cvar_RegisterVariable (&vid_window_x);
	Cvar_RegisterVariable (&vid_window_y);
	Cvar_RegisterVariable (&vid_vsync);
	Cvar_RegisterVariable (&vid_borderless);
	Cvar_RegisterVariable (&vid_config_swy);
	Cvar_RegisterVariable (&vid_config_swx);
	Cvar_RegisterVariable (&vid_config_gly);
	Cvar_RegisterVariable (&vid_config_glx);
	Cvar_RegisterVariable (&vid_config_consize);
	Cvar_RegisterVariable (&vid_mode);
	Cvar_RegisterVariable (&_enable_mouse);
	Cvar_RegisterVariable (&gl_lightmapfmt);

	Cmd_AddCommand ("vid_listmodes", VID_ListModes_f);
	Cmd_AddCommand ("vid_nummodes", VID_NumModes_f);
	Cmd_AddCommand ("vid_restart", VID_Restart_f);

	VID_InitPalette (palette);

	vid.numpages = 2;

	// SDL3 always supports multisampling
	sdl_has_multisample = true;

#ifndef __MORPHOS__
	// enable vsync for nvidia geforce or newer - S.A
	if (COM_CheckParm("-sync") || COM_CheckParm("-vsync"))
	{
		putenv(nvidia_env_vsync);
		Con_SafePrintf ("Nvidia GL vsync enabled\n");
	}
#endif

	// init sdl
	// the first check is actually unnecessary
	if ((SDL_WasInit(SDL_INIT_VIDEO)) == 0)
	{
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
			Sys_Error ("Couldn't init video: %s", SDL_GetError());
	}

#ifdef GL_DLSYM
	i = COM_CheckParm("--gllibrary");
	if (i == 0)
		i = COM_CheckParm ("-gllibrary");
	if (i == 0)
		i = COM_CheckParm ("-g");
	if (i && i < com_argc - 1)
		gl_library = com_argv[i+1];
	else
		gl_library = NULL;	// trust SDL's wisdom here

	// load the opengl library
	if (!GL_OpenLibrary(gl_library))
		Sys_Error ("Unable to load GL library %s", (gl_library != NULL) ? gl_library : SDL_GetError());
#endif

	i = COM_CheckParm("-bpp");
	if (i && i < com_argc-1)
	{
		bpp = atoi(com_argv[i+1]);
	}

	// prepare the modelists, find the actual modenum for vid_default
	VID_PrepareModes();

	// set vid_mode to our safe default first
	Cvar_SetValueQuick (&vid_mode, vid_default);

	// perform an early read of config.cfg
	CFG_ReadCvars (read_vars, num_readvars);

	// windowed mode is default
	// see if the user wants fullscreen
	if (COM_CheckParm("-fullscreen") || COM_CheckParm("-f"))
	{
		Cvar_SetQuick (&vid_config_fscr, "1");
	}
	else if (COM_CheckParm("-window") || COM_CheckParm("-w"))
	{
		Cvar_SetQuick (&vid_config_fscr, "0");
	}

	if (vid_config_fscr.integer && !num_fmodes) // FIXME: see below, as well
		Sys_Error ("No fullscreen modes available at this color depth");

	width = vid_config_glx.integer;
	height = vid_config_gly.integer;

	/* Default to desktop resolution for fullscreen on first run (640x480 = no saved config) */
	if (vid_config_fscr.integer && width == 640 && height == 480)
	{
		const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
		if (dm)
		{
			width = dm->w;
			height = dm->h;
			Cvar_SetValueQuick (&vid_config_glx, width);
			Cvar_SetValueQuick (&vid_config_gly, height);
		}
	}

	if (vid_config_consize.integer != width)
		vid_conscale = true;

	// user is always right ...
	i = COM_CheckParm("-width");
	if (i && i < com_argc-1)
	{	// FIXME: this part doesn't know about a disaster case
		// like we aren't reported any fullscreen modes.
		width = atoi(com_argv[i+1]);

		i = COM_CheckParm("-height");
		if (i && i < com_argc-1)
			height = atoi(com_argv[i+1]);
		else	// proceed with 4/3 ratio
			height = 3 * width / 4;
	}

	// user requested a mode either from the config or from the
	// command line
	// scan existing modes to see if this is already available
	// if not, add this as the last "valid" video mode and set
	// vid_mode to it only if it doesn't go beyond vid_maxwidth
	i = 0;
	while (i < *nummodes)
	{
		if (modelist[i].width == width && modelist[i].height == height)
			break;
		i++;
	}
	if (i < *nummodes)
	{
		Cvar_SetValueQuick (&vid_mode, i);
	}
	else if ( (width <= vid_maxwidth && width >= MIN_WIDTH &&
		   height <= vid_maxheight && height >= MIN_HEIGHT) ||
		  COM_CheckParm("-force") )
	{
		modelist[*nummodes].width = width;
		modelist[*nummodes].height = height;
		modelist[*nummodes].halfscreen = 0;
		modelist[*nummodes].fullscreen = 1;
		modelist[*nummodes].bpp = 16;
		q_snprintf (modelist[*nummodes].modedesc, MAX_DESC, "%d x %d (user mode)", width, height);
		Cvar_SetValueQuick (&vid_mode, *nummodes);
		(*nummodes)++;
	}
	else
	{
		Con_SafePrintf ("ignoring invalid -width and/or -height arguments\n");
	}

	if (!vid_conscale)
		Cvar_SetValueQuick (&vid_config_consize, width);

	// This will display a bigger hud and readable fonts at high
	// resolutions. The fonts will be somewhat distorted, though
	i = COM_CheckParm("-conwidth");
	if (i != 0 && i < com_argc-1)
		i = atoi(com_argv[i + 1]);
	else	i = vid_config_consize.integer;
	if (i < MIN_WIDTH)	i = MIN_WIDTH;
	else if (i > width)	i = width;
	Cvar_SetValueQuick(&vid_config_consize, i);
	if (vid_config_consize.integer != width)
		vid_conscale = true;

	multisample = vid_config_fsaa.integer;
	i = COM_CheckParm ("-fsaa");
	if (i && i < com_argc-1)
		multisample = atoi(com_argv[i+1]);

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)(vid.colormap + 256 * 64)));
	if (vid.fullbright < 1 || vid.fullbright > 256)
		vid.fullbright = 224;	/* fallback: standard Quake/Hexen II value */

	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;
	//set the mode
	VID_SetMode (vid_mode.integer);
	ClearAllStates ();

	GL_SetupLightmapFmt();
	GL_Init ();
	VID_InitGamma();

	// lock the early-read cvars until Host_Init is finished
	for (i = 0; i < (int)num_readvars; i++)
		Cvar_LockVar (read_vars[i]);

	vid_initialized = true;
	scr_disabled_for_loading = temp;
	vid.recalc_refdef = 1;

	Con_SafePrintf ("Video initialized.\n");

	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;

//	if (COM_CheckParm("-fullsbar"))
//		fullsbardraw = true;
}


void	VID_Shutdown (void)
{
	GL_PostProcess_Shutdown();
	VID_ShutdownGamma();

	/* Save window position for next session */
	if (window && modestate == MS_WINDOWED)
	{
		int wx, wy;
		SDL_GetWindowPosition(window, &wx, &wy);
		Cvar_SetValueQuick(&vid_window_x, wx);
		Cvar_SetValueQuick(&vid_window_y, wy);
	}

	if (glcontext)
		SDL_GL_DestroyContext(glcontext);
	if (window)
		SDL_DestroyWindow(window);

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}


/*
================
VID_ToggleFullscreen
Handles switching between fullscreen/windowed modes
and brings the mouse to a proper state afterwards
================
*/
extern qboolean menu_disabled_mouse;
void VID_ToggleFullscreen (void)
{
	int	is_fullscreen;

	if (!window) return;
	if (!fs_toggle_works)
		return;
	if (!num_fmodes)
		return;

	S_ClearBuffer ();

	// SDL3: only SDL_WINDOW_FULLSCREEN, no FULLSCREEN_DESKTOP
	is_fullscreen = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;

	// SDL3: SDL_SetWindowFullscreen takes bool: true for fullscreen, false for windowed
	fs_toggle_works = SDL_SetWindowFullscreen(window, !is_fullscreen);
	if (fs_toggle_works)
	{
		int dw, dh;

		is_fullscreen = !is_fullscreen;
		SDL_SyncWindow(window);

		if (!is_fullscreen)
		{
			// Restore window to the mode's logical size
			SDL_SetWindowSize(window, modelist[vid_modenum].width,
						 modelist[vid_modenum].height);
			SDL_SyncWindow(window);
		}

		SDL_GetWindowSizeInPixels(window, &dw, &dh);
		WRWidth = dw;
		WRHeight = dh;
		vid.width = vid.conwidth = WRWidth;
		vid.height = vid.conheight = WRHeight;
		Cvar_SetValueQuick (&vid_config_glx, WRWidth);
		Cvar_SetValueQuick (&vid_config_gly, WRHeight);
		Cvar_SetValueQuick(&vid_config_fscr, is_fullscreen);
		modestate = (is_fullscreen) ? MS_FULLDIB : MS_WINDOWED;
		vid.recalc_refdef = 1;
		glViewport_fp(0, 0, WRWidth, WRHeight);
		if (is_fullscreen)
		{
		//	if (!_enable_mouse.integer)
		//		Cvar_SetQuick (&_enable_mouse, "1");
			// activate mouse in fullscreen mode
			// in_sdl.c handles other non-moused cases
			if (menu_disabled_mouse)
				IN_ActivateMouse();
		}
		else
		{	// windowed mode:
			// deactivate mouse if we are in menus
			if (menu_disabled_mouse)
				IN_DeactivateMouse();
		}
		// update the video menu option
		vid_menu_fs = (modestate != MS_WINDOWED);
	}
	else
	{
		Con_Printf ("SDL_SetWindowFullscreen failed\n");
	}
}


#ifndef H2W /* unused in hexenworld */
void D_ShowLoadingSize (void)
{
#if defined(DRAW_PROGRESSBARS)
	int cur_perc;
	static int prev_perc;

	if (!vid_initialized)
		return;

	cur_perc = loading_stage * 100;
	if (total_loading_size)
		cur_perc += current_loading_size * 100 / total_loading_size;
	if (cur_perc == prev_perc)
		return;
	prev_perc = cur_perc;

	glDrawBuffer_fp (GL_FRONT);

	SCR_DrawLoading();

	glFlush_fp();

	glDrawBuffer_fp (GL_BACK);
#endif	/* DRAW_PROGRESSBARS */
}
#endif


//========================================================
// Video menu stuff
//========================================================

static int	vid_menunum;
static int	vid_cursor;
static qboolean	want_fstoggle, need_apply;
static qboolean	vid_menu_firsttime = true;

// Aspect ratio filter for the video menu
typedef struct {
	const char	*name;
	int		num;	// numerator (0 = "All", no filter)
	int		den;	// denominator
} aspect_t;

static const aspect_t vid_aspects[] = {
	{ "All",	0,	0  },
	{ "4:3",	4,	3  },
	{ "5:4",	5,	4  },
	{ "16:9",	16,	9  },
	{ "16:10",	16,	10 },
	{ "21:9",	21,	9  },
	{ "32:9",	32,	9  },
};
#define NUM_ASPECTS	(int)(sizeof(vid_aspects) / sizeof(vid_aspects[0]))

static int	vid_menu_aspect;	// index into vid_aspects[]

static qboolean VID_ModeMatchesAspect (int mode_idx)
{
	int w, h, aspect_num, aspect_den;

	if (vid_menu_aspect == 0)	// "All"
		return true;

	w = modelist[mode_idx].width;
	h = modelist[mode_idx].height;
	aspect_num = vid_aspects[vid_menu_aspect].num;
	aspect_den = vid_aspects[vid_menu_aspect].den;

	// Cross-multiply to avoid floating point: w/h == num/den
	// Use a small tolerance: abs(w*den - h*num) <= h
	// This allows ~1 pixel of rounding per scanline
	return (abs(w * aspect_den - h * aspect_num) <= h);
}

// Find the next mode matching the current aspect ratio in the given direction.
// Returns -1 if no matching mode exists.
static int VID_FindNextFilteredMode (int from, int dir)
{
	int i = from + dir;

	while (i >= 0 && i < *nummodes)
	{
		if (VID_ModeMatchesAspect(i))
			return i;
		i += dir;
	}
	return -1;
}

// Snap vid_menunum to nearest matching mode when aspect filter changes
static void VID_SnapToFilteredMode (void)
{
	int lo, hi;

	if (vid_menu_aspect == 0)
		return;	// "All" — keep current selection

	if (VID_ModeMatchesAspect(vid_menunum))
		return;	// already matches

	// Search outward from current position
	lo = VID_FindNextFilteredMode(vid_menunum, -1);
	hi = VID_FindNextFilteredMode(vid_menunum, +1);

	if (hi >= 0)
		vid_menunum = hi;
	else if (lo >= 0)
		vid_menunum = lo;
	// else: no modes match this aspect, keep current
}

enum {
	VID_FULLSCREEN,	// make sure the fullscreen entry (0)
	VID_ASPECT,	// aspect ratio filter
	VID_RESOLUTION,	// is lower than resolution entry (1)
	VID_MULTISAMPLE,
	VID_VSYNC,
	VID_TEXFILTER,
	VID_ANISOTROPY,
	VID_BLANKLINE,	// spacer line
	VID_RESET,
	VID_APPLY,
	VID_ITEMS
};

static void M_DrawYesNo (int x, int y, int on, int white)
{
	if (on)
	{
		if (white)
			M_PrintWhite (x, y, "yes");
		else
			M_Print (x, y, "yes");
	}
	else
	{
		if (white)
			M_PrintWhite (x, y, "no");
		else
			M_Print (x, y, "no");
	}
}

/*
================
VID_MenuDraw
================
*/
static void VID_MenuDraw (void)
{
	ScrollTitle("gfx/menu/title7.lmp");

	if (vid_menu_firsttime)
	{	// settings for entering the menu first time
		vid_menunum = vid_modenum;
		vid_menu_fs = (modestate != MS_WINDOWED);
		vid_menu_aspect = 0;	// "All"
		vid_cursor = (num_fmodes) ? 0 : VID_RESOLUTION;
		vid_menu_firsttime = false;
	}

	want_fstoggle = ( ((modestate == MS_WINDOWED) && vid_menu_fs) || ((modestate != MS_WINDOWED) && !vid_menu_fs) );

	need_apply = (vid_menunum != vid_modenum) || want_fstoggle ||
			(multisample != vid_config_fsaa.integer);

	M_Print (76, 92 + 8*VID_FULLSCREEN, "Fullscreen    :");
	M_DrawYesNo (76+16*8, 92 + 8*VID_FULLSCREEN, vid_menu_fs, !want_fstoggle);

	M_Print (76, 92 + 8*VID_ASPECT, "Aspect Ratio  :");
	M_PrintWhite (76+16*8, 92 + 8*VID_ASPECT, vid_aspects[vid_menu_aspect].name);

	M_Print (76, 92 + 8*VID_RESOLUTION, "Resolution    :");
	if (vid_menunum == vid_modenum)
		M_PrintWhite (76+16*8, 92 + 8*VID_RESOLUTION, modelist[vid_menunum].modedesc);
	else
		M_Print (76+16*8, 92 + 8*VID_RESOLUTION, modelist[vid_menunum].modedesc);

	M_Print (76, 92 + 8*VID_MULTISAMPLE, "Antialiasing  :");
	if (sdl_has_multisample)
	{
		if (multisample == vid_config_fsaa.integer)
			M_PrintWhite (76+16*8, 92 + 8*VID_MULTISAMPLE, va("%d",multisample));
		else
			M_Print (76+16*8, 92 + 8*VID_MULTISAMPLE, va("%d",multisample));
	}
	else
		M_PrintWhite (76+16*8, 92 + 8*VID_MULTISAMPLE, "N/A");

	M_Print (76, 92 + 8*VID_VSYNC, "VSync         :");
	if (vid_vsync.integer == -1)
		M_PrintWhite (76+16*8, 92 + 8*VID_VSYNC, "Adaptive");
	else if (vid_vsync.integer)
		M_PrintWhite (76+16*8, 92 + 8*VID_VSYNC, "On");
	else
		M_PrintWhite (76+16*8, 92 + 8*VID_VSYNC, "Off");

	M_Print (76, 92 + 8*VID_TEXFILTER, "Textures      :");
	M_PrintWhite (76+16*8, 92 + 8*VID_TEXFILTER,
		(gl_filter_idx <= 2) ? "Classic" : "Smooth");

	M_Print (76, 92 + 8*VID_ANISOTROPY, "Anisotropy    :");
	if (gl_max_anisotropy >= 2)
		M_PrintWhite (76+16*8, 92 + 8*VID_ANISOTROPY, va("%dx", (int)gl_texture_anisotropy.value));
	else
		M_PrintWhite (76+16*8, 92 + 8*VID_ANISOTROPY, "N/A");

	if (need_apply)
	{
		M_Print (76, 92 + 8*VID_RESET, "RESET CHANGES");
		M_Print (76, 92 + 8*VID_APPLY, "APPLY CHANGES");
	}

	M_DrawCharacter (64, 92 + vid_cursor*8, 12+((int)(realtime*4)&1));
}

/*
================
VID_MenuKey
================
*/
static void VID_MenuKey (int key)
{
	switch (key)
	{
	case K_ESCAPE:
		vid_cursor = (num_fmodes) ? 0 : VID_RESOLUTION;
		M_Menu_Options_f ();
		break;

	case K_UPARROW:
		S_LocalSound ("raven/menu1.wav");
		vid_cursor--;
		if (vid_cursor < 0)
		{
			vid_cursor = (need_apply) ? VID_ITEMS-1 : VID_BLANKLINE-1;
		}
		else if (vid_cursor == VID_BLANKLINE)
		{
			vid_cursor--;
		}
		break;

	case K_DOWNARROW:
		S_LocalSound ("raven/menu1.wav");
		vid_cursor++;
		if (vid_cursor >= VID_ITEMS)
		{
			vid_cursor = (num_fmodes) ? 0 : VID_RESOLUTION;
			break;
		}
		if (vid_cursor >= VID_BLANKLINE)
		{
			if (need_apply)
			{
				if (vid_cursor == VID_BLANKLINE)
					vid_cursor++;
			}
			else
			{
				vid_cursor = (num_fmodes) ? 0 : VID_RESOLUTION;
			}
		}
		break;

	case K_ENTER:
		switch (vid_cursor)
		{
		case VID_RESET:
			vid_menu_fs = (modestate != MS_WINDOWED);
			vid_menunum = vid_modenum;
			multisample = vid_config_fsaa.integer;
			vid_cursor = (num_fmodes) ? 0 : VID_RESOLUTION;
			break;
		case VID_APPLY:
			if (need_apply)
			{
				Cvar_SetValueQuick(&vid_mode, vid_menunum);
				Cvar_SetValueQuick(&vid_config_fscr, vid_menu_fs);
				VID_Restart_f();
			}
			vid_cursor = (num_fmodes) ? 0 : VID_RESOLUTION;
			break;
		}
		return;

	case K_LEFTARROW:
		switch (vid_cursor)
		{
		case VID_FULLSCREEN:
			vid_menu_fs = !vid_menu_fs;
			if (fs_toggle_works)
				VID_ToggleFullscreen();
			break;
		case VID_ASPECT:
			S_LocalSound ("raven/menu1.wav");
			vid_menu_aspect--;
			if (vid_menu_aspect < 0)
				vid_menu_aspect = NUM_ASPECTS - 1;
			VID_SnapToFilteredMode();
			break;
		case VID_RESOLUTION:
		{
			int next = VID_FindNextFilteredMode(vid_menunum, -1);
			if (next >= 0)
			{
				S_LocalSound ("raven/menu1.wav");
				vid_menunum = next;
			}
			break;
		}
		case VID_MULTISAMPLE:
			if (!sdl_has_multisample)
				break;
			if (multisample <= 2)
				multisample = 0;
			else if (multisample <= 4)
				multisample = 2;
			else
				multisample = 4;
			break;
		case VID_VSYNC:
			if (vid_vsync.integer == 1)
				Cvar_SetQuick (&vid_vsync, "0");
			else if (vid_vsync.integer == 0)
				Cvar_SetQuick (&vid_vsync, "-1");
			else
				Cvar_SetQuick (&vid_vsync, "1");
			SDL_GL_SetSwapInterval(vid_vsync.integer);
			break;
		case VID_TEXFILTER:
			/* toggle between Classic (nearest+mipmap) and Smooth (trilinear) */
			Cvar_Set ("gl_texturemode", (gl_filter_idx <= 2) ?
				"GL_LINEAR_MIPMAP_LINEAR" : "GL_NEAREST_MIPMAP_LINEAR");
			break;
		case VID_ANISOTROPY:
			if (gl_max_anisotropy >= 2)
			{
				int av = (int)gl_texture_anisotropy.value;
				av = (av <= 1) ? (int)gl_max_anisotropy : av / 2;
				Cvar_SetValueQuick(&gl_texture_anisotropy, av);
			}
			break;
		}
		return;

	case K_RIGHTARROW:
		switch (vid_cursor)
		{
		case VID_FULLSCREEN:
			vid_menu_fs = !vid_menu_fs;
			if (fs_toggle_works)
				VID_ToggleFullscreen();
			break;
		case VID_ASPECT:
			S_LocalSound ("raven/menu1.wav");
			vid_menu_aspect++;
			if (vid_menu_aspect >= NUM_ASPECTS)
				vid_menu_aspect = 0;
			VID_SnapToFilteredMode();
			break;
		case VID_RESOLUTION:
		{
			int next = VID_FindNextFilteredMode(vid_menunum, +1);
			if (next >= 0)
			{
				S_LocalSound ("raven/menu1.wav");
				vid_menunum = next;
			}
			break;
		}
		case VID_MULTISAMPLE:
			if (!sdl_has_multisample)
				break;
			if (multisample < 2)
				multisample = 2;
			else if (multisample < 4)
				multisample = 4;
			else if (multisample < 8)
				multisample = 8;
			break;
		case VID_VSYNC:
			if (vid_vsync.integer == 0)
				Cvar_SetQuick (&vid_vsync, "1");
			else if (vid_vsync.integer == 1)
				Cvar_SetQuick (&vid_vsync, "-1");
			else
				Cvar_SetQuick (&vid_vsync, "0");
			SDL_GL_SetSwapInterval(vid_vsync.integer);
			break;
		case VID_TEXFILTER:
			Cvar_Set ("gl_texturemode", (gl_filter_idx <= 2) ?
				"GL_LINEAR_MIPMAP_LINEAR" : "GL_NEAREST_MIPMAP_LINEAR");
			break;
		case VID_ANISOTROPY:
			if (gl_max_anisotropy >= 2)
			{
				int av = (int)gl_texture_anisotropy.value * 2;
				if (av > (int)gl_max_anisotropy) av = 1;
				if (av < 1) av = 2;
				Cvar_SetValueQuick(&gl_texture_anisotropy, av);
			}
			break;
		}
		return;

	default:
		break;
	}
}


/*
================
Video menu helper functions for combined Display menu
================
*/
void VID_MenuInit (void)
{
	vid_menunum = vid_modenum;
	vid_menu_fs = (modestate != MS_WINDOWED);
	vid_menu_aspect = 0;
	multisample = vid_config_fsaa.integer;
}

qboolean VID_MenuNeedApply (void)
{
	qboolean want_fs = ( ((modestate == MS_WINDOWED) && vid_menu_fs) || ((modestate != MS_WINDOWED) && !vid_menu_fs) );
	return (vid_menunum != vid_modenum) || want_fs || (multisample != vid_config_fsaa.integer);
}

void VID_MenuApply (void)
{
	Cvar_SetValueQuick(&vid_mode, vid_menunum);
	Cvar_SetValueQuick(&vid_config_fscr, vid_menu_fs);
	VID_Restart_f();
}

void VID_MenuReset (void)
{
	vid_menu_fs = (modestate != MS_WINDOWED);
	vid_menunum = vid_modenum;
	multisample = vid_config_fsaa.integer;
}

const char *VID_MenuGetResolution (qboolean *is_current)
{
	*is_current = (vid_menunum == vid_modenum);
	return modelist[vid_menunum].modedesc;
}

const char *VID_MenuGetAspect (void)
{
	return vid_aspects[vid_menu_aspect].name;
}

qboolean VID_MenuGetFullscreen (qboolean *want_toggle)
{
	*want_toggle = ( ((modestate == MS_WINDOWED) && vid_menu_fs) || ((modestate != MS_WINDOWED) && !vid_menu_fs) );
	return vid_menu_fs;
}

int VID_MenuGetMultisample (qboolean *is_current, qboolean *available)
{
	*available = sdl_has_multisample;
	*is_current = (multisample == vid_config_fsaa.integer);
	return multisample;
}

int VID_MenuGetVSync (void)
{
	return vid_vsync.integer;
}

qboolean VID_MenuGetTexFilter (void)
{
	return (gl_filter_idx > 2);
}

int VID_MenuGetAnisotropy (qboolean *available)
{
	*available = (gl_max_anisotropy >= 2);
	return (int)gl_texture_anisotropy.value;
}

void VID_MenuAdjustFullscreen (void)
{
	vid_menu_fs = !vid_menu_fs;
	if (fs_toggle_works)
		VID_ToggleFullscreen();
}

void VID_MenuAdjustAspect (int dir)
{
	vid_menu_aspect += dir;
	if (vid_menu_aspect < 0)
		vid_menu_aspect = NUM_ASPECTS - 1;
	if (vid_menu_aspect >= NUM_ASPECTS)
		vid_menu_aspect = 0;
	VID_SnapToFilteredMode();
}

void VID_MenuAdjustResolution (int dir)
{
	int next = VID_FindNextFilteredMode(vid_menunum, dir);
	if (next >= 0)
		vid_menunum = next;
}

void VID_MenuAdjustMultisample (int dir)
{
	if (!sdl_has_multisample)
		return;
	if (dir < 0)
	{
		if (multisample <= 2) multisample = 0;
		else if (multisample <= 4) multisample = 2;
		else multisample = 4;
	}
	else
	{
		if (multisample < 2) multisample = 2;
		else if (multisample < 4) multisample = 4;
		else if (multisample < 8) multisample = 8;
	}
}

void VID_MenuAdjustVSync (int dir)
{
	if (dir < 0)
	{
		if (vid_vsync.integer == 1) Cvar_SetQuick(&vid_vsync, "0");
		else if (vid_vsync.integer == 0) Cvar_SetQuick(&vid_vsync, "-1");
		else Cvar_SetQuick(&vid_vsync, "1");
	}
	else
	{
		if (vid_vsync.integer == 0) Cvar_SetQuick(&vid_vsync, "1");
		else if (vid_vsync.integer == 1) Cvar_SetQuick(&vid_vsync, "-1");
		else Cvar_SetQuick(&vid_vsync, "0");
	}
	SDL_GL_SetSwapInterval(vid_vsync.integer);
}

void VID_MenuAdjustTexFilter (void)
{
	Cvar_Set ("gl_texturemode", (gl_filter_idx <= 2) ?
		"GL_LINEAR_MIPMAP_LINEAR" : "GL_NEAREST_MIPMAP_LINEAR");
}

void VID_MenuAdjustAnisotropy (int dir)
{
	int av;
	if (gl_max_anisotropy < 2) return;
	av = (int)gl_texture_anisotropy.value;
	if (dir < 0)
		av = (av <= 1) ? (int)gl_max_anisotropy : av / 2;
	else
	{
		av = av * 2;
		if (av > (int)gl_max_anisotropy) av = 1;
		if (av < 1) av = 2;
	}
	Cvar_SetValueQuick(&gl_texture_anisotropy, av);
}

