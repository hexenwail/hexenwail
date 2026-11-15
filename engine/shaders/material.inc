/* material.inc -- tangent-space normal + specular from _norm/_bump/_gloss
 * sidecars (uhexen2-mfql, model half uhexen2-4kcb).
 *
 * TANGENT FRAME WITHOUT TANGENTS.  BSP surfaces carry no tangent attribute,
 * and adding one would mean a vertex-format change plus a generation pass
 * over every surface at load.  It is not needed: a tangent frame is fully
 * determined by how the texture coordinate varies across the surface, and
 * the fragment stage can read that straight off the screen-space derivatives
 * of position and UV (Christian Schuler's cotangent frame).  That costs a
 * handful of ALU, needs no new vertex data, and works unchanged for world
 * surfaces, brush ents and any other geometry that reaches this program.
 *
 * The geometric normal comes from cross(dFdx(p), dFdy(p)) rather than a
 * vertex normal, which is exact here -- every BSP surface is planar by
 * construction, so the face normal IS the surface normal with no
 * interpolation error.  Its sign follows triangle winding, so it is flipped
 * to face the viewer; in eye space "toward the viewer" is just -p.
 *
 * FAKE DELUXEMAPPING.  Hexen II bakes all lighting, dynamic lights included,
 * into the lightmap on the CPU (R_AddDynamicLights), so at this point there
 * is no light DIRECTION anywhere in the frame -- only an irradiance value.
 * Real per-pixel normal mapping needs one, and the ways to get it are a
 * deluxemap (which Hexen II BSPs do not have) or realtime lights (a far
 * larger project than this).  DarkPlaces hits the same wall and answers it
 * with r_glsl_deluxemapping 2, a fixed light vector in tangent space; this
 * is the same trick.  It is not physically correct and is not claimed to be:
 * it makes surface relief legible, which is the thing the feature was asked
 * for, and it does so consistently because the tangent frame is anchored to
 * the texture's own axes.
 *
 * BRIGHTNESS IS PRESERVED BY CONSTRUCTION.  The N.L term is divided by the
 * response a FLAT normal would have produced, so a flat region of a normal
 * map yields exactly 1.0 and multiplies the lightmap by nothing.  Only
 * deviation from flat shades.  This is what stops installing a normal-map
 * pack from globally darkening a map -- the naive form multiplies everything
 * by L.z and quietly dims the whole level.
 *
 * The one copy for both families since the world fragment stages became
 * files (#450, world slice W2); it was gl_shader.c's GLSL_MATERIAL_FN, which
 * this file was kept token-identical to while those were C literals.
 */
#ifndef MATERIAL_INC
#define MATERIAL_INC

const vec3 MAT_LIGHTDIR = vec3(-0.35, -0.35, 0.868);
/* Below this much light-facing, relief fades out rather than dividing by a
 * vanishing flat response.  Only the model half can reach it -- see
 * MaterialShadeCore.  0.35 is ~70 degrees off the surface normal, past which
 * a bump's lit face and its shadowed face are nearly the same brightness
 * anyway, so there is little relief left to lose. */
const float MAT_GRAZE = 0.35;
/* Ceiling on the relief gain.  Only the model half can approach it; the world
 * half's fixed light tops out at 1/0.868 = 1.15, so this leaves it
 * bit-identical.  4 is a safety net against the divide running away near
 * edge-on light, not a shaper -- the fade above has already taken most of the
 * term out by the time it binds. */
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
    /* Degenerate UVs (a zero-area triangle, or a surface seen exactly
     * edge-on) make both T and B zero and inversesqrt(0) infinite.  The max()
     * floor keeps the frame finite; the normal it produces is meaningless
     * there, but the fragment is sub-pixel and the alternative is a NaN that
     * propagates into the final colour as a black or white speck. */
    highp float d = max(dot(T,T), dot(B,B));
    highp float invmax = (d > 0.0) ? inversesqrt(d) : 0.0;
    return mat3(T * invmax, B * invmax, N);
}
/* The tangent frame for a camera-relative position, with the geometric normal
 * recovered from its derivatives.  Split out of MaterialShade so the
 * directional variant below can build the frame once and still hand it to the
 * shared core.  uhexen2-4kcb. */
