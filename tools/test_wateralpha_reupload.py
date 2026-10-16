#!/usr/bin/env python3
"""r_wateralpha reaches EF_TRANSPARENT alias skins when it changes (#124).

An EF_TRANSPARENT alias skin has r_wateralpha baked into its alpha at upload:
GL_Upload8 gives every odd palette index that alpha, and Mod_RestoreIndexAlpha
does the same for a replacement skin rebuilt from the indices.  World water
reads the cvar per frame, so a change used to reach the water and never the
models, until something reloaded them.

Four parts, all compiled from the real source with stubs around it:
  1. GL_WaterAlphaByte clamps to the [0.1, 1] the draw paths clamp to.  The old
     `(int)(255 * v) & 0xff` wrapped 1.5 to 126.
  2. GL_LoadTextureEx keys an indexed TEX_TRANSPARENT texture on the baked
     byte: same byte is a cache hit, a new byte is a fresh upload, and nothing
     else (opaque, RGBA, other flags) re-uploads because the cvar moved.
  3. Mod_WaterAlphaChanged / Mod_WaterAlphaFrame re-upload exactly the loaded
     EF_TRANSPARENT alias models, once per distinct byte, nothing before host
     init, and once per slider drag rather than once per step.
  4. Source: the callback is registered, and both writers read the helper
     rather than the cvar, so they cannot drift from the cache test.
"""

from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GLDRAW = ROOT / "engine/h2shared/gl_draw.c"
GLMODEL = ROOT / "engine/h2shared/gl_model.c"
GLMODELH = ROOT / "engine/h2shared/gl_model.h"
GLQUAKE = ROOT / "engine/h2shared/glquake.h"
GLRMISC = ROOT / "engine/hexen2/gl_rmisc.c"


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def function(text, signature):
    """The definition of signature: the first occurrence whose next `{` comes
    before any `;`, so a forward declaration is skipped."""
    start = text.index(signature)
    while ";" in text[start:text.index("{", start)]:
        start = text.index(signature, start + 1)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def define(text, name):
    m = re.search(r"#define\s+%s\s+(\([^)]*\)|\S+)" % name, text)
    if not m:
        raise AssertionError(f"cannot find #define {name}")
    return m.group(1)


draw = GLDRAW.read_text()
model = GLMODEL.read_text()
glq = GLQUAKE.read_text()
mh = GLMODELH.read_text()

DEFS = {n: define(glq, n) for n in
        ("TEX_MIPMAP", "TEX_ALPHA", "TEX_RGBA", "TEX_TRANSPARENT", "TEX_HOLEY",
         "TEX_FENCE")}
DEFS["EF_TRANSPARENT"] = define(mh, "EF_TRANSPARENT")
DEFS["NL_PRESENT"] = define(mh, "NL_PRESENT")
DEFS["NL_NEEDS_LOADED"] = define(mh, "NL_NEEDS_LOADED")

PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char byte;
typedef unsigned int GLuint;
typedef int qboolean;
enum { false, true };
#define MAX_QPATH 64
#define MAX_GLTEXTURES 64
#define GL_UNUSED_TEXTURE (~(GLuint)0)
#define __thisfunc__ __func__
''' + "".join(f"#define {k} {v}\n" for k, v in DEFS.items()) + r'''
typedef struct cvar_s { float value; int integer; } cvar_t;
cvar_t r_wateralpha = { 1.0f, 1 };
'''

HELPER = function(draw, "byte GL_WaterAlphaByte (void)")

# --------------------------------------------------------------------------
# Parts 1 and 2: the helper, and the cache test in GL_LoadTextureEx.
LOADEX = function(draw, "static GLuint GL_LoadTextureEx (")
LOADTEX = function(draw, "GLuint GL_LoadTexture (const char *identifier")

PART12 = PRELUDE + r'''
typedef struct { GLuint texnum; char identifier[MAX_QPATH]; int width, height;
		 int flags; unsigned short crc; byte wateralpha; } gltexture_t;
typedef struct { byte *blocks; int nummips; size_t mipofs[4], mipsize[4]; } imgreplace_t;
typedef struct { int dummy; } hashindex_t;
gltexture_t gltextures[MAX_GLTEXTURES];
int numgltextures;
hashindex_t hash_gltextures;
enum { ca_dedicated = 1, ca_connected };
struct { int state; } cls = { ca_connected };
GLuint currenttexture;
qboolean gl_stale_gltextures;
int host_framecount;
cvar_t gl_log_texgen;
static int uploads8, uploads32;
static byte last_baked;

