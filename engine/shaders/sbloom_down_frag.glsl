/* sbloom_down -- 4-tap box downsample of the previous bloom level.
 * Moved from bloom_down_frag_src (#450).
 * No #version line; gl_postprocess.c passes PP_FRAG_HEADER.
 */
#include "bloom.inc"

void main() {
    vec2 h = u_rcpframe * 0.5;
    vec4 s = texture(u_scene, v_uv + vec2(-h.x,-h.y))
           + texture(u_scene, v_uv + vec2( h.x,-h.y))
           + texture(u_scene, v_uv + vec2(-h.x, h.y))
           + texture(u_scene, v_uv + vec2( h.x, h.y));
    fragColor = s * 0.25;
}
