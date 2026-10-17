/* ssao_mip -- fill mip 0's y with the scene's view depth (entities in), tag
 * every 128x128 tile where world and scene disagree, and build mips 1-4 of
 * both channels (vkQuake ssao_mip.inc).  vkQuake reduces with subgroup
 * shuffles; core GL 4.3 has none, so this reduces through shared memory with
 * the same arithmetic and the same edge replication.  One workgroup is an 8x8
 * tile of mip 1, i.e. 16x16 of mip 0.  No #version: gl_ssao.c supplies it. */
#include "ssao.inc"

layout (local_size_x = 8, local_size_y = 8) in;

TEXBIND(8) uniform sampler2D u_scene_depth;	/* full-res snapshot, entities included */
IMGBIND(0, rg16f) uniform image2D u_mip0;
IMGBIND(1, rg16f) uniform writeonly image2D u_mip1;
IMGBIND(2, rg16f) uniform writeonly image2D u_mip2;
IMGBIND(3, rg16f) uniform writeonly image2D u_mip3;
IMGBIND(4, rg16f) uniform writeonly image2D u_mip4;
IMGBIND(5, r32ui) uniform uimage2D u_depth_mismatch;

shared vec2 depths1[64];
shared vec2 depths2[16];
shared vec2 depths3[4];
shared uint any_mismatch;

/* XeGTAO_DepthMIPFilter, with vkQuake's 65504 "missing" in either channel. */
float reduce_depth (float a, float b, float c, float d)
{
	float farthest = max (
		max (a < 65504.0 ? a : 0.0, b < 65504.0 ? b : 0.0),
		max (c < 65504.0 ? c : 0.0, d < 65504.0 ? d : 0.0));
	if (farthest == 0.0)
		return 65504.0;
	a = a < 65504.0 ? a : farthest;
	b = b < 65504.0 ? b : farthest;
	c = c < 65504.0 ? c : farthest;
	d = d < 65504.0 ? d : farthest;

	float depth_range_scale_factor = 0.75;
	float effect_radius = depth_range_scale_factor * u_settings.x * 1.457;
	float falloff_range = 0.615 * effect_radius;
	float falloff_from = effect_radius * (1.0 - get_gtao_constants ().effect_falloff_range);
	float falloff_mul = -1.0 / falloff_range;
	float falloff_add = falloff_from / falloff_range + 1.0;

	float weight0 = clamp ((farthest - a) * falloff_mul + falloff_add, 0.0, 1.0);
	float weight1 = clamp ((farthest - b) * falloff_mul + falloff_add, 0.0, 1.0);
	float weight2 = clamp ((farthest - c) * falloff_mul + falloff_add, 0.0, 1.0);
	float weight3 = clamp ((farthest - d) * falloff_mul + falloff_add, 0.0, 1.0);
	float weight_sum = weight0 + weight1 + weight2 + weight3;
	return (weight0 * a + weight1 * b + weight2 * c + weight3 * d) / weight_sum;
}

vec2 reduce (vec2 a, vec2 b, vec2 c, vec2 d)
{
	return vec2 (reduce_depth (a.x, b.x, c.x, d.x), reduce_depth (a.y, b.y, c.y, d.y));
}

/* Each mip-0 pixel has one owner: keep its world depth, add its scene depth. */
vec2 prepare_depth (ivec2 p)
{
	if (any (greaterThanEqual (p, imageSize (u_mip0))))
		return vec2 (65504.0);
	ivec2 src = ssao_source_pixel (p, int (u_projection.w));
	float view_depth = 65504.0;
	if (all (greaterThanEqual (src, ivec2 (0))) && all (lessThan (src, textureSize (u_scene_depth, 0))))
		view_depth = ssao_view_depth (texelFetch (u_scene_depth, src, 0).r);
	vec2 depths = unpackHalf2x16 (packHalf2x16 (vec2 (imageLoad (u_mip0, p).x, view_depth)));
	imageStore (u_mip0, p, vec4 (depths, 0.0, 0.0));
	return depths;
}

/* Clamped reads of a level's shared tile: a partial edge tile replicates its
 * last valid value, as vkQuake's reduction does. */
