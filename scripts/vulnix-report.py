#!/usr/bin/env python3
"""Render vulnix's JSON into the text report and the run-summary markdown.

Usage: vulnix-report.py REPORT_JSON REPORT_TXT
       (reads vulnix's exit code from $VULNIX_RC when set)

security-scan.yml runs vulnix once, as `--json -w <whitelist> -s`, so the
uploaded JSON keeps the whitelisted entries.  This writes the filtered,
human-readable report to REPORT_TXT and prints the step-summary markdown to
stdout.  It used to be a second full vulnix run in text mode, about half of
a ~49s step.

vulnix's JSON is a list with one object per derivation.  `affected_by` holds
the CVEs left after the whitelist, `whitelisted` the ones it masked.  The
report shows `affected_by` and counts `whitelisted`.

The outcomes are kept apart because the old text report could not tell them
apart.  vulnix's text output is never empty ("Found no advisories.
Excellent!"), so `[ -s vulnix-report.txt ]` was true on every run, and its
"no findings" branch only fired if vulnix crashed:

  not run     no JSON and no $VULNIX_RC: the scan step never ran, because
              an earlier step failed (the renderer test, the closure build).
  failed      JSON missing, empty, invalid, or not the shape above.
  suspicious  valid JSON, but it cannot be a real scan of this closure:
              - nothing active and nothing whitelisted (including `[]`).
                glibc, gcc and zlib always carry whitelisted CVEs.  vulnix
                silently drops closure paths whose deriver .drv it cannot
                find (a debug-level log line), and if it drops them all it
                prints [] and exits 0.
              - or the exit code contradicts the JSON (see below) when
                there are no findings.
  clean       nothing active, and at least one whitelisted CVE, which shows
              the closure really was matched.
  findings    at least one `affected_by` entry.

Exit codes are cross-checked against the JSON.  vulnix 1.12.4 exits 2 for
findings, 1 for whitelisted-only entries under -s, and 0 for nothing.  It
also exits 2 on a RuntimeError such as unreachable NVD feeds.  So rc 2 with
no findings in valid JSON means something failed after all.

Always exits 0 unless the arguments are wrong: the scan is advisory by
design (see the header of security-scan.yml).  The summary says which
outcome it was.

CI resolves `nix run nixpkgs#vulnix` against the registry's nixpkgs, which
can move past 1.12.4.  So `whitelisted`, `cvssv3_basescore`, `description`,
`pname`, `version` and `derivation` are optional per entry.  A wrong shape
or wrong element types count as failed.

Ordering matches vulnix's own text output.  Derivations sort by pname, then
by version under Nix's version comparison (vulnix's utils.compare_versions,
a port of nix/src/libexpr/names.cc).  CVEs within a derivation sort by CVSSv3
descending, then numerically by (year, number).
"""

import functools
import itertools
import json
import os
import re
import sys

SUMMARY_CAP = 60000       # bytes of report pasted into the step summary
DESCRIPTION_CAP = 400     # characters of each CVE description
CVE_RE = re.compile(r"^CVE-(\d+)-(\d+)$")
NAME_RE = re.compile(r"^(\S+?)-([0-9]\S*)$")   # Nix's parseDrvName, as vulnix


def load(path):
    """Return (entries, None) on success or (None, reason) on failure."""
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        return None, f"could not read {path}: {e.strerror}"
    if not text.strip():
        return None, f"{path} is empty"
    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        return None, f"{path} is not valid JSON ({e.msg} at line {e.lineno})"
    if not isinstance(data, list):
        return None, f"{path} is a JSON {type(data).__name__}, not a list"
    for i, d in enumerate(data):
        if not isinstance(d, dict):
            return None, f"entry {i} of {path} is not an object"
        name = d.get("name") or d.get("pname")
        if not isinstance(name, str) or not name:
            return None, f"entry {i} of {path} has no name string"
        for key, required in (("affected_by", True), ("whitelisted", False)):
            v = d.get(key)
            if v is None and not required:
                continue
            if not isinstance(v, list):
                return None, f"entry {i} ({name}) of {path}: {key} is not a list"
            if not all(isinstance(c, str) for c in v):
                return None, f"entry {i} ({name}) of {path}: {key} holds a non-string"
    return data, None


