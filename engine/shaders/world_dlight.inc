/* world_dlight.inc -- CLUSTERED GPU DYNAMIC LIGHTING, the world's shading
 * half.  uhexen2-26bm, phase B of uhexen2-a5nn.1; gl_lightcluster.c is the
 * half that fills the grid, lightgrid.inc the froxel lookup this calls.
 *
 * Every fragment finds the froxel it sits in, reads that froxel's 128-bit light
 * mask, and evaluates the handful of lights the mask names.  What it replaces
 * is R_AddDynamicLights: a CPU walk that rewrote lightmap blocks and re-uploaded
 * every dirtied atlas page, every frame, for every torch.
 *
 * DESKTOP GL 4.3 ONLY, and not by choice.  The whole thing needs an SSBO and a
 * compute pass to fill the grid, and the ES/WebGL2 tier has neither (WebGL2 is
 * GLSL ES 3.00: no shader storage blocks, no findLSB).  Include this under
 * #ifndef GL_ES, after lightgrid.inc; the CPU path stays live on that tier
 * permanently -- finding 1 on uhexen2-a5nn.1, and the reason this is an
 * ADDITIONAL path rather than a replacement.
 *
 * THE MATH IS R_AddDynamicLights, PER PIXEL.  Same shape: reduce the radius by
 * the light's distance to the surface plane, drop the light if what remains is
 * under its minlight, project the light onto the plane, and fall off linearly
 * from there.  Three differences, all of which make the GPU version the more
 * correct one, and the first of which is VISIBLE and worth knowing about:
 *
 *   - THE DISTANCE IS MEASURED IN WORLD UNITS.  The CPU path measures it in
 *     TEXTURE units: its `local` comes from DotProduct(impact, tex->vecs[i]),
 *     and a surface's texture vectors are the texture axes divided by its
 *     texture scale, so they are only unit length at scale 1.  The falloff then
 *     compares that texture-space distance against a radius in world units.
 *     The upshot is a dynamic light whose SIZE DEPENDS ON THE TEXTURE SCALE of
 *     whatever it lands on -- measured on demo1's corridor floor, |vecs| is
 *     0.6667 (scale 1.5), so the CPU pool there is half again as wide as the
 *     light's actual radius, and correspondingly brighter at any given point.
 *     This is inherited Quake behaviour, not something Hexen II added, and
 *     Ironwail's per-pixel path does not reproduce it.  Neither does this.
 *     EXPECT DYNAMIC LIGHTS TO TIGHTEN on scaled-texture surfaces; that is the
 *     bug being removed, and it is the bulk of what an A/B between the two
 *     paths shows.
 *   - THE IN-PLANE DISTANCE IS EUCLIDEAN, not the CPU's `max + min/2` octagon,
 *     a software-rasteriser trick for dodging a square root per luxel.  It
 *     over-estimates diagonal distance by about 6%, so it draws lights very
 *     slightly octagonal.  Small next to the above, and the opposite sign.
 *   - IT IS EVALUATED PER PIXEL, not per 16-unit luxel and interpolated.  Close
 *     up against a wall this is the visible win: the light no longer has a
 *     lattice.
 *
 * THE NORMAL COMES FROM SCREEN-SPACE DERIVATIVES of the eye-space position,
 * which is exact here -- every BSP face is planar by construction, so the face
 * normal IS the surface normal with no interpolation error.  Same reasoning as
 * the material-map tangent frame (material.inc), and the same reason neither
 * needs a normal attribute on the vertex.  Its sign follows triangle winding
 * and screen orientation, so it is arbitrary, and it is turned toward the eye
 * (the eye-space origin) before anything reads it.  That is the face's FRONT:
 * with gl_cull on (the default; the mirror pass swaps the face) the rasterizer
 * passes only the side a BSP face's winding faces, so the side a fragment is
 * seen from is the side its lightmap belongs to.  Under gl_cull 0 a back face
 * seen from inside solid is lit from the eye's side, which nobody can see.  The projection
 * N * dot(lpos - p, N) does not care which way N points; the test below does.
 *
 * A LIGHT BEHIND THE FACE DOES NOT LIGHT IT (GitHub #97, r_dlight_occlusion).
 * R_AddDynamicLights lit both sides of a plane, so a torch lit the far side of
 * its own wall; the one-sided test drops a light more than u_dlight_onesided
 * units behind the face's front plane, the same test the CPU twins make
 * against the BSP plane (gl_rsurf.c, gl_rlight.c, r_surf.c, r_light.c).  The
 * tolerance, about 2 units, is for torches whose origin sits a unit or two
 * inside their wall; the lit side keeps abs(pdist), so a light inside that
 * band falls off exactly as it did.  0 is vanilla, both sides: what
 * r_dlight_occlusion 0 writes, and what lit water writes around its batches
 * (EmitWaterPolys), because a liquid surface is seen, and lit, from both.
 *
 * THE ALIAS HALF HAS NO SUCH TEST, deliberately.  alias_dlight.inc lights a
 * model as a point-ish receiver with no surface plane to be behind: a torch
 * behind a monster's back still lights the monster, as it should, and what
 * would stop it lighting one through a wall is visibility, not a plane.
 *
 * IT WORKS IN EYE SPACE, so lights are transformed by u_lightview per light per
 * fragment.  The froxel grid is defined in eye space, v_eyepos is the one
 * position this shader has that is correct for BOTH world surfaces and brush
 * ents (the entity transform lives inside u_modelview and never reaches
 * a_position), and one space for the lookup and the shading is one space to get
 * right.  u_lightview is the view matrix WITHOUT the entity transform, which is
 * exactly the matrix the grid was clustered with.
 *
 * A NOTE ON WHY THE NORMAL IS COMPUTED BEFORE THE MASK IS TESTED.  It is
 * tempting to bail out early on an empty mask, and wrong: derivatives are
 * undefined in non-uniform control flow, neighbouring pixels in a 2x2 quad
 * routinely fall in different froxels, and the resulting garbage normal would
 * show up as speckle along froxel boundaries.  The only branch this may sit
 * behind is `u_dlight_scale > 0.0`, which is uniform across the draw.
 *
 * Moved from gl_shader.c's GLSL_DLIGHT_FN with the world fragment stages
 * (#450, world slice W2); token-identical to its ClusteredDlight.  The alias
 * family's half is alias_dlight.inc, which says why it is not a reuse of this.
 */
