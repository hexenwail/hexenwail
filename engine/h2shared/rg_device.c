/* rg_device.c -- SDL_GPU backend: device, swapchain and frame (#340 P0, P1c)
 *
 * Under -gpu the window is claimed by an SDL_GPU device instead of a GL
 * context.  The GL renderer still runs, but every GL entry point it calls is
 * a counting no-op (rg_null.c); the seams the renderer draws through
 * (gl_texcreate.c, gl_texbind.c, gl_pipeline.c, gl_uniforms.c, gl_vbo.c,
 * gl_buffer.c, R_Program_Create) have SDL_GPU arms that reach the rest of the backend
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
#include <SDL3/SDL_vulkan.h>	/* RG_VulkanMax2D's instance (#340 P5f) */
#include <stddef.h>		/* offsetof */

qboolean	rg_active = false;

SDL_GPUDevice		*rg_dev;
SDL_Window		*rg_win;
/* -gpuheadless (#340 P5f, test-only): the device renders without claiming
 * the window, and the present blits into a stand-in texture instead of a
 * swapchain.  For timing a GPU with no display to present to (the build
 * box's Tesla P4: SDL's offscreen video driver has no Vulkan surface there),
 * as GL's offscreen EGL path does.  Not in any menu, not a cvar. */
qboolean		rg_headless;
/* The Vulkan device's maxImageDimension2D, 0 until asked or when the asking
 * failed (RG_VulkanMax2D, #340 P5f). */
static int		rg_vk_max2d;
/* The depth formats it filters linearly, a bit per RGV_DEPTH_FORMATS entry,
 * asked the same way (#340 P6b). */
static Uint32		rg_vk_depth_linear;
static int		RG_VulkanMax2D (const char *name, Uint32 *depth_linear);
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
	Cmd_AddCommand ("gpu_frametimes", RG_FrameTimes_f);
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
	rg_headless = COM_CheckParm ("-gpuheadless") != 0;
	if (!rg_headless && !SDL_ClaimWindowForGPUDevice (rg_dev, window))
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
		if (!rg_headless)
			SDL_ReleaseWindowFromGPUDevice (rg_dev, window);
		SDL_DestroyGPUDevice (rg_dev);
		rg_dev = NULL;
		rg_win = NULL;
		rg_headless = false;
		return false;
	}
	RG_SetVSync (vsync);

	name = SDL_GetStringProperty (SDL_GetGPUDeviceProperties (rg_dev),
				      SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown device");
	rg_vk_depth_linear = 0;
	rg_vk_max2d = !strcmp (RG_DriverName (), "vulkan") ? RG_VulkanMax2D (name, &rg_vk_depth_linear) : 0;
	Con_SafePrintf ("SDL_GPU: %s on %s, present %s, debug %s%s, shaders from %s, textures to %d\n",
			RG_DriverName (), name, rg_headless ? "headless" : RG_PresentModeName (rg_present_mode),
			debug ? "on" : "off", software ? ", software drivers allowed" : "",
			RG_ShaderSource (), RG_MaxTextureSize ());
	rg_active = true;
	RG_Res_Init ();
	RG_Buf_Init ();
	RG_Pipeline_Init ();
	RG_Frame_Init ();
	RG_Target_Init ();
	return true;
}

/* VID_ApplyVSync's arm: the menu and vid_vsync change the present mode live,
 * as SDL_GL_SetSwapInterval does on the GL path. */
void RG_SetVSync (int vsync)
{
	if (!rg_dev || !rg_win || rg_headless)
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
	RG_Target_Shutdown ();
	RG_Pipeline_Shutdown ();
	RG_Shader_Shutdown ();
	RG_Res_Shutdown ();
	RG_Buf_Shutdown ();
	if (rg_win && !rg_headless)
		SDL_ReleaseWindowFromGPUDevice (rg_dev, rg_win);
	SDL_DestroyGPUDevice (rg_dev);
	rg_dev = NULL;
	rg_win = NULL;
	rg_headless = false;
	rg_vk_max2d = 0;
	rg_vk_depth_linear = 0;
	rg_active = false;
}

const char *RG_DriverName (void)
{
	const char	*d = rg_dev ? SDL_GetGPUDeviceDriver (rg_dev) : NULL;

	return d ? d : "none";
}

/* SDL 3.4 has no query for a device's texture limit (SDL_GetGPUDeviceProperties
 * carries names and versions only), so this is the floor each backend's
 * requirements guarantee, never more than the 16384 SDL_CreateGPUTexture's
 * debug-mode validation allows (SDL_gpu.c MAX_2D_DIMENSION):
 *   direct3d12  16384  D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; SDL requires
 *                      feature level 11_0 (SDL_gpu.h "System Requirements")
 *   metal       16384  Apple's feature set tables, for the Mac2 and A9
 *                      (Apple3) families SDL requires
 *   vulkan       4096  maxImageDimension2D's required minimum (Vulkan spec,
 *                      "Required Limits"); SDL checks no limit when it picks
 *                      a physical device, so a 4096 part is accepted
 * An unknown driver gets the smallest of them.  Vulkan, the only driver
 * RG_CreateDevice can get while it offers SPIR-V alone, asks the device
 * itself (rg_vk_max2d, #340 P5f): at the floor a map past 120 lightmap pages
 * (15 rows of 8 at 260 px, gl_rsurf.c) lost the atlas and with it GPU
 * lightstyles and the GPU world cull, where GL on the same box kept them --
 * Storm over Thyrion's courtyard has 189. */
