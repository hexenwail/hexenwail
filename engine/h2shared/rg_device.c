/* rg_device.c -- SDL_GPU backend: device, swapchain and frame (#340 P0, P1c)
 *
 * Under -gpu the window is claimed by an SDL_GPU device instead of a GL
 * context.  The GL renderer still runs, but every GL entry point it calls is
 * a counting no-op (rg_null.c); the seams the renderer draws through
 * (gl_texcreate.c, gl_texbind.c, gl_pipeline.c, gl_uniforms.c, gl_vbo.c,
 * R_Program_Create) have SDL_GPU arms that reach the rest of the backend
 * instead (rg_internal.h has the map).  P1c draws the 2D: console, menus,
 * HUD, loading plaque.  `gpu_unrouted` reports what still reaches the null
 * table, and which routed requests the backend could not do yet.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef USE_SDL_GPU
#error rg_device.c is the SDL_GPU backend; CMake compiles it only with USE_SDL_GPU
#endif

#include "quakedef.h"
#include "cfgfile.h"
#include "rg_internal.h"

qboolean	rg_active = false;

SDL_GPUDevice		*rg_dev;
SDL_Window		*rg_win;
static SDL_GPUPresentMode	rg_present_mode = SDL_GPU_PRESENTMODE_VSYNC;
static float		rg_clear[3] = { 0.25f, 0.5f, 0.75f };

/* #77 requires a hardware device: a software Vulkan driver would "work" at a
 * frame rate nobody can play at, and a player whose GPU driver is broken is
 * better served by the GL path than by lavapipe.  CI has no GPU, so tests and
 * headless runs turn this on to reach lavapipe.  Test-only: not archived, not
 * in any menu. */
static cvar_t	gpu_allow_software = {"gpu_allow_software", "0", CVAR_NONE};
/* SDL_GPU debug mode (Vulkan validation layers when installed).  Also on
 * whenever `developer` is. */
static cvar_t	gpu_debug = {"gpu_debug", "0", CVAR_NONE};
/* The colour P0 clears the swapchain to: "r g b", 0..1.  Distinctive on
 * purpose, so a frame grab shows at once whether SDL_GPU presented it. */
static cvar_t	gpu_clearcolor = {"gpu_clearcolor", "0.25 0.5 0.75", CVAR_NONE};

static void RG_ClearColor_f (cvar_t *var)
{
	float	c[3];

	if (sscanf (var->string, "%f %f %f", &c[0], &c[1], &c[2]) != 3)
	{
		Con_Printf ("gpu_clearcolor: want \"r g b\" in 0..1, got \"%s\"\n", var->string);
		return;
	}
	rg_clear[0] = CLAMP (0.0f, c[0], 1.0f);
	rg_clear[1] = CLAMP (0.0f, c[1], 1.0f);
	rg_clear[2] = CLAMP (0.0f, c[2], 1.0f);
}

float *RG_ClearColor (void)
{
	return rg_clear;
}

/* ------------------------------------------------------------------ */
/* Requests a seam routed but the backend cannot do yet                */
/* ------------------------------------------------------------------ */

#define RG_MAX_UNHANDLED	64
static struct {
	const char	*what;		/* a string literal at the call site */
	unsigned long	calls;
} rg_unhandled[RG_MAX_UNHANDLED];
static int		rg_numunhandled;

void RG_NoteUnhandled (const char *what)
{
	int	i;

	for (i = 0; i < rg_numunhandled; i++)
	{
		if (rg_unhandled[i].what == what || !strcmp (rg_unhandled[i].what, what))
		{
			rg_unhandled[i].calls++;
			return;
		}
	}
	if (rg_numunhandled == RG_MAX_UNHANDLED)
		i = RG_MAX_UNHANDLED - 1;	/* the last row absorbs the overflow */
	else
	{
		i = rg_numunhandled++;
		rg_unhandled[i].what = what;
		if (developer.integer)
			Con_DPrintf ("SDL_GPU: not handled yet: %s\n", what);
	}
	rg_unhandled[i].calls++;
}

unsigned long RG_UnhandledTotal (int *kinds)
{
	unsigned long	n = 0;
	int		i, k = 0;

	for (i = 0; i < rg_numunhandled; i++)
	{
		n += rg_unhandled[i].calls;
		k += (rg_unhandled[i].calls != 0);
	}
	if (kinds)
		*kinds = k;
	return n;
}

