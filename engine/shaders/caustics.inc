/* caustics.inc -- procedural underwater caustics, a cheap 2-sin product with
 * ~104 world-unit tiling (Hexen II uses 1 unit ~ 1 inch).  The pow steepens
 * the highlight so the additive contribution reads as crisp light caustics
 * rather than a smooth modulation.  Time advances via the caller's t, so the
 * pattern flows.  uhexen2-6bfm.
 *
 * A copy of gl_shader.c's GLSL_CAUSTICS_FN, which sworld_frag still splices
 * while the world family is a C literal.  tools/test_alias_frag_block.py
 * keeps the two token-identical, so a model and the floor it stands on get the
 * same highlight band (#450).
 */
#ifndef CAUSTICS_INC
#define CAUSTICS_INC

float Caustics(vec2 p, float t) {
    vec2 q1 = p * 0.06 + vec2(t * 0.42, t * 0.31);
    vec2 q2 = p * 0.05 - vec2(t * 0.27, t * 0.49);
    float c1 = sin(q1.x + sin(q1.y + t));
    float c2 = sin(q2.y * 1.3 + sin(q2.x * 0.9 + t * 1.1));
    return pow(max(c1 * c2 * 0.5 + 0.5, 0.0), 4.0);
}

#endif /* CAUSTICS_INC */
