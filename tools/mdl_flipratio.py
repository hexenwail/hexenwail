#!/usr/bin/env python3
"""mdl_flipratio.py -- recompute the engine's flipbook heuristic offline.

Why this exists (uhexen2-aqm0): Mathuzzz reported archers moving jerkily.
r_lerp_autodetect (engine/h2shared/gl_model.c, uhexen2-f807) decides per model
whether frame interpolation is worth doing, by measuring the worst per-vertex
displacement between poses that get blended, over the model's bbox diagonal.
Flipbooks -- flames, torches, explosions whose "frames" are unrelated geometry
sheets -- score near 1.0; real deforming animation scores low.  Above
r_lerp_autodetect_threshold (default 0.35) the model gets MOD_NOLERP and its
animation snaps instead of blending.

Deciding whether a given model trips that needs no GPU and no person, only the
pose data, so this computes the same number Mod_ComputeFlipbookRatio does and
prints it per model.

The catch it was written to find: every animation a model has lives end-to-end
in one frame array, so "frame f and frame f+1" includes the seam between the
last frame of one animation and the first of the next -- poses that are never
blended into each other and that differ about as much as two unrelated models
would.  --seams reports both numbers so the seam contribution is visible:

    models/archer.mdl  0.410 counting seams, 0.242 without.  The 0.410 is
    deathA22 -> draw1, which nothing ever interpolates; the worst pair the
    engine really blends is fire1 -> fire2 at 0.242.  One artifact was
    switching off interpolation for every animation the archer has.

Usage:  tools/mdl_flipratio.py <pak-or-dir> [...] [--seams] [--all]
  e.g.  tools/mdl_flipratio.py ~/hexen2/data1 --seams

Reads .mdl members out of .pak files (later paks win, as the engine's search
path does) and loose .mdl files.  Prints only over-threshold models unless
--all.  IDPO (v6) and RAPO (v50) alias models; anything else is skipped.
"""
import struct, sys, os, glob, math, re

THRESHOLD = 0.35        # r_lerp_autodetect_threshold default (gl_rmain.c)
BBOX_PAD  = 10.0        # Mod_LoadAliasModel pads mins/maxs by +/-10


