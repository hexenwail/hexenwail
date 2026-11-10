/* spart fragment stage -- see spart_vert.glsl.
 *
 * u_fog_density / u_fog_color are FragParams' u_ffogv.w / .xyz.
 *
 * No #version line here on purpose; gl_shader.c supplies the tier's own.
 */
#include "layout.inc"
#define UNIFORMS_FRAG
#include "uniforms.inc"

FRAG_TEX(0) uniform sampler2D u_texture0;

FRAG_IN(0) in vec2 v_texcoord;
FRAG_IN(1) in vec4 v_color;
FRAG_IN(2) in float v_fogdist;

FRAG_OUT(0) out vec4 fragColor;

void main() {
    vec4 tex = texture(u_texture0, v_texcoord);
    vec4 color = tex * v_color;
    if (color.a < 0.01) discard;
    float fogfac = u_fog_density * v_fogdist;
    float fog = exp(-fogfac * fogfac);
    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));
    fragColor = color;
}
