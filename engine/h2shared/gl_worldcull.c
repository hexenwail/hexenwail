/* gl_worldcull.c -- GPU compute world surface culling (AZDO Phase 3)
 *
 * Replaces CPU R_RecursiveWorldNode visibility marking with GPU compute:
 * - Upload decompressed PVS bitvector per frame
 * - Compute shader performs PVS + backface + frustum cull per surface
 * - Surviving surfaces written to indirect draw buffer
 * - One glMultiDrawElementsIndirect per texture bucket
 *
 * Requires GL 4.3 (compute shaders, SSBOs, indirect draw).
 * Falls back to CPU path when unavailable.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"

#ifdef GLQUAKE
#ifndef __EMSCRIPTEN__

#include "gl_shader.h"
#include "gl_vbo.h"
#include "gl_matrix.h"
#include "gl_postprocess.h"

/* ------------------------------------------------------------------ */
/* Data structures matching GLSL std430 layout                         */
/* ------------------------------------------------------------------ */

/* Per-surface GPU data (built at map load) */
typedef struct {
	float	plane[4];	/* normal.xyz + dist */
	float	mins[4];	/* bbox min.xyz + pad */
	float	maxs[4];	/* bbox max.xyz + pad */
	int	tex_bucket;	/* texture index for indirect cmd */
	int	firstindex;	/* IBO offset (in indices) */
	int	numindices;	/* triangle count * 3 */
	int	flags;		/* SURF_* flags */
} gpu_surface_t;		/* 64 bytes */

/* Per marksurface entry: which leaf references which surface */
typedef struct {
	int	leaf_idx;	/* leaf number (for PVS lookup) */
	int	surf_idx;	/* surface number (index into gpu_surfaces) */
} gpu_marksurf_t;		/* 8 bytes */

/* GL indirect draw command (matches DrawElementsIndirectCommand) */
typedef struct {
	unsigned int	count;		/* number of indices */
	unsigned int	instanceCount;	/* always 1 */
	unsigned int	firstIndex;	/* byte offset into IBO / sizeof(uint) */
	unsigned int	baseVertex;	/* always 0 */
	unsigned int	baseInstance;	/* always 0 */
} gpu_indirect_cmd_t;		/* 20 bytes */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static GLuint	cull_surf_ssbo;		/* binding 0: gpu_surface_t[] */
static GLuint	cull_marksurf_ssbo;	/* binding 1: gpu_marksurf_t[] */
/* binding 2 (PVS bitvector) streams from the frame ring via GL_Upload —
 * no dedicated SSBO; see R_DispatchWorldCull. uhexen2-o35n. */
static GLuint	cull_indirect_buf;	/* binding 3: gpu_indirect_cmd_t[] */
static GLuint	cull_src_ibo;		/* binding 4: source indices (read-only copy) */
static GLuint	cull_dst_ibo;		/* binding 5: dest indices (compute writes, draw reads) */
static GLuint	cull_dedup_ssbo;	/* binding 6: per-surface framecount for dedup */
static int	cull_total_indices;	/* total index count across all buckets */

/* Hi-Z occlusion culling — uhexen2-xd87.
 * Previous-frame depth pyramid (latency-1) drives a per-AABB rejection
 * inside cull_mark, before the dedup atomic.  The pyramid lives in a
 * R32F mip chain copied/reduced from the scene depth texture at end of
 * the 3D pass; the cull dispatch samples it via binding 7.
 */
cvar_t	gl_hiz_cull = {"gl_hiz_cull", "1", CVAR_ARCHIVE};
/* Hi-Z cull-rate stats (uhexen2-cyu0).  N = print every N frames; 0 = off.
 * Enables 7 atomicAdd counters inside cull_mark + glGetBufferSubData
 * readback.  Used to validate the "≥10% post-frustum cull rate" gate
 * before flipping gl_hiz_cull default to 1.  Per-frame readback stalls
 * the CPU — throttle with N >= 30 for normal use. */
cvar_t	gl_hiz_stats = {"gl_hiz_stats", "0", CVAR_NONE};
static GLuint	cull_stats_ssbo;	/* binding 7: stats[8] uints */
static GLint	cull_mark_u_stats_enable;
static int	hiz_stats_frame_counter;

static GLuint	hiz_pyramid_tex;	/* binding 7 sampler in cull_mark */
static GLuint	hiz_copy_prog;		/* copy scene depth -> mip 0 */
static GLuint	hiz_reduce_prog;	/* min/max 2x2 reduction per mip */
static int	hiz_width;		/* mip 0 size in pixels */
static int	hiz_height;
static int	hiz_mip_count;
static float	hiz_prev_mvp[16];	/* world MVP captured at end of last 3D pass */
static qboolean	hiz_prev_mvp_valid;

/* Standalone depth resolve (uhexen2-9912): when the postprocess pipeline
 * is inactive (no FXAA/HDR/etc.) we still need a sampler-readable depth
 * to seed the Hi-Z pyramid.  Maintain a private depth texture + FBO and
 * blit-resolve the default framebuffer's depth into it after the 3D pass.
 * Allocated lazily — zero overhead when gl_hiz_cull is off. */
static GLuint	hiz_resolve_fbo;
static GLuint	hiz_resolve_depth_tex;
static int	hiz_resolve_w;
static int	hiz_resolve_h;

static GLuint	cull_clear_prog;	/* clear_indirect compute shader */
static GLuint	cull_mark_prog;		/* cull_mark compute shader */

static int	cull_num_surfs;		/* total world surfaces */
static int	cull_num_marksurfs;	/* total marksurface entries */
static int	cull_num_buckets;	/* texture buckets (= numtextures) */
static int	cull_num_leaves;	/* total BSP leaves */

static int	*cull_bucket_map;	/* surface index -> texture bucket */

static qboolean	cull_initialized;

/* uniform locations */
static GLint	cull_mark_u_vieworg;
static GLint	cull_mark_u_frustum;
static GLint	cull_mark_u_framecount;
static GLint	cull_mark_u_num_marksurfs;
static GLint	cull_mark_u_num_buckets;
static GLint	cull_mark_u_max_dst_indices;
static GLint	cull_mark_u_prev_mvp;
static GLint	cull_mark_u_hiz_size;
static GLint	cull_mark_u_hiz_mip_count;
static GLint	cull_mark_u_hiz_enable;
static GLint	cull_mark_u_reverse_z;

static GLint	cull_clear_u_num_buckets;

/* ------------------------------------------------------------------ */
/* Compute shader sources                                              */
/* ------------------------------------------------------------------ */

static const char clear_indirect_src[] =
	"#version 430 core\n"
	"layout(local_size_x = 64) in;\n"
	"\n"
	"struct IndirectCmd {\n"
	"    uint count;\n"
	"    uint instanceCount;\n"
	"    uint firstIndex;\n"
	"    uint baseVertex;\n"
	"    uint baseInstance;\n"
	"};\n"
	"\n"
	"layout(std430, binding = 3) buffer IndirectBuffer {\n"
	"    IndirectCmd cmds[];\n"
	"};\n"
	"\n"
	"uniform int u_num_buckets;\n"
	"\n"
	"void main() {\n"
	"    uint id = gl_GlobalInvocationID.x;\n"
	"    if (id >= uint(u_num_buckets)) return;\n"
	"    cmds[id].count = 0u;\n"
	"    cmds[id].instanceCount = 1u;\n"
	"}\n";

