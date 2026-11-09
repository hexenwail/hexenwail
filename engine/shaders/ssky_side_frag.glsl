/* ssky_side fragment stage -- see ssky_side_vert.glsl.
 *
 * u_wind is FragParams' u_fsky.xy, fed from sky_wind_uv by GL_ImmPushBlocks.
 * No u_skyfog here on purpose: the skybox path fogs with a second flat pass
 * over the same quad (Sky_DrawSkyBox), not in this shader.
 *
 * No #version line here on purpose; gl_shader.c supplies the tier's own.
 */
#include "layout.inc"
#define UNIFORMS_FRAG
#include "uniforms.inc"

FRAG_TEX(0) uniform sampler2D u_texture0;

FRAG_IN(0) in vec2 v_texcoord;
FRAG_IN(1) in vec4 v_color;

FRAG_OUT(0) out vec4 fragColor;

void main() {
    fragColor = texture(u_texture0, v_texcoord + u_wind) * v_color;
}
