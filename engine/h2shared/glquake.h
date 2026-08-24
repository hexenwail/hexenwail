/* glquake.h -- common glquake header
 *
 * Copyright (C) 1996-1997  Id Software, Inc.
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

#ifndef GLQUAKE_H
#define GLQUAKE_H

/* Same implication glheader.h and gl_func.h make, restated because this
 * header does not include either and its texture format choice below depends
 * on the tier.  uhexen2-0py6. */
#if defined(__EMSCRIPTEN__) && !defined(USE_GLES)
#define USE_GLES 1
#endif

/* ====================================================================
   COMMON DEFINITIONS
   ================================================================== */

#define MAX_GLTEXTURES		8192
#define MAX_EXTRA_TEXTURES	156	/* 255-100+1 */
#define	MAX_CACHED_PICS		256
/* Lightmap page ceiling.  Large third-party maps (SoT 'tibet') exhausted
 * the stock 256 and died in AllocBlock with a fatal "full".  Pages are
 * BLOCK_WIDTH x BLOCK_HEIGHT luxels.  Only the pages a map actually uses are
 * allocated — see the growable lightmaps[] buffer in gl_rsurf.c — so raising
 * this costs nothing at runtime beyond the small per-page bookkeeping
 * arrays.  Keep it a multiple of LM_ATLAS_COLS.  uhexen2-vfvh.
 *
 * 512 -> 1536 for uhexen2-jwzf.  The LM_GUTTER padding around every surface
 * costs area superlinearly on small rects — most Quake surfaces are a handful
 * of luxels across, and a 4x4 becomes 8x8 — and demo1, a tiny map, went from
 * 4 pages to 10.  At 2.5x, a map sitting at 400 pages before would have hit
 * the 512 ceiling and died in AllocBlock on a map that loads today, so the
 * ceiling moves with the cost.  3x rather than 2.5x for headroom, since the
 * multiplier depends on a map's surface size distribution and 2.5 is one
 * sample.  The bookkeeping arrays grow to about 1.6 MB, from 0.5.
 *
 * Maps that then make the ATLAS too tall for the driver are not a new failure:
 * uhexen2-6yzs already catches that and falls back to per-page lightmaps,
 * which is slower and costs brush-entity instancing but renders correctly. */
#define	MAX_LIGHTMAPS		1536

/* maximum allowed size of a surface
 * vanilla limit was 16+1 (for linear sampling),
 * although glquake used 18 for some reason.
 * bigger values allow for less surfaces in certain places. */
#define	MAX_SURFACE_LIGHTMAP	255

#define	GL_UNUSED_TEXTURE	(~(GLuint)0)

/* uhexen2-khsa: GL 4.3 core profile requires sized internal formats.  The
 * legacy GLquake values (3, 4) are interpreted inconsistently across
 * drivers — most strip alpha for "3" and keep it for "4", but NVIDIA Win64
 * has been observed keeping alpha for both, which lights up palette-index-
 * 255 texels (d_8to24table[255] &= MASK_rgb in gl_vidsdl.c) with tex.a=0
 * and triggers the salias_frag discard path on otherwise opaque models.
 * gl_draw.c:2142 already overrides to GL_RGBA8 for TEX_FENCE/TEX_HOLEY for
 * the symmetric reason. */
/* uhexen2-6yh1: GL_RGB8 is not usable on the ES tier.  ES 3.0 validates the
 * (internalformat, format, type) triple of glTexImage2D against a fixed table
 * where sized GL_RGB8 admits only format GL_RGB, and every upload in this
 * engine hands over GL_RGBA/GL_UNSIGNED_BYTE (gl_draw.c GL_Upload32,
 * gl_rmisc.c).  Desktop GL accepts the mismatch and discards the alpha; ES
 * rejects the call with GL_INVALID_OPERATION and leaves the texture object
 * with no image at all, so it samples black.
 *
 * That silently blacked out every texture NOT flagged TEX_ALPHA -- measured on
 * demo1: 55 of them, including conback, both status bar halves, upsky, the
 * liquid warps (*lowlight, *rtex078, *rtex346) and a number of model skins.
 * Nothing reported it because the GL debug callback is desktop-only.
 *
 * RGBA8 on that tier costs one stored byte per texel and nothing else -- the
 * source data is already RGBA, and the alpha it now keeps is the 255 the
 * loader put there. */
