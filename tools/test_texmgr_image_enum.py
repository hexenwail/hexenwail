#!/usr/bin/env python3
"""imagelist/imagedump must see gl_texmgr.c's texture pool (issue #127).

Skybox faces are uploaded through TexMgr_LoadImage into gl_texmgr.c's own
managed_textures[] pool, not gltextures[].  (The scrolling sky, upsky/lowsky,
is not: R_InitSky loads it with GL_LoadTexture.)  The two debug commands used
to walk gltextures[] only, so `imagelist gfx/env` printed nothing with a
skybox loaded, and imagedump wrote everything but the sky faces.

Three parts:
  1. Compile the real gl_texmgr.c against a stub quakedef.h and exercise
     TexMgr_TextureAt: placeholders, loaded faces, flag translation, a freed
     slot disappearing, slot recycling, and long sky names keeping their
     face suffix.
  2. Compile the real GL_ImageDumpEntry against a fake GL.  The readback
     buffer must be sized from GL's level-0 size, the bind must not go
     through the currenttexture cache, and names GL no longer recognises or
     that now hold a larger texture (a dead name after vid_restart,
     reissued) must be skipped.
  3. Source-level: GL_ImageList_f and the desktop GL_ImageDump_f both walk
     the TexMgr pool as well as gltextures[], and imagedump restores the
     GL state it changes.
"""

from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEXMGR = ROOT / "engine/h2shared/gl_texmgr.c"
GLQUAKE = ROOT / "engine/h2shared/glquake.h"
GLDRAW = ROOT / "engine/h2shared/gl_draw.c"


def define(text, name):
    m = re.search(r"#define\s+%s\s+(\S+(?:\s*<<\s*\d+\))?)" % name, text)
    if not m:
        raise AssertionError(f"cannot find #define {name}")
    return m.group(1)


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


glq = GLQUAKE.read_text()
TEX_RGBA = define(glq, "TEX_RGBA")
TEX_ALPHA = define(glq, "TEX_ALPHA")
TEXPREF_ALPHA = define(TEXMGR.read_text(), "TEXPREF_ALPHA")

STUB_QUAKEDEF = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef unsigned char byte;
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef int qboolean;
#define MAX_QPATH 64
#define GL_UNUSED_TEXTURE (~(GLuint)0)
#define TEX_RGBA @TEX_RGBA@
#define TEX_ALPHA @TEX_ALPHA@
enum { GL_TEXTURE_2D = 1, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER,
       GL_NEAREST, GL_LINEAR, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T,
       GL_REPEAT, GL_RGBA, GL_UNSIGNED_BYTE, GL_TEXTURE0, GL_TEXTURE1 };
typedef struct { GLuint texnum; char identifier[MAX_QPATH];
                 int width, height; int flags; unsigned short crc; } gltexture_t;
typedef struct qmodel_s qmodel_t;
typedef struct { GLuint gl_texturenum; } texture_t;
extern texture_t *r_notexture_mip;
extern GLuint currenttexture;
#define GL_Bind(x) do {} while (0)
extern int deleted;
static GLuint next_name = 1;
static inline void glGenTextures_fp (int n, GLuint *t) { (void)n; *t = next_name++; }
static inline void glDeleteTextures_fp (int n, GLuint *t) { (void)n; (void)t; deleted++; }
static inline void glBindTexture_fp (GLenum a, GLuint b) { (void)a; (void)b; }
static inline void glTexParameterf_fp (GLenum a, GLenum b, float c) { (void)a; (void)b; (void)c; }
static inline void glTexImage2D_fp (GLenum a, int b, int c, int w, int h, int d,
				    GLenum e, GLenum f, const void *p)
{ (void)a; (void)b; (void)c; (void)w; (void)h; (void)d; (void)e; (void)f; (void)p; }
static inline void glActiveTexture_fp (GLenum a) { (void)a; }
#define Con_Printf printf
static inline void q_strlcpy (char *d, const char *s, size_t n) { snprintf (d, n, "%s", s); }
'''.replace("@TEX_RGBA@", TEX_RGBA).replace("@TEX_ALPHA@", TEX_ALPHA)

HARNESS = r'''
#include <assert.h>
#include "quakedef.h"
unsigned int d_8to24table[256], d_8to24table_fbright[256],
	d_8to24table_nobright[256], d_8to24table_conchars[256];