static const char cull_mark_src[] =
	"#version 430 core\n"
	"layout(local_size_x = 64) in;\n"
	"\n"
	"struct Surface {\n"
	"    vec4 plane;\n"
	"    vec4 mins;\n"
	"    vec4 maxs;\n"
	"    int tex_bucket;\n"
	"    int firstindex;\n"
	"    int numindices;\n"
	"    int flags;\n"
	"};\n"
	"\n"
	"struct MarkSurf {\n"
	"    int leaf_idx;\n"
	"    int surf_idx;\n"
	"};\n"
	"\n"
	"struct IndirectCmd {\n"
	"    uint count;\n"
	"    uint instanceCount;\n"
	"    uint firstIndex;\n"
	"    uint baseVertex;\n"
	"    uint baseInstance;\n"
	"};\n"
	"\n"
	"layout(std430, binding = 0) readonly buffer SurfaceBuffer {\n"
	"    Surface surfaces[];\n"
	"};\n"
	"layout(std430, binding = 1) readonly buffer MarkSurfBuffer {\n"
	"    MarkSurf marksurfs[];\n"
	"};\n"
	"layout(std430, binding = 2) readonly buffer VisBuffer {\n"
	"    uint vis[];\n"
	"};\n"
	"layout(std430, binding = 3) buffer IndirectBuffer {\n"
	"    IndirectCmd cmds[];\n"
	"};\n"
	"layout(std430, binding = 4) readonly buffer SrcIndexBuffer {\n"
	"    uint src_indices[];\n"
	"};\n"
	"layout(std430, binding = 5) writeonly buffer DstIndexBuffer {\n"
	"    uint dst_indices[];\n"
	"};\n"
	"layout(std430, binding = 6) buffer DedupBuffer {\n"
	"    int dedup[];\n"  /* per-surface framecount for dedup */
	"};\n"
	"/* Stats counters for cull-rate validation (uhexen2-cyu0).  Indices:\n"
	" * 0 total, 1 pvs_rej, 2 flags_rej, 3 frust_rej, 4 hiz_rej, 5 accepted,\n"
	" * 6 dedup_rej, 7 reserved.  Writes gated by u_stats_enable to keep\n"
	" * cost zero in production. */\n"
	"layout(std430, binding = 7) buffer StatsBuffer {\n"
	"    uint stats[8];\n"
	"};\n"
	"\n"
	"/* Hi-Z pyramid bound to texture unit 1.  Under reversed-Z the\n"
	" * pyramid stores min(depth) per tile (= farthest fragment), under\n"
	" * forward-Z max(depth). */\n"
	"layout(binding = 1) uniform sampler2D u_hiz_pyramid;\n"
	"\n"
	"uniform vec3 u_vieworg;\n"
	"uniform vec4 u_frustum[4];\n"
	"uniform int u_framecount;\n"
	"uniform int u_num_marksurfs;\n"
	"uniform int u_num_buckets;\n"
	"uniform int u_max_dst_indices;\n"
	"uniform mat4 u_prev_mvp;\n"
	"uniform vec2 u_hiz_size;\n"
	"uniform int u_hiz_mip_count;\n"
	"uniform int u_hiz_enable;\n"
	"uniform int u_reverse_z;\n"
	"uniform int u_stats_enable;\n"
	"\n"
	"#define STAT_INC(i) do { if (u_stats_enable != 0) atomicAdd(stats[i], 1u); } while(false)\n"
	"\n"
	"void main() {\n"
	"    uint id = gl_GlobalInvocationID.x;\n"
	"    if (id >= uint(u_num_marksurfs)) return;\n"
	"    STAT_INC(0);\n"
	"\n"
	"    MarkSurf ms = marksurfs[id];\n"
	"    int leaf = ms.leaf_idx;\n"
	"    int si = ms.surf_idx;\n"
	"\n"
	"    /* PVS test */\n"
	"    if ((vis[leaf >> 5] & (1u << (leaf & 31))) == 0u) {\n"
	"        STAT_INC(1); return;\n"
	"    }\n"
	"\n"
	"    Surface s = surfaces[si];\n"
	"\n"
	"    /* Skip non-lightmapped and special surfaces:\n"
	"     * sky(0x4) turb(0x10) drawtiled(0x20) drawblack(0x100)\n"
	"     * underwater(0x200) fence(0x800) */\n"
	"    if ((s.flags & 0xB34) != 0) { STAT_INC(2); return; }\n"
	"\n"
	"    /* Skip surfaces with no VBO data */\n"
	"    if (s.numindices <= 0) { STAT_INC(2); return; }\n"
	"\n"
	"    /* Backface cull — disabled to avoid popping at grazing angles.\n"
	"     * GPU rasterizer handles backfaces via winding order. */\n"
	"    /* float d = dot(s.plane.xyz, u_vieworg) - s.plane.w;\n"
	"    bool planeback = (s.flags & 2) != 0;\n"
	"    if (planeback && d > 0.01) return;\n"
	"    if (!planeback && d < -0.01) return; */\n"
	"\n"
	"    /* Frustum cull with generous padding — reject surfaces well\n"
	"     * outside the view frustum to limit PVS over-draw.  Without\n"
	"     * this, all PVS-visible surfaces are drawn regardless of view\n"
	"     * angle, causing abrupt visual shifts through translucent\n"
	"     * water when crossing leaf boundaries.  The 512-unit slack\n"
	"     * prevents surface popping at frustum edges. */\n"
	"    const float FSLACK = 512.0;\n"
	"    for (int fi = 0; fi < 4; fi++) {\n"
	"        vec3 fn = u_frustum[fi].xyz;\n"
	"        float fd = u_frustum[fi].w;\n"
	"        vec3 pv = vec3(\n"
	"            fn.x >= 0.0 ? s.maxs.x : s.mins.x,\n"
	"            fn.y >= 0.0 ? s.maxs.y : s.mins.y,\n"
	"            fn.z >= 0.0 ? s.maxs.z : s.mins.z);\n"
	"        if (dot(fn, pv) + fd < -FSLACK) { STAT_INC(3); return; }\n"
	"    }\n"
	"\n"
	"    /* Hi-Z occlusion test against previous-frame pyramid.\n"
	"     * Project the 8 AABB corners through last frame's MVP and form\n"
	"     * a conservative screen-space rect + nearest-to-camera depth.\n"
	"     * Sample the pyramid at a mip whose texel size matches the rect,\n"
	"     * compare; cull if the AABB sits entirely behind the occluder.\n"
	"     * Any corner with w<=0 (behind near plane) disables the test. */\n"
	"    if (u_hiz_enable != 0) {\n"
	"        vec3 mn = s.mins.xyz;\n"
	"        vec3 mx = s.maxs.xyz;\n"
	"        float rx0 = 1.0, ry0 = 1.0, rx1 = -1.0, ry1 = -1.0;\n"
	"        float dnear = (u_reverse_z != 0) ? 0.0 : 1.0;\n"
	"        bool valid = true;\n"
	"        for (int ci = 0; ci < 8; ci++) {\n"
	"            vec3 c = vec3(\n"
	"                ((ci & 1) != 0) ? mx.x : mn.x,\n"
	"                ((ci & 2) != 0) ? mx.y : mn.y,\n"
	"                ((ci & 4) != 0) ? mx.z : mn.z);\n"
	"            vec4 clip = u_prev_mvp * vec4(c, 1.0);\n"
	"            if (clip.w <= 0.0) { valid = false; break; }\n"
	"            vec3 ndc = clip.xyz / clip.w;\n"
	"            float dn = (u_reverse_z != 0) ? ndc.z : (ndc.z * 0.5 + 0.5);\n"
	"            if (u_reverse_z != 0) dnear = max(dnear, dn);\n"
	"            else                  dnear = min(dnear, dn);\n"
	"            rx0 = min(rx0, ndc.x); ry0 = min(ry0, ndc.y);\n"
	"            rx1 = max(rx1, ndc.x); ry1 = max(ry1, ndc.y);\n"
	"        }\n"
	"        if (valid) {\n"
	"            /* Clip rect to NDC bounds [-1,1] before mapping to texels. */\n"
	"            rx0 = clamp(rx0, -1.0, 1.0); ry0 = clamp(ry0, -1.0, 1.0);\n"
	"            rx1 = clamp(rx1, -1.0, 1.0); ry1 = clamp(ry1, -1.0, 1.0);\n"
	"            vec2 r0 = (vec2(rx0, ry0) * 0.5 + 0.5) * u_hiz_size;\n"
	"            vec2 r1 = (vec2(rx1, ry1) * 0.5 + 0.5) * u_hiz_size;\n"
	"            float dim = max(r1.x - r0.x, r1.y - r0.y);\n"
	"            if (dim > 0.5) {\n"
	"                int mip = int(ceil(log2(dim)));\n"
	"                if (mip < 0) mip = 0;\n"
	"                if (mip > u_hiz_mip_count - 1) mip = u_hiz_mip_count - 1;\n"
	"                vec2 cuv = (r0 + r1) * 0.5 / u_hiz_size;\n"
	"                /* 2x2 textureLod sample at the chosen mip, reduced\n"
	"                 * the same way the pyramid was built. */\n"
	"                float texel = exp2(float(mip)) / max(u_hiz_size.x, u_hiz_size.y);\n"
	"                float occ;\n"
	"                float s0 = textureLod(u_hiz_pyramid, cuv + vec2(-0.5,-0.5)*texel, float(mip)).r;\n"
	"                float s1 = textureLod(u_hiz_pyramid, cuv + vec2( 0.5,-0.5)*texel, float(mip)).r;\n"
	"                float s2 = textureLod(u_hiz_pyramid, cuv + vec2(-0.5, 0.5)*texel, float(mip)).r;\n"
	"                float s3 = textureLod(u_hiz_pyramid, cuv + vec2( 0.5, 0.5)*texel, float(mip)).r;\n"
	"                if (u_reverse_z != 0) {\n"
	"                    occ = min(min(s0,s1), min(s2,s3));\n"
	"                    /* AABB's nearest depth < occluder's min => fully behind */\n"
	"                    if (dnear < occ) { STAT_INC(4); return; }\n"
	"                } else {\n"
	"                    occ = max(max(s0,s1), max(s2,s3));\n"
	"                    if (dnear > occ) { STAT_INC(4); return; }\n"
	"                }\n"
	"            }\n"
	"        }\n"
	"    }\n"
	"\n"
	"    /* Dedup: same surface can appear in multiple leaves.\n"
	"     * atomicExchange the framecount — if it was already this\n"
	"     * frame's value, another thread already emitted it. */\n"
	"    int old = atomicExchange(dedup[si], u_framecount);\n"
	"    if (old == u_framecount) { STAT_INC(6); return; }\n"
	"\n"
	"    STAT_INC(5);\n"
	"\n"
	"    /* Bounds check texture bucket to prevent out-of-bounds writes */\n"
	"    if (s.tex_bucket >= uint(u_num_buckets)) return;\n"
	"\n"
	"    /* Claim index slots in the texture bucket's indirect command */\n"
	"    uint slot = atomicAdd(cmds[s.tex_bucket].count, uint(s.numindices));\n"
	"    uint dst_base = cmds[s.tex_bucket].firstIndex + slot;\n"
	"\n"
	"    /* Bounds check destination buffer writes */\n"
	"    if (dst_base + uint(s.numindices) > uint(u_max_dst_indices)) {\n"
	"        /* Buffer overflow would occur - abort write */\n"
	"        atomicAdd(cmds[s.tex_bucket].count, -int(s.numindices));\n"
	"        return;\n"
	"    }\n"
	"\n"
	"    /* Copy indices from source to destination */\n"
	"    for (int i = 0; i < s.numindices; i++)\n"
	"        dst_indices[dst_base + uint(i)] = src_indices[s.firstindex + i];\n"
	"}\n";

