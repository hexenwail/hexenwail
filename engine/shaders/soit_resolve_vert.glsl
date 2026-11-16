/* soit_resolve_vert -- the OIT resolve's fullscreen triangle, from
 * gl_VertexID with no vertex buffer.  Shared by both resolve programs.
 * Moved from gl_postprocess.c's oit_resolve_vert (#450).
 * No #version line; gl_postprocess.c passes PP_VERT_HEADER.
 */
#include "layout.inc"

/* Vulkan GLSL has no gl_VertexID; gl_VertexIndex equals it for this
 * glDrawArrays (GL_TRIANGLES, 0, 3), as in sbloom_vert. */
#ifdef VULKAN
#define gl_VertexID gl_VertexIndex
#endif

void main() {
    ivec2 v = ivec2(gl_VertexID & 1, gl_VertexID >> 1);
    gl_Position = vec4(vec2(v) * 4.0 - 1.0, 0.0, 1.0);
}
