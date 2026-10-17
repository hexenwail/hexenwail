/* ssao.inc -- shared by the r_ssao stages (gl_ssao.c, GitHub #188).
 *
 * Entity contact occlusion, ported from vkQuake (Shaders/ssao_*.inc, GPL-2.0+)
 * whose ground-truth AO is Intel's XeGTAO:
 *
 *   MIT License
 *
 *   Copyright (c) 2016-2021 Intel Corporation
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 * What differs from vkQuake, and why:
 *   - the AO textures are stored TOP-DOWN (row 0 = top of the screen), as
 *     Vulkan's are, by flipping rows wherever a GL (bottom-up) depth texture is
 *     read.  With the vertical projection term negated on the C side, the
 *     ported GTAO math -- its slice directions and normal handedness -- runs
 *     exactly as written instead of being re-derived for a flipped y;
 *   - view depth comes from u_depthlin (z = zb / (za - d), the same
 *     linearisation R_SoftParticleParams derives), not vkQuake's reversed-Z
 *     P14 / d, so forward-Z, reversed-Z and the mirror's split depth range all
 *     work;
 *   - "half" is plain float: GL 4.3 has no 16-bit arithmetic, and the FP16
 *     rounding that makes world and scene depths compare equal is kept by
 *     packing through packHalf2x16 where vkQuake does;
 *   - no subgroup operations (not core GL): the pyramid reduces in shared
 *     memory instead (ssao_mip_comp.glsl).
 */
#include "layout.inc"

#define half  float
#define half2 vec2
#define half3 vec3
#define half4 vec4

/* One block for every stage; each reads the fields it needs.  vec4s only, so
 * std140 cannot be got wrong. */
UBO(0) uniform SsaoParams {
	vec4  u_viewport;	/* xy: AO size; zw: ndc -> view multiplier */
	vec4  u_projection;	/* xy: ndc -> view offset; w: AO scale (1 or 2) */
	vec4  u_settings;	/* x radius, y strength, z fog density / final flag, w slices */
	vec4  u_depthlin;	/* x za, y zb, z the clear depth value, w full-res height */
};

/* Positive view-space distance of a window depth, or 65504 ("missing") for a
 * cleared pixel.  65472 caps real geometry below that, as in vkQuake. */
float ssao_view_depth (float d)
{
	if (d == u_depthlin.z)
		return 65504.0;
	float v = u_depthlin.y / (u_depthlin.x - d);
	return (v > 0.0) ? min (v, 65472.0) : 65504.0;
}

/* Full-res GL pixel for a top-down AO-space pixel at the given scale. */
ivec2 ssao_source_pixel (ivec2 top_down, int scale)
{
	ivec2 p = top_down * scale;
	return ivec2 (p.x, int (u_depthlin.w) - 1 - p.y);
}

struct gtao_constants
{
	ivec2 viewport_size;
	vec2  viewport_pixel_size;
	vec2  ndc_to_view_mul;
	vec2  ndc_to_view_add;
	vec2  ndc_to_view_mul_x_pixel_size;
	float effect_radius;
	float effect_falloff_range;
	float final_value_power;
	float denoise_blur_beta;
	float depth_mip_sampling_offset;
};

gtao_constants get_gtao_constants ()
{
	gtao_constants c;
	c.viewport_size = ivec2 (u_viewport.xy);
	c.viewport_pixel_size = 1.0 / u_viewport.xy;
	c.ndc_to_view_mul = u_viewport.zw;
	c.ndc_to_view_add = u_projection.xy;
	c.ndc_to_view_mul_x_pixel_size = c.ndc_to_view_mul * c.viewport_pixel_size;
	c.effect_radius = u_settings.x;
	c.effect_falloff_range = 0.615;
	c.final_value_power = 2.2;
	c.denoise_blur_beta = 1.2;
	c.depth_mip_sampling_offset = 3.3;
	return c;
}