int RG_MaxTextureSize (void)
{
	const char	*d = RG_DriverName ();

	if (!strcmp (d, "direct3d12") || !strcmp (d, "metal"))
		return 16384;
	if (!strcmp (d, "vulkan") && rg_vk_max2d >= 4096)
		return rg_vk_max2d < 16384 ? rg_vk_max2d : 16384;
	return 4096;
}

/* The head of VkPhysicalDeviceProperties, through limits.maxImageDimension2D
 * and on to the first VkDeviceSize, which gives the limits their 8-byte
 * alignment.  Vulkan's ABI has been fixed since 1.0 (vulkan_core.h); spelled
 * here because the build has SDL_vulkan.h's handles and no Vulkan headers. */
typedef struct {
	Uint32	api_version, driver_version, vendor_id, device_id, device_type;
	char	device_name[256];	/* VK_MAX_PHYSICAL_DEVICE_NAME_SIZE */
	Uint8	cache_uuid[16];		/* VK_UUID_SIZE */
	struct {
		Uint32	max1d, max2d, max3d, maxcube, maxlayers, maxtexelbuf, maxubo, maxssbo,
			maxpush, maxalloc, maxsamplers;
		Uint64	buffer_image_granularity;
	} limits;
} rgvkprops_t;
COMPILE_TIME_ASSERT (rgvkprops_name, offsetof (rgvkprops_t, device_name) == 20);
COMPILE_TIME_ASSERT (rgvkprops_max2d, offsetof (rgvkprops_t, limits.max2d) == 300);

typedef struct {
	int		stype;		/* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 1 */
	const void	*next;
	Uint32		flags;
	const void	*app;
	Uint32		nlayers;
	const char	*const *layers;
	Uint32		nexts;
	const char	*const *exts;
} rgvkinstinfo_t;

#if defined(_WIN32) && !defined(_WIN64)
#define RG_VKAPI	__stdcall
#else
#define RG_VKAPI
#endif
typedef void *(RG_VKAPI *rgvk_gipa_t) (void *instance, const char *name);
typedef int (RG_VKAPI *rgvk_create_t) (const rgvkinstinfo_t *info, const void *alloc, void **instance);
typedef void (RG_VKAPI *rgvk_destroy_t) (void *instance, const void *alloc);
typedef int (RG_VKAPI *rgvk_enum_t) (void *instance, Uint32 *count, void **devices);
typedef void (RG_VKAPI *rgvk_props_t) (void *device, void *props);
/* VkFormatProperties: linear-tiling, optimal-tiling and buffer features. */
typedef struct {
	Uint32	linear, optimal, buffer;
} rgvkfmtprops_t;
typedef void (RG_VKAPI *rgvk_fmtprops_t) (void *device, int format, rgvkfmtprops_t *props);
#define RGV_FILTER_LINEAR	0x00001000u	/* VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT */
/* rg_vk_depth_linear: the device was asked.  Unasked (a driver other than
 * Vulkan, or the instance failed) is not a "no": the sampler keeps what GL
 * asked for, as before P6b, and renderer_status says it is unknown. */
#define RGV_ASKED		0x80000000u

/* The depth formats SDL_GPU has, and the VkFormat SDL 3.4.12 makes each of
 * (SDL_gpu_vulkan.c SDLToVK_TextureFormat). */
static const struct {
	SDL_GPUTextureFormat	sdl;
	int			vk;
} RGV_DEPTH_FORMATS[] = {
	{ SDL_GPU_TEXTUREFORMAT_D16_UNORM,		124 },	/* VK_FORMAT_D16_UNORM */
	{ SDL_GPU_TEXTUREFORMAT_D24_UNORM,		125 },	/* VK_FORMAT_X8_D24_UNORM_PACK32 */
	{ SDL_GPU_TEXTUREFORMAT_D32_FLOAT,		126 },	/* VK_FORMAT_D32_SFLOAT */
	{ SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,	129 },	/* VK_FORMAT_D24_UNORM_S8_UINT */
	{ SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,	130 },	/* VK_FORMAT_D32_SFLOAT_S8_UINT */
};

qboolean RG_DepthFiltersAsked (void)
{
	return (rg_vk_depth_linear & RGV_ASKED) != 0;
}

