/* salias_inst_vert -- the INSTANCED alias vertex stage (r_alias_gpu 1):
 * per-instance data, poses and the shadedots table come from storage buffers,
 * and one glDrawElementsInstanced draws every entity sharing a (model, skin)
 * batch.  Moved from gl_shader.c's salias_inst_vert (#450, slice S3 of the
 * alias-family plan on that issue).  Linked with salias_frag.glsl by
 * alias_instanced and its affine twin alias_instanced_np.
 *
 * DESKTOP ONLY.  Shader storage blocks are GLSL ES 3.10, and the ES tier runs
 * ES 3.00, so gl_shader.c never builds this program under USE_GLES.
 *
 * No #version line; gl_shader.c passes GLSL_ALIAS_INST_VERT_HEADER, which is
 * GLSL_VERT_HEADER plus the four ShadedotRow constants below, with
 * "#define NOPERSP 1" appended for the affine (r_softemu) twin.
 *
 *   storage  instances  GL 0  per-frame InstanceData, streamed by GL_Upload
 *            poses      GL 1  the batch model's PV_QUAKE1 poses
 *            shadedots  GL 2  r_avertexnormal_dots, uploaded once
 *            md3 poses  GL 3  the batch model's PV_MD3 poses
 *   blocks   VertParams      u_viewproj, u_eyepos, u_inst_base,
 *                            u_poseverttype, u_alias_dlight
 *            AliasVert       u_lightview (alias.inc)
 *
 * The six non-opaque uniforms were loose; R_DrawAliasInstanced writes the
 * VertParams five through the R_Set* shadows and R_LightCluster_BindForAlias
 * writes u_lightview once a frame.  u_inst_base changes per batch, so each
 * batch flushes before its draw.
 *
 * invariant gl_Position, for the reason salias_vert.glsl gives: the additive
 * fullbright redraw has to land on the depth the base pass wrote
 * (uhexen2-iir3).
 */
#include "layout.inc"
#define UNIFORMS_VERT
#include "uniforms.inc"
#include "alias.inc"

/* InstanceData.ShadedotRow carries the shadedots row (0..15) in its low bits,
 * the per-instance dynamic-light switch in bit 8, the glow redraw's mark in
 * bit 9 and the entity yaw (1/65536 turns) in bits 16..31 -- ALIAS_INST_* in
 * gl_shader.h, which reach the engine's compile through the header chunk.
 * These defaults exist for the offline SPIR-V compile only, and
 * tools/test_alias_inst_vert_block.py keeps them equal to the C values. */
#ifndef ALIAS_INST_SHADEDOT_ROW_MASK
  #define ALIAS_INST_SHADEDOT_ROW_MASK	255
  #define ALIAS_INST_DLIGHT_BIT		256
  #define ALIAS_INST_FULLBRIGHT_BIT	512
  #define ALIAS_INST_YAW_SHIFT		16
#endif

/* Vulkan GLSL has gl_VertexIndex / gl_InstanceIndex instead.  gl_VertexIndex
 * equals gl_VertexID for a draw with no base vertex, which is every draw
 * here.  gl_InstanceIndex includes the draw's first instance where
 * gl_InstanceID does not, so the alias is exact only while that is 0: a
 * backend that passes batch->first as the first instance must leave
 * u_inst_base at 0, or the offset is applied twice.  Without the aliases the
 * -V compile, and with it shaders_spirv, fails. */
#ifdef VULKAN
#define gl_VertexID gl_VertexIndex
#define gl_InstanceID gl_InstanceIndex
#endif

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

/* 80 bytes, Ironwail's compact layout; R_CollectAliasInstance's
 * alias_instance_t is the C side.  Scale and origin are baked into the world
 * matrix CPU-side. */
struct InstanceData {
    vec4 WorldMatrix0;
    vec4 WorldMatrix1;
    vec4 WorldMatrix2;
    vec4 LightAlpha;        /* rgb = light_color, a = alpha */
    int Pose0;
    int Pose1;
    float Blend;
    int ShadedotRow;
};

VERT_SSBO(0, 0) restrict readonly buffer InstanceBuffer {
    InstanceData instances[];
};

