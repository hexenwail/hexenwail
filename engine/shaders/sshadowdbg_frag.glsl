/* sshadowdbg_frag -- r_shadow_debug (GitHub #97): layer 0 of the cube
 * shadow-map array, the map cache's slot 0, unwrapped as a horizontal cross, as the light sees it in
 * Quake's frame (x forward, y left, z up).  The middle row looks +Y, +X, -Y,
 * -X from left to right; the cells above and below +X look up and down.  The
 * grey is the stored distance over the map's radius: black at the light,
 * white at the radius, which is also what a texel nothing was drawn into holds.
 *
 * The directions are built here from the layout, NOT from the cube-face table
 * gl_shadow.c draws with, so a face drawn into the wrong slot or the wrong
 * way round shows up as a seam in the picture instead of agreeing with
 * itself.  The sampler is the plain samplerCubeArray: gl_shadow.c switches the
 * texture's compare mode off around this draw.  No uniforms at all, so no
 * block: the layer is always 0.  No #version: gl_shadow.c supplies it.
 */
#include "layout.inc"

FRAG_TEX(0) uniform highp samplerCubeArray u_shadowdbg;

FRAG_IN(0) in vec2 v_uv;
FRAG_OUT(0) out vec4 fragColor;

void main() {
    vec2 c = v_uv * vec2(4.0, 3.0);
    ivec2 cell = ivec2(floor(c));
    vec2 f = fract(c) * 2.0 - 1.0;
    vec3 fwd, right, up = vec3(0.0, 0.0, 1.0);
    if (cell.y == 1) {
        if (cell.x == 0)      { fwd = vec3(0.0, 1.0, 0.0);  right = vec3(1.0, 0.0, 0.0); }
        else if (cell.x == 1) { fwd = vec3(1.0, 0.0, 0.0);  right = vec3(0.0, -1.0, 0.0); }
        else if (cell.x == 2) { fwd = vec3(0.0, -1.0, 0.0); right = vec3(-1.0, 0.0, 0.0); }
        else                  { fwd = vec3(-1.0, 0.0, 0.0); right = vec3(0.0, 1.0, 0.0); }
    } else if (cell.x == 1 && cell.y == 2) {
        fwd = vec3(0.0, 0.0, 1.0);  right = vec3(0.0, -1.0, 0.0); up = vec3(-1.0, 0.0, 0.0);
    } else if (cell.x == 1 && cell.y == 0) {
        fwd = vec3(0.0, 0.0, -1.0); right = vec3(0.0, -1.0, 0.0); up = vec3(1.0, 0.0, 0.0);
    } else {
        fragColor = vec4(0.1, 0.0, 0.1, 1.0);
        return;
    }
    vec3 dir = fwd + f.x * right + f.y * up;
    float v = texture(u_shadowdbg, vec4(dir, 0.0)).r;
    fragColor = vec4(vec3(v), 1.0);
}
