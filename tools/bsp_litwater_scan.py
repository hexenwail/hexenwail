#!/usr/bin/env python3
"""Report which maps carry LIT WATER -- liquid faces that kept a lightmap.

Vanilla Hexen II qbsp marks every liquid texinfo TEX_SPECIAL, which means "no
lightmap, ever".  Modern ericw-tools does not (litwater is on unless
-nolitwater), so its liquid faces carry real lightmap samples.  Hexenwail
renders those through the world shader as of uhexen2-a5nn.2; before that it
ignored the samples and drew them flat.

This answers "does any of this content actually reach that path", which is the
question to ask before blaming -- or crediting -- lit water for what a tester
sees.  It reads the BSP directly, so it needs no engine and no game running.

    python3 tools/bsp_litwater_scan.py data1/pak0.pak sot/maps/*.bsp

Handles BSP29 and BSP2, loose files and .pak archives.  A map is only
interesting if it prints LIT.
"""
import glob
import os
import struct
import sys

LUMP_TEXTURES, LUMP_TEXINFO, LUMP_FACES = 2, 6, 7
TEX_SPECIAL = 1
BSP2_MAGIC = 0x32505342     # 'BSP2'


def read_pak(path):
    """Return {name: bytes} for a Quake/Hexen II .pak."""
    d = open(path, 'rb').read()
    magic, ofs, ln = struct.unpack_from('<4sii', d, 0)
    if magic != b'PACK':
        raise ValueError('not a pak file')
    out = {}
    for i in range(ln // 64):
        name, o, s = struct.unpack_from('<56sii', d, ofs + i * 64)
        out[name.split(b'\0')[0].decode('latin1')] = d[o:o + s]
    return out


def scan(b):
    """(lit, unlit) liquid face counts, or None if the version is unknown."""
    ver = struct.unpack_from('<i', b, 0)[0]
    if ver == 29:
        fsize, ti_ofs, lo_ofs, ti_fmt = 20, 10, 16, '<h'
    elif ver == BSP2_MAGIC:
        fsize, ti_ofs, lo_ofs, ti_fmt = 28, 16, 24, '<i'
    else:
        return None
    lumps = [struct.unpack_from('<ii', b, 4 + 8 * i) for i in range(15)]
    mo, _ = lumps[LUMP_TEXTURES]
    to, tl = lumps[LUMP_TEXINFO]
    fo, fl = lumps[LUMP_FACES]

    # texinfo is 8 floats, then miptex index, then flags -- same in both formats
    mip = [struct.unpack_from('<i', b, to + i * 40 + 32)[0] for i in range(tl // 40)]
    flg = [struct.unpack_from('<i', b, to + i * 40 + 36)[0] for i in range(tl // 40)]

    names = []
    for i in range(struct.unpack_from('<i', b, mo)[0]):
        off = struct.unpack_from('<i', b, mo + 4 + 4 * i)[0]
        names.append('' if off < 0 else
                     struct.unpack_from('<16s', b, mo + off)[0].split(b'\0')[0].decode('latin1'))

    lit = unlit = 0
    for i in range(fl // fsize):
        ti = struct.unpack_from(ti_fmt, b, fo + i * fsize + ti_ofs)[0]
        if not (0 <= ti < len(mip)) or not (0 <= mip[ti] < len(names)):
            continue
        if not names[mip[ti]].startswith('*'):
            continue
        lightofs = struct.unpack_from('<i', b, fo + i * fsize + lo_ofs)[0]
        if (flg[ti] & TEX_SPECIAL) or lightofs == -1:
            unlit += 1
        else:
            lit += 1
    return lit, unlit


def report(label, data):
    try:
        r = scan(data)
    except Exception as e:                          # a truncated or foreign file
        print('%-44s ERROR %s' % (label, e))
        return 0
    if r is None:
        print('%-44s unrecognised BSP version' % label)
        return 0
    lit, unlit = r
    if lit:
        print('%-44s LIT  %d lit liquid faces (%d unlit)' % (label, lit, unlit))
    elif unlit:
        print('%-44s --   %d liquid faces, none lit' % (label, unlit))
    else:
        print('%-44s --   no liquid faces' % label)
    return 1 if lit else 0


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    paths = [p for a in argv for p in (glob.glob(a) or [a])]
    total = affected = 0
    for path in paths:
        if path.lower().endswith('.pak'):
            for name, data in sorted(read_pak(path).items()):
                if name.lower().endswith('.bsp'):
                    total += 1
                    affected += report('%s:%s' % (os.path.basename(path), name), data)
        elif path.lower().endswith('.bsp'):
            total += 1
            affected += report(os.path.basename(path), open(path, 'rb').read())
    print('\n%d maps scanned, %d with lit water' % (total, affected))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
