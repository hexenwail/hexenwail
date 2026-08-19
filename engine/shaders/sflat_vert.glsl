#include "uniforms.inc"
ATTR(0) in vec3 a_position;
ATTR(3) in vec4 a_color;
VARY(1) out vec4 v_color;
void main() {
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
