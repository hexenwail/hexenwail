in vec3 a_position;
in vec4 a_color;
uniform mat4 u_mvp;
uniform vec3 u_eyepos;
out vec3 v_dir;
out vec4 v_color;
void main() {
    vec3 d = a_position - u_eyepos;
    v_dir = vec3(-d.y, d.z, d.x);
    v_color = a_color;
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
