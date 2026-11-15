/* ssky_layers fragment stage -- see ssky_layers_vert.glsl.
 *
 * u_skyfog is FragParams' u_fskyfog and u_wind is u_fsky.xy.  Fog_SetupFrame
 * writes u_skyfog every frame, Ironwail's skyfogdata: the fog colour, alpha
 * r_skyfog (or worldspawn skyfog) clamped to 0..1, and 0 with fog off, which
 * makes the mix below an exact no-op.  GitHub #486.
 *
 * No #version line here on purpose; gl_shader.c supplies the tier's own.
 */
#include "layout.inc"
#define UNIFORMS_FRAG
#include "uniforms.inc"

FRAG_TEX(0) uniform sampler2D u_texture0;	/* solid / back layer */
FRAG_TEX(1) uniform sampler2D u_texture1;	/* alpha / front layer */

FRAG_IN(0) in vec2 v_texcoord;
FRAG_IN(1) in vec2 v_lmcoord;
FRAG_IN(2) in vec4 v_color;

FRAG_OUT(0) out vec4 fragColor;

void main() {
    vec4 solid = texture(u_texture0, v_texcoord + u_wind);
    vec4 layer = texture(u_texture1, v_lmcoord + u_wind);
    vec3 color = mix(solid.rgb, layer.rgb, layer.a);
    color = mix(color, u_skyfog.rgb, u_skyfog.a);
    fragColor = vec4(color, 1.0) * v_color;
}
