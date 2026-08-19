/* Read by uniforms.inc: SDL_GPU numbers a fragment stage's descriptor
   sets differently from a vertex stage's.  Declared here rather than passed
   in with -D so a combined vert+frag compile still gets it right. */
#define FRAGMENT_STAGE
#include "uniforms.inc"
TEX(0) uniform sampler2D u_texture0;
TEX(1) uniform sampler2D u_texture1;
VARY(5) in vec3 v_dir;
VARY(0) in vec2 v_texcoord;
VARY(2) in vec2 v_lmcoord;
VARY(1) in vec4 v_color;
/* The OIT variant of this shader declares fragColor itself, as a plain
   global that its wrapper main() reads after calling ours -- see
   GL_CompileOITFragShader.  It prepends "#define OIT 1", so this
   declaration drops out rather than colliding. */
#ifndef OIT
FRAGOUT(0) out vec4 fragColor;
#endif
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
