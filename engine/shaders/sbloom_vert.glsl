/* sbloom_vert -- the bloom passes' fullscreen triangle, from gl_VertexID with
 * no vertex buffer (gl_postprocess.c binds an empty VAO).  Shared by all three
 * bloom programs.  Moved from gl_postprocess.c's bloom_vert_src (#450).
 *
 * No #version line: gl_postprocess.c passes PP_VERT_HEADER (desktop 430 core,
 * or ES 3.00 with its precision lines) as a separate chunk.
 */
#include "layout.inc"

/* Vulkan GLSL has no gl_VertexID.  gl_VertexIndex differs from it only by the
 * draw's firstVertex, and every bloom draw is glDrawArrays (GL_TRIANGLES, 0, 3),
 * so the alias is exact -- the same one ssao_composite_vert carries. */
#ifdef VULKAN
#define gl_VertexID gl_VertexIndex
#endif

VERT_OUT(0) out vec2 v_uv;
void main() {
    ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);
    vec2 pos = vec2(v) * 4.0 - 1.0;
    v_uv = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