#ifdef USE_GLES
#define	gl_solid_format		0x8058	/* GL_RGBA8 -- GL_RGB8 is invalid here */
#else
#define	gl_solid_format		0x8051	/* GL_RGB8 */
#endif
#define	gl_alpha_format		0x8058	/* GL_RGBA8 */

/* # of supported texture filter modes[] (gl_draw.c) */
#define	NUM_GL_FILTERS		6

/* defs for palettized textures	*/
#define	INVERSE_PAL_R_BITS	6
#define	INVERSE_PAL_G_BITS	6
#define	INVERSE_PAL_B_BITS	6
#define	INVERSE_PAL_TOTAL_BITS	(INVERSE_PAL_R_BITS + INVERSE_PAL_G_BITS + INVERSE_PAL_B_BITS)
#define	INVERSE_PAL_SIZE	(1 << INVERSE_PAL_TOTAL_BITS)

/* r_local.h defs		*/
#define ALIAS_BASE_SIZE_RATIO		(1.0 / 11.0)
					// normalizing factor so player model works
					// out to about 1 pixel per triangle
#define MAX_SKIN_HEIGHT		2048

#define BACKFACE_EPSILON	0.01

#define SKYSHIFT		7
#define	SKYSIZE			(1 << SKYSHIFT)
#define SKYMASK			(SKYSIZE - 1)


/* ====================================================================
   ENDIANNESS: RGBA
   ================================================================== */

#if ENDIAN_RUNTIME_DETECT

/* initialized by VID_Init() */
extern unsigned int	MASK_r;
extern unsigned int	MASK_g;
extern unsigned int	MASK_b;
extern unsigned int	MASK_a;
extern unsigned int	MASK_rgb;
extern unsigned int	SHIFT_r;
extern unsigned int	SHIFT_g;
extern unsigned int	SHIFT_b;
extern unsigned int	SHIFT_a;

#else	/* ENDIAN_RUNTIME_DETECT */

#if (BYTE_ORDER == BIG_ENDIAN)	/* R G B A */
#define	MASK_r		0xff000000
#define	MASK_g		0x00ff0000
#define	MASK_b		0x0000ff00
#define	MASK_a		0x000000ff
#define	SHIFT_r		24
#define	SHIFT_g		16
#define	SHIFT_b		8
#define	SHIFT_a		0
#elif (BYTE_ORDER == LITTLE_ENDIAN) /* A B G R */
#define	MASK_r		0x000000ff
#define	MASK_g		0x0000ff00
#define	MASK_b		0x00ff0000
#define	MASK_a		0xff000000
#define	SHIFT_r		0
#define	SHIFT_g		8
#define	SHIFT_b		16
#define	SHIFT_a		24
#endif

#define	MASK_rgb	(MASK_r|MASK_g|MASK_b)

#endif	/* ENDIAN_RUNTIME_DETECT */


/* ====================================================================
   TYPES
   ================================================================== */

/* texture types */
typedef struct
{
	GLuint		texnum;
	float	sl, tl, sh, th;
} glpic_t;

typedef struct cachepic_s
{
	char		name[MAX_QPATH];
	qpic_t		pic;
	byte		padding[32];	/* for appended glpic */
} cachepic_t;

typedef struct
{
	GLuint		texnum;
	char	identifier[MAX_QPATH];
	int		width, height;
	int		flags;
	unsigned short	crc;
} gltexture_t;

/* texture filters */
typedef struct
{
	const char	*name;
	int	minimize, maximize;
} glmode_t;

/* particle enums and types: note that hexen2 and
   hexenworld versions of these are different!! */
