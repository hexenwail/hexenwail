#include "uniforms.inc"
ATTR(0) in vec3 a_position;
ATTR(1) in vec2 a_texcoord;
ATTR(3) in vec4 a_color;
VARY(0) out vec2 v_texcoord;
VARY(1) out vec4 v_color;
VARY(3) out float v_fogdist;
void main() {
    v_texcoord = a_texcoord;
    v_color = a_color;
    vec4 eyepos = u_modelview * vec4(a_position, 1.0);
    v_fogdist = length(eyepos.xyz);
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