/* ------------------------------------------------------------------ */
/* Hi-Z compute shaders                                                */
/* ------------------------------------------------------------------ */

/* Copy the (sampled) scene depth into mip 0 of the R32F pyramid.
 * Reads via sampler2D — DEPTH24_STENCIL8 with TEXTURE_COMPARE_MODE = NONE
 * returns the raw depth value in [0,1] from the .r channel. */
static const char hiz_copy_src[] =
	"#version 430 core\n"
	"layout(local_size_x = 8, local_size_y = 8) in;\n"
	"layout(binding = 0) uniform sampler2D u_scene_depth;\n"
	"layout(binding = 0, r32f) uniform writeonly image2D u_dst;\n"
	"uniform ivec2 u_size;\n"
	"void main() {\n"
	"    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
	"    if (p.x >= u_size.x || p.y >= u_size.y) return;\n"
	"    float d = texelFetch(u_scene_depth, p, 0).r;\n"
	"    imageStore(u_dst, p, vec4(d, 0.0, 0.0, 0.0));\n"
	"}\n";

/* 2x2 reduction from mip N -> mip N+1.  Under reversed-Z (u_reverse_z!=0)
 * the conservative occluder (the farthest point in eye space) has the
 * smallest stored value, so we take min; under forward-Z, max. */
static const char hiz_reduce_src[] =
	"#version 430 core\n"
	"layout(local_size_x = 8, local_size_y = 8) in;\n"
	"layout(binding = 0) uniform sampler2D u_src;\n"
	"layout(binding = 0, r32f) uniform writeonly image2D u_dst;\n"
	"uniform ivec2 u_src_size;\n"
	"uniform ivec2 u_dst_size;\n"
	"uniform int u_src_lod;\n"
	"uniform int u_reverse_z;\n"
	"void main() {\n"
	"    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
	"    if (p.x >= u_dst_size.x || p.y >= u_dst_size.y) return;\n"
	"    ivec2 s = p * 2;\n"
	"    ivec2 a = clamp(s + ivec2(0,0), ivec2(0), u_src_size - ivec2(1));\n"
	"    ivec2 b = clamp(s + ivec2(1,0), ivec2(0), u_src_size - ivec2(1));\n"
	"    ivec2 c = clamp(s + ivec2(0,1), ivec2(0), u_src_size - ivec2(1));\n"
	"    ivec2 d = clamp(s + ivec2(1,1), ivec2(0), u_src_size - ivec2(1));\n"
	"    float va = texelFetch(u_src, a, u_src_lod).r;\n"
	"    float vb = texelFetch(u_src, b, u_src_lod).r;\n"
	"    float vc = texelFetch(u_src, c, u_src_lod).r;\n"
	"    float vd = texelFetch(u_src, d, u_src_lod).r;\n"
	"    float r;\n"
	"    if (u_reverse_z != 0) r = min(min(va,vb), min(vc,vd));\n"
	"    else                  r = max(max(va,vb), max(vc,vd));\n"
	"    /* odd-mip safety: if the parent dim is odd we also need to fold\n"
	"     * in the dangling row/column so coverage stays conservative. */\n"
	"    if (((u_src_size.x & 1) != 0) && (s.x + 2 < u_src_size.x)) {\n"
	"        ivec2 e = ivec2(s.x + 2, s.y);\n"
	"        ivec2 f = ivec2(s.x + 2, min(s.y + 1, u_src_size.y - 1));\n"
	"        float ve = texelFetch(u_src, e, u_src_lod).r;\n"
	"        float vf = texelFetch(u_src, f, u_src_lod).r;\n"
	"        if (u_reverse_z != 0) r = min(r, min(ve, vf));\n"
	"        else                  r = max(r, max(ve, vf));\n"
	"    }\n"
	"    if (((u_src_size.y & 1) != 0) && (s.y + 2 < u_src_size.y)) {\n"
	"        ivec2 e = ivec2(s.x, s.y + 2);\n"
	"        ivec2 f = ivec2(min(s.x + 1, u_src_size.x - 1), s.y + 2);\n"
	"        float ve = texelFetch(u_src, e, u_src_lod).r;\n"
	"        float vf = texelFetch(u_src, f, u_src_lod).r;\n"
	"        if (u_reverse_z != 0) r = min(r, min(ve, vf));\n"
	"        else                  r = max(r, max(ve, vf));\n"
	"    }\n"
	"    imageStore(u_dst, p, vec4(r, 0.0, 0.0, 0.0));\n"
	"}\n";

