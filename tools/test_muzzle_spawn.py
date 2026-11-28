#!/usr/bin/env python3
"""sv_muzzlespawn: every player missile goes through MuzzleFix, and at the
default the vanilla code is all that runs (GitHub #567).

muzzle.hc, in both shipped trees (gamecode/hc/h2 and gamecode/hc/portals), moves a player missile from eye height to the
weapon's muzzle when the engine cvar sv_muzzlespawn is 1.  The contract that
keeps the default byte-for-byte vanilla is spread across many files, so this
pins each piece of it in the source:

  - MuzzleFix's first statement returns when cvar("sv_muzzlespawn") is 0, so
    with the cvar at its default it reads and writes nothing, and its second
    returns for non-clients (the tornado and eidolon share the fire code);
  - each player weapon spawn site still builds the missile with Raven's own
    expression (the table in the issue), and calls MuzzleFix after it;
  - no proj_ofs-relative spawn in a player weapon file escapes the list: a
    new or missed site must be added to SITES or, with its reason, to ALLOWED;
  - MuzzleFix never makes two builtin calls in one statement (hcc reuses
    the return slot, so the first result is lost);
  - muzzle.hc is compiled into every image of its tree ahead of the weapons,
    and the two trees' copies are identical;
  - the engine registers the cvar archived with default "0", and the Game
    menu row is what sets it;
  - the necromancer's spellbook spells (magic missile, bone shards) leave
    from the casting hand.  spllbook.mdl holds the book left of centre and
    the hand right of it; the first cut took the model's forward-most vertex,
    which is the book's corner, so the shots came off the book (4 to 9 units
    LEFT and 15 down).  In every fire frame the hand is right of centre and
    never lower than 14 units below the camera, and the book is never higher
    than that (tools/muzzle_sheet.py draws it), so a book-spell muzzle must
    have right > 0 and up > -12.

Run with a directory argument to check another tree, e.g. an export of the
pre-fix master, which fails.
"""
from pathlib import Path
import re
import sys

ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
errors = []

# (file, function, Raven's spawn expression, MuzzleFix calls in the function)
SITES = [
    ("axe.hc", "launch_axe", "self.origin+self.proj_ofs+v_forward*10+v_right*1", 1),
    ("purifier.hc", "launch_pmissile2", "self.origin+self.proj_ofs+v_forward*10", 1),
    ("purifier.hc", "launch_pmissile1", "self.origin+self.proj_ofs+v_forward*6+v_right*10", 2),
    ("purifier.hc", "launch_pmissile1", "self.origin+self.proj_ofs+v_forward*6-v_right*10", 2),
    ("vorpal.hc", "launch_vorpal_missile", "self.origin+self.proj_ofs)", 1),
    ("warhamer.hc", "ThrowHammer", "self.origin+self.proj_ofs+v_forward*FL_SWIM", 1),
    ("icemace.hc", "FireFreeze", "self.origin+self.proj_ofs+v_forward*8", 1),
    ("icemace.hc", "FireBlizzard", "self.origin+self.proj_ofs+v_forward*16", 1),
    ("meteor.hc", "FireMeteor", "self.origin+self.proj_ofs+v_forward*12", 1),
    ("magicmis.hc", "FireMagicMissile", "self.origin+self.proj_ofs+v_forward*8+v_right*7+'005'", 1),
    ("boner.hc", "bone_fire", "self.origin+self.proj_ofs+v_forward*8+v_right*(ofs_y+12)+v_up*ofs_z", 2),
    ("ravenstf.hc", "launch_superraven", "self.origin+self.proj_ofs+v_forward*10", 1),
    ("ravenstf.hc", "launch_set", "self.origin+self.proj_ofs+v_forward*14", 0),
    ("ravenstf.hc", "create_raven_shot1", "setorigin(missile,location)", 1),
    ("assgren.hc", "ThrowMiniGrenade", "self.origin+self.proj_ofs+v_forward*8+v_right*8", 1),
    ("assgren.hc", "ThrowMultiGrenade", "self.origin+self.proj_ofs+v_forward*8+v_right*8", 1),
    ("crossbow.hc", "FireCB_Bolt", "self.origin+self.proj_ofs+v_forward*8", 1),
    ("setstaff.hc", "TheOldBallAndChain", "self.origin+self.proj_ofs+v_forward*12", 1),
    ("setstaff.hc", "Drilla", "self.origin+self.proj_ofs+v_forward*8", 1),
]

# proj_ofs-relative positions in the weapon files that are not missile spawns.
ALLOWED = {
    ("gauntlet.hc", "gauntlet_fire"): "melee trace start",
    ("vorpal.hc", "vorpal_melee"): "melee trace start",
    ("warhamer.hc", "warhammer_fire"): "melee trace start",
    ("warhamer.hc", "test_traceline"): "debug trace, never called",
    ("sickle.hc", "sickle_fire"): "melee trace start",
    ("punchdgr.hc", "fire_punchdagger"): "melee trace start",
    ("sunstaff.hc", "FireSunstaff"): "hitscan beam, no missile",
    ("boner.hc", "InstantDamage"): "hitscan, no missile",
    ("magicmis.hc", "FireFlash"): "hand flash effect, not a missile",
    ("ravenstf.hc", "create_raven"): "self is the raven missile, not the player",
    ("ravenstf.hc", "ravenstaff_fire"): "smoke puffs, not a missile",
}

