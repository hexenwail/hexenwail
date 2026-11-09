#!/usr/bin/env python3
"""Differential harnesses time a spinning call by CPU, never by wall clock.

Several Rust-port harnesses (engine/rust/*/tests/*.c) fork a child per case
and kill it when the call under test does not return -- some C originals
genuinely loop forever on some inputs, and the port must loop with them.
Timed with ITIMER_REAL, a busy CI runner can stretch an ordinary return past
the limit: sv_main_inv's harness reported a K_WRITEPAGE case (an fopen and an
unbuffered write per field) as a Rust-side spin while the C side returned,
and the same commit went green on a rerun.  ITIMER_PROF counts only the CPU
the child itself uses, so preemption and disk waits cannot trip it, and a
real spin -- a pure-CPU loop -- still does.

This fails if any harness:
  - arms ITIMER_REAL without also arming ITIMER_PROF in the same file (a
    wall-clock timer is allowed only as a backstop beside a CPU one), or
    arms it for less than 10 seconds;
  - decides "hung" from SIGALRM (WTERMSIG (...) == SIGALRM).

Usage: python3 tools/test_harness_cpu_timers.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
errors = []
checked = 0

for src in sorted((ROOT / "engine" / "rust").glob("*/tests/*.c")):
    text = src.read_text(errors="replace")
    if "setitimer" not in text:
        continue
    checked += 1
    rel = src.relative_to(ROOT)
    has_prof = re.search(r"setitimer\s*\(\s*ITIMER_PROF", text) is not None
    for m in re.finditer(r"setitimer\s*\(\s*ITIMER_REAL", text):
        if not has_prof:
            line = text.count("\n", 0, m.start()) + 1
            errors.append(f"{rel}:{line}: ITIMER_REAL with no ITIMER_PROF -- time the call by CPU")
            continue
        # A backstop must be long.  Look back to the arming of it_value just
        # before this setitimer; a disarm (memset only) has none, which is fine.
        window = text[max(0, m.start() - 400):m.start()]
        arm = list(re.finditer(r"it_value\.tv_(sec|usec)\s*=\s*([A-Za-z_0-9* ]+);", window))
        if arm:
            last_disarm = window.rfind("memset")
            if arm[-1].start() > last_disarm:
                field, expr = arm[-1].group(1), arm[-1].group(2).strip()
                macro = re.search(r"#define\s+%s\s+(\d+)" % re.escape(expr), text)
                value = int(macro.group(1)) if macro else (int(expr) if expr.isdigit() else None)
                if field == "usec" or value is None or value < 10:
                    line = text.count("\n", 0, m.start()) + 1
                    errors.append(f"{rel}:{line}: ITIMER_REAL backstop of {field} {expr} -- "
                                  "a wall-clock backstop must be >= 10 s")
    for m in re.finditer(r"WTERMSIG\s*\([^)]*\)\s*==\s*SIGALRM", text):
        line = text.count("\n", 0, m.start()) + 1
        errors.append(f"{rel}:{line}: 'hung' decided by SIGALRM -- the CPU timer's signal is SIGPROF")

if checked == 0:
    errors.append("no harness under engine/rust/*/tests/ calls setitimer; the glob is wrong")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print(f"PASS: {checked} timed harnesses spin-check by CPU time (ITIMER_PROF/SIGPROF)")
