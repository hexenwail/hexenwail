/* span_uv.inc -- the 1997 span rasteriser's texture coordinates (#176):
 * exact s/t every `span` pixels along x, linear in between, as D_DrawSpans8
 * computed them.  highp throughout, because the ES tier's default is mediump.
 *
 * A copy of gl_shader.c's GLSL_SPAN_UV_FN, which sworld_frag still splices
 * while the world family is a C literal; the derivation and the 8x depth
 * guard are written up there.  tools/test_alias_frag_block.py keeps the two
 * token-identical, and tools/test_softemu_spans.py compiles the C copy and
 * checks it against the 1997 reference (#450).
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
