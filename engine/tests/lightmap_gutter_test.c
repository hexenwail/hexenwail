#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Lightmap-gutter regression guard (uhexen2-jwzf, issue #113).
 *
 * jwzf gave every lightmap rect a 2-luxel gutter so bicubic stops reading
 * its neighbour: AllocBlock reserves the rect grown by LM_GUTTER on all four
 * sides and hands back the unpadded origin, LM_FillGutter replicates the edge
 * luxels into the padding on every rebuild, and LM_ExpandDirtyRect carries
 * the gutter to the GPU with the rect.  The page was widened 256 -> 260 so
 * the padded worst case (255 + 2*2 = 259) still fits.
 *
 * The invariant is asserted against the source text, in the
 * brush_render_state_test.c pattern: the gutter lives in gl_rsurf.c, a TU
 * that cannot link here without SDL and the GL backend.  This test would
 * have failed pre-jwzf (no LM_GUTTER at all) and passes at HEAD, so unlike
 * the failure mode the brush test warns about, it does discriminate the
 * regression it guards.
 *
 * This guards the fix.  It does NOT resolve #113: the Clean-preset
 * tile-floor line is still unconfirmed on a post-jwzf build, and its
 * remaining hypotheses (mip-band, tearing) need the field re-test, not code.
 *
 * Usage: ./lightmap_gutter_test [gl_rsurf.c] [glquake.h]
 * With no arguments the paths are resolved relative to the engine source
 * dir (CMake passes it explicitly; plain cc falls back to __FILE__).  The
 * arguments exist so the guard can be run against another copy of the
 * sources — e.g. the pre-jwzf gl_rsurf.c as a negative control.
 */

static const char *EngineSourceDir (void)
{
#ifdef LIGHTMAP_TEST_ENGINE_SOURCE_DIR
	return LIGHTMAP_TEST_ENGINE_SOURCE_DIR;
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

static char *ReadSourceFile (const char *path)
{
	FILE	*f;
	long	size;
	char	*text;

	f = fopen(path, "rb");
	if (!f)
	{
		fprintf(stderr, "lightmap gutter: cannot open %s\n", path);
		return NULL;
	}

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0)
	{
		fprintf(stderr, "lightmap gutter: cannot size %s\n", path);
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
		fprintf(stderr, "lightmap gutter: cannot read %s\n", path);
		free(text);
		fclose(f);
		return NULL;
	}

	text[size] = '\0';
	fclose(f);
	return text;
}

static char *ReadRelativeSourceFile (const char *relative_path)
{
	char	path[1024];

	if ((size_t)snprintf(path, sizeof(path), "%s/%s", EngineSourceDir(),
			     relative_path) >= sizeof(path))
	{
		fprintf(stderr, "lightmap gutter: path too long: %s\n",
			relative_path);
		return NULL;
	}

	return ReadSourceFile(path);
}

/* End of a top-level function body: the first "\n}" after its opening brace. */
static const char *FindFunctionBodyEnd (const char *body)
{
	const char *end = strstr(body, "\n}");

	return end ? end : body + strlen(body);
}

static const char *FindFunctionBody (const char *text, const char *signature,
				     const char *what, const char **end_out)
{
	const char *body = strstr(text, signature);

	if (!body)
	{
		fprintf(stderr, "lightmap gutter: %s not found\n", what);
		return NULL;
	}

	*end_out = FindFunctionBodyEnd(body);
	return body;
}

static int TextInRange (const char *text, const char *end, const char *needle,
			const char *what)
{
	const char *hit = strstr(text, needle);

	if (!hit || hit >= end)
	{
		fprintf(stderr, "lightmap gutter: %s\n", what);
		return 1;
	}

	return 0;
}

/* Value of a "#define NAME <int>" line; the first #define with that exact
 * name wins.  Returns 1 and fills *value_out on success, 0 otherwise. */
