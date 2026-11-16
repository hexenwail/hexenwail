/* lmstyles.inc -- GPU lightstyles (GitHub #439).  The atlas then holds each
 * surface's first style raw, the three layer atlases at the same UVs styles
 * 2-4, and every layer's alpha that layer's style index (255: none).  Summed
 * here weighted by LightStyles[] -- the light-cluster SSBO, lightgrid.inc --
 * and the CPU path's shift and clamp applied after, so the result matches the
 * combined lightmap it replaces to within rounding.  The layer-1 alpha is read
 * with a plain bilinear fetch first: a surface with one style (most of them)
 * pays that and nothing more.  Indices are constant over a surface's rect and
 * its gutter, so every filter tap agrees on them.
 *
 * DESKTOP ONLY: it needs the SSBO.  Include it under #ifndef GL_ES, after
 * lightgrid.inc and bicubic_lm.inc, with u_lmlayer1..3 declared.
 *
 * Moved from gl_shader.c's GLSL_LMSTYLES_FN with the world fragment stages
 * (#450, world slice W2); token-identical to it.
 */
#ifndef LMSTYLES_INC
#define LMSTYLES_INC

float LMStyle(float a) {
    int i = int(a * 255.0 + 0.5);
    return (i < MAX_LIGHTSTYLES) ? LightStyles[i] : 1.0;
}
vec4 LMLayer(sampler2D t, highp vec2 uv) {
    return (u_lightmap_bicubic > 0.5) ? BicubicLightmap(t, uv) : texture(t, uv);
}
vec4 LMStyles(vec4 lm0, highp vec2 uv) {
    const float NONE = 254.5 / 255.0;
    vec3 l = lm0.rgb * LMStyle(lm0.a);
    float a = texture(u_lmlayer1, uv).a;
    if (a < NONE) {
        l += LMLayer(u_lmlayer1, uv).rgb * LMStyle(a);
        a = texture(u_lmlayer2, uv).a;
        if (a < NONE) {
            l += LMLayer(u_lmlayer2, uv).rgb * LMStyle(a);
            a = texture(u_lmlayer3, uv).a;
            if (a < NONE) l += LMLayer(u_lmlayer3, uv).rgb * LMStyle(a);
        }
    }
    return vec4(min(l * u_lmstyles.y, vec3(1.0)), 1.0);
}

#endif /* LMSTYLES_INC */
