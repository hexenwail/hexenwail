/* Read by uniforms.inc: SDL_GPU numbers a fragment stage's descriptor
   sets differently from a vertex stage's.  Declared here rather than passed
   in with -D so a combined vert+frag compile still gets it right. */
#define FRAGMENT_STAGE
#include "uniforms.inc"
VARY(1) in vec4 v_color;
/* The OIT variant of this shader declares fragColor itself, as a plain
   global that its wrapper main() reads after calling ours -- see
   GL_CompileOITFragShader.  It prepends "#define OIT 1", so this
   declaration drops out rather than colliding. */
#ifndef OIT
FRAGOUT(0) out vec4 fragColor;
#endif
void main() {
    fragColor = v_color;
}