static texture_t notex_mip;
texture_t *r_notexture_mip = &notex_mip;
GLuint currenttexture;
int deleted;

void TexMgr_Init (void);
int TexMgr_NumTextures (void);
const gltexture_t *TexMgr_TextureAt (int i, int *texflags);
enum srcformat {SRC_INDEXED, SRC_LIGHTMAP, SRC_RGBA, SRC_EXTERNAL};
gltexture_t *TexMgr_LoadImage (qmodel_t *owner, char *name, int width, int height,
	enum srcformat format, byte *data, char *source_file, uintptr_t source_offset,
	unsigned flags);
void TexMgr_FreeTexture (gltexture_t *kill);

/* Every live texture whose identifier starts with prefix, as imagelist
 * filters them. */
static int count_prefix (const char *prefix)
{
	int i, n = 0, flags;
	const gltexture_t *t;
	for (i = 0; i < TexMgr_NumTextures (); i++)
		if ((t = TexMgr_TextureAt (i, &flags)) != NULL &&
		    !strncmp (t->identifier, prefix, strlen (prefix)))
			n++;
	return n;
}

int main (void)
{
	static unsigned int face[64 * 64];
	static const char *suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};
	gltexture_t *faces[6];
	const gltexture_t *t;
	char name[64];
	int i, flags;

	/* Placeholders are not uploaded until TexMgr_Init. */
	assert (TexMgr_NumTextures () == 2);
	assert (TexMgr_TextureAt (0, &flags) == NULL);
	TexMgr_Init ();
	t = TexMgr_TextureAt (0, &flags);
	assert (t && !strcmp (t->identifier, "notexture"));
	t = TexMgr_TextureAt (1, &flags);
	assert (t && !strcmp (t->identifier, "nulltexture"));
	assert (TexMgr_TextureAt (-1, &flags) == NULL);
	assert (TexMgr_TextureAt (2, &flags) == NULL);

	/* The bug: six skybox faces loaded, none enumerable. */
	for (i = 0; i < 6; i++)
	{
		snprintf (name, sizeof(name), "gfx/env/test_%s", suf[i]);
		faces[i] = TexMgr_LoadImage (NULL, name, 64, 64, SRC_RGBA,
					     (byte *)face, name, 0, 0);
	}
	assert (count_prefix ("gfx/env") == 6);
	t = TexMgr_TextureAt (2, &flags);
	assert (t == faces[0] && t->width == 64 && t->height == 64);
	assert (flags == TEX_RGBA);

	/* TEXPREF_ALPHA is reported in imagelist's TEX_* vocabulary. */
	TexMgr_LoadImage (NULL, "alpha_test", 8, 8, SRC_RGBA, (byte *)face, "", 0,
			  @TEXPREF_ALPHA@);
	assert (TexMgr_TextureAt (8, &flags) != NULL);
	assert (flags == (TEX_RGBA | TEX_ALPHA));

	/* A freed face vanishes from the listing, and its slot is reused
	 * rather than growing the pool. */
	TexMgr_FreeTexture (faces[2]);
	assert (count_prefix ("gfx/env") == 5);
	assert (TexMgr_TextureAt (4, &flags) == NULL);
	i = TexMgr_NumTextures ();
	t = TexMgr_LoadImage (NULL, "gfx/env/other_lf", 32, 32, SRC_RGBA,
			      (byte *)face, "", 0, 0);
	assert (t == faces[2] && TexMgr_NumTextures () == i);
	assert (count_prefix ("gfx/env") == 6);

	/* A sky name long enough to overflow identifier[]: the six faces
	 * must stay distinct, keep the gfx/env/ prefix, and keep their
	 * face suffix.  Plain truncation made all six identical. */
	{
		static const char *longsky =
			"gfx/env/an_unreasonably_long_custom_skybox_name_from_a_mod";
		gltexture_t *lf[6];
		char full[128];
		int j;
		size_t n;

		for (i = 0; i < 6; i++)
		{
			snprintf (full, sizeof(full), "%s_%s.png_face%d", longsky, suf[i], i);
			assert (strlen (full) >= MAX_QPATH);
			lf[i] = TexMgr_LoadImage (NULL, full, 4, 4, SRC_RGBA,
						  (byte *)face, full, 0, 0);
			n = strlen (lf[i]->identifier);
			assert (n == MAX_QPATH - 1);
			assert (!strncmp (lf[i]->identifier, "gfx/env/", 8));
			assert (!strcmp (lf[i]->identifier + n - strlen (full + strlen (longsky)),
					 full + strlen (longsky)));
		}
		for (i = 0; i < 6; i++)
			for (j = i + 1; j < 6; j++)
				assert (strcmp (lf[i]->identifier, lf[j]->identifier) != 0);
		for (i = 0; i < 6; i++)
			TexMgr_FreeTexture (lf[i]);
	}

	printf ("PASS: TexMgr pool enumeration\n");
	return 0;
}
'''.replace("@TEXPREF_ALPHA@", TEXPREF_ALPHA)


def compiled_check():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "quakedef.h").write_text(STUB_QUAKEDEF)
        (tmp / "harness.c").write_text(HARNESS)
        exe = tmp / "harness"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall", "-Werror", "-Wno-unused-function",
                        "-Wno-unused-parameter", "-I", str(tmp),
                        str(tmp / "harness.c"), str(TEXMGR), "-o", str(exe)],
                       check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
        print(out.stdout.strip())


DUMP_PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte;
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef int qboolean;
#define true 1
#define false 0
#define MAX_QPATH 64
#define MAX_OSPATH 256
enum { GL_TEXTURE_2D = 1, GL_TEXTURE_WIDTH, GL_TEXTURE_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE };
typedef struct { GLuint texnum; char identifier[MAX_QPATH];
                 int width, height; int flags; unsigned short crc; } gltexture_t;
#define Con_Printf printf
#define q_snprintf snprintf
static void q_strlcpy (char *d, const char *s, size_t n) { snprintf (d, n, "%s", s); }

/* A fake GL: a few named textures, one binding, and a readback that writes
 * exactly as many bytes as the bound texture holds -- which is what a real
 * glGetTexImage does to an undersized buffer. */
typedef struct { GLuint name; int w, h, alive; } faketex_t;
static faketex_t fake[] = {
	{ 5, 64, 64, 1 },	/* ordinary texture */
	{ 6, 32, 32, 1 },	/* recorded 64x64, uploaded under gl_picmip 1 */
	{ 9, 256, 256, 1 },	/* what GL really has bound at the start */
	{ 22, 16, 16, 1 },	/* a dead 2x2 name the new context reissued */
};
static GLuint bound = 9, currenttexture;
static size_t last_alloc;
static int png_w, png_h, pngs;
static char png_path[512];

static faketex_t *find (GLuint n)
{
	size_t i;
	for (i = 0; i < sizeof(fake) / sizeof(fake[0]); i++)
		if (fake[i].name == n && fake[i].alive)
			return &fake[i];
	return NULL;
}
static GLboolean glIsTexture_fp (GLuint n) { return find (n) != NULL; }
static void glBindTexture_fp (GLenum t, GLuint n) { (void)t; bound = n; }
static void glGetTexLevelParameteriv_fp (GLenum t, GLint l, GLenum p, GLint *v)
{
	faketex_t *f = find (bound);
	(void)t; assert (l == 0);
	*v = f ? (p == GL_TEXTURE_WIDTH ? f->w : f->h) : 0;
}
static void glGetTexImage_fp (GLenum t, GLint l, GLenum fmt, GLenum ty, void *buf)
{
	faketex_t *f = find (bound);
	(void)t; (void)l; (void)fmt; (void)ty;
	assert (f);
	assert ((size_t)f->w * f->h * 4 <= last_alloc);	/* the heap overrun */
	memset (buf, 0xab, (size_t)f->w * f->h * 4);
}
/* What the old code did: trust the cache and skip the real bind. */
#define GL_Bind(n) do { if (currenttexture != (n)) { currenttexture = (n); glBindTexture_fp (GL_TEXTURE_2D, (n)); } } while (0)
static void *test_malloc (size_t n) { last_alloc = n; return malloc (n); }
#define malloc test_malloc
static int Image_WritePNG (const char *p, byte *d, int w, int h, int bpp, qboolean up)
{
	(void)d; (void)bpp; (void)up;
	snprintf (png_path, sizeof(png_path), "%s", p);
	png_w = w; png_h = h; pngs++;
	return 1;
}
'''

