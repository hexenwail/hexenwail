#!/usr/bin/env python3
"""Exercise scripts/vulnix-report.py on synthetic vulnix JSON.

One case per outcome the renderer tells apart: not run, failed, suspicious,
clean and findings.  There are also cases for the damage a newer vulnix or
a crash could cause: missing optional keys, wrong element types, a
truncated file.

The bugs these guard against:
  - The old summary's `[ -s vulnix-report.txt ]`.  vulnix's text output is
    never empty, so a failed scan read as "no findings".
  - An empty `[]` from vulnix (every deriver lookup failed) reading as an
    all-clear.
  - A crash in the renderer leaving no summary at all.
"""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("vulnix_report", ROOT / "scripts/vulnix-report.py")
vr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vr)

LIBXMP = {
    "name": "libxmp-4.6.0", "pname": "libxmp", "version": "4.6.0",
    "derivation": "/nix/store/aaaa-libxmp-4.6.0.drv",
    "affected_by": ["CVE-2025-0002", "CVE-2025-0001"],
    "whitelisted": ["CVE-2020-9999"],
    "cvssv3_basescore": {"CVE-2025-0001": 9.8, "CVE-2025-0002": 5.5},
    "description": {"CVE-2025-0001": "Heap overflow in the IT loader. " * 40,
                    "CVE-2025-0002": "Tab\tand  ```fence``` in text."},
}
ZLIB_WL = {
    "name": "zlib-1.3.1", "pname": "zlib", "version": "1.3.1",
    "derivation": "/nix/store/bbbb-zlib-1.3.1.drv",
    "affected_by": [], "whitelisted": ["CVE-2026-27820", "CVE-2023-6992"],
    "cvssv3_basescore": {"CVE-2026-27820": 9.8}, "description": {},
}

STATES = {
    "not run": "The scan step did not run",
    "failed": "did not produce a usable report",
    "suspicious": "This is not a clean result",
    "clean": "No findings past the whitelist",
    "findings": "Findings in the runtime closure",
}
failures = []


def check(label, ok, extra=""):
    print(f"{'ok  ' if ok else 'FAIL'}   {label}" + (f"\n{extra}" if not ok and extra else ""))
    if not ok:
        failures.append(label)


def case(name, content, want_state, rc=None):
    """content None = no file.  rc None = $VULNIX_RC unset."""
    with tempfile.TemporaryDirectory() as d:
        js, txt = Path(d, "r.json"), Path(d, "r.txt")
        if content is not None:
            js.write_text(content if isinstance(content, str) else json.dumps(content))
        env = {} if rc is None else {"VULNIX_RC": str(rc)}
        try:
            with contextlib.redirect_stdout(io.StringIO()) as out:
                code = vr.main(["vulnix-report.py", str(js), str(txt)], env=env)
        except Exception as e:          # a crash is exactly what must not happen
            check(f"{name}: no crash", False, repr(e))
            return "", ""
        md = out.getvalue()
        state = next((s for s, marker in STATES.items() if marker in md), "?")
        ok = state == want_state and code == 0 and txt.read_text()
        print(f"{'ok  ' if ok else 'FAIL'} {name}: state={state} (want {want_state}), rc={code}")
        if not ok:
            failures.append(name)
            print(md)
        return md, txt.read_text()


print("-- not run / failed")
md, _ = case("no JSON and no VULNIX_RC (an earlier step failed)", None, "not run")
check("not run: does not blame the NVD feeds", "NVD" not in md)
md, _ = case("no JSON but VULNIX_RC=2 (vulnix ran, output lost)", None, "failed", rc=2)
check("failed: quotes vulnix's exit code", "vulnix exited 2." in md, md)
case("empty file (vulnix died before printing)", "", "failed", rc=2)
case("truncated JSON", '[{"name": "x", "affected_by": [', "failed", rc=2)
case("top level is an object", {"error": "NVD unreachable"}, "failed")
case("entry without affected_by", [{"name": "x"}], "failed")
md, _ = case("affected_by holds null", [{"name": "x-1", "affected_by": [None]}], "failed")
check("element-type reason given", "holds a non-string" in md, md)
case("whitelisted holds a number", [{"name": "x-1", "affected_by": [], "whitelisted": [7]}], "failed")
case("name is not a string", [{"name": 42, "affected_by": []}], "failed")

