#include "uniforms.inc"
in vec3 a_position;
in vec2 a_texcoord;
in vec2 a_lmcoord;
in vec4 a_color;
out vec3 v_dir;
out vec2 v_texcoord;
out vec2 v_lmcoord;
out vec4 v_color;
void main() {
    vec3 dir = a_position - u_eyepos;
    dir.z *= 3.0;
    v_dir = dir;
    v_texcoord = a_texcoord;
    v_lmcoord = a_lmcoord;
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
