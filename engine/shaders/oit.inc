/* oit.inc -- the order-independent-transparency output stage, for the OIT
 * twin of a fragment stage (#450 W3; it was gl_shader.c's oit_preamble, which
 * C spliced in place of the stage's colour output).
 *
 * A stage that has an OIT twin includes this INSTEAD of declaring its colour
 * output, under #ifdef OIT, with the declaration in the #else arm; the twin is
 * compiled with "#define OIT 1" in its header chunk (GL_InitOITProgramSplit
 * appends it).  Before including it the stage defines
 *
 *     OIT_TEX0  its sampler count
 *     OIT_UBO   its uniform-block count
 *
 * the SDL_GPU slots this file's samplers and block take, in set 2 and set 3:
 * after the stage's own, because SDL_GPU numbers a stage's resources from 0
 * with no gaps.  The light buffer, which follows the samplers in set 2, moves
 * up by OIT_TEX_COUNT on its own (lightgrid.inc).  The GL arms ignore every
 * one of these numbers.
 *
 * The stage's main() is renamed main_body() and this file's main() wraps it
 * (the rename trick from Ironwail).  fragColor is a plain global here rather
 * than an output, initialised rather than merely declared: main() reads it
 * after calling main_body(), which is only a prototype at that point, so the
 * compiler cannot see the assignment inside and correctly reports a read of an
 * uninitialised global -- three GL_DEBUG_SEVERITY_HIGH lines on every cold
 * start.  More than noise: this wrapper is only correct because every stage it
 * wraps assigns fragColor on every non-discard path, and nothing enforces
 * that.  An early `return;` added to one of them later would yield undefined
 * colour in OIT mode only.  Seeding it makes that case transparent black
 * instead of garbage.  uhexen2-nkvm.
 *
 * u_oit_mode picks what the wrapped stage writes (GitHub #87):
 *   0  weighted blended OIT -- accum, revealage (r_oit 1)
 *   1  moment-based OIT, moment pass -- absorbance b0 and its four
 *      depth-weighted power moments (r_oit 2)
 *   2  moment-based OIT, composite pass -- colour weighted by the
 *      transmittance reconstructed from those moments at this depth
 * One program per stage, branched on a block member, rather than a program
 * per mode: every per-frame value the engine writes for an OIT program
 * (caustics, overbright, light clusters, ...) then reaches all three modes
 * with no extra plumbing.
 *
 * The mode and the moment targets' multisampling are OitFrag's, GL binding
 * point 13 (UBO_BINDING_OIT_FRAG), assigned from C because ES 3.00 cannot put
 * layout(binding=) on a block -- not that ES ever compiles this: OIT is
 * desktop-only (no glBlendFunci in WebGL2), so this file is skipped on that
 * tier and costs WebGL2's fragment-block budget nothing.  One ivec4, mirrored
 * by r_oitfrag_t in gl_uniforms.h.  Written only by R_SetOITMode, at each OIT
 * pass transition (gl_postprocess.c), before the pass draws: per-pass state
 * with one writer, which every OIT program reads alike.  The samplers stay
 * loose; C points them at MBOIT_TMU_* once, at link.
 *
 * The moment maths is vkQuake's Shaders/mboit.inc (Muenstermann et al.,
 * "Moment-Based Order-Independent Transparency", I3D 2018), with two
 * departures: the moments come from texelFetch on the moment targets rather
 * than Vulkan subpass inputs, and depth is warped from view distance
 * (1/gl_FragCoord.w, valid whatever gl_reversed_z says) where vkQuake reads
 * its reversed-Z gl_FragCoord.z.
 */
#ifndef OIT_INC
#define OIT_INC

#if !defined(OIT_TEX0) || !defined(OIT_UBO)
#error oit.inc needs OIT_TEX0 and OIT_UBO, the stage sampler and block counts, defined first
#endif

vec4 fragColor = vec4(0.0);
FRAG_OUT(0) out vec4 out_accum;
FRAG_OUT(1) out vec4 out_reveal;
FRAG_UBO(OIT_UBO) uniform OitFrag {
    highp ivec4 u_foit;         /*  0  x mode (0 WBOIT, 1 MBOIT moments,
                                 *     2 MBOIT composite), y moment targets
                                 *     multisampled */
};                              /* 16 bytes */
#define u_oit_mode  u_foit.x
#define u_mboit_ms  u_foit.y
FRAG_TEX(OIT_TEX0) uniform sampler2D u_mboit_b0;
FRAG_TEX(OIT_TEX0 + 1) uniform sampler2D u_mboit_mom;
FRAG_TEX(OIT_TEX0 + 2) uniform sampler2DMS u_mboit_b0_ms;
FRAG_TEX(OIT_TEX0 + 3) uniform sampler2DMS u_mboit_mom_ms;
#define OIT_TEX_COUNT 4
#define MBOIT_OVERESTIMATION 0.25
/* log-warp view distance to [-1,1]: 14 octaves from the 4-unit near plane,
 * i.e. up to 65536 units */
