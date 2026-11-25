/* gl_postprocess.c -- GLSL post-process gamma/contrast correction
 *
 * Renders the scene into an FBO, then blits to the default framebuffer
 * with a shader that applies gamma and contrast.  When both gamma and
 * contrast are 1.0 (identity), the FBO is not used and there is zero
 * performance cost.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "sdl_inc.h"
#include "gl_postprocess.h"
#include "gl_matrix.h"
#include "gl_shader.h"
#include "gl_uniforms.h"	/* R_SetOITMode, the OIT twins' OitFrag (#450 W3) */
#include "gl_pipeline.h"
#include "gl_vbo.h"
#include "draw.h"
#include "gl_fbo.h"	/* framebuffers and renderbuffers, through the seam (#340 P4a-1) */
#include "gl_ssao.h"	/* R_SSAO_WantsSceneFBO: SSAO draws through the scene FBO (#539) */
#ifdef USE_SDL_GPU
#include "rg_device.h"	/* R_GPU(): the chain's -gpu gates and uniform mirrors */
#endif

/* GL defines that may be missing on some platforms (MinGW, ES3) */
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_COLOR_ATTACHMENT1
#define GL_COLOR_ATTACHMENT1 0x8CE1
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_TEXTURE_2D_MULTISAMPLE
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#endif

/* OIT needs per-draw-buffer blend equations (glBlendFunci, GL 4.0), which
 * WebGL2 does not expose.  There glBlendFunci_fp is a no-op function-like
 * macro, so it cannot be tested as a value — go through this helper instead. */
#ifdef USE_GLES
#define HW_OIT_HAS_BLEND_FUNCI	0
#else
#define HW_OIT_HAS_BLEND_FUNCI	(glBlendFunci_fp != NULL)
#endif

/* The programs in this file -- post-process, bloom, OIT resolve, the GPU
 * palette-LUT builder -- are built by id (R_Program_Create, #340 P1b).  Their
 * sources, and the PP_*_HEADER tier headers they compile with (TIER_ES_PP on
 * the ES tier, uhexen2-7wdv), are their rows in gl_shader.c's rprog_defs. */

/* FBO state — scaled 3D scene */
static GLuint	pp_fbo;		/* render target (may be multisampled) */
static GLuint	pp_color_rb;	/* multisampled color renderbuffer (0 if no MSAA) */
static GLuint	pp_depth_rb;	/* multisampled depth/stencil renderbuffer (0 if no MSAA) */
static GLuint	pp_depth_tex;	/* non-MSAA depth/stencil texture, sampler-readable for Hi-Z (uhexen2-xd87) */
static GLuint	pp_resolve_fbo;	/* resolve target (non-multisampled, for shader blit) */
static GLuint	pp_color_tex;	/* resolved color texture */
static int	pp_width, pp_height;
static int	pp_samples;	/* MSAA sample count (0 or 1 = no MSAA) */
static qboolean	pp_fbo_failed;	/* true if FBO creation failed — don't retry every frame */
static GLuint	pp_copyback_tex;/* fallback texture for copyback mode (no FBO) */
static int	pp_copyback_w, pp_copyback_h;

/* Soft-particle depth snapshot (uhexen2-mf9u).  Sprites need to read the
 * depth of the opaque scene behind them, but at sprite-draw time that depth
 * is the bound framebuffer's own attachment — sampling it is a rendering
 * feedback loop, undefined under the GL 4.3 spec.  Blit a private copy once,
 * after the opaque pass and before any translucency, and sample that.
 * Snapshotting there also makes the fade opaque-only by construction, which
 * is what it should be: a sprite ought to dissolve into the wall behind it,
 * not into whichever translucent entity happened to sort earlier. */
static GLuint	soft_depth_fbo;
static GLuint	soft_depth_tex;
static int	soft_depth_w, soft_depth_h;
static GLenum	soft_depth_fmt;		/* internal format the texture was built with */
static qboolean	soft_depth_valid;	/* captured successfully this frame */

/* FBO state — native res for 2D composite */
static GLuint	pp_native_fbo;
static GLuint	pp_native_color_tex;
static GLuint	pp_native_depth_rb;
static int	pp_native_w, pp_native_h;
static GLenum	pp_native_fmt;		/* colour format the native FBO was built with */

/* Bloom post-process FBO pyramid (1/2, 1/4, 1/8, 1/16 res) */
#define BLOOM_LEVELS 4
static GLuint	bloom_fbo[BLOOM_LEVELS];
static GLuint	bloom_tex[BLOOM_LEVELS];
static int	bloom_w[BLOOM_LEVELS], bloom_h[BLOOM_LEVELS];

/* Bloom shader programs */
static GLuint	bloom_bright_prog;	/* threshold extraction */
static GLuint	bloom_down_prog;	/* downsample 4-tap box */
static GLuint	bloom_up_prog;		/* upsample 3x3 tent + additive blend */

/* Bloom shader uniform locations */
static GLint	bloom_bright_loc_scene, bloom_down_loc_scene, bloom_up_loc_scene;

/* The bloom passes' one uniform block (engine/shaders/bloom.inc), replacing
 * the loose u_rcpframe / u_threshold the inline programs took (#450).  GL
 * binding point 6: 0 Hi-Z, 1/2 the shared vert/frag blocks, 3 SsaoParams,
 * 4 CullParams, 5 LcParams.  Assigned from C with glUniformBlockBinding,
 * since ES 3.00 cannot name a block binding in the shader. */
#define BLOOM_PARAMS_UBO_BINDING	6

typedef struct {
	float	p[4];	/* xy reciprocal source size, z bright-pass threshold */
} bloom_params_t;

COMPILE_TIME_ASSERT(bloomparams_size, sizeof(bloom_params_t) == 16);
static GLuint	bloom_dummy_vao;		/* dummy VAO required for glDrawArrays in GL 4.3 core */

/* Shader state */
static GLuint	pp_program;
static GLint	pp_loc_scene;

/* The post-process program's parameters (#450).  They were sixteen loose
 * uniforms; now u_mvp is the PpVert block and the rest PpParams, in
 * engine/shaders/spp_{vert,frag}.glsl.  GL binding points 8 and 9 (0 Hi-Z,
 * 1/2 the shared blocks, 3 SsaoParams, 4 CullParams, 5 LcParams,
 * 6 BloomParams, 7 OitParams).
 *
 * pp_params is a SHADOW, written field by field where the glUniform calls
 * used to be and uploaded whole before each draw.  It persists between draws
 * on purpose: loose uniforms kept their last value per program, and the
 * native-resolution pass relies on that -- it never sets lutmax or the bloom
 * strength and inherits whatever the previous final pass left.  Two own
 * buffers rather than the GL_Upload ring, because this runs on the ES tier,
 * where GL_Upload is a stub (see PP_PushBloomParams). */
#define PP_VERT_UBO_BINDING	8
#define PP_FRAG_UBO_BINDING	9

typedef struct {
	float	p0[4];	/* gamma, contrast, dither, lutmax */
	float	p1[4];	/* scale, waterwarp, time, fxaa_on */
	float	p2[4];	/* rcpFrame.xy, viewdelta.xy */
	float	p3[4];	/* motionblur, hdr_exposure, u_bloom_strength, unused */
	GLint	i0[4];	/* softemu */
} pp_params_t;		/* 80 bytes, std140 */

COMPILE_TIME_ASSERT(ppparams_size, sizeof(pp_params_t) == 80);

static pp_params_t	pp_params;
static float		pp_vert_mvp[16];
static GLuint		pp_vert_ubo, pp_frag_ubo;

/* The contrast the shader applies: v_contrast clamped to Ironwail's 1..2
 * (gl_rmain.c: q_min(2.0f, q_max(1.0f, vid_contrast.value))).  Below 1 it
 * is identity, as upstream, not a darkening. */
static float PP_Contrast (void)
{
	float c = v_contrast.value;
	if (c < 1.0f) c = 1.0f;
	if (c > 2.0f) c = 2.0f;
	return c;
}
static GLint	pp_loc_paletteLUT;
static GLint	pp_loc_bloom_tex;

/* Palette LUT state.  LUT_DIM^3 grid, RGBA8 -- see PP_UploadLUT for why the
 * colour is baked rather than looked up from a palette uniform, and why RGBA
 * and not RGB (rgba8 is an image-storable format; rgb8 is not, and the compute
 * builder writes into this same texture). */
/* The nearest-colour LUT is Ironwail's 128^3 (GitHub #80): at 32^3 the grid
 * quantised colour before the palette match even ran, a floor under every
 * metric.  The CPU builder that the ES / WebGL2 tier depends on searches a
 * sorted palette with an exact lower bound instead of all 255 entries, which
 * is what makes 128^3 affordable there (PP_BuildPaletteLUT_CPU).
 *
 * The colormap LUT (r_softemu 3) stays 32^3: its candidates depend on the
 * target's luma, so it has no such bound, and 128^3 would be 537M
 * evaluations.  The shader is told which grid it is reading (lutmax).
 * PP_LUT_DIM itself is in gl_postprocess.h, because the compute builder's
 * program row (gl_shader.c) pastes it into its LUT_DIM. */
#define PP_LUT_TEXELS	(PP_LUT_DIM * PP_LUT_DIM * PP_LUT_DIM)
#define PP_CMAP_DIM	32
#define PP_CMAP_TEXELS	(PP_CMAP_DIM * PP_CMAP_DIM * PP_CMAP_DIM)

/* Colour-distance functions the nearest-colour search can minimise, in
 * upstream's order -- r_softemu_metric indexes this directly, and the compute
 * variants are compiled with MODE set to it (Ironwail gl_shaders.c:379).
 * uhexen2-h8yy. */
typedef enum {
	SOFTEMU_METRIC_NAIVE     = 0,	/* Euclidean, straight on sRGB */
	SOFTEMU_METRIC_RIEMERSMA = 1,	/* compuphase.com/cmetric.htm, on linear RGB */
	SOFTEMU_METRIC_OKLAB     = 2,	/* bottosson.github.io/posts/oklab */
	SOFTEMU_METRIC_COUNT
} softemu_metric_t;

static GLuint	pp_palette_lut;		/* nearest-colour LUT (r_softemu 1/2) */
static GLuint	pp_colormap_lut;	/* via gfx/colormap.lmp (r_softemu 3) */
static qboolean	pp_lut_built;
/* Which metric pp_palette_lut currently holds.  COUNT is the "nothing valid
 * yet" sentinel, so the first PP_UpdatePaletteLUT always builds. */
static softemu_metric_t	pp_lut_metric = SOFTEMU_METRIC_COUNT;

static qboolean	pp_initialized;
static qboolean	pp_active;		/* true when scene is being rendered to FBO this frame */
static qboolean	pp_native_active;	/* true when End3D transferred 3D → composite FBO; EndFrame skips warp/blur */
static float	pp_prev_yaw, pp_prev_pitch;	/* view angle tracking for motionblur delta */
static vec3_t	pp_prev_origin;			/* position tracking for motionblur delta */
static int	pp_saved_glwidth, pp_saved_glheight;	/* original viewport dims */
static float	pp_waterwarp_preview_end;	/* cl.time when waterwarp preview should end */

cvar_t	r_scale = {"r_scale", "1", CVAR_ARCHIVE};
cvar_t	r_softemu = {"r_softemu", "0", CVAR_ARCHIVE};
cvar_t	r_dither = {"r_dither", "0.5", CVAR_ARCHIVE};	/* dither strength (0-2), reduced to avoid AMD noise artifacts */
/* Ironwail's per-stage decomposition of r_softemu (uhexen2-a5nn.3).  Names,
 * defaults and archive flags are upstream's (Quake/gl_texmgr.c:41-45) so a
 * config means the same thing in both engines.  r_dither stays the master
 * strength and these scale it, exactly as upstream multiplies the two.
 *
 * The ladders are not identical and the mapping is written down in
 * R_SoftEmuParams below, because our r_softemu 0-3 is Off / Dithered / Banded
 * / Colormap and upstream's is OFF / FINE / COARSE / BANDED. */
cvar_t	r_softemu_dither_screen  = {"r_softemu_dither_screen",  "1.0", CVAR_ARCHIVE};
cvar_t	r_softemu_dither_texture = {"r_softemu_dither_texture", "1.0", CVAR_ARCHIVE};
/* -1 = follow the mode (on at r_softemu 3), 0 = never, > 0 = always. */
cvar_t	r_softemu_lightmap_banding = {"r_softemu_lightmap_banding", "-1", CVAR_ARCHIVE};
/* -1 = follow the mode (on at r_softemu 3), 0 = never, > 0 = always.  The one
 * softemu sub-cvar Ironwail puts in its menu. */
cvar_t	r_softemu_mdl_warp = {"r_softemu_mdl_warp", "-1", CVAR_ARCHIVE};
/* The 1997 span rasteriser's texture interpolation on world surfaces:
 * perspective-correct every 8 pixels, linear between (sworld_frag's SpanUV,
 * after D_DrawSpans8).  Its own switch, not a stage of r_softemu: the palette
 * look and this are independent, and either is wanted without the other.
 * Alias models already have theirs, r_softemu_mdl_warp.  GitHub #176. */
cvar_t	r_softemu_spans = {"r_softemu_spans", "0", CVAR_ARCHIVE};
/* Which colour-distance function the nearest-colour LUT minimises: -1 auto,
 * 0 naive RGB, 1 Riemersma, 2 OKLab.  Upstream's name, default and archive
 * flag (Ironwail gl_texmgr.c:41).  uhexen2-h8yy.
 *
 * APPLIES TO r_softemu 1 AND 2 ONLY.  Mode 3 resolves through gfx/colormap.lmp,
 * an authored table with no distance function anywhere in it -- see the essay
 * on PP_BuildColormapLUT -- so this changes nothing there.  The one exception
 * is a game with no colormap.lmp, where mode 3 falls back to the nearest-colour
 * table and the metric starts mattering again; PP_SelectMetric treats exactly
 * that case as upstream's BANDED.  PP_MetricChanged says so on the console
 * rather than let somebody set it under mode 3 and watch nothing happen. */
cvar_t	r_softemu_metric = {"r_softemu_metric", "-1", CVAR_ARCHIVE};

/*
===============
R_SoftEmuParams

The four floats the world fragment shader's u_softemu carries.  All zero at
r_softemu 0, which is the default, so a stock configuration reaches none of
the stages.

WHERE OUR LADDER AND UPSTREAM'S DIVERGE.  Ironwail runs OFF / FINE / COARSE /
BANDED; ours runs Off / Dithered / Banded / Colormap, and the names line up
badly.  What matters is which of upstream's shader variants each of ours
corresponds to:

  r_softemu 1 "Dithered" ~ FINE   -- screen-space dither, and upstream does it
                                     in the POSTPROCESS pass, not in the world
                                     shader.  So nothing here is live; the
                                     scale still applies, in GL_PostProcess.
  r_softemu 2 "Banded"   ~ COARSE -- upstream's DITHER == 1: luxel-snapped
                                     lightmap UVs, a negative LOD bias, and
                                     the two-noise texture dither.
  r_softemu 3 "Colormap" ~ BANDED -- upstream's DITHER >= 2: the same snap and
                                     a harder LOD bias, the lightmap quantised
                                     to 64 levels, and NO dithering at all.
                                     Ours additionally routes the result
                                     through gfx/colormap.lmp downstream,
                                     which upstream has no equivalent for.

NOISESCALE is upstream's 9/255 (Quake/gl_rmain.c), the amplitude one dither
step covers at 8 bits.
===============
*/
void R_SoftEmuParams (float out[4])
{
	static const float NOISESCALE = 9.0f / 255.0f;
	const int	mode = r_softemu.integer;
	float		noise;
	qboolean	banding;

	out[0] = out[1] = out[2] = out[3] = 0.0f;

	/* -1, the shipped value, means "follow the mode"; 0 never, > 0 always.
	 * "Always" has to raise the live flag too, or the banded lightmap would
	 * be sampled off the luxel grid and read as a posterised gradient. */
	banding = (r_softemu_lightmap_banding.value < 0.0f)
		? (mode == 3)
		: (r_softemu_lightmap_banding.value > 0.0f);

	if (mode < 2 && !banding)
		return;

	out[3] = 1.0f;
	out[1] = banding ? 1.0f : 0.0f;

	/* Upstream's BANDED variant carries no noise: the point of it is the
	 * bands.  Dithering them back out again would undo the mode. */
	if (mode == 2 && !banding)
	{
		noise = NOISESCALE * r_dither.value;
		out[0] = noise * r_softemu_dither_texture.value;
		out[2] = noise * r_softemu_dither_screen.value;
		if (out[0] < 0.0f) out[0] = 0.0f;
		if (out[2] < 0.0f) out[2] = 0.0f;
	}
}

/*
===============
R_SoftEmuMdlWarp

Whether alias models draw with affine (non-perspective-correct) texture
mapping this frame.

Upstream's ladder, translated (Ironwail r_alias.c:327-339): under its BANDED
mode any nonzero value turns it on, so the shipped -1 does; under anything
else it takes an explicitly positive one.  Ours is the same test against our
own top mode -- see R_SoftEmuParams for why r_softemu 3 is the one that
corresponds.  uhexen2-ktjv.
===============
*/
qboolean R_SoftEmuMdlWarp (void)
{
	if (r_softemu.integer == 3)
		return r_softemu_mdl_warp.value != 0.0f;
	return r_softemu_mdl_warp.value > 0.0f;
}
cvar_t	r_hdr = {"r_hdr", "0", CVAR_ARCHIVE};		/* 0=off, 1=ACES tonemap */
cvar_t	r_hdr_exposure = {"r_hdr_exposure", "1.0", CVAR_ARCHIVE};
/* 0 off, 1 weighted blended OIT, 2 moment-based OIT (GitHub #87; falls
 * back to 1 where the moment targets cannot be built). */