/* ------------------------------------------------------------------ */
/* Compile helper                                                      */
/* ------------------------------------------------------------------ */

static GLuint R_CompileComputeProgram (const char *source, const char *name)
{
	GLuint shader, prog;
	GLint status;
	char log[1024];

	shader = glCreateShader_fp(GL_COMPUTE_SHADER);
	glShaderSource_fp(shader, 1, &source, NULL);
	glCompileShader_fp(shader);
	glGetShaderiv_fp(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		glGetShaderInfoLog_fp(shader, sizeof(log), NULL, log);
		Con_Printf("Compute shader '%s' compile error: %s\n", name, log);
		glDeleteShader_fp(shader);
		return 0;
	}

	prog = glCreateProgram_fp();
	glAttachShader_fp(prog, shader);
	glLinkProgram_fp(prog);
	glDeleteShader_fp(shader);

	glGetProgramiv_fp(prog, GL_LINK_STATUS, &status);
	if (!status)
	{
		glGetProgramInfoLog_fp(prog, sizeof(log), NULL, log);
		Con_Printf("Compute shader '%s' link error: %s\n", name, log);
		glDeleteProgram_fp(prog);
		return 0;
	}

	return prog;
}

/* ------------------------------------------------------------------ */
/* Build GPU data at map load                                          */
/* ------------------------------------------------------------------ */