void RG_UnhandledReport (void)
{
	int		i, k;
	unsigned long	n = RG_UnhandledTotal (&k);

	for (i = 0; i < rg_numunhandled; i++)
	{
		if (rg_unhandled[i].calls)
			Con_Printf ("%10lu  (routed, not handled) %s\n", rg_unhandled[i].calls,
				    rg_unhandled[i].what);
	}
	Con_Printf ("gpu_unrouted: %lu routed calls the SDL_GPU backend did not handle, %d kinds\n",
		    n, k);
}

void RG_UnhandledReset (void)
{
	int	i;

	for (i = 0; i < rg_numunhandled; i++)
		rg_unhandled[i].calls = 0;
}

/* gpu_unrouted [reset]: the null table's report, then the backend's. */
static void RG_Unrouted_f (void)
{
	RG_NullGL_Report_f ();
	if (!R_GPU ())
		return;
	if (Cmd_Argc () > 1 && !q_strcasecmp (Cmd_Argv (1), "reset"))
		RG_UnhandledReset ();
	else
		RG_UnhandledReport ();
}

qboolean RG_Requested (void)
{
	return COM_CheckParm ("-gpu") != 0;
}

void RG_Init (void)
{
	/* VID_Init runs before the command line's +commands are executed, and
	 * the device is created inside it, so +gpu_allow_software 1 has to be
	 * read now, the way snd_dma.c reads its early cvars. */
	const char	*early[] = { "gpu_allow_software", "gpu_debug", "gpu_clearcolor" };

	Cvar_RegisterVariable (&gpu_allow_software);
	Cvar_RegisterVariable (&gpu_debug);
	Cvar_RegisterVariable (&gpu_clearcolor);
	Cvar_SetCallback (&gpu_clearcolor, RG_ClearColor_f);
	CFG_ReadCvarOverrides (early, (int)(sizeof(early) / sizeof(early[0])));
	RG_ClearColor_f (&gpu_clearcolor);

	Cmd_AddCommand ("gpu_unrouted", RG_Unrouted_f);
}

static const char *RG_PresentModeName (SDL_GPUPresentMode mode)
{
	switch (mode)
	{
	case SDL_GPU_PRESENTMODE_VSYNC:		return "vsync";
	case SDL_GPU_PRESENTMODE_IMMEDIATE:	return "immediate";
	case SDL_GPU_PRESENTMODE_MAILBOX:	return "mailbox";
	}
	return "?";
}

/* vid_vsync: 0 off, 1 on, -1 adaptive.  SDL_GPU has no adaptive mode;
 * mailbox is the nearest (no tearing, no blocking), and VSYNC is the one
 * mode SDL guarantees, so every request falls back to it. */
static SDL_GPUPresentMode RG_PickPresentMode (int vsync)
{
	SDL_GPUPresentMode	want[2];
	int			i, n = 0;

	if (vsync == 0)
	{
		want[n++] = SDL_GPU_PRESENTMODE_IMMEDIATE;
		want[n++] = SDL_GPU_PRESENTMODE_MAILBOX;
	}
	else if (vsync < 0)
		want[n++] = SDL_GPU_PRESENTMODE_MAILBOX;
	for (i = 0; i < n; i++)
	{
		if (SDL_WindowSupportsGPUPresentMode (rg_dev, rg_win, want[i]))
			return want[i];
	}
	return SDL_GPU_PRESENTMODE_VSYNC;
}