cvar_t	r_oit = {"r_oit", "0", CVAR_ARCHIVE};
cvar_t	r_bloom = {"r_bloom", "0", CVAR_ARCHIVE};		/* bloom post-process */
cvar_t	r_bloom_intensity = {"r_bloom_intensity", "1.0", CVAR_ARCHIVE};	/* bloom glow strength */
cvar_t	r_bloom_threshold = {"r_bloom_threshold", "1.0", CVAR_ARCHIVE};	/* luminance threshold */
/* Soft particles (uhexen2-mf9u).  Default off until it has been looked at on
 * real hardware; costs one full-res depth blit per frame plus a depth fetch
 * per sprite fragment when on.
 *
 * The scale is the fade distance in world units, and 8 is a measured choice,
 * not a guess.  sm_expld.spr — the sprite the original report came in about —
 * is 48 units across and detonates centred on the impact surface, so the fade
 * distance trades artifact removal against washing the sprite out.  Compared
 * headless on demo1 (Necromancer, raven staff, explosions on the plinth): 24
 * dissolves most of the flame and visibly drains the explosion; 8 removes the
 * hard rectangular cut while leaving the flame at close to full brightness. */
cvar_t	r_softparticles = {"r_softparticles", "0", CVAR_ARCHIVE};
cvar_t	r_softparticles_scale = {"r_softparticles_scale", "8", CVAR_ARCHIVE};

/* r_hdr wants RGBA16F colour attachments, which are core on the desktop tier
 * but need EXT_color_buffer_float on ES/WebGL2.  Without it the FBO comes back
 * incomplete and the whole post-process chain falls over, so read the cvar
 * through here rather than directly.  d2c46f078. */
static qboolean PP_HDRActive (void)
{
	return r_hdr.integer && gl_renderer_caps.float_color_buffer;
}

/* ------------------------------------------------------------------ */
/* Order-Independent Transparency (McGuire & Bavoil WBOIT)            */
/* ------------------------------------------------------------------ */

extern cvar_t	vid_config_fsaa;	/* MSAA sample count (drives the GL context) */
extern cvar_t	vid_fsaamode;		/* per-sample shading fraction, uhexen2-a5nn.27 */

static GLuint	oit_accum_tex;		/* RGBA16F accumulation */
static GLuint	oit_revealage_tex;	/* RGBA16F revealage (.r used) */
static GLuint	oit_fbo;		/* MRT FBO sharing scene depth/stencil */
static GLuint	oit_resolve_prog;	/* fullscreen resolve shader (sampler2D) */
static GLuint	oit_resolve_prog_msaa;	/* per-sample resolve (sampler2DMS) */
static GLuint	oit_resolve_vao;	/* dummy VAO required for glDrawArrays
					 * in GL 4.3 core profile — without one
					 * bound, the resolve draw was a silent
					 * GL_INVALID_OPERATION and the entire OIT
					 * composite was being discarded. */
static GLint	oit_resolve_loc_accum;
static GLint	oit_resolve_loc_reveal;
static GLint	oit_resolve_msaa_loc_accum;
static GLint	oit_resolve_msaa_loc_reveal;
static int	oit_samples;		/* >1 when accum/revealage are multisampled */
static int	oit_width, oit_height;
static qboolean	oit_available;		/* true if FBO + shader created OK */
static qboolean	oit_in_pass;		/* true between Begin/EndTranslucency */
/* Moment-based OIT (r_oit 2, GitHub #87).  The moment pass accumulates b0 and
 * four power moments of depth into these; the composite pass then re-draws
 * the translucents into oit_accum_tex, weighted by the transmittance
 * reconstructed from them, and the ordinary resolve composites the result.
 * 32-bit float: the moments of a deep stack lose everything in fp16. */
static GLuint	mboit_b0_tex;		/* R32F  absorbance sum */
static GLuint	mboit_mom_tex;		/* RGBA32F  depth moments */
static GLuint	mboit_fbo;
static qboolean	mboit_available;
static int	mboit_pass;		/* 0 none, 1 moments, 2 composite */

/* OIT resolve shaders: engine/shaders/soit_resolve_vert.glsl,
 * soit_resolve_frag.glsl and the desktop-only soit_resolve_msaa_frag.glsl,
 * built as RPROG_OIT_RESOLVE(_MSAA) (#450).  The MSAA variant uses sampler2DMS and
 * gl_SampleID, which are GLSL ES 3.10, and glTexImage2DMultisample_fp is a
 * no-op on the ES tier, so oit_samples never exceeds 1 there and that program
 * is never built on ES (OIT_InitShader).
 *
 * u_mboit, the loose int that said whether TexReveal holds MBOIT's b0 (#87),
 * is now the OitParams block (oit_resolve.inc), GL binding point 7: 0 Hi-Z,
 * 1/2 the shared blocks, 3 SsaoParams, 4 CullParams, 5 LcParams,
 * 6 BloomParams.  Like BloomParams it lives in its own buffer, not the
 * GL_Upload ring, because the resolve runs on the ES tier, where GL_Upload is
 * a stub. */
#define OIT_PARAMS_UBO_BINDING	7

typedef struct {
	GLint	p[4];	/* x: MBOIT (1) or WBOIT (0) */
} oit_params_t;

COMPILE_TIME_ASSERT(oitparams_size, sizeof(oit_params_t) == 16);

static GLuint	oit_ubo;

/* ------------------------------------------------------------------ */
/* Bloom post-process shaders                                          */
/* ------------------------------------------------------------------ */

/* engine/shaders/sbloom_vert.glsl and sbloom_{bright,down,up}_frag.glsl
 * (#450), built as RPROG_BLOOM_*.  They carry no #version: PP_VERT_HEADER /
 * PP_FRAG_HEADER go in as a separate chunk, so one file serves both tiers. */

/* ------------------------------------------------------------------ */

/* Bloom is P4e of #340 under -gpu: its passes are bufferless draws
 * (glDrawArrays, no seam yet) and BloomParams has no mirror there, so -gpu
 * builds no pyramid and adds none.  Everywhere the chain asks "is bloom on",
 * it asks this. */
static qboolean PP_BloomOn (void)
{
#ifdef USE_SDL_GPU
	if (R_GPU ())
		return false;
#endif
	return r_bloom.integer != 0;
}

static qboolean PP_NeedsPostProcess (void)
{
	if (!pp_initialized)
		return false;
	if (v_gamma.value != 1.0f || PP_Contrast () != 1.0f)
		return true;
	if (r_scale.value < 1.0f)
		return true;
	if (r_softemu.value > 0)
		return true;
	if (gl_fxaa.value > 0)
		return true;
	if (r_waterwarp.value > 0)
		return true;
	if (Cvar_VariableValue("r_motionblur") > 0)
		return true;
	if (PP_HDRActive())
		return true;
	/* OIT's accum/revealage FBO is built as a sibling of the scene FBO
	 * in PP_CreateFBO and shares its depth/stencil attachment, so the
	 * postprocess pipeline must be up for any WBOIT to happen.  Without
	 * this, setting r_oit 1 at runtime left OIT_Active() false because
	 * oit_fbo was never created — and the user had to enable an
	 * unrelated effect (FXAA, HDR, …) to "wake" the pipeline. */
	if (r_oit.integer)
		return true;
	if (PP_BloomOn ())
		return true;
	/* SSAO snapshots and composites into the scene FBO, so like OIT it has
	 * to raise the chain itself: without this r_ssao 1 on a default config
	 * (gamma 1, nothing else on) drew nothing at all (#539).  What the chain
	 * brings -- the scene FBO at vid_fsaa samples (4 by default, so MSAA
	 * edges), its resolve and present blits, Hi-Z and soft particles off on
	 * the MSAA path -- then lands on r_ssao users alone.  Not on a driver
	 * whose FBO failed: the copyback fallback has no scene FBO, SSAO could
	 * not run, and the copy would be all cost. */
	if (!pp_fbo_failed && R_SSAO_WantsSceneFBO ())
		return true;
	return false;
}

/* ------------------------------------------------------------------ */
/* FBO management                                                      */
/* ------------------------------------------------------------------ */

static void PP_DeleteBloomFBOs (void);	/* forward decl to avoid implicit declaration */

/* r_hdr changes the FBO colour format and r_bloom decides whether the
 * pyramid exists at all, but the per-frame gate only rebuilds on a size
 * change -- so a runtime toggle used to do nothing until vid_restart.
 * The callbacks can fire mid-frame from the console or the menu, so they
 * only raise a flag; PP_BeginFrame does the reallocation at the same safe
 * point the size-change rebuild already uses.  uhexen2-3p6p. */
static qboolean	pp_rebuild_pending;

static void PP_FormatChanged (cvar_t *var)
{
	(void) var;
	pp_rebuild_pending = true;
}

static void PP_DeleteFBO (void)
{
	if (pp_color_tex)   { R_DeleteTextures (1, &pp_color_tex); pp_color_tex = 0; }
	if (pp_color_rb)    { R_DeleteRenderbuffers(1, &pp_color_rb); pp_color_rb = 0; }
	if (pp_depth_rb)    { R_DeleteRenderbuffers(1, &pp_depth_rb); pp_depth_rb = 0; }
	if (pp_depth_tex)   { R_DeleteTextures (1, &pp_depth_tex); pp_depth_tex = 0; }
	if (pp_resolve_fbo) { R_DeleteFramebuffers(1, &pp_resolve_fbo); pp_resolve_fbo = 0; }
	if (pp_fbo)         { R_DeleteFramebuffers(1, &pp_fbo); pp_fbo = 0; }
	PP_DeleteBloomFBOs();
	pp_width = pp_height = pp_samples = 0;
}

static void PP_DeleteNativeFBO (void)
{
	if (pp_native_color_tex) { R_DeleteTextures (1, &pp_native_color_tex); pp_native_color_tex = 0; }
	if (pp_native_depth_rb)  { R_DeleteRenderbuffers(1, &pp_native_depth_rb); pp_native_depth_rb = 0; }
	if (pp_native_fbo)       { R_DeleteFramebuffers(1, &pp_native_fbo); pp_native_fbo = 0; }
	pp_native_w = pp_native_h = 0;
	pp_native_fmt = 0;
}

static void PP_DeleteBloomFBOs (void)
{
	int i;
	for (i = 0; i < BLOOM_LEVELS; i++) {
		if (bloom_tex[i]) { R_DeleteTextures (1, &bloom_tex[i]); bloom_tex[i] = 0; }
		if (bloom_fbo[i]) { R_DeleteFramebuffers(1, &bloom_fbo[i]); bloom_fbo[i] = 0; }
		bloom_w[i] = bloom_h[i] = 0;
	}
}

static qboolean PP_CreateNativeFBO (int width, int height)
{
	GLenum status;

	/* Size alone is not enough: r_hdr selects the colour format, so a
	 * runtime toggle must reallocate even at an unchanged resolution.
	 * uhexen2-3p6p. */
	if (width == pp_native_w && height == pp_native_h && pp_native_fbo &&
	    pp_native_fmt == (GLenum)(PP_HDRActive() ? GL_RGBA16F : GL_RGBA8))
		return true;	/* already correct size and format */

	PP_DeleteNativeFBO();

	{
	GLenum native_fmt = PP_HDRActive() ? GL_RGBA16F : GL_RGBA8;
	GLenum native_type = PP_HDRActive() ? GL_FLOAT : GL_UNSIGNED_BYTE;
	R_GenTextures(1, &pp_native_color_tex);
	R_BindTextureForEdit (GL_TEXTURE_2D, pp_native_color_tex);
	R_TexImage2D(GL_TEXTURE_2D, pp_native_color_tex, 0, native_fmt, width, height,
			GL_RGBA, native_type, NULL);
	R_TexSetFilter(GL_TEXTURE_2D, pp_native_color_tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, pp_native_color_tex, GL_CLAMP_TO_EDGE);
	R_BindTextureForEdit (GL_TEXTURE_2D, 0);

	R_GenRenderbuffers(1, &pp_native_depth_rb);
	R_BindRenderbuffer(GL_RENDERBUFFER, pp_native_depth_rb);
	R_RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
	R_BindRenderbuffer(GL_RENDERBUFFER, 0);

	R_GenFramebuffers(1, &pp_native_fbo);
	R_BindFramebuffer(GL_FRAMEBUFFER, pp_native_fbo);
	R_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, pp_native_color_tex, 0);
	R_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
				  GL_RENDERBUFFER, pp_native_depth_rb);

	status = R_CheckFramebufferStatus(GL_FRAMEBUFFER);
	R_BindFramebuffer(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		PP_DeleteNativeFBO();
		return false;
	}

	pp_native_w = width;
	pp_native_h = height;
	pp_native_fmt = native_fmt;
	}  /* end native_fmt scope */
	return true;
}

/* Forward declaration — OIT FBO created after scene FBO */
static qboolean OIT_CreateFBO (int width, int height, GLuint depth_stencil_rb, GLuint depth_stencil_tex, int samples);
static void OIT_DeleteFBO (void);

/* Forward declaration — bloom pyramid FBOs */
static qboolean PP_CreateBloomFBOs (int width, int height);

static qboolean PP_CreateBloomFBOs (int width, int height)
{
	GLenum color_fmt, color_type, status;
	int i;

	/* Delete first so a runtime r_bloom 0 actually releases the pyramid
	 * rather than leaving it allocated but unused.  uhexen2-3p6p. */
	PP_DeleteBloomFBOs();

	if (!PP_BloomOn ())
		return true;	/* bloom disabled, skip allocation */

	color_fmt = PP_HDRActive() ? GL_RGBA16F : GL_RGBA8;
	color_type = PP_HDRActive() ? GL_FLOAT : GL_UNSIGNED_BYTE;

	for (i = 0; i < BLOOM_LEVELS; i++)
	{
		bloom_w[i] = (width >> (i + 1)) > 1 ? (width >> (i + 1)) : 1;		/* /2, /4, /8, /16 */
		bloom_h[i] = (height >> (i + 1)) > 1 ? (height >> (i + 1)) : 1;

		/* Bloom texture */
		R_GenTextures(1, &bloom_tex[i]);
		R_BindTextureForEdit (GL_TEXTURE_2D, bloom_tex[i]);
		R_TexImage2D(GL_TEXTURE_2D, bloom_tex[i], 0, color_fmt, bloom_w[i], bloom_h[i],
				GL_RGBA, color_type, NULL);
		R_TexSetFilter(GL_TEXTURE_2D, bloom_tex[i], GL_LINEAR, GL_LINEAR);
		R_TexSetWrap(GL_TEXTURE_2D, bloom_tex[i], GL_CLAMP_TO_EDGE);
		R_BindTextureForEdit (GL_TEXTURE_2D, 0);

		/* Bloom FBO */
		R_GenFramebuffers(1, &bloom_fbo[i]);
		R_BindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[i]);
		R_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				       GL_TEXTURE_2D, bloom_tex[i], 0);

		status = R_CheckFramebufferStatus(GL_FRAMEBUFFER);
		R_BindFramebuffer(GL_FRAMEBUFFER, 0);

		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			Con_DPrintf("Bloom: FBO %d incomplete (status 0x%x, %dx%d)\n",
				    i, status, bloom_w[i], bloom_h[i]);
			PP_DeleteBloomFBOs();
			return false;
		}
	}

	return true;
}

/* The MSAA sample count the scene FBO is built with: vid_config_fsaa after
 * the driver clamp and the -gpu gate.  Public because gl_lodbias auto scales
 * from it, and must read what the renderer uses, not what the player asked
 * for (GitHub #521).  The one runtime fallback it cannot see is an MSAA FBO
 * the driver reports incomplete, which falls back to 0 below. */
int PP_SceneSamples (void)
{
	int samples = (vid_config_fsaa.integer > 1) ? vid_config_fsaa.integer : 0;

	/* Clamp to what the driver can actually allocate.  config.cfg is a plain
	 * text file and survives GPU swaps, so an inherited 16 on an 8-sample
	 * driver is an ordinary case, not a corrupt one -- clamp it rather than
	 * letting the allocation fail into the fallback path on every restart.
	 * gl_max_samples is the engine's one GL_MAX_SAMPLES query (gl_vidsdl.c,
	 * populated post-context-init and already the bound the Options menu picks
	 * against); the cap cannot change within a context, so re-querying it here
	 * would only risk the two clamps drifting apart. */
	if (samples > gl_max_samples)
		samples = gl_max_samples;
#ifdef USE_SDL_GPU
	/* MSAA targets are P6 of #340.  gl_max_samples is 0 under -gpu today,
	 * which already says the same; this does not depend on it. */
	if (R_GPU ())
		samples = 0;
#endif
	if (samples < 2)
		samples = 0;
	return samples;
}

