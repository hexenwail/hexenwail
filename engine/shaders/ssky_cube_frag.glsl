/* ssky_cube fragment stage -- see ssky_cube_vert.glsl.
 *
 * u_wind3 and u_skyrot are FragParams' u_fskydir (xyz, w), fed from
 * sky_wind_vec and sky_box_rot by GL_ImmPushBlocks.
 *
 * One divergence from upstream, on purpose: Ironwail's ANIM arm samples twice
 * at two phases and cross-fades them to hide a wind that wraps.  Ours never
 * wraps (Sky_UpdateWind runs a triangle wave), so a single rotated sample is
 * the exact 3D analogue of the 2D slide it replaces.
 *
 * No #version line here on purpose; gl_shader.c supplies the tier's own.
 */
#include "layout.inc"
#define UNIFORMS_FRAG
#include "uniforms.inc"

FRAG_TEX(0) uniform samplerCube u_texture0;

FRAG_IN(0) in vec3 v_dir;
FRAG_IN(1) in vec4 v_color;

FRAG_OUT(0) out vec4 fragColor;

void main() {
    vec3 dir = normalize(v_dir);
    /* r_skybox_speed is a horizontal scroll.  On a cubemap the honest
     * equivalent is a yaw rotation of the sample direction, which does not
     * tear at the face edges.  World +Z is v_dir.y after the vertex swizzle,
     * so yaw lives in the (x, z) plane. */
    float cs = cos(u_skyrot), sn = sin(u_skyrot);
    dir.xz = vec2(dir.x * cs - dir.z * sn, dir.x * sn + dir.z * cs);
    fragColor = texture(u_texture0, dir + u_wind3) * v_color;
}
