#!/usr/bin/env python3
"""imagelist/imagedump must see gl_texmgr.c's texture pool (issue #127).

Skyboxes and the scrolling sky are uploaded through TexMgr_LoadImage into
gl_texmgr.c's own managed_textures[] pool, not gltextures[].  The two debug
commands used to walk gltextures[] only, so `imagelist gfx/env` printed nothing
with a skybox loaded, and imagedump wrote everything but the sky faces.

Two halves:
  1. Compile the real gl_texmgr.c against a stub quakedef.h and exercise
     TexMgr_TextureAt: placeholders, loaded faces, flag translation, a freed
     slot disappearing, and slot recycling.
  2. Source-level: GL_ImageList_f and the desktop GL_ImageDump_f both walk
     the TexMgr pool as well as gltextures[].
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
	TexMgr_LoadImage (NULL, "lowsky", 8, 8, SRC_RGBA, (byte *)face, "", 0,
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
    print("PASS: imagelist and imagedump walk both texture pools")


def main():
    compiled_check()
    source_check()


if __name__ == "__main__":
    main()
