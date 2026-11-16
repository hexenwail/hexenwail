/* sshadow_alias_vert -- an alias model drawn into one face of a dynamic
 * light's cube shadow map (GitHub #97, P6, r_shadow_casters 2).  The depth
 * variant of the three alias vertex stages: it poses a vertex the way
 * salias_inst_vert (PV_QUAKE1, PV_MD3) or sskeletal_vert (PV_IQM) does, from
 * the same buffers, and does nothing else.  Linked with sshadow_frag, whose
 * one varying it shares with sshadow_vert.  No #version: gl_shadow.c supplies
 * the desktop one.  Desktop GL 4.3 only, like the maps.
 *
 * ShadowPass is sshadow_vert's block, member for member -- one C mirror,
 * r_shadowpass_t, uploaded by one function for both programs, and
 * tools/test_dlshadow_block.py holds both declarations to it.  The world's
 * pass ignores the members past u_shadowlight.x.
 *
 * Three formats, one uniform branch (u_shadowpose.z, gm->poseverttype):
 *
 *   0 PV_QUAKE1  pose_data[base + gl_VertexID]: 8-bit xyz, as the instanced
 *                path reads it; the header's scale and origin are in
 *                u_shadowmodel, as they are in its WorldMatrix
 *   1 PV_MD3     md3_pose_data[...]: 16-bit xyz / 64
 *   2 PV_IQM     bone-weighted a_position, the two poses blended per bone
 *
 * Same buffers at the same GL bindings as the stages they mirror (1, 3, 4), so
 * a mesh's buffers are used as they are.  gl_VertexID is the mesh's own index,
 * as in salias_inst_vert: the IBO is drawn with no base vertex.  The skeletal
 * attributes are the IQM VAO's slots (0, 3, 4, sskeletal_vert's); the other
 * two formats' VAO leaves them disabled, and that arm never reads them.
 *
 * PUSHED AWAY FROM THE LIGHT.  The pass keeps the faces turned away from the
 * light (gl_shadow.c, SH_CULL_FACE), so a model stores its own far side -- for
 * the world that makes the wall's thickness the bias.  An alias receiver has
 * no one-sided test before its lookup (ClusteredAliasDlight has no N.L; the
 * whole model takes a light, as vanilla's per-entity term did), so the far
 * side of the model compares against itself, and at a few texels per triangle
 * that is acne.  Each vertex is moved u_shadowlight.y + u_shadowlight.z * |r|
 * units further along its own ray from the light.  That leaves the map's
 * coverage exactly as it was -- a point slid along the ray through the light
 * projects to the same texel of every face -- and only deepens what it stores,
 * so the model's far side passes and a wall more than that far behind the
 * model is still shadowed.
 */
#include "layout.inc"

VERT_UBO(0) uniform ShadowPass {
    highp mat4 u_shadowvp;      /*   0  light-relative position -> this face's clip space */
    highp mat4 u_shadowmodel;   /*  64  model -> world, minus the light origin */
    highp vec4 u_shadowlight;   /* 128  x 1 / the radius the map is drawn with;
                                 *      y push in units, z push per unit of distance,
                                 *      w weight of pose 0 (alias only) */
    highp ivec4 u_shadowpose;   /* 144  x pose 0 base, y pose 1 base, z format (alias only) */
};                              /* 160 bytes */

VERT_SSBO(1, 0) restrict readonly buffer PoseBuffer {
    uint pose_data[];
};

VERT_SSBO(3, 1) restrict readonly buffer MD3PoseBuffer {
    uvec2 md3_pose_data[];  /* short xyz[3] + ubyte normal[2], two uints */
};

VERT_SSBO(4, 2) restrict readonly buffer BoneBuffer {
    mat3x4 bones[];
};

VERT_IN(0) in vec3 a_position;
VERT_IN(3) in vec4 a_weights;
VERT_IN(4) in uvec4 a_indices;

/* sshadow_vert's varying: the light-relative position over the radius. */
VERT_OUT(0) out highp vec3 v_lrel;

#ifdef VULKAN
#define gl_VertexID gl_VertexIndex
#endif

vec3 MD3_Position(uvec2 v) {
    int x = int(v.x & 0xFFFFu), y = int((v.x >> 16) & 0xFFFFu), z = int(v.y & 0xFFFFu);
    if (x > 32767) x -= 65536;
    if (y > 32767) y -= 65536;
    if (z > 32767) z -= 65536;
    return vec3(x, y, z) / 64.0;
}

mat3x4 BonePose(uint i) {
    return bones[u_shadowpose.x + int(i)] * u_shadowlight.w +
           bones[u_shadowpose.y + int(i)] * (1.0 - u_shadowlight.w);
}

void main() {
    vec3 local;
    if (u_shadowpose.z == 2) {          /* PV_IQM */
        mat3x4 b = BonePose(a_indices.x) * a_weights.x;
        if (a_weights.y > 0.0) b += BonePose(a_indices.y) * a_weights.y;
        if (a_weights.z > 0.0) b += BonePose(a_indices.z) * a_weights.z;
        if (a_weights.w > 0.0) b += BonePose(a_indices.w) * a_weights.w;
        local = transpose(b) * vec4(a_position, 1.0);
    } else if (u_shadowpose.z == 1) {   /* PV_MD3 */
        local = mix(MD3_Position(md3_pose_data[u_shadowpose.y + gl_VertexID]),
                    MD3_Position(md3_pose_data[u_shadowpose.x + gl_VertexID]), u_shadowlight.w);
    } else {                            /* PV_QUAKE1 */
        uint p0 = pose_data[u_shadowpose.x + gl_VertexID];
        uint p1 = pose_data[u_shadowpose.y + gl_VertexID];
        vec3 v0 = vec3(float(p0 & 0xFFu), float((p0 >> 8) & 0xFFu), float((p0 >> 16) & 0xFFu));
        vec3 v1 = vec3(float(p1 & 0xFFu), float((p1 >> 8) & 0xFFu), float((p1 >> 16) & 0xFFu));
        local = mix(v1, v0, u_shadowlight.w);
    }
    highp vec3 rel = (u_shadowmodel * vec4(local, 1.0)).xyz;
    highp float len = length(rel);
    if (len > 0.0)
        rel *= 1.0 + (u_shadowlight.y + u_shadowlight.z * len) / len;
    v_lrel = rel * u_shadowlight.x;
    gl_Position = u_shadowvp * vec4(rel, 1.0);
}
