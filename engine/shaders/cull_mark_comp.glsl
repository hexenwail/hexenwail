/* cull_mark -- one invocation per marksurface: PVS, flag, frustum and Hi-Z
 * tests, dedup across leaves, then append the surviving surface's indices to
 * its texture bucket's indirect draw.
 *
 * No #version line: gl_worldcull.c passes HIZ_COMPUTE_VERSION as a separate
 * glShaderSource chunk, and EmbedShaders.cmake prepends its own for the
 * offline SPIR-V compile.  Moved from gl_worldcull.c's cull_mark_src (#450).
 *
 * Bindings: the first number is the GL point gl_worldcull.c binds with
 * glBindBufferBase / glActiveTexture, the second the SDL_GPU slot (see
 * TEXBIND2/SSBO_RO/SSBO_RW in layout.inc).  Set 0: the pyramid, then the
 * four read-only buffers; set 1: the four written ones.
 */
#include "cull.inc"

layout(local_size_x = 64) in;

struct Surface {
    vec4 plane;
    vec4 mins;
    vec4 maxs;
    int tex_bucket;
    int firstindex;
    int numindices;
    int flags;
};

struct MarkSurf {
    int leaf_idx;
    int surf_idx;
};

struct IndirectCmd {
    uint count;
    uint instanceCount;
    uint firstIndex;
    uint baseVertex;
    uint baseInstance;
};

SSBO_RO(0, 1) readonly buffer SurfaceBuffer {
    Surface surfaces[];
};
SSBO_RO(1, 2) readonly buffer MarkSurfBuffer {
    MarkSurf marksurfs[];
};
SSBO_RO(2, 3) readonly buffer VisBuffer {
    uint vis[];
};
SSBO_RW(3, 0) buffer IndirectBuffer {
    IndirectCmd cmds[];
};
SSBO_RO(4, 4) readonly buffer SrcIndexBuffer {
    uint src_indices[];
};
SSBO_RW(5, 1) writeonly buffer DstIndexBuffer {
    uint dst_indices[];
};
SSBO_RW(6, 2) buffer DedupBuffer {
    int dedup[];  /* per-surface framecount for dedup */
};
/* Stats counters for cull-rate validation (uhexen2-cyu0).  Indices:
 * 0 total, 1 pvs_rej, 2 flags_rej, 3 frust_rej, 4 hiz_rej, 5 accepted,
 * 6 dedup_rej, 7 reserved.  Writes gated by u_ccounts1.w to keep
 * cost zero in production. */
SSBO_RW(7, 3) buffer StatsBuffer {
    uint stats[8];
};

/* Hi-Z pyramid bound to texture unit 1.  Under reversed-Z the
 * pyramid stores min(depth) per tile (= farthest fragment), under
 * forward-Z max(depth). */
TEXBIND2(1, 0) uniform sampler2D u_hiz_pyramid;