DUMP_MAIN = r'''
int main (void)
{
	gltexture_t t;
	int written = 0, failed = 0, stale = 0;

	/* The cache claims 5 is bound while GL really has 9 (256x256). */
	memset (&t, 0, sizeof(t));
	t.texnum = 5; t.width = 64; t.height = 64;
	snprintf (t.identifier, sizeof(t.identifier), "gfx/env/sky_rt");
	currenttexture = 5;
	assert (GL_ImageDumpEntry (&t, "/dump", &written, &failed, &stale));
	assert (written == 1 && stale == 0 && pngs == 1);
	assert (png_w == 64 && png_h == 64);
	assert (!strcmp (png_path, "/dump/gfx_env_sky_rt.png"));

	/* Level 0 smaller than recorded is a legitimate picmip upload. */
	t.texnum = 6;
	assert (GL_ImageDumpEntry (&t, "/dump", &written, &failed, &stale));
	assert (written == 2 && stale == 0 && png_w == 32 && png_h == 32);

	/* A name GL does not know is skipped, not read. */
	t.texnum = 7;
	assert (GL_ImageDumpEntry (&t, "/dump", &written, &failed, &stale));
	assert (written == 2 && stale == 1 && pngs == 2);

	/* notexture after vid_restart: recorded 2x2, name now a 16x16. */
	t.texnum = 22; t.width = 2; t.height = 2;
	snprintf (t.identifier, sizeof(t.identifier), "notexture");
	assert (GL_ImageDumpEntry (&t, "/dump", &written, &failed, &stale));
	assert (written == 2 && stale == 2 && pngs == 2);

	printf ("PASS: imagedump readback is sized and bound from GL, stale names skipped\n");
	return 0;
}
'''


