/* span_uv.inc -- the 1997 span rasteriser's texture coordinates (GitHub
 * #176).  D_DrawSpans8 (d_scan.c) divided by z to get exact s/t once every 8
 * pixels along a span and stepped linearly between, so a surface's texture
 * swims a little between those columns and not at all on them -- not the full
 * affine wobble of a rasteriser that never divides.
 *
 * uv * gl_FragCoord.w and gl_FragCoord.w are linear in screen space across a
 * triangle (they are the barycentric sums perspective interpolation divides),
 * so their dFdx is exact, and both can be evaluated at the cell's first pixel
 * and at the first pixel of the next cell.  Divide there, interpolate between:
 * exact at every 8th column, linear in between, as in d_scan.c.  No
 * noperspective qualifier, so the ES tier (WebGL2) runs it too.
 *
 * The cells sit on a screen grid; d_scan.c's started where each span did.  The
 * difference is where the exact columns fall, not how far apart.
 *
 * D_DrawSpans8 also clamped its endpoints to the face's extents, which a
 * shader cannot see.  The bound here instead: a cell whose depth changes by
 * more than 8x across it -- an edge-on sliver, or a cell at a plane's
 * vanishing line, where the far end's 1/w heads for zero and its coordinate
 * for infinity -- keeps the perspective coordinate.  Elsewhere a cell end
 * past the face is at most one cell's extrapolation of the same plane, so a
 * lightmap sample there strays by under a luxel, into the atlas gutter
 * (LM_FillGutter).  highp throughout: the ES tier's default is mediump, and
 * a mediump return would undo the world's highp texcoords (uhexen2-mfql).
 *
 * The one copy for the alias and world stages since the world fragment
 * stages became files (#450, world slice W2); it was gl_shader.c's
 * GLSL_SPAN_UV_FN.  tools/test_softemu_spans.py compiles this function as C++
 * and checks it against the 1997 reference.
 */
#ifndef SPAN_UV_INC
#define SPAN_UV_INC

highp vec2 SpanUV(highp vec2 uv, highp float span) {
    highp float w  = gl_FragCoord.w;
    highp vec2  a  = uv * w;
    highp vec2  da = dFdx(a);
    highp float dw = dFdx(w);
    highp float d0 = floor(gl_FragCoord.x / span) * span + 0.5 - gl_FragCoord.x;
    highp float d1 = d0 + span;
    highp float w0 = w + dw * d0;
    highp float w1 = w + dw * d1;
    if (min(w0, w1) < 0.125 * max(w0, w1)) return uv;
    return mix((a + da * d0) / w0, (a + da * d1) / w1, -d0 / span);
}

#endif /* SPAN_UV_INC */
