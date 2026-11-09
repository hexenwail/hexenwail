/* ssky_cube -- the skybox as a cubemap, sampled by view direction.  Ironwail's
 * skycubemap (Quake/gl_shaders.h sky_cubemap_*), uhexen2-ctk9.  It exists so
 * r_skywind can animate a skybox without the face seams pulling apart, which
 * a per-face 2D UV slide cannot avoid.
 *
 * The axis swizzle is upstream's: st_to_vec in gl_sky.c puts rt/lf on world
 * +X/-X, bk/ft on +Y/-Y and up/dn on +Z/-Z, the convention Ironwail's
 * cubemap_order was written against.
 *
 * u_eyepos is VertParams' u_vscene.xyz, fed from r_origin by GL_ImmPushBlocks.
 *
 * No #version line here on purpose; gl_shader.c supplies the tier's own.
 */
#include "layout.inc"
#define UNIFORMS_VERT
#include "uniforms.inc"

/* ATTR_POSITION / ATTR_COLOR from gl_shader.h. */
VERT_IN(0) in vec3 a_position;
VERT_IN(3) in vec4 a_color;

VERT_OUT(0) out vec3 v_dir;
VERT_OUT(1) out vec4 v_color;

void main() {
    vec3 d = a_position - u_eyepos;
    v_dir = vec3(-d.y, d.z, d.x);
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
