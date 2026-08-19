#include "uniforms.inc"
uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
in vec3 v_dir;
in vec2 v_texcoord;
in vec2 v_lmcoord;
in vec4 v_color;
out vec4 fragColor;
void main() {
    if (u_alpha_threshold > 0.5) {
        vec4 solid = texture(u_texture0, v_texcoord + u_wind);
        fragColor = solid * v_color;
    } else {
        vec2 uv = normalize(v_dir).xy * (189.0 / 64.0);
        vec4 solid = texture(u_texture0, uv + u_time / 16.0 + u_wind);
        vec4 layer = texture(u_texture1, uv + u_time / 8.0 + u_wind);
        vec3 color = mix(solid.rgb, layer.rgb, layer.a);
        color = mix(color, u_skyfog.rgb, u_skyfog.a);
        fragColor = vec4(color, 1.0) * v_color;
    }
}
