/* lightgrid.inc -- what a consumer of the clustered dynamic-light froxel grid
 * needs: the light SSBO's layout and the froxel lookup (uhexen2-26bm,
 * uhexen2-waum).  gl_lightcluster.c's compute pass fills both.
 *
 * DESKTOP ONLY.  Include it under #ifndef GL_ES: the ES / WebGL2 tier has no
 * storage blocks, no findLSB and no compute pass to fill the grid.
 *
 * The includer declares, before including this:
 *   u_lightgrid      the usampler3D of per-froxel light masks
 *   u_lightgrid_xy   vec4: xy viewport origin, zw froxels per pixel
 *   u_lightgrid_z    vec2: zlogscale, zlogbias
 *   LIGHTGRID_SSBO_SLOT  the light buffer's SDL_GPU index in set 2, which is
 *                    the number of sampled textures the stage declares
 *                    (an OIT twin's four more, from oit.inc, are added here
 *                    through OIT_TEX_COUNT, so the stage states one number)
 * so each family can keep those in its own block: the world's in WorldFrag
 * (world.inc), the alias family's in AliasFrag (alias.inc).
 *
 * One copy for both families, which matters more here than anywhere: the slice
 * mapping is only correct while it is the exact inverse of the compute pass's,
 * and two copies drifting would render as "some things take dynamic light a
 * froxel early" rather than as anything obviously wrong.  It was the code half
 * of gl_shader.c's GLSL_LIGHTGRID_DECL / GLSL_LIGHTGRID_FETCH_FN until the
 * world fragment stages became files (#450, world slice W2).  The world's
 * lightstyles (lmstyles.inc) read LightStyles[] from this buffer too.
 *
 * The sizes are C-side constants: gl_shader.c passes them in the alias and
 * world fragment headers (GLSL_LIGHTGRID_CONSTS), so MAX_LIGHTSTYLES follows
 * the game being built.  The #ifndef defaults are Hexen II's values for the offline
 * SPIR-V compile, which has no C side, the same arrangement as
 * lightcluster_comp.glsl.
 */
#ifndef LIGHTGRID_INC
#define LIGHTGRID_INC

#ifndef LIGHT_TILES_X
  #define LIGHT_TILES_X		32
  #define LIGHT_TILES_Y		16
  #define LIGHT_TILES_Z		32
  #define MAX_LIGHTSTYLES	256
  #define LIGHT_SSBO_BINDING	8
#endif

#ifdef OIT_TEX_COUNT
  #define LIGHTGRID_SET2_SLOT	(LIGHTGRID_SSBO_SLOT + OIT_TEX_COUNT)
#else
  #define LIGHTGRID_SET2_SLOT	LIGHTGRID_SSBO_SLOT
#endif

struct Light { vec3 origin; float radius; vec3 color; float minlight; };
/* Layout and binding must match gl_lightcluster.c's compute pass.  LightStyles
 * is not read by the alias family, but it has to be declared or std430 puts
 * Lights[] at the wrong offset; the world reads it for GPU lightstyles. */
FRAG_SSBO(LIGHT_SSBO_BINDING, LIGHTGRID_SET2_SLOT) restrict readonly buffer LightBuffer {
    float LightStyles[MAX_LIGHTSTYLES];
    Light Lights[];
};

/* Which froxel an eye-space position falls in, and that froxel's 128-bit light
 * mask.  max() guards log2 of a non-positive depth. */
uvec4 LightGridMask(highp vec3 p) {
    vec3 tile;
    tile.xy = (gl_FragCoord.xy - u_lightgrid_xy.xy) * u_lightgrid_xy.zw;
    tile.z = log2(max(-p.z, 1.0)) * u_lightgrid_z.x + u_lightgrid_z.y;
    ivec3 t = clamp(ivec3(tile), ivec3(0), ivec3(LIGHT_TILES_X - 1, LIGHT_TILES_Y - 1, LIGHT_TILES_Z - 1));
    return texelFetch(u_lightgrid, t, 0);
}

#endif /* LIGHTGRID_INC */
