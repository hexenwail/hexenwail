/* rg_internal.h -- SDL_GPU backend: what the rg_*.c files share (#340 P1c)
 *
 * Private to the backend: only engine/h2shared/rg_*.c include this
 * (tools/test_rg_sources.py).  The seams see rg_device.h alone.
 *
 *   rg_device.c    device, swapchain, cvars, the unhandled-request counters
 *   rg_resource.c  textures and samplers, keyed by the GL texture names
 *   rg_shader.c    SPIR-V loading and reflection, programs by GL-shaped name
 *   rg_pipeline.c  graphics pipelines, cached by draw state + program + layout
 *   rg_frame.c     the frame recorder: draws per render-target segment, one
 *                  copy pass per segment, the scene target and the present
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#ifndef RG_INTERNAL_H
#define RG_INTERNAL_H

#ifndef USE_SDL_GPU
#error rg_internal.h belongs to the SDL_GPU backend; include it only from rg_*.c
#endif

#include "rg_device.h"
#include "gl_pipeline.h"

/* GL enums the backend maps, spelled here because the mingw-w64 gl.h is GL
 * 1.1 and glheader.h only carries what the GL renderer itself needs. */
#ifndef GL_BGRA
#define GL_BGRA				0x80E1
#endif
#ifndef GL_BGR
#define GL_BGR				0x80E0
#endif
#ifndef GL_RG
#define GL_RG				0x8227
#endif
#ifndef GL_RG8
#define GL_RG8				0x822B
#endif
#ifndef GL_R16F
#define GL_R16F				0x822D
#endif
#ifndef GL_R32F
#define GL_R32F				0x822E
#endif
#ifndef GL_RG16F
#define GL_RG16F			0x822F
#endif
#ifndef GL_R16UI
#define GL_R16UI			0x8234
#endif
#ifndef GL_R32UI
#define GL_R32UI			0x8236
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F			0x8814
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F			0x881A
#endif
#ifndef GL_RGBA32UI
#define GL_RGBA32UI			0x8D70
#endif
#ifndef GL_RGB10_A2
#define GL_RGB10_A2			0x8059
#endif
#ifndef GL_SRGB8
#define GL_SRGB8			0x8C41
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8			0x8C43
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16		0x81A5
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24		0x81A6
#endif
#ifndef GL_DEPTH_COMPONENT32F
#define GL_DEPTH_COMPONENT32F		0x8CAC
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8		0x88F0
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL		0x84F9
#endif
#ifndef GL_RED_INTEGER
#define GL_RED_INTEGER			0x8D94
#endif
#ifndef GL_RGBA_INTEGER
#define GL_RGBA_INTEGER			0x8D99
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT			0x140B
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV	0x8367
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8		0x84FA
#endif
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY		0x8C1A
#endif
#ifndef GL_TEXTURE_CUBE_MAP_ARRAY
#define GL_TEXTURE_CUBE_MAP_ARRAY	0x9009
#endif
#ifndef GL_TEXTURE_2D_MULTISAMPLE
#define GL_TEXTURE_2D_MULTISAMPLE	0x9100
#endif
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL		0x813C
#endif
#ifndef GL_TEXTURE_MIN_LOD
#define GL_TEXTURE_MIN_LOD		0x813A
#endif
#ifndef GL_TEXTURE_MAX_LOD
#define GL_TEXTURE_MAX_LOD		0x813B
#endif
#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS		0x8501
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE		0x884C
#endif
#ifndef GL_TEXTURE_COMPARE_FUNC
#define GL_TEXTURE_COMPARE_FUNC		0x884D
#endif
#ifndef GL_COMPARE_REF_TO_TEXTURE
#define GL_COMPARE_REF_TO_TEXTURE	0x884E
#endif
#ifndef GL_TEXTURE_SWIZZLE_A
#define GL_TEXTURE_SWIZZLE_A		0x8E45
#endif
#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT		0x8370
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER		0x812D
#endif
#ifndef GL_CONSTANT_COLOR
#define GL_CONSTANT_COLOR		0x8001
#endif
#ifndef GL_ONE_MINUS_CONSTANT_COLOR
#define GL_ONE_MINUS_CONSTANT_COLOR	0x8002
#endif
#ifndef GL_INCR_WRAP
#define GL_INCR_WRAP			0x8507
#endif
#ifndef GL_DECR_WRAP
#define GL_DECR_WRAP			0x8508
#endif

