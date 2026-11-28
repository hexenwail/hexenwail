/* hiz_copy (fragment) -- the scene depth into mip 0 of the Hi-Z pyramid,
 * drawn with ssao_composite.vert's full-screen triangle into a framebuffer
 * whose colour attachment is that level.  -gpu only (#340 P5f): GL makes
 * the same copy with hiz_copy.comp, and never builds this program.
 *
 * Why a draw there.  SDL 3.4.12 hands a texture back to SAMPLER after a
 * render pass with a barrier whose second scope is the vertex and fragment
 * stages, so a compute stage that samples the scene depth races the pass
 * that wrote it, and has to submit and wait first.  A fragment stage is in
 * that scope, and its colour output is read by the next level's reduce as a
 * read-only storage texture, which SDL's barriers do order for compute.
 * Depth formats cannot be storage textures, so the copy is the one step
 * that has to leave compute.
 *
 * Every render target holds GL's row order (VK_FLIP_Y), the depth and the
 * pyramid alike, so the texel under gl_FragCoord in one is the texel at the
 * same coordinate in the other.  No #version: gl_shader.c supplies it. */
#include "layout.inc"

FRAG_TEX(0) uniform sampler2D u_scene_depth;

FRAG_OUT(0) out vec4 out_depth;

void main ()
{
	out_depth = vec4 (texelFetch (u_scene_depth, ivec2 (gl_FragCoord.xy), 0).r, 0.0, 0.0, 0.0);
}