#ifndef WORLD_DLIGHT_INC
#define WORLD_DLIGHT_INC

vec3 ClusteredDlight(vec3 lm, highp vec3 p) {
    /* Geometric normal first -- see the header comment on derivatives. */
    highp vec3 N = normalize(cross(dFdx(p), dFdy(p)));
    if (dot(N, p) > 0.0) N = -N;   /* toward the eye: the face's front */
    uvec4 mask = LightGridMask(p);
    for (int w = 0; w < 4; w++) {
        uint bits = mask[w];
        while (bits != 0u) {
            int i = (w << 5) + findLSB(bits);
            bits &= bits - 1u;  /* clear that bit before any continue */
            vec3 lpos = (u_lightview * vec4(Lights[i].origin, 1.0)).xyz;
            float pdist = dot(lpos - p, N);
            if (u_dlight_onesided > 0.0 && pdist < -u_dlight_onesided) continue;
            float rad = Lights[i].radius - abs(pdist);
            float ml = Lights[i].minlight;
            if (rad < ml) continue;
            float d = length((lpos - N * pdist) - p);
            if (d >= rad - ml) continue;
            /* Hexen II's subtractive lights (dlight_t.dark, EF_DARKLIGHT)
             * ride with the colour negated by the gather in
             * gl_lightcluster.c, so one accumulate serves both.  The clamp at
             * zero is not decoration: it is what the `dark` arm of
             * R_AddDynamicLights does, clamping against the running total
             * rather than the final one, which is why this clamps inside the
             * loop and not after it.  Lights are visited in ascending index
             * order -- findLSB within a word, words in order -- which is the
             * order the CPU applies them in, so the two agree even where a
             * dark light and a bright one overlap.
             *
             * ONE PLACE THEY CANNOT AGREE, and it is not fixable here.  The
             * CPU subtracts from a 32-bit blocklight accumulator that has not
             * been clamped yet, so on a surface whose static light already
             * exceeds full scale a dark light eats headroom the player never
             * sees and appears to do nothing.  All this shader has is the
             * atlas texel, which was clamped to 255 on the way in, so the
             * same dark light visibly darkens.  Ours is the behaviour the
             * effect was presumably after; recording it because it is a
             * genuine difference an A/B in a very bright room will show, not
             * a bug to go hunting for.  Reproducing the CPU exactly would
             * mean carrying the unclamped sum, which the 8-bit atlas cannot. */
            lm = max(lm + (rad - d) * u_dlight_scale * Lights[i].color, vec3(0.0));
        }
    }
    /* R_BuildLightMap clamps the blocklight sum at 255 before it reaches the
     * atlas, so the CPU path cannot exceed a full lightmap either.  Matching
     * it keeps the A/B honest.  There IS headroom to be had here -- nothing
     * forces a GPU-evaluated light through an 8-bit texel -- but taking it
     * would be a brightness change dressed up as a port. */
    return min(lm, vec3(1.0));
}

#endif /* WORLD_DLIGHT_INC */