void R_BuildWorldCull (void)
{
	qmodel_t	*m = cl.worldmodel;
	msurface_t	*surf;
	mleaf_t		*leaf;
	gpu_surface_t	*gpu_surfs;
	gpu_marksurf_t	*gpu_marks;
	int		i, j, ms_count;

	if (!m || !glDispatchCompute_fp)
		return;

	R_FreeWorldCull();

	/* Compile compute shaders */
	cull_clear_prog = R_CompileComputeProgram(clear_indirect_src, "clear_indirect");
	cull_mark_prog = R_CompileComputeProgram(cull_mark_src, "cull_mark");
	if (!cull_clear_prog || !cull_mark_prog)
	{
		Con_Printf("GPU world culling: compute shaders failed, using CPU path\n");
		R_FreeWorldCull();
		return;
	}

	/* Get uniform locations */
	cull_clear_u_num_buckets = glGetUniformLocation_fp(cull_clear_prog, "u_num_buckets");
	cull_mark_u_vieworg = glGetUniformLocation_fp(cull_mark_prog, "u_vieworg");
	cull_mark_u_frustum = glGetUniformLocation_fp(cull_mark_prog, "u_frustum");
	cull_mark_u_framecount = glGetUniformLocation_fp(cull_mark_prog, "u_framecount");
	cull_mark_u_num_marksurfs = glGetUniformLocation_fp(cull_mark_prog, "u_num_marksurfs");
	cull_mark_u_num_buckets = glGetUniformLocation_fp(cull_mark_prog, "u_num_buckets");
	cull_mark_u_max_dst_indices = glGetUniformLocation_fp(cull_mark_prog, "u_max_dst_indices");
	cull_mark_u_prev_mvp = glGetUniformLocation_fp(cull_mark_prog, "u_prev_mvp");
	cull_mark_u_hiz_size = glGetUniformLocation_fp(cull_mark_prog, "u_hiz_size");
	cull_mark_u_hiz_mip_count = glGetUniformLocation_fp(cull_mark_prog, "u_hiz_mip_count");
	cull_mark_u_hiz_enable = glGetUniformLocation_fp(cull_mark_prog, "u_hiz_enable");
	cull_mark_u_reverse_z = glGetUniformLocation_fp(cull_mark_prog, "u_reverse_z");
	cull_mark_u_stats_enable = glGetUniformLocation_fp(cull_mark_prog, "u_stats_enable");

	cull_num_surfs = m->numsurfaces;
	cull_num_leaves = m->numleafs;
	cull_num_buckets = m->numtextures;

	/* Build per-surface GPU data */
	gpu_surfs = (gpu_surface_t *) malloc(cull_num_surfs * sizeof(gpu_surface_t));
	cull_bucket_map = (int *) malloc(cull_num_surfs * sizeof(int));
	if (!gpu_surfs || !cull_bucket_map)
	{
		Con_Printf("GPU world culling: malloc failed for surface data\n");
		R_FreeWorldCull();
		return;
	}
	for (i = 0; i < cull_num_surfs; i++)
	{
		surf = &m->surfaces[i];
		gpu_surfs[i].plane[0] = surf->plane->normal[0];
		gpu_surfs[i].plane[1] = surf->plane->normal[1];
		gpu_surfs[i].plane[2] = surf->plane->normal[2];
		gpu_surfs[i].plane[3] = surf->plane->dist;

		/* Compute surface AABB from polygon */
		if (surf->polys && surf->polys->numverts > 0)
		{
			float *v = surf->polys->verts[0];
			float bmin[3] = { v[0], v[1], v[2] };
			float bmax[3] = { v[0], v[1], v[2] };
			for (j = 1; j < surf->polys->numverts; j++)
			{
				v = surf->polys->verts[j];
				if (v[0] < bmin[0]) bmin[0] = v[0];
				if (v[1] < bmin[1]) bmin[1] = v[1];
				if (v[2] < bmin[2]) bmin[2] = v[2];
				if (v[0] > bmax[0]) bmax[0] = v[0];
				if (v[1] > bmax[1]) bmax[1] = v[1];
				if (v[2] > bmax[2]) bmax[2] = v[2];
			}
			gpu_surfs[i].mins[0] = bmin[0];
			gpu_surfs[i].mins[1] = bmin[1];
			gpu_surfs[i].mins[2] = bmin[2];
			gpu_surfs[i].maxs[0] = bmax[0];
			gpu_surfs[i].maxs[1] = bmax[1];
			gpu_surfs[i].maxs[2] = bmax[2];
		}
		gpu_surfs[i].mins[3] = 0;
		gpu_surfs[i].maxs[3] = 0;

		/* Texture bucket = find texture index in model's texture array */
		gpu_surfs[i].tex_bucket = 0;
		if (surf->texinfo->texture && m->textures)
		{
			for (j = 0; j < m->numtextures; j++)
			{
				if (m->textures[j] == surf->texinfo->texture)
				{
					gpu_surfs[i].tex_bucket = j;
					break;
				}
			}
		}

		gpu_surfs[i].firstindex = surf->vbo_firstindex;
		gpu_surfs[i].numindices = surf->vbo_numtris * 3;
		/* Propagate surface flags; mark lightmap-less surfaces as
		 * DRAWTILED so the compute shader skips them (they'd sample
		 * black from the atlas since they have no lightmap data). */
		gpu_surfs[i].flags = surf->flags;
		if (!surf->samples)
			gpu_surfs[i].flags |= SURF_DRAWTILED;

		cull_bucket_map[i] = gpu_surfs[i].tex_bucket;
	}

	/* Upload surface SSBO */
	glGenBuffers_fp(1, &cull_surf_ssbo);
	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_surf_ssbo);
	glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
			cull_num_surfs * sizeof(gpu_surface_t),
			gpu_surfs, GL_STATIC_DRAW);
	free(gpu_surfs);

	/* Build marksurface entries: flatten (leaf, surface) pairs */
	ms_count = 0;
	for (i = 1; i < cull_num_leaves; i++)
		ms_count += m->leafs[i].nummarksurfaces;

	gpu_marks = (gpu_marksurf_t *) malloc(ms_count * sizeof(gpu_marksurf_t));
	if (!gpu_marks)
	{
		Con_Printf("GPU world culling: malloc failed for marksurface data\n");
		R_FreeWorldCull();
		return;
	}
	ms_count = 0;
	for (i = 1; i < cull_num_leaves; i++)
	{
		leaf = &m->leafs[i];
		for (j = 0; j < leaf->nummarksurfaces; j++)
		{
			int surf_idx = (int)(leaf->firstmarksurface[j] - m->surfaces);
			gpu_marks[ms_count].leaf_idx = i;
			gpu_marks[ms_count].surf_idx = surf_idx;
			ms_count++;
		}
	}
	cull_num_marksurfs = ms_count;

	glGenBuffers_fp(1, &cull_marksurf_ssbo);
	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_marksurf_ssbo);
	glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
			cull_num_marksurfs * sizeof(gpu_marksurf_t),
			gpu_marks, GL_STATIC_DRAW);
	free(gpu_marks);

	/* PVS buffer is streamed per-frame via GL_Upload (uhexen2-o35n) —
	 * no static allocation needed. */

	/* Count max indices per texture bucket for firstIndex allocation */
	{
		int *bucket_max = (int *) calloc(cull_num_buckets, sizeof(int));
		gpu_indirect_cmd_t *cmds;
		int offset;

		for (i = 0; i < cull_num_surfs; i++)
		{
			surf = &m->surfaces[i];
			if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_UNDERWATER))
				continue;
			if (surf->vbo_numtris <= 0)
				continue;
			int bucket = cull_bucket_map[i];
			if (bucket >= 0 && bucket < cull_num_buckets)
				bucket_max[bucket] += surf->vbo_numtris * 3;
		}

		/* Allocate contiguous regions in the dest IBO per bucket */
		cmds = (gpu_indirect_cmd_t *) calloc(cull_num_buckets, sizeof(gpu_indirect_cmd_t));
		offset = 0;
		for (i = 0; i < cull_num_buckets; i++)
		{
			cmds[i].count = 0;		/* filled by compute each frame */
			cmds[i].instanceCount = 1;
			cmds[i].firstIndex = offset;	/* fixed region start */
			cmds[i].baseVertex = 0;
			cmds[i].baseInstance = 0;
			offset += bucket_max[i];
		}
		cull_total_indices = offset;
		free(bucket_max);

		glGenBuffers_fp(1, &cull_indirect_buf);
		glBindBuffer_fp(GL_DRAW_INDIRECT_BUFFER, cull_indirect_buf);
		glBufferData_fp(GL_DRAW_INDIRECT_BUFFER,
				cull_num_buckets * sizeof(gpu_indirect_cmd_t),
				cmds, GL_DYNAMIC_DRAW);
		free(cmds);
	}

	/* Source index SSBO: read-only copy of world IBO for compute to read from.
	 * We read the data back from the existing world IBO (built by R_BuildWorldVBO). */
	{
		extern GLuint world_ibo;
		extern int world_num_indices;
		unsigned int *idx_data;

		if (world_ibo && world_num_indices > 0)
		{
			idx_data = (unsigned int *) malloc(world_num_indices * sizeof(unsigned int));
			/* Rebuild index data from surface polys (same as R_BuildWorldVBO) */
			{
				int idx_pos = 0;
				for (i = 0; i < m->numsurfaces; i++)
				{
					glpoly_t *p;
					surf = &m->surfaces[i];
					p = surf->polys;
					if (!p || p->numverts < 3)
						continue;
					if (surf->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
						continue;
					for (j = 2; j < p->numverts; j++)
					{
						idx_data[idx_pos++] = surf->vbo_firstvert;
						idx_data[idx_pos++] = surf->vbo_firstvert + j - 1;
						idx_data[idx_pos++] = surf->vbo_firstvert + j;
					}
				}
			}

			glGenBuffers_fp(1, &cull_src_ibo);
			glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_src_ibo);
			glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
					world_num_indices * sizeof(unsigned int),
					idx_data, GL_STATIC_DRAW);
			free(idx_data);
		}
	}

	/* Dest index buffer: written by compute, read as GL_ELEMENT_ARRAY_BUFFER.
	 * Sized to hold the worst case (all surfaces visible). */
	if (cull_total_indices > 0)
	{
		glGenBuffers_fp(1, &cull_dst_ibo);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_dst_ibo);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				cull_total_indices * sizeof(unsigned int),
				NULL, GL_DYNAMIC_DRAW);
	}

	/* Dedup buffer: one int per surface, atomicExchange'd with framecount */
	{
		int *zeros = (int *) calloc(cull_num_surfs, sizeof(int));
		glGenBuffers_fp(1, &cull_dedup_ssbo);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_dedup_ssbo);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				cull_num_surfs * sizeof(int),
				zeros, GL_DYNAMIC_DRAW);
		free(zeros);
	}

	/* Stats buffer: 8 uints, zeroed each dispatch when stats enabled */
	{
		unsigned int zeros[8] = {0};
		glGenBuffers_fp(1, &cull_stats_ssbo);
		glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, cull_stats_ssbo);
		glBufferData_fp(GL_SHADER_STORAGE_BUFFER,
				sizeof(zeros), zeros, GL_DYNAMIC_DRAW);
	}

	glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, 0);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, 0);
	cull_initialized = true;

	/* Hi-Z reprojection lives across the map change only when the prior
	 * map's last-frame MVP is meaningful here — it isn't, so reset. */
	R_BuildHiZ();

	Con_SafePrintf("GPU world cull: %d surfs, %d marksurfs, %d buckets, %d max indices\n",
		       cull_num_surfs, cull_num_marksurfs, cull_num_buckets, cull_total_indices);
}

void R_FreeWorldCull (void)
{
	if (cull_surf_ssbo) { glDeleteBuffers_fp(1, &cull_surf_ssbo); cull_surf_ssbo = 0; }
	if (cull_marksurf_ssbo) { glDeleteBuffers_fp(1, &cull_marksurf_ssbo); cull_marksurf_ssbo = 0; }
	if (cull_indirect_buf) { glDeleteBuffers_fp(1, &cull_indirect_buf); cull_indirect_buf = 0; }
	if (cull_src_ibo) { glDeleteBuffers_fp(1, &cull_src_ibo); cull_src_ibo = 0; }
	if (cull_dst_ibo) { glDeleteBuffers_fp(1, &cull_dst_ibo); cull_dst_ibo = 0; }
	if (cull_dedup_ssbo) { glDeleteBuffers_fp(1, &cull_dedup_ssbo); cull_dedup_ssbo = 0; }
	if (cull_stats_ssbo) { glDeleteBuffers_fp(1, &cull_stats_ssbo); cull_stats_ssbo = 0; }
	if (cull_clear_prog) { glDeleteProgram_fp(cull_clear_prog); cull_clear_prog = 0; }
	if (cull_mark_prog) { glDeleteProgram_fp(cull_mark_prog); cull_mark_prog = 0; }
	if (cull_bucket_map) { free(cull_bucket_map); cull_bucket_map = NULL; }
	R_FreeHiZ();
	cull_initialized = false;
	cull_total_indices = 0;
}

/* ------------------------------------------------------------------ */
/* Hi-Z occlusion culling — uhexen2-xd87                               */
/* ------------------------------------------------------------------ */

extern qboolean	gl_clipcontrol_able;