#include "particle.h"


/* ====================================================================
   GLOBAL VARIABLES
   ================================================================== */

/* gl texture objects */
extern	GLuint		currenttexture;
extern	GLuint		particletexture;
extern	GLuint		lightmap_textures[MAX_LIGHTMAPS];
extern	GLuint		playertextures[MAX_CLIENTS];
extern	GLuint		gl_extra_textures[MAX_EXTRA_TEXTURES];	// generic textures for models

/* Real glBindTexture calls issued this frame -- the ones that survive the
 * redundancy filter below.  Reported by r_speeds 2.
 *
 * This exists to answer uhexen2-im9g, whose first condition for reconsidering
 * ARB_bindless_texture is "profile first and show that per-draw texture
 * binding is actually a dominant bucket".  There was no way to show that:
 * every other cost in the frame has an rprof_ counter and this one had none,
 * so the question could only be argued, not measured.  A counter is not the
 * feature; it is what the decision was waiting on. */
extern	int		c_texbinds;

/* the GL_Bind macro */
#define GL_Bind(texnum)							\
	do {								\
		if (currenttexture != (texnum))				\
		{							\
			currenttexture = (texnum);			\
			c_texbinds++;					\
			glBindTexture_fp(GL_TEXTURE_2D,currenttexture);	\
		}							\
	} while (0)

extern	int		gl_texlevel;
extern	int		numgltextures;
extern	qboolean	flush_textures;
extern	gltexture_t	gltextures[MAX_GLTEXTURES];

extern	int		gl_filter_idx;
extern	float		gldepthmin, gldepthmax;
extern	int		glx, gly, glwidth, glheight;

extern	glmode_t	gl_texmodes[NUM_GL_FILTERS];

/* hardware-caps related globals */
extern	GLint		gl_max_size;
extern	GLfloat		gl_max_anisotropy;
extern	int		gl_max_samples;	/* GL_MAX_SAMPLES; bounds both the Options
					 * picker and the scene FBO's sample count */
extern	qboolean	have_stencil;
extern	qboolean	gl_clipcontrol_able;	/* reversed-Z when true */

/* Renderer capability summary, filled in by GL_InitRendererCaps in GL_Init.
 *
 * The individual `gl_*_able` flags above and the USE_GLES compile-time tier
 * remain authoritative for the code paths that branch on them -- this is the
 * one place that collects the answers so `renderer_status` can print them and
 * the Options menu can hide controls whose backing feature is missing.  It is
 * reporting and gating, not a second source of truth: do not reimplement an
 * existing `#ifdef USE_GLES` or entry-point probe as a lookup here.
 * Ported from alextnewman/hexenwail d2c46f078. */
typedef enum
{
	GL_RENDERER_DESKTOP_43,
	GL_RENDERER_GLES3
} gl_renderer_profile_t;

typedef struct
{
	gl_renderer_profile_t	profile;
	const char		*profile_name;
	qboolean		anisotropy;
	qboolean		float_color_buffer;	/* RGBA16F render targets, i.e. HDR */
	qboolean		shader_storage;
	qboolean		compute_shaders;
	qboolean		indirect_draw;
	qboolean		indexed_blending;	/* glBlendFunci, i.e. WBOIT */
	qboolean		gpu_particles;
	qboolean		skeletal_animation;
	qboolean		oit;
	qboolean		postprocess;
	qboolean		fbo_selftest;
} gl_renderer_caps_t;

extern	gl_renderer_caps_t	gl_renderer_caps;

void	GL_ReportLightmapStatus (void);

/* Block-compressed texture families backing the DDS/KTX loader
 * (engine/h2shared/img_dds.c, uhexen2-0vgo.5).  Probed in GL_Init.  The three
 * ship independently: S3TC (BC1-3) is an extension on every driver and never
 * core GL, while RGTC (BC4/5) and BPTC (BC7) are core on desktop GL 3.0/4.2
 * but absent on the ES tier.  A container whose format is not backed here is
 * refused, and the caller falls through to the PNG/TGA/PCX path. */
