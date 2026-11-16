/* sshadow_vert -- one face of a dynamic light's cube shadow map (GitHub #97,
 * P3).  gl_shadow.c draws the world and the brush entities within the light's
 * radius six times, once per face, with this and sshadow_frag.  No #version:
 * gl_shadow.c supplies the desktop one.  Desktop GL 4.3 only, like the maps.
 *
 * ShadowPass is the pass's one block, vertex stage only: the fragment stage
 * reads nothing but the varying.  GL binding point 14 (UBO_BINDING_SHADOW_PASS),
 * assigned from C; the C mirror is r_shadowpass_t in gl_shadow.c, and
 * tools/test_dlshadow_block.py keeps the two equal.  Every member mat4 or vec4.
 *
 * LIGHT-RELATIVE, not world space: u_shadowmodel is the entity's transform
 * (identity for the world) with the light origin subtracted, so u_shadowvp is
 * one of the same six matrices for every light -- GL's cube-map face table,
 * which tools/test_shadow_cube_faces.py checks gl_shadow.c's copy against.
 */
#include "layout.inc"

VERT_UBO(0) uniform ShadowPass {
    highp mat4 u_shadowvp;      /*   0  light-relative position -> this face's clip space */
    highp mat4 u_shadowmodel;   /*  64  model -> world, minus the light origin */
    highp vec4 u_shadowlight;   /* 128  x 1 / the radius the map is drawn with */
};                              /* 144 bytes */

VERT_IN(0) in vec3 a_position;

/* The light-relative position over the radius.  Affine in the vertex, so the
 * perspective-correct interpolation hands the fragment its exact own value, and
 * its length is the fragment's distance from the light as a fraction of the
 * radius -- what the receivers compare against (dlshadow.inc). */
VERT_OUT(0) out highp vec3 v_lrel;

void main() {
    highp vec3 rel = (u_shadowmodel * vec4(a_position, 1.0)).xyz;
    v_lrel = rel * u_shadowlight.x;
    gl_Position = u_shadowvp * vec4(rel, 1.0);
}
