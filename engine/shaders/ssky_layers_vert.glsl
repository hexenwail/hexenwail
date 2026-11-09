/* ssky_layers -- two scrolling cloud layers blended in one pass, used at
 * r_skyalpha >= 1.  Ironwail's skylayers (Quake/gl_shaders.h sky_layers_*),
 * except that the layer UVs come from the vertex: gl_warp.c computes them
 * from r_skyspeed_back / r_skyspeed_front on realtime, which keeps those cvars
 * live on this path as well as on the two-pass one.
 *
 * No #version line here on purpose; gl_shader.c supplies the tier's own.
 */
#include "layout.inc"
#define UNIFORMS_VERT
#include "uniforms.inc"

/* ATTR_POSITION / ATTR_TEXCOORD / ATTR_LMCOORD / ATTR_COLOR from gl_shader.h.
 * a_lmcoord carries the FRONT layer's UV, not a lightmap coordinate. */
VERT_IN(0) in vec3 a_position;
VERT_IN(1) in vec2 a_texcoord;
VERT_IN(2) in vec2 a_lmcoord;
VERT_IN(3) in vec4 a_color;

VERT_OUT(0) out vec2 v_texcoord;
VERT_OUT(1) out vec2 v_lmcoord;
VERT_OUT(2) out vec4 v_color;

void main() {
    v_texcoord = a_texcoord;
    v_lmcoord = a_lmcoord;
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
