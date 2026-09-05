/* sflat fragment stage -- see sflat_vert.glsl.
 *
 * Declares no uniform block at all, because it reads nothing but the
 * interpolated colour.  R_BindProgramBlocks tolerates a program that declares
 * neither block, and GL_ImmEnd keys its two paths on the per-block index
 * rather than on shader identity -- so this program takes the block path for
 * its vertex stage and issues no fragment push whatsoever.
 *
 * No #version line here on purpose; gl_shader.c supplies the tier's own.
 */
#include "layout.inc"

FRAG_IN(0) in vec4 v_color;

FRAG_OUT(0) out vec4 fragColor;

void main() {
    fragColor = v_color;
}
