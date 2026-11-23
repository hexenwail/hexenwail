#!/usr/bin/env python3
"""What the reflected pass derives from the reflected view is put back before
the mirror's own surfaces draw, and r_speeds counts both passes (#517).

R_Mirror (engine/hexen2/gl_rmain.c) renders the scene a second time from the
view reflected in the mirror's plane: R_RenderScene -> R_SetupFrame ->
R_SetupGL, the same calls the view itself made.  Those write per-frame state
from whatever view is current, and the mirror's own surfaces, blended over the
reflection at the end of R_Mirror, are drawn from the VIEW, so each of them
has to be derived again once the view is back:

  1. The world frame record (R_SetupWorldFrame -> R_SetWorldFrame): the
     caustics intensity comes from r_viewleaf, the reflected leaf in the
     reflected pass.  A view under water lost its caustics on the mirror, a
     reflection from under water put them on it.
  2. The light grid (R_LightCluster_Update and its two binds): the dlights
     were re-clustered against the reflected view and projection, so the
     mirror's surfaces looked their froxels up in the reflection's grid.  It reads the
     modelview and projection current when it runs, so it has to come after
     R_Mirror loads the view's matrix and un-flips the projection.
  3. r_world_matrix and the frustum, the view's matrix and planes.

And the counters: R_SetupFrame zeroed c_brush_polys / c_alias_polys /
c_texbinds, so the reflected pass threw the view's counts away and r_speeds 1
on a mirror map reported the reflection and the mirror only.  R_RenderView
zeroes them, once, ahead of both passes and whatever r_speeds is.

tools/sweep-mirror-leaks.steps is the frames these were found on.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
RMAIN = "engine/hexen2/gl_rmain.c"
errors = []


def fail(msg):
    errors.append(msg)


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def bodies(code):
    """name -> list of bodies (text between the outer braces)."""
    out = {}
    for m in re.finditer(r"^(?:static\s+)?void\s+(\w+)\s*\(\s*void\s*\)\s*\{", code, re.M):
        i, depth = m.end() - 1, 0
        while i < len(code):
            depth += {"{": 1, "}": -1}.get(code[i], 0)
            if depth == 0:
                break
            i += 1
        out.setdefault(m.group(1), []).append(code[m.end():i])
    return out


def one(fns, name):
    b = fns.get(name, [])
    if len(b) != 1:
        fail("%s: %d definitions of %s, this pin reads exactly one" % (RMAIN, len(b), name))
        return None
    return b[0]


def call(name):
    return re.compile(r"\b%s\s*\(\s*\)\s*;" % name)


def at_top(body, pos):
    """pos is at brace depth 0 of body: not inside an if or a loop."""
    return body[:pos].count("{") == body[:pos].count("}")


ZERO = {c: re.compile(r"\b%s\s*=\s*0\s*;" % c) for c in ("c_brush_polys", "c_alias_polys", "c_texbinds")}


def check(code):
    fns = bodies(code)

    # --- R_SetupWorldFrame: the world frame record, from r_viewleaf ---------
    wf = one(fns, "R_SetupWorldFrame")
    if wf is not None and not re.search(r"\bR_SetWorldFrame\s*\(\s*R_CausticsIntensity\s*\(\s*\)", wf):
        fail("%s: R_SetupWorldFrame must write the world frame record "
             "(R_SetWorldFrame (R_CausticsIntensity (), ...)) -- it is what R_Mirror re-runs to put the "
             "view's caustics back (#517)" % RMAIN)
    sf = one(fns, "R_SetupFrame")
    if sf is not None:
        leaf = re.search(r"\br_viewleaf\s*=\s*Mod_PointInLeaf\s*\(", sf)
        w = call("R_SetupWorldFrame").search(sf)
        if not leaf or not w or w.start() < leaf.end() or "R_SetWorldFrame" in sf:
            fail("%s: R_SetupFrame must call R_SetupWorldFrame () after it sets r_viewleaf, and not write "
                 "the record itself -- one implementation, so R_Mirror's restore cannot drift (#517)" % RMAIN)

    # --- the light grid: R_SetupGL's and R_Mirror's, nobody else's -----------
    LC = ("R_LightCluster_Update", "R_LightCluster_BindForWorld", "R_LightCluster_BindForAlias")
    sg = one(fns, "R_SetupGL")
    if sg is not None:
        gm = re.search(r"\bGL_GetModelview\s*\(\s*r_world_matrix\s*\)\s*;", sg)
        seq = [call(n).search(sg) for n in LC]
        if not gm or not all(seq) or not (gm.end() < seq[0].start() < seq[1].start() < seq[2].start()):
            fail("%s: R_SetupGL must run R_LightCluster_Update, _BindForWorld and _BindForAlias, in that order, "
                 "once its view and projection are final (after GL_GetModelview (r_world_matrix))" % RMAIN)
    for n in LC:
        k = len(call(n).findall(code))
        if k != 2:
            fail("%s: %s is called %d times; R_SetupGL and R_Mirror's restore are its two callers (#517)"
                 % (RMAIN, n, k))

    # --- R_Mirror: the restore, in order -------------------------------------
    rm = one(fns, "R_Mirror")
    if rm is not None:
        scene = list(call("R_RenderScene").finditer(rm))
        loop = list(re.finditer(r"\bR_RenderBrushPoly\s*\(\s*&\s*r_worldentity\s*,\s*s\s*,\s*true\s*\)\s*;", rm))
        refl = [m.end() for m in re.finditer(r"\b(?:R_DrawParticles|R_DrawTransEntitiesOnList|R_DrawWaterSurfaces)"
                                             r"\s*\([^;]*\)\s*;", rm)]
        load = list(re.finditer(r"\bGL_LoadMatrixf\s*\(\s*r_base_world_matrix\s*\)\s*;", rm))
        scale = [m.end() for m in re.finditer(r"\bGL_Scalef\s*\(", rm)]
        back = {
            "r_viewleaf": re.search(r"\br_viewleaf\s*=\s*\w+\s*;", rm),
            "r_origin": re.search(r"\bVectorCopy\s*\(\s*\w+\s*,\s*r_origin\s*\)\s*;", rm),
            "vpn": re.search(r"\bVectorCopy\s*\(\s*\w+\s*,\s*vpn\s*\)\s*;", rm),
            "vright": re.search(r"\bVectorCopy\s*\(\s*\w+\s*,\s*vright\s*\)\s*;", rm),
            "vup": re.search(r"\bVectorCopy\s*\(\s*\w+\s*,\s*vup\s*\)\s*;", rm),
        }
        if len(scene) != 1 or len(loop) != 1 or len(load) != 1 or not refl or not scale or \
                not all(back.values()):
            fail("%s: R_Mirror is not the shape this pin reads (one R_RenderScene, one mirror-surface "
                 "R_RenderBrushPoly loop, one GL_LoadMatrixf (r_base_world_matrix), the view copied back)"
                 % RMAIN)
        else:
            lo, hi = max([scene[0].end()] + refl + [b.end() for b in back.values()]), loop[0].start()
            after_mats = max(load[0].end(), max(scale))
            want = (
                ("R_SetupWorldFrame ()", call("R_SetupWorldFrame"), lo,
                 "the caustics, from the reflected leaf"),
                ("R_LightCluster_Update ()", call("R_LightCluster_Update"), after_mats,
                 "the dlights, clustered against the reflected view; it reads the modelview and projection, "
                 "so after GL_LoadMatrixf (r_base_world_matrix) and the last GL_Scalef"),
                ("R_LightCluster_BindForWorld ()", call("R_LightCluster_BindForWorld"), after_mats,
                 "the reflected grid's lookup constants"),
                ("R_LightCluster_BindForAlias ()", call("R_LightCluster_BindForAlias"), after_mats,
                 "the reflected grid's lookup constants"),
                ("R_SetFrustum ()", call("R_SetFrustum"), lo, "the reflected frustum"),
                ("memcpy (r_world_matrix, r_base_world_matrix, ...)",
                 re.compile(r"\bmemcpy\s*\(\s*r_world_matrix\s*,\s*r_base_world_matrix\s*,"), lo,
                 "the reflected view matrix"),
            )
            seq = [call(n).search(rm) for n in LC]
            if not all(seq) or not (seq[0].start() < seq[1].start() < seq[2].start()):
                fail("%s: R_Mirror's restore must run R_LightCluster_Update before _BindForWorld and "
                     "_BindForAlias -- the binds upload the last update's lookup constants, so binding first "
                     "hands the mirror the reflected grid's (#517)" % RMAIN)
            for text, rx, floor, why in want:
                hits = list(rx.finditer(rm))
                if len(hits) != 1 or not (floor <= hits[0].start() and hits[0].end() <= hi) or \
                        not at_top(rm, hits[0].start()):
                    fail("%s: R_Mirror must call %s once, unconditionally, after the view is back and ahead "
                         "of the mirror's own surfaces -- else they draw with %s (#517)" % (RMAIN, text, why))

    # --- the counters ---------------------------------------------------------
    for n in ("R_SetupFrame", "R_SetupWorldFrame", "R_SetupGL", "R_RenderScene", "R_Mirror"):
        b = fns.get(n, [""])[0]
        for c, rx in ZERO.items():
            if rx.search(b):
                fail("%s: %s zeroes %s -- it runs for the reflected view too, which threw away the view's "
                     "count and left r_speeds 1 reporting the reflection alone (#517)" % (RMAIN, n, c))
    rv = one(fns, "R_RenderView")
    if rv is not None:
        first = call("R_RenderScene").search(rv)
        for c, rx in ZERO.items():
            hits = list(rx.finditer(rv))
            if len(hits) != 1 or not first or hits[0].start() > first.start() or not at_top(rv, hits[0].start()):
                fail("%s: R_RenderView must zero %s once, unconditionally (not under r_speeds), ahead of the "
                     "view's R_RenderScene -- the one reset both passes count into (#517)" % (RMAIN, c))


def self_test():
    """The pin rejects the pre-#517 shapes, each made from the live source."""
    good = strip((ROOT / RMAIN).read_text())
    restore = re.compile(r"(R_SetFrustum \(\);\s*R_SetupWorldFrame \(\);\s*)(R_LightCluster_Update \(\);\s*"
                         r"R_LightCluster_BindForWorld \(\);\s*R_LightCluster_BindForAlias \(\);)")
    m = restore.search(good)
    variants = {
        "R_SetupFrame zeroes": re.sub(r"(R_SetupWorldFrame \(\);\s*r_cache_thrash)", r"c_brush_polys = 0; \1",
                                      good, count=1),
        "R_RenderView zeroes under r_speeds": re.sub(r"(rprof_texbinds = c_texbinds;\s*)\}(\s*c_brush_polys = 0;)",
                                                     r"\1\2 }", good, count=1),
        "no world-frame restore": re.sub(r"(R_SetFrustum \(\);\s*)R_SetupWorldFrame \(\);", r"\1", good, count=1),
        "no light-grid restore": restore.sub(r"\1", good, count=1),
        "binds before the update": restore.sub(
            r"\1R_LightCluster_BindForWorld (); R_LightCluster_BindForAlias (); R_LightCluster_Update ();",
            good, count=1),
        "no frustum restore": re.sub(r"R_SetFrustum \(\);(\s*R_SetupWorldFrame)", r"\1", good, count=1),
        # The grid moved ahead of the view's matrix: clustered against the
        # reflected one, which is the bug.
        "grid before the matrix": (restore.sub(r"\1", good, count=1).replace(
            "R_SetBlend (true);", m.group(2) + " R_SetBlend (true);", 1) if m else good),
    }
    ok = True
    for name, src in variants.items():
        if src == good:
            print("self-test: %s: the mutation did not apply" % name)
            ok = False
            continue
        del errors[:]
        check(src)
        if not errors:
            print("self-test: %s: NOT caught" % name)
            ok = False
    del errors[:]
    return ok


def main():
    src = ROOT / RMAIN
    if not src.is_file():
        print("test_mirror_view_state: %s is missing" % RMAIN)
        return 1
    if not self_test():
        print("test_mirror_view_state: FAIL (self-test)")
        return 1
    check(strip(src.read_text()))
    if errors:
        for e in errors:
            print("FAIL: " + e)
        return 1
    print("test_mirror_view_state: OK -- R_Mirror re-derives the world frame, the light grid, the view "
          "matrix and the frustum after the reflection; r_speeds' counters reset once, in R_RenderView")
    return 0


if __name__ == "__main__":
    sys.exit(main())
