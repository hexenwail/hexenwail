/* s2d -- the one program that draws every 2D element: HUD, menus, console,
 * loading plaques.  Orthographic, textured, vertex-coloured quads, submitted
 * through GL_ImmEnd(GL_QUADS, &gl_shader_2d) from gl_draw.c.
 *
 * No #version line here on purpose.  The GL side passes it as a separate
 * glShaderSource chunk (gl_shader.c) because the desktop and ES tiers want
 * different ones -- ES also needs a default precision, which desktop rejects
 * spelling the same way -- and EmbedShaders.cmake prepends its own for the
 * offline SPIR-V compile.
 */
#include "layout.inc"

/* Locations are ATTR_POSITION / ATTR_TEXCOORD / ATTR_COLOR from gl_shader.h.
 * ATTR_LMCOORD (2) is skipped: the immediate-mode vertex carries it, this
 * program does not read it. */
VERT_IN(0) in vec3 a_position;
VERT_IN(1) in vec2 a_texcoord;
VERT_IN(3) in vec4 a_color;

/* u_mvp is a block member rather than a loose uniform because Vulkan GLSL has
 * no loose non-opaque uniforms at all.  A mat4 is already std140-clean, so
 * unlike a scalar there is no padding here to get wrong. */
VERT_UBO(0) uniform S2DVertParams {
    mat4 u_mvp;
};

VERT_OUT(0) out vec2 v_texcoord;
VERT_OUT(1) out vec4 v_color;

void main() {
    v_texcoord = a_texcoord;
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
