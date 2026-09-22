#!/usr/bin/env python3
"""Data-free regressions for the vanilla liquid/brush alpha contract (#250).

Compiles the production R_LiquidAlpha, the GL brush adapter and the 8bpp
adapter against the shared classifier, then checks the rules vanilla actually
has:

  * r_wateralpha defaults to 0.33, not 1 (gl_rmain.c), and the Water Alpha row
    can still reach it (menu.c).
  * A regular DRF_TRANSLUCENT brush takes r_wateralpha -- vanilla reads the
    same cvar for the translucent turb allowlist and for every DRF_TRANSLUCENT
    brush entity, so Hexen II's teleporter shells (func_illusionary with
    spawnflags 1) are translucent out of the box and opaque at r_wateralpha 1.
  * Only *lowlight and *rtex078 are translucent liquids; *rtex346 (the Castle
    gold pool) and any other non-allowlisted turb texture stay opaque, and
    *tele in a water volume is opaque too -- it is not on the allowlist.
  * An explicit entity alpha still wins on every surface kind.
  * The 8bpp renderer keeps its own fixed palette translucency: it consumes
    only the visible/translucent bits, so its DRF_TRANSLUCENT brushes stay
    translucent at any r_wateralpha, as vanilla's software path did.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

from test_multiplayer_transparency import ROOT, function

PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "brush_render_state.h"
#include "bspfile.h"
#define DRF_TRANSLUCENT 128
/* engine/h2shared/gl_model.h — bspfile.h does not carry the render flags. */
#define SURF_DRAWTURB 0x10
#define SURF_TRANSLUCENT 0x80
#define ENTALPHA_DEFAULT 0
#define ENTALPHA_DECODE(a) (((a)==ENTALPHA_DEFAULT)?1.0f:((float)(a)-1)/254.0f)
#define q_strncasecmp strncasecmp
typedef struct { float value; } cvar_t;
static cvar_t r_wateralpha, r_lavaalpha, r_slimealpha, r_telealpha, r_turbalpha;
typedef struct { char name[16]; int content_class; qboolean translucent_turb; } texture_t;
typedef struct { int drawflags; uint8_t alpha; } entity_t;
typedef struct { texture_t *texture; } mtexinfo_t;
typedef struct { int flags; mtexinfo_t *texinfo; } msurface_t;
'''

TEST = r'''
/* The GL path hands the regular-surface default in from r_wateralpha.  Assert
 * the classifier passes it through untouched: vanilla's coupling is the
 * caller's, and this is the rule that made demo1's portals opaque at 1. */
static void check_portal(float wateralpha)
{
    entity_t portal = {DRF_TRANSLUCENT, ENTALPHA_DEFAULT};
    brush_render_state_t state = R_BrushEntityRenderState(&portal, BRUSH_SURFACE_REGULAR,
        (portal.drawflags & DRF_TRANSLUCENT) ? wateralpha : 1.0f);
    assert(state.visible);
    /* Vanilla reads r_wateralpha here: 1.0 is opaque, anything below blends. */
    assert(state.alpha == wateralpha);
    /* The drawflag keeps it in the blend pass, exactly as vanilla's
     * glEnable(GL_BLEND) does; at alpha 1.0 that pass is a no-op. */
    assert(state.translucent);
    /* Without the flag the same expression yields 1.0 and stays opaque,
     * whatever the water slider says. */
    portal.drawflags = 0;
    state = R_BrushEntityRenderState(&portal, BRUSH_SURFACE_REGULAR,
        (portal.drawflags & DRF_TRANSLUCENT) ? wateralpha : 1.0f);
    assert(state.visible && !state.translucent && state.alpha == 1.0f);
}

static void check_liquid(const char *name, int contents, int translucent, float alpha)
{
    texture_t t = {{0}, contents, translucent};
    strcpy(t.name, name);
    assert(R_LiquidAlpha(&t) == alpha);
}

int main(void)
{
    entity_t portal = {DRF_TRANSLUCENT, ENTALPHA_DEFAULT};
    brush_render_state_t state;
    const float water_values[] = {1.0f, 0.5f, 0.33f};
    unsigned i;
    texture_t regular = {"rtex412", 0, 0};
    mtexinfo_t texinfo = {&regular};
    msurface_t surface = {0, &texinfo};

    r_lavaalpha.value = 0.8f;
    r_slimealpha.value = 0.6f;
    r_telealpha.value = 0.3f;
    r_turbalpha.value = 1.0f;

    for (i = 0; i < sizeof(water_values)/sizeof(water_values[0]); i++)
    {
        r_wateralpha.value = water_values[i];
        check_portal(water_values[i]);

        /* Vanilla allowlist: only these two turb textures are translucent,
         * and both read r_wateralpha. */
        check_liquid("*lowlight", CONTENTS_WATER, 1, r_wateralpha.value);
        check_liquid("*rtex078", CONTENTS_WATER, 1, r_wateralpha.value);
        /* Not allowlisted -> opaque even in a water volume.  This is the
         * Castle gold pool, and it is also why *tele is not special here:
         * vanilla has no teleport contents and no r_telealpha. */
        check_liquid("*rtex346", CONTENTS_WATER, 0, 1.0f);
        check_liquid("*teleport", CONTENTS_WATER, 0, 1.0f);
        check_liquid("*telewater", CONTENTS_WATER, 1, r_wateralpha.value);
        /* Lava and slime contents stay authoritative for a water-named tex. */
        check_liquid("*water", CONTENTS_LAVA, 1, 0.8f);
        check_liquid("*lava", CONTENTS_WATER, 0, 1.0f);

        /* The 8bpp renderer has no continuous alpha: it rebuilds
         * SURF_TRANSLUCENT from the classified bits, so a DRF_TRANSLUCENT
         * brush takes the palette translucent pass at any r_wateralpha. */
        surface.flags = 0;
        state = R_SoftwareBrushRenderState(&portal, &surface);
        assert(state.visible && state.translucent);
        state = R_SoftwareBrushRenderState(&portal, NULL);
        assert(state.visible && state.translucent);
        /* A liquid's own bit decides there, and a non-allowlisted liquid
         * stays opaque. */
        surface.flags = SURF_DRAWTURB;
        state = R_SoftwareBrushRenderState(&portal, &surface);
        assert(state.visible && !state.translucent && state.alpha == 1.0f);
        surface.flags |= SURF_TRANSLUCENT;
        state = R_SoftwareBrushRenderState(&portal, &surface);
        assert(state.visible && state.translucent);
    }

    /* Name fallback still runs where content_class is 0 (brush submodels and
     * illusionary turb brushes in empty/solid space). */
    r_wateralpha.value = 0.33f;
    check_liquid("*lowlight", 0, 1, 1.0f);
    check_liquid("*coldwater", 0, 1, r_wateralpha.value);
    check_liquid("*teleport", 0, 0, 0.3f);
    check_liquid("*tele", 0, 0, 0.3f);
    r_telealpha.value = 0.0f;
    check_liquid("*teleport", 0, 0, 0.7f);
    r_telealpha.value = -1.0f;
    check_liquid("*teleport", 0, 0, 0.7f);
    r_telealpha.value = 0.01f;
    check_liquid("*teleport", 0, 0, 0.1f);
    r_telealpha.value = 2.0f;
    check_liquid("*teleport", 0, 0, 1.0f);

    /* Explicit entity alpha wins on every surface kind, and survives the
     * water slider. */
    for (i = BRUSH_SURFACE_REGULAR; i <= BRUSH_SURFACE_CUTOUT; i++)
    {
        portal.alpha = 1;
        state = R_BrushEntityRenderState(&portal, i, 0.33f);
        assert(state.has_explicit_alpha && !state.visible && state.alpha == 0.0f);
        portal.alpha = 128;
        state = R_BrushEntityRenderState(&portal, i, 0.33f);
        assert(state.visible && state.translucent && state.alpha == 0.5f);
        portal.alpha = 255;
        state = R_BrushEntityRenderState(&portal, i, 0.33f);
        assert(state.visible && state.alpha == 1.0f);
    }

    /* Cutouts keep their hole semantics and stay opaque by default. */
    portal.alpha = ENTALPHA_DEFAULT;
    strcpy(regular.name, "{fence");
    surface.flags = 0;
    state = R_SoftwareBrushRenderState(&portal, &surface);
    assert(state.cutout && !state.translucent && state.alpha == 1.0f);
    portal.alpha = 128;
    state = R_SoftwareBrushRenderState(&portal, &surface);
    assert(state.cutout && state.translucent && state.alpha == 0.5f);

    puts("PASS: vanilla water/brush alpha contract, explicit overrides, liquids and palette path");
    return 0;
}
'''


def main():
    gl_rmain = (ROOT / "engine/hexen2/gl_rmain.c").read_text()
    # Vanilla's default.  A default of 1 is what made the portals opaque out
    # of the box; this is the whole of #250's fix.
    assert re.search(r'cvar_t\s+r_wateralpha\s*=\s*\{"r_wateralpha",\s*"0\.33",\s*CVAR_ARCHIVE\};', gl_rmain), \
        "r_wateralpha must default to vanilla's 0.33"

    menu = (ROOT / "engine/hexen2/menu.c").read_text()
    # The row must be able to represent the default, or the slider would snap
    # 0.33 up to its old 0.7 floor on the first keypress.
    case = menu[menu.index("case REND_WATERALPHA:"):]
    case = case[:case.index("case REND_WATERWARP:")]
    assert "0.1f" in case and "0.7f" not in case, "Water Alpha row must reach 0.1"
    assert re.search(r'\{ REND_WATERALPHA,\s*"r_wateralpha",\s*0\.0f,\s*1\.0f,\s*0\.1f,\s*1\.0f,',
                     menu), "Water Alpha slider range must include 0.33"

    surf = (ROOT / "engine/hexen2/gl_rsurf.c").read_text()
    poly = function("engine/hexen2/gl_rsurf.c", "void R_RenderBrushPoly")
    # Vanilla coupling: the regular default comes from r_wateralpha, and the
    # adapter classifies it rather than deciding after the fact.
    assert "default_alpha = (e->drawflags & DRF_TRANSLUCENT) ?" in poly
    assert "r_wateralpha.value : 1.0f;" in poly
    assert "R_BrushEntityRenderState(e, surface_kind, default_alpha)" in poly
    # No *tele carve-out ahead of the content switch: vanilla's allowlist
    # decides, and *tele is not on it.
    liquid = function("engine/hexen2/gl_rsurf.c", "static float R_LiquidAlpha (const texture_t *t)\n{")
    assert '"*tele"' not in liquid, "no *tele exception may precede the content switch"
    assert 'q_strncasecmp(n, "tele", 4)' in liquid, "the unclassified fallback keeps r_telealpha"

    main_source = (ROOT / "engine/hexen2/gl_rmain.c").read_text()
    assert main_source.count("R_BrushEntityRenderState(e,") >= 3
    soft = (ROOT / "engine/hexen2/r_main.c").read_text()
    start = soft.index("brush_render_state_t R_SoftwareBrushRenderState")
    # First unindented closing brace ends this top-level function; unlike a
    # naive brace counter, this isn't confused by the '{' texture-name literal.
    soft_adapter = soft[start:soft.index("\n}", start) + 2]
    assert "return R_ClassifyBrushRenderState(&input);" in soft_adapter

    source = PRELUDE + function(
        "engine/hexen2/gl_rsurf.c", "static float R_LiquidAlpha (const texture_t *t)\n{"
    ) + function(
        "engine/hexen2/gl_rsurf.c", "brush_render_state_t R_BrushEntityRenderState"
    ) + soft_adapter + TEST
    with tempfile.TemporaryDirectory(prefix="brush-alpha-") as tmp:
        cfile = Path(tmp) / "brush_alpha.c"
        binary = Path(tmp) / "brush_alpha"
        cfile.write_text(source)
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-misleading-indentation", "-I" + str(ROOT / "engine/h2shared"),
            "-I" + str(ROOT / "common"), str(cfile), "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
