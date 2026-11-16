#!/usr/bin/env python3
"""A held entity keeps its lerps: the reset is not keyed on msgtime (GitHub #494).

Hexen II's server delta-codes entities against the reference frame the client
acknowledged and skips one that has not changed (SV_WriteEntitiesToClient,
`if (!bits && FoundInList) continue;`).  The client keeps it alive through
BE_ON.  So an entity missing from the last message is usually held, not lost.
CL_RelinkEntities still had Quake's test,
`if (ent->forcelink || ent->msgtime != cl.mtime[0])`, which set
LERP_RESETMOVE | LERP_RESETANIM for every held entity, and CL_ParseUpdate's
`forcelink` (`msgtime != cl.mtime[1]`) fired for anything held in the message
before.  With r_lerpmodels 1 the chase-cam player and every held animating
model snapped to the target pose and never blended.

Checked here:
  engine/hexen2/cl_main.c, CL_RelinkEntities --
  * no LERP_RESETANIM / LERP_RESETMOVE write (|= or =, ternaries included)
    sits under a condition that holds for a stale msgtime (any comparison
    with cl.mtime[0] but ==, or the else side of ==), except the drop block,
    the one that clears ->model;
  * the drop block (stale msgtime and no BE_ON) sets ent->forcelink = true
    and never clears it,
    so a dropped entity that comes back is treated as new;
  * the first branch after `VectorCopy (ent->origin, oldorg);` is exactly
    `if (ent->forcelink)`, and it resets both lerps;
  * a held branch, `else if (ent->msgtime != cl.mtime[0])`, puts the entity at
    msg_origins[0] / msg_angles[0], comes before the msg_origins[1] -> [0]
    frac lerp, and touches no lerp flag;
  * the step-mover teleport test only reads a fresh pair: the loop that sets
    `teleport = true` is gated on `ent->msgtime == cl.mtime[0]`.
  engine/hexen2/cl_parse.c, CL_ParseUpdate --
  * no `msgtime != cl.mtime[1]` test at all, and no lerp reset under any
    condition that reads msgtime;
  * forcelink is `(ent->model == NULL || ent->forcelink)`, computed after the
    U_CLEAR_ENT memset and before msgtime and ->model are overwritten;
  * a model change sets LERP_RESETANIM;
  * nothing assigns forcelink from a msgtime test.
  engine/hexen2/cl_parse.c, CL_ParseServerMessage --
  * svc_clear_edicts sets forcelink on the entity it removes (a re-send in the
    same client frame, before any relink drops it, is new);
  * svc_reference sets LERP_RESETANIM when it restores a different ->model.
  engine/hexen2/cl_hw.c, HWCL_CopyEntity -- why HexenWorld needed no change:
  * it stamps ent->msgtime = cl.mtime[0] unconditionally for every active
    entity on every pass, and sets forcelink only on !was_on || model_changed.
  engine/hexen2/gl_rmain.c, R_AliasResolveLerp --
  * the stale-threshold reset (uhexen2-43f8) is still there.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


def body_of(text, signature):
    """The brace-balanced body of the first function whose header matches."""
    m = re.search(signature, text)
    if not m:
        return None
    i = text.index("{", m.end() - 1)
    depth = 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[i:j + 1]
    return None


def match_paren_back(text, close):
    """Index of the '(' matching the ')' at `close`."""
    depth = 0
    for k in range(close, -1, -1):
        depth += {")": 1, "(": -1}.get(text[k], 0)
        if depth == 0:
            return k
    return -1


def match_brace_back(text, close):
    depth = 0
    for k in range(close, -1, -1):
        depth += {"}": 1, "{": -1}.get(text[k], 0)
        if depth == 0:
            return k
    return -1


def header(text, pos):
    """What controls the statement or block starting at `pos`.
    Returns (kind, cond, start) with kind 'if', 'elif', 'else', 'loop' or None."""
    before = text[:pos].rstrip()
    if before.endswith(")"):
        op = match_paren_back(text, len(before) - 1)
        kw = re.search(r"(else\s+if|if|for|while)\s*$", text[:op])
        if kw:
            cond = text[op + 1:len(before) - 1]
            k = kw.group(1)
            kind = "elif" if k.startswith("else") else ("if" if k == "if" else "loop")
            return kind, cond, kw.start()
    m = re.search(r"\belse\s*$", before)
    if m:
        return "else", None, m.start()
    return None, None, pos


def unbraced_arm(text, semi):
    """For an unbraced `if (c) stmt;` / `else if (c) stmt;` ending at the ';' at
    `semi`: (kind, cond, start), else (None, None, semi)."""
    st = max(text.rfind(";", 0, semi), text.rfind("{", 0, semi), text.rfind("}", 0, semi)) + 1
    st += len(text[st:semi]) - len(text[st:semi].lstrip())
    m = re.match(r"(else\s+if|if)\s*\(", text[st:])
    if not m:
        return None, None, semi
    op = st + m.end() - 1
    depth = 0
    for k in range(op, semi):
        depth += {"(": 1, ")": -1}.get(text[k], 0)
        if depth == 0:
            return ("elif" if m.group(1) != "if" else "if"), text[op + 1:k], st
    return None, None, semi


def chain_conds(text, start):
    """Conditions of the earlier arms of the if/else chain whose `else` begins at
    `start`; every one of them is false on this arm."""
    out = []
    while True:
        before = text[:start].rstrip()
        if before.endswith("}"):
            kind, cond, start = header(text, match_brace_back(text, len(before) - 1))
        elif before.endswith(";"):
            kind, cond, start = unbraced_arm(text, len(before) - 1)
        else:
            return out
        if kind not in ("if", "elif"):
            return out
        out.append(cond)
        if kind == "if":
            return out


def guards(text, pos):
    """(cond, negated) for every condition that must hold for the statement at
    `pos` to run, innermost first, walking out through enclosing blocks."""
    out = []
    kind, cond, hstart = header(text, pos)          # unbraced `if (x) stmt;`
    if kind in ("if", "elif", "loop"):
        out.append((cond, False))
    if kind in ("elif", "else"):
        out += [(c, True) for c in chain_conds(text, hstart)]
    depth = 0
    for k in range(pos, -1, -1):
        ch = text[k]
        if ch == "}":
            depth += 1
        elif ch == "{":
            if depth:
                depth -= 1
                continue
            kind, cond, hstart = header(text, k)
            if kind in ("if", "elif", "loop"):
                out.append((cond, False))
            if kind in ("elif", "else"):
                out += [(c, True) for c in chain_conds(text, hstart)]
    return out


STALE = re.compile(r"ent->msgtime\s*(?:!=|<=?|>=?)\s*cl\.mtime\[0\]|cl\.mtime\[0\]\s*(?:!=|<=?|>=?)\s*ent->msgtime")
FRESH = re.compile(r"ent->msgtime\s*==\s*cl\.mtime\[0\]|cl\.mtime\[0\]\s*==\s*ent->msgtime")


RESET = re.compile(r"\bent->lerpflags\s*\|?=(?!=)[^;]*\bLERP_RESET(?:ANIM|MOVE)\b")


def ternary_conds(text, pos):
    """`c` of a `x = c ? ... : ...` statement starting at `pos`."""
    stmt = text[pos:text.index(";", pos)]
    m = re.search(r"\|?=(?!=)\s*\(?(.*?)\)?\s*\?", stmt)
    return [m.group(1)] if m else []


def stale_guard(gs):
    return [c for c, neg in gs if (STALE.search(c) and not neg) or (FRESH.search(c) and neg)]


main = strip((ROOT / "engine/hexen2/cl_main.c").read_text())
parse = strip((ROOT / "engine/hexen2/cl_parse.c").read_text())
hw = strip((ROOT / "engine/hexen2/cl_hw.c").read_text())
rmain = strip((ROOT / "engine/hexen2/gl_rmain.c").read_text())

# -- CL_RelinkEntities ----------------------------------------------------
rel = body_of(main, r"static void CL_RelinkEntities \(void\)\s*\{")
if rel is None:
    errors.append("CL_RelinkEntities: definition not found")
else:
    drop = re.search(r"if \(ent->msgtime != cl\.mtime\[0\] && !\(ent->baseline\.flags & BE_ON\)\)\s*\{", rel)
    if not drop:
        errors.append("CL_RelinkEntities: the drop test (stale msgtime and no BE_ON) not found")
        drop_body = ""
        drop_span = (0, 0)
    else:
        drop_body = body_of(rel[drop.start():], re.escape(drop.group(0)))
        drop_span = (drop.start(), drop.start() + len(drop.group(0)) + len(drop_body))
        if not re.search(r"\bent->model\s*=\s*NULL\s*;", drop_body):
            errors.append("CL_RelinkEntities: the drop block must clear ent->model")
        if (not re.search(r"\bent->forcelink\s*=\s*true\s*;", drop_body) or
                re.search(r"\bent->forcelink\s*=\s*(?:false|0)\s*;", drop_body)):
            errors.append("CL_RelinkEntities: the drop block must set ent->forcelink = true, or an entity that "
                          "comes back is lerped from where it left (#494)")

    sites = [m.start() for m in RESET.finditer(rel)]
    if not sites:
        errors.append("CL_RelinkEntities: no lerp reset site found")
    for s in sites:
        if drop_span[0] <= s < drop_span[1]:
            continue
        bad = stale_guard(guards(rel, s) + [(c, False) for c in ternary_conds(rel, s)])
        if bad:
            line = rel[:s].count("\n")
            errors.append(f"CL_RelinkEntities: a lerp reset (body line {line}) runs for a stale msgtime "
                          f"[{bad[0].strip()}]: a held entity is merely unchanged, not new (#494)")

    anchor = re.search(r"VectorCopy \(ent->origin, oldorg\);\s*", rel)
    if not anchor:
        errors.append("CL_RelinkEntities: `VectorCopy (ent->origin, oldorg);` not found")
    else:
        after = rel[anchor.end():]
        first = re.match(r"if \(([^{]*)\)\s*\{", after)
        if not first or first.group(1).strip() != "ent->forcelink":
            errors.append("CL_RelinkEntities: the first branch after oldorg must be exactly `if (ent->forcelink)`")
        else:
            fb = body_of(after, re.escape(first.group(0)))
            if not re.search(r"lerpflags\s*\|=\s*LERP_RESETMOVE\s*\|\s*LERP_RESETANIM\s*;", fb):
                errors.append("CL_RelinkEntities: the forcelink branch must reset both lerps")

    held = re.search(r"else if \(\s*(?:ent->msgtime\s*!=\s*cl\.mtime\[0\]|cl\.mtime\[0\]\s*!=\s*ent->msgtime)\s*\)\s*\{", rel)
    frac = re.search(r"ent->origin\[j\]\s*=\s*ent->msg_origins\[1\]\[j\]\s*\+", rel)
    if not held:
        errors.append("CL_RelinkEntities: no held branch `else if (ent->msgtime != cl.mtime[0])`")
    else:
        hb = body_of(rel[held.start():], re.escape(held.group(0)))
        if not (re.search(r"VectorCopy \(ent->msg_origins\[0\], ent->origin\);", hb) and
                re.search(r"VectorCopy \(ent->msg_angles\[0\], ent->angles\);", hb)):
            errors.append("CL_RelinkEntities: the held branch must put the entity at msg_origins[0] / msg_angles[0]")
        if re.search(r"lerpflags|msg_origins\[1\]|\bfrac\b", hb):
            errors.append("CL_RelinkEntities: the held branch must not touch lerp flags or lerp msg_origins[1] by frac")
        if not frac or frac.start() < held.start():
            errors.append("CL_RelinkEntities: the held branch must come before the msg_origins[1] -> [0] frac lerp")

    tele = re.search(r"for \(([^)]*)\)\s*\{[^{}]*\bteleport\s*=\s*true\s*;", rel)
    if not tele or not FRESH.search(tele.group(1)):
        errors.append("CL_RelinkEntities: the step-mover teleport loop must be gated on "
                      "ent->msgtime == cl.mtime[0]; a held pair would reset the lerps every frame")

# -- CL_ParseUpdate -------------------------------------------------------
pu = body_of(parse, r"static void CL_ParseUpdate \(int bits\)\s*\{")
if pu is None:
    errors.append("CL_ParseUpdate: definition not found")
else:
    if re.search(r"msgtime\s*!=\s*cl\.mtime\[1\]|cl\.mtime\[1\]\s*!=\s*\w+->msgtime", pu):
        errors.append("CL_ParseUpdate: forcelink must not come from msgtime != cl.mtime[1]; "
                      "missing from the last message means held, not new (#494)")
    fl = re.search(r"\bforcelink\s*=\s*\(?\s*(?:ent->model\s*==\s*NULL|!\s*ent->model)\s*\|\|\s*ent->forcelink\s*\)?\s*;", pu)
    clear = re.search(r"if \(bits & U_CLEAR_ENT\)", pu)
    stamp = re.search(r"\bent->msgtime\s*=\s*cl\.mtime\[0\]\s*;", pu)
    setmodel = re.search(r"\bent->model\s*=\s*model\s*;", pu)
    if not fl:
        errors.append("CL_ParseUpdate: forcelink must be `(ent->model == NULL || ent->forcelink)`")
    elif not (clear and stamp and setmodel and clear.start() < fl.start() < stamp.start() < setmodel.start()):
        errors.append("CL_ParseUpdate: forcelink must be computed after the U_CLEAR_ENT memset and before "
                      "msgtime and ->model are overwritten")
    for m in list(RESET.finditer(pu)) + list(re.finditer(r"\b(?:ent->)?forcelink\s*=(?!=)[^;]*;", pu)):
        bad = [c for c, _ in guards(pu, m.start()) + [(c, 0) for c in ternary_conds(pu, m.start())]
               if re.search(r"\bmsgtime\b", c)]
        if re.search(r"\bmsgtime\b", pu[m.start():pu.index(";", m.start())]):
            bad.append(pu[m.start():pu.index(";", m.start())])
        if bad:
            errors.append(f"CL_ParseUpdate: a lerp reset or forcelink keyed on msgtime [{bad[0].strip()}]: an entity the "
                          "server held is not stale (#494)")
    mc = re.search(r"if \(model != ent->model\)\s*\{", pu)
    if not mc or not re.search(r"lerpflags\s*\|=\s*LERP_RESETANIM\s*;",
                               body_of(pu[mc.start():], re.escape(mc.group(0))) or ""):
        errors.append("CL_ParseUpdate: a model change must set LERP_RESETANIM")

psm = body_of(parse, r"void CL_ParseServerMessage \(void\)\s*\{")
if psm is None:
    errors.append("CL_ParseServerMessage: definition not found")
else:
    ref = re.search(r"case svc_reference:", psm)
    clr = re.search(r"case svc_clear_edicts:", psm)
    nxt = re.search(r"case \w+:", psm[clr.end():]) if clr else None
    ref_txt = psm[ref.start():clr.start()] if ref and clr else ""
    clr_txt = psm[clr.start():clr.end() + nxt.start()] if clr and nxt else ""
    if not re.search(r"if \(ent->model != model\)\s*ent->lerpflags\s*\|=\s*LERP_RESETANIM\s*;\s*ent->model\s*=\s*model\s*;",
                     ref_txt):
        errors.append("svc_reference: restoring a different ->model from the reference frame must set LERP_RESETANIM, "
                      "or a held entity keeps the other model's pose indices")
    if not re.search(r"ent->baseline\.flags\s*&=\s*~BE_ON\s*;\s*ent->forcelink\s*=\s*true\s*;", clr_txt):
        errors.append("svc_clear_edicts: a removed entity must get forcelink = true, or one re-sent before a relink "
                      "drops it lerps from where it left")

# -- HexenWorld: why it needed no change ----------------------------------
ce = body_of(hw, r"static void HWCL_CopyEntity \(int entitynum, const hwcl_entity_state_t \*state,\s*"
                 r"qboolean player, qboolean new_update\)\s*\{")
if ce is None:
    errors.append("HWCL_CopyEntity: definition not found")
else:
    st = re.search(r"\bent->msgtime\s*=\s*cl\.mtime\[0\]\s*;", ce)
    if not st or guards(ce, st.start()) or sum({"{": 1, "}": -1}.get(ch, 0) for ch in ce[:st.start()]) != 1:
        errors.append("HWCL_CopyEntity: must stamp ent->msgtime = cl.mtime[0] unconditionally; "
                      "CL_RelinkEntities' held branch would otherwise freeze HexenWorld entities")
    fls = re.findall(r"if \(([^)]*(?:\)[^)]*)?)\)\s*ent->forcelink\s*=\s*true\s*;", ce)
    if fls != ["!was_on || model_changed"] or len(re.findall(r"\bforcelink\b", ce)) != 1:
        errors.append("HWCL_CopyEntity: forcelink must be set only for !was_on || model_changed")

# -- uhexen2-43f8's stale-threshold reset stays ---------------------------
res = body_of(rmain, r"static void R_AliasResolveLerp \(entity_t \*e, aliashdr_t \*paliashdr,\s*"
                     r"int \*out_pose1, int \*out_pose2, float \*out_blend\)\s*\{")
if res is None or not re.search(r"\(e->lerpflags & LERP_RESETANIM\)\s*\|\|\s*e->lerpstart == 0\.0f\s*\|\|\s*"
                                r"cl\.time - e->lerpstart > stale_threshold", res):
    errors.append("R_AliasResolveLerp: the LERP_RESETANIM / lerpstart 0 / stale-threshold reset (uhexen2-43f8) "
                  "must stay")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS a held entity keeps its lerps; only a real (re)appearance resets them (#494)")
