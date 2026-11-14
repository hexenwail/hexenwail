/* bloom.inc -- what the three bloom fragment stages share: the source texture
 * and the BloomParams block that replaced their loose uniforms (#450).
 *
 * BloomParams is one vec4 (16 bytes, std140), mirrored by bloom_params_t in
 * gl_postprocess.c.  GL binding point 6 (BLOOM_PARAMS_UBO_BINDING), assigned
 * from C because ES 3.00 cannot put layout(binding=) on a block; FRAG_UBO(0)
 * is the SDL_GPU fragment-stage slot.  The aliases keep the old uniform names,
 * so the stage bodies are unchanged.
 */
#ifndef BLOOM_INC
#define BLOOM_INC

#include "layout.inc"

FRAG_UBO(0) uniform BloomParams {
    vec4 u_bloomp;	/* xy reciprocal source size, z bright-pass threshold */
};
#define u_rcpframe	u_bloomp.xy
#define u_threshold	u_bloomp.z

FRAG_TEX(0) uniform sampler2D u_scene;
FRAG_IN(0) in vec2 v_uv;
FRAG_OUT(0) out vec4 fragColor;

#endif /* BLOOM_INC */
