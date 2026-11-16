/* softemu_noise.inc -- software-emulation noise, ported from Ironwail
 * (Quake/gl_shaders.h, NOISE_FUNCTIONS).  uhexen2-a5nn.3.
 *
 * bayer01 is the same ALU-only 16x16 ordered matrix gl_postprocess.c already
 * carries -- that one returns it centred on zero, this one in [0,1) because
 * tri() wants it that way.  tri() reshapes a uniform distribution into a
 * triangular one, which is what makes ordered dither read as film grain rather
 * than as a visible checker.
 *
 * whitenoise01 is "Hash without Sine", https://www.shadertoy.com/view/4djSRW
 *
 *   Copyright (c) 2014 David Hoskins.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the "Software"),
 *   to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *   and/or sell copies of the Software, and to permit persons to whom the
 *   Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 *   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *   DEALINGS IN THE SOFTWARE.
 *
 * Moved from gl_shader.c's GLSL_SOFTEMU_NOISE_FN with the world fragment
 * stages (#450, world slice W2); token-identical to it on each tier.
 */
#ifndef SOFTEMU_NOISE_INC
#define SOFTEMU_NOISE_INC

#ifdef GL_ES
/* bitfieldReverse is GLSL 4.00 / ES 3.10; the ES tier is 3.00.  Same fallback
 * gl_postprocess.c carries for its own copy of the Bayer matrix.  This was the
 * USE_GLES arm of gl_shader.c's GLSL_BITFIELD_REVERSE; GL_ES, which the ES
 * compiler predefines, makes the same choice. */
uint bitfieldReverse(uint v) {
    v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);
    v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v >> 4) & 0x0F0F0F0Fu);
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    return (v << 16) | (v >> 16);
}
#endif

float bayer01(ivec2 coord) {
    coord &= 15;
    coord.y ^= coord.x;
    uint v = uint(coord.y | (coord.x << 8));
    v = (v ^ (v << 2)) & 0x3333u;
    v = (v ^ (v << 1)) & 0x5555u;
    v |= v >> 7;
    v = bitfieldReverse(v) >> 24;
    return float(v) * (1.0/256.0);
}
float whitenoise01(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
/* Uniform [0,1] -> triangular [-1,1].  Based on
 * https://www.shadertoy.com/view/4t2SDh */
float tri(float x) {
    float orig = x * 2.0 - 1.0;
    uint signbit = floatBitsToUint(orig) & 0x80000000u;
    x = sqrt(abs(orig)) - 1.0;
    x = uintBitsToFloat(floatBitsToUint(x) ^ signbit);
    return x;
}
#define SCREEN_NOISE()     tri(bayer01(ivec2(floor(gl_FragCoord.xy) + 0.5)))
#define SUPPRESS_BANDING() (bayer01(ivec2(gl_FragCoord.xy)) - 0.5)

#endif /* SOFTEMU_NOISE_INC */
