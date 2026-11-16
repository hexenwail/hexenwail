/* softemu_lut -- build the r_softemu palette LUT on the GPU: for every cell
 * of a LUT_DIM^3 colour grid, the nearest Hexen II palette entry under the
 * metric MODE selects (0 plain RGB, 1 Riemersma, 2 OKLab).
 *
 * Transliterated from Ironwail's palette_init_compute_shader
 * (Quake/gl_shaders.h).  Differences, all ours and all deliberate:
 *  - the palette arrives as a 256x1 texture rather than an SSBO, which drops
 *    a capability this pass would otherwise need for 1 KB of data;
 *  - the search skips index 255, Hexen II's transparency key, exactly as
 *    PP_BuildPaletteLUT_CPU does and for the same reason;
 *  - it stores resolved colour, not the index, because our post-process
 *    fragment shader has no palette to resolve one with (see PP_UploadLUT);
 *  - the grid coordinate is mapped to 8 bits with integer arithmetic,
 *    matching PP_GRID_TO_8BIT, rather than upstream's bit-twiddled version.
 * The maths is line-for-line PP_FillPaletteGrid's in gl_postprocess.c; the
 * two must agree, since the CPU builder is the fallback.
 *
 * Moved from gl_postprocess.c's pp_palette_init_body (#450).  No #version:
 * PP_BuildPaletteLUT_GPU passes a header chunk with the #version, MODE and
 * LUT_DIM.  LUT_DIM used to be the loose `uniform int u_dim`, always set to
 * PP_LUT_DIM; as a compile-time constant it needs no uniform at all, which
 * Vulkan SPIR-V requires of a non-opaque value outside a block.  The #ifndef
 * defaults serve only the offline SPIR-V compile, which has no C side.
 */
#include "layout.inc"

#ifndef MODE
#define MODE 2
#endif
#ifndef LUT_DIM
#define LUT_DIM 128
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 4) in;

IMGBIND(0, rgba8) uniform writeonly image3D u_lut;
TEXBIND(0) uniform sampler2D u_palette;

vec3 sRGBToLinearRGB(vec3 v)
{
    return mix(v / 12.92, pow((v + 0.055) / 1.055, vec3(2.4)), greaterThan(v, vec3(0.04045)));
}

// A perceptual color space for image processing - Bjorn Ottosson
// https://bottosson.github.io/posts/oklab/
vec3 LinearRGBToOKLab(vec3 c)
{
    float l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
    float m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
    float s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;

    float l_ = pow(l, 1./3.);
    float m_ = pow(m, 1./3.);
    float s_ = pow(s, 1./3.);

    return vec3(
        0.2104542553*l_ + 0.7936177850*m_ - 0.0040720468*s_,
        1.9779984951*l_ - 2.4285922050*m_ + 0.4505937099*s_,
        0.0259040371*l_ + 0.7827717662*m_ - 0.8086757660*s_
    );
}

vec3 NormalizeColor(vec3 clr)
{
#if MODE >= 1
    clr = sRGBToLinearRGB(clr);
    #if MODE >= 2
        clr = LinearRGBToOKLab(clr);
    #endif
#endif
    return clr;
}

float ColorDistanceSquared(vec3 c0, vec3 c1)
{
    vec3 delta = c1 - c0;
#if MODE == 1
    // Colour metric - Thiadmer Riemersma
    // https://www.compuphase.com/cmetric.htm
    float rmean = (c0.r + c1.r) * 0.5;
    return dot(delta * delta, vec3(2. + rmean, 4., 3. - rmean));
#elif MODE == 2
    delta.x *= 1.25; // lightness
    delta.z *= 1.5;  // blue-yellow
#endif
    return dot(delta, delta);
}

shared vec3 palsrgb[256];
shared vec3 palmetric[256];

void main()
{
    uint groupsize = gl_WorkGroupSize.x * gl_WorkGroupSize.y * gl_WorkGroupSize.z;
    uint i, ofs;
    for (ofs = 0u; ofs < 256u; ofs += groupsize)
    {
        uint idx = gl_LocalInvocationIndex + ofs;
        if (idx < 256u)
        {
            vec3 c = texelFetch(u_palette, ivec2(int(idx), 0), 0).rgb;
            palsrgb[idx] = c;
            palmetric[idx] = NormalizeColor(c);
        }
    }
    memoryBarrierShared();
    barrier();

    ivec3 gid = ivec3(gl_GlobalInvocationID);
    ivec3 t8 = (gid * 255) / (LUT_DIM - 1);
    vec3 target = NormalizeColor(vec3(t8) * (1. / 255.));

    uint bestidx = 0u;
    float bestdist = 1e+30;
    for (i = 0u; i < 255u; i++)
    {
        float dist = ColorDistanceSquared(target, palmetric[i]);
        if (dist < bestdist)
        {
            bestidx = i;
            bestdist = dist;
        }
    }
    imageStore(u_lut, gid, vec4(palsrgb[bestidx], 1.));
}
