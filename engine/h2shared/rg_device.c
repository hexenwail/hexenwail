/* rg_device.c -- SDL_GPU backend: device, swapchain and frame (#340 P0)
 *
 * P0 of the SDL_GPU backend: under -gpu the window is claimed by an SDL_GPU
 * device instead of a GL context, and each frame is one render pass that
 * clears the swapchain to gpu_clearcolor.  Nothing else is drawn yet; the GL
 * renderer still runs, against rg_null.c's counting no-ops, and
 * `gpu_unrouted` reports what it called.
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
#include "rg_device.h"

qboolean	rg_active = false;

static SDL_GPUDevice	*rg_device;
static SDL_Window	*rg_window;
static SDL_GPUPresentMode	rg_present_mode = SDL_GPU_PRESENTMODE_VSYNC;
static float		rg_clear[3] = { 0.25f, 0.5f, 0.75f };
/* Per kind, so a burst of one cannot silence another for the whole run. */
enum { RG_FAIL_CMDBUF, RG_FAIL_SWAPCHAIN, RG_FAIL_SUBMIT, RG_FAIL_KINDS };
static unsigned int	rg_failures[RG_FAIL_KINDS];
#define RG_REPORT(kind)	(rg_failures[kind]++ < 4)

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

	Cmd_AddCommand ("gpu_unrouted", RG_NullGL_Report_f);
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
		if (SDL_WindowSupportsGPUPresentMode (rg_device, rg_window, want[i]))
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
	rg_device = SDL_CreateGPUDeviceWithProperties (props);
	SDL_DestroyProperties (props);
	if (!rg_device)
	{
		Con_SafePrintf ("SDL_GPU: no device (%s)%s\n", SDL_GetError (),
				software ? "" : "; software drivers are refused unless gpu_allow_software is 1");
		return false;
	}
	if (!SDL_ClaimWindowForGPUDevice (rg_device, window))
	{
		Con_SafePrintf ("SDL_GPU: cannot claim the window (%s)\n", SDL_GetError ());
		SDL_DestroyGPUDevice (rg_device);
		rg_device = NULL;
		return false;
	}
	rg_window = window;
	RG_SetVSync (vsync);

	name = SDL_GetStringProperty (SDL_GetGPUDeviceProperties (rg_device),
				      SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown device");
	Con_SafePrintf ("SDL_GPU: %s on %s, present %s, debug %s%s\n",
			RG_DriverName (), name, RG_PresentModeName (rg_present_mode),
			debug ? "on" : "off", software ? ", software drivers allowed" : "");
	rg_active = true;
	memset (rg_failures, 0, sizeof(rg_failures));
	return true;
}

/* VID_ApplyVSync's arm: the menu and vid_vsync change the present mode live,
 * as SDL_GL_SetSwapInterval does on the GL path. */
void RG_SetVSync (int vsync)
{
	if (!rg_device || !rg_window)
		return;
	rg_present_mode = RG_PickPresentMode (vsync);
	if (!SDL_SetGPUSwapchainParameters (rg_device, rg_window,
					    SDL_GPU_SWAPCHAINCOMPOSITION_SDR, rg_present_mode))
	{
		Con_SafePrintf ("SDL_GPU: present mode %s refused (%s), using vsync\n",
				RG_PresentModeName (rg_present_mode), SDL_GetError ());
		rg_present_mode = SDL_GPU_PRESENTMODE_VSYNC;
		SDL_SetGPUSwapchainParameters (rg_device, rg_window,
					       SDL_GPU_SWAPCHAINCOMPOSITION_SDR, rg_present_mode);
	}
}

void RG_Shutdown (void)
{
	if (!rg_device)
		return;
	SDL_WaitForGPUIdle (rg_device);
	if (rg_window)
		SDL_ReleaseWindowFromGPUDevice (rg_device, rg_window);
	SDL_DestroyGPUDevice (rg_device);
	rg_device = NULL;
	rg_window = NULL;
	rg_active = false;
}

const char *RG_DriverName (void)
{
	const char	*d = rg_device ? SDL_GetGPUDeviceDriver (rg_device) : NULL;

	return d ? d : "none";
}

void RG_Present (void)
{
	SDL_GPUCommandBuffer	*cmd;
	SDL_GPUTexture		*swap = NULL;
	Uint32			w, h;

	if (!rg_device)
		return;
	cmd = SDL_AcquireGPUCommandBuffer (rg_device);
	if (!cmd)
	{
		if (RG_REPORT (RG_FAIL_CMDBUF))
			Con_Printf ("SDL_GPU: no command buffer (%s)\n", SDL_GetError ());
		return;
	}
	if (!SDL_WaitAndAcquireGPUSwapchainTexture (cmd, rg_window, &swap, &w, &h))
	{
		if (RG_REPORT (RG_FAIL_SWAPCHAIN))
			Con_Printf ("SDL_GPU: no swapchain texture (%s)\n", SDL_GetError ());
		SDL_CancelGPUCommandBuffer (cmd);
		return;
	}
	/* NULL with success is a minimised or occluded window: nothing to draw
	 * into, but the command buffer still has to be submitted. */
	if (swap)
	{
		SDL_GPUColorTargetInfo	target;
		SDL_GPURenderPass	*pass;

		SDL_zero (target);
		target.texture = swap;
		target.clear_color.r = rg_clear[0];
		target.clear_color.g = rg_clear[1];
		target.clear_color.b = rg_clear[2];
		target.clear_color.a = 1.0f;
		target.load_op = SDL_GPU_LOADOP_CLEAR;
		target.store_op = SDL_GPU_STOREOP_STORE;
		pass = SDL_BeginGPURenderPass (cmd, &target, 1, NULL);
		if (pass)
			SDL_EndGPURenderPass (pass);
	}
	if (!SDL_SubmitGPUCommandBuffer (cmd) && RG_REPORT (RG_FAIL_SUBMIT))
		Con_Printf ("SDL_GPU: submit failed (%s)\n", SDL_GetError ());
}

void RG_Status (void)
{
	int		hit;
	unsigned long	total = RG_NullGL_Total (&hit);
	const char	*name = rg_device ?
		SDL_GetStringProperty (SDL_GetGPUDeviceProperties (rg_device),
				       SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown device") : "none";

	Con_Printf ("[RENDERER] SDL_GPU driver=%s device=%s present=%s swapchain=%s\n",
		    RG_DriverName (), name, RG_PresentModeName (rg_present_mode),
		    rg_device ? "SDR" : "none");
	Con_Printf ("[RENDERER] SDL_GPU P0: clear colour only; GL runs against the null table "
		    "(%lu calls into %d entry points, see gpu_unrouted)\n", total, hit);
}