static int Hash_GenerateKeyString (hashindex_t *h, const char *s, qboolean c)
{ (void)h; (void)s; (void)c; return 0; }
static int Hash_First (hashindex_t *h, int key)
{ (void)h; (void)key; for (int i = 0; i < numgltextures; i++) if (gltextures[i].identifier[0]) return i; return -1; }
static int Hash_Next (hashindex_t *h, int i)
{ (void)h; for (i++; i < numgltextures; i++) if (gltextures[i].identifier[0]) return i; return -1; }
static void Hash_Remove (hashindex_t *h, int key, int i) { (void)h; (void)key; (void)i; }
static void Hash_Add (hashindex_t *h, int key, int i) { (void)h; (void)key; (void)i; }
static unsigned short CRC_Block (const byte *d, int n)
{ unsigned short c = 0; while (n--) c = (unsigned short)(c * 31 + *d++); return c; }
static int GL_ClaimStaleTexture (void) { return -1; }
static void Sys_Error (const char *f, ...) { (void)f; abort (); }
static void q_strlcpy (char *d, const char *s, size_t n) { snprintf (d, n, "%s", s); }
#define Con_Printf(...) ((void)0)
#define Con_DPrintf(...) ((void)0)
static GLuint next_name = 1;
static void glGenTextures_fp (int n, GLuint *t) { (void)n; *t = next_name++; }
static void glDeleteTextures_fp (int n, GLuint *t) { (void)n; (void)t; }
static void GL_Bind (GLuint t) { (void)t; }
static void GL_UploadCompressed (const imgreplace_t *r, unsigned f, gltexture_t *g)
{ (void)r; (void)f; (void)g; }
static void GL_Upload32 (unsigned int *d, gltexture_t *g) { (void)d; (void)g; uploads32++; }
/* The real one reads glt->wateralpha in its `p & 1` arm (part 4 checks that);
 * record what it would have baked. */
static void GL_Upload8 (byte *d, gltexture_t *g) { (void)d; uploads8++; last_baked = g->wateralpha; }

@HELPER@
@LOADEX@
@LOADTEX@

static void set (float v) { r_wateralpha.value = v; r_wateralpha.integer = (int)v; }

int main (void)
{
	static byte skin[16 * 16], opaque[8 * 8], rgba[4 * 4 * 4];
	GLuint a, b;
	int i;

	for (i = 0; i < (int)sizeof skin; i++) skin[i] = (byte)i;
	for (i = 0; i < (int)sizeof opaque; i++) opaque[i] = (byte)(i * 3);

	/* Part 1: the clamp. */
	set (0.5f);  assert (GL_WaterAlphaByte () == 127);
	set (0.33f); assert (GL_WaterAlphaByte () == 84);
	set (1.0f);  assert (GL_WaterAlphaByte () == 255);
	set (1.5f);  assert (GL_WaterAlphaByte () == 255);	/* was 126 */
	set (0.0f);  assert (GL_WaterAlphaByte () == 25);
	set (-1.0f); assert (GL_WaterAlphaByte () == 25);
	set (0.05f); assert (GL_WaterAlphaByte () == 25);

	/* Part 2: an indexed EF_TRANSPARENT skin. */
	set (0.5f);
	a = GL_LoadTexture ("models/x_0", skin, 16, 16, TEX_MIPMAP | TEX_TRANSPARENT);
	assert (uploads8 == 1 && last_baked == 127);
	b = GL_LoadTexture ("models/x_0", skin, 16, 16, TEX_MIPMAP | TEX_TRANSPARENT);
	assert (b == a && uploads8 == 1);		/* same byte: cache hit */

	set (1.0f);
	b = GL_LoadTexture ("models/x_0", skin, 16, 16, TEX_MIPMAP | TEX_TRANSPARENT);
	assert (b != a && uploads8 == 2 && last_baked == 255);	/* #124 */

	set (1.2f);	/* clamps to the byte already uploaded */
	a = GL_LoadTexture ("models/x_0", skin, 16, 16, TEX_MIPMAP | TEX_TRANSPARENT);
	assert (a == b && uploads8 == 2);

	/* An opaque skin does not depend on the cvar and must not churn. */
	set (0.5f);
	a = GL_LoadTexture ("models/y_0", opaque, 8, 8, TEX_MIPMAP);
	set (0.9f);
	b = GL_LoadTexture ("models/y_0", opaque, 8, 8, TEX_MIPMAP);
	assert (a == b && uploads8 == 3);

	/* An RGBA texture carries its own alpha; the crc decides, not the cvar. */
	set (0.5f);
	a = GL_LoadTexture ("models/z_0", rgba, 4, 4, TEX_RGBA | TEX_TRANSPARENT);
	set (0.9f);
	b = GL_LoadTexture ("models/z_0", rgba, 4, 4, TEX_RGBA | TEX_TRANSPARENT);
	assert (a == b && uploads32 == 1);
	rgba[3] = 7;	/* what a rebuilt replacement looks like after the change */
	b = GL_LoadTexture ("models/z_0", rgba, 4, 4, TEX_RGBA | TEX_TRANSPARENT);
	assert (a != b && uploads32 == 2);

	puts ("ok   GL_WaterAlphaByte clamp and GL_LoadTextureEx cache key");
	return 0;
}
'''.replace("@HELPER@", HELPER).replace("@LOADEX@", LOADEX).replace("@LOADTEX@", LOADTEX)

# --------------------------------------------------------------------------
# Part 3: the callback.
# The settle constant, the pending timestamp, the callback and the frame hook,
# as one span of the source.
_start = model.index("#define WATERALPHA_SETTLE")
CALLBACK = model[_start:model.index(function(model, "void Mod_WaterAlphaFrame (void)"))] + \
    function(model, "void Mod_WaterAlphaFrame (void)")

PART3 = PRELUDE + r'''
typedef enum { mod_brush, mod_sprite, mod_alias } modtype_t;
typedef struct { void *data; } cache_user_t;
typedef struct qmodel_s { char name[MAX_QPATH]; modtype_t type; int flags;
			  int needload; qboolean cache_is_hunk; cache_user_t cache;
			  int reuploads; } qmodel_t;
