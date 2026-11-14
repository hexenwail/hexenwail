/* salias_frag -- the alias (model) fragment stage: textured, vertex-coloured,
 * fogged, with the per-batch effects the sprite, particle, warp-poly and
 * unlit-brush-poly batches that share gl_shader_alias switch on and off.
 * Moved from gl_shader.c's salias_frag (#450, slice S2 of the alias-family
 * plan on that issue).
 *
 * Linked by all ten alias-family programs: alias, alias_np, both OIT twins,
 * skeletal x4 and alias_instanced x2.  The vertex stages are salias_vert.glsl,
 * and sskeletal_vert / salias_inst_vert, still C literals in gl_shader.c; all
 * three fill the varyings below by name (the plain arm of FRAG_IN emits no
 * location).
 *
 * No #version line; gl_shader.c passes GLSL_ALIAS_FRAG_HEADER, the tier's
 * GLSL_FRAG_HEADER plus the froxel-grid sizes on desktop, with
 * "#define NOPERSP 1" appended for the affine (r_softemu) twins.  The OIT twins
 * go through GL_CompileOITFragShaderSplit, which drops the line holding the
 * colour output's declaration and splices its own outputs in.
 *
 * The non-opaque uniforms that were loose are block members now: fog, the
 * alpha pair, the caustics and the soft-particle parameters in FragParams
 * (uniforms.inc), the warp, span cell, material, light direction and froxel
 * lookup in AliasFrag (alias.inc).  The aliases keep the old names, so the
 * body below is the literal's body.
 */
#include "layout.inc"
#define UNIFORMS_FRAG
#include "uniforms.inc"
#include "alias.inc"

/* Affine texture mapping, the way the software rasteriser did it
 * (uhexen2-ktjv).  An interpolation qualifier is not something a uniform can
 * switch, so this is a compile-time variant -- the only stage of r_softemu
 * that needs one.  Empty object-like macro when NOPERSP is not defined, so the
 * qualifier simply is not there.  The ES tier never defines it. */
#ifdef NOPERSP
#define TCQUAL noperspective
#else
#define TCQUAL
#endif

FRAG_TEX(0) uniform sampler2D u_texture0;
/* Soft particles (uhexen2-mf9u).  Explicitly highp: GLSL_FRAG_HEADER defaults
 * the ES tier to mediump, whose 10-bit mantissa cannot tell two window-space
 * depths apart, and the whole fade is a difference of two of them.  Desktop
 * GLSL accepts the qualifier and ignores it. */
FRAG_TEX(1) uniform highp sampler2D u_soft_depth;
/* Material maps on model skins (uhexen2-4kcb): the normal map, a flat
 * sentinel when absent, and the gloss map, a black sentinel when absent.  Their
 * parameters, u_alias_material and u_alias_lightdir, are named apart from the
 * world's u_material because this program is also the sprite / particle /
 * warp-poly / unlit-brush-poly program and its value is per-batch state.  The
 * light direction is in the same frame as v_matpos: C rotates it per frame
 * per program, eye space for this program and the skeletal one, world space
 * for the instanced one.  See R_AliasMaterialLightDir. */
FRAG_TEX(2) uniform sampler2D u_texture3;
FRAG_TEX(3) uniform sampler2D u_texture4;

FRAG_IN(0) TCQUAL in vec2 v_texcoord;
FRAG_IN(1) in vec4 v_color;
FRAG_IN(2) in float v_fogdist;
FRAG_IN(3) in vec2 v_worldxy;
/* Camera-relative position for the material tangent frame.  Deliberately NOT
 * v_eyepos: that one exists only on the desktop tier, and on the instanced
 * path it is eye space via u_lightview -- a matrix only gl_lightcluster.c
 * uploads, and only when clustered lighting is live.  This one is filled
 * unconditionally by all three vertex shaders, each in its own native frame.
 * uhexen2-4kcb. */