def dump_check():
    text = GLDRAW.read_text()
    entry = function(text, "static qboolean GL_ImageDumpEntry (")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        src = tmp / "dump.c"
        src.write_text(DUMP_PRELUDE + "\n" + entry + "\n" + DUMP_MAIN)
        exe = tmp / "dump"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall", "-Werror",
                        "-Wno-unused-function", str(src), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
        print(out.stdout.strip().splitlines()[-1])


def source_check():
    text = GLDRAW.read_text()
    listing = function(text, "static void GL_ImageList_f (void)\n{")
    # The GLES stub has no glGetTexImage and dumps nothing; the desktop
    # body is the one after #else.
    desktop = text[text.index("#else", text.index("GL_ImageDump_f")):]
    dump = function(desktop, "static void GL_ImageDump_f (void)\n{")
    for name, body in (("imagelist", listing), ("imagedump", dump)):
        if "gltextures" not in body:
            raise AssertionError(f"{name} no longer walks gltextures[]")
        if not re.search(r"TexMgr_NumTextures\s*\(\s*\).*?TexMgr_TextureAt\s*\(", body, re.S):
            raise AssertionError(f"{name} does not walk the TexMgr pool (issue #127)")
    # imagedump changes the binding, the active unit and GL_PACK_ALIGNMENT;
    # it must put all three back and leave GL_Bind's cache truthful.
    for pattern, what in (
            (r"glGetIntegerv_fp\s*\(\s*GL_PACK_ALIGNMENT.*glPixelStorei_fp\s*\(\s*GL_PACK_ALIGNMENT\s*,\s*prev_pack",
             "restore GL_PACK_ALIGNMENT"),
            (r"glGetIntegerv_fp\s*\(\s*GL_TEXTURE_BINDING_2D.*glBindTexture_fp\s*\(\s*GL_TEXTURE_2D\s*,\s*\(GLuint\)\s*prev_tex",
             "restore the previous binding"),
            (r"glGetIntegerv_fp\s*\(\s*GL_ACTIVE_TEXTURE.*glActiveTexture_fp\s*\(\s*\(GLenum\)\s*prev_unit",
             "restore the active texture unit"),
            (r"currenttexture\s*=", "resync currenttexture")):
        if not re.search(pattern, dump, re.S):
            raise AssertionError(f"imagedump does not {what}")
    print("PASS: imagelist and imagedump walk both texture pools; imagedump restores GL state")


def main():
    compiled_check()
    dump_check()
    source_check()


if __name__ == "__main__":
    main()
