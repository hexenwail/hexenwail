uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
uniform vec4 u_skyfog;
uniform vec2 u_wind;
in vec2 v_texcoord;
in vec2 v_lmcoord;
in vec4 v_color;
out vec4 fragColor;
void main() {
    vec4 solid = texture(u_texture0, v_texcoord + u_wind);
    vec4 layer = texture(u_texture1, v_lmcoord + u_wind);
    vec3 color = mix(solid.rgb, layer.rgb, layer.a);
    color = mix(color, u_skyfog.rgb, u_skyfog.a);
    fragColor = vec4(color, 1.0) * v_color;
}