extern	qboolean	gl_have_s3tc;
extern	qboolean	gl_have_rgtc;
extern	qboolean	gl_have_bptc;

/* Frame-resources streaming buffer ring (engine/h2shared/gl_buffer.c).
 * uhexen2-8pc2: Ironwail-parity replacement for raw glBufferSubData → bind
 * → draw uploads.  See gl_buffer.c for the full architecture. */
extern	qboolean	gl_buffer_storage_able;	/* ARB_buffer_storage present */
extern	qboolean	gl_multi_bind_able;	/* ARB_multi_bind present */
extern	qboolean	gl_sync_able;		/* ARB_sync present */
extern	GLint		gl_ssbo_align;
extern	GLint		gl_ubo_align;

void GL_CreateFrameResources (void);
void GL_DeleteFrameResources (void);
void GL_AcquireFrameResources (void);
void GL_ReleaseFrameResources (void);
void GL_ClearBufferBindings (void);
void GL_AddGarbageBuffer (GLuint handle);

/* Generic-target buffer binder that keeps the binding cache in sync.
 * Prefer over raw glBindBuffer_fp for ARRAY/ELEMENT_ARRAY/SHADER_STORAGE/
 * UNIFORM/DRAW_INDIRECT targets — raw binds desync the cache and cause
 * subsequent cached binds to short-circuit to the wrong buffer. */
void GL_BindBuffer (GLenum target, GLuint buffer);

void GL_Upload (GLenum target, const void *data, size_t numbytes,
		GLuint *outbuf, GLintptr *outofs);
void GL_BindBufferRange (GLenum target, GLuint index,
			 GLuint buffer, GLintptr offset, GLsizeiptr size);
/* Indexed binder that keeps the range cache in sync.  Prefer over raw
 * glBindBufferBase_fp for SHADER_STORAGE/UNIFORM: a raw bind leaves the
 * cache stale and the next GL_BindBufferRange can short-circuit onto a
 * binding that is no longer there. */
void GL_BindBufferBase (GLenum target, GLuint index, GLuint buffer);
void GL_BindBuffersRange (GLenum target, GLuint first, GLsizei count,
			  const GLuint *buffers,
			  const GLintptr *offsets,
			  const GLsizeiptr *sizes);

#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE			0x935F
#endif
#ifndef GL_LOWER_LEFT
#define GL_LOWER_LEFT			0x8CA1
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING	0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING	0x8CAA
#endif
#ifndef GL_COPY_READ_BUFFER
#define GL_COPY_READ_BUFFER		0x8F36
#endif
#ifndef GL_COPY_WRITE_BUFFER
#define GL_COPY_WRITE_BUFFER		0x8F37
#endif

/* view origin */
extern	vec3_t		vup;
extern	vec3_t		vpn;
extern	vec3_t		vright;
extern	vec3_t		r_origin;

/* screen size info */
extern	refdef_t	r_refdef;
extern	vrect_t		scr_vrect;

extern	mleaf_t		*r_viewleaf, *r_oldviewleaf;
extern	float		r_world_matrix[16];
extern	entity_t	r_worldentity;
extern	qboolean	r_cache_thrash;		// compatability
extern	vec3_t		modelorg, r_entorigin;
extern	int		r_visframecount;	// ??? what difs?
extern	int		r_framecount;
extern	mplane_t	frustum[4];
extern	int		c_brush_polys, c_alias_polys;

/* palette stuff */
extern	const int		ColorIndex[16];
extern	const unsigned int	ColorPercent[16];
extern	float		RTint[256], GTint[256], BTint[256];
extern	unsigned char	*inverse_pal;