vec2 read1 (ivec2 p, ivec2 valid) { p = clamp (p, ivec2 (0), valid - 1); return depths1[p.y * 8 + p.x]; }
vec2 read2 (ivec2 p, ivec2 valid) { p = clamp (p, ivec2 (0), valid - 1); return depths2[p.y * 4 + p.x]; }
vec2 read3 (ivec2 p, ivec2 valid) { p = clamp (p, ivec2 (0), valid - 1); return depths3[p.y * 2 + p.x]; }

void main ()
{
	ivec2 local = ivec2 (gl_LocalInvocationID.xy);
	ivec2 group = ivec2 (gl_WorkGroupID.xy);
	ivec2 p = group * 8 + local;	/* mip 1 */
	ivec2 q = p * 2;		/* mip 0 */

	if (gl_LocalInvocationIndex == 0u)
		any_mismatch = 0u;
	barrier ();

	vec2 depth00 = prepare_depth (q);
	vec2 depth10 = prepare_depth (q + ivec2 (1, 0));
	vec2 depth01 = prepare_depth (q + ivec2 (0, 1));
	vec2 depth11 = prepare_depth (q + ivec2 (1, 1));
	if (!(depth00.x == depth00.y && depth10.x == depth10.y && depth01.x == depth01.y && depth11.x == depth11.y))
		atomicOr (any_mismatch, 1u);
	depths1[local.y * 8 + local.x] = reduce (depth00, depth10, depth01, depth11);
	barrier ();

	if (gl_LocalInvocationIndex == 0u && any_mismatch != 0u)
	{
		/* 8x8 workgroups of 16x16 mip-0 pixels make one 128x128 tag; an 8x4
		 * block of tags packs into each 32-bit texel. */
		uvec2 tile = gl_WorkGroupID.xy >> 3;
		uint  bit_index = (tile.y & 3u) * 8u + (tile.x & 7u);
		imageAtomicOr (u_depth_mismatch, ivec2 (tile >> uvec2 (3, 2)), 1u << bit_index);
	}

	/* Valid extent of each level inside this group's tile. */
	ivec2 v1 = clamp (imageSize (u_mip1) - group * 8, ivec2 (1), ivec2 (8));
	ivec2 v2 = clamp (imageSize (u_mip2) - group * 4, ivec2 (1), ivec2 (4));
	ivec2 v3 = clamp (imageSize (u_mip3) - group * 2, ivec2 (1), ivec2 (2));

	if (all (lessThan (p, imageSize (u_mip1))))
		imageStore (u_mip1, p, vec4 (depths1[local.y * 8 + local.x], 0.0, 0.0));

	if (all (lessThan (local, ivec2 (4))))
	{
		ivec2 s = local * 2;
		vec2 d = reduce (read1 (s, v1), read1 (s + ivec2 (1, 0), v1),
				 read1 (s + ivec2 (0, 1), v1), read1 (s + ivec2 (1, 1), v1));
		depths2[local.y * 4 + local.x] = d;
		if (all (lessThan (group * 4 + local, imageSize (u_mip2))))
			imageStore (u_mip2, group * 4 + local, vec4 (d, 0.0, 0.0));
	}
	barrier ();

	if (all (lessThan (local, ivec2 (2))))
	{
		ivec2 s = local * 2;
		vec2 d = reduce (read2 (s, v2), read2 (s + ivec2 (1, 0), v2),
				 read2 (s + ivec2 (0, 1), v2), read2 (s + ivec2 (1, 1), v2));
		depths3[local.y * 2 + local.x] = d;
		if (all (lessThan (group * 2 + local, imageSize (u_mip3))))
			imageStore (u_mip3, group * 2 + local, vec4 (d, 0.0, 0.0));
	}
	barrier ();

	if (gl_LocalInvocationIndex == 0u && all (lessThan (group, imageSize (u_mip4))))
		imageStore (u_mip4, group, vec4 (reduce (read3 (ivec2 (0), v3), read3 (ivec2 (1, 0), v3),
							read3 (ivec2 (0, 1), v3), read3 (ivec2 (1), v3)), 0.0, 0.0));
}
