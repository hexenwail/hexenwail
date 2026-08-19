#include "uniforms.inc"
uniform sampler2D u_texture0;
uniform sampler2D u_texture1;
uniform sampler2D u_texture2;
in vec2 v_texcoord;
in vec2 v_lmcoord;
in vec4 v_color;
in float v_fogdist;
in vec2 v_worldxy;
out vec4 fragColor;
#include "bicubic_lm.inc"
#include "caustics.inc"
void main() {
    vec4 tex = texture(u_texture0, v_texcoord);
    vec4 lm = (u_lightmap_bicubic > 0.5)
        ? BicubicLightmap(u_texture1, v_lmcoord)
        : texture(u_texture1, v_lmcoord);
    lm.rgb = mix(lm.rgb, vec3(1.0), u_lightdebug.x);
    tex.rgb = mix(tex.rgb, vec3(1.0), u_lightdebug.y);
    vec4 color = tex * lm * v_color;
    color.rgb *= u_overbright;
    vec3 fb = texture(u_texture2, v_texcoord).rgb * (1.0 - u_lightdebug.y);
    color.rgb += fb;
    if (u_caustics.x > 0.0) {
        float c = Caustics(v_worldxy, u_caustics.y);
        color.rgb += color.rgb * c * u_caustics.x;
    }
    float fogfac = u_fog_density * v_fogdist;
    float fog = exp(-fogfac * fogfac);
    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));
    fragColor = vec4(color.rgb, 1.0);
}