qboolean RG_CreateDevice (SDL_Window *window, int vsync)
{
	SDL_PropertiesID	props;
	qboolean		debug, software;
	const char		*name;

	debug = gpu_debug.integer || developer.integer || COM_CheckParm ("-developer");
	software = gpu_allow_software.integer != 0;

	props = SDL_CreateProperties ();
	/* SPIR-V only for now: it is the one format the offline gate builds and
	 * checks for every stage.  DXIL / MSL blobs come with P7. */
	SDL_SetBooleanProperty (props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
	SDL_SetBooleanProperty (props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, debug);
	SDL_SetBooleanProperty (props, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, false);
	SDL_SetBooleanProperty (props,
		SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN, !software);
	rg_dev = SDL_CreateGPUDeviceWithProperties (props);
	SDL_DestroyProperties (props);
	if (!rg_dev)
	{
		Con_SafePrintf ("SDL_GPU: no device (%s)%s\n", SDL_GetError (),
				software ? "" : "; software drivers are refused unless gpu_allow_software is 1");
		return false;
	}
	if (!SDL_ClaimWindowForGPUDevice (rg_dev, window))
	{
		Con_SafePrintf ("SDL_GPU: cannot claim the window (%s)\n", SDL_GetError ());
		SDL_DestroyGPUDevice (rg_dev);
		rg_dev = NULL;
		return false;
	}
	rg_win = window;

	/* No program can be built without the blobs (rg_shader.c has where
	 * they are looked for), and a device that can only clear is no use to
	 * a player: refuse, and the caller falls back to GL. */
	RG_Shader_Init ();
	if (!RG_ShaderSource ())
	{
		Con_SafePrintf ("SDL_GPU: no SPIR-V shaders (the embedded table has no s2d.vert, and "
				"no -gpushaders <dir> holding s2d.vert.vk.spv was given)\n");
		SDL_ReleaseWindowFromGPUDevice (rg_dev, window);
		SDL_DestroyGPUDevice (rg_dev);
		rg_dev = NULL;
		rg_win = NULL;
		return false;
	}
	RG_SetVSync (vsync);

	name = SDL_GetStringProperty (SDL_GetGPUDeviceProperties (rg_dev),
				      SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown device");
	Con_SafePrintf ("SDL_GPU: %s on %s, present %s, debug %s%s, shaders from %s\n",
			RG_DriverName (), name, RG_PresentModeName (rg_present_mode),
			debug ? "on" : "off", software ? ", software drivers allowed" : "",
			RG_ShaderSource ());
	rg_active = true;
	RG_Res_Init ();
	RG_Pipeline_Init ();
	RG_Frame_Init ();
	return true;
}

/* VID_ApplyVSync's arm: the menu and vid_vsync change the present mode live,
 * as SDL_GL_SetSwapInterval does on the GL path. */
void RG_SetVSync (int vsync)
{
	if (!rg_dev || !rg_win)
		return;
	rg_present_mode = RG_PickPresentMode (vsync);
	if (!SDL_SetGPUSwapchainParameters (rg_dev, rg_win,
					    SDL_GPU_SWAPCHAINCOMPOSITION_SDR, rg_present_mode))
	{
		Con_SafePrintf ("SDL_GPU: present mode %s refused (%s), using vsync\n",
				RG_PresentModeName (rg_present_mode), SDL_GetError ());
		rg_present_mode = SDL_GPU_PRESENTMODE_VSYNC;
		SDL_SetGPUSwapchainParameters (rg_dev, rg_win,
					       SDL_GPU_SWAPCHAINCOMPOSITION_SDR, rg_present_mode);
	}
}

void RG_Shutdown (void)
{
	if (!rg_dev)
		return;
	SDL_WaitForGPUIdle (rg_dev);
	RG_Frame_Shutdown ();
	RG_Pipeline_Shutdown ();
	RG_Shader_Shutdown ();
	RG_Res_Shutdown ();
	if (rg_win)
		SDL_ReleaseWindowFromGPUDevice (rg_dev, rg_win);
	SDL_DestroyGPUDevice (rg_dev);
	rg_dev = NULL;
	rg_win = NULL;
	rg_active = false;
}

const char *RG_DriverName (void)
{
	const char	*d = rg_dev ? SDL_GetGPUDeviceDriver (rg_dev) : NULL;

	return d ? d : "none";
}

void RG_EndFrame (qboolean show)
{
	if (!rg_dev)
		return;
	RG_Frame_End (show);
}

void RG_Status (void)
{
	int		hit;
	unsigned long	total = RG_NullGL_Total (&hit);
	const char	*name = rg_dev ?
		SDL_GetStringProperty (SDL_GetGPUDeviceProperties (rg_dev),
				       SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown device") : "none";

	Con_Printf ("[RENDERER] SDL_GPU driver=%s device=%s present=%s swapchain=%s\n",
		    RG_DriverName (), name, RG_PresentModeName (rg_present_mode),
		    rg_dev ? "SDR" : "none");
	Con_Printf ("[RENDERER] SDL_GPU P2b: 2D, world, brush and alias models drawn; GL runs against "
		    "the null table (%lu calls into %d entry points, see gpu_unrouted)\n", total, hit);
	Con_Printf ("[RENDERER] SDL_GPU shaders from %s\n", RG_ShaderSource () ? RG_ShaderSource () : "nowhere");
	if (rg_dev)
	{
		RG_Frame_Status ();
		RG_Res_Status ();
	}
}
