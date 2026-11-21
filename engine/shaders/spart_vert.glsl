/* spart -- textured triangles with per-vertex color (particle program), and
 * gl_particles 0's square points.  Submitted through
 * GL_ImmEnd(&gl_shader_particle) from r_part.c.
 *
 * u_mvp and u_modelview are VertParams members u_mvpm / u_modelviewm; the
 * fog uniforms live in the fragment stage.  Attributes and varyings carry
 * the layout.inc qualifiers so the offline SPIR-V compile sees explicit
 * locations.
 *
 * No #version line here on purpose; gl_shader.c supplies the tier's own.
 */
#include "layout.inc"
#define UNIFORMS_VERT
#include "uniforms.inc"

/* Locations are ATTR_POSITION / ATTR_TEXCOORD / ATTR_COLOR from gl_shader.h.
 * ATTR_LMCOORD (2) is skipped: the immediate-mode vertex carries it, this
 * program does not read it. */
VERT_IN(0) in vec3 a_position;
VERT_IN(1) in vec2 a_texcoord;
VERT_IN(3) in vec4 a_color;

VERT_OUT(0) out vec2 v_texcoord;
VERT_OUT(1) out vec4 v_color;
VERT_OUT(2) out float v_fogdist;

void main() {
    v_texcoord = a_texcoord;
    v_color = a_color;
    vec4 eyepos = u_modelview * vec4(a_position, 1.0);
    v_fogdist = length(eyepos.xyz);
    gl_Position = u_mvp * vec4(a_position, 1.0);
#ifdef VULKAN
    /* gl_particles 0 draws GL_POINTS at GL_SetPointSize (2.0f) (r_part.c),
     * which SDL_GPU has no state for: a point-list pipeline takes its size
     * from here, and Vulkan requires the write.  GL keeps glPointSize, its
     * preprocessed program unchanged, and the triangles ignore the size.
     * #340 P3b; tools/test_rg_sources.py holds the two sizes together. */
    gl_PointSize = 2.0;
#endif
    VK_FLIP_Y()
}
