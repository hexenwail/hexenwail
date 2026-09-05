/* hiz_copy -- copy the (sampled) scene depth into mip 0 of the R32F Hi-Z
 * pyramid.  Reads via sampler2D: DEPTH24_STENCIL8 with TEXTURE_COMPARE_MODE
 * set to NONE returns the raw depth value in [0,1] from the .r channel.
 *
 * No #version line here on purpose.  The GL side passes it as a separate
 * glShaderSource chunk (gl_worldcull.c) because the desktop and ES tiers want
 * different ones, and EmbedShaders.cmake prepends its own for the offline
 * SPIR-V compile.
 */
#include "layout.inc"

layout(local_size_x = 8, local_size_y = 8) in;

TEXBIND(0) uniform sampler2D u_scene_depth;
IMGBIND(0, r32f) uniform writeonly image2D u_dst;

/* Padded to ivec4 so std140 cannot be got wrong: a bare ivec2 would still
 * occupy 16 bytes here, but only by rule rather than by construction. */
UBO(0) uniform HizParams {
    ivec4 u_size;
};

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= u_size.x || p.y >= u_size.y) return;
    float d = texelFetch(u_scene_depth, p, 0).r;
    imageStore(u_dst, p, vec4(d, 0.0, 0.0, 0.0));
}
