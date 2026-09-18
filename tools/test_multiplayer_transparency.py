#!/usr/bin/env python3
"""Data-free regression test for HexenWorld entity-alpha wire ordering.

Extracts the production client delta parser, stubs its message reader, and feeds
it a server-format update containing U_ALPHA and U_SOUND together.  This catches
field-order drift without proprietary game data or a live server.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    text = (ROOT / path).read_text()
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "brush_render_state.h"
#define true 1
#define false 0
#define DRF_TRANSLUCENT 128
#define ENTALPHA_DEFAULT 0
#define ENTALPHA_OPAQUE(a) ((uint8_t)((a)-1) >= 254)
#define ENTALPHA_DECODE(a) (((a)==ENTALPHA_DEFAULT)?1.0f:((float)(a)-1)/254.0f)
typedef struct { int drawflags; uint8_t alpha; } entity_t;
typedef struct {
    int active, modelindex, frame, colormap, skinnum, drawflags, effects;
    float origin[3], angles[3];
    int scale, abslight, alpha;
} hwcl_entity_state_t;
static const uint8_t *wire;
static size_t wire_len, wire_pos;
static int MSG_ReadByte(void) {
    assert(wire_pos < wire_len);
    return wire[wire_pos++];
}
static int MSG_ReadShort(void) {
    int lo = MSG_ReadByte(), hi = MSG_ReadByte();
    return (int16_t)(lo | (hi << 8));
}
static int MSG_ReadLong(void) {
    int a = MSG_ReadShort(), b = MSG_ReadShort();
    return a | (b << 16);
}
static float MSG_ReadCoord(void) { return (float)MSG_ReadShort() / 8.0f; }
static float MSG_ReadAngle(void) { return (float)MSG_ReadByte() * (360.0f / 256.0f); }
'''

TEST = r'''
int main(void) {
    hwcl_entity_state_t from, to;
    /* SV_WriteDelta order after the entity short: extension bytes, alpha,
     * then the little-endian weapon sound. */
    const uint8_t update[] = {0x80, 0x12, 0x80, 0x34, 0x12};
    const int initial_bits = (1 << 15) | (1 << 7) | 7;

    memset(&from, 0, sizeof(from));
    from.alpha = 255;
    wire = update; wire_len = sizeof(update); wire_pos = 0;
    HWCL_ParseEntityDelta(&from, &to, initial_bits);
    assert(to.alpha == 0x80);
    assert(wire_pos == wire_len);

    entity_t brush = {0, ENTALPHA_DEFAULT};
    brush_render_state_t state =
        R_BrushEntityRenderState(&brush, BRUSH_SURFACE_REGULAR, 1.0f);
    assert(!state.translucent);
    brush.drawflags = DRF_TRANSLUCENT;
    state = R_BrushEntityRenderState(&brush, BRUSH_SURFACE_REGULAR, 1.0f);
    assert(state.translucent);
    brush.drawflags = 0; brush.alpha = ENTALPHA_DEFAULT;
    state = R_BrushEntityRenderState(&brush, BRUSH_SURFACE_REGULAR, 0.4f);
    assert(state.translucent);
    assert(state.alpha == 0.4f);
    brush.alpha = 128;
    state = R_BrushEntityRenderState(&brush, BRUSH_SURFACE_REGULAR, 0.4f);
    assert(state.translucent);
    assert(state.alpha > 0.49f);
    assert(state.alpha < 0.51f);
    /* sworld_frag thresholds tex.a * entity alpha.  A solid texel must
     * survive and a below-cutoff texel must still be discarded. */
    state = R_BrushEntityRenderState(&brush, BRUSH_SURFACE_CUTOUT, 1.0f);
    assert(state.cutout);
    float fence_threshold = R_BrushFenceThreshold(&state);
    assert(state.alpha >= fence_threshold);
    assert(0.5f * state.alpha < fence_threshold);
    brush.alpha = ENTALPHA_DEFAULT;
    state = R_BrushEntityRenderState(&brush, BRUSH_SURFACE_CUTOUT, 1.0f);
    assert(R_BrushFenceThreshold(&state) > 0.665f);
    assert(R_BrushFenceThreshold(&state) < 0.667f);
    brush.alpha = 255;
    state = R_BrushEntityRenderState(&brush, BRUSH_SURFACE_REGULAR, 1.0f);
    assert(!state.translucent);
    puts("PASS: HW wire order and explicit brush alpha routing");
    return 0;
}
'''


def main():
    shader = (ROOT / "engine/h2shared/gl_shader.c").read_text()
    multiply = shader.index('"    vec4 color = tex * lm * v_color;\\n"')
    discard = shader.index('"    if (color.a < u_alpha_threshold) discard;\\n"', multiply)
    assert multiply < discard
    render_brush = function("engine/hexen2/gl_rsurf.c", "void R_RenderBrushPoly")
    assert "render_state = R_BrushEntityRenderState(e, surface_kind, default_alpha);" in render_brush
    cutout = render_brush.index("if (render_state.cutout)")
    cutout_end = render_brush.index("\n\t/* MLS_ABSLIGHT", cutout)
    cutout_route = render_brush[cutout:cutout_end]
    assert "GL_SetAlphaThreshold(R_BrushFenceThreshold(&render_state));" in cutout_route
    assert "R_SetAlphaToCoverage(!render_state.has_explicit_alpha);" in cutout_route

    # Pin the complete WEBSOFT brush route.  CL_RelinkEntities preserves
    # canonical brush alpha; the software adapter feeds it through the shared
    # classifier, then the saved edge scan draws only classified translucency.
    relink = function("engine/hexen2/cl_main.c", "static void CL_RelinkEntities")
    adapt = relink.index("ent->drawflags |= DRF_TRANSLUCENT;")
    publish = relink.index("cl_visedicts[cl_numvisedicts] = ent;")
    assert adapt < publish
    rdraw = (ROOT / "engine/h2shared/r_draw.c").read_text()
    assert "render_state = R_SoftwareBrushRenderState(currententity, psurf);" in rdraw
    propagate = "surface_p->flags = psurf->flags & ~SURF_TRANSLUCENT;"
    assert propagate in rdraw
    assert "if (render_state.translucent)\n\t\tsurface_p->flags |= SURF_TRANSLUCENT;" in rdraw
    edge_pass = function("engine/hexen2/r_main.c", "static void R_EdgeDrawing")
    assert edge_pass.index("R_DrawBEntitiesOnList ();") < edge_pass.rindex(
        "R_ScanEdges (Translucent);"
    )
    draw_surfaces = function("engine/h2shared/d_edge.c", "void D_DrawSurfaces")
    assert "if (s->flags & SURF_TRANSLUCENT)\n\t\t\t\t\tcontinue;" in draw_surfaces
    assert "if (!s->spans || !(s->flags & SURF_TRANSLUCENT))\n\t\t\t\t\tcontinue;" in draw_surfaces

    source = PRELUDE + function(
        "engine/hexen2/cl_hw.c", "static void HWCL_ParseEntityDelta"
    ) + function(
        "engine/hexen2/gl_rsurf.c", "brush_render_state_t R_BrushEntityRenderState"
    ) + function(
        "engine/hexen2/gl_rsurf.c", "static float R_BrushFenceThreshold"
    ) + TEST
    with tempfile.TemporaryDirectory(prefix="multiplayer-transparency-") as tmp:
        cfile = Path(tmp) / "wire.c"
        binary = Path(tmp) / "wire"
        cfile.write_text(source)
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
            "-Werror", "-I" + str(ROOT / "engine/h2shared"),
            "-I" + str(ROOT / "common"), str(cfile), "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
