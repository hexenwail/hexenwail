/* material.inc -- tangent-space normal + specular from _norm/_bump/_gloss
 * sidecars (uhexen2-mfql, model half uhexen2-4kcb).
 *
 * A copy of gl_shader.c's GLSL_MATERIAL_FN, which sworld_frag still splices
 * while the world family is a C literal.  The design is written up there: the
 * cotangent frame built from screen-space derivatives instead of a tangent
 * attribute, the fixed fake-deluxemap light for world faces, and why dividing
 * by the flat response preserves brightness.  tools/test_alias_frag_block.py
 * keeps the two token-identical (#450).
 */
#ifndef MATERIAL_INC
#define MATERIAL_INC

const vec3 MAT_LIGHTDIR = vec3(-0.35, -0.35, 0.868);
/* Below this much light-facing, relief fades out rather than dividing by a
 * vanishing flat response.  Only the model half can reach it. */
const float MAT_GRAZE = 0.35;
/* Ceiling on the relief gain: a safety net against the divide running away
 * near edge-on light.  The world half's fixed light tops out at 1.15. */
const float MAT_MAXGAIN = 4.0;
mat3 CotangentFrame(highp vec3 N, highp vec3 p, highp vec2 uv) {
    highp vec3 dp1 = dFdx(p);
    highp vec3 dp2 = dFdy(p);
    highp vec2 duv1 = dFdx(uv);
    highp vec2 duv2 = dFdy(uv);
    highp vec3 dp2perp = cross(dp2, N);
    highp vec3 dp1perp = cross(N, dp1);
    highp vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    highp vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    /* Degenerate UVs make T and B zero; the floor keeps the frame finite
     * rather than a NaN that lands in the final colour as a speck. */
    highp float d = max(dot(T,T), dot(B,B));
    highp float invmax = (d > 0.0) ? inversesqrt(d) : 0.0;
    return mat3(T * invmax, B * invmax, N);
}
/* The tangent frame for a camera-relative position, with the geometric normal
 * recovered from its derivatives. */
mat3 MaterialFrame(highp vec3 p, highp vec2 uv) {
    highp vec3 Ng = normalize(cross(dFdx(p), dFdy(p)));
    if (dot(Ng, p) > 0.0) Ng = -Ng;
    return CotangentFrame(Ng, p, uv);
}
/* vec4(diffuse scale, specular rgb).  `Lt` is already in tangent space. */
vec4 MaterialShadeCore(sampler2D normtex, sampler2D glosstex, highp vec2 uv,
                       mat3 TBN, highp vec3 p, vec3 Lt,
                       vec2 params, float glossexp) {
    vec3 n = texture(normtex, uv).xyz * 2.0 - 1.0;
    n = normalize(mix(vec3(0.0, 0.0, 1.0), n, params.x));
    float ndotl = max(dot(n, Lt), 0.0);
    /* Bound the RATIO, not the divisor, so a flat map stays exactly 1.0 at
     * any light angle; see GLSL_MATERIAL_FN for the full argument. */
    float w = smoothstep(0.0, MAT_GRAZE, Lt.z);
    float ratio = min(ndotl / max(Lt.z, 1e-4), MAT_MAXGAIN);
    float diffuse = mix(1.0, ratio, w);
    vec3 spec = vec3(0.0);
    if (params.y > 0.0) {
        /* Camera is at the origin of p's frame, so the view vector is -p. */
        highp vec3 V = normalize(-p) * TBN;
        vec3 H = normalize(Lt + V);
        float s = pow(max(dot(n, H), 0.0), glossexp);
        spec = texture(glosstex, uv).rgb * s * params.y * w;
    }
    return vec4(diffuse, spec);
}
/* World surfaces: a fixed tangent-space direction. */
vec4 MaterialShade(sampler2D normtex, sampler2D glosstex, highp vec2 uv,
                   highp vec3 p, vec2 params, float glossexp) {
    return MaterialShadeCore(normtex, glosstex, uv, MaterialFrame(p, uv), p,
                             MAT_LIGHTDIR, params, glossexp);
}
/* Alias models: `L` arrives in the same frame as `p` (eye space on the
 * immediate and skeletal paths, world space on the instanced one) and is
 * rotated into tangent space here; TBN is orthonormal, so `L * TBN` is that
 * rotation. */
vec4 MaterialShadeDir(sampler2D normtex, sampler2D glosstex, highp vec2 uv,
                      highp vec3 p, vec3 L, vec2 params, float glossexp) {
    mat3 TBN = MaterialFrame(p, uv);
    return MaterialShadeCore(normtex, glosstex, uv, TBN, p,
                             normalize(L * TBN), params, glossexp);
}

#endif /* MATERIAL_INC */