#define MAX_MOD_KNOWN 16
static qmodel_t mod_known[MAX_MOD_KNOWN];
static int mod_numknown;
qboolean host_initialized;
double realtime = 100.0;
static void *Cache_Check (cache_user_t *c) { return c->data; }
static void Mod_ReuploadAliasSkins (qmodel_t *m) { m->reuploads++; }
#define Con_DPrintf(...) ((void)0)

@HELPER@
@CALLBACK@

static qmodel_t *add (const char *n, modtype_t t, int flags, int needload,
		      qboolean hunk, int cached)
{
	static int live;
	qmodel_t *m = &mod_known[mod_numknown++];
	snprintf (m->name, sizeof m->name, "%s", n);
	m->type = t; m->flags = flags; m->needload = needload;
	m->cache_is_hunk = hunk; m->cache.data = cached ? &live : NULL;
	return m;
}

/* A set, then frames until well past the settle time. */
static void set (float v)
{
	r_wateralpha.value = v;
	Mod_WaterAlphaChanged (&r_wateralpha);
	Mod_WaterAlphaFrame ();
	realtime += 1.0;
	Mod_WaterAlphaFrame ();
}

int main (void)
{
	qmodel_t *hit  = add ("models/stclrbm.mdl", mod_alias, EF_TRANSPARENT, NL_PRESENT, false, 1);
	qmodel_t *hunk = add ("models/md5.mdl", mod_alias, EF_TRANSPARENT, NL_PRESENT, true, 1);
	qmodel_t *opq  = add ("models/paladin.mdl", mod_alias, 0, NL_PRESENT, false, 1);
	qmodel_t *spr  = add ("sprites/s.spr", mod_sprite, EF_TRANSPARENT, NL_PRESENT, false, 1);
	qmodel_t *gone = add ("models/evicted.mdl", mod_alias, EF_TRANSPARENT, NL_PRESENT, false, 0);
	qmodel_t *nl   = add ("models/unloaded.mdl", mod_alias, EF_TRANSPARENT, NL_NEEDS_LOADED, false, 1);
	qmodel_t *anon = add ("", mod_alias, EF_TRANSPARENT, NL_PRESENT, false, 1);

	set (0.5f);	/* config.cfg, before host init: nothing to do */
	assert (hit->reuploads == 0);

	host_initialized = true;
	set (0.5f);
	assert (hit->reuploads == 1);
	/* An MD5/MD3 substitute: the .mdl skins would clobber its material. */
	assert (hunk->reuploads == 0);
	assert (opq->reuploads == 0 && spr->reuploads == 0);
	assert (gone->reuploads == 0 && nl->reuploads == 0 && anon->reuploads == 0);

	set (0.5f);	/* same byte: no work */
	set (0.501f);	/* still 127 */
	assert (hit->reuploads == 1);

	set (1.0f);
	assert (hit->reuploads == 2 && hunk->reuploads == 0);
	set (3.0f);	/* clamps to 255, already applied */
	assert (hit->reuploads == 2);

	/* A slider drag: a set every 50 ms with a frame between.  Nothing goes
	 * up while the value is moving; one pass goes up once it holds still. */
	for (int step = 0; step < 12; step++)
	{
		r_wateralpha.value = 0.95f - 0.05f * step;
		Mod_WaterAlphaChanged (&r_wateralpha);
		realtime += 0.05;
		Mod_WaterAlphaFrame ();
	}
	assert (hit->reuploads == 2);
	realtime += 0.1;
	Mod_WaterAlphaFrame ();
	assert (hit->reuploads == 2);		/* 0.15 s after the last step */
	realtime += 0.2;
	Mod_WaterAlphaFrame ();
	assert (hit->reuploads == 3);		/* settled */
	Mod_WaterAlphaFrame ();
	assert (hit->reuploads == 3);		/* and only once */

	/* A drag that ends where it began uploads nothing. */
	r_wateralpha.value = 0.5f; Mod_WaterAlphaChanged (&r_wateralpha);
	r_wateralpha.value = 0.95f - 0.05f * 11; Mod_WaterAlphaChanged (&r_wateralpha);
	realtime += 1.0; Mod_WaterAlphaFrame ();
	assert (hit->reuploads == 3);

	puts ("ok   Mod_WaterAlphaChanged picks the loaded EF_TRANSPARENT alias models");
	return 0;
}
'''.replace("@HELPER@", HELPER).replace("@CALLBACK@", CALLBACK)


def run(src, name, tmp):
    c = Path(tmp) / f"{name}.c"
    exe = Path(tmp) / name
    c.write_text(src)
    cc = os.environ.get("CC", "cc")
    r = subprocess.run([cc, "-std=gnu99", "-O1", "-Wall", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable",
                        "-o", str(exe), str(c)], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"FAIL compiling {name}:\n{r.stderr}")
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode:
        sys.exit(f"FAIL {name}:\n{r.stderr}")


def source_checks():
    errors = []
    d = strip_comments(draw)
    m = strip_comments(model)
    upload8 = function(d, "static void GL_Upload8 (byte *data, gltexture_t *glt)")
    if not re.search(r"else\s+if\s*\(\s*p\s*&\s*1\s*\)\s*\{\s*p\s*=\s*glt->wateralpha\s*;", upload8):
        errors.append("GL_Upload8's `p & 1` arm must bake glt->wateralpha, the byte the cache keyed on")
    if re.search(r"r_wateralpha\.value", upload8):
        errors.append("GL_Upload8 reads r_wateralpha directly again")
    restore = function(m, "static qboolean Mod_RestoreIndexAlpha (")
    if "GL_WaterAlphaByte ()" not in restore or "r_wateralpha" in restore:
        errors.append("Mod_RestoreIndexAlpha must take the byte from GL_WaterAlphaByte")
    rmain = strip_comments((ROOT / "engine/hexen2/gl_rmain.c").read_text())
    if "Mod_WaterAlphaFrame ();" not in function(rmain, "void R_RenderView (void)"):
        errors.append("R_RenderView no longer calls Mod_WaterAlphaFrame, so a change never applies")
    rmisc = strip_comments(GLRMISC.read_text())
    if not re.search(r"Cvar_SetCallback\s*\(\s*&r_wateralpha\s*,\s*Mod_WaterAlphaChanged\s*\)", rmisc):
        errors.append("gl_rmisc.c no longer registers Mod_WaterAlphaChanged on r_wateralpha")
    for path in sorted((ROOT / "engine").rglob("gl_*.c")):
        text = strip_comments(path.read_text(errors="replace"))
        if re.search(r"255(\.0f?)?\s*\*\s*r_wateralpha\.value", text):
            errors.append(f"{path.relative_to(ROOT)} bakes r_wateralpha itself; use GL_WaterAlphaByte")
    if errors:
        sys.exit("FAIL source checks:\n  " + "\n  ".join(errors))
    print("ok   writers share GL_WaterAlphaByte; callback registered")


with tempfile.TemporaryDirectory() as tmp:
    run(PART12, "part12", tmp)
    run(PART3, "part3", tmp)
source_checks()
print("PASS r_wateralpha reaches EF_TRANSPARENT alias skins (#124)")
