#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brush_render_state.h"

/*
 * Poly-count ordering regression (see R_RenderBmodelFace / R_RenderBrushPoly).
 *
 * The counters are plain globals inside renderer translation units that cannot
 * be linked here without dragging in SDL and the GL backend, so the invariant
 * is asserted against the source text instead: within the counting function,
 * the increment must appear after the zero-alpha early return.  A unit test
 * built on the classifier alone could only restate R_ClassifyBrushRenderState
 * and would have passed while the bug was live.
 */
typedef struct
{
	const char	*path;		/* relative to the engine source dir */
	const char	*function;	/* signature prefix that opens the body */
	const char	*counter;	/* increment that must come last */
} poly_count_order_case_t;

/*
 * CMake passes the engine source dir explicitly, which is cwd-independent.
 * CI compiles this file directly with plain cc and no -D, so fall back to
 * __FILE__ with the "tests/" tail stripped rather than failing to build.
 */
static const char *EngineSourceDir (void)
{
#ifdef BRUSH_TEST_ENGINE_SOURCE_DIR
	return BRUSH_TEST_ENGINE_SOURCE_DIR;
#else
	static char	dir[1024];
	const char	*self = __FILE__;
	const char	*tail = strstr(self, "tests/");
	size_t		len;

	if (!tail || tail == self)
		return ".";

	len = (size_t)(tail - self) - 1;	/* drop the separator too */
	if (len >= sizeof(dir))
		return ".";

	memcpy(dir, self, len);
	dir[len] = '\0';
	return dir;
#endif
}

static char *ReadSourceFile (const char *relative_path)
{
	char	path[1024];
	FILE	*f;
	long	size;
	char	*text;

	if ((size_t)snprintf(path, sizeof(path), "%s/%s", EngineSourceDir(),
			     relative_path) >= sizeof(path))
	{
		fprintf(stderr, "poly count order: path too long: %s\n", relative_path);
		return NULL;
	}

	f = fopen(path, "rb");
	if (!f)
	{
		fprintf(stderr, "poly count order: cannot open %s\n", path);
		return NULL;
	}

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0)
	{
		fprintf(stderr, "poly count order: cannot size %s\n", path);
		fclose(f);
		return NULL;
	}

	text = (char *) malloc((size_t)size + 1);
	if (!text)
	{
		fclose(f);
		return NULL;
	}

	if (fread(text, 1, (size_t)size, f) != (size_t)size)
	{
		fprintf(stderr, "poly count order: cannot read %s\n", path);
		free(text);
		fclose(f);
		return NULL;
	}

	text[size] = '\0';
	fclose(f);
	return text;
}

/* End of a top-level function body: the first "\n}" after its opening brace. */
static const char *FindFunctionBodyEnd (const char *body)
{
	const char *end = strstr(body, "\n}");

	return end ? end : body + strlen(body);
}

static int CheckPolyCountOrder (const poly_count_order_case_t *c)
{
	char		*text;
	const char	*body, *end, *guard, *count;
	int		result = 1;

	text = ReadSourceFile(c->path);
	if (!text)
		return 1;

	body = strstr(text, c->function);
	if (!body)
	{
		fprintf(stderr, "poly count order: %s not found in %s\n",
			c->function, c->path);
		goto done;
	}

	end = FindFunctionBodyEnd(body);
	guard = strstr(body, "if (!render_state.visible)");
	count = strstr(body, c->counter);

	if (!guard || guard >= end)
	{
		fprintf(stderr, "poly count order: no visibility guard in %s\n",
			c->function);
		goto done;
	}
	if (!count || count >= end)
	{
		fprintf(stderr, "poly count order: %s not incremented in %s\n",
			c->counter, c->function);
		goto done;
	}
	if (count < guard)
	{
		fprintf(stderr,
			"poly count order: %s increments before the zero-alpha "
			"early return in %s\n", c->counter, c->function);
		goto done;
	}

	result = 0;
done:
	free(text);
	return result;
}

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
		{"drawflag regular", {false, 1.0f, 1.0f, true, BRUSH_SURFACE_REGULAR}, true, true, false, 1.0f},
		/* Vanilla coupling: a regular DRF_TRANSLUCENT brush takes the caller's
		 * default, which gl_rsurf.c fills in from r_wateralpha (issue #250). */
		{"drawflag regular uses caller default", {false, 1.0f, 0.33f, true, BRUSH_SURFACE_REGULAR}, true, true, false, 0.33f},
		{"drawflag opaque liquid", {false, 1.0f, 1.0f, true, BRUSH_SURFACE_LIQUID}, true, false, false, 1.0f},
		{"drawflag invisible regular", {true, 0.0f, 1.0f, true, BRUSH_SURFACE_REGULAR}, false, false, false, 0.0f},
		{"drawflag explicit regular", {true, 0.25f, 1.0f, true, BRUSH_SURFACE_REGULAR}, true, true, false, 0.25f},
		{"drawflag explicit opaque regular", {true, 1.0f, 0.5f, true, BRUSH_SURFACE_REGULAR}, true, true, false, 1.0f}
	};
	static const poly_count_order_case_t order_cases[] =
	{
		{"h2shared/r_draw.c",
		 "void R_RenderBmodelFace (bedge_t *pedges, msurface_t *psurf)",
		 "r_polycount++"},
		{"hexen2/gl_rsurf.c",
		 "void R_RenderBrushPoly (entity_t *e, msurface_t *fa, qboolean override)",
		 "c_brush_polys++"}
	};
	unsigned int i;

	for (i = 0; i < sizeof(order_cases) / sizeof(order_cases[0]); i++)
	{
		if (CheckPolyCountOrder(&order_cases[i]) != 0)
			return 1;
	}

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
