/* soit_resolve -- composite the OIT accumulation over the opaque scene.
 * No stencil gate, no early_fragment_tests: WBOIT handles empty pixels via
 * math (accum=0, revealage=1 -> alpha=0 -> scene unchanged).
 * Moved from gl_postprocess.c's oit_resolve_frag (#450).
 * No #version line; gl_postprocess.c passes PP_FRAG_HEADER.
 */
#include "oit_resolve.inc"

FRAG_TEX(0) uniform sampler2D TexAccum;
FRAG_TEX(1) uniform sampler2D TexReveal;

void main() {
    ivec2 coords = ivec2(gl_FragCoord.xy);
    float revealage = texelFetch(TexReveal, coords, 0).r;
    /* MBOIT: the scene shows through what the absorbance b0 leaves */
    if (u_mboit != 0) revealage = exp(-max(revealage, 0.0));
    vec4 accumulation = texelFetch(TexAccum, coords, 0);
    if (isinf(max3(abs(accumulation.rgb))))
        accumulation.rgb = vec3(accumulation.a);
    vec3 average_color = accumulation.rgb / max(accumulation.a, 1e-5);
    out_fragcolor = vec4(average_color, 1.0 - revealage);
}
