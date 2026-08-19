/* Read by uniforms.inc: SDL_GPU numbers a fragment stage's descriptor
   sets differently from a vertex stage's.  Declared here rather than passed
   in with -D so a combined vert+frag compile still gets it right. */
#define FRAGMENT_STAGE
#include "uniforms.inc"
TEX(0) uniform sampler2D u_texture0;
TEX(1) uniform sampler2D u_texture1;
TEX(2) uniform sampler2D u_texture2;
VARY(0) in vec2 v_texcoord;
VARY(2) in vec2 v_lmcoord;
VARY(1) in vec4 v_color;
VARY(3) in float v_fogdist;
VARY(4) in vec2 v_worldxy;
/* The OIT variant of this shader declares fragColor itself, as a plain
   global that its wrapper main() reads after calling ours -- see
   GL_CompileOITFragShader.  It prepends "#define OIT 1", so this
   declaration drops out rather than colliding. */
#ifndef OIT
FRAGOUT(0) out vec4 fragColor;
#endif
#include "bicubic_lm.inc"
#include "caustics.inc"
void main() {
    vec4 tex = texture(u_texture0, v_texcoord);
    vec4 lm = (u_lightmap_bicubic > 0.5)
        ? BicubicLightmap(u_texture1, v_lmcoord)
        : texture(u_texture1, v_lmcoord);
    lm.rgb = mix(lm.rgb, vec3(1.0), u_lightdebug.x);
    tex.rgb = mix(tex.rgb, vec3(1.0), u_lightdebug.y);
    vec3 fb = texture(u_texture2, v_texcoord).rgb * (1.0 - u_lightdebug.y);
    vec4 color = tex * lm * v_color;
    color.rgb *= u_overbright;
    if (color.a < u_alpha_threshold) discard;
    color.rgb += fb;
    if (u_caustics.x > 0.0) {
        float c = Caustics(v_worldxy, u_caustics.y);
        color.rgb += color.rgb * c * u_caustics.x;
    }
    float fogfac = u_fog_density * v_fogdist;
    float fog = exp(-fogfac * fogfac);
    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));
#ifdef OIT
    fragColor = color;
#else
    fragColor = vec4(color.rgb, u_force_opaque_alpha > 0.5 ? 1.0 : color.a);
#endif
}
