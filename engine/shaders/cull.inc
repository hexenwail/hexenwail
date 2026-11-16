/* cull.inc -- the CullParams block shared by the three worldcull compute
 * programs (cull_clear_comp, cull_mark_comp, hiz_reduce_comp).
 *
 * One copy, so the three programs cannot disagree about it.  The C mirror is
 * cull_params_t in gl_worldcull.c (224 bytes, COMPILE_TIME_ASSERT), and
 * tools/test_cullparams_block.py checks the two member for member: an
 * std140 offset mismatch compiles clean and culls wrong.
 *
 * Every member is vec4/ivec4/mat4 so the std140 layout is exact by
 * construction.
 *
 * Binding: GL point 4 (CULL_PARAMS_UBO_BINDING), assigned from C with
 * glUniformBlockBinding, because the plain-GLSL arm of UBO() cannot name a
 * block binding.  UBO(0) is the SDL_GPU slot: CullParams is each pipeline's
 * only uniform buffer.  Same split as SsaoParams (GL 3, slot 0).
 */
#ifndef CULL_INC
#define CULL_INC

#include "layout.inc"

UBO(0) uniform CullParams {
    ivec4 u_ccounts0;	/* x framecount, y num_marksurfs, z num_buckets, w max_dst_indices */
    ivec4 u_ccounts1;	/* x hiz_mip_count, y hiz_enable, z reverse_z, w stats_enable */
    vec4 u_cvieworg;	/* xyz view origin */
    vec4 u_cfrustum[4];	/* frustum planes (normal.xyz, -dist) */
    mat4 u_cprevmvp;	/* previous-frame MVP for Hi-Z reprojection */
    vec4 u_chizsize;	/* xy Hi-Z pyramid size */
    ivec4 u_creduce;	/* x src_w, y src_h, z dst_w, w dst_h */
    ivec4 u_creduce2;	/* x src_lod, y reverse_z */
};

#endif /* CULL_INC */
