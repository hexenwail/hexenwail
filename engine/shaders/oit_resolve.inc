/* oit_resolve.inc -- what the two OIT resolve fragment stages share: the
 * OitParams block that replaced their loose `uniform int u_mboit` (#450),
 * the output, and max3.
 *
 * OitParams is one ivec4 (16 bytes, std140), mirrored by oit_params_t in
 * gl_postprocess.c.  GL binding point 7 (OIT_PARAMS_UBO_BINDING), assigned
 * from C because ES 3.00 cannot put layout(binding=) on a block; FRAG_UBO(0)
 * is the SDL_GPU fragment-stage slot.  u_mboit stays as an alias, so the
 * bodies are unchanged: non-zero means TexReveal holds MBOIT's absorbance
 * b0 (#87) rather than WBOIT revealage.
 */
#ifndef OIT_RESOLVE_INC
#define OIT_RESOLVE_INC

#include "layout.inc"

FRAG_UBO(0) uniform OitParams {
    ivec4 u_oitp;	/* x: MBOIT (1) or WBOIT (0) */
};
#define u_mboit	u_oitp.x

FRAG_OUT(0) out vec4 out_fragcolor;

float max3(vec3 v) { return max(max(v.x, v.y), v.z); }

#endif /* OIT_RESOLVE_INC */