# Portal of Praevus: the same weapons (near-copies of h2's), plus the Demoness.
SITES_PORTALS = SITES + [
    ("bldrain.hc", "FireBloodMissile",
     "self.origin+self.proj_ofs+v_forward*f_dist-v_right*12+v_right*(offset*3)-'006'", 1),
    ("acidorb.hc", "FireAcidBlob", "self.origin+self.proj_ofs+v_forward*8-v_right*12-'003'", 1),
    ("acidorb.hc", "FireAcidMissile",
     "self.origin+self.proj_ofs+v_forward*8-v_right*12+v_right*(offset*3)+'002'", 1),
    ("flameorb.hc", "flamestream_fire", "self.origin+self.proj_ofs+v_forward*16-v_right*16-v_up*16", 1),
    ("flameorb.hc", "flameswarm_fire", "self.origin+self.proj_ofs+v_forward*16-v_right*24-v_up*16", 1),
    ("lightwp.hc", "FireLightningBall", "self.origin+self.proj_ofs+v_forward*10", 1),
]
ALLOWED_PORTALS = {k: v for k, v in ALLOWED.items() if k != ("warhamer.hc", "test_traceline")}
ALLOWED_PORTALS.update({
    ("lightwp.hc", "shebitch_chain_lightning_strike"): "hitscan lightning, no missile",
})
TREES = [
    ("h2", SITES, ALLOWED, ("progs.src", "progs2.src")),
    ("portals", SITES_PORTALS, ALLOWED_PORTALS, ("progs.src",)),
]


def squash(s):
    return re.sub(r"\s+", "", s)


