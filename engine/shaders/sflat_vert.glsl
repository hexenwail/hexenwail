/* sflat -- untextured, vertex-coloured geometry: dlight cones, blend polys,
 * screen fades, the sky stencil pass and the r_showtris wireframe.  The
 * busiest GL_ImmEnd program in the engine by call-site count.
 *
 * No #version line here on purpose.  The GL side passes it as a separate
 * glShaderSource chunk (gl_shader.c) because the desktop and ES tiers want
 * different ones, and EmbedShaders.cmake prepends its own for the offline
 * SPIR-V compile.
 */
#include "layout.inc"
#define UNIFORMS_VERT
#include "uniforms.inc"

/* ATTR_POSITION / ATTR_COLOR from gl_shader.h.  The sky-stencil and showtris
 * paths draw with the a_color array DISABLED and rely on the generic attribute
 * value glVertexAttrib4f leaves behind, so this has to stay an ordinary
 * attribute read. */
VERT_IN(0) in vec3 a_position;
VERT_IN(3) in vec4 a_color;

VERT_OUT(0) out vec4 v_color;

void main() {
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
