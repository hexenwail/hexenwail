/* lightcluster -- the froxel light grid: for each of LIGHT_TILES_X * _Y * _Z
 * froxels, a 128-bit mask of the dynamic lights whose sphere reaches it.
 * Ported from Ironwail's cluster_lights pass; gl_lightcluster.c's header
 * explains the three departures (GL -Z view space, 128 lights, dark lights).
 * Moved out of gl_lightcluster.c's lc_compute_body (#450).
 *
 * No #version line.  gl_lightcluster.c passes a header chunk holding the
 * #version and the sizes below, which are C-side constants (MAX_LIGHTSTYLES
 * is 256 in Hexen II and 64 in HexenWorld).  The #ifndef defaults exist for
 * the offline SPIR-V compile only, which has no C side; they are Hexen II's
 * values, and the GL build never sees them.
 */
#include "layout.inc"

#ifndef LIGHT_TILES_X
  #define LIGHT_TILES_X		32
  #define LIGHT_TILES_Y		16
  #define LIGHT_TILES_Z		32
  #define MAX_LIGHTS		128
  #define MAX_LIGHTSTYLES	256
  #define LIGHT_SSBO_BINDING	8
#endif

/* Everything the pass used to take as loose uniforms.  std140, 160 bytes,
 * mirrored by lc_params_t in gl_lightcluster.c (COMPILE_TIME_ASSERT).  GL
 * binding point 5 (LC_PARAMS_UBO_BINDING), assigned from C as for every
 * block here; UBO(0) is the SDL_GPU slot, the pipeline's only uniform buffer. */
UBO(0) uniform LcParams {
    mat4  u_view;
    mat4  u_transposed_proj;
    vec4  u_lczlog;		/* x log-depth scale, y log-depth bias */
    ivec4 u_lccount;	/* x number of lights */
};
#define u_zlogscale	u_lczlog.x
#define u_zlogbias	u_lczlog.y
#define u_numlights	u_lccount.x

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct Light
{
    vec3  origin;
    float radius;
    vec3  color;
    float minlight;
};

/* The only storage resource: GL point LIGHT_SSBO_BINDING (8, above
 * gl_worldcull.c's 0-7), SDL_GPU set 0 slot 0. */
SSBO_RO(LIGHT_SSBO_BINDING, 0) restrict readonly buffer LightBuffer
{
    float LightStyles[MAX_LIGHTSTYLES];
    Light Lights[];
};

IMGBIND(0, rgba32ui) uniform writeonly uimage3D LightClusters;

shared vec4 local_lights[MAX_LIGHTS];	/* xyz view-space pos, w radius */

vec3 cluster_center;
vec3 cluster_half_size;

/* A tile edge as a view-space plane, straight out of the projection.
 * Convention-independent, so this half is upstream's unchanged. */
vec4 ExtractFrustumPlane(int axis, float ndcval, float side)
{
    vec4 plane = u_transposed_proj[axis] - ndcval * u_transposed_proj[3];
    return inversesqrt(dot(plane.xyz, plane.xyz)) * side * plane;
}

/* Where a ray from the eye crosses view-space depth `dist` in front of it.
 * We look down -Z, so that plane is z == -dist. */
vec3 IntersectDepthPlane(vec3 dir, float dist)
{
    return dir * (-dist / dir.z);
}

void ComputeClusterExtents(uvec3 gid)
{
    const float TileSizeX = 2.0 / float(LIGHT_TILES_X);
    const float TileSizeY = 2.0 / float(LIGHT_TILES_Y);
    float x0 = -1.0 + float(gid.x) * TileSizeX;
    float y0 = -1.0 + float(gid.y) * TileSizeY;
    /* Logarithmic depth slices: slice 0 is the near plane, slice
     * LIGHT_TILES_Z the far one, so froxels stay roughly cubic in
     * screen terms all the way out. */
    float d0 = exp2((float(gid.z)        - u_zlogbias) / u_zlogscale);
    float d1 = exp2((float(gid.z) + 1.0  - u_zlogbias) / u_zlogscale);

    vec4 left   = ExtractFrustumPlane(0, x0,              -1.0);
    vec4 right  = ExtractFrustumPlane(0, x0 + TileSizeX,   1.0);
    vec4 bottom = ExtractFrustumPlane(1, y0,              -1.0);
    vec4 top    = ExtractFrustumPlane(1, y0 + TileSizeY,   1.0);

    /* Two opposite corner rays, as the intersection of the plane pairs
     * that meet there.  cross() fixes the line but not which way along
     * it, so point them into the frustum explicitly rather than relying
     * on a handedness that a projection change could flip. */
    vec3 bl = cross(bottom.xyz, left.xyz);
    vec3 tr = cross(top.xyz, right.xyz);
    if (bl.z > 0.0) bl = -bl;
    if (tr.z > 0.0) tr = -tr;

    vec3 p0 = IntersectDepthPlane(bl, d0);
    vec3 p1 = IntersectDepthPlane(bl, d1);
    vec3 p2 = IntersectDepthPlane(tr, d0);
    vec3 p3 = IntersectDepthPlane(tr, d1);

    vec3 mins = vec3(min(min(p0.xy, p1.xy), min(p2.xy, p3.xy)), -d1);
    vec3 maxs = vec3(max(max(p0.xy, p1.xy), max(p2.xy, p3.xy)), -d0);
    cluster_center    = (maxs + mins) * 0.5;
    cluster_half_size = (maxs - mins) * 0.5;
}

/* Sphere against the froxel's bounding box.  Upstream's cheap test, and
 * it needs no convention at all. */
bool LightTouchesCluster(vec4 l)
{
    vec3 delta = max(abs(l.xyz - cluster_center) - cluster_half_size, 0.0);
    return dot(delta, delta) < l.w * l.w;
}

void main()
{
    uvec3 gid = gl_GlobalInvocationID;
    if (any(greaterThanEqual(gid, uvec3(LIGHT_TILES_X, LIGHT_TILES_Y, LIGHT_TILES_Z))))
        return;

    uint numlights = uint(max(u_numlights, 0));
    if (numlights == 0u)
    {
        imageStore(LightClusters, ivec3(gid), uvec4(0u));
        return;
    }

    uint groupsize = gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z;
    uint ofs;
    for (ofs = 0u; ofs < numlights; ofs += groupsize)
    {
        uint index = gl_LocalInvocationIndex + ofs;
        if (index < numlights)
            local_lights[index] = vec4((u_view * vec4(Lights[index].origin, 1.0)).xyz,
                                       Lights[index].radius);
    }
    memoryBarrierShared();
    barrier();

    ComputeClusterExtents(gid);

    uvec4 mask = uvec4(0u);
    uint i;
    for (i = 0u; i < numlights; i++)
    {
        if (!LightTouchesCluster(local_lights[i]))
            continue;
        uint word = i >> 5u;
        uint bit  = 1u << (i & 31u);
        if      (word == 0u) mask.x |= bit;
        else if (word == 1u) mask.y |= bit;
        else if (word == 2u) mask.z |= bit;
        else                 mask.w |= bit;
    }
    imageStore(LightClusters, ivec3(gid), mask);
}