float MBOIT_Depth() {
    float d = max(1.0 / gl_FragCoord.w, 4.0) * 0.25;
    return clamp(log2(d) * (1.0 / 14.0), 0.0, 1.0) * 2.0 - 1.0;
}
float MBOIT_Transmittance4(float b_0, vec2 b_even, vec2 b_odd, float depth, float bias, vec4 bias_vector) {
    vec4 b = vec4(b_odd.x, b_even.x, b_odd.y, b_even.y);
    b = mix(b, bias_vector, bias);
    vec3 z;
    z[0] = depth;
    float L21D11 = fma(-b[0], b[1], b[2]);
    float D11 = fma(-b[0], b[0], b[1]);
    float InvD11 = 1.0 / D11;
    float L21 = L21D11 * InvD11;
    float SquaredDepthVariance = fma(-b[1], b[1], b[3]);
    float D22 = fma(-L21D11, L21, SquaredDepthVariance);
    vec3 c = vec3(1.0, z[0], z[0] * z[0]);
    c[1] -= b.x;
    c[2] -= b.y + L21 * c[1];
    c[1] *= InvD11;
    c[2] /= D22;
    c[1] -= L21 * c[2];
    c[0] -= dot(c.yz, b.xy);
    float InvC2 = 1.0 / c[2];
    float p = c[1] * InvC2;
    float q = c[0] * InvC2;
    float D = (p * p * 0.25) - q;
    float r = sqrt(D);
    z[1] = -p * 0.5 - r;
    z[2] = -p * 0.5 + r;
    vec3 polynomial;
    vec3 weight_factor = vec3(MBOIT_OVERESTIMATION, (z[1] < z[0]) ? 1.0 : 0.0, (z[2] < z[0]) ? 1.0 : 0.0);
    float f0 = weight_factor[0];
    float f1 = weight_factor[1];
    float f2 = weight_factor[2];
    float f01 = (f1 - f0) / (z[1] - z[0]);
    float f12 = (f2 - f1) / (z[2] - z[1]);
    float f012 = (f12 - f01) / (z[2] - z[0]);
    polynomial[0] = f012;
    polynomial[1] = polynomial[0];
    polynomial[0] = f01 - polynomial[0] * z[1];
    polynomial[2] = polynomial[1];
    polynomial[1] = polynomial[0] - polynomial[1] * z[0];
    polynomial[0] = f0 - polynomial[0] * z[0];
    float absorbance = polynomial[0] + dot(b.xy, polynomial.yz);
    if (isnan(absorbance))
        absorbance = MBOIT_OVERESTIMATION;
    return clamp(exp(-b_0 * absorbance), 0.0, 1.0);
}
float MBOIT_TransmittanceAtDepth(float z, float b0, vec4 moments0) {
    if (b0 <= 1e-3)
        return 1.0;
    moments0 /= b0;
    /* vkQuake's bias, well above the paper's: a lone fragment's Hankel matrix
     * is rank one and its pivots drown in fp32 noise otherwise */
    return MBOIT_Transmittance4(b0, moments0.yw, moments0.xz, z, 6e-5, vec4(0.0, 0.375, 0.0, 0.375));
}
void main_body();
void main() {
    main_body();
    fragColor = clamp(fragColor, 0.0, 1.0);
    if (u_oit_mode == 0) {
        float z = 1.0 / gl_FragCoord.w;
        float w = clamp(fragColor.a * fragColor.a * 0.03 / (1e-5 + pow(z/1e7, 1.0)), 1e-2, 3e3);
        out_accum = vec4(fragColor.rgb * fragColor.a * w, fragColor.a * w);
        out_reveal = vec4(fragColor.a, 0.0, 0.0, 0.0);
        return;
    }
    /* both MBOIT passes must drop the same fragments */
    if (fragColor.a < (1.0 / 255.0))
        discard;
    float mz = MBOIT_Depth();
    if (u_oit_mode == 1) {
        float absorbance = -log(max(1.0 - clamp(fragColor.a, 0.0, 0.999), 1e-4));
        float z2 = mz * mz;
        out_accum = vec4(absorbance, 0.0, 0.0, 0.0);
        out_reveal = vec4(mz, z2, z2 * mz, z2 * z2) * absorbance;
        return;
    }
    ivec2 pix = ivec2(gl_FragCoord.xy);
    float b0;
    vec4 mom;
    /* sample 0 under MSAA, as vkQuake: moments only differ between samples at
     * the edges of translucent geometry */
    if (u_mboit_ms != 0) {
        b0 = texelFetch(u_mboit_b0_ms, pix, 0).r;
        mom = texelFetch(u_mboit_mom_ms, pix, 0);
    } else {
        b0 = texelFetch(u_mboit_b0, pix, 0).r;
        mom = texelFetch(u_mboit_mom, pix, 0);
    }
    float t = MBOIT_TransmittanceAtDepth(mz, b0, mom);
    out_accum = vec4(fragColor.rgb * fragColor.a * t, fragColor.a * t);
    out_reveal = vec4(0.0);
}
#define main main_body

#endif /* OIT_INC */