qboolean RG_DepthFiltersLinear (SDL_GPUTextureFormat fmt)
{
	size_t	i;

	if (!(rg_vk_depth_linear & RGV_ASKED))
		return true;
	for (i = 0; i < sizeof(RGV_DEPTH_FORMATS) / sizeof(RGV_DEPTH_FORMATS[0]); i++)
	{
		if (RGV_DEPTH_FORMATS[i].sdl == fmt)
			return (rg_vk_depth_linear >> i) & 1;
	}
	return false;
}

/* maxImageDimension2D of the physical device SDL_GPU named `name`, from an
 * instance of our own (no layers, no extensions, no surface); 0 when any
 * step fails or no device has that name.  SDL's Vulkan backend names the
 * device with its deviceName, so the two match.  *depth_linear gets the
 * RGV_DEPTH_FORMATS it filters linearly in optimal tiling, the tiling SDL
 * makes every texture in (#340 P6b); 0 when that cannot be asked. */
static int RG_VulkanMax2D (const char *name, Uint32 *depth_linear)
{
	rgvk_gipa_t	gipa;
	rgvk_create_t	create;
	rgvk_destroy_t	destroy;
	rgvk_enum_t	enumerate;
	rgvk_props_t	props;
	rgvk_fmtprops_t	fmtprops;
	rgvkinstinfo_t	info;
	void		*inst = NULL, *devs[16];
	Uint32		i, n = 16;
	int		found = 0;
	union { rgvkprops_t p; Uint8 bytes[2048]; } pr;	/* the whole struct is 824 bytes */

	if (!name || !SDL_Vulkan_LoadLibrary (NULL))
		return 0;
	gipa = (rgvk_gipa_t) SDL_Vulkan_GetVkGetInstanceProcAddr ();
	create = gipa ? (rgvk_create_t) gipa (NULL, "vkCreateInstance") : NULL;
	SDL_zero (info);
	info.stype = 1;
	if (create && create (&info, NULL, &inst) == 0 && inst)
	{
		destroy = (rgvk_destroy_t) gipa (inst, "vkDestroyInstance");
		enumerate = (rgvk_enum_t) gipa (inst, "vkEnumeratePhysicalDevices");
		props = (rgvk_props_t) gipa (inst, "vkGetPhysicalDeviceProperties");
		fmtprops = (rgvk_fmtprops_t) gipa (inst, "vkGetPhysicalDeviceFormatProperties");
		/* VK_INCOMPLETE (5) still fills the first 16 */
		if (enumerate && props && enumerate (inst, &n, devs) >= 0)
		{
			for (i = 0; i < n && i < 16 && !found; i++)
			{
				memset (&pr, 0, sizeof(pr));
				props (devs[i], &pr);
				pr.p.device_name[sizeof(pr.p.device_name) - 1] = 0;
				if (!strcmp (pr.p.device_name, name))
				{
					size_t	k;

					found = (int) pr.p.limits.max2d;
					for (k = 0; fmtprops && k < sizeof(RGV_DEPTH_FORMATS) / sizeof(RGV_DEPTH_FORMATS[0]); k++)
					{
						rgvkfmtprops_t	fp;

						memset (&fp, 0, sizeof(fp));
						fmtprops (devs[i], RGV_DEPTH_FORMATS[k].vk, &fp);
						if (fp.optimal & RGV_FILTER_LINEAR)
							*depth_linear |= 1u << k;
					}
					if (fmtprops)
						*depth_linear |= RGV_ASKED;
				}
			}
		}
		if (destroy)
			destroy (inst, NULL);
	}
	SDL_Vulkan_UnloadLibrary ();
	return found;
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
		    RG_DriverName (), name, rg_headless ? "headless" : RG_PresentModeName (rg_present_mode),
		    rg_dev && !rg_headless ? "SDR" : "none");
	Con_Printf ("[RENDERER] SDL_GPU P6b: 2D, world, brush and alias models, the stencilled sky, liquids, "
		    "particles, sprites, glows, fog and mirrors drawn, post-process (gamma, contrast, r_scale, FXAA, "
		    "r_softemu on the GPU palette LUT, the underwater warp, motion blur, bloom, HDR) on render "
		    "targets, compute passes and readbacks, clustered dynamic lights and GPU lightstyles, "
		    "the GPU world cull and its indirect draws, the Hi-Z pyramid with no fence wait, SSAO, "
		    "soft particles, the dynamic lights' cube shadow maps, the world, "
		    "brush models, sky pre-pass and alias meshes from their vertex buffers, instanced and "
		    "skeletal alias models from their storage buffers; GL runs against "
		    "the null table (%lu calls into %d entry points, see gpu_unrouted)\n", total, hit);
	Con_Printf ("[RENDERER] SDL_GPU shaders from %s\n", RG_ShaderSource () ? RG_ShaderSource () : "nowhere");
	if (rg_dev)
	{
		RG_Frame_Status ();
		RG_Target_Status ();
		RG_Res_Status ();
		RG_Buf_Status ();
	}
}