static qboolean PP_CreateFBO (int width, int height)
{
	GLenum status;
	/* OIT now follows Ironwail: when MSAA is active the accum/revealage
	 * targets are created as GL_TEXTURE_2D_MULTISAMPLE and resolved
	 * per-sample, so they share the multisampled depth/stencil cleanly. */
	int samples = PP_SceneSamples ();

	PP_DeleteFBO();

	/* resolve texture (always non-multisampled — this is what the shader reads) */
	{
		GLenum color_fmt = PP_HDRActive() ? GL_RGBA16F : GL_RGBA8;
		GLenum color_type = PP_HDRActive() ? GL_FLOAT : GL_UNSIGNED_BYTE;
	R_GenTextures(1, &pp_color_tex);
	R_BindTextureForEdit (GL_TEXTURE_2D, pp_color_tex);
	R_TexImage2D(GL_TEXTURE_2D, pp_color_tex, 0, color_fmt, width, height,
			GL_RGBA, color_type, NULL);
	R_TexSetFilter(GL_TEXTURE_2D, pp_color_tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, pp_color_tex, GL_CLAMP_TO_EDGE);
	R_BindTextureForEdit (GL_TEXTURE_2D, 0);

	if (samples > 1 && glRenderbufferStorageMultisample_fp && glBlitFramebuffer_fp)
	{
		/* multisampled render FBO */
		R_GenRenderbuffers(1, &pp_color_rb);
		R_BindRenderbuffer(GL_RENDERBUFFER, pp_color_rb);
		glRenderbufferStorageMultisample_fp(GL_RENDERBUFFER, samples, color_fmt, width, height);

		R_GenRenderbuffers(1, &pp_depth_rb);
		R_BindRenderbuffer(GL_RENDERBUFFER, pp_depth_rb);
		glRenderbufferStorageMultisample_fp(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
		R_BindRenderbuffer(GL_RENDERBUFFER, 0);

		R_GenFramebuffers(1, &pp_fbo);
		R_BindFramebuffer(GL_FRAMEBUFFER, pp_fbo);
		R_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					  GL_RENDERBUFFER, pp_color_rb);
		R_FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
					  GL_RENDERBUFFER, pp_depth_rb);

		status = R_CheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			Con_Printf("PostProcess: MSAA FBO incomplete (status 0x%x), falling back\n", status);
			/* clean up only MSAA resources, keep pp_color_tex for non-MSAA fallback */
			R_BindFramebuffer(GL_FRAMEBUFFER, 0);
			if (pp_fbo)       { R_DeleteFramebuffers(1, &pp_fbo); pp_fbo = 0; }
			if (pp_color_rb)  { R_DeleteRenderbuffers(1, &pp_color_rb); pp_color_rb = 0; }
			if (pp_depth_rb)  { R_DeleteRenderbuffers(1, &pp_depth_rb); pp_depth_rb = 0; }
			samples = 0;
			/* fall through to non-MSAA path below */
		}
		else
		{
			/* resolve FBO (non-multisampled texture) */
			R_GenFramebuffers(1, &pp_resolve_fbo);
			R_BindFramebuffer(GL_FRAMEBUFFER, pp_resolve_fbo);
			R_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					       GL_TEXTURE_2D, pp_color_tex, 0);

			status = R_CheckFramebufferStatus(GL_FRAMEBUFFER);
			R_BindFramebuffer(GL_FRAMEBUFFER, 0);

			if (status != GL_FRAMEBUFFER_COMPLETE)
			{
				Con_Printf("PostProcess: resolve FBO incomplete\n");
				PP_DeleteFBO();
				pp_fbo_failed = true;
				return false;
			}

			pp_samples = samples;
			pp_width = width;
			pp_height = height;
			Con_DPrintf("PostProcess: %dx%d FBO with %dx MSAA\n", width, height, samples);
			OIT_CreateFBO(width, height, pp_depth_rb, 0, samples);
			/* Bloom samples the resolved texture, so it applies under MSAA
			 * exactly as it does without.  Omitting this left r_bloom 1 a
			 * silent no-op whenever vid_config_fsaa >= 2.  uhexen2-3p6p. */
			PP_CreateBloomFBOs(width, height);
			return true;
		}
	}

	/* non-MSAA path — depth/stencil as a sampler-readable texture so the
	 * Hi-Z compute pass (uhexen2-xd87) can read it. */
	R_GenTextures(1, &pp_depth_tex);
	R_BindTextureForEdit (GL_TEXTURE_2D, pp_depth_tex);
	R_TexImage2D(GL_TEXTURE_2D, pp_depth_tex, 0, GL_DEPTH24_STENCIL8, width, height,
			GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
	R_TexSetFilter(GL_TEXTURE_2D, pp_depth_tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, pp_depth_tex, GL_CLAMP_TO_EDGE);
	/* GL_TEXTURE_COMPARE_MODE defaults to NONE so sampler2D returns the
	 * raw depth value rather than a comparison result. */
	R_BindTextureForEdit (GL_TEXTURE_2D, 0);

	R_GenFramebuffers(1, &pp_fbo);
	R_BindFramebuffer(GL_FRAMEBUFFER, pp_fbo);
	R_FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, pp_color_tex, 0);
	R_FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
			       GL_TEXTURE_2D, pp_depth_tex, 0);

	status = R_CheckFramebufferStatus(GL_FRAMEBUFFER);
	R_BindFramebuffer(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_Printf("PostProcess: FBO incomplete (status 0x%x, %dx%d, tex=%u, depth_tex=%u)\n",
			   status, width, height, pp_color_tex, pp_depth_tex);
		PP_DeleteFBO();
		pp_fbo_failed = true;
		return false;
	}

	pp_samples = 0;
	pp_width = width;
	pp_height = height;
	}  /* end color_fmt scope */
	OIT_CreateFBO(width, height, 0, pp_depth_tex, 0);
	PP_CreateBloomFBOs(width, height);
	return true;
}

/* ------------------------------------------------------------------ */
/* Shader                                                              */
/* ------------------------------------------------------------------ */

/* The post-process program is engine/shaders/spp_vert.glsl and spp_frag.glsl
 * (#450); PP_InitShader builds it as RPROG_POSTPROCESS. */

/* Pin a program's uniform block to its binding point, checking the driver's
 * layout against the C mirror's size (the RU_CheckBlockSize practice).  The
 * binding dies with the program, so vid_restart is covered.  Used by the
 * bloom (BloomParams) and OIT-resolve (OitParams) programs. */
static void PP_BindUniformBlock (GLuint prog, const char *block, GLuint binding, size_t mirror)
{
	GLuint	blk;
	GLint	size = 0;

#ifdef USE_SDL_GPU
	/* The program's reflection fixes each block's slot under -gpu. */
	if (R_GPU ())
		return;
#endif
	if (!prog || !glGetUniformBlockIndex_fp || !glUniformBlockBinding_fp)
		return;
	blk = glGetUniformBlockIndex_fp(prog, block);
	if (blk == GL_INVALID_INDEX)
		return;
	if (glGetActiveUniformBlockiv_fp)
	{
		glGetActiveUniformBlockiv_fp(prog, blk, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
		if (size != (GLint)mirror)
			Con_Printf("WARNING: uniform block %s is %d bytes on this driver, "
				   "C mirror is %d -- its uniforms will be wrong\n",
				   block, (int)size, (int)mirror);
	}
	glUniformBlockBinding_fp(prog, blk, binding);
}

/* Write one bloom pass's parameters into bloom_ubo and bind it.
 *
 * NOT the GL_Upload frame ring that gl_worldcull.c and gl_lightcluster.c use:
 * on the ES tier GL_Upload is a stub that hands back buffer 0 (gl_buffer.c,
 * the USE_GLES arm), and bloom runs on both tiers.  Through the ring, the ES
 * build read BloomParams as zeros -- threshold 0 and a zero step -- and
 * bloomed every pixel, while the desktop build stayed pixel-identical.  So
 * the bloom passes own one 16-byte buffer, the way gl_ssao.c owns ss_ubo.  A
 * glBufferSubData between draws is ordered against the earlier draws by GL
 * itself, so seven of them a frame are correct on every driver. */
static GLuint	bloom_ubo;

static void PP_PushBloomParams (float rcp_w, float rcp_h, float threshold)
{
	bloom_params_t	bp;

	bp.p[0] = rcp_w;
	bp.p[1] = rcp_h;
	bp.p[2] = threshold;
	bp.p[3] = 0.0f;
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_NoteUnhandled ("PP_PushBloomParams (BloomParams is P4e)");
		return;
	}
#endif
	GL_BindBuffer(GL_UNIFORM_BUFFER, bloom_ubo);
	glBufferSubData_fp(GL_UNIFORM_BUFFER, 0, sizeof(bp), &bp);
	GL_BindBuffer(GL_UNIFORM_BUFFER, 0);
	GL_BindBufferBase(GL_UNIFORM_BUFFER, BLOOM_PARAMS_UBO_BINDING, bloom_ubo);
}

/* Upload the post-process shadow (u_mvp from the current matrices) into its
 * two blocks and bind them.  Called right before each pp_program draw. */
static void PP_PushParams (void)
{
	GL_GetMVP(pp_vert_mvp);
#ifdef USE_SDL_GPU
	/* The mirrors PP_InitShader registered are the blocks under -gpu. */
	if (R_GPU ())
	{
		RG_UniformBlockFlushed ("PpVert");
		RG_UniformBlockFlushed ("PpParams");
		return;
	}
#endif
	GL_BindBuffer(GL_UNIFORM_BUFFER, pp_vert_ubo);
	glBufferSubData_fp(GL_UNIFORM_BUFFER, 0, sizeof(pp_vert_mvp), pp_vert_mvp);
	GL_BindBuffer(GL_UNIFORM_BUFFER, pp_frag_ubo);
	glBufferSubData_fp(GL_UNIFORM_BUFFER, 0, sizeof(pp_params), &pp_params);
	GL_BindBuffer(GL_UNIFORM_BUFFER, 0);
	GL_BindBufferBase(GL_UNIFORM_BUFFER, PP_VERT_UBO_BINDING, pp_vert_ubo);
	GL_BindBufferBase(GL_UNIFORM_BUFFER, PP_FRAG_UBO_BINDING, pp_frag_ubo);
}

static qboolean PP_InitShader (void)
{
	pp_program = R_Program_Create(RPROG_POSTPROCESS);
	if (!pp_program) return false;

	pp_loc_scene = R_ProgramSamplerLoc(pp_program, "scene");
	pp_loc_paletteLUT = R_ProgramSamplerLoc(pp_program, "paletteLUT");
	PP_BindUniformBlock(pp_program, "PpVert", PP_VERT_UBO_BINDING, sizeof(pp_vert_mvp));
	PP_BindUniformBlock(pp_program, "PpParams", PP_FRAG_UBO_BINDING, sizeof(pp_params_t));
	/* A fresh program starts with every loose uniform at zero; so does the
	 * shadow that replaced them. */
	memset(&pp_params, 0, sizeof(pp_params));
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_UniformBlockRegister ("PpVert", pp_vert_mvp, sizeof(pp_vert_mvp));
		RG_UniformBlockRegister ("PpParams", &pp_params, sizeof(pp_params));
	}
	else
#endif
	if (!pp_vert_ubo)
	{
		glGenBuffers_fp(1, &pp_vert_ubo);
		GL_BindBuffer(GL_UNIFORM_BUFFER, pp_vert_ubo);
		glBufferData_fp(GL_UNIFORM_BUFFER, sizeof(pp_vert_mvp), NULL, GL_DYNAMIC_DRAW);
		glGenBuffers_fp(1, &pp_frag_ubo);
		GL_BindBuffer(GL_UNIFORM_BUFFER, pp_frag_ubo);
		glBufferData_fp(GL_UNIFORM_BUFFER, sizeof(pp_params_t), NULL, GL_DYNAMIC_DRAW);
		GL_BindBuffer(GL_UNIFORM_BUFFER, 0);
	}

	/* bind samplers once */
	R_UseProgram (pp_program);
	R_ProgramSamplerUnit(pp_program, pp_loc_scene, "scene", 0);
	R_ProgramSamplerUnit(pp_program, pp_loc_paletteLUT, "paletteLUT", 1);
	pp_loc_bloom_tex = R_ProgramSamplerLoc(pp_program, "u_bloom");
	R_ProgramSamplerUnit(pp_program, pp_loc_bloom_tex, "u_bloom", 2);
	R_UseProgram (0);

	/* Bloom shaders; bloom is optional, so a failure still returns true */
	bloom_bright_prog = R_Program_Create(RPROG_BLOOM_BRIGHT);
	if (!bloom_bright_prog) return true;

	bloom_bright_loc_scene = R_ProgramSamplerLoc(bloom_bright_prog, "u_scene");
	PP_BindUniformBlock(bloom_bright_prog, "BloomParams", BLOOM_PARAMS_UBO_BINDING, sizeof(bloom_params_t));

	bloom_down_prog = R_Program_Create(RPROG_BLOOM_DOWN);
	if (!bloom_down_prog) return true;

	bloom_down_loc_scene = R_ProgramSamplerLoc(bloom_down_prog, "u_scene");
	PP_BindUniformBlock(bloom_down_prog, "BloomParams", BLOOM_PARAMS_UBO_BINDING, sizeof(bloom_params_t));

	bloom_up_prog = R_Program_Create(RPROG_BLOOM_UP);
	if (!bloom_up_prog) return true;

	bloom_up_loc_scene = R_ProgramSamplerLoc(bloom_up_prog, "u_scene");
	PP_BindUniformBlock(bloom_up_prog, "BloomParams", BLOOM_PARAMS_UBO_BINDING, sizeof(bloom_params_t));

	/* GL 4.3 core profile requires a VAO bound for any draw call. */
	R_GenVertexArrays(1, &bloom_dummy_vao);

	/* BloomParams storage; same lifetime as the programs and VAO above.
	 * Under -gpu it becomes a mirror at P4e: no buffer until then. */
#ifdef USE_SDL_GPU
	if (!R_GPU ())
#endif
	{
		glGenBuffers_fp(1, &bloom_ubo);
		GL_BindBuffer(GL_UNIFORM_BUFFER, bloom_ubo);
		glBufferData_fp(GL_UNIFORM_BUFFER, sizeof(bloom_params_t), NULL, GL_DYNAMIC_DRAW);
		GL_BindBuffer(GL_UNIFORM_BUFFER, 0);
	}

	return true;
}

/* ------------------------------------------------------------------ */
/* Palette LUT                                                         */
/* ------------------------------------------------------------------ */

extern unsigned int d_8to24table[256];

/* ITU-R BT.601 luma, fixed point: (r*77 + g*151 + b*28) >> 8 spans 0..255. */
#define PP_LUMA(r,g,b)	((((r) * 77) + ((g) * 151) + ((b) * 28)) >> 8)

/* The grid coordinate -> 8-bit channel value the LUT is built against.
 *
 * Integer, and shared verbatim with the compute builder, because the two have
 * to agree exactly: a float (gid * 255/31) rounds 31 to 254.99998 on some
 * drivers and the top corner of the cube would then disagree between tiers. */
#define PP_GRID_TO_8BIT(i)	(((i) * 255) / (PP_LUT_DIM - 1))

/* Creates the empty 3D texture both LUT routes fill: CPU builders through
 * PP_UploadLUT, the compute builder by imageStore.  An edit bind, so on unit
 * 0's 3D target, and unbound again; the post-process pass binds it on unit 1
 * when it samples it. */
