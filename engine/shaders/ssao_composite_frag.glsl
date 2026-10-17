/* ssao_composite -- darken the world where entities occlude it (vkQuake
 * ssao_composite.inc).  A receiver is a world pixel no entity covers: its two
 * depth snapshots, before and after the entities, are bitwise equal.  vkQuake
 * marks entity pixels in the stencil instead; the snapshots make that
 * unnecessary here.  Blended as (0, 0, 0, contribution) over the scene, which
 * multiplies it by 1 - contribution.  No #version: gl_ssao.c supplies it. */
#include "ssao.inc"

TEXBIND(8) uniform sampler2D u_world_depth;	/* full res, GL rows */
TEXBIND(9) uniform sampler2D u_scene_depth;
TEXBIND(10) uniform sampler2D filtered_ao;	/* AO res, top-down rows */
TEXBIND(11) uniform sampler2D prepared_depth;	/* the pyramid, level 0 */

layout (location = 0) out vec4 out_color;

/* vkQuake's bilateral upsample from a half-resolution AO, unchanged. */
float UpsampleVisibility (ivec2 p, float depth)
{
	ivec2 size = textureSize (filtered_ao, 0);
	if (all (equal (size, textureSize (u_world_depth, 0))))
		return texelFetch (filtered_ao, p, 0).x;

	vec2  position = vec2 (p) * 0.5;
	ivec2 base = ivec2 (floor (position));
	vec2  blend = fract (position);
	float sum = 0.0;
	float total_weight = 0.0;
	float nearest_difference = 1e30;
	float nearest_visibility = 1.0;
	float depth_scale = 1.0 / max (depth * 0.01, 0.01);
	for (int y = 0; y < 2; ++y)
	{
		for (int x = 0; x < 2; ++x)
		{
			ivec2 q = clamp (base + ivec2 (x, y), ivec2 (0), size - 1);
			float sample_depth = texelFetch (prepared_depth, q, 0).x;
			float visibility = texelFetch (filtered_ao, q, 0).x;
			float difference = abs (sample_depth - depth);
			vec2  weight_xy = mix (1.0 - blend, blend, bvec2 (x != 0, y != 0));
			float weight = weight_xy.x * weight_xy.y * exp2 (-difference * depth_scale);
			sum += visibility * weight;
			total_weight += weight;
			if (difference < nearest_difference)
			{
				nearest_difference = difference;
				nearest_visibility = visibility;
			}
		}
	}
	return total_weight > 1e-5 ? sum / total_weight : nearest_visibility;
}

void main ()
{
	ivec2 p = ivec2 (gl_FragCoord.xy);
	float world = texelFetch (u_world_depth, p, 0).r;
	float scene = texelFetch (u_scene_depth, p, 0).r;
	/* r_ssao_debug 2: the receiver mask, opaque white on an entity's own
	 * pixel (u_settings.w carries the debug view here). */
	if (u_settings.w == 2.0)
	{
		out_color = vec4 (vec3 (scene != world ? 1.0 : 0.0), 1.0);
		return;
	}
	if (world == u_depthlin.z || scene != world)
	{
		if (u_settings.w == 1.0)
		{
			out_color = vec4 (1.0);
			return;
		}
		discard;	/* sky, or an entity's own pixel: not a receiver */
	}

	float depth = ssao_view_depth (world);
	float visibility = UpsampleVisibility (ivec2 (p.x, int (u_depthlin.w) - 1 - p.y), depth);
	float occlusion = 1.0 - visibility;
	/* The world shader's fog, exp(-(density * distance)^2), with view depth
	 * standing in for its radial distance as vkQuake's composite does. */
	float fog = u_settings.z * depth;
	float transmittance = exp (-fog * fog);
	float contribution = transmittance * u_settings.y * occlusion;
	/* Linear-light attenuation, approximated for a gamma-encoded scene. */
	contribution = 1.0 - pow (clamp (1.0 - contribution, 0.0, 1.0), 1.0 / 2.2);
	if (u_settings.w == 1.0)
	{
		out_color = vec4 (vec3 (1.0 - contribution), 1.0);
		return;
	}
	out_color = vec4 (0.0, 0.0, 0.0, contribution);
}