/* rg_device.c */
extern SDL_GPUDevice	*rg_dev;
extern SDL_Window	*rg_win;
float	*RG_ClearColor (void);		/* gpu_clearcolor, rgb */

/* ------------------------------------------------------------------ */
/* rg_resource.c                                                       */
/* ------------------------------------------------------------------ */

/* What a shader samples: the image dimensionality its SPIR-V declares.  A
 * texture unit holds one texture per GL target, so this picks which. */
typedef enum {
	RG_TT_2D,
	RG_TT_3D,
	RG_TT_CUBE,
	RG_TT_2D_ARRAY,
	RG_TT_CUBE_ARRAY,
	RG_TT_COUNT
} rg_textype_t;

/* What a sampler returns, from its SPIR-V sampled type: a float sampler
 * reads a float, unorm or depth format, an int / uint sampler only an
 * integer format of its own signedness.  Vulkan makes any other pairing an
 * error at the draw. */
typedef enum {
	RG_SC_FLOAT,
	RG_SC_SINT,
	RG_SC_UINT,
	RG_SC_COUNT
} rg_sampclass_t;

/* A sampler's GL texture unit before anything names one: a draw binds it a
 * placeholder (R_ProgramSamplerUnit, gl_shader.c). */
#define RG_UNIT_NONE	0xFF

/* The zeroed buffer every read-only storage-buffer slot of a graphics stage
 * is bound to until P5 routes the real ones.  Big enough for the fixed part
 * of every such block in engine/shaders -- the largest is salias_inst_vert's
 * ShadeDots, float[4096]; the others end in runtime arrays, whose minimum is
 * empty.  tools/test_sdlgpu_bindings.py holds every blob to it. */
#define RG_SSBO_STANDIN_BYTES	16384

void	RG_Res_Init (void);
void	RG_Res_Shutdown (void);
/* The texture and sampler a draw binds for a sampler reading GL unit `unit`
 * (RG_UNIT_NONE: none) as dimensionality `type` and class `sclass` -- a depth
 * texture with a compare sampler when `shadow` -- created now if it has not
 * been.  A placeholder of exactly that kind when the unit holds nothing the
 * sampler can read: opaque white (float), zero (int, uint), depth 1.0
 * (shadow).  Stamps the texture as read by the current segment, so a later
 * upload into it flushes the segment first.  False, and counted, when there
 * is nothing to bind at all (a multisample sampler, a placeholder kind the
 * device lacks): the draw has to be dropped. */
qboolean RG_Res_ForDraw (GLuint unit, rg_textype_t type, rg_sampclass_t sclass, qboolean shadow,
			 SDL_GPUTextureSamplerBinding *out);
/* The depth placeholders are cleared rather than uploaded (a depth upload is
 * not portable): one depth-only render pass per layer, once, outside any
 * other pass.  rg_frame.c calls this after each copy pass. */
void	RG_Res_ClearPlaceholders (SDL_GPUCommandBuffer *cmd);
/* Bytes of queued texture uploads, and writing them: `RG_Res_StageUploads`
 * copies every queued upload into `dst` (mapped transfer memory at
 * `base`) and records where; `RG_Res_IssueUploads` issues them into the copy
 * pass and frees the queue. */
Uint32	RG_Res_PendingBytes (void);
void	RG_Res_StageUploads (Uint8 *dst, Uint32 base);
void	RG_Res_IssueUploads (SDL_GPUCopyPass *copy, SDL_GPUTransferBuffer *tb);
/* After the frame's command buffer is submitted: release what deletes and
 * re-creations retired during it. */