static GLuint PP_CreateLUTTexture (int dim)
{
	GLuint tex = 0;

	R_GenTextures(1, &tex);
	R_BindTextureForEdit (GL_TEXTURE_3D, tex);
	R_TexImage3D(GL_TEXTURE_3D, tex, 0, GL_RGBA8,
			dim, dim, dim,
			GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	R_TexSetFilter(GL_TEXTURE_3D, tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_3D, tex, GL_CLAMP_TO_EDGE);
	R_BindTextureForEdit (GL_TEXTURE_3D, 0);

	return tex;
}

/* Takes a LUT_DIM^3 grid of palette *indices* -- which is what both CPU
 * builders below compute -- and uploads it as resolved RGBA8 colour into
 * *tex, creating the texture on first use.
 *
 * The shader used to fetch the index and look it up in a `uniform vec3
 * palette[256]`.  That array alone is 256 uniform vectors, over the 224 a
 * WebGL2 implementation is only required to offer a fragment shader, so the
 * entire post-process program could fail to link on a conforming browser or
 * mobile GL ES driver -- taking gamma, contrast, FXAA and render scale down
 * with softemu.  Resolving the colour here costs 128 KB of texture per LUT
 * instead, and drops a per-frame 768-float uniform upload on every tier.
 *
 * RGBA rather than RGB purely so the compute builder can write the same
 * texture: rgba8 is one of the image formats GL 4.3 requires, rgb8 is not.
 * The alpha byte is never read -- the post-process shader takes .rgb.
 *
 * Safe to bake because d_8to24table is built once in VID_InitPalette and never
 * mutated afterwards; palette flashes go through v_blend, not this table.
 * Ported from alextnewman/hexenwail d2c46f078. */
static qboolean PP_UploadLUT (GLuint *tex, const unsigned char *lut, int dim)
{
	/* static: 8 MB at 128^3, far too much for the stack */
	static unsigned char rgba[PP_LUT_TEXELS * 4];
	int i, texels = dim * dim * dim;

	if (!*tex)
	{
		*tex = PP_CreateLUTTexture(dim);
		if (!*tex)
			return false;
	}

	for (i = 0; i < texels; i++)
	{
		unsigned int c = d_8to24table[lut[i]];
		rgba[i * 4 + 0] = (c >>  0) & 0xff;
		rgba[i * 4 + 1] = (c >>  8) & 0xff;
		rgba[i * 4 + 2] = (c >> 16) & 0xff;
		rgba[i * 4 + 3] = 255;
	}

	R_BindTextureForEdit (GL_TEXTURE_3D, *tex);
	R_TexImage3D(GL_TEXTURE_3D, *tex, 0, GL_RGBA8,
			dim, dim, dim,
			GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	R_BindTextureForEdit (GL_TEXTURE_3D, 0);

	return true;
}

/* ------------------------------------------------------------------ */
/* Colour metrics (uhexen2-h8yy)                                       */
/*                                                                     */
/* Transliterated from Ironwail's palette_init compute shader          */
/* (Quake/gl_shaders.h, NormalizeColor / ColorDistanceSquared), which  */
/* the GPU builder further down compiles very nearly verbatim.  Two    */
/* implementations of one function is a drift hazard, so they are kept */
/* adjacent, in the same order, with the same constants, and the       */
/* compute source quotes the C by structure rather than paraphrasing   */
/* it.  The CPU copy is the authoritative one: it is what the ES /     */
/* WebGL2 tier runs, and what the GPU path falls back to.              */
/*                                                                     */
/* Checked against the retail palette rather than assumed.  Under the  */
/* naive metric this reproduces the exact integer nearest-colour table */
/* over all 32768 grid points of the then 32^3 grid -- zero worse      */
/* choices.  (Now 128^3: GitHub #80.)                                  */
/*                                                                     */
/* The two implementations were then diffed against each other on a    */
/* real driver (GL 4.3, NVIDIA 580): Riemersma byte-identical over the */
/* whole grid, OKLab one texel apart, naive 260 texels apart.  Every   */
/* one of those is a TIE -- two palette entries at identical distance, */
/* with `dist < bestdist` keeping whichever it reached first, and      */
/* float rounding deciding which that is.  Zero cells where one side   */
/* picked a genuinely worse colour, and zero cells the compute pass    */
/* left unwritten.  Ties are the only thing the two may split on, and  */
/* they are common under the naive metric precisely because plain RGB  */
/* distance is the one with no perceptual tie-breaking in it.          */
/*                                                                     */
/* Measured mean CIELAB dE over the grid -- a yardstick none of the    */
/* three optimises for -- is naive 26.4, Riemersma 25.3, OKLab 22.6,   */
/* worst case 126.1 / 96.0 / 72.9.  That is why -1 resolves to OKLab.  */
/* ------------------------------------------------------------------ */

static float PP_SRGBToLinear (float v)
{
	return (v > 0.04045f) ? powf((v + 0.055f) / 1.055f, 2.4f) : (v / 12.92f);
}

/* One 8-bit channel through the per-channel part of PP_NormalizeColor:
 * scale, and to linear for every metric but naive.  Split out so the LUT
 * builder can do it once per grid coordinate (GitHub #80); the whole-colour
 * function below calls exactly this, so the two cannot drift. */
static float PP_NormalizeChannel (softemu_metric_t metric, int v8)
{
	float c = (float)v8 * (1.0f / 255.0f);

	if (metric >= SOFTEMU_METRIC_RIEMERSMA)
		c = PP_SRGBToLinear(c);
	return c;
}

/* The channel-mixing part of PP_NormalizeColor: OKLab from linear RGB, in
 * place.  Only OKLab mixes channels. */
static void PP_MixChannels (softemu_metric_t metric, float c[3])
{
	if (metric >= SOFTEMU_METRIC_OKLAB)
	{
		{
			/* A perceptual color space for image processing -- Bjorn
			 * Ottosson.  https://bottosson.github.io/posts/oklab/ */
			float l = 0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2];
			float m = 0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2];
			float s = 0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2];

			/* powf, not cbrtf: the shader has no cbrt and uses pow, and
			 * the two paths matching matters more here than the last ulp.
			 * Linear RGB is non-negative, so pow is well defined. */
			float l_ = powf(l, 1.0f / 3.0f);
			float m_ = powf(m, 1.0f / 3.0f);
			float s_ = powf(s, 1.0f / 3.0f);

			c[0] = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
			c[1] = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
			c[2] = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
		}
	}
}

/* 8-bit sRGB triple -> the space `metric` measures distance in. */
static void PP_NormalizeColor (softemu_metric_t metric, int r8, int g8, int b8, float out[3])
{
	out[0] = PP_NormalizeChannel(metric, r8);
	out[1] = PP_NormalizeChannel(metric, g8);
	out[2] = PP_NormalizeChannel(metric, b8);
	PP_MixChannels(metric, out);
}

static float PP_ColorDistanceSquared (softemu_metric_t metric, const float c0[3], const float c1[3])
{
	float d0 = c1[0] - c0[0];
	float d1 = c1[1] - c0[1];
	float d2 = c1[2] - c0[2];

	if (metric == SOFTEMU_METRIC_RIEMERSMA)
	{
		/* Colour metric -- Thiadmer Riemersma.
		 * https://www.compuphase.com/cmetric.htm */
		float rmean = (c0[0] + c1[0]) * 0.5f;
		return d0 * d0 * (2.0f + rmean) + d1 * d1 * 4.0f + d2 * d2 * (3.0f - rmean);
	}
	if (metric == SOFTEMU_METRIC_OKLAB)
	{
		d0 *= 1.25f;	/* lightness */
		d2 *= 1.5f;	/* blue-yellow */
	}
	return d0 * d0 + d1 * d1 + d2 * d2;
}

static const char *PP_MetricName (softemu_metric_t metric)
{
	switch (metric)
	{
	case SOFTEMU_METRIC_NAIVE:	return "naive RGB";
	case SOFTEMU_METRIC_RIEMERSMA:	return "Riemersma";
	case SOFTEMU_METRIC_OKLAB:	return "OKLab";
	default:			return "?";
	}
}

/*
===============
PP_SelectMetric

Which metric the nearest-colour LUT should be built with right now.

r_softemu_metric >= 0 names one outright.  The shipped -1 defers, and Ironwail
(gl_texmgr.c:2030-2042) then picks naive RGB in exactly one situation: its
BANDED mode on a connected map with no .lit file, where reproducing the 1997
image is the whole point and a perceptual metric would improve it away from
that.  Everywhere else it takes OKLab, which simply matches better.

Translating the condition rather than the code: our BANDED-equivalent is
r_softemu 3 (see R_SoftEmuParams), and mode 3 does not normally consult a
distance metric at all -- it reads the colormap LUT, which is authored.  The
only way mode 3 reaches this table is a game shipping no gfx/colormap.lmp, and
that fallback is precisely upstream's case, so it is the one that gets naive.

The .lit half of upstream's test has no counterpart here: coloured lighting is
not what the metric is choosing between, and we carry no per-model litfile
flag to test anyway.
===============
*/
static softemu_metric_t PP_SelectMetric (void)
{
	int m;

	if (r_softemu_metric.value < 0.0f)
	{
		if (r_softemu.integer == 3 && !pp_colormap_lut)
			return SOFTEMU_METRIC_NAIVE;
		return SOFTEMU_METRIC_OKLAB;
	}

	m = (int)r_softemu_metric.value;
	if (m < 0)
		m = 0;
	if (m >= SOFTEMU_METRIC_COUNT)
		m = SOFTEMU_METRIC_COUNT - 1;
	return (softemu_metric_t)m;
}

/* The cvar is live -- PP_UpdatePaletteLUT notices the change on the next frame
 * and rebuilds -- but at r_softemu 3 with a colormap present the rebuilt table
 * is not the one that gets bound, so the screen does not change and the
 * setting looks broken.  Say why instead. */
static void PP_MetricChanged (cvar_t *var)
{
	(void) var;
	if (r_softemu.integer == 3 && pp_colormap_lut)
		Con_Printf("r_softemu_metric has no effect at r_softemu 3: that mode resolves\n"
			   "through gfx/colormap.lmp, which is authored, not searched.\n");
}

/* r_softemu 3 -- quantize the way the 1997 rasterizer did.
 *
 * The software renderer never chose a colour by RGB distance.  Every lit pixel
 * was colormap[light * 256 + texel]: an authored table whose darkening ramps
 * take a deliberately non-linear path through the palette, with hue shifts at
 * the dark end that a Euclidean nearest-colour search cannot reproduce.
 *
 * The post-process only ever sees the product of texel and light, so splitting
 * one back into the other is an inference.  The table's own calibration makes
 * the split: row ~30 is the identity row (colormap[30*256 + i] == i for most
 * i), so a fully lit texel sits mid-ramp and the rows above it are Raven's
 * overbright headroom.  Luminance is therefore the light axis -- for a
 * candidate texel, the light level is the row of that texel's ramp whose
 * luminance lands nearest the incoming pixel's.  Pinning the light level that
 * way leaves exactly one reachable colour per texel, and the texel is then
 * whichever of those 255 candidates is closest in RGB.
 *
 * So brightness picks the light level and chroma picks the texel: the
 * rasterizer's own split, run backwards.  The result is whatever Raven's table
 * says lives at that pair, which is why it is not the nearest palette entry --
 * 27.8% of the 32^3 grid lands elsewhere, and the disagreement concentrates in
 * the dark ranges (56% of dark grid points move, mean delta 40/255, against
 * 25% and 11/255 in the midtones) where the authored ramps bend.
 *
 * Ramp luminance is not quite monotonic in light level (90 of the 256 ramps
 * wobble), so the level search has to be exhaustive rather than a bisection.
 */
static qboolean PP_BuildColormapLUT (void)
{
	/* static: ~114 KB of tables, too much for the stack on Windows */
	static unsigned char lut[PP_CMAP_TEXELS];
	static unsigned char rampluma[VID_GRADES][256];	/* [light][texel] -> luma */
	static unsigned char bestlevel[256][256];	/* [texel][target luma] -> light */
	const byte *cmap = host_colormap;
	int r, g, b, i, l, y;

	if (!glTexImage3D_fp || !cmap)
		return false;

	for (l = 0; l < VID_GRADES; l++)
	{
		for (i = 0; i < 256; i++)
		{
			unsigned int c = d_8to24table[cmap[l * 256 + i]];
			rampluma[l][i] = (unsigned char)
				PP_LUMA((int)((c >> 0) & 0xff),
					(int)((c >> 8) & 0xff),
					(int)((c >> 16) & 0xff));
		}
	}

	for (i = 0; i < 256; i++)
	{
		for (y = 0; y < 256; y++)
		{
			int best = 0;
			int bestdist = 0x7fffffff;

			for (l = 0; l < VID_GRADES; l++)
			{
				int d = (int)rampluma[l][i] - y;
				if (d < 0)
					d = -d;
				if (d < bestdist)
				{
					bestdist = d;
					best = l;
				}
			}
			bestlevel[i][y] = (unsigned char)best;
		}
	}

	for (b = 0; b < PP_CMAP_DIM; b++)
	{
		for (g = 0; g < PP_CMAP_DIM; g++)
		{
			for (r = 0; r < PP_CMAP_DIM; r++)
			{
				int tr = (r * 255) / (PP_CMAP_DIM - 1);
				int tg = (g * 255) / (PP_CMAP_DIM - 1);
				int tb = (b * 255) / (PP_CMAP_DIM - 1);
				int ty = PP_LUMA(tr, tg, tb);
				int best = 0;
				int bestdist = 0x7fffffff;

				/* skip texel 255 (transparent / fullbright), as the
				 * nearest-colour LUT does.  No other texel's ramp
				 * resolves to 255 in the retail colormap, so mode 3
				 * never emits it either. */
				for (i = 0; i < 255; i++)
				{
					int entry = cmap[bestlevel[i][ty] * 256 + i];
					unsigned int pal = d_8to24table[entry];
					int dr = tr - (int)((pal >>  0) & 0xff);
					int dg = tg - (int)((pal >>  8) & 0xff);
					int db = tb - (int)((pal >> 16) & 0xff);
					int dist = dr*dr + dg*dg + db*db;
					if (dist < bestdist)
					{
						bestdist = dist;
						best = entry;
					}
				}
				lut[(b * PP_CMAP_DIM + g) * PP_CMAP_DIM + r] = (unsigned char)best;
			}
		}
	}

	return PP_UploadLUT(&pp_colormap_lut, lut, PP_CMAP_DIM);
}

/* Per-axis bounds on PP_ColorDistanceSquared, as weights on the squared
 * per-channel differences: the distance always lies between
 * sum(lo[k] * d[k]^2) and sum(hi[k] * d[k]^2).  Exact (lo == hi) for the two
 * Euclidean metrics -- naive, and OKLab with its 1.25 / 1.5 axis scales --
 * and for Riemersma from rmean in [0,1] (linear RGB): its red and blue
 * weights move between 2 and 3, green is 4.  GitHub #80. */
static void PP_MetricWeights (softemu_metric_t metric, float lo[3], float hi[3])
{
	switch (metric)
	{
	case SOFTEMU_METRIC_RIEMERSMA:
		lo[0] = 2.0f; lo[1] = 4.0f; lo[2] = 2.0f;
		hi[0] = 3.0f; hi[1] = 4.0f; hi[2] = 3.0f;
		break;
	case SOFTEMU_METRIC_OKLAB:
		lo[0] = hi[0] = 1.5625f; lo[1] = hi[1] = 1.0f; lo[2] = hi[2] = 2.25f;
		break;
	default:
		lo[0] = hi[0] = lo[1] = hi[1] = lo[2] = hi[2] = 1.0f;
		break;
	}
}

#define PP_LUT_BLOCK	8	/* grid cells per block edge; divides PP_LUT_DIM */

/*
===============
PP_FillPaletteGrid

The nearest-colour grid: for every PP_LUT_DIM^3 cell, the palette index a
scan of all `count' entries keeping the first strict minimum would pick --
ties included -- but without scanning them all.  The grid is cut into
PP_LUT_BLOCK^3 blocks; for each, the box its targets span in metric space
gives every entry a lower and an upper bound on its distance to any target
inside.  The least upper bound caps every target's nearest distance, so only
entries whose lower bound does not exceed it can be nearest -- or tie for
nearest -- anywhere in the block.  Those are then scanned per cell in index
order with the same strict comparison as the full scan, so the result is the
same table.  The cap is widened by a relative margin far above float
rounding, which can only admit extra candidates, never drop one.

Shared verbatim with tools/test_softemu_lut.py, which checks it cell for cell
against the full scan.  GitHub #80.
===============
*/
static void PP_FillPaletteGrid (softemu_metric_t metric, float palcolors[][3], int count,
				unsigned char *lut)
{
	static float	targets[PP_LUT_BLOCK][PP_LUT_BLOCK][PP_LUT_BLOCK][3];
	static float	axis[PP_LUT_DIM];
	static int	cand[256];
	float		wlo[3], whi[3];
	int		br, bg, bb, r, g, b, i, k, ncand;

	PP_MetricWeights(metric, wlo, whi);

	/* The per-channel part of the transform, once per grid coordinate: the
	 * same calls PP_NormalizeColor makes, so the targets are bit-identical
	 * to whole-colour calls.  Only OKLab's channel mixing is left per cell. */
	for (i = 0; i < PP_LUT_DIM; i++)
		axis[i] = PP_NormalizeChannel(metric, PP_GRID_TO_8BIT(i));

	for (bb = 0; bb < PP_LUT_DIM; bb += PP_LUT_BLOCK)
	for (bg = 0; bg < PP_LUT_DIM; bg += PP_LUT_BLOCK)
	for (br = 0; br < PP_LUT_DIM; br += PP_LUT_BLOCK)
	{
		float	boxlo[3] = { 1e30f, 1e30f, 1e30f };
		float	boxhi[3] = { -1e30f, -1e30f, -1e30f };
		float	cap = 1e30f;

		for (b = 0; b < PP_LUT_BLOCK; b++)
		for (g = 0; g < PP_LUT_BLOCK; g++)
		for (r = 0; r < PP_LUT_BLOCK; r++)
		{
			float	*t = targets[b][g][r];
			t[0] = axis[br + r];
			t[1] = axis[bg + g];
			t[2] = axis[bb + b];
			PP_MixChannels(metric, t);
			for (k = 0; k < 3; k++)
			{
				if (t[k] < boxlo[k]) boxlo[k] = t[k];
				if (t[k] > boxhi[k]) boxhi[k] = t[k];
			}
		}

		/* least upper bound over entries: no target in the box is farther
		 * than this from its nearest entry */
		for (i = 0; i < count; i++)
		{
			float	upper = 0.0f;
			for (k = 0; k < 3; k++)
			{
				float	d0 = palcolors[i][k] - boxlo[k];
				float	d1 = boxhi[k] - palcolors[i][k];
				float	d = (d0 > d1) ? d0 : d1;
				upper += whi[k] * d * d;
			}
			if (upper < cap)
				cap = upper;
		}
		cap = cap * 1.001f + 1e-9f;

		ncand = 0;
		for (i = 0; i < count; i++)
		{
			float	lower = 0.0f;
			for (k = 0; k < 3; k++)
			{
				float	d = 0.0f;
				if (palcolors[i][k] < boxlo[k])
					d = boxlo[k] - palcolors[i][k];
				else if (palcolors[i][k] > boxhi[k])
					d = palcolors[i][k] - boxhi[k];
				lower += wlo[k] * d * d;
			}
			if (lower <= cap)
				cand[ncand++] = i;
		}

		for (b = 0; b < PP_LUT_BLOCK; b++)
		for (g = 0; g < PP_LUT_BLOCK; g++)
		for (r = 0; r < PP_LUT_BLOCK; r++)
		{
			int	best = 0;
			float	bestdist = 1e+30f;
			for (k = 0; k < ncand; k++)
			{
				float	dist = PP_ColorDistanceSquared(metric, targets[b][g][r],
								       palcolors[cand[k]]);
				if (dist < bestdist)
				{
					bestdist = dist;
					best = cand[k];
				}
			}
			lut[((bb + b) * PP_LUT_DIM + (bg + g)) * PP_LUT_DIM + (br + r)] =
				(unsigned char)best;
		}
	}
}

