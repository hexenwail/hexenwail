/* dlshadow.inc -- the receiving half of the dynamic-light cube shadow maps
 * (GitHub #97, P4).  gl_shadow.c draws the maps; this is what ClusteredDlight
 * (world_dlight.inc) and ClusteredAliasDlight (alias_dlight.inc) ask, per
 * light, after the radius and minlight rejects: how much of this light reaches
 * this point.  Desktop only: include it under #ifndef GL_ES, after the stage's
 * own blocks and u_lightview (world.inc / alias.inc), after oit.inc in an OIT
 * twin, and after declaring
 *
 *   u_dlshadowmap        the samplerCubeArrayShadow, texture unit DLSHADOW_TMU
 *   DLSHADOW_UBO_SLOT    the stage's own fragment-block count, which is this
 *                        block's SDL_GPU slot in set 3 -- one more in an OIT
 *                        twin, whose OitFrag comes first (OIT_TEX_COUNT is
 *                        oit.inc's mark that it was included), the same
 *                        arrangement lightgrid.inc makes for its buffer
 *
 * WHAT A MAP HOLDS.  One cube per shadowed light, its six faces drawn from the
 * light keeping only the faces turned away from it (gl_shadow.c culls
 * GL_BACK, which is that set: tools/test_shadow_cube_faces.py), so each texel
 * holds the distance to the first surface seen from behind -- the far side of whatever wall the ray
 * entered -- as a fraction of the radius the map was drawn with.  A receiver
 * on the light's side of a wall is nearer than that wall's far side by at least
 * the wall's thickness, so it passes the compare without any tuned bias; one
 * beyond the wall is farther than it, and fails.  That is the "bias is the wall
 * thickness" property, and the bias below only has to cover nearest-texel
 * quantisation along a grazing ray, not depth precision (a 16-bit unorm over a
 * radius of a few hundred units resolves well under a unit).
 *
 * WHICH LIGHTS HAVE ONE.  At most DLSHADOW_MAX_MAPS of a frame's lights
 * (r_shadow_maxlights), chosen and cached by gl_shadow.c.  u_dlsmask has light
 * i's bit set when it has a map; the k-th mapped light in ascending light
 * index -- k is the popcount of the mask below i -- has its map at layer
 * (u_dlslayers[k >> 3] >> 4 * (k & 7)) & 15, so the layer a light's map lives
 * in is free to stay put in the cache while the lights around it come and go.
 * A light without a bit is unshadowed, exactly as before this existed, so the
 * light order the dark-light clamp depends on (world_dlight.inc) is untouched:
 * this only scales each light's term.
 *
 * THE LOOKUP USES THE ORIGIN THE MAP WAS DRAWN FROM, u_dlsorigin[layer].xyz,
 * not the light's current one.  The cache keeps a map while its light moves
 * less than a few units, and a map read from somewhere it was not drawn from is
 * self-inconsistent: its own wall would self-shadow on one side and leak on the
 * other.  Read from its own origin it is merely a few units late.
 *
 * WORLD SPACE, because the map is.  The fragment arrives in eye space;
 * u_lightview is the view WITHOUT any entity transform, a rotation and a
 * translation (the mirror pass flips the projection, never this), so its
 * inverse is the transpose of its rotation part and DlShadowWorldPos is exact
 * for world surfaces, brush entities and alias models alike.
 *
 * NO MIPS, AND MIN == MAG FILTER (both NEAREST under r_shadow_filter 0, both
 * LINEAR -- hardware 2x2 PCF -- under 1).  The lookup runs inside the light
 * loop, in non-uniform control flow, where implicit derivatives are undefined;
 * with one level and one filter the level of detail cannot change the result.
 *
 * Every block member is vec4 or ivec4, for the reason uniforms.inc gives.  The C
 * mirror is r_dlshadow_t in gl_shadow.c; tools/test_dlshadow_block.py keeps the
 * two equal.
 */
#ifndef DLSHADOW_INC
#define DLSHADOW_INC

#ifdef OIT_TEX_COUNT
  #define DLSHADOW_SET3_SLOT	(DLSHADOW_UBO_SLOT + 1)
#else
  #define DLSHADOW_SET3_SLOT	DLSHADOW_UBO_SLOT
#endif
#define DLSHADOW_MAX_MAPS	16

/* GL binding point 15 (UBO_BINDING_DLSHADOW), assigned from C. */
FRAG_UBO(DLSHADOW_SET3_SLOT) uniform DlShadow {
    highp ivec4 u_dlsmask;      /*   0  bit i & 31 of word i >> 5: light i has a map */
    highp ivec4 u_dlslayers;    /*  16  4-bit map layer of the k-th mapped light, 8 per word */
    highp vec4  u_dlsparams;    /*  32  x 1 = any light has a map, y bias in units,
                                 *      z bias per unit of distance, w unused */
    highp vec4  u_dlsorigin[DLSHADOW_MAX_MAPS];
                                /*  48  per layer: xyz the origin it was drawn from,
                                 *      w 1 / the radius it was drawn with */
};                              /* 304 bytes */

/* The world position of eye-space p; see the header for why the transpose. */
highp vec3 DlShadowWorldPos(highp vec3 p) {
    return transpose(mat3(u_lightview)) * (p - u_lightview[3].xyz);
}

/* 1 where light i reaches world point wp, 0 where its map says something is in
 * the way, in between at a PCF edge.  1 for a light with no map. */
float DlShadowFactor(int i, highp vec3 wp) {
    uint bits = uint(u_dlsmask[i >> 5]);
    uint bit = 1u << uint(i & 31);
    if ((bits & bit) == 0u) return 1.0;
    int k = bitCount(bits & (bit - 1u));
    for (int w = 0; w < (i >> 5); w++)
        k += bitCount(uint(u_dlsmask[w]));
    int layer = (u_dlslayers[k >> 3] >> (4 * (k & 7))) & 15;
    highp vec4 o = u_dlsorigin[layer];
    highp vec3 d = wp - o.xyz;
    highp float dist = length(d);
    /* At most 1, what a texel nothing was drawn into holds: a point beyond
     * the radius the map was drawn with -- a light that outgrew a map the
     * update budget has not redrawn yet -- is then lit wherever the map saw
     * nothing in the way, instead of everywhere being "behind" the clear. */
    highp float ref = min((dist - u_dlsparams.y - dist * u_dlsparams.z) * o.w, 1.0);
    return texture(u_dlshadowmap, vec4(d, float(layer)), ref);
}

#endif /* DLSHADOW_INC */
