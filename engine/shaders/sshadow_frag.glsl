/* sshadow_frag -- writes a cube shadow map texel (GitHub #97, P3): the
 * distance from the light, over the radius the map is drawn with, as the
 * depth.  LINEAR distance, not the projection's z, so one compare against a
 * receiver's own distance works on every face without knowing which face it
 * is (dlshadow.inc), and the precision is even along the whole radius.
 *
 * Writing gl_FragDepth turns early depth testing off; at the map sizes used
 * (r_shadow_resolution, 16-1024, default 128) that costs less than a second set of
 * projection maths per receiver would.  No colour output: the pass has no
 * colour attachment.  No #version: gl_shadow.c supplies it.
 */
#include "layout.inc"

FRAG_IN(0) in highp vec3 v_lrel;

void main() {
    gl_FragDepth = length(v_lrel);
}
