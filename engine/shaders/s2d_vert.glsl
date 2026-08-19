#include "uniforms.inc"
in vec3 a_position;
in vec2 a_texcoord;
in vec4 a_color;
out vec2 v_texcoord;
out vec4 v_color;
void main() {
    v_texcoord = a_texcoord;
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