# --- ordering, mirroring vulnix -------------------------------------------

def _split_components(v):
    """Runs of digits or of non-digits; '.' and '-' only separate."""
    return [m.group(0) for m in re.finditer(r"[0-9]+|[^0-9.\-]+", v or "")]


def _component_lt(left, right):
    # ASCII digits only, as vulnix's category(); str.isdigit() also takes "²".
    ln = int(left) if re.fullmatch(r"[0-9]+", left) else None
    rn = int(right) if re.fullmatch(r"[0-9]+", right) else None
    if ln is not None and rn is not None:
        return ln < rn
    if left == "" and rn is not None:
        return True
    if left == "pre" and right != "pre":
        return True
    if right == "pre":
        return False
    if rn is not None:
        return True
    if ln is not None:
        return False
    return left < right


def compare_versions(left, right):
    if left == right:
        return 0
    for lc, rc in itertools.zip_longest(_split_components(left), _split_components(right), fillvalue=""):
        if lc == rc:
            continue
        if _component_lt(lc, rc):
            return -1
        if _component_lt(rc, lc):
            return 1
    return 0


def _pname_version(d):
    pname, version = d.get("pname"), d.get("version")
    if not isinstance(pname, str) or not isinstance(version, str):
        m = NAME_RE.match(d.get("name") or "")
        pname, version = (m.group(1), m.group(2)) if m else (d.get("name") or pname, "")
    return pname, version


def _derivation_cmp(a, b):
    (ap, av), (bp, bv) = _pname_version(a), _pname_version(b)
    if ap != bp:
        return -1 if ap < bp else 1
    return compare_versions(av, bv)


def _cve_key(cve):
    m = CVE_RE.match(cve)
    # Anything not shaped like a CVE id sorts after the real ones, by text.
    return (0, int(m.group(1)), int(m.group(2)), "") if m else (1, 0, 0, cve)


# --- rendering --------------------------------------------------------------

def score(d, cve):
    s = (d.get("cvssv3_basescore") or {}).get(cve) if isinstance(d.get("cvssv3_basescore"), dict) else None
    return s if isinstance(s, (int, float)) and not isinstance(s, bool) else None


def describe(d, cve):
    descs = d.get("description") if isinstance(d.get("description"), dict) else {}
    text = " ".join(str(descs.get(cve) or "").split())
    if len(text) > DESCRIPTION_CAP:
        text = text[:DESCRIPTION_CAP].rsplit(" ", 1)[0] + " ..."
    return text


def render(entries):
    """The filtered text report, shaped and ordered like vulnix's text output."""
    active = [d for d in entries if d["affected_by"]]
    hidden = [d for d in entries if not d["affected_by"]]
    lines = [f"{len(active)} derivations with active advisories"]
    if hidden:
        lines.append(f"{len(hidden)} derivations left out due to whitelisting")
    for d in sorted(active, key=functools.cmp_to_key(_derivation_cmp)):
        name = d.get("name") or f"{d.get('pname')}-{d.get('version', '')}"
        lines += ["", "-" * 72, name, ""]
        if d.get("derivation"):
            lines.append(str(d["derivation"]))
        lines.append(f"{'CVE':50} {'CVSSv3':<8} Description")
        cves = sorted(d["affected_by"], key=lambda c: (-(score(d, c) or 0), _cve_key(c)))
        for c in cves:
            s = score(d, c)
            row = f"https://nvd.nist.gov/vuln/detail/{c:17} {s if s is not None else '':<8} {describe(d, c)}"
            lines.append(row.rstrip())
        masked = d.get("whitelisted") or []
        if masked:
            lines.append(f"(+{len(masked)} whitelisted on this derivation: "
                         f"{', '.join(sorted(masked, key=_cve_key))})")
    return "\n".join(lines) + "\n"


def whitelisted_count(entries):
    return sum(len(d.get("whitelisted") or []) for d in entries)


def expected_rc(entries):
    if any(d["affected_by"] for d in entries):
        return 2
    return 1 if whitelisted_count(entries) else 0


def parse_rc(value):
    return int(value) if isinstance(value, str) and re.fullmatch(r"[0-9]+", value.strip()) else None


