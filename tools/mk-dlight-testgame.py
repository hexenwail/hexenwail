#!/usr/bin/env python3
"""Write a game directory whose demo2 has dynamic lights that hold still, for
tools/sweep-dlight-gpu.steps (#340 P5b).

Stock wall torches cast no dynamic light: Init_Torch (torch.hc) calls
makestatic, and a static entity never reaches CL_RelinkEntities.  A torch with
a targetname skips makestatic, stays a real entity, and its flame model
(XF_TORCH_GLOW) gets the torch dlight, radius 150, 8 units up (cl_main.c).
That is tools/dlight-leak-check.sh's trick, and this writes the same entity
lump as an external .ent, demo2's own lump with:

  * "targetname" "dlsweep" added to the three torches the leak check's
    scenes use: 170 312 80 (its "lit" scene, a torch on its own wall),
    -720 -88 224 (scene 1, the far side of a wall out of the light's PVS)
    and -798 930 304 (scene 3, a back face in the light's PVS);
  * the leak check's cast-scene pair appended: a light_torch_castle with a
    targetname standing free at 120 250 32, and an obj_barrel at 120 288 0
    between it and the wall -- an alias model inside a dlight, which is what
    r_lightclusters_models changes.

  * every monster dropped: each monster_* entity and each
    func_monsterspawner.  The sweep moves its camera unpaused (setpos is the
    server's), and a spider on the lit wall turned between two frames of
    the same view; a monster that attacks also lights nothing the torches
    do not.

Nothing else changes: every other entity, and the order of all of them, is
demo2's, and the torches' dlights are the only dynamic lights in view.

Nothing here is committed: the lump is read out of the retail data each time.

Usage:  mk-dlight-testgame.py <basedir> [game]
reads the demo2.bsp the engine loads from <basedir>/data1 (in the engine's
search order: tools/mk-mirror-testgame.py's read_from_paks) and writes
<basedir>/<game>/maps/demo2.ent (game defaults to "dlight").  An .ent in a
game directory outranks data1's paks; run the engine with -game <game> and
-userdir on an empty one, as tools/headless-drive.sh does.  The basedir has
to hold data1 as a directory or a link that resolves inside
tools/headless-drive.sh's sandbox (anything outside $HOME, as
tools/mk-alias-testgame.py says).

  mk-dlight-testgame.py --self-test
builds a small synthetic BSP and checks the edit and every refusal on it (no
retail data needed).
"""
import argparse
import importlib.util
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("mk_mirror_testgame", os.path.join(HERE, "mk-mirror-testgame.py"))
_mirror = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mirror)

TARGETNAME = "dlsweep"
TORCHES = ("170 312 80", "-720 -88 224", "-798 930 304")
ADDED = (
    '{\n"classname" "light_torch_castle"\n"origin" "120 250 32"\n"targetname" "%s"\n}\n' % TARGETNAME,
    '{\n"classname" "obj_barrel"\n"origin" "120 288 0"\n}\n',
)


class DlightError(Exception):
    pass


def entity_lump(bsp):
    """demo2's entity text: BSP29 lump 0, up to its terminating NUL."""
    if len(bsp) < 12 or struct.unpack_from("<i", bsp, 0)[0] != 29:
        raise DlightError("not a BSP29 map")
    off, length = struct.unpack_from("<ii", bsp, 4)
    if off < 0 or length < 0 or off + length > len(bsp):
        raise DlightError("the entity lump runs past the end of the map")
    return bsp[off:off + length].split(b"\0")[0].decode("latin1")


MONSTER = re.compile(r'\{[^{}]*"classname"\s*"(?:monster_\w*|func_monsterspawner)"[^{}]*\}\n?')


def tag(ents):
    """The lump with each of TORCHES given the targetname, every monster
    dropped and ADDED appended.  Exactly one light_torch* per origin, and
    none already named: a second torch would make which one lights the
    frame a guess, and a named one is already a real entity whose name
    something may target."""
    ents = MONSTER.sub("", ents)
    for origin in TORCHES:
        pat = re.compile(r'\{[^{}]*"origin"\s*"%s"[^{}]*\}' % re.escape(origin))
        hits = [m for m in pat.finditer(ents) if re.search(r'"classname"\s*"light_torch', m.group(0))]
        if len(hits) != 1:
            raise DlightError('expected exactly one light_torch at "%s", found %d' % (origin, len(hits)))
        block = hits[0].group(0)
        if '"targetname"' in block:
            raise DlightError('the light_torch at "%s" already has a targetname' % origin)
        m = hits[0]
        ents = ents[:m.start()] + block[:-1] + '"targetname" "%s"\n}' % TARGETNAME + ents[m.end():]
    if not ents.endswith("\n"):
        ents += "\n"
    return ents + "".join(ADDED)