#define STAT_INC(i) do { if (u_ccounts1.w != 0) atomicAdd(stats[i], 1u); } while(false)

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= uint(u_ccounts0.y)) return;
    STAT_INC(0);

    MarkSurf ms = marksurfs[id];
    int leaf = ms.leaf_idx;
    int si = ms.surf_idx;

    /* PVS test */
    if ((vis[leaf >> 5] & (1u << (leaf & 31))) == 0u) {
        STAT_INC(1); return;
    }

    Surface s = surfaces[si];

    /* Skip non-lightmapped and special surfaces:
     * sky(0x4) turb(0x10) drawtiled(0x20) drawblack(0x100)
     * underwater(0x200) fence(0x800) */
    if ((s.flags & 0xB34) != 0) { STAT_INC(2); return; }

    /* Skip surfaces with no VBO data */
    if (s.numindices <= 0) { STAT_INC(2); return; }

    /* Backface cull — disabled to avoid popping at grazing angles.
     * GPU rasterizer handles backfaces via winding order. */
    /* float d = dot(s.plane.xyz, u_cvieworg.xyz) - s.plane.w;
    bool planeback = (s.flags & 2) != 0;
    if (planeback && d > 0.01) return;
    if (!planeback && d < -0.01) return; */

    /* Frustum cull with generous padding — reject surfaces well
     * outside the view frustum to limit PVS over-draw.  Without
     * this, all PVS-visible surfaces are drawn regardless of view
     * angle, causing abrupt visual shifts through translucent
     * water when crossing leaf boundaries.  The 512-unit slack
     * prevents surface popping at frustum edges. */
    const float FSLACK = 512.0;
    for (int fi = 0; fi < 4; fi++) {
        vec3 fn = u_cfrustum[fi].xyz;
        float fd = u_cfrustum[fi].w;
        vec3 pv = vec3(
            fn.x >= 0.0 ? s.maxs.x : s.mins.x,
            fn.y >= 0.0 ? s.maxs.y : s.mins.y,
            fn.z >= 0.0 ? s.maxs.z : s.mins.z);
        if (dot(fn, pv) + fd < -FSLACK) { STAT_INC(3); return; }
    }

    /* Hi-Z occlusion test against previous-frame pyramid.
     * Project the 8 AABB corners through last frame's MVP and form
     * a conservative screen-space rect + nearest-to-camera depth.
     * Sample the pyramid at a mip whose texel size matches the rect,
     * compare; cull if the AABB sits entirely behind the occluder.
     * Any corner with w<=0 (behind near plane) disables the test. */
    if (u_ccounts1.y != 0) {
        vec3 mn = s.mins.xyz;
        vec3 mx = s.maxs.xyz;
        float rx0 = 1.0, ry0 = 1.0, rx1 = -1.0, ry1 = -1.0;
        float dnear = (u_ccounts1.z != 0) ? 0.0 : 1.0;
        bool valid = true;
        for (int ci = 0; ci < 8; ci++) {
            vec3 c = vec3(
                ((ci & 1) != 0) ? mx.x : mn.x,
                ((ci & 2) != 0) ? mx.y : mn.y,
                ((ci & 4) != 0) ? mx.z : mn.z);
            vec4 clip = u_cprevmvp * vec4(c, 1.0);
            if (clip.w <= 0.0) { valid = false; break; }
            vec3 ndc = clip.xyz / clip.w;
            float dn = (u_ccounts1.z != 0) ? ndc.z : (ndc.z * 0.5 + 0.5);
            if (u_ccounts1.z != 0) dnear = max(dnear, dn);
            else                  dnear = min(dnear, dn);
            rx0 = min(rx0, ndc.x); ry0 = min(ry0, ndc.y);
            rx1 = max(rx1, ndc.x); ry1 = max(ry1, ndc.y);
        }
        /* Into the window-Z range the pyramid's frame drew the world with:
         * half of it at r_mirroralpha != 1, where R_Clear splits the range
         * for the mirror (#533).  Increasing in dnear, so the min/max above
         * still hold; exact at the usual [0, 1]. */
        dnear = u_cdepthrange.x + (u_cdepthrange.y - u_cdepthrange.x) * dnear;
        if (valid) {
            /* Clip rect to NDC bounds [-1,1] before mapping to texels. */
            rx0 = clamp(rx0, -1.0, 1.0); ry0 = clamp(ry0, -1.0, 1.0);
            rx1 = clamp(rx1, -1.0, 1.0); ry1 = clamp(ry1, -1.0, 1.0);
            vec2 r0 = (vec2(rx0, ry0) * 0.5 + 0.5) * u_chizsize.xy;
            vec2 r1 = (vec2(rx1, ry1) * 0.5 + 0.5) * u_chizsize.xy;
            float dim = max(r1.x - r0.x, r1.y - r0.y);
            if (dim > 0.5) {
                int mip = int(ceil(log2(dim)));
                if (mip < 0) mip = 0;
                if (mip > u_ccounts1.x - 1) mip = u_ccounts1.x - 1;
                vec2 cuv = (r0 + r1) * 0.5 / u_chizsize.xy;
                /* 2x2 textureLod sample at the chosen mip, reduced
                 * the same way the pyramid was built. */
                float texel = exp2(float(mip)) / max(u_chizsize.x, u_chizsize.y);
                float occ;
                float s0 = textureLod(u_hiz_pyramid, cuv + vec2(-0.5,-0.5)*texel, float(mip)).r;
                float s1 = textureLod(u_hiz_pyramid, cuv + vec2( 0.5,-0.5)*texel, float(mip)).r;
                float s2 = textureLod(u_hiz_pyramid, cuv + vec2(-0.5, 0.5)*texel, float(mip)).r;
                float s3 = textureLod(u_hiz_pyramid, cuv + vec2( 0.5, 0.5)*texel, float(mip)).r;
                if (u_ccounts1.z != 0) {
                    occ = min(min(s0,s1), min(s2,s3));
                    /* AABB's nearest depth < occluder's min => fully behind */
                    if (dnear < occ) { STAT_INC(4); return; }
                } else {
                    occ = max(max(s0,s1), max(s2,s3));
                    if (dnear > occ) { STAT_INC(4); return; }
                }
            }
        }
    }

    /* Dedup: same surface can appear in multiple leaves.
     * atomicExchange the framecount — if it was already this
     * frame's value, another thread already emitted it. */
    int old = atomicExchange(dedup[si], u_ccounts0.x);
    if (old == u_ccounts0.x) { STAT_INC(6); return; }

    STAT_INC(5);

    /* Bounds check texture bucket to prevent out-of-bounds writes */
    if (s.tex_bucket >= uint(u_ccounts0.z)) return;

    /* Claim index slots in the texture bucket's indirect command */
    uint slot = atomicAdd(cmds[s.tex_bucket].count, uint(s.numindices));
    uint dst_base = cmds[s.tex_bucket].firstIndex + slot;

    /* Bounds check destination buffer writes */
    if (dst_base + uint(s.numindices) > uint(u_ccounts0.w)) {
        /* Buffer overflow would occur - abort write */
        atomicAdd(cmds[s.tex_bucket].count, -int(s.numindices));
        return;
    }

    /* Copy indices from source to destination */
    for (int i = 0; i < s.numindices; i++)
        dst_indices[dst_base + uint(i)] = src_indices[s.firstindex + i];
}