def classify(entries, reason, rc):
    """(state, detail) -- detail is the reason for failed/suspicious, else ''."""
    if entries is None:
        if rc is None and reason.startswith("could not read"):
            return "not run", reason
        return "failed", reason
    active = any(d["affected_by"] for d in entries)
    exp = expected_rc(entries)
    mismatch = rc is not None and rc != exp
    if active:
        return "findings", (f"vulnix exited {rc} but the JSON implies {exp}" if mismatch else "")
    if mismatch:
        return "suspicious", (f"vulnix exited {rc}, but its JSON has no findings past the "
                              f"whitelist, which should be exit {exp}.  With vulnix 1.12.4, "
                              "exit 2 and no findings means a failure such as unreachable "
                              "NVD feeds")
    if not whitelisted_count(entries):
        return "suspicious", ("vulnix matched nothing at all: no findings and no whitelisted "
                              "CVEs.  This closure always carries whitelisted CVEs (glibc, "
                              "gcc, zlib), so vulnix most likely did not scan it.  It silently "
                              "drops closure paths whose deriver .drv it cannot find")
    return "clean", ""


def summary(entries, reason, report, rc=None):
    state, detail = classify(entries, reason, rc)
    rc_note = f" vulnix exited {rc}." if rc is not None else ""
    out = ["## vulnix CVE scan", ""]
    if state == "not run":
        out += [
            "**The scan step did not run**, so there is no result at all.",
            "Check which earlier step failed: the renderer test or the",
            "closure build.",
        ]
    elif state == "failed":
        out += [
            "**The scan did not produce a usable report**, so this is not a",
            f"clean result: {detail}.{rc_note}",
            "",
            "Check the \"Scan closure against NVD\" step log. The usual cause",
            "is the NVD feeds being unreachable. The job stays green because",
            "this scan is advisory.",
        ]
    elif state == "suspicious":
        out += [
            "**This is not a clean result**, even though nothing was reported:",
            f"{detail}.",
            "",
            "Check the \"Scan closure against NVD\" step log, or re-run vulnix",
            "locally with `-v` against the same closure to see what it skipped.",
            "The job stays green because this scan is advisory.",
        ]
    elif state == "clean":
        out += [
            "No findings past the whitelist in the runtime closure of the",
            f"`.#nixos` build. {whitelisted_count(entries)} whitelisted CVE reference(s) across",
            f"{len(entries)} derivation(s) are hidden; the uploaded JSON",
            "artifact lists them.",
        ]
    else:
        body = report.encode("utf-8")[:SUMMARY_CAP].decode("utf-8", "ignore")
        # A description containing a fence would close the code block early.
        body = body.replace("```", "'''")
        out += [
            "Findings in the runtime closure of the `.#nixos` build,",
            "after filtering through `.github/vulnix-whitelist.toml`:",
            "",
            "```",
            body.rstrip("\n"),
            "```",
            "",
            "Everything here is either new or on a path the engine can",
            "actually be made to parse — attacker-supplied map, model,",
            "texture, music and soundfont files.  Closure-only packages",
            f"are filtered out by the whitelist ({whitelisted_count(entries)} CVE reference(s) hidden),",
            "each with a written reason; the uploaded JSON artifact still",
            "lists them.",
            "",
            "If a finding here is unreachable, add it to the whitelist",
            "with the reason rather than ignoring the report.",
        ]
        if detail:
            out += ["", f"Note: {detail}; the findings above are still what the JSON says."]
    return "\n".join(out) + "\n"


def main(argv, env=None):
    env = os.environ if env is None else env
    if len(argv) != 3:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    rc = parse_rc(env.get("VULNIX_RC"))
    entries, reason = load(argv[1])
    state, detail = classify(entries, reason, rc)
    if entries is None:
        report = f"vulnix produced no usable report ({state}): {detail}.\n"
    else:
        report = render(entries)
        if state == "suspicious":
            report = f"NOT A CLEAN RESULT: {detail}.\n\n" + report
    with open(argv[2], "w", encoding="utf-8") as f:
        f.write(report)
    sys.stdout.write(summary(entries, reason, report, rc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
