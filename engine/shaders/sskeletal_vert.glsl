/* sskeletal_vert -- the SKELETAL alias vertex stage (PV_IQM, the MD5mesh
 * models gl_model.c loads beside a .mdl): bone-weighted skinning, two poses
 * blended per draw.  Moved from gl_shader.c's sskeletal_vert (#450, slice S4
 * of the alias-family plan on that issue).  Linked with salias_frag.glsl by
 * skeletal, its affine twin skeletal_np, and both OIT variants.
 *
 * DESKTOP ONLY.  The bone matrices arrive in a shader storage block, which is
 * GLSL ES 3.10, and the ES tier runs ES 3.00, so gl_shader.c never builds this
 * program under USE_GLES (uhexen2-dfay).
 *
 * No #version line; gl_shader.c passes GLSL_VERT_HEADER, with
 * "#define NOPERSP 1" appended for the affine (r_softemu) twins.
 *
 *   storage  bones       GL 4  numposes x numbones 3x4 matrices, GL_DrawAliasSkeletal
 *   blocks   VertParams  u_mvp, u_modelview, u_alias_model, u_alias_dlight,
 *                        u_pose_base, u_pose_base2, u_skel_blend
 *            AliasVert   u_shadevector, u_fullbright, u_lightcolor (alias.inc)
 *
 * The ten non-opaque uniforms were loose; GL_DrawAliasSkeletal writes all of
 * them through the R_Set* shadows, every draw, before its flush.
 *
 * invariant gl_Position, for the reason salias_vert.glsl gives: the additive
 * fullbright redraw has to land on the depth the base pass wrote
 * (uhexen2-iir3).
 */
#include "layout.inc"
#define UNIFORMS_VERT
#include "uniforms.inc"
#include "alias.inc"

/* Affine texture mapping, the way the software rasteriser did it
 * (uhexen2-ktjv).  An interpolation qualifier is not something a uniform can
 * switch, so this is a compile-time variant -- the only stage of r_softemu
 * that needs one.  Empty object-like macro when NOPERSP is not defined, so the
 * qualifier simply is not there. */
#ifdef NOPERSP
#define TCQUAL noperspective
#else
#define TCQUAL
#endif

/* The IQM VAO's slots (GL_MakeAliasGPUMesh in gl_mesh.c), NOT the ATTR_*
 * contract every other program shares: a_texcoord is 2 here where ATTR_TEXCOORD
 * is 1.  A layout qualifier overrides the glBindAttribLocation GL_LinkProgram
 * applies, so this stays local to the skeletal programs.  uhexen2-bynk. */
VERT_IN(0) in vec3 a_position;
VERT_IN(1) in vec4 a_normal;
VERT_IN(2) in vec2 a_texcoord;
VERT_IN(3) in vec4 a_weights;
VERT_IN(4) in uvec4 a_indices;

/* Vulkan index 0: the vertex stage samples no texture, so the storage buffer
 * is the first resource in set 0. */
VERT_SSBO(4, 0) restrict readonly buffer BoneBuffer {
    mat3x4 bones[];
};

/* The varyings must match salias_frag.glsl by name (the plain arm of VERT_OUT
 * emits no location) and, for SPIR-V, by location;
 * tools/test_alias_vert_block.py checks every stage that links it. */
VERT_OUT(0) TCQUAL out vec2 v_texcoord;
VERT_OUT(1) out vec4 v_color;
VERT_OUT(2) out float v_fogdist;
VERT_OUT(3) out vec2 v_worldxy;
/* Eye space, as salias_vert.glsl's (uhexen2-4kcb). */
VERT_OUT(4) out highp vec3 v_matpos;
/* Clustered dynamic lighting on alias models (uhexen2-waum).  No #ifndef
 * GL_ES around these, unlike salias_vert.glsl: this stage has no ES arm. */
VERT_OUT(5) out highp vec3 v_eyepos;
VERT_OUT(6) flat out float v_dlightscale;

invariant gl_Position;

/* Bone matrix for influence `i`, already blended across the two poses.
 * u_pose_base is the pose R_AliasResolveLerp resolved as current, u_pose_base2
 * the one it is blending away from, both already multiplied by numbones, and
 * u_skel_blend runs 0..1 from the latter to the former (uhexen2-7ok0.3).
 * mat3x4 supports scalar multiply and matrix add, so the blend is a plain
 * linear combination -- mix() is genType-only and does not accept matrices.
 * Linear blending of two adjacent keyframes' skinning matrices is the standard
 * approximation and is what the equivalent vertex-lerp does on the trivertx
 * path. */
mat3x4 BonePose(uint i) {
    return bones[u_pose_base  + int(i)] * u_skel_blend +
           bones[u_pose_base2 + int(i)] * (1.0 - u_skel_blend);
}

void main() {
    mat3x4 blend = BonePose(a_indices.x) * a_weights.x;
    if (a_weights.y > 0.0) blend += BonePose(a_indices.y) * a_weights.y;
    if (a_weights.z > 0.0) blend += BonePose(a_indices.z) * a_weights.z;
    if (a_weights.w > 0.0) blend += BonePose(a_indices.w) * a_weights.w;

    mat4x3 mat_3x4 = transpose(blend);
    vec3 skinned_pos = mat_3x4 * vec4(a_position, 1.0);
    vec3 skinned_normal = normalize(mat_3x4 * vec4(a_normal.xyz, 0.0));

    /* The legacy path looks up shadedots[lightnormalindex], a table of
     * 1 + dot(normal, shadevector) with negative dots scaled by 0.3.  A
     * skinned vertex has a real normal rather than an index into Quake's
     * 162-normal set, so evaluate the same expression directly and the two
     * paths agree on the same model to within the table's quantization.
     * u_shadevector is in MODEL space, matching the -e->angles[1]
     * construction in R_DrawAliasModel; u_lightcolor.rgb carries the tint and
     * R_AliasLightScale folded in, .a the entity alpha. */
    float d = dot(skinned_normal, u_shadevector);
    if (d < 0.0) d *= 0.3;
    vec3 lit = u_lightcolor.rgb * (1.0 + d);

    v_texcoord = a_texcoord;
    v_color = vec4(mix(lit, vec3(1.0), u_fullbright), u_lightcolor.a);
    v_worldxy = (u_alias_model * vec4(skinned_pos, 1.0)).xy;
    vec4 eyepos = u_modelview * vec4(skinned_pos, 1.0);
    v_fogdist = length(eyepos.xyz);
    v_matpos = eyepos.xyz;
    v_eyepos = eyepos.xyz;
    v_dlightscale = u_alias_dlight;
    gl_Position = u_mvp * vec4(skinned_pos, 1.0);
    VK_FLIP_Y()
}