static int R_HiZ_MipCount (int w, int h)
{
	int d = (w > h) ? w : h;
	int n = 1;
	while (d > 1) { d >>= 1; n++; }
	return n;
}

static qboolean R_HiZ_Available (void)
{
	return (gl_hiz_cull.integer != 0)
	    && (glDispatchCompute_fp != NULL)
	    && (glTexStorage2D_fp    != NULL)
	    && (glBindImageTexture_fp != NULL)
	    && (glMemoryBarrier_fp   != NULL);
}

static qboolean R_HiZ_EnsureResources (int scene_w, int scene_h)
{
	if (scene_w <= 0 || scene_h <= 0)
		return false;

	if (hiz_pyramid_tex && hiz_width == scene_w && hiz_height == scene_h)
		return true;

	if (hiz_pyramid_tex)
	{
		glDeleteTextures_fp(1, &hiz_pyramid_tex);
		hiz_pyramid_tex = 0;
	}

	hiz_width = scene_w;
	hiz_height = scene_h;
	hiz_mip_count = R_HiZ_MipCount(scene_w, scene_h);

	glGenTextures_fp(1, &hiz_pyramid_tex);
	glBindTexture_fp(GL_TEXTURE_2D, hiz_pyramid_tex);
	glTexStorage2D_fp(GL_TEXTURE_2D, hiz_mip_count, GL_R32F, scene_w, scene_h);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture_fp(GL_TEXTURE_2D, 0);

	if (!hiz_copy_prog)
		hiz_copy_prog = R_CompileComputeProgram(hiz_copy_src, "hiz_copy");
	if (!hiz_reduce_prog)
		hiz_reduce_prog = R_CompileComputeProgram(hiz_reduce_src, "hiz_reduce");

	if (!hiz_copy_prog || !hiz_reduce_prog)
	{
		Con_Printf("Hi-Z: compute shader failed, disabling\n");
		R_FreeHiZ();
		return false;
	}

	Con_DPrintf("Hi-Z: %dx%d pyramid, %d mips\n",
		    hiz_width, hiz_height, hiz_mip_count);
	return true;
}

void R_BuildHiZ (void)
{
	/* Allocation deferred to R_BuildHiZForNextFrame so we don't burn
	 * GPU memory when gl_hiz_cull is off. */
	memset(hiz_prev_mvp, 0, sizeof(hiz_prev_mvp));
	hiz_prev_mvp_valid = false;
}

void R_FreeHiZ (void)
{
	if (hiz_pyramid_tex)   { glDeleteTextures_fp(1, &hiz_pyramid_tex); hiz_pyramid_tex = 0; }
	if (hiz_copy_prog)     { glDeleteProgram_fp(hiz_copy_prog);        hiz_copy_prog = 0; }
	if (hiz_reduce_prog)   { glDeleteProgram_fp(hiz_reduce_prog);      hiz_reduce_prog = 0; }
	if (hiz_resolve_fbo)       { glDeleteFramebuffers_fp(1, &hiz_resolve_fbo);   hiz_resolve_fbo = 0; }
	if (hiz_resolve_depth_tex) { glDeleteTextures_fp(1, &hiz_resolve_depth_tex); hiz_resolve_depth_tex = 0; }
	hiz_resolve_w = hiz_resolve_h = 0;
	hiz_width = hiz_height = hiz_mip_count = 0;
	hiz_prev_mvp_valid = false;
}

/* uhexen2-9912: standalone depth resolve.  When the postprocess pipeline
 * isn't active (no FXAA/HDR/gamma override/etc.), the scene is drawn
 * straight into the default framebuffer and there is no sampler-readable
 * depth texture for Hi-Z to seed from.  Maintain a private depth texture
 * and blit-resolve the default fb's depth into it.  Returns the texture
 * or 0 on failure. */
static GLuint R_HiZ_ResolveStandaloneDepth (int scene_w, int scene_h)
{
	GLenum status;

	if (scene_w <= 0 || scene_h <= 0)
		return 0;

	if (hiz_resolve_depth_tex && hiz_resolve_fbo &&
	    hiz_resolve_w == scene_w && hiz_resolve_h == scene_h)
	{
		/* existing texture matches — just refresh contents */
		goto blit;
	}

	if (hiz_resolve_depth_tex)
	{
		glDeleteTextures_fp(1, &hiz_resolve_depth_tex);
		hiz_resolve_depth_tex = 0;
	}
	if (hiz_resolve_fbo)
	{
		glDeleteFramebuffers_fp(1, &hiz_resolve_fbo);
		hiz_resolve_fbo = 0;
	}

	glGenTextures_fp(1, &hiz_resolve_depth_tex);
	glBindTexture_fp(GL_TEXTURE_2D, hiz_resolve_depth_tex);
	glTexImage2D_fp(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
			scene_w, scene_h, 0,
			GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf_fp(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture_fp(GL_TEXTURE_2D, 0);

	glGenFramebuffers_fp(1, &hiz_resolve_fbo);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, hiz_resolve_fbo);
	glFramebufferTexture2D_fp(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
				  GL_TEXTURE_2D, hiz_resolve_depth_tex, 0);
	/* Depth-only FBO needs DRAW_BUFFER = NONE for completeness on strict
	 * drivers.  READ_BUFFER state is irrelevant since we only ever bind
	 * this fbo as the DRAW target of glBlitFramebuffer. */
	{
		GLenum none = GL_NONE;
		glDrawBuffers_fp(1, &none);
	}

	status = glCheckFramebufferStatus_fp(GL_DRAW_FRAMEBUFFER);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		Con_DPrintf("Hi-Z standalone resolve FBO incomplete (0x%x)\n", status);
		glDeleteFramebuffers_fp(1, &hiz_resolve_fbo);
		glDeleteTextures_fp(1, &hiz_resolve_depth_tex);
		hiz_resolve_fbo = 0;
		hiz_resolve_depth_tex = 0;
		return 0;
	}

	hiz_resolve_w = scene_w;
	hiz_resolve_h = scene_h;

blit:
	/* Resolve default framebuffer depth -> private depth texture.  Default
	 * fb is the source (id 0), our private fbo is the destination.  Blit
	 * is a no-op for non-MSAA → non-MSAA except that it copies the data;
	 * GL_NEAREST is the only valid filter for depth blits. */
	glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, hiz_resolve_fbo);
	glBlitFramebuffer_fp(0, 0, scene_w, scene_h,
			     0, 0, scene_w, scene_h,
			     GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer_fp(GL_DRAW_FRAMEBUFFER, 0);
	glBindFramebuffer_fp(GL_READ_FRAMEBUFFER, 0);

	return hiz_resolve_depth_tex;
}

