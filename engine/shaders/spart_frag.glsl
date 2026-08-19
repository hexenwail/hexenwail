#include "uniforms.inc"
uniform sampler2D u_texture0;
in vec2 v_texcoord;
in vec4 v_color;
in float v_fogdist;
out vec4 fragColor;
void main() {
    vec4 tex = texture(u_texture0, v_texcoord);
    vec4 color = tex * v_color;
    if (color.a < 0.01) discard;
    float fogfac = u_fog_density * v_fogdist;
    float fog = exp(-fogfac * fogfac);
    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));
    fragColor = color;
}
