#!/usr/bin/env python3
"""Write a game directory for tools/sweep-shadow-gpu.steps: demo2 with the
dynamic lights every tools/dlight-leak-check.sh scene needs, as one map, and
a second map with a skeletal caster (#340 P6b).

tools/mk-dlight-testgame.py's demo2 names three of the leak check's torches
and adds its cast-scene light and barrel, every monster dropped.  The shadow
maps' sweep needs the rest of the leak check in the same map, so this takes
that lump and also:

  * names the torch at -758 482 192 (the leak check's scene 2, a face in the
    light's PVS with a wall between, which only the shadow maps fix), and
  * appends its alias scene's obj_barrel at -838 434 136, in the door recess
    that torch's wall hides from it.

The leak check takes those in runs of their own; here every one is in the one
lump, and each box is measured on the frame of its own camera.  No camera of
tools/sweep-shadow-gpu.steps sees one scene's addition from another's.

maps/demo2m.bsp and maps/demo2m.ent, the second map: demo2.bsp byte for byte
under another name (an .ent belongs to a map name), and the lump above with
the cast scene's obj_barrel replaced by an obj_tombstone1 at the same spot,
which r_enhancedmodels draws as tools/mk-alias-testgame.py's two-joint MD5
box (models/tombstn1.md5mesh and .md5anim, written here too): a skeletal
caster.

Nothing here is committed: the lump and the map come out of the retail data
each time, and the models are text.

Usage:  mk-shadow-testgame.py <basedir> [game]     (game defaults to "shadow")
reads demo2 from <basedir>/data1 as tools/mk-dlight-testgame.py does and
writes <basedir>/<game>/maps/{demo2.ent,demo2m.bsp,demo2m.ent} and
<basedir>/<game>/models/tombstn1.md5{mesh,anim}.  Run the engine with
-game <game>.  The basedir has to hold data1 as a directory or a link that
resolves inside tools/headless-drive.sh's sandbox, as the other two say.

  mk-shadow-testgame.py --self-test
checks the edits and every refusal on synthetic lumps (no retail data).
"""
import argparse
import importlib.util
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


_dlight = _load("mk_dlight_testgame", "mk-dlight-testgame.py")
_mirror = _dlight._mirror
_alias = _load("mk_alias_testgame", "mk-alias-testgame.py")

TARGETNAME = _dlight.TARGETNAME
SCENE2_TORCH = "-758 482 192"
RECESS_BARREL = '{\n"classname" "obj_barrel"\n"origin" "-838 434 136"\n}\n'
CAST_BARREL = '{\n"classname" "obj_barrel"\n"origin" "120 288 0"\n}\n'
CAST_TOMB = '{\n"classname" "obj_tombstone1"\n"origin" "120 288 0"\n}\n'


class ShadowError(Exception):
    pass


def tag(ents):
    """demo2's lump as tools/mk-dlight-testgame.py tags it, plus scene 2's
    torch named and the recess barrel appended."""
    ents = _dlight.tag(ents)
    pat = re.compile(r'\{[^{}]*"origin"\s*"%s"[^{}]*\}' % re.escape(SCENE2_TORCH))
    hits = [m for m in pat.finditer(ents) if re.search(r'"classname"\s*"light_torch', m.group(0))]
    if len(hits) != 1:
        raise ShadowError('expected exactly one light_torch at "%s", found %d' % (SCENE2_TORCH, len(hits)))
    m = hits[0]
    if '"targetname"' in m.group(0):
        raise ShadowError('the light_torch at "%s" already has a targetname' % SCENE2_TORCH)
    ents = ents[:m.start()] + m.group(0)[:-1] + '"targetname" "%s"\n}' % TARGETNAME + ents[m.end():]
    return ents + RECESS_BARREL


def tombstone(ents):
    """demo2m's lump: the tagged one with the cast scene's barrel a tombstone,
    exactly the one tools/mk-dlight-testgame.py appended, nothing else."""
    if ents.count(CAST_BARREL) != 1:
        raise ShadowError("expected the cast scene's barrel once, found %d" % ents.count(CAST_BARREL))
    return ents.replace(CAST_BARREL, CAST_TOMB)