VERT_SSBO(1, 1) restrict readonly buffer PoseBuffer {
    uint pose_data[];
};

VERT_SSBO(2, 2) restrict readonly buffer ShadeDots {
    float shadedots[4096];
};

VERT_SSBO(3, 3) restrict readonly buffer MD3PoseBuffer {
    uvec2 md3_pose_data[];  /* short xyz[3] + ubyte normal[2] packed as two uint32s */
};

VERT_IN(1) in vec2 a_texcoord;

/* The varyings must match salias_frag.glsl by name (the plain arm of VERT_OUT
 * emits no location) and, for SPIR-V, by location;
 * tools/test_alias_vert_block.py checks both stages. */
VERT_OUT(0) TCQUAL out vec2 v_texcoord;
VERT_OUT(1) out vec4 v_color;
VERT_OUT(2) out float v_fogdist;
VERT_OUT(3) out vec2 v_worldxy;
/* WORLD space here, not eye -- this path has no u_modelview, and the one
 * eye-space vector it does build (v_eyepos) goes through u_lightview, which
 * only gl_lightcluster.c writes and only while clustered lighting is live.
 * u_eyepos is unconditional, so camera-relative world space is the frame that
 * is always correct.  C uploads u_alias_lightdir in world space for this
 * program to match; the tangent-space maths is invariant under the pure
 * rotation between the two frames.  uhexen2-4kcb. */
VERT_OUT(4) out highp vec3 v_matpos;
/* Clustered dynamic lighting on alias models (uhexen2-waum).  No #ifndef
 * GL_ES around these, unlike salias_vert.glsl: this stage has no ES arm. */
VERT_OUT(5) out highp vec3 v_eyepos;
VERT_OUT(6) flat out float v_dlightscale;

invariant gl_Position;

vec3 MD3_DecodePosition(uvec2 vdata) {
    int x = int((vdata.x) & 0xFFFFu);
    int y = int((vdata.x >> 16) & 0xFFFFu);
    int z = int((vdata.y) & 0xFFFFu);
    if (x > 32767) x -= 65536;
    if (y > 32767) y -= 65536;
    if (z > 32767) z -= 65536;
    return vec3(x, y, z) / 64.0;
}

/* Byte 6 is longitude and byte 7 is latitude: the file stores one
 * little-endian short whose LOW byte is longitude (Quake III tr_surface.c,
 * lat = normal >> 8).  This had the two the wrong way round, which nothing
 * caught because until uhexen2-2ah9 no loader existed to reach it.
 * md3mesh.c's MD3_DecodeNormal is the CPU twin of this and must stay in
 * step. */
vec3 MD3_DecodeNormal(uvec2 vdata) {
    uint lngbyte = (vdata.y >> 16) & 0xFFu;
    uint latbyte = (vdata.y >> 24) & 0xFFu;
    float lat = float(latbyte) * (3.14159265 / 128.0);
    float lng = float(lngbyte) * (3.14159265 / 128.0);
    float sinlng = sin(lng);
    return vec3(sinlng * cos(lat), sinlng * sin(lat), cos(lng));
}

