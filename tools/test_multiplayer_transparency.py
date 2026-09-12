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
    brush.alpha = 255;
    assert(!R_BrushEntityTranslucent(&brush));
    puts("PASS: HW wire order and explicit brush alpha routing");
    return 0;
}
'''


def main():
    source = PRELUDE + function(
        "engine/hexen2/cl_hw.c", "static void HWCL_ParseEntityDelta"
    ) + function(
        "engine/hexen2/gl_rsurf.c", "static qboolean R_BrushEntityTranslucent"
    ) + function(
        "engine/hexen2/gl_rsurf.c", "static float R_BrushEntityAlpha"
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
