/* ssky_side -- one sky texture, UVs supplied by the caller.  Ironwail's
 * skyboxside (Quake/gl_shaders.h sky_boxside_*): the six faces of a loaded
 * skybox, and both passes of the two-pass cloud path at r_skyalpha < 1.
 *
 * No #version line here on purpose; gl_shader.c supplies the tier's own and
 * EmbedShaders.cmake prepends its own for the offline SPIR-V compile.
 */
#include "layout.inc"
#define UNIFORMS_VERT
#include "uniforms.inc"

/* ATTR_POSITION / ATTR_TEXCOORD / ATTR_COLOR from gl_shader.h. */
VERT_IN(0) in vec3 a_position;
VERT_IN(1) in vec2 a_texcoord;
VERT_IN(3) in vec4 a_color;

VERT_OUT(0) out vec2 v_texcoord;
VERT_OUT(1) out vec4 v_color;

void main() {
    v_texcoord = a_texcoord;
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