print("-- suspicious")
md, txt = case("vulnix printed [] and exited 0 (every deriver lookup failed)", [], "suspicious", rc=0)
check("suspicious: says vulnix likely did not scan", "most likely did not scan it" in md, md)
check("suspicious: text report flagged too", txt.startswith("NOT A CLEAN RESULT"), txt)
case("[] with no VULNIX_RC", [], "suspicious")
case("derivations with no CVEs at all", [{"name": "a-1", "affected_by": [], "whitelisted": []}], "suspicious", rc=0)
md, _ = case("rc 2 but no findings (contradiction)", [ZLIB_WL], "suspicious", rc=2)
check("contradiction explained", "vulnix exited 2, but its JSON has no findings" in md, md)

print("-- clean")
md, _ = case("only whitelisted entries, rc 1", [ZLIB_WL], "clean", rc=1)
check("clean: whitelisted count", "2 whitelisted CVE reference(s) across" in md, md)
case("only whitelisted entries, no VULNIX_RC", [ZLIB_WL], "clean")

print("-- findings")
md, txt = case("findings plus a whitelisted derivation", [LIBXMP, ZLIB_WL], "findings", rc=2)
check("CVSS order", txt.index("CVE-2025-0001") < txt.index("CVE-2025-0002"))
check("score shown", "CVE-2025-0001     9.8" in txt)
check("description capped", "..." in txt and len(max(txt.splitlines(), key=len)) < 520)
check("whitespace collapsed", "Tab and" in txt)
check("whitelisted-only derivation not listed",
      "zlib" not in txt.split("left out due to whitelisting", 1)[1])
check("per-derivation masked count", "(+1 whitelisted on this derivation: CVE-2020-9999)" in txt)
check("no stray fence in summary", md.count("```") == 2)
check("hidden total in summary", "(3 CVE reference(s) hidden)" in md)
md, _ = case("findings but rc 0 (contradiction, still findings)", [LIBXMP], "findings", rc=0)
check("contradiction noted under findings", "Note: vulnix exited 0 but the JSON implies 2" in md, md)

print("-- ordering matches vulnix")
cves = {"name": "p-1", "affected_by": ["CVE-2024-10000", "CVE-2024-9999", "CVE-2023-50000"],
        "cvssv3_basescore": {"CVE-2024-10000": 7.5, "CVE-2024-9999": 7.5, "CVE-2023-50000": 7.5}}
_, txt = case("numeric CVE order at equal score", [cves], "findings")
check("CVE-2023-50000 < CVE-2024-9999 < CVE-2024-10000",
      txt.index("CVE-2023-50000") < txt.index("CVE-2024-9999") < txt.index("CVE-2024-10000"), txt)
drvs = [{"name": n, "pname": n.rsplit("-", 1)[0], "version": n.rsplit("-", 1)[1], "affected_by": ["CVE-2024-1"]}
        for n in ("openssl-3.0.10", "curl-8.9.0", "openssl-3.0.9", "openssl-3.0.9pre1")]
_, txt = case("numeric version order", drvs, "findings")
order = [l for l in txt.splitlines() if l in ("openssl-3.0.10", "curl-8.9.0", "openssl-3.0.9", "openssl-3.0.9pre1")]
check("curl, then openssl 3.0.9pre1 < 3.0.9 < 3.0.10", order ==
      ["curl-8.9.0", "openssl-3.0.9pre1", "openssl-3.0.9", "openssl-3.0.10"], str(order))
for a, b, want in [("3.0.9", "3.0.10", -1), ("1.0", "1.0.1", -1), ("2.0pre1", "2.0", -1),
                   # "²" is not an ASCII digit to vulnix, so it compares as text,
                   # after "a" (U+00B2 > U+0061); int("²") would have crashed.
                   ("1.2a", "1.2", 1), ("1²", "1a", 1), ("1.0", "1.0", 0)]:
    got = vr.compare_versions(a, b)
    check(f"compare_versions({a!r}, {b!r}) == {want}", got == want, f"got {got}")

print("-- tolerance")
bare = {"name": "libsndfile-1.2.2", "affected_by": ["CVE-2024-50612"]}
_, txt = case("no whitelisted / cvssv3_basescore / description keys", [bare], "findings", rc=2)
check("bare entry rendered", "CVE-2024-50612" in txt)
case("VULNIX_RC not a number is ignored", [ZLIB_WL], "clean", rc="x")

many = {"name": "big-1", "affected_by": [f"CVE-2099-{i:05d}" for i in range(3000)],
        "description": {f"CVE-2099-{i:05d}": "x " * 300 for i in range(3000)}}
md, txt = case("oversized report", [many], "findings", rc=2)
check("summary capped near 60000 bytes, text report not",
      len(md.encode()) <= vr.SUMMARY_CAP + 2000 and len(txt.encode()) > vr.SUMMARY_CAP)

if failures:
    print(f"\n{len(failures)} failure(s): {failures}")
    sys.exit(1)
print("\nall vulnix-report cases pass")
