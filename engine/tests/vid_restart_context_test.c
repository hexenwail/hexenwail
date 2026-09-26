/*
 * vid_restart GL-context lifetime regression (issue #205).
 *
 * VID_ChangeVideoMode really destroys and recreates the GL context, so every
 * texture name minted against the old one is dead -- and because GL reuses low
 * names, each dead name is immediately aliased onto a different live object.
 * Three engine-side caches outlive the teardown and used to keep handing those
 * names out: TexMgr's managed pool, the notexture/nulltexture placeholders
 * (plus r_notexture_mip->gl_texturenum) and gl_sky.c's skybox cache.
 *
 * Two halves to the invariant, and the second is the subtle one:
 *
 *   1. VID_ChangeVideoMode must invalidate those caches before destroying the
 *      context and rebuild them after the new one is current.
 *   2. The invalidation must NOT delete the GL names.  Sky_CacheFlush, the
 *      function that looks like it belongs here, deletes the cached faces and
 *      cubemaps -- on a recreated context that destroys whatever now owns
 *      those names, which is worse than leaking them.
 *
 * Like brush_render_state_test, this asserts against source text: the state
 * involved is file-static inside translation units that cannot link without
 * SDL and the GL backend, so a linkable unit test could only restate the fix.
 * Against the pre-fix tree every case below fails -- VID_ChangeVideoMode named
 * neither function and neither function existed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	failures;

static void Fail (const char *fmt, const char *a, const char *b)
{
	fprintf(stderr, "FAIL: ");
	fprintf(stderr, fmt, a, b);
	fprintf(stderr, "\n");
	failures++;
}

/*
 * CMake passes the engine source dir explicitly, which is cwd-independent.
 * CI compiles this file directly with plain cc and no -D, so fall back to
 * __FILE__ with the "tests/" tail stripped rather than failing to build.
 */
static const char *EngineSourceDir (void)
{
#ifdef VID_RESTART_TEST_ENGINE_SOURCE_DIR
	return VID_RESTART_TEST_ENGINE_SOURCE_DIR;
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
	size_t	got;

	if ((size_t)snprintf(path, sizeof(path), "%s/%s", EngineSourceDir(),
			     relative_path) >= sizeof(path))
	{
		fprintf(stderr, "FAIL: path too long for %s\n", relative_path);
		failures++;
		return NULL;
	}

	f = fopen(path, "rb");
	if (!f)
	{
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		failures++;
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0)
	{
		fclose(f);
		fprintf(stderr, "FAIL: cannot size %s\n", path);
		failures++;
		return NULL;
	}
	rewind(f);
	text = (char *)malloc((size_t)size + 1);
	if (!text)
	{
		fclose(f);
		fprintf(stderr, "FAIL: out of memory reading %s\n", path);
		failures++;
		return NULL;
	}
	got = fread(text, 1, (size_t)size, f);
	fclose(f);
	text[got] = '\0';
	return text;
}

/*
 * Body of the function whose signature line starts with `signature`, from the
 * opening brace to the first closing brace in column 0.  Every function in
 * this codebase is K&R-braced at column 0, so that terminator is exact.
 * Returns a malloc'd copy, or NULL (and records a failure) if not found.
 */
static char *FunctionBody (const char *source, const char *signature, const char *where)
{
	const char	*start, *open, *end;
	char		*body;
	size_t		len;

	start = strstr(source, signature);
	if (!start)
	{
		Fail("%s: no function matching '%s'", where, signature);
		return NULL;
	}
	open = strchr(start, '{');
	if (!open)
	{
		Fail("%s: no body for '%s'", where, signature);
		return NULL;
	}
	end = strstr(open, "\n}");
	if (!end)
	{
		Fail("%s: unterminated body for '%s'", where, signature);
		return NULL;
	}
	len = (size_t)(end - open);
	body = (char *)malloc(len + 1);
	if (!body)
	{
		Fail("%s: out of memory for '%s'", where, signature);
		return NULL;
	}
	memcpy(body, open, len);
	body[len] = '\0';
	return body;
}

/*
 * The comments inside these functions name the very symbols the cases below
 * forbid, because that rationale is the point of the fix.  A raw strstr over
 * the body would read the explanation as the defect, so strip block and line
 * comments before matching.
 */
static char *StripComments (const char *body)
{
	size_t	n = strlen(body);
	char	*out = (char *)malloc(n + 1);
	size_t	i = 0, o = 0;

	if (!out)
		return NULL;
	while (i < n)
	{
		if (body[i] == '/' && i + 1 < n && body[i + 1] == '*')
		{
			const char *close = strstr(body + i + 2, "*" "/");
			i = close ? (size_t)(close - body) + 2 : n;
		}
		else if (body[i] == '/' && i + 1 < n && body[i + 1] == '/')
		{
			while (i < n && body[i] != '\n')
				i++;
		}
		else
		{
			out[o++] = body[i++];
		}
	}
	out[o] = '\0';
	return out;
}

static void RequireContains (const char *body, const char *needle, const char *where)
{
	if (!strstr(body, needle))
		Fail("%s: expected to call %s", where, needle);
}

