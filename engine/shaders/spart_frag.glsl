/* Read by uniforms.inc: SDL_GPU numbers a fragment stage's descriptor
   sets differently from a vertex stage's.  Declared here rather than passed
   in with -D so a combined vert+frag compile still gets it right. */
#define FRAGMENT_STAGE
#include "uniforms.inc"
TEX(0) uniform sampler2D u_texture0;
VARY(0) in vec2 v_texcoord;
VARY(1) in vec4 v_color;
VARY(3) in float v_fogdist;
/* The OIT variant of this shader declares fragColor itself, as a plain
   global that its wrapper main() reads after calling ours -- see
   GL_CompileOITFragShader.  It prepends "#define OIT 1", so this
   declaration drops out rather than colliding. */
#ifndef OIT
FRAGOUT(0) out vec4 fragColor;
#endif
void main() {
    vec4 tex = texture(u_texture0, v_texcoord);
    vec4 color = tex * v_color;
    if (color.a < 0.01) discard;
    float fogfac = u_fog_density * v_fogdist;
    float fog = exp(-fogfac * fogfac);
    color.rgb = mix(u_fog_color, color.rgb, clamp(fog, 0.0, 1.0));
    fragColor = color;
}