/* global cvars */
extern	cvar_t	r_norefresh;
extern	cvar_t	r_drawentities;
extern	cvar_t	r_drawworld;
extern	cvar_t	r_drawviewmodel;
extern	cvar_t	r_speeds;
extern	cvar_t	r_waterwarp;
extern	cvar_t	r_fullbright;
extern	cvar_t	r_lightmap;
extern	cvar_t	r_shadows;
extern	cvar_t	r_mirroralpha;
extern	cvar_t	r_wateralpha;
extern	cvar_t	r_skyalpha;
extern	cvar_t	r_dynamic;
extern	cvar_t	r_lerplightstyles;
extern	cvar_t	r_farclip;
extern	cvar_t	r_entdist;
extern	cvar_t	r_viewmodel_fov;
extern	cvar_t	cl_gun_fovscale;
extern	cvar_t	r_lavaalpha;
extern	cvar_t	r_slimealpha;
extern	cvar_t	r_telealpha;
extern	cvar_t	r_turbalpha;	/* default alpha for unknown turb names (uhexen2-6697 fallback) */
extern	cvar_t	r_turbtjunc;	/* heal T-junctions between adjacent turb surfaces (uhexen2-9o7u) */
extern	cvar_t	r_caustics;	/* underwater caustics on/off (uhexen2-6bfm) */
extern	cvar_t	r_caustics_intensity;	/* caustics overlay strength */
extern	cvar_t	r_motionblur;
extern	cvar_t	r_alias_gpu;
extern	cvar_t	r_alphatocoverage;
extern	cvar_t	r_novis;
extern	cvar_t	r_lerpmodels;
extern	cvar_t	r_lerp_viewmodel;
extern	cvar_t	r_lerp_complete;
extern	cvar_t	r_nolerp_list;
extern	cvar_t	r_dlight_model_list;	/* uhexen2-vdmz */
extern	cvar_t	r_water_pixel_warp;	/* uhexen2-9o7u */
extern	cvar_t	r_lerp_autodetect;
extern	cvar_t	r_lerp_autodetect_threshold;
extern	cvar_t	r_showbboxes;
extern	cvar_t	r_showbboxes_think;
extern	cvar_t	r_showbboxes_health;
extern	cvar_t	r_showbboxes_targets;
extern	cvar_t	r_showbboxes_links;
extern	cvar_t	r_pointfile_depthtest;
extern	cvar_t	leak_color;
extern	cvar_t	r_wholeframe;
extern	cvar_t	r_clearcolor;
extern	cvar_t	r_alphasort;
extern	cvar_t	r_vm_watch;	/* log viewmodel draw-state transitions (uhexen2-ac4c) */
extern	cvar_t	r_texture_external;
extern	cvar_t	r_texture_external_hud;

#if defined(H2W)
extern	cvar_t	r_netgraph;
extern	cvar_t	r_entdistance;
extern	cvar_t	r_teamcolor;
#endif

extern	cvar_t	gl_playermip;

extern	cvar_t	gl_clear;
extern	cvar_t	gl_cull;
extern	cvar_t	gl_poly;
extern	cvar_t	gl_zfix;
extern	cvar_t	gl_purge_maptex;
extern	cvar_t	gl_log_texgen;	/* uhexen2-0fsq diagnostic */
extern	cvar_t	gl_smoothmodels;
extern	cvar_t	gl_affinemodels;
extern	cvar_t	gl_polyblend;
extern	cvar_t	gl_cshiftpercent;
extern	cvar_t	gl_keeptjunctions;
extern	cvar_t	gl_reporttjunctions;
extern	cvar_t	gl_flashblend;
extern	cvar_t	gl_nocolors;
extern	cvar_t	gl_waterripple;
extern	cvar_t		gl_subdivide_size;
extern	cvar_t	gl_waterwarp_speed;
extern	cvar_t	gl_waterwarp_amount;
float R_WaterWarpSpeed (void);
float R_WaterWarpAmount (void);
extern	cvar_t	gl_particles;
extern	cvar_t	gl_fullbrights;
extern	cvar_t	gl_overbright_models;
extern	cvar_t	gl_overbright;
extern	cvar_t	r_lightmap_bicubic;
extern	cvar_t	gl_fxaa;
extern	cvar_t	gl_lmatlas;
extern	cvar_t	gl_glows;
extern	cvar_t	gl_other_glows;
extern	cvar_t	gl_missile_glows;
extern	cvar_t	gl_torch_dlight;
extern	cvar_t	gl_glow_intensity;
extern	cvar_t	gl_flashintensity;

