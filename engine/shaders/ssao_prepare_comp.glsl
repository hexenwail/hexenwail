/* ssao_prepare -- the world's view-space depth into mip 0's x, before any
 * entity is drawn (vkQuake ssao_prepare.inc).  y, the scene depth, is filled
 * by ssao_mip once the entities are in.  No #version: gl_ssao.c supplies it. */
#include "ssao.inc"

layout (local_size_x = 8, local_size_y = 8) in;

TEXBIND(8) uniform sampler2D u_world_depth;	/* full-res snapshot, DEPTH24_STENCIL8 */
IMGBIND(0, rg16f) uniform writeonly image2D u_prepared;

void main ()
{
	ivec2 o = ivec2 (gl_GlobalInvocationID.xy);
	if (any (greaterThanEqual (o, imageSize (u_prepared))))
		return;
	ivec2 src = ssao_source_pixel (o, int (u_projection.w));
	float view_depth = 65504.0;
	if (all (greaterThanEqual (src, ivec2 (0))) && all (lessThan (src, textureSize (u_world_depth, 0))))
		view_depth = ssao_view_depth (texelFetch (u_world_depth, src, 0).r);
	/* Round explicitly, as ssao_mip does, so a world and a scene depth from
	 * the same pixel compare equal. */
	view_depth = unpackHalf2x16 (packHalf2x16 (vec2 (view_depth))).x;
	imageStore (u_prepared, o, vec4 (view_depth, 65504.0, 0.0, 0.0));
}
