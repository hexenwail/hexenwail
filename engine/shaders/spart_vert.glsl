#include "uniforms.inc"
in vec3 a_position;
in vec2 a_texcoord;
in vec4 a_color;
out vec2 v_texcoord;
out vec4 v_color;
out float v_fogdist;
void main() {
    v_texcoord = a_texcoord;
    v_color = a_color;
    vec4 eyepos = u_modelview * vec4(a_position, 1.0);
    v_fogdist = length(eyepos.xyz);
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
