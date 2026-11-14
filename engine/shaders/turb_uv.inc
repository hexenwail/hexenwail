/* turb_uv.inc -- the analytic band-limited liquid UV warp (uhexen2-9o7u).
 *
 * A copy of gl_shader.c's GLSL_TURB_UV_FN, which sworld_frag still splices
 * while the world family is a C literal: an unlit liquid (this program) and a
 * lit one (the world program, uhexen2-a5nn.2) must ripple identically, so
 * tools/test_alias_frag_block.py keeps the two token-identical (#450).  Why
 * this is not just sin(), and what the sinc attenuation filters, is written
 * up on GLSL_TURB_UV_FN.
 *
 * Callers gate the call on turb.x > 0.0, which keeps the fwidth() below in
 * uniform control flow and costs a non-liquid draw nothing.
 */
#ifndef TURB_UV_INC
#define TURB_UV_INC

vec2 TurbUV(vec2 uv, vec2 turb) {
    const float K = 0.125;
    const float TAU = 6.2831853;
    vec2 raw = uv * 64.0;
    vec2 fw  = fwidth(raw);
    vec2 z   = 0.5 * K * vec2(fw.y, fw.x);
    vec2 att = vec2(z.x > 1e-4 ? sin(z.x) / z.x : 1.0,
                    z.y > 1e-4 ? sin(z.y) / z.y : 1.0);
    att = clamp(att, 0.0, 1.0);
    float as = mod(K * raw.y + turb.y, TAU);
    float at = mod(K * raw.x + turb.y, TAU);
    raw += turb.x * vec2(att.x * sin(as), att.y * sin(at));
    return raw * (1.0 / 64.0);
}

#endif /* TURB_UV_INC */