def functions(text):
    """name -> body, for both `void name (...) {` and `void() name = {`."""
    out = {}
    args = r"\((?:[^()]|\([^()]*\))*\)"	# one level of nesting: `void() nextfunc'
    head = re.compile(r"^\s*(?:void|float|vector|entity|string)\s*(?:%s\s*(\w+)\s*=|(\w+)\s*%s)\s*\n?\s*\{" % (args, args),
                      re.M)
    for m in head.finditer(text):
        name = m.group(1) or m.group(2)
        i, depth = m.end(), 1
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        out.setdefault(name, text[m.end():i - 1])
    return out


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def check_tree(tree, SITES, ALLOWED, srcs):
    TREE = ROOT / "gamecode/hc" / tree
    WEAPON_FILES = sorted({f for f, *_ in SITES} | {f for f, _ in ALLOWED})
    errors = []
    src = {}
    for f in WEAPON_FILES + ["muzzle.hc"]:
        p = TREE / f
        if not p.exists():
            errors.append(f"{p.relative_to(ROOT)} is missing")
            continue
        src[f] = strip_comments(p.read_text(encoding="latin-1"))

    if "muzzle.hc" in src:
        fns = functions(src["muzzle.hc"])
        body = fns.get("MuzzleFix")
        if body is None:
            errors.append("muzzle.hc defines no MuzzleFix")
        else:
            stmts = [s for s in squash(body).split(";") if s and not s.startswith("local")]
            if not stmts or not stmts[0].startswith('if(!cvar("sv_muzzlespawn"))return'):
                errors.append("MuzzleFix must start with `if (!cvar(\"sv_muzzlespawn\")) return;`, "
                              "so the default reads and writes nothing")
            elif len(stmts) < 2 or not stmts[1].startswith("if(!(shooter.flags&FL_CLIENT))return"):
                errors.append("MuzzleFix's second statement must return for non-clients: the tornado "
                              "and eidolon reach the same fire functions and must stay vanilla")
            # hcc leaves every builtin's result in one shared return slot, so a
            # second builtin call in the same statement overwrites the first:
            # `vlen (a) <= 2 * vlen (b)' compares b with itself.  That bug shipped
            # in the first cut of this file and turned the feature off silently.
            for st in stmts:
                calls = re.findall(r"\b(vlen|normalize|vectoangles|cvar|vectoyaw)\(", st)
                if len(calls) > 1:
                    errors.append(f"MuzzleFix: two builtin calls in one statement ({', '.join(calls)}): `{st}`")
        if re.search(r"^\s*\.\w", src["muzzle.hc"], re.M):
            errors.append("muzzle.hc declares an entity field; that changes the savegame field set")

    for f, fn, expr, calls in SITES:
        if f not in src:
            continue
        body = functions(src[f]).get(fn)
        if body is None:
            errors.append(f"{f}: no function {fn}")
            continue
        flat = squash(body)
        at = flat.find(squash(expr))
        if at < 0:
            errors.append(f"{f}:{fn}: Raven's spawn expression `{expr}` is gone; the default path must stay vanilla")
            continue
        for call in re.findall(r"MuzzleFix\(([\w.]+),([\w.]+),", flat):
            if call[1] != "self":
                errors.append(f"{f}:{fn}: MuzzleFix's shooter must be self, got {call[1]}")
        got = flat.count("MuzzleFix(")
        if got != calls:
            errors.append(f"{f}:{fn}: {got} MuzzleFix calls, expected {calls}")
        elif calls and "MuzzleFix(" not in flat[at:]:
            errors.append(f"{f}:{fn}: MuzzleFix must come after the vanilla spawn")
        elif calls:
            # The spawn expression is sometimes only stored (`org=...',
            # `o_angle=...') and the setorigin comes later.  Nothing may set
            # the missile's origin or velocity after the last MuzzleFix, or it
            # undoes the move and leaves a re-aimed shot at the eye.
            tail = flat[flat.rindex("MuzzleFix("):]
            for m in set(re.findall(r"MuzzleFix\(([\w.]+),", flat)):
                if re.search(r"setorigin\(%s," % re.escape(m), tail) or \
                        re.search(r"(?<![\w.])%s\.velocity=" % re.escape(m), tail):
                    errors.append(f"{f}:{fn}: {m}'s origin or velocity is set after MuzzleFix")

    seen_book = set()
    pinned = {(f, fn) for f, fn, *_ in SITES}
    spawn = re.compile(r"self\.origin\s*\+\s*self\.proj_ofs")
    seen = set()
    for f in WEAPON_FILES:
        if f not in src:
            continue
        for fn, body in functions(src[f]).items():
            if not spawn.search(body):
                continue
            seen.add((f, fn))
            if (f, fn) not in pinned and (f, fn) not in ALLOWED:
                errors.append(f"{f}:{fn} positions something at self.origin + self.proj_ofs; "
                              "pin it in SITES (and call MuzzleFix) or say why in ALLOWED")
    # spllbook.mdl: muzzle must be on the casting hand (right of centre, up
    # -10..+1), not the book (up -14 and below).  No forward bound: the
    # fingertips reach 29 units forward in fire6.
    for f in ("magicmis.hc", "boner.hc"):
        if f not in src:
            continue
        for line in strip_comments(src[f]).splitlines():
            m = re.search(r"MuzzleFix\s*\(\s*\w+\s*,\s*self\s*,\s*'(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)'", line)
            if not m:
                continue
            _fwd, right, up = map(float, m.groups())
            if right <= 0 or up <= -12:
                errors.append(f"{f}: spellbook muzzle '{' '.join(m.groups())}' is on the book, "
                              "not the casting hand (want right > 0, up > -12)")
            seen_book.add(f)
    for f, fn in sorted(set(ALLOWED) - seen):
        errors.append(f"ALLOWED names {f}:{fn}, which no longer uses self.origin + self.proj_ofs")

    for f in ("magicmis.hc", "boner.hc"):
        if f in src and f not in seen_book:
            errors.append(f"{f}: no spellbook MuzzleFix call with a literal muzzle to check")

    for name in srcs:
        p = TREE / name
        lines = [l.split("//")[0].strip() for l in p.read_text(encoding="latin-1").splitlines()]
        if "muzzle.hc" not in lines:
            errors.append(f"{name} does not compile muzzle.hc")
        elif lines.index("muzzle.hc") > lines.index("gauntlet.hc"):
            errors.append(f"{name}: muzzle.hc must come before the weapon files")
    return [f"{tree}: {e}" for e in errors], src.get("muzzle.hc")


copies = {}
for tree, sites, allowed, srcs in TREES:
    errs, copies[tree] = check_tree(tree, sites, allowed, srcs)
    errors += errs
if None not in copies.values() and len(set(copies.values())) != 1:
    errors.append("gamecode/hc/h2/muzzle.hc and gamecode/hc/portals/muzzle.hc differ (beyond comments); keep them identical")

sv = (ROOT / "engine/hexen2/sv_main.c").read_text()
if not re.search(r'\{\s*"sv_muzzlespawn"\s*,\s*"0"\s*,\s*CVAR_ARCHIVE\s*\}', sv):
    errors.append("sv_main.c: sv_muzzlespawn must be registered archived with default \"0\" (vanilla)")
if "Cvar_RegisterVariable (&sv_muzzlespawn);" not in sv:
    errors.append("sv_main.c: SV_Init does not register sv_muzzlespawn")
menu = (ROOT / "engine/hexen2/menu.c").read_text()
case = re.search(r"case GAME_MUZZLE:(.*?)break;", menu, re.S)
if not case or 'Cvar_SetValue ("sv_muzzlespawn"' not in case.group(1):
    errors.append("menu.c: the GAME_MUZZLE row must set sv_muzzlespawn")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print(f"PASS {len(SITES)} base-game and {len(SITES_PORTALS)} Portals player missile spawns go through "
      "MuzzleFix, vanilla when sv_muzzlespawn is 0 (#567)")