def write_game(basedir, game):
    data, where = _mirror.read_from_paks(os.path.join(basedir, "data1"), "maps/demo2.bsp")
    out = tag(entity_lump(data))
    dst = os.path.join(basedir, game, "maps")
    os.makedirs(dst, exist_ok=True)
    with open(os.path.join(dst, "demo2.ent"), "w", encoding="latin1") as f:
        f.write(out)
    return where, os.path.join(dst, "demo2.ent")


def synthetic_bsp(ents):
    """A BSP29 header whose lump 0 is `ents`, NUL-terminated; the other
    lumps empty.  All entity_lump reads."""
    body = ents.encode("latin1") + b"\0"
    head = struct.pack("<i", 29) + struct.pack("<ii", 4 + 15 * 8, len(body)) + b"\0" * 14 * 8
    return head + body


def self_test():
    failures = []
    # spelled out, not TORCHES / ADDED: the sweep's cameras are aimed at these
    want = ("170 312 80", "-720 -88 224", "-798 930 304")
    torch = '{\n"classname" "light_torch_castle"\n"origin" "%s"\n}\n'
    good = '{\n"classname" "worldspawn"\n}\n' + "".join(torch % o for o in want) + \
        '{\n"classname" "light"\n"origin" "170 312 80"\n}\n' + \
        '{\n"classname" "monster_spider_yellow_small"\n"origin" "1 2 3"\n}\n' + \
        '{\n"origin" "4 5 6"\n"classname" "func_monsterspawner"\n"targetname" "t2"\n}\n' + \
        '{\n"classname" "obj_monsterbox"\n"origin" "7 8 9"\n}\n'
    try:
        out = tag(entity_lump(synthetic_bsp(good)))
        for o in want:
            if not re.search(r'"origin" "%s"\n"targetname" "dlsweep"\n\}' % re.escape(o), out):
                failures.append("the torch at %s was not named" % o)
        if out.count('"targetname"') != 4:
            failures.append("%d targetnames, want 4" % out.count('"targetname"'))
        if not out.endswith('{\n"classname" "light_torch_castle"\n"origin" "120 250 32"\n"targetname" "dlsweep"\n}\n'
                            '{\n"classname" "obj_barrel"\n"origin" "120 288 0"\n}\n'):
            failures.append("the cast-scene light and barrel are not appended")
        if '{\n"classname" "light"\n"origin" "170 312 80"\n}' not in out:
            failures.append("a plain light at a torch's origin was touched")
        if "monster_spider" in out or "func_monsterspawner" in out:
            failures.append("a monster or a spawner survived")
        if '{\n"classname" "obj_monsterbox"\n"origin" "7 8 9"\n}\n' not in out:
            failures.append("an entity that only names a monster was dropped")
    except DlightError as e:
        failures.append("the good map refused: %s" % e)
    refusals = {
        "a torch missing": good.replace(torch % want[1], ""),
        "a torch twice": good + torch % want[2],
        "a torch already named": good.replace(torch % want[0],
                                              torch.replace("}", '"targetname" "t1"\n}') % want[0]),
    }
    for what, ents in refusals.items():
        try:
            tag(entity_lump(synthetic_bsp(ents)))
            failures.append("%s: not refused" % what)
        except DlightError:
            pass
    for what, data in (("a BSP30", struct.pack("<i", 30) + synthetic_bsp(good)[4:]),
                       ("a lump past the end", synthetic_bsp(good)[:-20])):
        try:
            entity_lump(data)
            failures.append("%s: not refused" % what)
        except DlightError:
            pass
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "data1", "maps"))
        with open(os.path.join(d, "data1", "maps", "demo2.bsp"), "wb") as f:
            f.write(synthetic_bsp(good))
        try:
            _, path = write_game(d, "dlight")
            with open(path, encoding="latin1") as f:
                if f.read() != tag(good):
                    failures.append("write_game wrote something other than tag()'s lump")
        except (DlightError, _mirror.MirrorError, OSError) as e:
            failures.append("write_game: %s" % e)
    for f in failures:
        print("FAIL: " + f)
    if failures:
        print("mk-dlight-testgame self-test: %d failure(s)" % len(failures))
        return 1
    print("mk-dlight-testgame self-test: OK, the edit, the monsters dropped, 5 refusals and the write")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("basedir", nargs="?")
    ap.add_argument("game", nargs="?", default="dlight")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.basedir:
        ap.error("a basedir is needed")
    try:
        where, path = write_game(a.basedir, a.game)
    except (DlightError, _mirror.MirrorError, OSError) as e:
        print("mk-dlight-testgame: %s" % e, file=sys.stderr)
        return 1
    print("mk-dlight-testgame: %s from %s" % (path, where))
    return 0


if __name__ == "__main__":
    sys.exit(main())
