#!/usr/bin/env python3
"""Check the HexenWorld client's CE_* effect readers against the writers.

Companion to scripts/hw_tent_layout_check.py, same idea for a different
message family.  engine/hexen2/cl_hw_effects.inc has to consume every effect payload
byte for byte or the rest of the datagram -- reliable data included -- is
misread (GitHub #213).

Unlike the TE_* temp entities, effects are NOT written by gamecode.  They
are written by the engine's own server:

    svc_start_effect   SV_SendEffect       engine/hexenworld/server/sv_effect.c
    svc_update_effect  PF_updateeffect     engine/h2shared/pr_cmds.c
    svc_multieffect    SV_ParseMultiEffect engine/hexenworld/server/sv_effect.c
    svc_turn_effect    PF_turneffect       engine/h2shared/pr_cmds.c

So those four functions are the contract, and this script derives the wire
size of every case arm in them independently -- by walking the MSG_Write*
calls and summing (coord/short 2, byte/char/angle 1, long/float 4) -- then
compares the result against what HWCL_ParseEffectPayload and friends read.

Two things it reports that a plain size diff would not:

  * types SV_SendEffect can emit that the reader has no case for (a live
    desync), and
  * types effects.h defines that no writer can ever emit (unreachable, and
    so deliberately absent from the reader rather than missing from it).

Exit 0 when everything agrees, 1 on any mismatch.  No arguments.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EFFECTS_H = os.path.join(ROOT, "engine", "hexenworld", "shared", "effects.h")
SV_EFFECT = os.path.join(ROOT, "engine", "hexenworld", "server", "sv_effect.c")
PR_CMDS = os.path.join(ROOT, "engine", "h2shared", "pr_cmds.c")
CL_HW = os.path.join(ROOT, "engine", "hexen2", "cl_hw_effects.inc")

WRITE_SIZES = {"Coord": 2, "Short": 2, "Byte": 1, "Char": 1,
               "Angle": 1, "Long": 4, "Float": 4}

WRITE_RE = re.compile(r"\bMSG_Write(Coord|Short|Byte|Char|Angle|Long|Float)\s*\(")

# Reader primitives in cl_hw_effects.inc and the bytes each consumes.  The Skip*
# helpers take a count; the MSG_Read* ones are a single value.
READ_COUNTED = {"HWCL_SkipCoords": 2, "HWCL_SkipAngles": 1, "HWCL_SkipFloats": 4}
READ_SINGLE = {"Coord": 2, "Short": 2, "Byte": 1, "Char": 1,
               "Angle": 1, "Long": 4, "Float": 4}
READ_COUNTED_RE = re.compile(r"\b(HWCL_Skip(?:Coords|Angles|Floats))\s*\(\s*(\d+)\s*\)")
READ_SINGLE_RE = re.compile(r"\bMSG_Read(Coord|Short|Byte|Char|Angle|Long|Float)\s*\(")
READ_COORDS_RE = re.compile(r"\bHWCL_ReadCoords\s*\(")

# Writer arms whose length depends on the data they carry: the five-bolt
# loop in SV_SendEffect, and the branch-per-command arms of PF_updateeffect.
# Byte accounting for these is asserted in scripts/hw_ce_test.c instead,
# which walks each branch against the reader.
VARIABLE_START = {"CE_HWSHEEPINATOR", "CE_HWXBOWSHOOT"}
VARIABLE_UPDATE = {"CE_HWSHEEPINATOR", "CE_HWXBOWSHOOT", "CE_HWDRILLA"}

# Arms that contain control flow but whose length is still fixed, each
# checked by hand and pinned to the size asserted here.  An arm that grows a
# branch which actually writes will change the computed sum and fail.
HAND_VERIFIED_UPDATE = {
    # if/else picks Chain.state only; the single WriteShort is unconditional.
    "CE_SCARABCHAIN": 2,
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"),
                  text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def function_body(text, signature):
    """Source of one function, from its opening brace to the matching one."""
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:i + 1]
    raise SystemExit("unbalanced braces in %s" % signature)


def switch_after(body, marker):
    """The body of the first switch that follows `marker`."""
    start = body.index(marker) if marker else 0
    sw = body.index("switch", start)
    brace = body.index("{", sw)
    depth = 0
    for i in range(brace, len(body)):
        if body[i] == "{":
            depth += 1
        elif body[i] == "}":
            depth -= 1
            if depth == 0:
                return body[brace + 1:i]
    raise SystemExit("unbalanced switch braces")


def case_arms(switch_body):
    """[(list of CE_ labels, arm source)], for arms at the switch's own
    brace depth.  Nested switches would confuse the label scan; none of the
    four writers has one."""
    arms, labels, buf, depth = [], [], [], 0
    has_default = False
    for line in switch_body.split("\n"):
        depth_before = depth
        depth += line.count("{") - line.count("}")
        m = re.match(r"\s*case\s+([A-Za-z_]\w*)\s*:", line)
        if m and depth_before == 0:
            if buf:
                arms.append((labels, "\n".join(buf)))
                labels, buf = [], []
            labels.append(m.group(1))
            continue
        if re.match(r"\s*default\s*:", line) and depth_before == 0:
            has_default = True
            if buf:
                arms.append((labels, "\n".join(buf)))
            labels, buf = [], []
            continue
        if labels:
            buf.append(line)
    if labels and buf:
        arms.append((labels, "\n".join(buf)))
    return arms, has_default


def writer_arm_size(src):
    """(bytes, variable?) for one writer arm."""
    variable = bool(re.search(r"\bfor\s*\(|\bif\s*\(", src))
    size = sum(WRITE_SIZES[k] for k in WRITE_RE.findall(src))
    return size, variable


def reader_arm_size(src):
    """(bytes, variable?) for one reader arm."""
    variable = bool(re.search(r"\bfor\s*\(|\bif\s*\(|HWCL_SkipXbowBolts", src))
    size = 0
    for name, count in READ_COUNTED_RE.findall(src):
        size += READ_COUNTED[name] * int(count)
    src_wo = READ_COUNTED_RE.sub("", src)
    size += 6 * len(READ_COORDS_RE.findall(src_wo))
    size += sum(READ_SINGLE[k] for k in READ_SINGLE_RE.findall(src_wo))
    return size, variable


def ce_numbers():
    nums = {}
    for m in re.finditer(r"^#define\s+(CE_\w+)\s+(\d+)",
                         strip_comments(open(EFFECTS_H, encoding="latin-1").read()),
                         re.M):
        nums[m.group(1)] = int(m.group(2))
    return nums


def arms_to_sizes(arms, variable_ok, what, problems, hand_verified=None):
    """{label: size}, plus the set of labels whose arm is data-dependent."""
    hand_verified = hand_verified or {}
    sizes, variable = {}, set()
    for labels, src in arms:
        size, is_var = (writer_arm_size(src) if what[0] == "w"
                        else reader_arm_size(src))
        for label in labels:
            if is_var and label in hand_verified:
                # Branchy but fixed: pin the sum so a branch that starts
                # writing is caught here rather than on the wire.
                if size != hand_verified[label]:
                    problems.append(
                        "%s: hand-verified arm %s now sums to %d bytes, not "
                        "the %d it was checked at" %
                        (what, label, size, hand_verified[label]))
                sizes[label] = size
            elif is_var:
                variable.add(label)
                if label not in variable_ok:
                    problems.append(
                        "%s: arm %s is data-dependent but is not listed as "
                        "variable" % (what, label))
            else:
                sizes[label] = size
    return sizes, variable


def main():
    problems = []
    nums = ce_numbers()

    sv = strip_comments(open(SV_EFFECT, encoding="latin-1").read())
    pr = strip_comments(open(PR_CMDS, encoding="latin-1").read())
    cl = strip_comments(open(CL_HW, encoding="latin-1").read())

    # --- svc_start_effect ------------------------------------------------
    send = function_body(sv, "void SV_SendEffect")
    # The first switch only picks a PVS test origin; the payload is the one
    # after the type byte goes on the wire.
    w_arms, w_default = case_arms(
        switch_after(send, "MSG_WriteByte (&sv.multicast, sv.Effects[idx].type);"))
    w_start, w_start_var = arms_to_sizes(w_arms, VARIABLE_START,
                                         "write start_effect", problems)

    payload = function_body(cl, "static qboolean HWCL_ParseEffectPayload")
    r_arms, r_default = case_arms(switch_after(payload, ""))
    r_start, r_start_var = arms_to_sizes(r_arms, VARIABLE_START,
                                         "read start_effect", problems)

    emittable = set(w_start) | w_start_var
    readable = set(r_start) | r_start_var
    for label in sorted(emittable - readable):
        problems.append("start_effect: server can send %s but "
                        "HWCL_ParseEffectPayload has no case for it" % label)
    for label in sorted(emittable & readable):
        if label in w_start_var or label in r_start_var:
            if not (label in w_start_var and label in r_start_var):
                problems.append("start_effect: %s is data-dependent on one "
                                "side only" % label)
            continue
        if w_start[label] != r_start[label]:
            problems.append("start_effect: %s writes %d bytes, reader "
                            "consumes %d" % (label, w_start[label],
                                             r_start[label]))

    # --- svc_update_effect -----------------------------------------------
    upd = function_body(pr, "static void PF_updateeffect")
    u_arms, u_default = case_arms(
        switch_after(upd, "MSG_WriteByte (&sv.multicast, type);"))
    u_write, u_write_var = arms_to_sizes(u_arms, VARIABLE_UPDATE,
                                         "write update_effect", problems,
                                         HAND_VERIFIED_UPDATE)
    if u_default:
        problems.append(
            "update_effect: PF_updateeffect grew a default arm; the client's "
            "'unknown type carries no payload' assumption no longer holds")

    parse_upd = function_body(cl, "static qboolean HWCL_ParseUpdateEffect")
    pu_arms, pu_default = case_arms(switch_after(parse_upd, ""))
    pu_read, pu_read_var = arms_to_sizes(pu_arms, VARIABLE_UPDATE,
                                         "read update_effect", problems,
                                         HAND_VERIFIED_UPDATE)
    if not pu_default:
        problems.append("update_effect: the reader lost its default arm, so "
                        "an unknown type would fall through unhandled")
    for label in sorted((set(u_write) | u_write_var) -
                        (set(pu_read) | pu_read_var)):
        problems.append("update_effect: server can send %s but the reader "
                        "has no case for it" % label)
    for label in sorted(set(u_write) & set(pu_read)):
        if u_write[label] != pu_read[label]:
            problems.append("update_effect: %s writes %d bytes, reader "
                            "consumes %d" % (label, u_write[label],
                                             pu_read[label]))

    # --- svc_multieffect --------------------------------------------------
    multi = function_body(sv, "void SV_ParseMultiEffect")
    m_arms, _ = case_arms(switch_after(multi, ""))
    m_labels = sorted(l for labels, _ in m_arms for l in labels)
    if m_labels != ["CE_HWRAVENPOWER"]:
        problems.append("multieffect: server arms are %s; the client only "
                        "accepts CE_HWRAVENPOWER" % m_labels)
    if "PR_RunError" not in switch_after(multi, ""):
        problems.append("multieffect: SV_ParseMultiEffect no longer errors on "
                        "an unknown type, so the client may not assume one")
    # The arm writes the svc byte, the type byte and 6 coords straight
    # through, then one index byte per iteration of a fixed 3-pass loop.
    # Count the straight-line part and add the loop explicitly: the regex
    # sees the WriteByte inside the loop only once.
    m_arm = m_arms[0][1]
    m_loop = m_arm[m_arm.index("for ("):] if "for (" in m_arm else ""
    m_straight = sum(WRITE_SIZES[k] for k in
                     WRITE_RE.findall(m_arm[:len(m_arm) - len(m_loop)]))
    m_loop_once = sum(WRITE_SIZES[k] for k in WRITE_RE.findall(m_loop))
    if m_loop_once != 1 or "count < 3" not in m_loop:
        problems.append("multieffect: the slot loop is no longer 3 x 1 byte; "
                        "re-derive it by hand")
    # less the svc byte, which the client consumes before dispatching
    m_size = m_straight - 1 + 3 * m_loop_once

    # Reader side has the mirror-image fixed loop; count it the same way.
    parse_multi = function_body(cl, "static qboolean HWCL_ParseMultiEffect")
    r_loop = parse_multi[parse_multi.index("for ("):] if "for (" in parse_multi else ""
    r_straight, _ = reader_arm_size(parse_multi[:len(parse_multi) - len(r_loop)])
    r_loop_once, _ = reader_arm_size(r_loop)
    if r_loop_once != 1 or "i < 3" not in r_loop:
        problems.append("multieffect: the reader's slot loop is no longer "
                        "3 x 1 byte; re-derive it by hand")
    r_multi = r_straight + 3 * r_loop_once
    if m_size != r_multi:
        problems.append("multieffect: writer puts %d bytes on the wire, "
                        "reader consumes %d" % (m_size, r_multi))

    # --- svc_turn_effect --------------------------------------------------
    turn = function_body(pr, "static void PF_turneffect")
    t_size = sum(WRITE_SIZES[k] for k in WRITE_RE.findall(turn))
    t_size -= 1  # the svc byte itself is not payload
    parse_turn = function_body(cl, "static qboolean HWCL_ParseTurnEffect")
    r_turn, _ = reader_arm_size(parse_turn)
    if t_size != r_turn:
        problems.append("turn_effect: writer puts %d bytes on the wire, "
                        "reader consumes %d" % (t_size, r_turn))

    # --- reachability -----------------------------------------------------
    unreachable = sorted(n for n in nums if n not in emittable)

    print("CE_* effects: %d defined in effects.h, %d emittable by "
          "SV_SendEffect (%d fixed-size, %d data-dependent), "
          "%d never emitted by any writer."
          % (len(nums), len(emittable), len(w_start), len(w_start_var),
             len(unreachable)))
    print("svc_update_effect: %d types carry a payload (%s); every other "
          "type writes none." % (len(u_write) + len(u_write_var),
                                 ", ".join(sorted(set(u_write) | u_write_var))))
    print("svc_turn_effect: %d payload bytes, fixed." % t_size)
    print("svc_multieffect: CE_HWRAVENPOWER only, %d payload bytes." % m_size)
    if unreachable:
        print("never emitted: %s" % ", ".join(unreachable))

    for p in problems:
        print("PROBLEM: %s" % p, file=sys.stderr)
    print("%d problem(s)" % len(problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