static void RequireAbsent (const char *body, const char *needle, const char *where)
{
	if (strstr(body, needle))
		Fail("%s: must not reference %s", where, needle);
}

/* `first` must appear, and appear before `second`. */
static void RequireOrder (const char *body, const char *first, const char *second, const char *where)
{
	const char	*a = strstr(body, first);
	const char	*b = strstr(body, second);
	char		msg[256];

	if (!a || !b)
	{
		if (!a)
			Fail("%s: expected to call %s", where, first);
		if (!b)
			Fail("%s: expected to call %s", where, second);
		return;
	}
	if (a > b)
	{
		snprintf(msg, sizeof(msg), "%s: %%s must come before %%s", where);
		Fail(msg, first, second);
	}
}

int main (void)
{
	char	*source, *body, *clean;

	/*
	 * Case 1 -- VID_ChangeVideoMode invalidates before the teardown and
	 * rebuilds after it.  Sky_ContextLost must precede TexMgr_ContextLost:
	 * the skybox cache holds gltexture_t * into the pool TexMgr empties.
	 */
	source = ReadSourceFile("h2shared/gl_vidsdl.c");
	if (source)
	{
		body = FunctionBody(source, "static void VID_ChangeVideoMode (int newmode)",
				    "VID_ChangeVideoMode");
		if (body && (clean = StripComments(body)) != NULL)
		{
			RequireOrder(clean, "Sky_ContextLost", "TexMgr_ContextLost",
				     "VID_ChangeVideoMode");
			RequireOrder(clean, "TexMgr_ContextLost", "SDL_GL_DestroyContext",
				     "VID_ChangeVideoMode");
			RequireOrder(clean, "SDL_GL_DestroyContext", "TexMgr_Init",
				     "VID_ChangeVideoMode");
			RequireOrder(clean, "TexMgr_Init", "Sky_ContextRestored",
				     "VID_ChangeVideoMode");
			/* Sky_ContextRestored reloads the faces off disk, so it
			 * has to run after the models are back, not before. */
			RequireOrder(clean, "Mod_ReloadTextures", "Sky_ContextRestored",
				     "VID_ChangeVideoMode");
			free(clean);
		}
		free(body);
		free(source);
	}

	/*
	 * Case 2 -- TexMgr_ContextLost zeroes, never deletes.  The names are
	 * already gone; the new context has reissued them to live textures.
	 */
	source = ReadSourceFile("h2shared/gl_texmgr.c");
	if (source)
	{
		body = FunctionBody(source, "void TexMgr_ContextLost (void)", "TexMgr_ContextLost");
		if (body && (clean = StripComments(body)) != NULL)
		{
			RequireAbsent(clean, "glDeleteTextures", "TexMgr_ContextLost");
			RequireContains(clean, "managed_textures", "TexMgr_ContextLost");
			RequireContains(clean, "num_managed_textures = 0", "TexMgr_ContextLost");
			RequireContains(clean, "notexture_val", "TexMgr_ContextLost");
			RequireContains(clean, "nulltexture_val", "TexMgr_ContextLost");
			RequireContains(clean, "r_notexture_mip->gl_texturenum = 0",
					"TexMgr_ContextLost");
			free(clean);
		}
		free(body);
		free(source);
	}

	/*
	 * Case 3 -- Sky_ContextLost drops the cache without touching GL, and
	 * clears skybox_name so the reload is not swallowed by Sky_LoadSkyBox's
	 * same-name early return.
	 */
	source = ReadSourceFile("h2shared/gl_sky.c");
	if (source)
	{
		body = FunctionBody(source, "void Sky_ContextLost (void)", "Sky_ContextLost");
		if (body && (clean = StripComments(body)) != NULL)
		{
			RequireAbsent(clean, "glDeleteTextures", "Sky_ContextLost");
			RequireAbsent(clean, "TexMgr_FreeTexture", "Sky_ContextLost");
			RequireAbsent(clean, "Sky_CacheFlush", "Sky_ContextLost");
			RequireContains(clean, "skybox_cache = NULL", "Sky_ContextLost");
			RequireContains(clean, "skybox_name[0] = 0", "Sky_ContextLost");
			RequireContains(clean, "skybox_texnums[i] = 0", "Sky_ContextLost");
			RequireContains(clean, "skybox_textures[i] = NULL", "Sky_ContextLost");
			/* zeroes skybox_cubemap and drops the staging pixels */
			RequireContains(clean, "Sky_ResetCubemapBuild", "Sky_ContextLost");
			free(clean);
		}
		free(body);

		body = FunctionBody(source, "void Sky_ContextRestored (void)",
				    "Sky_ContextRestored");
		if (body && (clean = StripComments(body)) != NULL)
		{
			RequireContains(clean, "Sky_LoadSkyBox", "Sky_ContextRestored");
			free(clean);
		}
		free(body);
		free(source);
	}

	if (failures)
	{
		fprintf(stderr, "vid_restart_context_test: %d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	printf("vid_restart_context_test: all cases passed\n");
	return EXIT_SUCCESS;
}