/*
===============
PP_BuildPaletteLUT_CPU

The nearest-colour table, built under `metric`.  Runs on every tier; on the ES
/ WebGL2 one it is the only route, since that tier has no compute shaders.
128^3 (Ironwail's grid) by a full scan is 537M distance evaluations, too slow
at load; PP_FillPaletteGrid returns the same table from a few candidates per
cell.  GitHub #80.
===============
*/
static qboolean PP_BuildPaletteLUT_CPU (softemu_metric_t metric)
{
	/* static: 2 MB of indices, and this is called from the frame path when
	 * the metric changes */
	static unsigned char	lut[PP_LUT_TEXELS];
	static float		palcolors[256][3];
	int	i;

	if (!glTexImage3D_fp)
		return false;

	/* transform the palette once, not once per grid point */
	for (i = 0; i < 255; i++)
	{
		unsigned int pal = d_8to24table[i];
		PP_NormalizeColor(metric,
				  (int)((pal >>  0) & 0xff),
				  (int)((pal >>  8) & 0xff),
				  (int)((pal >> 16) & 0xff),
				  palcolors[i]);
	}

	/* 255, not 256: index 255 is skipped (transparent / fullbright).
	 * Upstream searches all 256 -- Quake's last entry is an ordinary
	 * colour, ours is the transparency key, and emitting it would paint
	 * holes. */
	PP_FillPaletteGrid(metric, palcolors, 255, lut);

	return PP_UploadLUT(&pp_palette_lut, lut, PP_LUT_DIM);
}

/* ------------------------------------------------------------------ */
/* GPU palette LUT (uhexen2-h8yy)                                      */
/*                                                                     */
/* Ironwail generates this table with a compute pass and three         */
/* compile-time metric variants (gl_shaders.c:379).  Desktop GL 4.3    */
/* only: the ES / WebGL2 tier has no compute shaders at all, so it     */
/* keeps PP_BuildPaletteLUT_CPU, and this is an accelerator rather     */
/* than the sole route.  Both must agree, which is why the maths below */
/* is line-for-line the C above it -- same order, same constants, same */
/* index-255 skip, same integer grid mapping.                          */
/* ------------------------------------------------------------------ */

static qboolean	pp_lut_gpu_built;	/* whether the live table came from the GPU */

#ifndef USE_GLES

static GLuint	pp_palette_init_prog[SOFTEMU_METRIC_COUNT];
static GLuint	pp_palette_tex;		/* 256x1 RGBA8, index -> colour */
static qboolean	pp_lut_gpu_failed;	/* compile failed once; do not keep trying */

/* The pass is engine/shaders/softemu_lut_comp.glsl (#450); its header comment
 * carries the notes on how it departs from Ironwail's palette_init.  One
 * program per metric, each its own id: the row's header chunk supplies
 * #version, MODE (the metric) and LUT_DIM, which replaced the loose
 * `uniform int u_dim` -- it was always PP_LUT_DIM. */
static const rprog_id_t pp_palette_init_id[SOFTEMU_METRIC_COUNT] = {
	[SOFTEMU_METRIC_NAIVE]     = RPROG_SOFTEMU_LUT_MODE0,
	[SOFTEMU_METRIC_RIEMERSMA] = RPROG_SOFTEMU_LUT_MODE1,
	[SOFTEMU_METRIC_OKLAB]     = RPROG_SOFTEMU_LUT_MODE2
};

/* d_8to24table as a 256x1 texture, so the compute pass can read it without an
 * SSBO.  Built once; the table is fixed after VID_InitPalette. */
static GLuint PP_CreatePaletteTexture (void)
{
	unsigned char	rgba[256 * 4];
	GLuint		tex = 0;
	int		i;

	for (i = 0; i < 256; i++)
	{
		unsigned int c = d_8to24table[i];
		rgba[i * 4 + 0] = (c >>  0) & 0xff;
		rgba[i * 4 + 1] = (c >>  8) & 0xff;
		rgba[i * 4 + 2] = (c >> 16) & 0xff;
		rgba[i * 4 + 3] = 255;
	}

	R_GenTextures(1, &tex);
	R_BindTextureForEdit (GL_TEXTURE_2D, tex);
	R_TexImage2D(GL_TEXTURE_2D, tex, 0, GL_RGBA8, 256, 1,
			GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	R_TexSetFilter(GL_TEXTURE_2D, tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, tex, GL_CLAMP_TO_EDGE);
	R_BindTextureForEdit (GL_TEXTURE_2D, 0);

	return tex;
}

static qboolean PP_BuildPaletteLUT_GPU (softemu_metric_t metric)
{
	pp_lut_gpu_built = false;

	if (pp_lut_gpu_failed || !glTexImage3D_fp)
		return false;
	if (!gl_renderer_caps.compute_shaders || !glBindImageTexture_fp)
		return false;

	if (!pp_palette_init_prog[metric])
	{
		pp_palette_init_prog[metric] = R_Program_Create(pp_palette_init_id[metric]);
		if (!pp_palette_init_prog[metric])
		{
			/* One failure means the driver rejects the pass, not this
			 * variant of it -- do not recompile it every frame. */
			pp_lut_gpu_failed = true;
			return false;
		}
	}

	if (!pp_palette_tex)
	{
		pp_palette_tex = PP_CreatePaletteTexture();
		if (!pp_palette_tex)
			return false;
	}

	if (!pp_palette_lut)
	{
		pp_palette_lut = PP_CreateLUTTexture(PP_LUT_DIM);
		if (!pp_palette_lut)
			return false;
	}

	R_UseProgram (pp_palette_init_prog[metric]);
	R_BindTextureSlot (0, pp_palette_tex);
	/* u_palette is pinned to unit 0 in the shader (TEXBIND(0)); LUT_DIM is
	 * compiled in.  Nothing to upload. */

	R_BindImageTexture (0, pp_palette_lut, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
	glDispatchCompute_fp(PP_LUT_DIM / 8, PP_LUT_DIM / 8, PP_LUT_DIM / 4);
	glMemoryBarrier_fp(GL_TEXTURE_FETCH_BARRIER_BIT);

	R_BindImageTexture (0, 0, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA8);
	R_BindTextureSlot (0, 0);
	R_UseProgram (0);

	pp_lut_gpu_built = true;
	return true;
}

static void PP_FreePaletteGPU (void)
{
	int i;

	for (i = 0; i < SOFTEMU_METRIC_COUNT; i++)
	{
		if (pp_palette_init_prog[i])
		{
			glDeleteProgram_fp(pp_palette_init_prog[i]);
			pp_palette_init_prog[i] = 0;
		}
	}
	if (pp_palette_tex)
	{
		R_DeleteTextures (1, &pp_palette_tex);
		pp_palette_tex = 0;
	}
	pp_lut_gpu_failed = false;
	pp_lut_gpu_built = false;
}

#else	/* USE_GLES: no compute shaders on this tier at all */

static qboolean PP_BuildPaletteLUT_GPU (softemu_metric_t metric)
{
	(void)metric;
	pp_lut_gpu_built = false;
	return false;
}

static void PP_FreePaletteGPU (void)
{
	pp_lut_gpu_built = false;
}

#endif	/* !USE_GLES */

/*
===============
PP_UpdatePaletteLUT

Rebuilds the nearest-colour LUT when the metric it was built under stops being
the one PP_SelectMetric wants -- an r_softemu_metric change, or a mode change
that moves the automatic choice.  Cheap to call every frame: the common case
is one comparison.

Mirrors Ironwail's cached_softemu_metric early-out (gl_texmgr.c:2049).  It also
compares the palette itself there; we do not, because d_8to24table is fixed
after VID_InitPalette here -- the same invariant PP_UploadLUT bakes colour on.
===============
*/
static void PP_UpdatePaletteLUT (void)
{
	softemu_metric_t metric = PP_SelectMetric();

	if (metric == pp_lut_metric)
		return;

	/* Recorded either way, so a failure is not retried on every single frame
	 * -- the CPU builder is tens of milliseconds and this runs from the frame
	 * path.  Changing the cvar moves the metric and buys one more attempt. */
	pp_lut_metric = metric;

	if (PP_BuildPaletteLUT_GPU(metric) || PP_BuildPaletteLUT_CPU(metric))
	{
		pp_lut_built = true;
		Con_DPrintf("PostProcess: palette LUT rebuilt, %s metric (%s)\n",
			    PP_MetricName(metric), pp_lut_gpu_built ? "GPU" : "CPU");
	}
	else
	{
		/* Nothing to bind: GL_PostProcess_EndFrame tests pp_lut_built before
		 * it binds a LUT, so softemu goes inert rather than sampling a stale
		 * or absent table. */
		pp_lut_built = false;
		Con_DPrintf("PostProcess: palette LUT build failed (%s metric)\n", PP_MetricName(metric));
	}
}

static void PP_BuildPaletteLUT (void)
{
	/* Colormap first: PP_SelectMetric asks whether it exists, because a game
	 * with no gfx/colormap.lmp is the one case where r_softemu 3 falls
	 * through to the nearest-colour table and wants upstream's naive RGB. */
	if (PP_BuildColormapLUT())
		Con_SafePrintf("PostProcess: colormap LUT built\n");
	else
		Con_SafePrintf("PostProcess: no colormap, r_softemu 3 falls back to nearest-colour\n");

	PP_UpdatePaletteLUT();

	if (pp_lut_built)
		Con_SafePrintf("PostProcess: palette LUT built (%s metric, %s)\n",
			       PP_MetricName(pp_lut_metric),
			       pp_lut_gpu_built ? "GPU" : "CPU");
	else
		Con_SafePrintf("PostProcess: palette LUT unavailable, r_softemu inactive\n");
}

/* ------------------------------------------------------------------ */
/* OIT FBO + resolve                                                   */
/* ------------------------------------------------------------------ */

static void OIT_DeleteFBO (void)
{
	if (oit_accum_tex) { R_DeleteTextures (1, &oit_accum_tex); oit_accum_tex = 0; }
	if (oit_revealage_tex) { R_DeleteTextures (1, &oit_revealage_tex); oit_revealage_tex = 0; }
	if (oit_fbo) { glDeleteFramebuffers_fp(1, &oit_fbo); oit_fbo = 0; }
	if (mboit_b0_tex) { R_DeleteTextures (1, &mboit_b0_tex); mboit_b0_tex = 0; }
	if (mboit_mom_tex) { R_DeleteTextures (1, &mboit_mom_tex); mboit_mom_tex = 0; }
	if (mboit_fbo) { glDeleteFramebuffers_fp(1, &mboit_fbo); mboit_fbo = 0; }
	mboit_available = false;
	oit_samples = 0;
	oit_available = false;
}

static qboolean OIT_CreateFBO (int width, int height, GLuint depth_stencil_rb, GLuint depth_stencil_tex, int samples)
{
	GLenum status;
	GLenum drawbufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	GLenum textarget = (samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;

#ifdef USE_SDL_GPU
	/* OIT is P6 of #340.  Its twins read the targets through sampler2DMS,
	 * which -gpu drops, so once P4 turns post-process on under -gpu,
	 * r_oit 1 would lose every translucent: -gpu stays on sorted blending. */
	if (R_GPU ())
		return false;
#endif
	OIT_DeleteFBO();
#ifdef USE_GLES
	/* ES 3.0 and WebGL2 have no multisample textures, so the two OIT
	 * targets cannot match a multisampled scene's depth.  Creating them as
	 * GL_TEXTURE_2D_MULTISAMPLE anyway failed both binds and both
	 * attachments, and the depth-only FBO left behind still checked
	 * complete: "OIT: FBO ready", drawing translucency into nothing.
	 * Under MSAA on this tier there is no OIT; sorted translucency it is.
	 * GitHub #112. */
	if (samples > 1)
	{
		Con_SafePrintf("OIT: unavailable with MSAA on the GLES tier (no multisample textures)\n");
		oit_samples = 0;
		return false;
	}
#endif
	oit_samples = (samples > 1) ? samples : 0;

	/* Accumulation texture (RGBA16F). When MSAA is active these must be
	 * multisampled to share the scene's multisampled depth/stencil. */
	R_GenTextures(1, &oit_accum_tex);
	R_BindTextureForEdit (textarget, oit_accum_tex);
	if (oit_samples > 1)
		R_TexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, oit_accum_tex, oit_samples,
					   GL_RGBA16F, width, height, GL_TRUE);
	else
	{
		R_TexImage2D(GL_TEXTURE_2D, oit_accum_tex, 0, GL_RGBA16F, width, height,
				GL_RGBA, GL_FLOAT, NULL);
		R_TexSetFilter(GL_TEXTURE_2D, oit_accum_tex, GL_NEAREST, GL_NEAREST);
	}

	R_GenTextures(1, &oit_revealage_tex);
	R_BindTextureForEdit (textarget, oit_revealage_tex);
	if (oit_samples > 1)
		R_TexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, oit_revealage_tex, oit_samples,
					   GL_RGBA16F, width, height, GL_TRUE);
	else
	{
		R_TexImage2D(GL_TEXTURE_2D, oit_revealage_tex, 0, GL_RGBA16F, width, height,
				GL_RGBA, GL_FLOAT, NULL);
		R_TexSetFilter(GL_TEXTURE_2D, oit_revealage_tex, GL_NEAREST, GL_NEAREST);
	}

	/* FBO with both color attachments + shared depth/stencil. The scene
	 * FBO supplies depth as either a multisampled renderbuffer (MSAA
	 * branch) or a sampler-readable texture (non-MSAA, lets Hi-Z compute
	 * read it). */
	glGenFramebuffers_fp(1, &oit_fbo);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, oit_fbo);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				  textarget, oit_accum_tex, 0);
	glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
				  textarget, oit_revealage_tex, 0);
	if (depth_stencil_tex)
		glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
					  GL_TEXTURE_2D, depth_stencil_tex, 0);
	else
		glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
					     GL_RENDERBUFFER, depth_stencil_rb);
	glDrawBuffers_fp(2, drawbufs);

	status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_SafePrintf("OIT: FBO incomplete (status 0x%x)\n", status);
		OIT_DeleteFBO();
		return false;
	}

	oit_width = width;
	oit_height = height;
	oit_available = true;
	Con_SafePrintf("OIT: %dx%d FBO ready\n", width, height);

#ifndef USE_GLES
	/* The MBOIT moment targets (#87), sharing the scene depth like the WBOIT
	 * pair.  Optional: without them r_oit 2 falls back to weighted blended. */
	{
		GLenum	fmts[2] = { GL_R32F, GL_RGBA32F };
		GLenum	cfmts[2] = { GL_RED, GL_RGBA };
		GLuint	*texs[2] = { &mboit_b0_tex, &mboit_mom_tex };
		int	k;

		for (k = 0; k < 2; k++)
		{
			R_GenTextures(1, texs[k]);
			R_BindTextureForEdit (textarget, *texs[k]);
			if (oit_samples > 1)
				R_TexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, *texs[k], oit_samples,
							   fmts[k], width, height, GL_TRUE);
			else
			{
				R_TexImage2D(GL_TEXTURE_2D, *texs[k], 0, fmts[k], width, height,
						cfmts[k], GL_FLOAT, NULL);
				R_TexSetFilter(GL_TEXTURE_2D, *texs[k], GL_NEAREST, GL_NEAREST);
			}
		}
		glGenFramebuffers_fp(1, &mboit_fbo);
		glBindFramebuffer_fp(GL_FRAMEBUFFER, mboit_fbo);
		glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textarget, mboit_b0_tex, 0);
		glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, textarget, mboit_mom_tex, 0);
		if (depth_stencil_tex)
			glFramebufferTexture2D_fp(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
						  GL_TEXTURE_2D, depth_stencil_tex, 0);
		else
			glFramebufferRenderbuffer_fp(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
						     GL_RENDERBUFFER, depth_stencil_rb);
		glDrawBuffers_fp(2, drawbufs);
		status = glCheckFramebufferStatus_fp(GL_FRAMEBUFFER);
		glBindFramebuffer_fp(GL_FRAMEBUFFER, 0);
		R_BindTextureForEdit (textarget, 0);
		mboit_available = (status == GL_FRAMEBUFFER_COMPLETE);
		if (!mboit_available)
			Con_SafePrintf("OIT: MBOIT moment targets incomplete (status 0x%x); r_oit 2 falls back to 1\n", status);
	}
#endif
	return true;
}

static qboolean OIT_InitShader (void)
{
	GLuint prog;

#ifdef USE_SDL_GPU
	/* OIT is P6 (see OIT_CreateFBO).  Gated here as well because this runs
	 * at every -gpu boot: the null glBlendFunci is not NULL, so
	 * HW_OIT_HAS_BLEND_FUNCI lets it in, and all it would do is raw GL. */
	if (R_GPU ())
		return false;
#endif
	prog = R_Program_Create(RPROG_OIT_RESOLVE);
	if (!prog)
		return false;

	oit_resolve_prog = prog;
	oit_resolve_loc_accum = glGetUniformLocation_fp(prog, "TexAccum");
	oit_resolve_loc_reveal = glGetUniformLocation_fp(prog, "TexReveal");
	PP_BindUniformBlock(prog, "OitParams", OIT_PARAMS_UBO_BINDING, sizeof(oit_params_t));
	if (!oit_ubo)
	{
		glGenBuffers_fp(1, &oit_ubo);
		GL_BindBuffer(GL_UNIFORM_BUFFER, oit_ubo);
		glBufferData_fp(GL_UNIFORM_BUFFER, sizeof(oit_params_t), NULL, GL_DYNAMIC_DRAW);
		GL_BindBuffer(GL_UNIFORM_BUFFER, 0);
	}

#ifndef USE_GLES
	/* MSAA per-sample resolve variant — best-effort. If it fails to build
	 * the MSAA OIT path is simply never selected, since oit_samples stays 0
	 * then.  Not built at all on the ES tier — see oit_resolve_frag_msaa. */
	prog = R_Program_Create(RPROG_OIT_RESOLVE_MSAA);
	if (prog)
	{
		oit_resolve_prog_msaa = prog;
		oit_resolve_msaa_loc_accum = glGetUniformLocation_fp(prog, "TexAccum");
		oit_resolve_msaa_loc_reveal = glGetUniformLocation_fp(prog, "TexReveal");
		PP_BindUniformBlock(prog, "OitParams", OIT_PARAMS_UBO_BINDING, sizeof(oit_params_t));
	}
#endif /* !USE_GLES */

	/* GL 4.3 core profile requires a VAO bound for any draw call. */
	glGenVertexArrays_fp(1, &oit_resolve_vao);

	Con_SafePrintf("OIT: resolve shader OK\n");
	return true;
}

