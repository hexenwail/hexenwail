#!/usr/bin/env python3
"""Keep demo playback reachable from the client frame loop.

Demo playback has no qsocket.  CL_PlayDemo_f opens cls.demofh, sets
cls.demoplayback and cls.state = ca_connected, and CL_GetMessage then reads
the demo file instead of the network -- cls.netcon stays NULL for the whole
demo.  Two things have to hold every frame or the demo never advances:

  1. _Host_Frame must call CL_ReadFromServer while a demo plays.  It does that
     via `cls.state == ca_connected`, which is why CL_PlayDemo_f sets it.
  2. CL_ReadFromServer must reach CL_AdvanceTime/CL_GetMessage.  Its
     H2W_INTEGRATED prologue returns early when HexenWorld owns the socket,
     and that guard must not also swallow demo playback.

GitHub #209.  Commit 30ea43b17 ("feat(hexenworld): integrate client
networking into Hexenwail") added a bare `if (!cls.netcon) return 0;` to that prologue.  Every
demo then froze on the loading plaque until the 25-second "load timeout", and
because Storm of the Thyrion and Shadows of Chaos drive their whole front end
from `startdemos`, it was reported as mod loading being broken.  Nothing
caught it: no demo is played by data1's hexen.rc, and the headless lanes reach
the world with `+map`, never through a demo.

This source check fails if:

  - any early return in CL_ReadFromServer's prologue can fire while
    cls.demoplayback is set,
  - _Host_Frame stops calling CL_ReadFromServer for ca_connected, or
  - CL_PlayDemo_f stops putting the client in ca_connected.
"""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "engine"
CL_MAIN = ENGINE / "hexen2/cl_main.c"
CL_DEMO = ENGINE / "hexen2/cl_demo.c"
HOST = ENGINE / "hexen2/host.c"

READ_SIGNATURE = "int CL_ReadFromServer (void)\n{"
PLAY_SIGNATURE = "void CL_PlayDemo_f (void)\n{"
FRAME_SIGNATURE = "static void _Host_Frame (float time)\n{"


def blank(match):
    """Whitespace of the same shape, so offsets and line numbers survive."""
    return re.sub(r"[^\n]", " ", match.group(0))


def strip_comments(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)


def strip_directives(text):
    """Blank out preprocessor lines.

    The guards under test are written across `#if defined(H2W_INTEGRATED)`,
    and a literal `#endif` otherwise reads as the keyword `if` when scanning
    backwards for a condition.  Blanking the directives leaves both the
    conditions and the braces they span intact.
    """
    return re.sub(r"^[ \t]*#[^\n]*", blank, text, flags=re.M)


def prepare(text):
    return strip_directives(strip_comments(text))


def function(text, signature):
    """Return the source of the function introduced by `signature`."""
    try:
        start = text.index(signature)
    except ValueError:
        raise AssertionError(f"missing {signature.splitlines()[0]}")
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def block_end(text, index):
    """Return the offset just past the braced block opening at/after `index`."""
    depth = 1
    end = text.index("{", index) + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return end


def condition_before(text, index):
    """Return the `if (...)` condition immediately governing `index`.

    Walks back to the nearest `if` and re-parses its parentheses forward, so a
    condition spanning several lines (the H2W_INTEGRATED guards all do) comes
    back whole.
    """
    heads = list(re.finditer(r"(?<![A-Za-z0-9_])if\s*\(", text[:index]))
    if not heads:
        return None
    open_paren = heads[-1].end() - 1
    depth = 1
    pos = open_paren + 1
    while depth:
        depth += (text[pos] == "(") - (text[pos] == ")")
        pos += 1
    return text[open_paren:pos]


def check_read_from_server(failures):
    body = function(prepare(CL_MAIN.read_text()), READ_SIGNATURE)

    # Drop the HexenWorld-owned branch: its returns are correct, and a demo
    # cannot be playing inside it.  Anything left is on the Hexen II path.
    hw = body.find("HWCL_Active ()")
    if hw < 0:
        # No integrated HexenWorld client in this tree; nothing to gate.
        return
    head = body.rfind("if", 0, hw)          # cut the whole `if (HWCL_Active ())`
    prologue = body[:head] + body[block_end(body, hw):]

    # Everything before the shared clock is the prologue proper; past it the
    # demo is already being read.
    advance = prologue.find("CL_AdvanceTime ()")
    if advance < 0:
        failures.append(
            f"{CL_MAIN}: CL_ReadFromServer no longer calls CL_AdvanceTime; "
            "demo playback drives its clock from there")
        return
    prologue = prologue[:advance]

    for match in re.finditer(r"\breturn\b", prologue):
        condition = condition_before(prologue, match.start())
        if condition is None:
            failures.append(
                f"{CL_MAIN}: unconditional return in CL_ReadFromServer's "
                "prologue -- demo playback can never advance")
            continue
        if "demoplayback" not in condition:
            flat = " ".join(condition.split())
            failures.append(
                f"{CL_MAIN}: early return in CL_ReadFromServer guarded by "
                f"{flat} does not spare demo playback.  A demo has no "
                "cls.netcon, so this freezes every demo on the loading "
                "plaque until \"load timeout\" (see 30ea43b17).")


def check_host_frame(failures):
    body = function(prepare(HOST.read_text()), FRAME_SIGNATURE)
    call = body.find("CL_ReadFromServer ()")
    if call < 0:
        failures.append(f"{HOST}: _Host_Frame no longer calls CL_ReadFromServer")
        return
    condition = condition_before(body, call)
    if condition is None or "ca_connected" not in condition:
        flat = " ".join(condition.split()) if condition else "<none>"
        failures.append(
            f"{HOST}: _Host_Frame calls CL_ReadFromServer under {flat}, which "
            "no longer covers ca_connected.  Demo playback runs in "
            "ca_connected and would stop being read.")


def check_play_demo(failures):
    body = function(prepare(CL_DEMO.read_text()), PLAY_SIGNATURE)
    if not re.search(r"cls\.demoplayback\s*=\s*true", body):
        failures.append(f"{CL_DEMO}: CL_PlayDemo_f does not set cls.demoplayback")
    if not re.search(r"cls\.state\s*=\s*ca_connected", body):
        failures.append(
            f"{CL_DEMO}: CL_PlayDemo_f does not put the client in "
            "ca_connected; _Host_Frame gates CL_ReadFromServer on that.")


def main():
    failures = []
    check_read_from_server(failures)
    check_host_frame(failures)
    check_play_demo(failures)

    if failures:
        print("demo playback gate FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("demo playback gate: CL_ReadFromServer, _Host_Frame and "
          "CL_PlayDemo_f still agree that a demo has no qsocket.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
