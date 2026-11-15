/* turb_uv.inc -- the analytic band-limited liquid UV warp (uhexen2-9o7u),
 * shared by the alias and world fragment shaders so an unlit liquid and a lit
 * one ripple identically -- lit water renders through the WORLD program
 * (uhexen2-a5nn.2), and two hand-copies of this would be free to drift apart.
 * The one copy since the world fragment stages became files (#450, world
 * slice W2); it was gl_shader.c's GLSL_TURB_UV_FN.
 *
 * Callers gate the call on turb.x > 0.0, which keeps the fwidth() below in
 * uniform control flow and costs a non-liquid draw nothing.
 *
 * WHY THIS IS NOT JUST sin().  The legacy CPU warp evaluates the same sine at
 * tile corners and lets the rasteriser interpolate linearly.  That reconstructs
 * the sine badly -- 93% peak error even at gl_subdivide_size 24, see
 * tools/warp_recon.py -- but it is inherently band-limited: it cannot alias in
 * screen space, because the samples are fixed in world space.  Evaluating the
 * true sine per pixel fixes the shape and forfeits that protection: at a
 * glancing angle one pixel can span many warp periods and the sine aliases into
 * banding.  That is what sank the first attempt (uhexen2-tlsh) and its
 * fwidth-threshold follow-up (uhexen2-famb).
 *
 * The band-limit here is analytic rather than a threshold.  The box average of
 * sin(k*x) over a footprint of width W is sin(k*x)*sinc(k*W/2), so scaling the
 * amplitude by that sinc IS the filtered signal: full throw up close, smoothly
 * to zero as the footprint grows, reaching zero precisely when one pixel covers
 * one whole warp period (k*W/2 = pi).  Clamped at the first zero so the sinc's
 * negative lobes cannot bring back a phase-inverted ripple further out.
 *
 * s is displaced by a sine of t and vice versa (EmitWaterPolys), so each axis
 * is attenuated by the OTHER axis' footprint.
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
