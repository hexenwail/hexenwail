/* sworld_frag -- the world fragment stage: textured + lightmapped, fogged,
 * with the alpha-test discard fences and holey textures need.  Linked by
 * gl_shader_world (the cutout program: fences, underwater-fallback batches,
 * brush-entity fence passes, lit water and the GL_ImmEnd world batches) and
 * gl_shader_world_oit (the same, inside an OIT pass).  sworld_opaque_frag.glsl
 * is its early-Z twin for batches with nothing to discard.  Moved from
 * gl_shader.c's sworld_frag (#450, world slice W2); the vertex stage is
 * sworld_vert.glsl (slice W1), which fills the varyings below by name (the
 * plain arm of FRAG_IN emits no location) and, for SPIR-V, by location.
 *
 * No #version line; gl_shader.c passes GLSL_WORLD_FRAG_HEADER, the tier's
 * GLSL_FRAG_HEADER plus the froxel-grid sizes on desktop.  The OIT twin adds
 * "#define OIT 1", which swaps the colour output for oit.inc's outputs and
 * main() wrapper (#450 W3).
 *
 * NO early_fragment_tests HERE, and none may be added.  Early tests run depth
 * and stencil -- and write them -- BEFORE the fragment shader executes, and a
 * later `discard` cannot undo the depth write that already happened.  For an
 * alpha-tested fence (e.g. a func_illusionary bush), every cutout pixel still
 * wrote the bush's depth, occluding any entity drawn after at that pixel even
 * though no color was written there.  Visible on mill.bsp (SoT): bush
 * silhouette z-rejected the tree behind it.  uhexen2-238u.  The opaque twin
 * has no discard and takes them.
 *
 * The non-opaque uniforms that were loose are block members now: fog, the
 * alpha pair, overbright, bicubic, the caustics and the debug views in
 * FragParams (uniforms.inc), the material, warp, span cell, software
 * emulation, lightstyle mode and froxel lookup in WorldFrag (world.inc).  The
 * aliases keep the old names, so the body below is the literal's body.
 * sworld_opaque_frag reads the same blocks at the same bindings, so a call
 * site can switch between the two programs without writing anything
 * different: every world draw path writes the whole set (R_SetWorldFrag).
 */
#include "layout.inc"
#define UNIFORMS_FRAG
#include "uniforms.inc"
#include "world.inc"

FRAG_TEX(0) uniform sampler2D u_texture0;   /* diffuse */
FRAG_TEX(1) uniform sampler2D u_texture1;   /* lightmap atlas */
FRAG_TEX(2) uniform sampler2D u_texture2;   /* fullbright mask (uhexen2-sjvf) */
FRAG_TEX(3) uniform sampler2D u_texture3;   /* normal map, flat sentinel when absent (uhexen2-mfql) */
FRAG_TEX(4) uniform sampler2D u_texture4;   /* gloss map, black sentinel when absent (uhexen2-mfql) */
#ifndef GL_ES
/* Clustered dynamic lighting (uhexen2-26bm) and GPU lightstyles (#439),
 * desktop only: both read the light-cluster SSBO, which the ES tier does not
 * have.  This was the GLSL_DLIGHT_DECL / GLSL_LMSTYLES_DECL splice, whose ES
 * arms were empty; GL_ES, which the ES compiler predefines, makes the same
 * choice. */
FRAG_TEX(5) uniform usampler3D u_lightgrid;  /* per-froxel light bitmasks */
FRAG_TEX(6) uniform sampler2D u_lmlayer1;
FRAG_TEX(7) uniform sampler2D u_lmlayer2;
FRAG_TEX(8) uniform sampler2D u_lmlayer3;
#endif

FRAG_IN(0) in highp vec2 v_texcoord;    /* highp: see sworld_vert (uhexen2-mfql) */
FRAG_IN(1) in highp vec2 v_lmcoord;     /* highp: SpanUV extrapolates it across a cell (#176) */
FRAG_IN(2) in vec4 v_color;
FRAG_IN(3) in float v_fogdist;
FRAG_IN(4) in vec2 v_worldxy;
FRAG_IN(5) in highp vec3 v_eyepos;      /* highp: see sworld_vert (uhexen2-mfql) */
#ifdef OIT
#define OIT_TEX0 9
#define OIT_UBO 2
#include "oit.inc"
#else
FRAG_OUT(0) out vec4 fragColor;
#endif

#include "bicubic_lm.inc"
#include "caustics.inc"
#include "turb_uv.inc"
#include "span_uv.inc"
#include "softemu_noise.inc"
#include "material.inc"
#ifndef GL_ES
#define LIGHTGRID_SSBO_SLOT 9
#include "lightgrid.inc"
#include "world_dlight.inc"
#include "lmstyles.inc"
#endif

