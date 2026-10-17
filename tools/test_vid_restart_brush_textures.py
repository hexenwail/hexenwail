#!/usr/bin/env python3
"""vid_restart re-uploads every loaded brush model's textures (GitHub #430).

Two bugs, one cause -- the reload path was a second, thinner copy of the load:

  1. Mod_ReloadTextures reloaded only cl.worldmodel.  The server loads the
     map, and uploads its textures, before the client parses serverdata; a
     vid_restart in that window found cl.worldmodel NULL, skipped the world,
     and the cached model kept names from the dead context.  Mod_ForName then
     handed it to the client as loaded, and the level drew with whatever the
     new context had recycled those names to.
  2. The reload uploaded only the BSP's embedded miptex, so an installed
     replacement (an HD pack) reverted to the stock texture after any
     vid_restart, and the _norm / _gloss material maps kept dead names.

Held here: the selection loop, compiled out of gl_model.c and run against
stubs (world reloaded with cl.worldmodel NULL; inline "*N" submodels, which
share the world's texture_t array, skipped; standalone .bsp models reloaded;
unloaded slots and other model types skipped); and that load and reload go
through the one Mod_UploadBrushTexture, which clears the glow and material
names before deciding what to upload.

Measured on demo1, NVIDIA offscreen: vid_restart issued with the map command
drew red/black/white blocks before and the correct level after; with an
all-magenta replacement set, master reverted to the stock textures across a
restart and the fix keeps the replacements.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "engine/h2shared/gl_model.c").read_text()


def function(signature):
    """The definition after `signature`, braces matched outside comments,
    strings and character literals (the fence checks compare against '{')."""
    if signature + "\n{" not in SRC:
        sys.exit(f"FAIL: gl_model.c has no definition `{signature}`")
    start = SRC.index(signature + "\n{")
    i, depth = SRC.index("{", start) + 1, 1
    while depth:
        if SRC.startswith("/*", i):
            i = SRC.index("*/", i) + 2
            continue
        if SRC.startswith("//", i):
            i = SRC.index("\n", i)
            continue
        ch = SRC[i]
        if ch in "\"'":
            i += 1
            while SRC[i] != ch:
                i += 2 if SRC[i] == "\\" else 1
        depth += (ch == "{") - (ch == "}")
        i += 1
    return SRC[start:i]


errors = []
upload = function("static void Mod_UploadBrushTexture (texture_t *tx, const char *modelname)")
for need in ("tx->gl_fb_texturenum = 0;", "tx->gl_norm_texturenum = 0;", "tx->gl_gloss_texturenum = 0;",
             "R_InitSky (tx);", "IMG_LoadReplacement (tx->name, modelname, true, &rep)",
             "IMG_LoadReplacementNormal (tx->name, modelname, &mat)",
             "IMG_LoadReplacementGloss (tx->name, modelname, &mat)"):
    if need not in upload:
        errors.append(f"Mod_UploadBrushTexture lacks {need}")
if upload.index("tx->gl_gloss_texturenum = 0;") > upload.index("R_InitSky (tx);"):
    errors.append("Mod_UploadBrushTexture must clear the names before any upload")
if "Mod_UploadBrushTexture (tx, loadmodel->name);" not in function("static void Mod_LoadTextures (lump_t *l)"):
    errors.append("Mod_LoadTextures must upload through Mod_UploadBrushTexture")
reload = function("void Mod_ReloadTextures (void)")
if "cl.worldmodel" in re.sub(r"/\*.*?\*/", "", reload, flags=re.S):
    errors.append("Mod_ReloadTextures must not gate brush textures on cl.worldmodel")
if "GL_LoadTexture" in function("static void Mod_ReloadBrushTextures (qmodel_t *mod)"):
    errors.append("Mod_ReloadBrushTextures must not upload on its own; use Mod_UploadBrushTexture")

selection = reload[:reload.index("\t// Reload alias models and sprites")] + "}\n"
harness = r'''
#include <stdio.h>
#include <string.h>
#define NL_PRESENT 0
#define NL_NEEDS_LOADED 1
#define NL_UNREFERENCED 2
enum { mod_brush, mod_sprite, mod_alias };
typedef struct { char name[16]; } texture_t;
typedef struct { char name[64]; int type, needload, numtextures; texture_t **textures; } qmodel_t;
static qmodel_t mod_known[16];
static int mod_numknown;
static char log_[1024];
static void Mod_UploadBrushTexture (texture_t *tx, const char *modelname)
{ strcat (log_, modelname); strcat (log_, ":"); strcat (log_, tx->name); strcat (log_, " "); }
''' + function("static void Mod_ReloadBrushTextures (qmodel_t *mod)") + "\n" + selection + r'''
static texture_t w0 = {"rtex001"}, w1 = {"sky1"}, b0 = {"crate"};
static texture_t *world_tex[] = { &w0, NULL, &w1 }, *bsp_tex[] = { &b0 };
static void add (const char *n, int type, int nl, texture_t **t, int nt)
{ qmodel_t *m = &mod_known[mod_numknown++]; strcpy (m->name, n); m->type = type; m->needload = nl; m->textures = t; m->numtextures = nt; }
int main (void)
{
	/* cl.worldmodel is not even declared: the world is reached through
	 * mod_known whether or not the client has parsed serverdata yet. */
	add ("maps/demo1.bsp", mod_brush, NL_PRESENT, world_tex, 3);
	add ("*1", mod_brush, NL_PRESENT, world_tex, 3);
	add ("models/crate.bsp", mod_brush, NL_PRESENT, bsp_tex, 1);
	add ("maps/old.bsp", mod_brush, NL_UNREFERENCED, bsp_tex, 1);
	add ("maps/next.bsp", mod_brush, NL_NEEDS_LOADED, bsp_tex, 1);
	add ("models/paladin.mdl", mod_alias, NL_PRESENT, bsp_tex, 1);
	add ("", mod_brush, NL_PRESENT, bsp_tex, 1);
	Mod_ReloadTextures ();
	if (strcmp (log_, "maps/demo1.bsp:rtex001 maps/demo1.bsp:sky1 models/crate.bsp:crate ")) {
		printf ("got: %s\n", log_);
		return 1;
	}
	return 0;
}
'''
if not errors:
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = os.path.join(tmp, "t.c"), os.path.join(tmp, "t")
        Path(c).write_text(harness)
        r = subprocess.run(["cc", "-std=gnu99", "-Wall", "-Werror", "-o", exe, c], capture_output=True, text=True)
        if r.returncode:
            errors.append("harness did not compile:\n" + r.stderr)
        else:
            r = subprocess.run([exe], capture_output=True, text=True)
            if r.returncode:
                errors.append("Mod_ReloadTextures picked the wrong models: " + r.stdout.strip())

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS vid_restart re-uploads every loaded brush model through the load path (#430)")