void R_BuildHiZForNextFrame (void)
{
	GLuint	scene_depth_tex;
	int	scene_w, scene_h;
	int	mip;

	if (!R_HiZ_Available())
		return;

	scene_depth_tex = GL_PostProcess_GetSceneDepthTex();
	if (scene_depth_tex)
	{
		/* Postprocess is active with a sampler-readable depth (non-MSAA
		 * pp path).  Use the scene size it actually rendered at — may
		 * be scaled down by r_scale < 1. */
		if (!GL_PostProcess_GetSceneSize(&scene_w, &scene_h))
			return;
	}
	else if (!GL_PostProcess_Active())
	{
		/* uhexen2-9912: postprocess inactive (no FXAA/HDR/gamma/etc.) —
		 * scene went straight to the default framebuffer.  Resolve its
		 * depth to our private texture so the pyramid build still runs.
		 * (When pp IS active but has no depth texture — MSAA pp path —
		 * the scene is in pp_fbo, not the default fb; reading default
		 * fb would give stale data, so skip silently.) */
		extern int glwidth, glheight;
		scene_w = glwidth;
		scene_h = glheight;
		scene_depth_tex = R_HiZ_ResolveStandaloneDepth(scene_w, scene_h);
		if (!scene_depth_tex)
			return;
	}
	else
	{
		/* pp_active && no depth texture — MSAA postprocess path.  No
		 * sampler-readable depth available; cull-mark will run without
		 * Hi-Z this frame. */
		return;
	}

	if (!R_HiZ_EnsureResources(scene_w, scene_h))
		return;

	/* Copy scene depth -> mip 0 */
	glUseProgram_fp(hiz_copy_prog);
	glActiveTexture_fp(GL_TEXTURE0);
	glBindTexture_fp(GL_TEXTURE_2D, scene_depth_tex);
	glBindImageTexture_fp(0, hiz_pyramid_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
	{
		GLint loc = glGetUniformLocation_fp(hiz_copy_prog, "u_size");
		if (loc >= 0) glUniform2i_fp(loc, scene_w, scene_h);
	}
	glDispatchCompute_fp((scene_w + 7) / 8, (scene_h + 7) / 8, 1);
	glMemoryBarrier_fp(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	/* Reduce mip N -> mip N+1 */
	glUseProgram_fp(hiz_reduce_prog);
	glActiveTexture_fp(GL_TEXTURE0);
	glBindTexture_fp(GL_TEXTURE_2D, hiz_pyramid_tex);
	{
		GLint loc_src_size = glGetUniformLocation_fp(hiz_reduce_prog, "u_src_size");
		GLint loc_dst_size = glGetUniformLocation_fp(hiz_reduce_prog, "u_dst_size");
		GLint loc_src_lod  = glGetUniformLocation_fp(hiz_reduce_prog, "u_src_lod");
		GLint loc_rev_z    = glGetUniformLocation_fp(hiz_reduce_prog, "u_reverse_z");
		int src_w = scene_w, src_h = scene_h;
		for (mip = 1; mip < hiz_mip_count; mip++)
		{
			int dst_w = src_w > 1 ? src_w >> 1 : 1;
			int dst_h = src_h > 1 ? src_h >> 1 : 1;
			glBindImageTexture_fp(0, hiz_pyramid_tex, mip, GL_FALSE, 0,
					      GL_WRITE_ONLY, GL_R32F);
			if (loc_src_size >= 0) glUniform2i_fp(loc_src_size, src_w, src_h);
			if (loc_dst_size >= 0) glUniform2i_fp(loc_dst_size, dst_w, dst_h);
			if (loc_src_lod  >= 0) glUniform1i_fp(loc_src_lod, mip - 1);
			if (loc_rev_z    >= 0) glUniform1i_fp(loc_rev_z, gl_clipcontrol_able ? 1 : 0);
			glDispatchCompute_fp((dst_w + 7) / 8, (dst_h + 7) / 8, 1);
			glMemoryBarrier_fp(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
			src_w = dst_w;
			src_h = dst_h;
		}
	}

	glUseProgram_fp(0);
	glActiveTexture_fp(GL_TEXTURE0);
	glBindTexture_fp(GL_TEXTURE_2D, 0);
}

qboolean R_WorldCullAvailable (void)
{
	return cull_initialized;
}

/* ------------------------------------------------------------------ */
/* Per-frame dispatch                                                  */
/* ------------------------------------------------------------------ */

void R_DispatchWorldCull (void)
{
	byte		*pvs;
	unsigned int	*vis_bits;
	int		vis_size, i;
	static int	framecount;
	extern vec3_t	r_origin;

	if (!cull_initialized)
		return;

	framecount++;

	/* Decompress PVS for current viewleaf */
	pvs = Mod_LeafPVS(r_viewleaf, cl.worldmodel);
	vis_size = (cull_num_leaves + 31) / 32;
	vis_bits = (unsigned int *) alloca(vis_size * sizeof(unsigned int));
	memset(vis_bits, 0, vis_size * sizeof(unsigned int));

	/* Convert byte PVS to bit array */
	for (i = 0; i < cull_num_leaves - 1; i++)
	{
		if (pvs[i >> 3] & (1 << (i & 7)))
			vis_bits[(i + 1) >> 5] |= (1u << ((i + 1) & 31));
	}

	/* Stream PVS into this frame's host ring slot (uhexen2-o35n).
	 * Persistent-mapped when available, falls back to BufferSubData. */
	{
		GLuint		vis_buf;
		GLintptr	vis_ofs;
		GLsizeiptr	vis_bytes = vis_size * sizeof(unsigned int);
		GL_Upload(GL_SHADER_STORAGE_BUFFER, vis_bits, vis_bytes,
			  &vis_buf, &vis_ofs);
		GL_BindBufferRange(GL_SHADER_STORAGE_BUFFER, 2,
				   vis_buf, vis_ofs, vis_bytes);
	}

	/* Pass 1: clear indirect buffer */
	glUseProgram_fp(cull_clear_prog);
	glUniform1i_fp(cull_clear_u_num_buckets, cull_num_buckets);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 3, cull_indirect_buf);
	glDispatchCompute_fp((cull_num_buckets + 63) / 64, 1, 1);
	glMemoryBarrier_fp(GL_SHADER_STORAGE_BARRIER_BIT);

	/* Pass 2: cull + mark surfaces */
	glUseProgram_fp(cull_mark_prog);
	glUniform3f_fp(cull_mark_u_vieworg, r_origin[0], r_origin[1], r_origin[2]);
	glUniform1i_fp(cull_mark_u_framecount, framecount);
	glUniform1i_fp(cull_mark_u_num_marksurfs, cull_num_marksurfs);
	if (cull_mark_u_num_buckets >= 0)
		glUniform1i_fp(cull_mark_u_num_buckets, cull_num_buckets);
	if (cull_mark_u_max_dst_indices >= 0)
		glUniform1i_fp(cull_mark_u_max_dst_indices, cull_total_indices);

	/* Hi-Z occlusion (uhexen2-xd87).  Sampler binding 1 = pyramid; the
	 * enable flag is also gated by gl_hiz_cull/prev_mvp_valid CPU-side
	 * so shaders early-out cheaply when not in use. */
	{
		qboolean hiz_on = gl_hiz_cull.integer && hiz_pyramid_tex && hiz_prev_mvp_valid;
		if (cull_mark_u_hiz_enable >= 0)
			glUniform1i_fp(cull_mark_u_hiz_enable, hiz_on ? 1 : 0);
		if (cull_mark_u_reverse_z >= 0)
			glUniform1i_fp(cull_mark_u_reverse_z, gl_clipcontrol_able ? 1 : 0);
		if (cull_mark_u_prev_mvp >= 0)
			glUniformMatrix4fv_fp(cull_mark_u_prev_mvp, 1, GL_FALSE, hiz_prev_mvp);
		if (cull_mark_u_hiz_size >= 0)
			glUniform2f_fp(cull_mark_u_hiz_size, (float)hiz_width, (float)hiz_height);
		if (cull_mark_u_hiz_mip_count >= 0)
			glUniform1i_fp(cull_mark_u_hiz_mip_count, hiz_mip_count);
		if (hiz_on)
		{
			glActiveTexture_fp(GL_TEXTURE1);
			glBindTexture_fp(GL_TEXTURE_2D, hiz_pyramid_tex);
			glActiveTexture_fp(GL_TEXTURE0);
		}
	}

	/* Upload frustum planes */
	{
		extern mplane_t frustum[4];
		float frust[16]; /* 4 vec4s */
		for (i = 0; i < 4; i++)
		{
			frust[i*4+0] = frustum[i].normal[0];
			frust[i*4+1] = frustum[i].normal[1];
			frust[i*4+2] = frustum[i].normal[2];
			frust[i*4+3] = -frustum[i].dist;  /* negate: shader uses dot(n,p)+d, engine uses dot(n,p)-dist */
		}
		glUniform4fv_fp(cull_mark_u_frustum, 4, frust);
	}

	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 0, cull_surf_ssbo);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 1, cull_marksurf_ssbo);
	/* binding 2 already bound via GL_BindBufferRange above (PVS upload). */
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 3, cull_indirect_buf);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 4, cull_src_ibo);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 5, cull_dst_ibo);
	glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 6, cull_dedup_ssbo);

	/* Stats counters (uhexen2-cyu0).  Bind always; gating happens via
	 * u_stats_enable in the shader so we don't pay for atomicAdds when
	 * gl_hiz_stats is 0.  When enabled, zero the buffer before dispatch
	 * so the readback sees only this frame's contributions. */
	{
		qboolean stats_on = (gl_hiz_stats.integer > 0) && cull_stats_ssbo;
		if (stats_on)
		{
			unsigned int zeros[8] = {0};
			GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_stats_ssbo);
			glBufferSubData_fp(GL_SHADER_STORAGE_BUFFER, 0, sizeof(zeros), zeros);
		}
		if (cull_mark_u_stats_enable >= 0)
			glUniform1i_fp(cull_mark_u_stats_enable, stats_on ? 1 : 0);
		glBindBufferBase_fp(GL_SHADER_STORAGE_BUFFER, 7, cull_stats_ssbo);
	}

	glDispatchCompute_fp((cull_num_marksurfs + 63) / 64, 1, 1);
	glMemoryBarrier_fp(GL_COMMAND_BARRIER_BIT | GL_ELEMENT_ARRAY_BARRIER_BIT |
			   GL_SHADER_STORAGE_BARRIER_BIT);

	/* Gated stats readback: only every gl_hiz_stats frames, so the
	 * implied glGetBufferSubData stall amortizes.  Print to console;
	 * caller decides whether to leave running or set 0 after reading. */
	if (gl_hiz_stats.integer > 0 && cull_stats_ssbo && glGetBufferSubData_fp)
	{
		hiz_stats_frame_counter++;
		if (hiz_stats_frame_counter >= gl_hiz_stats.integer)
		{
			unsigned int counts[8] = {0};
			unsigned int post;
			hiz_stats_frame_counter = 0;
			GL_BindBuffer(GL_SHADER_STORAGE_BUFFER, cull_stats_ssbo);
			glGetBufferSubData_fp(GL_SHADER_STORAGE_BUFFER, 0,
					      sizeof(counts), counts);
			post = counts[4] + counts[5] + counts[6];
			Con_Printf("hiz_stats: total=%u pvs=%u flags=%u frust=%u "
				   "hiz=%u accepted=%u dedup=%u\n",
				   counts[0], counts[1], counts[2], counts[3],
				   counts[4], counts[5], counts[6]);
			if (post > 0)
				Con_Printf("  hiz cull rate = %.1f%% of post-frustum "
					   "(%u / %u; gate %.1f%%)\n",
					   100.0 * counts[4] / (double)post,
					   counts[4], post, 10.0);
			else
				Con_Printf("  hiz cull rate = (no post-frustum surfaces)\n");
		}
	}

	glUseProgram_fp(0);
}

