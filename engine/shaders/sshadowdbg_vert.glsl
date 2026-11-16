/* sshadowdbg_vert -- r_shadow_debug's overlay (GitHub #97): one triangle that
 * covers the viewport gl_shadow.c sets, from gl_VertexID with no vertex
 * buffer, handing the fragment stage 0..1 across it.  No #version:
 * gl_shadow.c supplies it.
 *
 * Vulkan GLSL has no gl_VertexID; gl_VertexIndex equals it here because the
 * triangle is always drawn from vertex 0, as ssao_composite_vert says. */
#include "layout.inc"

#ifdef VULKAN
#define gl_VertexID gl_VertexIndex
#endif

VERT_OUT(0) out vec2 v_uv;

void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
