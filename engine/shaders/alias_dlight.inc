/* alias_dlight.inc -- the alias family's half of the clustered dynamic-light
 * grid (uhexen2-waum, phase C).  Desktop only; include it under #ifndef GL_ES,
 * after lightgrid.inc, dlshadow.inc and u_lightview.  Moved from
 * gl_shader.c's GLSL_ALIAS_DLIGHT_FN when salias_frag became a file (#450).
 *
 * WHAT THIS REPLACES.  Every alias model used to take the whole frame's
 * dynamic light as ONE number, computed once at the entity's ORIGIN
 * (R_DrawAliasModel's `add = radius - VectorLengthFast(e->origin - dl->origin)`
 * loop) and folded flat into the vertex colour.  A torch at a monster's feet
 * lit its head exactly as much as its boots, and a model straddling a light's
 * edge either took all of it or none.  This evaluates the same expression per
 * pixel instead, from the froxel grid phase A builds.
 *
 * IT IS NOT A REUSE OF THE WORLD'S ClusteredDlight, for two structural reasons:
 *
 *  - THE FALLOFF IS A DIFFERENT FUNCTION.  The world one is
 *    R_AddDynamicLights' -- project the light onto the surface plane, subtract
 *    the perpendicular distance from the radius, then subtract the in-plane
 *    distance.  That shape exists because a lightmap luxel IS a point on a
 *    plane.  The alias CPU term this replaces is a plain `radius - distance`,
 *    and matching it is the whole point: the only thing phase C is meant to
 *    change is WHERE the expression is evaluated, not what it computes.
 *  - THE NORMAL WOULD BE WRONG.  ClusteredDlight recovers its plane from
 *    screen-space derivatives, which is exact for world surfaces because every
 *    BSP face is planar.  An alias model is not, so the same trick yields the
 *    flat triangle normal and the light would break into visible facets on
 *    exactly the low-poly meshes Hexen II is made of.
 *
 * WHAT DELIBERATELY DOES NOT CARRY OVER FROM THE CPU TERM.  There, the dlight
 * sum is added to the entity's static light BEFORE the vertex is multiplied by
 * the shadedots table (Quake's fake directional shading, keyed to the entity's
 * quantized yaw) and by the colorshade tint.  Here it is added after, so a
 * dynamic light is neither modulated by a fake light direction that has nothing
 * to do with where the torch actually is, nor tinted by a palette recolour that
 * belongs to the skin.  Both are defensible the other way; what is NOT
 * defensible is having the three alias paths disagree, and the shadedot is
 * recoverable in only two of them -- the legacy path streams a finished colour
 * through GL_ImmColor4f with the dot already multiplied in.  Uniformity decided
 * it.  Expect dynamic light on models to sit slightly flatter across a surface
 * and slightly stronger on faces turned away from the shadevector.
 *
 * The scale is per-BATCH state, u_alias_dlight, a VERTEX-stage value; this
 * stage reads it off the flat v_dlightscale varying instead.  The instanced
 * path batches entities whose CPU lighting came from DIFFERENT arms of
 * R_DrawAliasModel's branch chain -- an MLS_ABSLIGHT model and an ordinary one
 * can share a draw call -- and only the ordinary arm ever accumulated dlights.
 * A uniform cannot distinguish them; a flat varying the vertex shader fills
 * from the instance record can.  The other two paths set it from the uniform
 * unchanged.
 *
 * SHADOWED BY THE WORLD under r_dlight_occlusion 2 (#97, P4): each light's
 * term is scaled by DlShadowFactor, its cube shadow map's answer for this pixel
 * (dlshadow.inc), so a torch no longer lights a monster through the wall
 * between them -- per pixel, so a model half round a corner is half lit.  The
 * maps hold the world and brush entities, and under r_shadow_casters 2 the
 * alias models too (P6), so one model can stand in another's shadow.  A
 * light with no map, and every light under r_dlight_occlusion 0 and 1, takes
 * the term untouched.
 */
#ifndef ALIAS_DLIGHT_INC
#define ALIAS_DLIGHT_INC

vec3 ClusteredAliasDlight(vec3 lit, highp vec3 p, float scale) {
    uvec4 mask = LightGridMask(p);
    bool shadowed = u_dlsparams.x > 0.0;
    highp vec3 wp = shadowed ? DlShadowWorldPos(p) : vec3(0.0);
    for (int w = 0; w < 4; w++) {
        uint bits = mask[w];
        while (bits != 0u) {
            int i = (w << 5) + findLSB(bits);
            bits &= bits - 1u;  /* clear that bit before any continue */
            vec3 lpos = (u_lightview * vec4(Lights[i].origin, 1.0)).xyz;
            float add = Lights[i].radius - distance(lpos, p);
            if (add <= 0.0) continue;
            /* Subtractive lights (dlight_t.dark) arrive with their colour
             * negated by the gather in gl_lightcluster.c, so one accumulate
             * serves both kinds.  The clamp is inside the loop and against the
             * RUNNING total, which is what the CPU term did -- its dark arm
             * clamps each channel at zero as it subtracts, so a dark light
             * cannot bank negative headroom that a later bright one spends.
             * Ascending light index is the CPU's order too (findLSB within a
             * word, words in order), so the two agree even where a dark light
             * and a bright one overlap the same pixel. */
            vec3 term = add * scale * Lights[i].color;
            if (shadowed) term *= DlShadowFactor(i, wp);
            lit = max(lit + term, vec3(0.0));
        }
    }
    /* NO UPPER CLAMP, and that is a decision rather than an omission.
     * gl_overbright_models 0 makes the CPU clamp each lightcolor channel at
     * 192 after its dlight loop -- but it does that BEFORE the shadedots
     * multiply, and `lit` here is already past it: v_color carries
     * static * shadedot * tint, and shadedot runs to 2.0.  Re-applying the
     * same ceiling on this side of that multiply would cap a legitimately
     * bright vertex at 0.96 and visibly darken models that took no dynamic
     * light at all.  The faithful expression, min(static + dlight, 192) *
     * shadedot, needs the two factors separately and the legacy path has
     * already multiplied them together in GL_ImmColor4f.
     *
     * Leaving it off is consistent with the shadedot decision above -- a
     * dlight that is not modulated by the fake light direction has no reason
     * to be bounded by a ceiling derived from it -- and it costs little:
     * gl_overbright_models defaults to on, where the CPU path has no ceiling
     * either, and even at the worst case (a full-radius light one unit away)
     * the term adds about 1.0, which is where the CPU's own clamped result
     * lands too. */
    return lit;
}

#endif /* ALIAS_DLIGHT_INC */