mat3 MaterialFrame(highp vec3 p, highp vec2 uv) {
    highp vec3 Ng = normalize(cross(dFdx(p), dFdy(p)));
    if (dot(Ng, p) > 0.0) Ng = -Ng;
    return CotangentFrame(Ng, p, uv);
}
/* Returns vec4(diffuse scale, specular rgb): .x multiplies the lightmap, .yzw
 * is added after it.  One call rather than two so the cutout and opaque
 * fragment variants cannot drift apart.
 *
 * `Lt` is the light direction ALREADY IN TANGENT SPACE.  The world half passes
 * the MAT_LIGHTDIR constant straight through; the model half (uhexen2-4kcb)
 * rotates a real direction into the frame first. */
vec4 MaterialShadeCore(sampler2D normtex, sampler2D glosstex, highp vec2 uv,
                       mat3 TBN, highp vec3 p, vec3 Lt,
                       vec2 params, float glossexp) {
    /* Decode [0,1] -> [-1,1].  A pack that ships a flat 128,128,255 map, or
     * the sentinel bound when it ships none, decodes to (0,0,1). */
    vec3 n = texture(normtex, uv).xyz * 2.0 - 1.0;
    n = normalize(mix(vec3(0.0, 0.0, 1.0), n, params.x));
    float ndotl = max(dot(n, Lt), 0.0);
    /* Divide out the flat response so flat == no change.  Lt.z IS that
     * response, because a flat normal is (0,0,1), which is why the ratio is
     * EXACTLY 1.0 for a flat normal at any light angle -- the numerator and
     * the denominator are the same quantity.  That identity is what stops a
     * normal-map pack from globally darkening the scene, and it is why the
     * guard below bounds the RATIO rather than the divisor: flooring Lt.z
     * instead would make the two differ, and a flat map would come out at
     * Lt.z/MAT_GRAZE < 1 -- darkening every model the light merely grazes,
     * including models with no sidecar at all, which sample the flat sentinel
     * and must be pixel-identical to the path being off.
     *
     * The world half's Lt is the MAT_LIGHTDIR constant, whose .z is 0.868, so
     * `w` is 1 and the cap (max ratio 1/0.868 = 1.15) never binds: this
     * reduces to the plain ndotl/MAT_LIGHTDIR.z it has always computed.  The
     * model half's Lt varies per fragment, and on a surface the light grazes
     * or backfaces its .z falls to 0, where the ratio runs away.  Cap it, and
     * fade the whole term back to flat as the light goes edge-on: relief is a
     * redistribution of arriving light, and no light arrives at a face turned
     * away from the source.  uhexen2-4kcb. */
    float w = smoothstep(0.0, MAT_GRAZE, Lt.z);
    float ratio = min(ndotl / max(Lt.z, 1e-4), MAT_MAXGAIN);
    float diffuse = mix(1.0, ratio, w);
    vec3 spec = vec3(0.0);
    if (params.y > 0.0) {
        /* Camera is at the origin of p's frame, so the view vector is -p. */
        highp vec3 V = normalize(-p) * TBN;
        vec3 H = normalize(Lt + V);
        float s = pow(max(dot(n, H), 0.0), glossexp);
        /* Same fade as the diffuse term, and for the same reason: a highlight
         * on a face the light cannot reach is light invented from nothing. */
        spec = texture(glosstex, uv).rgb * s * params.y * w;
    }
    return vec4(diffuse, spec);
}
/* World surfaces: a fixed tangent-space direction, anchored to the texture's
 * own axes because a BSP face carries no light direction. */
vec4 MaterialShade(sampler2D normtex, sampler2D glosstex, highp vec2 uv,
                   highp vec3 p, vec2 params, float glossexp) {
    return MaterialShadeCore(normtex, glosstex, uv, MaterialFrame(p, uv), p,
                             MAT_LIGHTDIR, params, glossexp);
}
/* Alias models: `L` arrives in the SAME frame as `p` -- eye space on the
 * immediate and skeletal paths, world space on the instanced one -- and is
 * rotated into tangent space here.  The two frames differ by a pure rotation,
 * under which the whole tangent-space computation is invariant, so each vertex
 * shader can stay in whatever frame it already had.  TBN is orthonormal, so
 * the transpose is the inverse and `L * TBN` is that rotation; the same trick
 * already moves V above.  uhexen2-4kcb. */
vec4 MaterialShadeDir(sampler2D normtex, sampler2D glosstex, highp vec2 uv,
                      highp vec3 p, vec3 L, vec2 params, float glossexp) {
    mat3 TBN = MaterialFrame(p, uv);
    return MaterialShadeCore(normtex, glosstex, uv, TBN, p,
                             normalize(L * TBN), params, glossexp);
}

#endif /* MATERIAL_INC */