void	RG_Res_EndFrame (void);
void	RG_Res_Status (void);

/* ------------------------------------------------------------------ */
/* rg_shader.c                                                         */
/* ------------------------------------------------------------------ */

#define RG_MAX_STAGE_SAMPLERS	16
#define RG_MAX_STAGE_UBOS	4
#define RG_MAX_STAGE_SSBOS	8

typedef struct {
	char		name[32];
	Uint8		binding;
	Uint8		type;		/* rg_textype_t */
	Uint8		sclass;		/* rg_sampclass_t */
	Uint8		shadow;		/* depth-compare (a *Shadow sampler) */
	Uint8		ms;		/* multisample: nothing SDL_GPU can sample (P6) */
	Uint8		unit;		/* the GL texture unit it reads, RG_UNIT_NONE */
} rgsampler_t;

typedef struct {
	char		name[32];
	Uint8		binding;	/* the SDL_GPU push slot */
	int		block;		/* rg_frame.c's registered block, -1 none */
} rgubo_t;

typedef struct {
	int		num_samplers;
	rgsampler_t	samplers[RG_MAX_STAGE_SAMPLERS];
	int		num_ubos;
	rgubo_t		ubos[RG_MAX_STAGE_UBOS];
	int		num_ssbos;	/* read-only storage buffers, the reflection row's */
} rgstage_t;

typedef struct {
	GLuint			name;
	rprog_id_t		id;
	qboolean		compute;
	SDL_GPUShader		*vs, *fs;
	SDL_GPUComputePipeline	*cs;
	rgstage_t		stage[RPROG_MAX_STAGES];
} rgprog_t;

void	RG_Shader_Init (void);
void	RG_Shader_Shutdown (void);
/* Where this run's .vk.spv blobs come from, for the log ("the embedded
 * table (N blobs)" or "-gpushaders <dir>"), NULL if nowhere; RG_CreateDevice
 * refuses -gpu without them. */
const char *RG_ShaderSource (void);
const rgprog_t *RG_Program (GLuint name);
int	RG_ProgramCount (void);

/* ------------------------------------------------------------------ */
/* rg_pipeline.c                                                       */
/* ------------------------------------------------------------------ */

/* Vertex layouts the recorder draws with. */
typedef enum {
	RG_VL_IMM,		/* immvert_t: pos3 tc2 lm2 color4, 44 bytes */
	RG_VL_COUNT
} rg_vlayout_t;

void	RG_Pipeline_Init (void);
void	RG_Pipeline_Shutdown (void);
/* The pipeline for the current draw state (gl_pipeline.c's shadow), `prog`,
 * primitive and layout, created on first use; NULL when the state has no
 * SDL_GPU equivalent (logged once through RG_NoteUnhandled). */
SDL_GPUGraphicsPipeline *RG_PipelineFor (const rgprog_t *prog, SDL_GPUPrimitiveType prim,
					 rg_vlayout_t layout, SDL_GPUTextureFormat color,
					 SDL_GPUTextureFormat depth);
int	RG_PipelineCount (void);

/* ------------------------------------------------------------------ */
/* rg_frame.c                                                          */
/* ------------------------------------------------------------------ */

void	RG_Frame_Init (void);
void	RG_Frame_Shutdown (void);
/* Ends the open segment (copy pass, then its render pass) so that what is
 * recorded next sees a change that cannot share a pass with what came
 * before: an upload into a texture an earlier draw of the segment reads.
 * Outside a frame, submits the queued uploads alone. */
void	RG_Frame_Flush (void);
/* Serial of the open segment, for RG_Res_ForDraw's hazard stamp. */
unsigned int RG_Frame_Segment (void);
qboolean RG_Frame_SegmentHasDraws (void);
void	RG_Frame_End (qboolean show);	/* RG_EndFrame's body */
void	RG_Frame_Status (void);
/* The block rg_frame.c registered under `name`, -1 if none. */
int	RG_Frame_BlockIndex (const char *name);

#endif /* RG_INTERNAL_H */
