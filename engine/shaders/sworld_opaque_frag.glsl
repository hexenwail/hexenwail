/* sworld_opaque_frag -- sworld_frag's opaque-only twin: early_fragment_tests
 * and no discard.  Linked by gl_shader_world_opaque, bound for batches that
 * contain no fence/holey surfaces -- the world MDI pass, the brush-entity MDI
 * opaque pass, the DrawTextureChains fast path and the R_DrawBrushModel fast
 * path -- which is most of the world.  Recovers the Hi-Z benefit that was lost
 * in uhexen2-238u without re-introducing the fence-occludes-tree bug.
 * uhexen2-5c6r.  Moved from gl_shader.c's sworld_frag_opaque (#450, world
 * slice W2); the file is sworld_opaque_frag because EmbedShaders.cmake names
 * a stage by its _frag suffix.
 *
 * No #version line; gl_shader.c passes GLSL_WORLD_FRAG_HEADER, as for
 * sworld_frag.
 *
 * SAME BLOCKS AS sworld_frag, deliberately: the two are switched between by
 * glUseProgram and no call site knows which it has.  That used to mean the
 * same loose uniform names and types, re-uploaded per program; now it means
 * the same FragParams and WorldFrag declarations (uniforms.inc, world.inc) at
 * the same bindings, and every world draw path writes the whole of both
 * before it draws (R_SetWorldFrag), whichever program it bound.  This stage
 * reads a subset: no liquid warp (no liquid reaches it) and no alpha pair (no
 * discard, and its output alpha is always 1).  Most of the world draws
 * through THIS one, so leaving softemu out of it would have meant the modes
 * reached only fences and brush ents.  uhexen2-a5nn.3.
 */
#include "layout.inc"
#define UNIFORMS_FRAG
#include "uniforms.inc"
#include "world.inc"

#ifndef GL_ES
/* GLSL ES 3.00 has no early_fragment_tests.  Safe here because nothing in this
 * stage discards; see sworld_frag for why its twin must never have it. */
layout(early_fragment_tests) in;
#endif

FRAG_TEX(0) uniform sampler2D u_texture0;   /* diffuse */
FRAG_TEX(1) uniform sampler2D u_texture1;   /* lightmap atlas */
FRAG_TEX(2) uniform sampler2D u_texture2;   /* fullbright mask */
FRAG_TEX(3) uniform sampler2D u_texture3;   /* normal map, flat sentinel when absent (uhexen2-mfql) */
FRAG_TEX(4) uniform sampler2D u_texture4;   /* gloss map, black sentinel when absent (uhexen2-mfql) */
#ifndef GL_ES
/* Desktop only, as in sworld_frag. */
FRAG_TEX(5) uniform usampler3D u_lightgrid;  /* per-froxel light bitmasks */
FRAG_TEX(6) uniform sampler2D u_lmlayer1;
FRAG_TEX(7) uniform sampler2D u_lmlayer2;
FRAG_TEX(8) uniform sampler2D u_lmlayer3;
#endif

FRAG_IN(0) in highp vec2 v_texcoord;    /* highp: see sworld_vert (uhexen2-mfql) */
FRAG_IN(1) in highp vec2 v_lmcoord;     /* see sworld_frag */
FRAG_IN(2) in vec4 v_color;
FRAG_IN(3) in float v_fogdist;
FRAG_IN(4) in vec2 v_worldxy;
FRAG_IN(5) in highp vec3 v_eyepos;      /* highp: see sworld_vert (uhexen2-mfql) */
FRAG_OUT(0) out vec4 fragColor;

#include "bicubic_lm.inc"
#include "caustics.inc"
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
    highp vec2 tc = v_texcoord;     /* see sworld_frag (#176) */
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
    float lodbias = (u_softemu.w > 0.0) ? ((u_softemu.y > 0.0) ? -1.0 : -0.5) : 0.0;
    vec4 tex = texture(u_texture0, tc, lodbias);
    vec2 lmsize = vec2(textureSize(u_texture1, 0)) * 16.0;
    highp vec2 lmuv = lmc;
    if (u_softemu.w > 0.0)
        lmuv = (floor(lmuv * lmsize) + 0.5) / lmsize;
    vec4 lm = (u_lightmap_bicubic > 0.5)
        ? BicubicLightmap(u_texture1, lmuv)
        : texture(u_texture1, lmuv);
#ifndef GL_ES
    /* Before banding and the debug views; see sworld_frag. */
    if (u_lmstyles.x > 0.5) lm = LMStyles(lm, lmuv);
    if (u_dlight_scale > 0.0) lm.rgb = ClusteredDlight(lm.rgb, v_eyepos);
#endif
    if (u_softemu.y > 0.0)
        lm.rgb = floor(lm.rgb * 63.0 + 0.5) * (1.0/63.0);
    lm.rgb = mix(lm.rgb, vec3(1.0), u_lightdebug.x);    /* uhexen2-isq7 */
    tex.rgb = mix(tex.rgb, vec3(1.0), u_lightdebug.y);
    /* Material maps.  Branch is uniform-static and therefore coherent across
     * the whole draw, so a map with no _norm/_bump/_gloss anywhere -- or a
     * user with r_materialmaps 0 -- pays one scalar compare per fragment and
     * skips the derivatives, the frame build and both texture fetches.
     * uhexen2-mfql. */
    vec3 matspec = vec3(0.0);
    if (u_material.x > 0.0 || u_material.y > 0.0) {
        vec4 m = MaterialShade(u_texture3, u_texture4, tc,
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
    vec3 fb = texture(u_texture2, tc).rgb * (1.0 - u_lightdebug.y);
    color.rgb += fb;
    color.rgb += matspec;
    if (u_caustics.x > 0.0) {
        float c = Caustics(v_worldxy, u_caustics.y);
        color.rgb += color.rgb * c * u_caustics.x;
    }
    float fogfac = u_fog_density * v_fogdist;
    float fog = exp(-fogfac * fogfac);
    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));
    /* The DITHER == 1 tail; see sworld_frag for what the two noises are for. */
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
    fragColor = vec4(color.rgb, 1.0);
}