FRAG_IN(4) in highp vec3 v_matpos;
#ifndef GL_ES
/* Clustered dynamic lighting on alias models (uhexen2-waum), desktop only: the
 * ES tier has no SSBO and no compute pass to fill the grid, and an `in` with no
 * matching `out` would be a link error there.  This was the
 * GLSL_ALIAS_DLIGHT_FS_IN / _DECL / _FN splice; GL_ES, which the ES compiler
 * predefines, now makes the same choice. */
FRAG_IN(5) in highp vec3 v_eyepos;
FRAG_IN(6) flat in float v_dlightscale;
FRAG_TEX(4) uniform usampler3D u_lightgrid;
#endif

FRAG_OUT(0) out vec4 fragColor;

#include "caustics.inc"
#include "turb_uv.inc"
#include "span_uv.inc"
#include "material.inc"
#ifndef GL_ES
#define LIGHTGRID_SSBO_SLOT 5
#include "lightgrid.inc"
#include "alias_dlight.inc"
#endif

void main() {
    /* Per-pixel liquid warp (uhexen2-9o7u).  Off unless C sets u_turb.x,
     * which only EmitWaterPolys does, and only under r_water_pixel_warp 1 --
     * every other user of this program (models, sprites, brush polys) leaves it
     * 0.  A lit liquid face takes the world program instead of this one and
     * has to ripple identically, so the two share one TurbUV.  uhexen2-a5nn.2. */
    highp vec2 uv = v_texcoord;
    /* The brush polys and unlit liquids this program draws were spans in 1997
     * too (D_DrawSpans8, Turbulent8); models were not, and leave it 0.  Never
     * set on the NOPERSP twins, whose texcoord is already affine. */
    if (u_spans > 0.0) uv = SpanUV(v_texcoord, u_spans);
    if (u_turb.x > 0.0) uv = TurbUV(uv, u_turb);
    /* Point-sample one mip, offset half a texel.  The qualifier alone is not
     * the look: the rasteriser had no mip chain and no bilinear blend, and
     * upstream's variant takes both away too (Ironwail gl_shaders.h, the
     * ALIASSHADER_NOPERSP branch).  uhexen2-ktjv. */
#ifdef NOPERSP
    uv -= 0.5 / vec2(textureSize(u_texture0, 0));
    vec4 tex = textureLod(u_texture0, uv, 0.0);
#else
    vec4 tex = texture(u_texture0, uv);
#endif
#ifndef GL_ES
    vec3 lit = v_color.rgb;
    if (v_dlightscale > 0.0) lit = ClusteredAliasDlight(lit, v_eyepos, v_dlightscale);
    vec4 color = vec4(tex.rgb * lit, tex.a * v_color.a);
#else
    /* Exactly the desktop arm with the grid off, so the tiers stay provably
     * identical when the path is not in use. */
    vec4 color = tex * v_color;
#endif
    /* Material maps on the skin (uhexen2-4kcb).  Uniform-static branch, so a
     * model with no _norm/_bump/_gloss sidecar -- or r_materialmaps 0, or any
     * of the sprite / particle / warp / brush batches that share this program
     * and leave the member at its resting zero -- skips the derivatives, the
     * frame build and both texture fetches.
     *
     * Runs BEFORE the alpha-test discard, for the same reason sworld_frag
     * samples its fullbright mask early: this reads textures and takes
     * derivatives, and both are undefined in a 2x2 quad where some lanes have
     * already discarded.  Sampling after the discard draws dark outlines along
     * every cutout edge (Ironwail 017fdd2). */
    vec3 matspec = vec3(0.0);
    if (u_alias_material.x > 0.0 || u_alias_material.y > 0.0) {
        vec4 m = MaterialShadeDir(u_texture3, u_texture4, uv, v_matpos,
                                  u_alias_lightdir,
                                  u_alias_material.xy, u_alias_material.z);
        /* Weight the highlight by the light that reaches the model, not by
         * the shaded colour, so a bright skin does not get a brighter
         * highlight than a dark one under the same lamp.  v_color.rgb is this
         * path's analogue of the lightmap sworld_frag weights by: the static
         * light with shadedots and the entity tint already folded in. */
        matspec = m.yzw * v_color.rgb;
        color.rgb *= m.x;
    }
    /* uhexen2-khsa r20: revert r15's threshold gate.  r15 only ran the
     * discard for u_alpha_threshold > 0.5, exempting opaque batches.
     * Confirmed via bisect (bobberb): some alias models route through the
     * opaque path (threshold=0.01) yet have palette-255 cutout texels in their
     * skin -- trees, viewmodel hands, etc.  r14's unconditional test caught
     * them; r15's gate let them render with the cutout texels' RGB
     * (black/white/teal depending on upload path), which looked like the
     * cutout area was filled in.  Restore the r14 behavior. */
    if (color.a < u_alpha_threshold) discard;
    /* Added rather than multiplied, and after the alpha test, exactly where
     * sworld_frag adds its own.  uhexen2-4kcb. */
    color.rgb += matspec;
    /* Underwater caustics, same formula and same pre-fog position in the chain
     * as sworld_frag so a model and the floor it stands on receive the
     * identical highlight band.  v_worldxy is world space on every vertex
     * shader that links against this fragment shader.  Gated by
     * u_alias_caustics.x, which C leaves at 0 for every non-alias batch that
     * shares this program.  uhexen2-0gn3. */
    if (u_alias_caustics.x > 0.0) {
        float c = Caustics(v_worldxy, u_alias_caustics.y);
        color.rgb += color.rgb * c * u_alias_caustics.x;
    }
    float fogfac = u_fog_density * v_fogdist;
    float fog = exp(-fogfac * fogfac);
    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));
    /* Soft particles (uhexen2-mf9u).  A view-parallel billboard whose origin
     * sits on an impact surface has up to half its quad behind that surface at
     * any oblique view angle; depth-testing it per fragment slices the sprite
     * along a hard line.  Fade alpha with the distance to whatever opaque
     * geometry is behind instead, so it dissolves into the wall.
     *
     * u_soft_params.yz linearize a window-space depth to a positive view-space
     * distance as z = b / (a - d).  C derives a and b from the live projection
     * matrix with glDepthRange folded in, so the expression is identical for
     * the reversed-Z and forward-Z conventions and for the mirror pass's split
     * depth range.
     *
     * Deliberately after the alpha test: the cutout decision belongs to the
     * sprite's own texel alpha, and a texel faded to near-zero by proximity
     * must still blend rather than discard.  Gated by .x, which every
     * non-sprite user of this program leaves at 0. */
    if (u_soft_params.x > 0.0) {
        highp float dscene = texelFetch(u_soft_depth, ivec2(gl_FragCoord.xy), 0).r;
        highp float zscene = u_soft_params.z / (u_soft_params.y - dscene);
        highp float zfrag  = u_soft_params.z / (u_soft_params.y - gl_FragCoord.z);
        color.a *= clamp((zscene - zfrag) * u_soft_params.x, 0.0, 1.0);
    }
    /* uhexen2-khsa r13: u_force_opaque_alpha replaces the old threshold-based
     * test.  Threshold > 0.5 was correct for fence cutouts but caught
     * opaque-as-translucent ents (CASTLE_TR.MDL etc.) that need an output
     * alpha of 1.0 to avoid bleeding a garbage alpha into FB.a where any
     * downstream pass (OIT compositor, post-process) could read it as a
     * transparency key.  C sets force_opaque_alpha=1 in confirmed-opaque alias
     * paths and =0 for ENTALPHA / DRF_TRANSLUCENT which legitimately need
     * blend-stage src.a from the shader. */
#ifdef OIT
    fragColor = color;
#else
    fragColor = vec4(color.rgb, u_force_opaque_alpha > 0.5 ? 1.0 : color.a);
#endif
}
