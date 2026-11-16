#!/usr/bin/env python3
"""The CPU alias glow draws at the base pass's blend, resolved once (GitHub #493).

R_AliasResolveLerp used to skip the blend while model_fullbright_pass was set,
so on the CPU path (r_alias_gpu 0, and every MD5 model) the glow layer was
built from currentpose alone while the skin under it was still mid-blend.  The
instanced path lerps both passes, so the two paths disagreed mid-lerp.  The
resolver also ran its state machine a second time for the glow -- harmless
today only because nothing sets LERP_RESETANIM2, whose branch leaves lerpstart
0 for a second call to reset.

Checked here, in engine/hexen2/gl_rmain.c:
  * R_AliasResolveLerp does not read model_fullbright_pass: the blend it
    returns is the same whichever pass asks;
  * R_SetupAliasFrame takes the resolved pair and blend as parameters, never
    resolves them itself, and hands exactly those to GL_DrawAliasSkeletal
    (MD5) and GL_DrawAliasFrame (MDL, and MD3 through GL_DrawAliasFrameMD3);
  * R_DrawAliasModel calls R_AliasResolveLerp exactly once, as a statement of
    its own at the function's top level (no condition can skip it), into three
    locals nothing else writes, before its first R_SetupAliasFrame, with no
    return between it and the glow pass; and it calls
    R_SetupAliasFrame exactly twice, the base pass and the fullbright pass
    (the second between model_fullbright_pass = true and = false), each with
    those three locals in the resolver's order;
  * R_SetupAliasFrame has no other caller, and model_fullbright_pass is read
    only by the three draw functions, where it picks the vertex colour --
    never a pose;
  * the instanced path also resolves once per entity: R_CollectAliasInstance
    calls R_AliasResolveLerp once (test_alias_inst_vert_block.py pins its
    glow pass drawing the same records).
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


def functions(text):
    """(name, body) for every top-level function definition."""
    out = []
    for m in re.finditer(r"^(?:static\s+)?[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{}]*?)\)\s*\{", text, re.M):
        body = body_of(text[m.start():], re.escape(m.group(0)))
        if body:
            out.append((m.group(1), body))
    return out


def writes_of(name, text):
    """Every place `name` is written: any assignment operator, ++/-- either
    side, or its address taken (a unary &, not the && operator)."""
    return re.findall(rf"(?<![=!<>])\b{name}\s*(?:(?:[-+*/%&|^]|<<|>>)?=(?!=)|\+\+|--)"
                      rf"|(?:\+\+|--)\s*{name}\b"
                      rf"|(?<![&\w)\]\s])\s*&\s*{name}\b", text)


rmain = strip((ROOT / "engine/hexen2/gl_rmain.c").read_text())

# -- the resolver is pass-agnostic ---------------------------------------
resolve = body_of(rmain, r"static void R_AliasResolveLerp \(entity_t \*e, aliashdr_t \*paliashdr,\s*"
                         r"int \*out_pose1, int \*out_pose2, float \*out_blend\)\s*\{")
if resolve is None:
    errors.append("R_AliasResolveLerp: definition not found")
elif re.search(r"\bmodel_fullbright_pass\b", resolve):
    errors.append("R_AliasResolveLerp must not read model_fullbright_pass: the glow would draw at a "
                  "different blend from the skin it sits on (#493)")

# -- R_SetupAliasFrame draws what it is handed ---------------------------
sig = re.search(r"static void R_SetupAliasFrame \(entity_t \*e, aliashdr_t \*paliashdr,\s*"
                r"int (\w+), int (\w+), float (\w+)\)\s*\{", rmain)
if not sig:
    errors.append("R_SetupAliasFrame must take the resolved (prevpose, pose, blend) as parameters")
else:
    prev_p, pose_p, blend_p = sig.groups()
    setup = body_of(rmain, re.escape(sig.group(0)))
    if re.search(r"\bR_AliasResolveLerp\s*\(", setup):
        errors.append("R_SetupAliasFrame must not resolve the lerp itself; R_DrawAliasModel resolves once for both passes")
    for name in (prev_p, pose_p, blend_p):
        if writes_of(name, setup):
            errors.append(f"R_SetupAliasFrame must not rewrite its {name} parameter")
    if re.search(r"\bmodel_fullbright_pass\b|e->(?:currentpose|previouspose|lerpstart)", setup):
        errors.append("R_SetupAliasFrame must pick its poses from its parameters alone, "
                      "not model_fullbright_pass or the entity's lerp state")
    if not re.search(rf"GL_DrawAliasSkeletal \(e, paliashdr, {pose_p}, {prev_p}, {blend_p}\);", setup):
        errors.append("R_SetupAliasFrame: the MD5 (PV_IQM) arm must draw the pair and blend it was handed")
    if not re.search(rf"GL_DrawAliasFrame\s*\(e, paliashdr, {pose_p}, {prev_p}, {blend_p}\);", setup):
        errors.append("R_SetupAliasFrame: the MDL/MD3 mid-blend arm must draw the pair and blend it was handed")

# -- R_DrawAliasModel resolves once, both passes take it -----------------
dam = body_of(rmain, r"static void R_DrawAliasModel \(entity_t \*e\)\s*\{")
if dam is None:
    errors.append("R_DrawAliasModel: definition not found")
else:
    res = re.findall(r"\bR_AliasResolveLerp\s*\(\s*e\s*,\s*paliashdr\s*,\s*&(\w+)\s*,\s*&(\w+)\s*,\s*&(\w+)\s*\)", dam)
    if len(re.findall(r"\bR_AliasResolveLerp\s*\(", dam)) != 1 or len(res) != 1:
        errors.append("R_DrawAliasModel must call R_AliasResolveLerp (e, paliashdr, &prev, &pose, &blend) exactly once, "
                      "for both passes (#493)")
    else:
        lp, lc, lb = res[0]
        at_resolve = dam.index("R_AliasResolveLerp")
        # Unconditional: at brace depth 1 of the body, and the previous token
        # ends a statement or block (no `if (x)` / `else` in front of it).
        depth = sum({"{": 1, "}": -1}.get(ch, 0) for ch in dam[:at_resolve])
        before = dam[:at_resolve].rstrip()
        if depth != 1 or not before or before[-1] not in ";{}":
            errors.append("R_DrawAliasModel: the resolve must be a top-level statement no condition can skip")
        calls = [(m.start(), m.groups()) for m in
                 re.finditer(r"\bR_SetupAliasFrame\s*\(\s*e\s*,\s*paliashdr\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)", dam)]
        if len(re.findall(r"\bR_SetupAliasFrame\s*\(", dam)) != 2 or len(calls) != 2:
            errors.append("R_DrawAliasModel must call R_SetupAliasFrame exactly twice: the base pass and the fullbright pass")
        else:
            for pos, args in calls:
                if args != (lp, lc, lb):
                    errors.append(f"R_DrawAliasModel: R_SetupAliasFrame{args} must take the resolved "
                                  f"({lp}, {lc}, {lb}) in the resolver's order")
                if pos < at_resolve:
                    errors.append("R_DrawAliasModel: a pass draws before the lerp is resolved")
            m_on = re.search(r"\bmodel_fullbright_pass\s*=\s*true\s*;", dam)
            m_off = re.search(r"\bmodel_fullbright_pass\s*=\s*false\s*;", dam)
            on = m_on.start() if m_on else -1
            off = m_off.start() if m_off else -1
            if re.search(r"\breturn\b", dam[at_resolve:calls[1][0]]):
                errors.append("R_DrawAliasModel: no return between the resolve and the glow pass")
            if not (0 <= on < calls[1][0] < off) or on < calls[0][0]:
                errors.append("R_DrawAliasModel: the second R_SetupAliasFrame must be the fullbright pass, "
                              "between model_fullbright_pass = true and = false, after the base pass")
        for name in (lp, lc, lb):
            writes = writes_of(name, dam)
            if len(writes) != 1:
                errors.append(f"R_DrawAliasModel: {name} must be written only by the resolve, or the glow "
                              f"pass could draw a different blend from the base pass")

# -- nothing else draws through R_SetupAliasFrame, nothing else poses by pass
# Whole-file counts first, so a use the function scan misses still counts.
# Calls only: a definition or prototype has `void` in front of the name.
if len(re.findall(r"(?<!void )\bR_SetupAliasFrame\s*\(", rmain)) != 2:
    errors.append("R_SetupAliasFrame: expected R_DrawAliasModel's two calls and no other "
                  "(any other caller skips the once-per-draw resolve)")
COLOUR_ONLY = ("GL_DrawAliasFrame", "GL_DrawAliasFrameMD3", "GL_DrawAliasSkeletal")
fns = functions(rmain)
allowed = 0
for name, body in fns:
    n = len(re.findall(r"\bmodel_fullbright_pass\b", body))
    if name in COLOUR_ONLY or name == "R_DrawAliasModel":
        allowed += n
    elif n:
        errors.append(f"{name}: reads model_fullbright_pass; only the three draw functions may, for vertex colour")
if len(re.findall(r"\bmodel_fullbright_pass\b", rmain)) != allowed + 1:
    errors.append("model_fullbright_pass: used outside its declaration, R_DrawAliasModel and the three draw functions")
if dam is not None and len(re.findall(r"\bmodel_fullbright_pass\b", dam)) != 2:
    errors.append("R_DrawAliasModel: model_fullbright_pass may only be set true and back false around the glow pass")
for name in ("GL_DrawAliasFrame", "GL_DrawAliasFrameMD3"):
    body = body_of(rmain, rf"static void {name} \(entity_t \*e, aliashdr_t \*paliashdr,\s*int posenum, "
                          rf"int prevposenum, float lerpfrac\)\s*\{{")
    if body is None:
        errors.append(f"{name}: definition not found")
        continue
    arms = re.findall(r"\bif \(model_fullbright_pass\)\s*\{\s*(?:tmp_color\[vi\]\[[0-3]\]\s*=\s*)+1;\s*\}\s*else\b", body)
    if not arms or len(re.findall(r"\bmodel_fullbright_pass\b", body)) != len(arms):
        errors.append(f"{name}: model_fullbright_pass may only pick a white vertex colour over the shaded one, never a pose")
skel = [b for n, b in fns if n == "GL_DrawAliasSkeletal"]
if not skel:
    errors.append("GL_DrawAliasSkeletal: definition not found")
for body in skel:
    uses = len(re.findall(r"\bmodel_fullbright_pass\b", body))
    light = len(re.findall(r"R_SetSkelLight \(shadevector, light, model_fullbright_pass \? 1\.0f : 0\.0f\);", body))
    if uses != light:
        errors.append("GL_DrawAliasSkeletal: model_fullbright_pass may only pick the unshaded light")

# -- and the instanced path resolves once too ----------------------------
cai = body_of(rmain, r"(?:static )?\w[\w \*]*\bR_CollectAliasInstance \([^)]*\)\s*\{")
if cai is None:
    errors.append("R_CollectAliasInstance: definition not found")
elif len(re.findall(r"\bR_AliasResolveLerp\s*\(", cai)) != 1:
    errors.append("R_CollectAliasInstance must resolve the lerp exactly once per entity")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS the CPU alias glow draws at the base pass's blend, resolved once per draw (#493)")
