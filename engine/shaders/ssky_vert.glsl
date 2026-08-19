#include "uniforms.inc"
ATTR(0) in vec3 a_position;
ATTR(1) in vec2 a_texcoord;
ATTR(2) in vec2 a_lmcoord;
ATTR(3) in vec4 a_color;
VARY(5) out vec3 v_dir;
VARY(0) out vec2 v_texcoord;
VARY(2) out vec2 v_lmcoord;
VARY(1) out vec4 v_color;
void main() {
    vec3 dir = a_position - u_eyepos;
    dir.z *= 3.0;
    v_dir = dir;
    v_texcoord = a_texcoord;
    v_lmcoord = a_lmcoord;
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
