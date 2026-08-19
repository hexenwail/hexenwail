#include "uniforms.inc"
uniform sampler2D u_texture0;
uniform highp sampler2D u_soft_depth;
in vec2 v_texcoord;
in vec4 v_color;
in float v_fogdist;
in vec2 v_worldxy;
out vec4 fragColor;
#include "caustics.inc"
void main() {
    vec4 tex = texture(u_texture0, v_texcoord);
    vec4 color = tex * v_color;
    if (color.a < u_alpha_threshold) discard;
    if (u_alias_caustics.x > 0.0) {
        float c = Caustics(v_worldxy, u_alias_caustics.y);
        color.rgb += color.rgb * c * u_alias_caustics.x;
    }
    float fogfac = u_fog_density * v_fogdist;
    float fog = exp(-fogfac * fogfac);
    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));
    if (u_soft_params.x > 0.0) {
        highp float dscene = texelFetch(u_soft_depth, ivec2(gl_FragCoord.xy), 0).r;
        highp float zscene = u_soft_params.z / (u_soft_params.y - dscene);
        highp float zfrag  = u_soft_params.z / (u_soft_params.y - gl_FragCoord.z);
        color.a *= clamp((zscene - zfrag) * u_soft_params.x, 0.0, 1.0);
    }
#ifdef OIT
    fragColor = color;
#else
    fragColor = vec4(color.rgb, u_force_opaque_alpha > 0.5 ? 1.0 : color.a);
#endif
}
