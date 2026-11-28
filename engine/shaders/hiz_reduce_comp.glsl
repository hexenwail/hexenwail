/* hiz_reduce -- 2x2 reduction from mip N -> mip N+1 of the Hi-Z pyramid.
 * Under reversed-Z (u_creduce2.y != 0) the conservative occluder (the
 * farthest point in eye space) has the smallest stored value, so we take
 * min; under forward-Z, max.
 *
 * No #version line: gl_worldcull.c passes HIZ_COMPUTE_VERSION as a separate
 * glShaderSource chunk, and EmbedShaders.cmake prepends its own for the
 * offline SPIR-V compile.  Moved from gl_worldcull.c's hiz_reduce_src (#450).
 *
 * -gpu reads the level below as a read-only storage texture, a texture2D
 * with no sampler (#340 P5f).  SDL 3.4.12 orders a sampled read only for the
 * vertex and fragment stages, so a sampling dispatch had to submit and wait
 * for the level the one before it wrote: 11 waits a frame for the pyramid.
 * A read-only storage texture is put in its layout by a barrier whose second
 * scope is the compute stage, so the level is ordered with none.  texelFetch
 * reads the same texel either way; GL's program is the sampler arm, as it
 * was.
 */
#ifdef VULKAN
#extension GL_EXT_samplerless_texture_functions : require
#endif
#include "cull.inc"

layout(local_size_x = 8, local_size_y = 8) in;
#ifdef VULKAN
TEXBIND(0) uniform texture2D u_src;
#else
TEXBIND(0) uniform sampler2D u_src;
#endif
IMGBIND(0, r32f) uniform writeonly image2D u_dst;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= u_creduce.z || p.y >= u_creduce.w) return;
    ivec2 s = p * 2;
    ivec2 a = clamp(s + ivec2(0,0), ivec2(0), u_creduce.xy - ivec2(1));
    ivec2 b = clamp(s + ivec2(1,0), ivec2(0), u_creduce.xy - ivec2(1));
    ivec2 c = clamp(s + ivec2(0,1), ivec2(0), u_creduce.xy - ivec2(1));
    ivec2 d = clamp(s + ivec2(1,1), ivec2(0), u_creduce.xy - ivec2(1));
    float va = texelFetch(u_src, a, u_creduce2.x).r;
    float vb = texelFetch(u_src, b, u_creduce2.x).r;
    float vc = texelFetch(u_src, c, u_creduce2.x).r;
    float vd = texelFetch(u_src, d, u_creduce2.x).r;
    float r;
    if (u_creduce2.y != 0) r = min(min(va,vb), min(vc,vd));
    else                  r = max(max(va,vb), max(vc,vd));
    /* odd-mip safety: if the parent dim is odd we also need to fold
     * in the dangling row/column so coverage stays conservative. */
    if (((u_creduce.x & 1) != 0) && (s.x + 2 < u_creduce.x)) {
        ivec2 e = ivec2(s.x + 2, s.y);
        ivec2 f = ivec2(s.x + 2, min(s.y + 1, u_creduce.y - 1));
        float ve = texelFetch(u_src, e, u_creduce2.x).r;
        float vf = texelFetch(u_src, f, u_creduce2.x).r;
        if (u_creduce2.y != 0) r = min(r, min(ve, vf));
        else                  r = max(r, max(ve, vf));
    }
    if (((u_creduce.y & 1) != 0) && (s.y + 2 < u_creduce.y)) {
        ivec2 e = ivec2(s.x, s.y + 2);
        ivec2 f = ivec2(min(s.x + 1, u_creduce.x - 1), s.y + 2);
        float ve = texelFetch(u_src, e, u_creduce2.x).r;
        float vf = texelFetch(u_src, f, u_creduce2.x).r;
        if (u_creduce2.y != 0) r = min(r, min(ve, vf));
        else                  r = max(r, max(ve, vf));
    }
    imageStore(u_dst, p, vec4(r, 0.0, 0.0, 0.0));
}