extern	cvar_t	gl_coloredlight;
extern	cvar_t	gl_extra_dynamic_lights;
extern	cvar_t	gl_lightmapfmt;

/* other globals */
extern	int		gl_coloredstatic;	/* value of gl_coloredlight stored at level start */
extern	int		gl_lightmap_format;	/* value of gl_lightmapfmt stored at level start */

extern	vec3_t		lightcolor;
extern	vec3_t		lightspot;

extern	texture_t	*r_notexture_mip;
extern	int		d_lightstylevalue[256];	// 8.8 fraction of base light value

extern	byte		*playerTranslation;
extern	const int	color_offsets[MAX_PLAYER_CLASS];

extern	qboolean	mirror;
extern	mplane_t	*mirror_plane;
extern	int		mirrortexturenum;	/* quake texturenum, not gltexturenum */
extern	int		skytexturenum;		/* index in cl.loadmodel, not gl texture object */


/* ====================================================================
   GLOBAL FUNCTIONS
   ================================================================== */

void GL_BeginRendering (int *x, int *y, int *width, int *height);
void GL_EndRendering (void);

void GL_Set2D (void);

GLuint GL_LoadTexture (const char *identifier, byte *data,
			int width, int height, int flags);
/* LoadTexture Flags */
#define	TEX_DEFAULT		0
#define	TEX_MIPMAP		(1 << 1)
#define	TEX_ALPHA		(1 << 2)
#define	TEX_RGBA		(1 << 5)	/* texture is 32 bit RGBA, not 8 bit */
/* TEX_NEAREST and TEX_LINEAR aren't supposed to be ORed with TEX_MIPMAP */
#define	TEX_NEAREST		(1 << 6)	/* force point sampled */
#define	TEX_LINEAR		(1 << 7)	/* force linear filtering */
/* duplicated EF_ values from gl_model.h: */
#define	TEX_TRANSPARENT		(1 << 12)	/* Transparent sprite				*/
#define	TEX_HOLEY		(1 << 14)	/* Solid model with color 0			*/
#define	TEX_FENCE		(1 << 16)	/* Fence texture (binary transparency)		*/
#define	TEX_SPECIAL_TRANS	(1 << 15)	/* Translucency through the particle table	*/
#define	TEX_EMBEDDED_MIPS	(1 << 17)	/* Data contains all 4 BSP mip levels		*/
/* Never pick a block-compressed internal format for this texture, whatever
 * gl_compress_textures says.  BC7 is a lossy 4x4-block codec: on text glyphs
 * and HUD art the block boundaries are visible at 1:1 magnification, and on an
 * alpha-tested cutout the interpolated alpha inside a block fringes the edge
 * the alpha test is supposed to make crisp.  Ironwail 1011ff8 / 74d8e74;
 * uhexen2-r7zu. */
#define	TEX_UNCOMPRESSED	(1 << 18)

/* Block-compressed internal formats, for the DDS/KTX replacement path
 * (uhexen2-0vgo.5).  Spelled out here rather than taken from the system GL
 * headers: S3TC is an extension everywhere, and the ES tier's headers do not
 * carry the desktop BPTC/RGTC enums at all. */
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT		0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT	0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT	0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT	0x83F3
#endif
#ifndef GL_COMPRESSED_RED_RGTC1
#define GL_COMPRESSED_RED_RGTC1			0x8DBB
#endif
#ifndef GL_COMPRESSED_RG_RGTC2
#define GL_COMPRESSED_RG_RGTC2			0x8DBD
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM		0x8E8C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM	0x8E8D
#endif

