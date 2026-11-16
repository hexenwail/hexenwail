/* soit_resolve_msaa -- per-sample OIT resolve via sampler2DMS + gl_SampleID,
 * used when the accum/revealage targets are GL_TEXTURE_2D_MULTISAMPLE.
 * Moved from gl_postprocess.c's oit_resolve_frag_msaa (#450).
 *
 * DESKTOP ONLY: sampler2DMS and gl_SampleID are GLSL ES 3.10, and the ES
 * tier never builds this program (gl_postprocess.c compiles it under
 * #ifndef USE_GLES).  It is still a file so the SDL_GPU backend has it.
 * No #version line; gl_postprocess.c passes PP_FRAG_HEADER.
 */
#include "oit_resolve.inc"

FRAG_TEX(0) uniform sampler2DMS TexAccum;
FRAG_TEX(1) uniform sampler2DMS TexReveal;

void main() {
    ivec2 coords = ivec2(gl_FragCoord.xy);
    float revealage = texelFetch(TexReveal, coords, gl_SampleID).r;
    if (u_mboit != 0) revealage = exp(-max(revealage, 0.0));
    vec4 accumulation = texelFetch(TexAccum, coords, gl_SampleID);
    if (isinf(max3(abs(accumulation.rgb))))
        accumulation.rgb = vec3(accumulation.a);
    vec3 average_color = accumulation.rgb / max(accumulation.a, 1e-5);
    out_fragcolor = vec4(average_color, 1.0 - revealage);
}