static int FindDefineValue (const char *text, const char *name, int *value_out)
{
	const char	*cursor = text;
	size_t		namelen = strlen(name);

	for (;;)
	{
		const char	*def = strstr(cursor, "#define");
		const char	*p;
		char		*endptr;
		long		value;

		if (!def)
			return 0;

		p = def + strlen("#define");
		while (*p == ' ' || *p == '\t')
			p++;

		if (strncmp(p, name, namelen) != 0 ||
		    (p[namelen] != ' ' && p[namelen] != '\t'))
		{
			cursor = def + strlen("#define");
			continue;
		}

		p += namelen;
		while (*p == ' ' || *p == '\t')
			p++;

		value = strtol(p, &endptr, 10);
		if (endptr == p)
			return 0;

		*value_out = (int)value;
		return 1;
	}
}

static int CheckGutterDefined (const char *gl_rsurf, int *gutter_out)
{
	int	gutter;

	if (!FindDefineValue(gl_rsurf, "LM_GUTTER", &gutter))
	{
		fprintf(stderr, "lightmap gutter: LM_GUTTER not defined\n");
		return 1;
	}

	if (gutter < 2)
	{
		fprintf(stderr,
			"lightmap gutter: LM_GUTTER is %d, must be at least 2\n",
			gutter);
		return 1;
	}

	*gutter_out = gutter;
	return 0;
}

static int CheckAllocBlock (const char *gl_rsurf)
{
	const char	*body, *end;

	body = FindFunctionBody(gl_rsurf,
		"static unsigned int AllocBlock (int w, int h, int *x, int *y)",
		"AllocBlock", &end);
	if (!body)
		return 1;

	/* The reservation is the rect grown by the gutter on all four sides. */
	if (TextInRange(body, end, "padw = w + LM_GUTTER * 2",
			"AllocBlock does not pad the width") ||
	    TextInRange(body, end, "padh = h + LM_GUTTER * 2",
			"AllocBlock does not pad the height"))
		return 1;

	/* The bounds check runs on the padded size, not the surface size. */
	if (TextInRange(body, end, "padw > BLOCK_WIDTH || padh > BLOCK_HEIGHT",
			"AllocBlock does not bounds-check the padded size"))
		return 1;

	/* Packing itself runs in padded units, so no other allocation can be
	 * placed in the padding.  The needles are the loop header and the
	 * height test themselves, not the bare expressions: the comment above
	 * the loop quotes "BLOCK_WIDTH - padw", so the short form would pass
	 * on the comment alone. */
	if (TextInRange(body, end, "i <= BLOCK_WIDTH - padw; i++",
			"AllocBlock does not pack in padded units") ||
	    TextInRange(body, end, "if (best + padh > BLOCK_HEIGHT)",
			"AllocBlock does not pack in padded units"))
		return 1;

	/* ... while the reported origin is the surface's own top-left luxel. */
	if (TextInRange(body, end, "*x += LM_GUTTER",
			"AllocBlock does not return the unpadded origin") ||
	    TextInRange(body, end, "*y += LM_GUTTER",
			"AllocBlock does not return the unpadded origin"))
		return 1;

	return 0;
}

static int CheckFillGutterCalls (const char *gl_rsurf)
{
	const char	*body, *end;

	if (!strstr(gl_rsurf,
		    "static void LM_FillGutter (byte *block, int stride, int smax, int tmax)"))
	{
		fprintf(stderr, "lightmap gutter: LM_FillGutter not defined\n");
		return 1;
	}

	/* Every rebuild, not just at load: the block is re-written whenever a
	 * dlight or a light style touches it.  The needle is the call itself:
	 * the comment above it names LM_FillGutter, so the bare name would
	 * pass on the comment alone. */
	body = FindFunctionBody(gl_rsurf,
		"static void R_BuildLightMap (msurface_t *surf, byte *dest, int stride)",
		"R_BuildLightMap", &end);
	if (!body)
		return 1;
	if (TextInRange(body, end, "LM_FillGutter (block, stride, smax, tmax);",
			"R_BuildLightMap does not fill the gutter"))
		return 1;

	/* The GPU lightstyles layers (#439) are built by the same shape. */
	body = FindFunctionBody(gl_rsurf,
		"static void LM_BuildRawLayer (const msurface_t *surf, int map, byte *block, int stride)",
		"LM_BuildRawLayer", &end);
	if (!body)
		return 1;
	if (TextInRange(body, end, "LM_FillGutter (block, stride, smax, tmax);",
			"LM_BuildRawLayer does not fill the gutter"))
		return 1;

	return 0;
}