void OIT_BeginTranslucency (void)
{
	static const float zeroes[4] = {0.f, 0.f, 0.f, 0.f};
	static const float ones[4] = {1.f, 1.f, 1.f, 1.f};

#ifdef USE_SDL_GPU
	if (R_GPU ())	/* P6, see OIT_CreateFBO */
		return;
#endif
	if (!oit_available || !r_oit.integer || !HW_OIT_HAS_BLEND_FUNCI)
		return;

	oit_in_pass = true;

	/* r_oit 2: the moment pass first; OIT_NextPass moves on to the
	 * composite.  Both targets sum additively. */
	if (r_oit.integer >= 2 && mboit_available)
	{
		mboit_pass = 1;
		glBindFramebuffer_fp(GL_FRAMEBUFFER, mboit_fbo);
		glClearBufferfv_fp(GL_COLOR, 0, zeroes);
		glClearBufferfv_fp(GL_COLOR, 1, zeroes);
		R_SetBlend (true);
		R_SetBlendFuncIndexed (0, GL_ONE, GL_ONE);
		R_SetBlendFuncIndexed (1, GL_ONE, GL_ONE);
		R_SetDepthMask (false);
		R_SetOITMode (1, oit_samples > 1);
		return;
	}

	mboit_pass = 0;
	R_SetOITMode (0, false);
	glBindFramebuffer_fp(GL_FRAMEBUFFER, oit_fbo);
	glClearBufferfv_fp(GL_COLOR, 0, zeroes);
	glClearBufferfv_fp(GL_COLOR, 1, ones);

	/* Per-buffer blending for WBOIT */
	R_SetBlend (true);
	R_SetBlendFuncIndexed (0, GL_ONE, GL_ONE);			/* accum: additive */
	R_SetBlendFuncIndexed (1, GL_ZERO, GL_ONE_MINUS_SRC_COLOR);	/* revealage: multiplicative */

	/* Translucent geometry reads depth but doesn't write it */
	R_SetDepthMask (false);
}

/* Moves an r_oit 2 translucency pass from moments to composite; the caller
 * draws its translucents again while this returns true (gl_rmain.c).  The
 * composite writes oit_accum_tex only -- one draw buffer, so the second
 * output of the OIT programs goes nowhere -- reading the moment targets
 * through MBOIT_TMU_*.  GitHub #87. */