void main() {
    InstanceData inst = instances[u_inst_base + gl_InstanceID];
    vec3 local_pos, normal;
    uint ni;

    if (u_poseverttype == 1) {  /* PV_MD3 */
        uvec2 p0 = md3_pose_data[inst.Pose0 + gl_VertexID];
        uvec2 p1 = md3_pose_data[inst.Pose1 + gl_VertexID];
        vec3 v0 = MD3_DecodePosition(p0);
        vec3 v1 = MD3_DecodePosition(p1);
        vec3 n0 = MD3_DecodeNormal(p0);
        vec3 n1 = MD3_DecodeNormal(p1);
        local_pos = mix(v1, v0, inst.Blend);
        normal = normalize(mix(n1, n0, inst.Blend));
        ni = uint(0);  /* MD3 uses vector normal, not dot-product table */
    } else {  /* PV_QUAKE1 (default) */
        uint p0 = pose_data[inst.Pose0 + gl_VertexID];
        uint p1 = pose_data[inst.Pose1 + gl_VertexID];
        vec3 v0 = vec3(float(p0 & 0xFFu), float((p0>>8)&0xFFu), float((p0>>16)&0xFFu));
        vec3 v1 = vec3(float(p1 & 0xFFu), float((p1>>8)&0xFFu), float((p1>>16)&0xFFu));
        ni = (p0 >> 24) & 0xFFu;
        local_pos = mix(v1, v0, inst.Blend);
    }

    mat4x3 world = transpose(mat3x4(
        inst.WorldMatrix0, inst.WorldMatrix1, inst.WorldMatrix2));
    vec3 world_pos = world * vec4(local_pos, 1.0);

    float sdot;
    /* MD3 carries a real normal instead of an index into the 256-entry anorm
     * table, so the shadedots row cannot be looked up.  Build the light
     * vector R_DrawAliasModel builds, from the entity yaw, and evaluate the
     * same function the table holds: 1 + d, with d scaled by 0.3 where it
     * faces away.  That is what r_avertexnormal_dots is, and it is what
     * md5mesh.c's skeletal shader and md3mesh.c's CPU path both compute --
     * the three have to agree or one model lights differently depending on
     * r_alias_gpu and on which format it was shipped in.  uhexen2-2ah9.
     *
     * The yaw is the record's own (bits 16..31), not the 16-step row: the
     * CPU path's shadevector is unquantized, and the row put a turned .md3
     * up to 22.5 degrees (one truncated step) off it under r_alias_gpu 1.
     * GitHub #492. */
    /* ShadedotRow carries the dlight bit (8), the glow mark (9) and the yaw
     * (16..31) above the row, so the mask is what keeps the QUAKE1 arm's
     * shadedots index in bounds; packing them here keeps the 80-byte
     * InstanceData layout unchanged.  uhexen2-waum, GitHub #491 / #492. */
    int sdot_row = inst.ShadedotRow & ALIAS_INST_SHADEDOT_ROW_MASK;
    if (u_poseverttype == 1) {  /* MD3: use decoded normal */
        float an = float((inst.ShadedotRow >> ALIAS_INST_YAW_SHIFT) & 0xFFFF)
                 * (6.28318531 / 65536.0);
        vec3 sv = normalize(vec3(cos(-an), sin(-an), 1.0));
        float d = dot(normalize(normal), sv);
        if (d < 0.0) d *= 0.3;
        sdot = max(1.0 + d, 0.0);
    } else {  /* QUAKE1: use shadedots table */
        int sdot_idx = sdot_row * 256 + int(ni);
        sdot = shadedots[sdot_idx];
        sdot = max(sdot, 0.0);
    }
    /* The gl_fullbrights glow redraw marks its records and sets their light to
     * a flat 1: the glow is emissive, drawn unshaded as GL_DrawAliasFrame's
     * model_fullbright_pass draws it and as Ironwail adds its FullbrightTex
     * sample after the shading (Quake/gl_shaders.h).  Shading it here made
     * every glow face 0.7x to 2x what r_alias_gpu 0 drew.  GitHub #491. */
    if ((inst.ShadedotRow & ALIAS_INST_FULLBRIGHT_BIT) != 0)
        sdot = 1.0;
    v_color = vec4(inst.LightAlpha.rgb * sdot, inst.LightAlpha.a);

    v_texcoord = a_texcoord;
    /* The instance world matrix is model-only, so world_pos is already the
     * world-space vertex -- no u_alias_model needed on this path and no change
     * to the 80-byte InstanceData layout.  uhexen2-0gn3. */
    v_worldxy = world_pos.xy;
    v_fogdist = distance(world_pos, u_eyepos);
    v_matpos = world_pos - u_eyepos;
    v_eyepos = (u_lightview * vec4(world_pos, 1.0)).xyz;
    /* Per instance, because one instanced draw mixes entities whose CPU
     * lighting came from different arms of R_DrawAliasModel's branch chain
     * and only the ordinary arm ever took dynamic light.  An MLS_ABSLIGHT or
     * EF_ROTATE entity carries the bit clear and shades exactly as before. */
    v_dlightscale = ((inst.ShadedotRow & ALIAS_INST_DLIGHT_BIT) != 0)
                  ? u_alias_dlight : 0.0;
    gl_Position = u_viewproj * vec4(world_pos, 1.0);
}