static int CheckDirtyRect (const char *gl_rsurf)
{
	const char	*body, *end;

	body = FindFunctionBody(gl_rsurf,
		"static void LM_ExpandDirtyRect (int page, int x, int y, int w, int h)",
		"LM_ExpandDirtyRect", &end);
	if (!body)
		return 1;

	/* The gutter travels to the GPU with the rect, on all four sides. */
	if (TextInRange(body, end, "x -= LM_GUTTER",
			"LM_ExpandDirtyRect does not expand left") ||
	    TextInRange(body, end, "y -= LM_GUTTER",
			"LM_ExpandDirtyRect does not expand up") ||
	    TextInRange(body, end, "w += LM_GUTTER * 2",
			"LM_ExpandDirtyRect does not expand right") ||
	    TextInRange(body, end, "h += LM_GUTTER * 2",
			"LM_ExpandDirtyRect does not expand down"))
		return 1;

	return 0;
}

static int CheckFit (const char *gl_rsurf, const char *glquake, int gutter)
{
	int	block_width, block_height, max_surface, need;

	if (!FindDefineValue(gl_rsurf, "BLOCK_WIDTH", &block_width) ||
	    !FindDefineValue(gl_rsurf, "BLOCK_HEIGHT", &block_height))
	{
		fprintf(stderr,
			"lightmap gutter: BLOCK_WIDTH/BLOCK_HEIGHT not defined\n");
		return 1;
	}

	if (!FindDefineValue(glquake, "MAX_SURFACE_LIGHTMAP", &max_surface))
	{
		fprintf(stderr,
			"lightmap gutter: MAX_SURFACE_LIGHTMAP not defined\n");
		return 1;
	}

	/* The page must be at least as big as the biggest surface the loader
	 * admits, grown by the gutter — or the padded worst case stops fitting
	 * and AllocBlock rejects surfaces the loader accepts. */
	need = max_surface + gutter * 2;
	if (block_width < need || block_height < need)
	{
		fprintf(stderr,
			"lightmap gutter: %dx%d page cannot hold the padded "
			"worst case %d (MAX_SURFACE_LIGHTMAP %d + 2*LM_GUTTER %d)\n",
			block_width, block_height, need, max_surface, gutter);
		return 1;
	}

	return 0;
}

int main (int argc, char **argv)
{
	char	*gl_rsurf, *glquake;
	int	gutter;
	int	result = 1;

	if (argc > 1)
		gl_rsurf = ReadSourceFile(argv[1]);
	else
		gl_rsurf = ReadRelativeSourceFile("hexen2/gl_rsurf.c");
	if (!gl_rsurf)
		return 1;

	if (argc > 2)
		glquake = ReadSourceFile(argv[2]);
	else
		glquake = ReadRelativeSourceFile("h2shared/glquake.h");
	if (!glquake)
	{
		free(gl_rsurf);
		return 1;
	}

	if (CheckGutterDefined(gl_rsurf, &gutter) != 0 ||
	    CheckAllocBlock(gl_rsurf) != 0 ||
	    CheckFillGutterCalls(gl_rsurf) != 0 ||
	    CheckDirtyRect(gl_rsurf) != 0 ||
	    CheckFit(gl_rsurf, glquake, gutter) != 0)
		goto done;

	result = 0;
done:
	free(gl_rsurf);
	free(glquake);
	return result;
}
