#!/usr/bin/env python3
"""The Rust gate's CI shards cover every item exactly once.

scripts/check-rust.sh --shard I/N splits the ~24-minute Rust gate across a CI
job matrix.  The one way that can go wrong silently is coverage: a plan that
drops an item, or hands one to two shards and another to none, still turns
every job green.  So this checks, without nix or a compiler:

  - for N = 1..12, `--plan N` puts every item (core, each sv-main part, each
    world-trace part) in exactly one shard numbered 1..N;
  - each split gate lists the parts it actually loops over (--list-parts);
  - RUST_GATE_PARTS narrows a gate in the gate's own order and rejects a
    part the gate does not have, so a typo cannot skip work;
  - malformed --shard values are refused, and a shard with nothing to do
    (more shards than items) exits 0 without running anything;
  - ci.yml derives N from the matrix itself (strategy.job-total), so the
    shard list and N cannot disagree.

Usage: python3 tools/test_rust_gate_shards.py
"""
import collections
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GATE = ROOT / "scripts" / "check-rust.sh"
LIB = ROOT / "scripts" / "lib" / "rust-gate.sh"

errors = []


def run(args, env_extra=None, check=True):
    # A clean environment, so an RUST_GATE_PARTS exported in the caller's
    # shell cannot change what is being tested.
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": os.environ.get("HOME", "/tmp")}
    if env_extra:
        env.update(env_extra)
    p = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True)
    if check and p.returncode != 0:
        raise SystemExit(f"FAIL: {' '.join(args)} exited {p.returncode}\n{p.stderr}")
    return p


def parts(gate):
    out = run(["bash", f"scripts/check-rust-{gate}.sh", "--list-parts"]).stdout.split()
    return out


sv_parts = parts("sv-main")
wt_parts = parts("world-trace")
if sv_parts != ["sound", "ents", "datagram", "connect", "inv", "spawn"]:
    errors.append(f"sv-main --list-parts: unexpected {sv_parts}")
expected_wt = [f"{v}/{p}" for v in ("h2", "hw")
               for p in ("trace", "hull", "link", "move", "svmove", "svphys",
                         "svpush", "svwalk", "svmovetype")]
if wt_parts != expected_wt:
    errors.append(f"world-trace --list-parts: unexpected {wt_parts}")

# The loops must iterate the selected parts, not a hard-coded list that
# --list-parts could drift away from.
for gate in ("sv-main", "world-trace"):
    text = (ROOT / "scripts" / f"check-rust-{gate}.sh").read_text()
    if 'for port in "${parts[@]}"' not in text and 'for part in "${parts[@]}"' not in text:
        errors.append(f"check-rust-{gate}.sh does not loop over the selected parts")
    if "rust_gate_parts_selected" not in text:
        errors.append(f"check-rust-{gate}.sh does not select its parts through the shared helper")

items = ["core"] + [f"sv-main:{p}" for p in sv_parts] + [f"world-trace:{p}" for p in wt_parts]

for n in range(1, 13):
    lines = run(["bash", str(GATE), "--plan", str(n)]).stdout.splitlines()
    seen = collections.Counter()
    for line in lines:
        m = re.fullmatch(r"(\d+) (\S+)", line)
        if not m:
            errors.append(f"--plan {n}: malformed line {line!r}")
            continue
        s = int(m.group(1))
        if not 1 <= s <= n:
            errors.append(f"--plan {n}: shard {s} out of range for {m.group(2)}")
        seen[m.group(2)] += 1
    for item in items:
        if seen[item] != 1:
            errors.append(f"--plan {n}: {item} is in {seen[item]} shards, want exactly 1")
    for extra in set(seen) - set(items):
        errors.append(f"--plan {n}: unknown item {extra}")
    if n == 1 and len(lines) != len(items):
        errors.append(f"--plan 1: {len(lines)} lines for {len(items)} items")

# Determinism: every shard job computes the plan independently.
if run(["bash", str(GATE), "--plan", "6"]).stdout != run(["bash", str(GATE), "--plan", "6"]).stdout:
    errors.append("--plan 6 is not deterministic")

# The selection helper: subset in the gate's order; unknown part refused.
sel = run(["bash", "-c", f". '{LIB}'; rust_gate_parts_selected sound ents inv"],
          {"RUST_GATE_PARTS": "inv sound"}).stdout.split()
if sel != ["sound", "inv"]:
    errors.append(f"rust_gate_parts_selected kept {sel}, want ['sound', 'inv'] (gate order)")
sel = run(["bash", "-c", f". '{LIB}'; rust_gate_parts_selected a b"], {"RUST_GATE_PARTS": ""}).stdout.split()
if sel != ["a", "b"]:
    errors.append(f"rust_gate_parts_selected with RUST_GATE_PARTS empty kept {sel}, want all")
for gate in ("sv-main", "world-trace"):
    p = run(["bash", f"scripts/check-rust-{gate}.sh"], {"RUST_GATE_PARTS": "nosuchpart"}, check=False)
    if p.returncode != 2 or "nosuchpart" not in p.stderr:
        errors.append(f"check-rust-{gate}.sh with RUST_GATE_PARTS=nosuchpart: exit {p.returncode}, want 2 naming the part")

for bad in ("0/6", "7/6", "x/6", "3", "3/0", ""):
    p = run(["bash", str(GATE), "--shard", bad], check=False)
    if p.returncode != 2:
        errors.append(f"--shard {bad!r}: exit {p.returncode}, want 2")

p = run(["bash", str(GATE), "--shard", "40/40"], check=False)
if p.returncode != 0 or "no items" not in p.stdout:
    errors.append(f"--shard 40/40 (empty): exit {p.returncode}, want 0 with 'no items'")

ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text()
if not re.search(r"check-rust\.sh --shard \$\{\{ matrix\.shard \}\}/\$\{\{ strategy\.job-total \}\}", ci):
    errors.append("ci.yml: the Rust job must run check-rust.sh --shard ${{ matrix.shard }}/${{ strategy.job-total }}")
if "fail-fast: false" not in ci:
    errors.append("ci.yml: the Rust matrix must set fail-fast: false, or one red shard cancels the rest")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print(f"PASS: check-rust.sh shards cover all {len(items)} items exactly once for N=1..12; "
      "parts selection and --shard validation hold")