qboolean OIT_NextPass (void)
{
	static const float zeroes[4] = {0.f, 0.f, 0.f, 0.f};
	GLenum	one = GL_COLOR_ATTACHMENT0;
	GLenum	textarget = (oit_samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;

	if (!oit_in_pass || mboit_pass != 1)
		return false;

	mboit_pass = 2;
	glBindFramebuffer_fp(GL_FRAMEBUFFER, oit_fbo);
	glDrawBuffers_fp(1, &one);
	glClearBufferfv_fp(GL_COLOR, 0, zeroes);
	R_SetBlend (true);
	R_SetBlendFuncIndexed (0, GL_ONE, GL_ONE);
	R_SetDepthMask (false);

	R_BindTextureTarget ((oit_samples > 1) ? MBOIT_TMU_B0_MS : MBOIT_TMU_B0, textarget, mboit_b0_tex);
	R_BindTextureTarget ((oit_samples > 1) ? MBOIT_TMU_MOM_MS : MBOIT_TMU_MOM, textarget, mboit_mom_tex);

	R_SetOITMode (2, oit_samples > 1);
	return true;
}

/* True while translucents are being drawn for a pass that will be drawn
 * again: the r_oit 2 moment pass.  Draw code that consumes a list as it
 * draws it (R_DrawWaterSurfaces nulls its texture chains) keeps it for the
 * composite.  GitHub #87. */
qboolean OIT_KeepChains (void)
{
	return oit_in_pass && mboit_pass == 1;
}

void OIT_EndTranslucency (GLuint scene_fbo)
{
	GLenum textarget;
	GLuint prog;
	GLint loc_accum, loc_reveal;
	qboolean cull_was_on, mboit;
	GLint saved_viewport[4];

#ifdef USE_SDL_GPU
	if (R_GPU ())	/* P6, see OIT_CreateFBO */
		return;
#endif
	if (!oit_available || !r_oit.integer || !HW_OIT_HAS_BLEND_FUNCI)
		return;

	textarget = (oit_samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
	prog = (oit_samples > 1) ? oit_resolve_prog_msaa : oit_resolve_prog;
	loc_accum = (oit_samples > 1) ? oit_resolve_msaa_loc_accum : oit_resolve_loc_accum;
	loc_reveal = (oit_samples > 1) ? oit_resolve_msaa_loc_reveal : oit_resolve_loc_reveal;

	oit_in_pass = false;

	/* r_oit 2: the composite left oit_accum_tex holding transmittance-
	 * weighted colour; resolve it with b0 standing in for revealage. */
	mboit = (mboit_pass == 2);
	if (mboit_pass)
	{
		GLenum	two[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		GLenum	mt = (oit_samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;

		glBindFramebuffer_fp(GL_FRAMEBUFFER, oit_fbo);
		glDrawBuffers_fp(2, two);
		R_BindTextureTarget ((oit_samples > 1) ? MBOIT_TMU_B0_MS : MBOIT_TMU_B0, mt, 0);
		R_BindTextureTarget ((oit_samples > 1) ? MBOIT_TMU_MOM_MS : MBOIT_TMU_MOM, mt, 0);
		R_SetOITMode (0, false);
		mboit_pass = 0;
	}

	cull_was_on = R_GetCull();
	glGetIntegerv_fp(GL_VIEWPORT, saved_viewport);

	glBindFramebuffer_fp(GL_FRAMEBUFFER, scene_fbo);

	R_UseProgram (prog);

	R_SetBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	R_SetBlend (true);
	R_SetDepthMask (false);
	R_SetDepthTest (false);

	R_BindTextureTarget (0, textarget, oit_accum_tex);
	R_BindTextureTarget (1, textarget, mboit ? mboit_b0_tex : oit_revealage_tex);

	if (loc_accum >= 0) glUniform1i_fp(loc_accum, 0);
	if (loc_reveal >= 0) glUniform1i_fp(loc_reveal, 1);
	{
		oit_params_t	op = {{ mboit ? 1 : 0, 0, 0, 0 }};

		GL_BindBuffer(GL_UNIFORM_BUFFER, oit_ubo);
		glBufferSubData_fp(GL_UNIFORM_BUFFER, 0, sizeof(op), &op);
		GL_BindBuffer(GL_UNIFORM_BUFFER, 0);
		GL_BindBufferBase(GL_UNIFORM_BUFFER, OIT_PARAMS_UBO_BINDING, oit_ubo);
	}

	/* Full-buffer for the resolve triangle, but SAVED and put back below.
	 * The scene's viewport is not the whole buffer: R_SetupGL() derives it
	 * from r_refdef.vrect, and SCR_CalcRefdef() gives the status bar 36 lines
	 * whenever viewsize < 110, so the 3D view is (0,36,W,H-36).  Leaving the
	 * full-buffer viewport behind put everything drawn after this -- glows,
	 * viewmodel, mirror, the bbox/pointfile debug draws -- through a different
	 * viewport transform than the world it is meant to sit on: same clip
	 * coords, so a downward shift plus a vertical stretch.  Measured at
	 * 640x480 viewsize 100, the mana-orb glows landed 24 px below their orbs
	 * while the world stayed pixel-identical.  uhexen2-osya.
	 *
	 * Same reasoning as the cull-state save/restore below, and the same
	 * mid-frame reason it has to be a restore and not a re-assert: this runs
	 * between R_RenderScene and those passes, none of which set a viewport of
	 * their own.  Queried rather than recomputed so a caller that arrives with
	 * some other viewport gets its own back. */
	R_SetViewport(0, 0, pp_width, pp_height);
	R_SetColorMask (true, true, true, true);

	/* The resolve triangle is counter-clockwise in NDC, i.e. front-facing,
	 * and this engine culls FRONT faces (gl_vidsdl.c :: GL_Init(), and
	 * R_SetupGL re-asserts it every frame when gl_cull is on).  Leave culling
	 * enabled and the whole fullscreen triangle is thrown away before
	 * rasterisation: accum and revealage are written correctly and then
	 * composited nowhere, so every translucent thing routed through OIT --
	 * sprites, particles, translucent water -- renders as absolutely nothing.
	 * That is uhexen2-z4r1, and uhexen2-a0hp / uhexen2-mex9 before it; it
	 * looked hardware-specific only because nobody had reproduced it on a
	 * second stack.  PP_BlitWith3DEffects and the final blit both disable
	 * culling before their fullscreen draws; this one has to as well.
	 *
	 * Restored rather than left off, because unlike those two this runs
	 * mid-frame -- R_DrawAllGlows, R_DrawViewModel and R_Mirror all draw
	 * after it and expect the scene's cull state.  Read back rather than
	 * assumed, since gl_cull 0 legitimately leaves it off; the read is off
	 * gl_pipeline.c's shadow, which knows it exactly and for free. */
	R_SetCull (false);

	/* GL 4.3 core profile requires a VAO bound for glDrawArrays. */
	glBindVertexArray_fp(oit_resolve_vao);
	glDrawArrays_fp(GL_TRIANGLES, 0, 3);
	glBindVertexArray_fp(0);

	if (cull_was_on)
		R_SetCull (true);

	R_SetViewport(saved_viewport[0], saved_viewport[1],
		      saved_viewport[2], saved_viewport[3]);

	R_BindTextureTarget (1, textarget, 0);
	R_UseProgram (0);
	R_SetDepthTest (true);
	R_SetDepthMask (true);
	R_SetBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

qboolean OIT_Active (void)
{
	/* map_checks suppresses the pass without tearing the pipeline down, so
	 * the mapper sees unordered translucency the way a plain blend gives it
	 * and toggling the mode back off restores OIT with no vid_restart.
	 * uhexen2-a5nn.34 */
	if (map_checks.integer)
		return false;
#ifdef USE_SDL_GPU
	if (R_GPU ())	/* P6, see OIT_CreateFBO */
		return false;
#endif
	return oit_available && r_oit.integer && HW_OIT_HAS_BLEND_FUNCI;
}

qboolean OIT_InPass (void)
{
	return oit_in_pass;
}

/* The scene FBO only while this frame renders into it.  pp_fbo outlives the
 * chain: when nothing needs post any more (r_ssao 1 -> 0 at runtime, #539)
 * it stays allocated, and handing it out made Hi-Z build from, and soft
 * particles match formats against, a buffer nothing draws into. */
GLuint GL_GetSceneFBO (void)
{
	return pp_active ? pp_fbo : 0;
}

GLuint GL_PostProcess_GetSceneDepthTex (void)
{
	return pp_active ? pp_depth_tex : 0;	/* 0 in the MSAA branch too */
}

/* ------------------------------------------------------------------ */
/* Soft-particle depth snapshot (uhexen2-mf9u)                         */
/* ------------------------------------------------------------------ */

static void PP_DeleteSoftDepth (void)
{
	if (soft_depth_tex) { R_DeleteTextures (1, &soft_depth_tex); soft_depth_tex = 0; }
	if (soft_depth_fbo) { R_DeleteFramebuffers(1, &soft_depth_fbo); soft_depth_fbo = 0; }
	soft_depth_w = soft_depth_h = 0;
	soft_depth_fmt = 0;
	soft_depth_valid = false;
}

void GL_SoftDepth_Capture (void)
{
	GLenum	fmt, attach, status;
	GLuint	prev_draw = 0, prev_read = 0;
	int	w = glwidth, h = glheight;	/* pp rebinds these to the scaled scene size */
	qboolean	packed;

	soft_depth_valid = false;

	if (!r_softparticles.integer || !glBlitFramebuffer_fp || w <= 0 || h <= 0)
		return;

	/* MSAA scene: resolving a multisampled depth buffer down to one sample
	 * is implementation-defined, so the snapshot would not reliably match
	 * what the depth test actually compared against.  Skip and let sprites
	 * keep the hard cut rather than fade against a guess.  Mirrors how
	 * R_BuildHiZForNextFrame declines the same path. */
	if (GL_GetSceneFBO () && pp_samples > 1)
		return;

	/* glBlitFramebuffer wants matching depth formats.  The scene FBO carries
	 * packed depth/stencil; the default-framebuffer case mirrors the format
	 * the Hi-Z standalone resolve has been blitting from fb 0 since
	 * uhexen2-9912.  Under -gpu framebuffer 0 is a render target too, in
	 * the scene's own depth format, which is what GL_DEPTH24_STENCIL8 names
	 * there (rg_resource.c RGT_Format); the backend copies depth only
	 * between textures of one format, so both sources take that (#340 P4b). */
	packed = GL_GetSceneFBO () != 0;
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		/* A snapshot the device cannot sample binds a placeholder, read as
		 * the near plane: every soft sprite would fade out entirely. */
		if (!RG_DepthSampled ())
			return;
		packed = true;
	}
#endif
	if (packed)	{ fmt = GL_DEPTH24_STENCIL8;  attach = GL_DEPTH_STENCIL_ATTACHMENT; }
	else		{ fmt = GL_DEPTH_COMPONENT24; attach = GL_DEPTH_ATTACHMENT; }

	if (soft_depth_tex && soft_depth_fbo &&
	    soft_depth_w == w && soft_depth_h == h && soft_depth_fmt == fmt)
		goto blit;

	PP_DeleteSoftDepth();

	R_GenTextures(1, &soft_depth_tex);
	R_BindTextureForEdit (GL_TEXTURE_2D, soft_depth_tex);
	R_TexImage2D(GL_TEXTURE_2D, soft_depth_tex, 0, fmt, w, h,
			(fmt == GL_DEPTH24_STENCIL8) ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT,
			(fmt == GL_DEPTH24_STENCIL8) ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_INT,
			NULL);
	R_TexSetFilter(GL_TEXTURE_2D, soft_depth_tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, soft_depth_tex, GL_CLAMP_TO_EDGE);
	R_BindTextureForEdit (GL_TEXTURE_2D, 0);

	prev_draw = R_FramebufferBinding(GL_DRAW_FRAMEBUFFER_BINDING);
	R_GenFramebuffers(1, &soft_depth_fbo);
	R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, soft_depth_fbo);
	R_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attach,
			       GL_TEXTURE_2D, soft_depth_tex, 0);
	/* Depth-only FBO needs DRAW_BUFFER = NONE for completeness on strict
	 * drivers; it is only ever a blit destination. */
	{
		GLenum none = GL_NONE;
		R_DrawBuffers(1, &none);
	}
	status = R_CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
	R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_DPrintf("Soft particles: depth FBO incomplete (0x%x)\n", status);
		PP_DeleteSoftDepth();
		return;
	}

	soft_depth_w = w;
	soft_depth_h = h;
	soft_depth_fmt = fmt;

blit:
	/* Read from whatever the scene is currently drawing into — pp_fbo when
	 * postprocess is up, the default framebuffer otherwise, and either one
	 * again on the mirror pass.  GL_NEAREST is the only legal depth filter. */
	prev_draw = R_FramebufferBinding(GL_DRAW_FRAMEBUFFER_BINDING);
	prev_read = R_FramebufferBinding(GL_READ_FRAMEBUFFER_BINDING);
	R_BindFramebuffer(GL_READ_FRAMEBUFFER, prev_draw);
	R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, soft_depth_fbo);
	soft_depth_valid = R_BlitFramebuffer(0, 0, w, h, 0, 0, w, h,
					     GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
	R_BindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
}

GLuint GL_SoftDepth_GetTex (void)
{
	return soft_depth_valid ? soft_depth_tex : 0;
}

qboolean GL_PostProcess_GetSceneSize (int *w, int *h)
{
	if (!pp_initialized || !GL_GetSceneFBO () || pp_width <= 0 || pp_height <= 0)
		return false;
	if (w) *w = pp_width;
	if (h) *h = pp_height;
	return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void GL_PostProcess_Init (void)
{
	Cvar_RegisterVariable(&r_scale);
	Cvar_RegisterVariable(&r_softemu);
	Cvar_RegisterVariable(&r_dither);
	Cvar_RegisterVariable(&r_softemu_dither_screen);
	Cvar_RegisterVariable(&r_softemu_dither_texture);
	Cvar_RegisterVariable(&r_softemu_lightmap_banding);
	Cvar_RegisterVariable(&r_softemu_mdl_warp);
	Cvar_RegisterVariable(&r_softemu_spans);
	Cvar_RegisterVariable(&r_softemu_metric);
	Cvar_RegisterVariable(&r_hdr);
	Cvar_RegisterVariable(&r_hdr_exposure);
	Cvar_RegisterVariable(&r_oit);
	Cvar_RegisterVariable(&r_bloom);
	Cvar_RegisterVariable(&r_bloom_intensity);
	Cvar_RegisterVariable(&r_bloom_threshold);
	Cvar_RegisterVariable(&r_softparticles);
	Cvar_RegisterVariable(&r_softparticles_scale);

	Cvar_SetCallback(&r_hdr, PP_FormatChanged);
	Cvar_SetCallback(&r_bloom, PP_FormatChanged);
	Cvar_SetCallback(&r_softemu_metric, PP_MetricChanged);

	/* Report only -- deliberately NOT Cvar_Set("r_hdr", "0"), which is what
	 * d2c46f078 did here.  PP_HDRActive() already gates every functional
	 * reader, so an archived r_hdr 1 cannot reach an RGBA16F allocation on a
	 * context without EXT_color_buffer_float; resetting the cvar would only
	 * rewrite the config of somebody who also plays on a desktop.  Same
	 * reasoning as the r_oit note directly below. */
	if (r_hdr.integer && !gl_renderer_caps.float_color_buffer)
		Con_SafePrintf("[RENDERER] Floating-point render targets unavailable; HDR inactive\n");

	/* r_oit used to be force-reset to 0 here, discarding an archived 1 on
	 * every startup, because OIT rendered nothing at all.  That was
	 * uhexen2-z4r1: the resolve's fullscreen triangle was front-face culled,
	 * so correctly filled accum/revealage buffers were composited nowhere.
	 * Fixed in 8dcbc6284, so the workaround is now hiding a fixed bug and
	 * stopping anybody from accumulating playtime on the feature.
	 *
	 * Still defaults to 0 -- WBOIT is an approximation, not a free upgrade,
	 * and this only lets the setting persist for someone who opts in.
	 * uhexen2-hhs1. */

	pp_initialized = false;
	pp_active = false;
	pp_fbo = 0;
	pp_color_tex = 0;
	pp_depth_rb = 0;
	pp_depth_tex = 0;
	pp_program = 0;
	pp_width = pp_height = 0;
	/* Context is new — drop the old names rather than deleting them. */
	soft_depth_fbo = 0;
	soft_depth_tex = 0;
	soft_depth_w = soft_depth_h = 0;
	soft_depth_fmt = 0;
	soft_depth_valid = false;
	/* The post-process uniform buffers (#450) are names in the same dead
	 * context; their creation paths test for 0 (oit_ubo, pp_*_ubo) or
	 * regenerate unconditionally (bloom_ubo). */
	oit_ubo = 0;
	bloom_ubo = 0;
	pp_vert_ubo = 0;
	pp_frag_ubo = 0;

	/* GL 4.3: shaders always available */

	/* check for FBO function pointers */
#ifndef USE_GLES
#ifdef USE_SDL_GPU
	/* -gpu loads no GL library, so every lookup here would return NULL and
	 * overwrite rg_null.c's stub for the same pointer (#340). */
	if (!R_GPU())
#endif
	{
		/* On desktop GL, dynamically load function pointers */
		glGenFramebuffers_fp = (glGenFramebuffers_f) SDL_GL_GetProcAddress("glGenFramebuffers");
		glDeleteFramebuffers_fp = (glDeleteFramebuffers_f) SDL_GL_GetProcAddress("glDeleteFramebuffers");
		glBindFramebuffer_fp = (glBindFramebuffer_f) SDL_GL_GetProcAddress("glBindFramebuffer");
		glFramebufferTexture2D_fp = (glFramebufferTexture2D_f) SDL_GL_GetProcAddress("glFramebufferTexture2D");
		glFramebufferRenderbuffer_fp = (glFramebufferRenderbuffer_f) SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
		glCheckFramebufferStatus_fp = (glCheckFramebufferStatus_f) SDL_GL_GetProcAddress("glCheckFramebufferStatus");
		glGenRenderbuffers_fp = (glGenRenderbuffers_f) SDL_GL_GetProcAddress("glGenRenderbuffers");
		glDeleteRenderbuffers_fp = (glDeleteRenderbuffers_f) SDL_GL_GetProcAddress("glDeleteRenderbuffers");
		glBindRenderbuffer_fp = (glBindRenderbuffer_f) SDL_GL_GetProcAddress("glBindRenderbuffer");
		glRenderbufferStorage_fp = (glRenderbufferStorage_f) SDL_GL_GetProcAddress("glRenderbufferStorage");
		glRenderbufferStorageMultisample_fp = (glRenderbufferStorageMultisample_f) SDL_GL_GetProcAddress("glRenderbufferStorageMultisample");
		glTexImage2DMultisample_fp = (glTexImage2DMultisample_f) SDL_GL_GetProcAddress("glTexImage2DMultisample");
		glBlitFramebuffer_fp = (glBlitFramebuffer_f) SDL_GL_GetProcAddress("glBlitFramebuffer");

		if (!glGenFramebuffers_fp || !glDeleteFramebuffers_fp ||
		    !glBindFramebuffer_fp || !glFramebufferTexture2D_fp ||
		    !glFramebufferRenderbuffer_fp || !glCheckFramebufferStatus_fp ||
		    !glGenRenderbuffers_fp || !glDeleteRenderbuffers_fp ||
		    !glBindRenderbuffer_fp || !glRenderbufferStorage_fp)
		{
			Con_SafePrintf("PostProcess: FBO functions not available\n");
			return;
		}

		/* GL functions loaded at runtime */
		glTexImage3D_fp = (glTexImage3D_f) SDL_GL_GetProcAddress("glTexImage3D");
	}
#endif /* !USE_GLES */

	if (!PP_InitShader())
	{
		Con_SafePrintf("PostProcess: shader init failed\n");
		return;
	}

	/* build the palette LUT for software rendering emulation */
	PP_BuildPaletteLUT();

	pp_initialized = true;
	Con_SafePrintf("PostProcess: gamma/contrast shader ready\n");

	/* Init OIT resolve shader (FBO created lazily when scene FBO is ready) */
	if (HW_OIT_HAS_BLEND_FUNCI && glDrawBuffers_fp && glClearBufferfv_fp)
		OIT_InitShader();
}

void GL_PostProcess_Shutdown (void)
{
	R_PipelineForgetProgram ();	/* GL reuses program names */
	OIT_DeleteFBO();
	if (oit_resolve_prog) { glDeleteProgram_fp(oit_resolve_prog); oit_resolve_prog = 0; }
	if (oit_resolve_prog_msaa) { glDeleteProgram_fp(oit_resolve_prog_msaa); oit_resolve_prog_msaa = 0; }
	if (oit_ubo) { GL_ForgetBuffer(oit_ubo); glDeleteBuffers_fp(1, &oit_ubo); oit_ubo = 0; }
	if (bloom_ubo) { GL_ForgetBuffer(bloom_ubo); glDeleteBuffers_fp(1, &bloom_ubo); bloom_ubo = 0; }
	if (pp_vert_ubo) { GL_ForgetBuffer(pp_vert_ubo); glDeleteBuffers_fp(1, &pp_vert_ubo); pp_vert_ubo = 0; }
	if (pp_frag_ubo) { GL_ForgetBuffer(pp_frag_ubo); glDeleteBuffers_fp(1, &pp_frag_ubo); pp_frag_ubo = 0; }
	PP_DeleteFBO();
	PP_DeleteNativeFBO();
	PP_DeleteSoftDepth();
	if (pp_program)
	{
		glDeleteProgram_fp(pp_program);
		pp_program = 0;
	}
	if (pp_palette_lut)
	{
		R_DeleteTextures (1, &pp_palette_lut);
		pp_palette_lut = 0;
	}
	if (pp_colormap_lut)
	{
		R_DeleteTextures (1, &pp_colormap_lut);
		pp_colormap_lut = 0;
	}
	PP_FreePaletteGPU();
	pp_lut_built = false;
	pp_lut_metric = SOFTEMU_METRIC_COUNT;
	pp_initialized = false;
	pp_active = false;
	pp_native_active = false;
	pp_prev_yaw = pp_prev_pitch = 0.0f;
	pp_fbo_failed = false;
	if (pp_copyback_tex) { R_DeleteTextures (1, &pp_copyback_tex); pp_copyback_tex = 0; }
	pp_copyback_w = pp_copyback_h = 0;
}

/*
=================
PP_ApplySampleShading

vid_fsaamode, applied to the scene FBO for the 3D pass only.

Three conditions, and all three are load-bearing.  The driver has to have
glMinSampleShading at all (GL 4.0; the ES/WebGL2 tier has neither the entry
point nor the extension, which is why gl_renderer_caps carries the answer
rather than this file guessing).  The target has to be genuinely multisampled,
because GL_SAMPLE_SHADING against a single-sample framebuffer is a no-op that
still costs a state change every frame.  And the player has to have asked,
because shading every sample is supersampling and priced like it.

Turned off again in End3D rather than left on: the resolve blit and everything
2D draw after it render to single-sample targets, where the state is dead
weight at best.
=================
*/
static void PP_ApplySampleShading (void)
{
#ifndef USE_GLES
	static float	pp_reported_frac = -1.0f;
	float		frac;

	if (!gl_renderer_caps.sample_shading || pp_samples <= 1)
		return;

	frac = vid_fsaamode.value;
	if (frac < 0.0f)
		frac = 0.0f;
	else if (frac > 1.0f)
		frac = 1.0f;

	GL_SetSampleShading (frac);

	/* Reported on change only -- per frame it would be a wall of text, and
	 * never reported at all leaves no way to tell "the cvar did nothing"
	 * from "the cvar was never reached", which are different bugs.  The
	 * three preconditions above are all silent failures otherwise. */
	if (frac != pp_reported_frac)
	{
		pp_reported_frac = frac;
		Con_DPrintf ("[RENDERER] sample shading %s (%.2f of %d samples)\n",
			     (frac > 0.0f) ? "on" : "off", frac, pp_samples);
	}
#endif
}

static void PP_EndSampleShading (void)
{
#ifndef USE_GLES
	if (gl_renderer_caps.sample_shading)
		GL_SetSampleShading (0.0f);
#endif
}

void GL_PostProcess_BeginFrame (void)
{
	int w, h;
	float scale;

	pp_active = false;
	pp_native_active = false;

	if (!PP_NeedsPostProcess())
		return;

	/* clamp render scale */
	scale = r_scale.value;
	if (scale < 0.25f) scale = 0.25f;
	if (scale > 1.0f) scale = 1.0f;

	/* get current viewport size, apply scale */
	w = (int)(glwidth * scale);
	h = (int)(glheight * scale);
	if (w <= 0 || h <= 0)
		return;

	/* (re)create FBO if size changed — MSAA changes go through
	 * vid_restart which destroys and recreates everything */
	if (pp_fbo_failed)
	{
		static qboolean warned;
#ifdef USE_SDL_GPU
		/* The copyback reads the framebuffer back (R_TexCopySubImage2D),
		 * which -gpu does not do: with no FBO, no post-process. */
		if (R_GPU ())
		{
			if (!warned)
			{
				Con_Printf("PostProcess: FBO failed, post-process off under -gpu\n");
				warned = true;
			}
			return;
		}
#endif
		if (!warned)
		{
			Con_Printf("PostProcess: FBO failed, using copyback fallback for gamma/contrast\n");
			warned = true;
		}
		/* No FBO — scene renders to default framebuffer.
		 * EndFrame will copy backbuffer to texture and apply shader. */
		pp_active = true;
		pp_saved_glwidth = glwidth;
		pp_saved_glheight = glheight;
		return;
	}
	if (w != pp_width || h != pp_height || pp_rebuild_pending)
	{
		pp_rebuild_pending = false;
		if (!PP_CreateFBO(w, h))
			return;
	}

	/* save original viewport and override with scaled resolution */
	pp_saved_glwidth = glwidth;
	pp_saved_glheight = glheight;
	glwidth = w;
	glheight = h;

	/* bind scene FBO */
	R_BindFramebuffer(GL_FRAMEBUFFER, pp_fbo);
	PP_ApplySampleShading ();
	pp_active = true;
}

/* Apply warp+blur shader to src_tex into the currently-bound framebuffer.
 * Uses identity gamma/contrast/fxaa — 3D-scene effects only. */
static void PP_BlitWith3DEffects (GLuint src_tex, int w, int h, float warp, float blur, float scale)
{
	R_SetDepthTest (false);
	R_SetBlend (false);
	R_SetCull (false);

	GL_MatrixMode(GL_MAT_PROJECTION);
	GL_PushMatrix();
	GL_LoadIdentity();
	GL_Ortho(0, 1, 0, 1, -1, 1);
	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_PushMatrix();
	GL_LoadIdentity();

	R_BindTextureSlot (0, src_tex);
	R_TexSetFilter(GL_TEXTURE_2D, src_tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, src_tex, GL_CLAMP_TO_EDGE);

	R_UseProgram (pp_program);
	/* the PpVert MVP is taken from the current matrices in PP_PushParams */
	pp_params.p0[0] = 1.0f;			/* gamma */
	pp_params.p0[1] = 1.0f;			/* contrast */
	pp_params.p3[1] = 0.0f;			/* hdr_exposure */
	pp_params.i0[0] = 0;			/* softemu */
	pp_params.p0[2] = 0.0f;			/* dither */
	pp_params.p1[3] = 0.0f;			/* fxaa_on */
	pp_params.p1[0] = scale;		/* scale */
	pp_params.p1[1] = warp;			/* waterwarp */
	pp_params.p1[2] = (float)realtime;	/* time */
	pp_params.p2[0] = 1.0f / w;		/* rcpFrame */
	pp_params.p2[1] = 1.0f / h;		/* rcpFrame.y */
	/* No bloom here.  This pass (the 3D effects: motion blur, and water warp
	 * when underwater) runs before EndFrame builds this frame's bloom.  It
	 * never set the strength, so it inherited the previous frame's -- in the
	 * shadow, as in the loose uniform before it -- and sampled whatever was
	 * still on unit 2, the previous frame's bloom_tex[0].  So with r_bloom on
	 * it composited LAST frame's bloom into the image this frame's bloom is
	 * then extracted from and added to again: a second, stale bloom. */
	pp_params.p3[2] = 0.0f;			/* u_bloom_strength */
	{
		float yaw   = cl.viewangles[1];
		float pitch = cl.viewangles[0];
		float dy = (yaw   - pp_prev_yaw)   * 0.002f;
		float dp = (pitch - pp_prev_pitch) * 0.002f;

		/* position-based velocity: project movement onto screen axes */
		{
			vec3_t delta;
			float move_h, move_v;
			VectorSubtract(r_origin, pp_prev_origin, delta);
			move_h = DotProduct(delta, vright) * 0.0004f;
			move_v = DotProduct(delta, vup)    * 0.0004f;
			dy += move_h;
			dp += move_v;
			VectorCopy(r_origin, pp_prev_origin);
		}

		if (dy >  0.03f) dy =  0.03f; else if (dy < -0.03f) dy = -0.03f;
		if (dp >  0.03f) dp =  0.03f; else if (dp < -0.03f) dp = -0.03f;
		pp_params.p3[0] = blur;		/* motionblur */
		pp_params.p2[2] = dy;		/* viewdelta */
		pp_params.p2[3] = dp;		/* viewdelta.y */
		pp_prev_yaw   = yaw;
		pp_prev_pitch = pitch;
	}
	PP_PushParams();

	GL_ImmBegin();
	GL_ImmColor4f(1, 1, 1, 1);
	GL_ImmTexCoord2f(0, 0); GL_ImmVertex2f(0, 0);
	GL_ImmTexCoord2f(1, 0); GL_ImmVertex2f(1, 0);
	GL_ImmTexCoord2f(1, 1); GL_ImmVertex2f(1, 1);
	GL_ImmTexCoord2f(0, 1); GL_ImmVertex2f(0, 1);
	GL_ImmDraw(GL_QUADS);

	R_UseProgram (0);

	GL_MatrixMode(GL_MAT_PROJECTION);
	GL_PopMatrix();
	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_PopMatrix();

	R_SetDepthTest (true);
}

void GL_PostProcess_End3D (void)
{
	int native_w, native_h;
	float warp, blur, scale;
	extern mleaf_t *r_viewleaf;

	if (!pp_active)
		return;

	PP_EndSampleShading ();

	if (pp_fbo_failed)
	{
		/* Copyback path: 3D scene is in the default framebuffer.
		 * Apply warp/blur now so 2D draws on top of the processed scene. */
		int w = pp_saved_glwidth;
		int h = pp_saved_glheight;

		warp = 0.0f;
		if (r_waterwarp.value && r_viewleaf && r_viewleaf->contents <= CONTENTS_WATER)
			warp = r_waterwarp.value;
		else if (cl.time < pp_waterwarp_preview_end)
			warp = r_waterwarp.value;	/* show preview even if not in water */
		blur = Cvar_VariableValue("r_motionblur");

		if (warp > 0.0f || blur > 0.0f)
		{
			/* Grab the 3D scene from the backbuffer */
			if (!pp_copyback_tex || w != pp_copyback_w || h != pp_copyback_h)
			{
				if (pp_copyback_tex)
					R_DeleteTextures (1, &pp_copyback_tex);
				R_GenTextures(1, &pp_copyback_tex);
				R_BindTextureForEdit (GL_TEXTURE_2D, pp_copyback_tex);
				R_TexImage2D(GL_TEXTURE_2D, pp_copyback_tex, 0, GL_RGB10_A2, w, h,
						GL_RGBA, GL_UNSIGNED_BYTE, NULL);
				R_TexSetFilter(GL_TEXTURE_2D, pp_copyback_tex, GL_NEAREST, GL_NEAREST);
				R_TexSetWrap(GL_TEXTURE_2D, pp_copyback_tex, GL_CLAMP_TO_EDGE);
				pp_copyback_w = w;
				pp_copyback_h = h;
			}
			R_BindTextureForEdit (GL_TEXTURE_2D, pp_copyback_tex);
			R_TexCopySubImage2D(GL_TEXTURE_2D, pp_copyback_tex, 0, 0, 0, 0, 0, w, h);

			/* Apply warp+blur, writing the processed 3D back into the default framebuffer */
			R_BindFramebuffer(GL_FRAMEBUFFER, 0);
			R_SetViewport(0, 0, w, h);
			scale = r_scale.value;
			if (scale < 0.25f) scale = 0.25f;
			if (scale > 1.0f)  scale = 1.0f;
			PP_BlitWith3DEffects(pp_copyback_tex, w, h, warp, blur, scale);

			/* 2D will now draw on top of the already-warped 3D scene.
			 * Tell EndFrame to skip warp/blur and only apply gamma/FXAA. */
			pp_native_active = true;
		}
		return;
	}

	/* Resolve MSAA render buffer → pp_color_tex so the shader can sample it */
	if (pp_samples > 1)
	{
		R_BindFramebuffer(GL_READ_FRAMEBUFFER, pp_fbo);
		R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, pp_resolve_fbo);
		R_BlitFramebuffer(0, 0, pp_width, pp_height,
				   0, 0, pp_width, pp_height,
				   GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	/* Scene depth is final and pp_depth_tex is sampler-readable: build the
	 * Hi-Z pyramid that next frame's cull dispatch consumes (uhexen2-xd87). */
#ifndef USE_GLES
	R_BuildHiZForNextFrame();
#endif

	/* Restore native viewport */
	glwidth = pp_saved_glwidth;
	glheight = pp_saved_glheight;
	native_w = glwidth;
	native_h = glheight;

	/* Ensure composite FBO exists at native resolution */
	if (!PP_CreateNativeFBO(native_w, native_h))
	{
		pp_active = false;
		R_BindFramebuffer(GL_FRAMEBUFFER, 0);
		return;
	}

	/* Bind composite FBO — 2D will draw into this after we return */
	R_BindFramebuffer(GL_FRAMEBUFFER, pp_native_fbo);
	R_SetViewport(0, 0, native_w, native_h);

	/* Compute 3D-only effect strengths */
	warp = 0.0f;
	if (r_waterwarp.value && r_viewleaf && r_viewleaf->contents <= CONTENTS_WATER)
		warp = r_waterwarp.value;
	else if (cl.time < pp_waterwarp_preview_end)
		warp = r_waterwarp.value;	/* show preview even if not in water */
	blur = Cvar_VariableValue("r_motionblur");
	scale = r_scale.value;
	if (scale < 0.25f) scale = 0.25f;
	if (scale > 1.0f)  scale = 1.0f;

	if (warp > 0.0f || blur > 0.0f)
	{
		/* Shader blit: apply warp+blur, upscale to native res.
		 * Identity gamma/contrast/fxaa so only 3D effects are baked in. */
		PP_BlitWith3DEffects(pp_color_tex, native_w, native_h, warp, blur, scale);
	}
	else
	{
		/* Simple blit: upscale (or same-size copy) with no shader overhead */
		GLuint src = (pp_samples > 1) ? pp_resolve_fbo : pp_fbo;
		R_BindFramebuffer(GL_READ_FRAMEBUFFER, src);
		R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, pp_native_fbo);
		R_BlitFramebuffer(0, 0, pp_width, pp_height,
				   0, 0, native_w, native_h,
				   GL_COLOR_BUFFER_BIT, GL_NEAREST);
		R_BindFramebuffer(GL_FRAMEBUFFER, pp_native_fbo);
	}

	/* pp_native_fbo is now bound; 2D draws will composite on top of the 3D scene.
	 * EndFrame will read pp_native_color_tex and apply gamma/contrast/FXAA only. */
	pp_native_active = true;
}

void GL_PostProcess_EndFrame (void)
{
	GLuint blit_tex;
	int blit_w, blit_h;
	float lutmax;	/* grid size - 1 of the LUT bound for softemu (#80) */

	/* Drain any pending HUD glyph quads before we touch FBOs and shaders. */
	Draw_FlushCharBatch ();

	if (!pp_active)
		return;

	pp_active = false;

	/* Copyback fallback: no FBO, copy backbuffer to texture */
	if (pp_fbo_failed)
	{
		int w = pp_saved_glwidth;
		int h = pp_saved_glheight;
		if (!pp_copyback_tex || w != pp_copyback_w || h != pp_copyback_h)
		{
			if (pp_copyback_tex)
				R_DeleteTextures (1, &pp_copyback_tex);
			R_GenTextures(1, &pp_copyback_tex);
			R_BindTextureForEdit (GL_TEXTURE_2D, pp_copyback_tex);
			R_TexImage2D(GL_TEXTURE_2D, pp_copyback_tex, 0, GL_RGBA, w, h,
					GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			R_TexSetFilter(GL_TEXTURE_2D, pp_copyback_tex, GL_NEAREST, GL_NEAREST);
			R_TexSetWrap(GL_TEXTURE_2D, pp_copyback_tex, GL_CLAMP_TO_EDGE);
			pp_copyback_w = w;
			pp_copyback_h = h;
		}
		R_BindTextureForEdit (GL_TEXTURE_2D, pp_copyback_tex);
		R_TexCopySubImage2D(GL_TEXTURE_2D, pp_copyback_tex, 0, 0, 0, 0, 0, w, h);
		blit_tex = pp_copyback_tex;
		blit_w = w;
		blit_h = h;
		goto apply_shader;
	}

	/* determine which texture has the composited scene */
	if (pp_native_active)
	{
		/* End3D transferred 3D → composite FBO (with warp/blur already applied);
		 * 2D then drew on top — read the composite from here. */
		blit_tex = pp_native_color_tex;
		blit_w = pp_native_w;
		blit_h = pp_native_h;
	}
	else if (pp_samples > 1)
	{
		/* MSAA: resolve to pp_color_tex */
		R_BindFramebuffer(GL_READ_FRAMEBUFFER, pp_fbo);
		R_BindFramebuffer(GL_DRAW_FRAMEBUFFER, pp_resolve_fbo);
		R_BlitFramebuffer(0, 0, pp_width, pp_height,
				   0, 0, pp_width, pp_height,
				   GL_COLOR_BUFFER_BIT, GL_NEAREST);
		blit_tex = pp_color_tex;
		blit_w = pp_width;
		blit_h = pp_height;
	}
	else
	{
		blit_tex = pp_color_tex;
		blit_w = pp_width;
		blit_h = pp_height;
	}

	/* restore original viewport dimensions */
	glwidth = pp_saved_glwidth;
	glheight = pp_saved_glheight;

	/* Bloom post-process pass */
	if (PP_BloomOn () && bloom_fbo[0] && bloom_bright_prog)
	{
		GLuint bloom_src = pp_native_active ? pp_native_color_tex : pp_color_tex;
		int bloom_src_w = pp_native_active ? pp_native_w : pp_width;
		int bloom_src_h = pp_native_active ? pp_native_h : pp_height;
		int i;

		/* GL 4.3 core profile requires a VAO bound for glDrawArrays. */
		R_BindVertexArray(bloom_dummy_vao);

		/* Bright pass: extract overbright pixels */
		R_SetViewport(0, 0, bloom_w[0], bloom_h[0]);
		R_BindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[0]);
		R_UseProgram (bloom_bright_prog);
		R_BindTextureSlot (0, bloom_src);
		R_ProgramSamplerUnit(bloom_bright_prog, bloom_bright_loc_scene, "u_scene", 0);
		PP_PushBloomParams(1.0f / bloom_src_w, 1.0f / bloom_src_h, r_bloom_threshold.value);
		glDrawArrays_fp(GL_TRIANGLES, 0, 3);

		/* Downsample chain: level 0 → 1 → 2 → 3 */
		for (i = 1; i < BLOOM_LEVELS; i++)
		{
			R_SetViewport(0, 0, bloom_w[i], bloom_h[i]);
			R_BindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[i]);
			R_UseProgram (bloom_down_prog);
			R_BindTextureSlot (0, bloom_tex[i-1]);
			R_ProgramSamplerUnit(bloom_down_prog, bloom_down_loc_scene, "u_scene", 0);
			PP_PushBloomParams(1.0f / bloom_w[i-1], 1.0f / bloom_h[i-1], 0.0f);
			glDrawArrays_fp(GL_TRIANGLES, 0, 3);
		}

		/* Upsample + additive blend: level 3 → 2 → 1 → 0.
		 * The upsample shader outputs the tent-filtered coarser level
		 * only; GL_ONE/GL_ONE blending accumulates it into the
		 * downsampled content already in bloom_tex[i] via the ROP,
		 * avoiding the feedback loop that arose from sampling
		 * bloom_tex[i] in the shader while also rendering to it. */
		R_SetBlend (true);
		R_SetBlendFunc (GL_ONE, GL_ONE);
		for (i = BLOOM_LEVELS - 2; i >= 0; i--)
		{
			R_SetViewport(0, 0, bloom_w[i], bloom_h[i]);
			R_BindFramebuffer(GL_FRAMEBUFFER, bloom_fbo[i]);
			R_UseProgram (bloom_up_prog);
			R_BindTextureSlot (0, bloom_tex[i + 1]);
			R_ProgramSamplerUnit(bloom_up_prog, bloom_up_loc_scene, "u_scene", 0);
			PP_PushBloomParams(1.0f / bloom_w[i+1], 1.0f / bloom_h[i+1], 0.0f);
			glDrawArrays_fp(GL_TRIANGLES, 0, 3);
		}
		/* Back to the resting pair every other additive pass restores:
		 * blended draws that set no func of their own (the sky's front
		 * layer at r_skyalpha < 1) inherit it, and one bloom frame used to
		 * leave the sky additive until restart.  GitHub #547. */
		R_SetBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		R_SetBlend (false);

		R_BindVertexArray(0);
		/* bloom_tex[0] now contains the final bloom result at 1/2 scene res */
	}

	/* unbind scene FBO -- render to default framebuffer */
	R_BindFramebuffer(GL_FRAMEBUFFER, 0);

apply_shader:
	/* set full viewport for the blit */
	R_SetViewport(0, 0, glwidth, glheight);

	/* Before anything is bound: this may dispatch a compute pass and will
	 * clobber unit 0.  No-op unless the metric actually changed. */
	if ((int)r_softemu.value > 0)
		PP_UpdatePaletteLUT();

	/* disable depth test, blending, etc. for the blit */
	R_SetDepthTest (false);
	R_SetBlend (false);
	R_SetCull (false);

	/* set up orthographic projection for full-screen quad */
	GL_MatrixMode(GL_MAT_PROJECTION);
	GL_PushMatrix();
	GL_LoadIdentity();
	GL_Ortho(0, 1, 0, 1, -1, 1);
	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_PushMatrix();
	GL_LoadIdentity();

	/* bind composited scene texture */
	R_BindTextureSlot (0, blit_tex);
	R_TexSetFilter(GL_TEXTURE_2D, blit_tex, GL_NEAREST, GL_NEAREST);
	R_TexSetWrap(GL_TEXTURE_2D, blit_tex, GL_CLAMP_TO_EDGE);

	/* bind palette LUT on texture unit 1 if softemu is active */
	lutmax = (float)(PP_LUT_DIM - 1);
	if ((int)r_softemu.value > 0 && pp_lut_built)
	{
		/* mode 3 swaps in the colormap-derived LUT; the shader is identical
		 * either way, only the table it reads changes */
		qboolean cmap = ((int)r_softemu.value == 3 && pp_colormap_lut);
		GLuint lut = cmap ? pp_colormap_lut : pp_palette_lut;
		lutmax = (float)((cmap ? PP_CMAP_DIM : PP_LUT_DIM) - 1);
		R_BindTextureTarget (1, GL_TEXTURE_3D, lut);
	}

	/* activate shader and set all uniforms before drawing */
	R_UseProgram (pp_program);
	/* the PpVert MVP is taken from the current matrices in PP_PushParams */
	pp_params.p0[0] = v_gamma.value;	/* gamma */
	pp_params.p0[1] = PP_Contrast ();	/* contrast */
	pp_params.p3[1] = PP_HDRActive() ? r_hdr_exposure.value : 0.0f;	/* hdr_exposure */

	/* softemu uniforms */
	pp_params.i0[0] = (int)r_softemu.value;	/* softemu */
	pp_params.p0[3] = lutmax;		/* lutmax */
	{
		/* r_softemu_dither_screen scales the master strength, which is what
		 * upstream does with the same pair (Quake/gl_rmain.c: NOISESCALE *
		 * r_dither.value * r_softemu_dither_screen.value).  This is the FINE
		 * half of the split -- the screen dither upstream applies here in the
		 * postprocess rather than in the world shader.  Default 1.0, so the
		 * shipped behaviour is unchanged.  uhexen2-a5nn.3. */
		float d = r_dither.value * r_softemu_dither_screen.value;
		if (d < 0.0f)
			d = 0.0f;
		pp_params.p0[2] = d;		/* dither */
	}
	{
		float scale = r_scale.value;
		if (scale < 0.25f) scale = 0.25f;
		if (scale > 1.0f) scale = 1.0f;
		pp_params.p1[0] = scale;	/* scale */
	}

	/* Warp and blur: already baked into pp_native_color_tex when pp_native_active.
	 * Apply here only for the copyback fallback path (pp_native_active=false). */
	{
		float warp = 0;
		if (!pp_native_active)
		{
			extern mleaf_t *r_viewleaf;
			if (r_waterwarp.value && r_viewleaf &&
			    r_viewleaf->contents <= CONTENTS_WATER)
				warp = r_waterwarp.value;
			else if (cl.time < pp_waterwarp_preview_end)
				warp = r_waterwarp.value;	/* show preview even if not in water */
		}
		pp_params.p1[1] = warp;		/* waterwarp */
	}
	pp_params.p1[2] = (float)realtime;			/* time */
	pp_params.p1[3] = Cvar_VariableValue("gl_fxaa");	/* fxaa_on */
	pp_params.p2[0] = 1.0f / glwidth;			/* rcpFrame */
	pp_params.p2[1] = 1.0f / glheight;			/* rcpFrame.y */
	{
		float blur = 0, dy = 0, dp = 0;
		if (!pp_native_active)
		{
			float yaw   = cl.viewangles[1];
			float pitch = cl.viewangles[0];
			blur = Cvar_VariableValue("r_motionblur");
			dy = (yaw   - pp_prev_yaw)   * 0.0005f;
			dp = (pitch - pp_prev_pitch) * 0.0005f;

			/* position-based velocity: project movement onto screen axes */
			{
				vec3_t delta;
				float move_h, move_v;
				VectorSubtract(r_origin, pp_prev_origin, delta);
				move_h = DotProduct(delta, vright) * 0.0001f;
				move_v = DotProduct(delta, vup)    * 0.0001f;
				dy += move_h;
				dp += move_v;
				VectorCopy(r_origin, pp_prev_origin);
			}

			if (dy >  0.03f) dy =  0.03f; else if (dy < -0.03f) dy = -0.03f;
			if (dp >  0.03f) dp =  0.03f; else if (dp < -0.03f) dp = -0.03f;
			pp_prev_yaw   = yaw;
			pp_prev_pitch = pitch;
		}
		pp_params.p3[0] = blur;		/* motionblur */
		pp_params.p2[2] = dy;		/* viewdelta */
		pp_params.p2[3] = dp;		/* viewdelta.y */
	}

	/* bloom composite texture */
	if (pp_loc_bloom_tex >= 0)
	{
		R_BindTextureSlot (2, (PP_BloomOn () && bloom_tex[0]) ? bloom_tex[0] : 0);
		R_ProgramSamplerUnit(pp_program, pp_loc_bloom_tex, "u_bloom", 2);
	}
	pp_params.p3[2] = PP_BloomOn () ? r_bloom_intensity.value : 0.0f;	/* u_bloom_strength */
	PP_PushParams();

	/* draw full-screen quad using streaming VBO (shader already active) */
	GL_ImmBegin();
	GL_ImmColor4f(1, 1, 1, 1);
	GL_ImmTexCoord2f(0, 0); GL_ImmVertex2f(0, 0);
	GL_ImmTexCoord2f(1, 0); GL_ImmVertex2f(1, 0);
	GL_ImmTexCoord2f(1, 1); GL_ImmVertex2f(1, 1);
	GL_ImmTexCoord2f(0, 1); GL_ImmVertex2f(0, 1);
	GL_ImmDraw(GL_QUADS);	/* draw without changing shader */

	R_UseProgram (0);

	/* unbind palette LUT from unit 1 to avoid interfering with lightmap binds */
	if ((int)r_softemu.value > 0 && pp_lut_built)
	{
		R_BindTextureTarget (1, GL_TEXTURE_3D, 0);
	}

	/* restore matrices */
	GL_MatrixMode(GL_MAT_PROJECTION);
	GL_PopMatrix();
	GL_MatrixMode(GL_MAT_MODELVIEW);
	GL_PopMatrix();

	/* re-enable depth test (it's normally on) */
	R_SetDepthTest (true);
}

qboolean GL_PostProcess_Active (void)
{
	return pp_active;
}

/* Active is "post-processing ran this frame"; Available is "the shader linked
 * and the chain could run at all".  `renderer_status` wants the second.  */
qboolean GL_PostProcess_Available (void)
{
	return pp_initialized;
}

void GL_PostProcess_RequestWaterwarpPreview (float duration)
{
	pp_waterwarp_preview_end = cl.time + duration;
}

void GL_PostProcess_ResetWaterwarpPreview (void)
{
	pp_waterwarp_preview_end = -1.0f;  /* Clear preview timer on level change */
}