def write_game(basedir, game):
    data, where = _mirror.read_from_paks(os.path.join(basedir, "data1"), "maps/demo2.bsp")
    ents = tag(_dlight.entity_lump(data))
    maps = os.path.join(basedir, game, "maps")
    models = os.path.join(basedir, game, "models")
    os.makedirs(maps, exist_ok=True)
    os.makedirs(models, exist_ok=True)
    with open(os.path.join(maps, "demo2.ent"), "w", encoding="latin1") as f:
        f.write(ents)
    with open(os.path.join(maps, "demo2m.ent"), "w", encoding="latin1") as f:
        f.write(tombstone(ents))
    with open(os.path.join(maps, "demo2m.bsp"), "wb") as f:
        f.write(data)
    for name, text in (("tombstn1.md5mesh", _alias.md5mesh()), ("tombstn1.md5anim", _alias.md5anim())):
        with open(os.path.join(models, name), "w") as f:
            f.write(text)
    return where


def self_test():
    failures = []
    torch = '{\n"classname" "light_torch_castle"\n"origin" "%s"\n}\n'
    good = '{\n"classname" "worldspawn"\n}\n' + "".join(
        torch % o for o in _dlight.TORCHES + (SCENE2_TORCH,)) + \
        '{\n"classname" "monster_imp"\n"origin" "1 2 3"\n}\n'
    try:
        out = tag(good)
        if not re.search(r'"origin" "%s"\n"targetname" "%s"\n\}' % (re.escape(SCENE2_TORCH), TARGETNAME), out):
            failures.append("scene 2's torch was not named")
        if out.count('"targetname" "%s"' % TARGETNAME) != 5:
            failures.append("%d named, want 5 (four torches and the cast light)"
                            % out.count('"targetname" "%s"' % TARGETNAME))
        if not out.endswith(CAST_BARREL + RECESS_BARREL):
            failures.append("the recess barrel is not appended after the cast pair")
        if "monster_imp" in out:
            failures.append("a monster survived")
        t = tombstone(out)
        if CAST_BARREL in t or t.count(CAST_TOMB) != 1 or RECESS_BARREL not in t:
            failures.append("the tombstone edit is wrong: the cast barrel must go, the recess barrel stay")
    except (ShadowError, _dlight.DlightError) as e:
        failures.append("the good lump refused: %s" % e)
    for what, ents in (("scene 2's torch missing", good.replace(torch % SCENE2_TORCH, "")),
                       ("scene 2's torch twice", good + torch % SCENE2_TORCH),
                       ("scene 2's torch already named",
                        good.replace(torch % SCENE2_TORCH,
                                     torch.replace("}", '"targetname" "x"\n}') % SCENE2_TORCH))):
        try:
            tag(ents)
            failures.append("%s: not refused" % what)
        except (ShadowError, _dlight.DlightError):
            pass
    try:
        tombstone(tag(good) + CAST_BARREL)
        failures.append("two cast barrels: not refused")
    except ShadowError:
        pass
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "data1", "maps"))
        bsp = _dlight.synthetic_bsp(good)
        with open(os.path.join(d, "data1", "maps", "demo2.bsp"), "wb") as f:
            f.write(bsp)
        try:
            write_game(d, "shadow")
            mp = os.path.join(d, "shadow", "maps")
            with open(os.path.join(mp, "demo2m.bsp"), "rb") as f:
                if f.read() != bsp:
                    failures.append("demo2m.bsp is not demo2.bsp byte for byte")
            with open(os.path.join(mp, "demo2m.ent"), encoding="latin1") as f:
                if f.read() != tombstone(tag(good)):
                    failures.append("demo2m.ent is not the tombstone lump")
            if not os.path.isfile(os.path.join(d, "shadow", "models", "tombstn1.md5mesh")):
                failures.append("no tombstn1.md5mesh written")
        except (ShadowError, _dlight.DlightError, _mirror.MirrorError, OSError) as e:
            failures.append("write_game: %s" % e)
    for f in failures:
        print("FAIL: " + f)
    if failures:
        print("mk-shadow-testgame self-test: %d failure(s)" % len(failures))
        return 1
    print("mk-shadow-testgame self-test: OK, the edits, 4 refusals and the write")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("basedir", nargs="?")
    ap.add_argument("game", nargs="?", default="shadow")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.basedir:
        ap.error("a basedir is needed")
    try:
        where = write_game(a.basedir, a.game)
    except (ShadowError, _dlight.DlightError, _mirror.MirrorError, OSError) as e:
        print("mk-shadow-testgame: %s" % e, file=sys.stderr)
        return 1
    print("mk-shadow-testgame: %s/%s from %s" % (a.basedir, a.game, where))
    return 0


if __name__ == "__main__":
    sys.exit(main())
