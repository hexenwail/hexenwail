/* sbloom_up -- 3x3 tent upsample of the coarser level; the C side blends it
 * additively (GL_ONE, GL_ONE) into the finer one.
 * Moved from bloom_up_frag_src (#450).
 * No #version line; gl_postprocess.c passes PP_FRAG_HEADER.
 */
#include "bloom.inc"

void main() {
    vec2 h = u_rcpframe;
    vec4 s = texture(u_scene, v_uv + vec2(-h.x, 0)) * 2.0
           + texture(u_scene, v_uv + vec2( h.x, 0)) * 2.0
           + texture(u_scene, v_uv + vec2(0, -h.y)) * 2.0
           + texture(u_scene, v_uv + vec2(0,  h.y)) * 2.0
           + texture(u_scene, v_uv + vec2(-h.x,-h.y))
           + texture(u_scene, v_uv + vec2( h.x,-h.y))
           + texture(u_scene, v_uv + vec2(-h.x, h.y))
           + texture(u_scene, v_uv + vec2( h.x, h.y));
    s /= 12.0;
    fragColor = s;
}