/* Upload an already-resolved replacement image (img_load.h) as a GL texture,
 * taking the compressed or the RGBA path according to what was found on disk.
 * extraflags carries the caller's own requirements -- TEX_MIPMAP, TEX_FENCE,
 * TEX_HOLEY -- and TEX_ALPHA / TEX_RGBA are added here from the image itself.
 * Returns the GL texture name. */
struct imgreplace_s;
GLuint GL_LoadReplacement (const char *identifier, const struct imgreplace_s *r, int extraflags);

GLuint GL_LoadPicTexture (qpic_t *pic);
void D_ClearOpenGLTextures (int last_tex);

qboolean R_CullBox (vec3_t mins, vec3_t maxs);
void R_DrawBrushModel (entity_t *e, qboolean Translucent);
void R_DrawWorld (void);
void R_RenderBrushPoly (entity_t *e, msurface_t *fa, qboolean override);
void R_RotateForEntity (entity_t *e);
void R_StoreEfrags (efrag_t **ppefrag);

#if defined(QUAKE2)
void R_LoadSkys (void);
void R_DrawSkyBox (void);
void R_ClearSkyBox (void);
#endif
void GL_SubdivideSurface (qmodel_t *m, msurface_t *fa);
void GL_HealTurbTJunctions (qmodel_t *mod);
void EmitWaterPolys (msurface_t *fa);
void EmitBothSkyLayers (msurface_t *fa);
void R_DrawSkyChain (msurface_t *s);
/* phase: ALL = both (mirror path), OPAQUE = opaque liquids only,
 * TRANSLUCENT = translucent liquids only.  Opaque liquids must draw
 * before OIT_BeginTranslucency or they're eaten by the WBOIT resolve. */
#define WATER_PHASE_ALL		0
#define WATER_PHASE_OPAQUE	1
#define WATER_PHASE_TRANSLUCENT	2
void R_DrawWaterSurfaces (int phase);

int R_GetPimpFlags (entity_t *e, float **gsettings_out);
int R_GetEntityModelFlags (entity_t *e);

void R_RenderDlights (void);
void R_MarkLights (dlight_t *light, int bit, mnode_t *node);
/* Side planes for an arbitrary camera, into the caller's array rather than
 * frustum[].  R_PushDlights needs them before R_SetFrustum runs.  uhexen2-137k */
void R_BuildFrustum (const vec3_t origin, const vec3_t pn, const vec3_t right,
		     const vec3_t up, float fov_x, float fov_y, mplane_t out[4]);
void R_AnimateLight(void);
int R_LightPoint (vec3_t p);
float R_LightPointColor (vec3_t p, lightcache_t *cache);
void GL_BuildLightmaps (void);
void R_BuildWorldVBO (void);
void R_FreeWorldVBO (void);
void R_BuildSkyStencilVBO (void);
void R_FreeSkyStencilVBO (void);
void R_BuildWorldCull (void);
void R_FreeWorldCull (void);
void R_DispatchWorldCull (void);
void R_DrawWorldCulled (void);
qboolean R_WorldCullAvailable (void);
void R_BuildHiZ (void);
void R_FreeHiZ (void);
void R_BuildHiZForNextFrame (void);
void GL_SetupLightmapFmt (void);
void GL_MakeAliasModelDisplayLists (qmodel_t *m, aliashdr_t *hdr);

void R_InitParticleTexture (void);
void R_InitExtraTextures (void);
#if defined(H2W)
void R_NetGraph (void);
void R_InitNetgraphTexture (void);
#endif

/* The pointfile path itself is stored in r_part.c, which every renderer
 * compiles, so its declarations live in r_part.h.  Only the drawing below is
 * GL-specific. */

void R_ReadPointFile_f (void);
void R_ClearPointFile (void);
/* Canvas-space position of the leak origin, valid this frame only. */
qboolean R_GetPointFileLabelPos (float *x, float *y);
void R_AliasInfo_f (void);	/* uhexen2-khsa diagnostic */
void R_TranslatePlayerSkin (int playernum);

#endif	/* GLQUAKE_H */

