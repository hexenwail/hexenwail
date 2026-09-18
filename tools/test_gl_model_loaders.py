#!/usr/bin/env python3
"""Pin two gl_model.c loader invariants that have no runtime test coverage.

GH #154: every member of an ALIAS_SKIN_GROUP must be flood-filled through its
own pointer.  Mod_LoadAllSkins used to pass the entry-time `skin` pointer,
which filled skin 0 (or the group header) groupskins times and never touched
members 1..n-1.  The single-skin branch keeps `skin` on purpose -- see the
comment in Mod_LoadAllSkins -- and this test pins that too, so a later
"cleanup" that changes retail skins has to be a deliberate edit here.

GH #128: sprites live on the hunk, so Mod_LoadSpriteModel must set
cache_is_hunk; every reader uses that flag to decide whether cache.data may be
handed to Cache_Check, which dies in Cache_UnlinkLRU on a hunk address.  The
texture-reload loop in Mod_ReloadTextures skips hunk models, so that skip has
to stay alias-only or sprites stop getting their frames re-uploaded.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "engine/h2shared/gl_model.c"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def strip_comments(src):
    """Drop comments and blank out literals, so '{' in either cannot throw
    function()'s brace matching off (Mod_ReloadTextures tests name[0] == '{')."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    src = re.sub(r"'(?:\\.|[^'\\\n])'", "' '", src)
    return re.sub(r'"(?:\\.|[^"\\\n])*"', '""', src)


def require(pattern, source, description):
    if not re.search(pattern, source, re.S):
        raise AssertionError(f"FAIL: {description}")


def forbid(pattern, source, description):
    if re.search(pattern, source, re.S):
        raise AssertionError(f"FAIL: {description}")


def check_skin_groups(text):
    skins = strip_comments(function(
        text, "static void *Mod_LoadAllSkins (int numskins, daliasskintype_t *pskintype, int mdl_flags)\n{"))
    group_start = skins.index("groupskins = LittleLong")
    single, group = skins[:group_start], skins[group_start:]

    loop = group[group.index("for (j = 0; j < groupskins; j++)"):]
    fill = re.search(r"Mod_FloodFillSkin\s*\(([^,]+),", loop)
    if not fill:
        raise AssertionError("FAIL: skin-group loop no longer flood-fills its members")
    arg = re.sub(r"\s+", "", fill.group(1))
    if arg not in ("(byte*)(pskintype)", "(byte*)pskintype"):
        raise AssertionError(
            f"FAIL: skin-group member flood-filled through {fill.group(1).strip()!r}, "
            "not the per-member pskintype pointer (GH #154)")
    require(r"GL_LoadTexture\s*\([^;]*\(byte\s*\*\)\s*\(?pskintype\)?\s*,", loop,
            "flood fill and GL_LoadTexture read the same member buffer")

    require(r"Mod_FloodFillSkin\s*\(\s*skin\s*,", single,
            "single-skin branch keeps the entry-time skin pointer (retail parity, GH #154)")
    print("PASS: skin-group members flood-filled through their own pointer (GH #154)")


def check_sprite_hunk_flag(text):
    sprite = strip_comments(function(
        text, "static void Mod_LoadSpriteModel (qmodel_t *mod, void *buffer)\n{"))
    require(r"Hunk_AllocName\s*\([^;]*\);\s*mod->cache\.data\s*=\s*psprite\s*;\s*mod->cache_is_hunk\s*=\s*true\s*;",
            sprite, "Mod_LoadSpriteModel marks its hunk allocation cache_is_hunk (GH #128)")
    forbid(r"cache_is_hunk\s*=\s*false", sprite,
           "Mod_LoadSpriteModel must never clear cache_is_hunk")

    reload = strip_comments(function(text, "void Mod_ReloadTextures (void)\n{"))
    skip = re.search(r"if\s*\(([^)]*cache_is_hunk[^)]*)\)\s*continue\s*;", reload)
    if not skip:
        raise AssertionError("FAIL: Mod_ReloadTextures lost its hunk-model skip")
    require(r"type\s*==\s*mod_alias", skip.group(1),
            "Mod_ReloadTextures hunk skip is alias-only, so sprites still re-upload (GH #128)")

    # Mod_ClearAll runs on every map change and sprites take its else branch;
    # a Cache_Check added there is exactly the uhexen2-iwdt crash.
    clear = strip_comments(function(text, "void Mod_ClearAll (void)\n{"))
    require(r"if\s*\(\s*mod->type\s*==\s*mod_alias\s*&&\s*!mod->cache_is_hunk\s*\)\s*\{[^}]*Cache_Check",
            clear, "Mod_ClearAll only Cache_Checks cache-backed alias models")
    else_branch = clear[clear.index("else"):]
    forbid(r"Cache_(Check|Free)", else_branch,
           "Mod_ClearAll's else branch (sprites, brushes, hunk alias) must not touch the cache")
    require(r"memset\s*\(\s*mod\s*,\s*0\s*,\s*sizeof\s*\(\s*qmodel_t\s*\)\s*\)", else_branch,
            "Mod_ClearAll zeroes non-cache slots, resetting cache_is_hunk before reuse")
    print("PASS: sprites flagged cache_is_hunk and still reloaded (GH #128)")


def main():
    # Optional path argument, so the test can be pointed at an older copy of
    # gl_model.c to prove it fails there.
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else SOURCE
    text = strip_comments(source.read_text())
    check_skin_groups(text)
    check_sprite_hunk_flag(text)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print(exc)
        sys.exit(1)