void main() {
    /* The 1997 spans (#176) replace both coordinates, as the surface cache
     * baked light into the texture before the span loop ever sampled it. */
    highp vec2 tc = v_texcoord;
    highp vec2 lmc = v_lmcoord;
    if (u_spans > 0.0) {
        tc = SpanUV(v_texcoord, u_spans);
        lmc = SpanUV(v_lmcoord, u_spans);
        /* D_DrawSpans8 clamped s/t to the surface's extents; a shader cannot
         * see the face's lightmap rect.  Inside a cell the linear coordinate
         * overshoots the perspective one by up to half the cell's span, which
         * at a face edge would read past LM_GUTTER (gl_rsurf.c) into the next
         * face's light.  So the lightmap may depart from its perspective
         * coordinate by at most LM_GUTTER - 0.5 luxels (bilinear reads half a
         * luxel further, bicubic a whole one: 1.0 then); the texture, which
         * tiles, keeps the full span. */
        highp vec2 lmlim = ((u_lightmap_bicubic > 0.5) ? 1.0 : 1.5) / vec2(textureSize(u_texture1, 0));
        lmc = v_lmcoord + clamp(lmc - v_lmcoord, -lmlim, lmlim);
    }
    /* Lit water rides this program (uhexen2-a5nn.2), so the liquid warp has
     * to live here too.  u_turb.x is 0 for every other draw -- only
     * EmitWaterPolys raises it, and only under r_water_pixel_warp 1 -- so uv
     * is v_texcoord untouched for the whole world.  Everything keyed to the
     * diffuse texture space follows uv; the lightmap deliberately does not,
     * because warping the light along with the texture would drag the
     * shoreline shading around with the ripple. */
    highp vec2 uv = tc;
    if (u_turb.x > 0.0) uv = TurbUV(tc, u_turb);
    /* Negative LOD bias under softemu, as upstream: the 1997 rasteriser had
     * no trilinear blend, so biasing toward the sharper mip is closer to it
     * than letting the hardware pick.  -1 where the lightmap is banded too,
     * -0.5 otherwise; 0 (a no-op) when softemu is off. */
    float lodbias = (u_softemu.w > 0.0) ? ((u_softemu.y > 0.0) ? -1.0 : -0.5) : 0.0;
    vec4 tex = texture(u_texture0, uv, lodbias);
    /* Snap the lightmap coordinate to the luxel grid at 1/16 texel, which is
     * the resolution the software renderer interpolated light at.  Without
     * it the banded lightmap below still fades smoothly between luxels and
     * the effect reads as a posterised gradient rather than as bands. */
    vec2 lmsize = vec2(textureSize(u_texture1, 0)) * 16.0;
    highp vec2 lmuv = lmc;
    if (u_softemu.w > 0.0)
        lmuv = (floor(lmuv * lmsize) + 0.5) / lmsize;
    vec4 lm = (u_lightmap_bicubic > 0.5)
        ? BicubicLightmap(u_texture1, lmuv)
        : texture(u_texture1, lmuv);
#ifndef GL_ES
    /* GPU lightstyles (#439), then clustered dynamic lights, added to the
     * static lightmap sample before anything downstream sees it -- the CPU
     * path they replace baked into exactly this value, so banding, the debug
     * views, overbright and the material weighting all have to keep acting on
     * the sum.  uhexen2-26bm. */
    if (u_lmstyles.x > 0.5) lm = LMStyles(lm, lmuv);
    if (u_dlight_scale > 0.0) lm.rgb = ClusteredDlight(lm.rgb, v_eyepos);
#endif
    /* 64 light levels, which is what colormap.lmp had.  Upstream folds its
     * overbright doubling into the same expression; ours stays separate in
     * u_overbright, so the scale here is 1/63 rather than 2/63. */
    if (u_softemu.y > 0.0)
        lm.rgb = floor(lm.rgb * 63.0 + 0.5) * (1.0/63.0);
    /* r_fullbright / r_lightmap.  Applied to the samples rather than to the
     * final colour so everything downstream -- overbright, the fullbright
     * mask, caustics, fog, the alpha test -- keeps seeing a well-formed
     * value.  rgb only on the diffuse: tex.a still gates the alpha test, so
     * r_lightmap must not turn a fence into a solid sheet.  uhexen2-isq7. */
    lm.rgb = mix(lm.rgb, vec3(1.0), u_lightdebug.x);
    tex.rgb = mix(tex.rgb, vec3(1.0), u_lightdebug.y);
    /* Sample the fullbright mask BEFORE the alpha-test discard.  texture()
     * uses implicit dFdx/dFdy to pick the mip level; derivatives are
     * undefined in a 2x2 quad where some lanes have already discarded, so
     * sampling after discard produces dark outlines along fence/holey
     * edges (Ironwail 017fdd2).  Add fullbright contribution: palette-
     * index >= vid.fullbright pixels in the diffuse texture get rendered
     * at full intensity regardless of lightmap.  For surfaces with no
     * fullbright pixels the engine binds a 1x1 black sentinel at unit 2
     * so the sample contributes 0.  uhexen2-9a1l. */
    vec3 fb = texture(u_texture2, uv).rgb * (1.0 - u_lightdebug.y);
    /* Material maps.  Branch is uniform-static and therefore coherent across
     * the whole draw, so a map with no _norm/_bump/_gloss anywhere -- or a
     * user with r_materialmaps 0 -- pays one scalar compare per fragment and
     * skips the derivatives, the frame build and both texture fetches.
     * uhexen2-mfql. */
    vec3 matspec = vec3(0.0);
    if (u_material.x > 0.0 || u_material.y > 0.0) {
        vec4 m = MaterialShade(u_texture3, u_texture4, uv,
                               v_eyepos, u_material.xy, u_material.z);
        /* Weight the highlight by the lightmap BEFORE the relief term is
         * folded into it.  Relief redistributes the light that is already
         * arriving; applying it to the specular as well would count it twice
         * and make lit bumps bloom.  The r_lightmap debug view
         * (u_lightdebug.y) drops the highlight entirely, for the same reason
         * it drops the fullbright add -- that view is meant to show lighting
         * alone. */
        matspec = m.yzw * lm.rgb * (1.0 - u_lightdebug.y);
        lm.rgb *= m.x;
    }
    vec4 color = tex * lm * v_color;
    color.rgb *= u_overbright;      /* Ironwail-style overbright (uhexen2-f29y) */
    if (color.a < u_alpha_threshold) discard;
    color.rgb += fb;
    color.rgb += matspec;
    /* Underwater caustics: gated by u_caustics.x (set to 0 by C when the
     * view leaf is not CONTENTS_WATER or the cvar is off, otherwise to
     * r_caustics_intensity).  Applied as a brightness multiplier so dark
     * areas still receive a visible highlight band.  uhexen2-6bfm. */
    if (u_caustics.x > 0.0) {
        float c = Caustics(v_worldxy, u_caustics.y);
        color.rgb += color.rgb * c * u_caustics.x;
    }
    float fogfac = u_fog_density * v_fogdist;
    float fog = exp(-fogfac * fogfac);
    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));
    /* Texture-space dither.  Two noises blended by how much world space one
     * pixel covers: up close, white noise keyed to the LIGHTMAP coordinate,
     * so the grain sticks to the surface the way the software renderer's
     * per-luxel quantisation did; far away, where that grain would alias
     * into a shimmer, an ordered screen-space pattern instead.  Both are
     * applied in gamma space (the sqrt/square pair) because that is where
     * the palette quantisation downstream happens.  Ironwail
     * Quake/gl_shaders.h, the DITHER == 1 tail of its world shader.
     *
     * fwidth of the EYE-space position, not the world one upstream uses:
     * eye space is a rigid transform of world space, so the magnitude this
     * asks for is the same, and v_eyepos is the position this shader
     * actually has for a brush entity. */
    if (u_softemu.x > 0.0 || u_softemu.z > 0.0) {
        vec3 dpos = fwidth(v_eyepos);
        float farblend = clamp(max(dpos.x, max(dpos.y, dpos.z)) * 0.5 - 0.125, 0.0, 1.0);
        farblend *= farblend;
        color.rgb = sqrt(max(color.rgb, vec3(0.0)));
        float luma = dot(color.rgb, vec3(0.25, 0.625, 0.125));
        float nearnoise = tri(whitenoise01(lmuv * lmsize)) * luma * u_softemu.x;
        float farnoise = (u_fog_density > 0.0) ? SCREEN_NOISE() * u_softemu.z : 0.0;
        color.rgb += mix(nearnoise, farnoise, farblend);
        color.rgb *= color.rgb;
    }
    /* For cutout alpha-test (threshold > 0.5 = fence/holey, A2C enabled):
     * surviving fragments are by definition opaque, so force alpha=1 to
     * stop A2C from dithering their coverage based on the noisy
     * lightmap.a x tex.a x v_color.a multiply.
     * For non-cutout draws (threshold ~ 0.01): preserve actual alpha so
     * GL_BLEND with GL_SRC_ALPHA works for translucent surfaces.
     * OIT path always preserves alpha for weighted blending.
     * uhexen2-khsa r13: u_force_opaque_alpha replaces the threshold test.
     * See salias_frag for rationale. */
#ifdef OIT
    fragColor = color;
#else
    fragColor = vec4(color.rgb, u_force_opaque_alpha > 0.5 ? 1.0 : color.a);
#endif
}
