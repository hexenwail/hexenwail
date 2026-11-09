uniform sampler2D u_texture0;
uniform vec2 u_wind;
in vec2 v_texcoord;
in vec4 v_color;
out vec4 fragColor;
void main() {
    fragColor = texture(u_texture0, v_texcoord + u_wind) * v_color;
}
