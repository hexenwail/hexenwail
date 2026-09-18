/*
 * brush_render_state.h -- backend-neutral brush entity render policy
 *
 * Entity alpha arrives in renderer-owned canonical form.  In particular,
 * callers decode any protocol representation before filling this input: the
 * classifier deliberately does not know about network encodings.
 */
#ifndef H2_BRUSH_RENDER_STATE_H
#define H2_BRUSH_RENDER_STATE_H

#include "q_stdinc.h"

typedef enum
{
	BRUSH_SURFACE_REGULAR,
	BRUSH_SURFACE_LIQUID,
	BRUSH_SURFACE_CUTOUT
} brush_surface_kind_t;

typedef struct
{
	qboolean		has_explicit_alpha;
	float		explicit_alpha;
	float		default_alpha;
	qboolean		drawflag_translucent;
	brush_surface_kind_t	surface_kind;
} brush_render_state_input_t;

typedef struct
{
	qboolean	has_explicit_alpha;
	qboolean	visible;
	qboolean	translucent;
	qboolean	cutout;
	float		alpha;
} brush_render_state_t;

/*
 * Classify the semantic state shared by brush renderers.  Liquids use their
 * effective alpha instead of DRF_TRANSLUCENT, because their per-liquid alpha
 * intentionally overrides that legacy entity flag.  Cutouts retain cutout
 * intent while an explicit partial alpha still routes them through a
 * translucent pass.
 */
static inline brush_render_state_t R_ClassifyBrushRenderState (
	const brush_render_state_input_t *input)
{
	brush_render_state_t state;

	state.has_explicit_alpha = input->has_explicit_alpha;
	state.alpha = input->has_explicit_alpha ? input->explicit_alpha :
		input->default_alpha;
	state.visible = state.alpha > 0.0f;
	state.cutout = input->surface_kind == BRUSH_SURFACE_CUTOUT;
	state.translucent = false;

	if (!state.visible)
		return state;

	switch (input->surface_kind)
	{
	case BRUSH_SURFACE_LIQUID:
		state.translucent = state.alpha < 1.0f;
		break;
	case BRUSH_SURFACE_CUTOUT:
		/* A default cutout remains opaque even if a legacy DRF flag is set;
		 * an explicit alpha keeps that flag's translucent intent. */
		state.translucent = state.alpha < 1.0f ||
			(input->has_explicit_alpha && input->drawflag_translucent);
		break;
	case BRUSH_SURFACE_REGULAR:
	default:
		state.translucent = input->drawflag_translucent ||
			state.alpha < 1.0f;
		break;
	}

	return state;
}

#endif /* H2_BRUSH_RENDER_STATE_H */
