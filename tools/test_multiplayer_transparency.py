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
typedef int qboolean;
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
    assert(!R_BrushEntityTranslucent(&brush));
    brush.drawflags = DRF_TRANSLUCENT;
    assert(R_BrushEntityTranslucent(&brush));
    assert(R_BrushEntityAlpha(&brush, 0.4f) == 0.4f);
    brush.drawflags = 0; brush.alpha = 128;
    assert(R_BrushEntityTranslucent(&brush));
    assert(R_BrushEntityAlpha(&brush, 0.4f) > 0.49f);
    assert(R_BrushEntityAlpha(&brush, 0.4f) < 0.51f);
    /* sworld_frag thresholds tex.a * entity_alpha.  A solid texel must
     * survive and a below-cutoff texel must still be discarded. */
    float entity_alpha = R_BrushEntityAlpha(&brush, 1.0f);
    float fence_threshold = R_BrushFenceThreshold(&brush);
    assert(entity_alpha >= fence_threshold);
    assert(0.5f * entity_alpha < fence_threshold);
    brush.alpha = ENTALPHA_DEFAULT;
    assert(R_BrushFenceThreshold(&brush) > 0.665f);
    assert(R_BrushFenceThreshold(&brush) < 0.667f);
    brush.alpha = 255;
    assert(!R_BrushEntityTranslucent(&brush));
    puts("PASS: HW wire order and explicit brush alpha routing");
    return 0;
}
'''


def main():
    shader = (ROOT / "engine/h2shared/gl_shader.c").read_text()
    multiply = shader.index('"    vec4 color = tex * lm * v_color;\\n"')
    discard = shader.index('"    if (color.a < u_alpha_threshold) discard;\\n"', multiply)
    assert multiply < discard
    brush = (ROOT / "engine/hexen2/gl_rsurf.c").read_text()
    assert "GL_SetAlphaThreshold(R_BrushFenceThreshold(e));" in brush
    assert "R_SetAlphaToCoverage (e->alpha == ENTALPHA_DEFAULT);" in brush

    # Pin the complete WEBSOFT brush route.  CL_RelinkEntities maps partial
    # protocol alpha to DRF before publishing the entity; brush face emission
    # propagates DRF to SURF_TRANSLUCENT; the first edge scan skips that flag
    # and the saved second scan draws only that flag.
    relink = function("engine/hexen2/cl_main.c", "static void CL_RelinkEntities")
    adapt = relink.index("ent->drawflags |= DRF_TRANSLUCENT;")
    publish = relink.index("cl_visedicts[cl_numvisedicts] = ent;")
    assert adapt < publish
    rdraw = (ROOT / "engine/h2shared/r_draw.c").read_text()
    propagate = "surface_p->flags = psurf->flags | SURF_TRANSLUCENT;"
    assert propagate in rdraw
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
        "engine/hexen2/gl_rsurf.c", "static qboolean R_BrushEntityTranslucent"
    ) + function(
        "engine/hexen2/gl_rsurf.c", "static float R_BrushEntityAlpha"
    ) + function(
        "engine/hexen2/gl_rsurf.c", "static float R_BrushFenceThreshold"
    ) + TEST
    with tempfile.TemporaryDirectory(prefix="multiplayer-transparency-") as tmp:
        cfile = Path(tmp) / "wire.c"
        binary = Path(tmp) / "wire"
        cfile.write_text(source)
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
            "-Werror", str(cfile), "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
