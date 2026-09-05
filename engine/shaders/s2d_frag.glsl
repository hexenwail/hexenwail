/* s2d fragment stage -- see s2d_vert.glsl.  Paired only with that shader, so
 * the varying locations here need agree with nothing else.
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
    vec4 tex = texture(u_texture0, v_texcoord);
    vec4 color = tex * v_color;
    if (color.a < u_alpha_threshold) discard;
    fragColor = color;
}
