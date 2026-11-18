/* spp_vert -- the post-process blit's vertex stage: a unit quad from the
 * immediate-mode stream, through the ortho MVP the C side sets up.
 * Moved from gl_postprocess.c's pp_vert_src (#450).
 *
 * No #version line; gl_postprocess.c passes PP_VERT_HEADER.  u_mvp was a
 * loose uniform; it is now the PpVert block (GL binding 8,
 * PP_VERT_UBO_BINDING), filled from the C shadow before each draw.
 */
#include "layout.inc"

VERT_UBO(0) uniform PpVert {
    mat4 u_ppmvp;
};
#define u_mvp	u_ppmvp

VERT_IN(0) in vec3 a_position;
VERT_IN(1) in vec2 a_texcoord;
VERT_OUT(0) out vec2 v_texcoord;
void main() {
    v_texcoord = a_texcoord;
    gl_Position = u_mvp * vec4(a_position, 1.0);
    VK_FLIP_Y()
}
