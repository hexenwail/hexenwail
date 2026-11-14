/* sbloom_bright -- threshold extraction: keep the part of each pixel's
 * luminance above u_threshold.  Moved from bloom_bright_frag_src (#450).
 * No #version line; gl_postprocess.c passes PP_FRAG_HEADER.
 */
#include "bloom.inc"

void main() {
    vec3 c = texture(u_scene, v_uv).rgb;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float w = max(0.0, lum - u_threshold);
    fragColor = vec4(c * (w / max(lum, 0.001)), 1.0);
}
