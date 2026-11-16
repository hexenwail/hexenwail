/* bicubic_lm.inc -- 4-tap B-spline bicubic lightmap fetch (Sigg/Hadwiger).
 * 16-luxel Hexen II lightmaps expose the bilinear lattice on big flat walls;
 * bicubic smooths it for ~3 extra texture fetches per fragment.  Cost gated by
 * u_lightmap_bicubic: uniform-static branch -> no work on the bilinear path.
 * Uses textureSize() so the helper is agnostic to atlas resolution.
 * uhexen2-b2f0.
 *
 * Moved from gl_shader.c's GLSL_BICUBIC_LM_FN with the world fragment stages
 * (#450, world slice W2); token-identical to it.
 */
#ifndef BICUBIC_LM_INC
#define BICUBIC_LM_INC

vec4 BicubicLightmap(sampler2D s, vec2 uv) {
    vec2 ts = vec2(textureSize(s, 0));
    vec2 invts = 1.0 / ts;
    vec2 tc = uv * ts - 0.5;
    vec2 fr = fract(tc);
    vec2 ic = floor(tc) + 0.5;
    vec2 fr2 = fr * fr;
    vec2 fr3 = fr2 * fr;
    vec2 omf = 1.0 - fr;
    vec2 omf3 = omf * omf * omf;
    vec2 w0 = (1.0/6.0) * omf3;
    vec2 w1 = (1.0/6.0) * (3.0*fr3 - 6.0*fr2 + 4.0);
    vec2 w3 = (1.0/6.0) * fr3;
    vec2 g0 = w0 + w1;
    vec2 g1 = 1.0 - g0;
    vec2 h0 = ((w1 / g0) - 1.0) * invts;
    vec2 h1 = ((w3 / g1) + 1.0) * invts;
    vec2 base = ic * invts;
    vec4 c00 = texture(s, vec2(base.x + h0.x, base.y + h0.y));
    vec4 c10 = texture(s, vec2(base.x + h1.x, base.y + h0.y));
    vec4 c01 = texture(s, vec2(base.x + h0.x, base.y + h1.y));
    vec4 c11 = texture(s, vec2(base.x + h1.x, base.y + h1.y));
    return mix(mix(c00, c10, g1.x), mix(c01, c11, g1.x), g1.y);
}

#endif /* BICUBIC_LM_INC */
