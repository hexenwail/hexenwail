#include "uniforms.inc"
uniform sampler2D u_texture0;
in vec2 v_texcoord;
in vec4 v_color;
out vec4 fragColor;
void main() {
    vec4 tex = texture(u_texture0, v_texcoord);
    vec4 color = tex * v_color;
    if (color.a < u_alpha_threshold) discard;
    fragColor = color;
}
