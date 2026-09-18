#include <stdio.h>

#include "brush_render_state.h"

typedef struct
{
	const char			*name;
	brush_render_state_input_t	input;
	qboolean			expect_visible;
	qboolean			expect_translucent;
	qboolean			expect_cutout;
	float			expect_alpha;
} brush_render_state_case_t;

int main (void)
{
	static const brush_render_state_case_t cases[] =
	{
		{"default regular", {false, 1.0f, 1.0f, false, BRUSH_SURFACE_REGULAR}, true, false, false, 1.0f},
		{"default liquid", {false, 1.0f, 0.5f, false, BRUSH_SURFACE_LIQUID}, true, true, false, 0.5f},
		{"default cutout", {false, 1.0f, 1.0f, true, BRUSH_SURFACE_CUTOUT}, true, false, true, 1.0f},
		{"invisible regular", {true, 0.0f, 1.0f, false, BRUSH_SURFACE_REGULAR}, false, false, false, 0.0f},
		{"invisible liquid", {true, 0.0f, 0.5f, true, BRUSH_SURFACE_LIQUID}, false, false, false, 0.0f},
		{"invisible cutout", {true, 0.0f, 1.0f, true, BRUSH_SURFACE_CUTOUT}, false, false, true, 0.0f},
		{"partial regular", {true, 0.5f, 1.0f, false, BRUSH_SURFACE_REGULAR}, true, true, false, 0.5f},
		{"partial liquid", {true, 0.5f, 1.0f, false, BRUSH_SURFACE_LIQUID}, true, true, false, 0.5f},
		{"partial cutout", {true, 0.5f, 1.0f, false, BRUSH_SURFACE_CUTOUT}, true, true, true, 0.5f},
		{"opaque regular", {true, 1.0f, 0.5f, false, BRUSH_SURFACE_REGULAR}, true, false, false, 1.0f},
		{"opaque liquid", {true, 1.0f, 0.5f, true, BRUSH_SURFACE_LIQUID}, true, false, false, 1.0f},
		{"opaque cutout with drawflag", {true, 1.0f, 0.5f, true, BRUSH_SURFACE_CUTOUT}, true, true, true, 1.0f},
		{"opaque cutout", {true, 1.0f, 0.5f, false, BRUSH_SURFACE_CUTOUT}, true, false, true, 1.0f},
		{"drawflag regular", {false, 1.0f, 1.0f, true, BRUSH_SURFACE_REGULAR}, true, true, false, 1.0f}
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
	{
		brush_render_state_t state = R_ClassifyBrushRenderState(&cases[i].input);
		if (state.has_explicit_alpha != cases[i].input.has_explicit_alpha ||
		    state.visible != cases[i].expect_visible ||
		    state.translucent != cases[i].expect_translucent ||
		    state.cutout != cases[i].expect_cutout ||
		    state.alpha != cases[i].expect_alpha)
		{
			fprintf(stderr, "brush render state case failed: %s\n", cases[i].name);
			return 1;
		}
	}

	return 0;
}