/* ------------------------------------------------------------------ */
/* Draw with indirect commands after compute cull                      */
/* ------------------------------------------------------------------ */

void R_DrawWorldCulled (void)
{
	int		i;
	float		mvp[16], mv[16];
	extern GLuint	world_vao;
	extern float	r_fog_density;
	extern float	r_fog_color[3];
	extern GLuint	lm_atlas_texture;
	extern entity_t	r_worldentity;

	if (!cull_initialized || !cull_dst_ibo || cull_total_indices <= 0)
		return;

	/* Bind world VAO but replace its IBO with the compute-written dest IBO */
	glBindVertexArray_fp(world_vao);
	glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, cull_dst_ibo);
	glVertexAttrib4f_fp(ATTR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);

	/* Set up world shader.  MDI cull path emits opaque world only — fence
	 * surfaces flow through R_DrawBrushInstanced's fence sub-pass — so use
	 * the early_fragment_tests variant for Hi-Z.  uhexen2-5c6r. */
	{
		glprogram_t *prog = &gl_shader_world_opaque;
		glUseProgram_fp(prog->program);
		GL_GetMVP(mvp);
		GL_GetModelview(mv);
		if (prog->u_mvp >= 0)
			glUniformMatrix4fv_fp(prog->u_mvp, 1, GL_FALSE, mvp);
		if (prog->u_modelview >= 0)
			glUniformMatrix4fv_fp(prog->u_modelview, 1, GL_FALSE, mv);
		if (prog->u_fog_density >= 0)
			glUniform1f_fp(prog->u_fog_density, r_fog_density);
		if (prog->u_fog_color >= 0)
			glUniform3f_fp(prog->u_fog_color,
				       r_fog_color[0], r_fog_color[1], r_fog_color[2]);
		if (prog->u_alpha_threshold >= 0)
			glUniform1f_fp(prog->u_alpha_threshold, 0.01f);
	}

	/* Bind lightmap atlas on unit 1, default fb null at unit 2 */
	glActiveTexture_fp(GL_TEXTURE1);
	glBindTexture_fp(GL_TEXTURE_2D, lm_atlas_texture);
	glActiveTexture_fp(GL_TEXTURE2);
	glBindTexture_fp(GL_TEXTURE_2D, gl_null_fb_texture);
	glActiveTexture_fp(GL_TEXTURE0);

	/* Draw each texture bucket via indirect commands */
	glBindBuffer_fp(GL_DRAW_INDIRECT_BUFFER, cull_indirect_buf);

	for (i = 0; i < cull_num_buckets; i++)
	{
		texture_t *t = (cl.worldmodel->textures) ? cl.worldmodel->textures[i] : NULL;
		if (!t)
			continue;

		/* Bind diffuse texture + matching fullbright mask (uhexen2-sjvf) */
		GL_Bind(t->gl_texturenum);
		glActiveTexture_fp(GL_TEXTURE2);
		glBindTexture_fp(GL_TEXTURE_2D,
			t->gl_fb_texturenum ? t->gl_fb_texturenum : gl_null_fb_texture);
		glActiveTexture_fp(GL_TEXTURE0);

		/* Issue indirect draw for this bucket */
		glDrawElementsIndirect_fp(GL_TRIANGLES, GL_UNSIGNED_INT,
					  (void *)((size_t)i * sizeof(gpu_indirect_cmd_t)));
	}

	glBindBuffer_fp(GL_DRAW_INDIRECT_BUFFER, 0);

	/* Restore original world IBO in the VAO so later code that
	 * binds world_vao gets the correct element buffer */
	{
		extern GLuint world_ibo;
		glBindBuffer_fp(GL_ELEMENT_ARRAY_BUFFER, world_ibo);
	}

	glBindVertexArray_fp(0);
	glUseProgram_fp(0);
	/* External upload to gl_shader_world; clear GL_ImmEnd's uniform
	 * cache so a later ImmEnd reusing the same shader uploads fresh. */
	GL_ImmInvalidateState();

	/* Capture this frame's world MVP for next-frame Hi-Z reprojection.
	 * The pyramid will be built at end-of-3D (after every depth write
	 * has landed), and the cull dispatch one frame from now consults
	 * this matrix when reprojecting AABBs onto the pyramid. */
	memcpy(hiz_prev_mvp, mvp, sizeof(hiz_prev_mvp));
	hiz_prev_mvp_valid = true;
}


#endif /* !__EMSCRIPTEN__ */
#endif /* GLQUAKE */