def read_pak(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'PACK':
        return
    dirofs, dirlen = struct.unpack_from('<ii', data, 4)
    for i in range(dirlen // 64):
        o = dirofs + i * 64
        name = data[o:o+56].split(b'\0')[0].decode('latin-1').lower()
        fp, fl = struct.unpack_from('<ii', data, o + 56)
        yield name, data[fp:fp+fl]


def parse_mdl(data):
    """Layout per common/genmodel.h.  Returns None for anything not an alias model."""
    ident = data[:4]
    if ident not in (b'IDPO', b'RAPO'):
        return None
    scale     = struct.unpack_from('<3f', data, 8)
    scale_org = struct.unpack_from('<3f', data, 20)
    # boundingradius@32, eyeposition@36..47
    numskins, skinw, skinh, numverts, numtris, numframes = struct.unpack_from('<6i', data, 48)
    # synctype@72, flags@76, size@80
    p = 84
    num_st = numverts
    if ident == b'RAPO':
        (num_st,) = struct.unpack_from('<i', data, p); p += 4
    for _ in range(numskins):
        (group,) = struct.unpack_from('<i', data, p); p += 4
        if group == 0:
            p += skinw * skinh
        else:
            (n,) = struct.unpack_from('<i', data, p); p += 4
            p += 4 * n + n * skinw * skinh
    p += num_st * 12            # stvert_t / RAPO st verts
    p += numtris * 16           # dtriangle_t and dnewtriangle_t are both 16
    poses, frames = [], []
    for _ in range(numframes):
        (ftype,) = struct.unpack_from('<i', data, p); p += 4
        if ftype == 0:
            name = data[p+8:p+24].split(b'\0')[0].decode('latin-1')
            p += 24
            frames.append((len(poses), 1, name)); poses.append(p); p += numverts * 4
        else:
            (n,) = struct.unpack_from('<i', data, p); p += 4
            p += 8 + 4 * n      # group bbox + intervals
            first = len(poses)
            name = data[p+8:p+24].split(b'\0')[0].decode('latin-1')
            for _ in range(n):
                p += 24
                poses.append(p); p += numverts * 4
            frames.append((first, n, name))
    if p != len(data):
        return None             # misparsed; don't report a bogus number
    return dict(data=data, scale=scale, scale_org=scale_org,
                numverts=numverts, poses=poses, frames=frames)


def frames_are_sequential(a, b):
    """Mirror of Mod_FramesAreSequential: same stem, trailing number +1."""
    ma, mb = re.match(r'^(.*?)(\d+)$', a), re.match(r'^(.*?)(\d+)$', b)
    if not ma or not mb or ma.group(1) != mb.group(1):
        return False
    return int(mb.group(2)) == int(ma.group(2)) + 1


def flipratio(m):
    """Returns (ratio_same_sequence_only, ratio_counting_seams, worst_seam_label)."""
    data, nv = m['data'], m['numverts']
    sc, so = m['scale'], m['scale_org']
    poses, frames = m['poses'], m['frames']
    if len(poses) < 2:
        return 0.0, 0.0, None

    lo = [1e30] * 3; hi = [-1e30] * 3
    for po in poses:
        chunk = data[po:po+nv*4]
        for k in range(3):
            col = chunk[k::4]
            a = min(col) * sc[k] + so[k]; b = max(col) * sc[k] + so[k]
            if a < lo[k]: lo[k] = a
            if b > hi[k]: hi[k] = b
    diag = math.sqrt(sum(((hi[k] + BBOX_PAD) - (lo[k] - BBOX_PAD)) ** 2 for k in range(3)))
    if diag < 1.0:
        return 0.0, 0.0, None

    best_seq = best_all = 0.0
    worst_seam = None
    for fi, (first, n, name) in enumerate(frames):
        if n > 1:
            pairs, in_seq = n, True
        elif fi + 1 < len(frames) and frames[fi+1][1] == 1:
            pairs, in_seq = 1, frames_are_sequential(name, frames[fi+1][2])
        else:
            continue
        for i in range(pairs):
            if n > 1:
                a, b = first + i, first + ((i + 1) % n)
                label = '%s[%d->%d]' % (name, i, (i + 1) % n)
            else:
                a, b = first, frames[fi+1][0]
                label = '%s -> %s' % (name, frames[fi+1][2])
            ca = data[poses[a]:poses[a]+nv*4]
            cb = data[poses[b]:poses[b]+nv*4]
            worst = 0.0
            for v in range(0, nv * 4, 4):
                dx = (cb[v]   - ca[v])   * sc[0]
                dy = (cb[v+1] - ca[v+1]) * sc[1]
                dz = (cb[v+2] - ca[v+2]) * sc[2]
                d2 = dx*dx + dy*dy + dz*dz
                if d2 > worst: worst = d2
            r = math.sqrt(worst) / diag
            if r > best_all:
                best_all = r
                if not in_seq: worst_seam = label
            if in_seq and r > best_seq:
                best_seq = r
    return best_seq, best_all, worst_seam


def collect(paths):
    models = {}
    for path in paths:
        if os.path.isdir(path):
            for pak in sorted(glob.glob(os.path.join(path, '*.pak'))):
                for name, blob in read_pak(pak):
                    if name.endswith('.mdl'): models[name] = blob
            for mdl in sorted(glob.glob(os.path.join(path, '**', '*.mdl'), recursive=True)):
                models[os.path.relpath(mdl, path).replace(os.sep, '/').lower()] = open(mdl, 'rb').read()
        elif path.lower().endswith('.pak'):
            for name, blob in read_pak(path):
                if name.endswith('.mdl'): models[name] = blob
        else:
            models[os.path.basename(path).lower()] = open(path, 'rb').read()
    return models


def main(argv):
    show_seams = '--seams' in argv
    show_all   = '--all' in argv
    paths = [a for a in argv[1:] if not a.startswith('--')]
    if not paths:
        print(__doc__); return 1

    models = collect(paths)
    rows = []
    for name in sorted(models):
        m = parse_mdl(models[name])
        if not m: continue
        seq, allp, seam = flipratio(m)
        rows.append((seq, allp, seam, name, len(m['frames']), m['numverts']))
    rows.sort(reverse=True)

    print('r_lerp_autodetect_threshold = %.2f  (above it: MOD_NOLERP, animation snaps)\n' % THRESHOLD)
    hdr = '%-30s %7s' % ('model', 'ratio')
    if show_seams: hdr += ' %7s  %s' % ('w/seams', 'worst seam pair')
    print(hdr)
    shown = 0
    for seq, allp, seam, name, nf, nv in rows:
        if not show_all and seq <= THRESHOLD and not (show_seams and allp > THRESHOLD):
            continue
        line = '%-30s %7.3f' % (name, seq)
        if show_seams:
            line += ' %7.3f  %s' % (allp, seam or '')
        if seq > THRESHOLD: line += '   NOLERP'
        print(line); shown += 1
    print('\n%d of %d alias models listed; %d over threshold' %
          (shown, len(rows), sum(1 for r in rows if r[0] > THRESHOLD)))
    if show_seams:
        masked = [r for r in rows if r[1] > THRESHOLD >= r[0]]
        print('%d models would trip the threshold only because of an animation seam' % len(masked))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
