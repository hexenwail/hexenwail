#!/usr/bin/env python3
"""PR_ClassifyGamecode() must name every Raven release it has a row for, and
nothing else (GitHub #160).

pr_edict.c's pr_raven_images[] maps (whole-file CRC, progdefs CRC) to a
release.  This lifts that table and the whole of PR_ClassifyGamecode() out of
engine/h2shared/pr_edict.c verbatim, compiles them against stubs of the engine
state and of the two marker lookups (PR_GamecodeStamp, ED_FindFunctioni), and
checks:

  - every one of the 65536 file CRCs under every progdefs generation in
    progdefs.h: the KNOWN_RAVEN rows of tools/progs_crc.py get their release,
    everything else -- the measured mods included -- gets "Third-party".  So
    a row needs BOTH CRCs, the C and Python tables hold exactly the same rows,
    and no special case outside the table can hide;
  - no (file, progdefs) pair appears twice;
  - the hexenwail tiers on a non-Raven image: a "_YYYYMMDD" marker gives
    "hexenwail-YYYY-MM-DD", a malformed or bare marker "hexenwail (undated)",
    and BadBackpackDump alone "hexenwail (undated)";
  - a Raven image still reads as Raven with a marker present (the table is
    asked first).

With HCODE_ARCHIVE set to a checkout of sezero's uhexen2-hcode_archive, it also
re-reads every archive-sourced row from that row's bin/ image, so the numbers
can be re-derived rather than trusted.  An optional argument checks another
copy of pr_edict.c, for showing the test fails on a broken classifier.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import progs_crc  # noqa: E402

PR_EDICT = ROOT / "engine/h2shared/pr_edict.c"
PROGDEFS = ROOT / "engine/hexen2/progdefs.h"

# Whole-file and progdefs CRCs of mods measured in docs/BUNDLED_GAMECODE.md's
# verification matrix.  None is Raven's; the exhaustive sweep covers them, and
# this keeps them from being added to KNOWN_RAVEN by mistake.
MODS = {
    "sot": (28154, 26905),
    "soc": (58269, 26905),
    "karma2": (22850, 26905),
    "GameOfTomes": (41534, 38488),
}

# (marker tail or None, BadBackpackDump present, expected) for a non-Raven image.
TIERS = [
    (None, False, "Third-party"),
    ("_20260815", False, "hexenwail-2026-08-15"),
    ("_2026081", False, "hexenwail (undated)"),
    ("_202608150", False, "hexenwail (undated)"),
    ("_20260815x", False, "hexenwail (undated)"),
    ("_2026O815", False, "hexenwail (undated)"),
    ("", False, "hexenwail (undated)"),
    (None, True, "hexenwail (undated)"),
]


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def lift(src):
    m = re.search(r"static const struct \{[^}]*\} pr_raven_images\[\] = \{.*?\n\};",
                  src, re.S)
    if not m:
        fail("pr_raven_images[] not found in pr_edict.c")
    table = m.group(0)
    m = re.search(r"static const char \*PR_ClassifyGamecode \(void\)\n\{\n.*?\n\}\n",
                  src, re.S)
    if not m:
        fail("PR_ClassifyGamecode not found in pr_edict.c")
    func = m.group(0)
    if 'return "Third-party";' not in func:
        fail("lifted PR_ClassifyGamecode has no Third-party fallthrough")
    return table, func


def progdefs_defines():
    out = {}
    for name, val in re.findall(r"#define\s+(PROGS_V\d+_CRC)\s+\(?(\w+)\)?",
                                PROGDEFS.read_text()):
        out[name] = val
    # resolve aliases such as PROGS_V109_CRC -> (PROGS_V111_CRC)
    for k, v in list(out.items()):
        while not v.isdigit():
            v = out[v]
        out[k] = v
    return out


def cstr(s):
    return "NULL" if s is None else '"%s"' % s


def main():
    src_path = Path(sys.argv[1]) if len(sys.argv) > 1 else PR_EDICT
    table, func = lift(src_path.read_text())
    defines = progdefs_defines()
    known = progs_crc.KNOWN_RAVEN

    for name, key in MODS.items():
        if key in known:
            fail("mod %s %s is in KNOWN_RAVEN" % (name, key))
    for key in progs_crc.ARCHIVE_ROWS:
        if key not in known:
            fail("ARCHIVE_ROWS %s has no KNOWN_RAVEN row" % (key,))

    gens = sorted({int(v) for v in defines.values()})
    defs = "".join("#define %s %s\n" % (k, v) for k, v in defines.items())
    rows = ",\n".join("{%d, %d, %s}" % (f, g, cstr(r)) for (f, g), (r, _) in known.items())
    tiers = ",\n".join("{%s, %d, %s}" % (cstr(s), int(b), cstr(e)) for s, b, e in TIERS)
    prog = r'''
#include <stdio.h>
#include <string.h>
#define q_snprintf snprintf
%s
typedef struct dfunction_s { int unused; } dfunction_t;
static unsigned short pr_crc;
static struct { int crc; } progs_s, *progs = &progs_s;
static const char *stub_stamp;
static int stub_backpack;
static dfunction_t stub_fn;
static const char *PR_GamecodeStamp (void) { return stub_stamp; }
static dfunction_t *ED_FindFunctioni (const char *name)
{
	return (stub_backpack && !strcmp (name, "BadBackpackDump")) ? &stub_fn : NULL;
}
%s
%s
static const struct { int f, g; const char *want; } known[] = {
%s
};
static const struct { const char *stamp; int backpack; const char *want; } tiers[] = {
%s
};
static const int gens[] = { %s };
static const char *classify (int f, int g, const char *stamp, int backpack)
{
	pr_crc = (unsigned short)f;
	progs->crc = g;
	stub_stamp = stamp;
	stub_backpack = backpack;
	return PR_ClassifyGamecode ();
}
static const char *want_for (int f, int g)
{
	size_t	k;
	for (k = 0; k < sizeof(known) / sizeof(known[0]); k++)
		if (known[k].f == f && known[k].g == g)
			return known[k].want;
	return NULL;
}
int main (void)
{
	size_t	i, j, n = sizeof(pr_raven_images) / sizeof(pr_raven_images[0]);
	long	probes = 0;
	int	bad = 0, f;
	for (i = 0; i < sizeof(gens) / sizeof(gens[0]); i++)
		for (f = 0; f < 65536; f++)
		{
			const char *want = want_for (f, gens[i]);
			const char *got = classify (f, gens[i], NULL, 0);
			probes++;
			if (strcmp (got, want ? want : "Third-party"))
			{
				if (bad++ < 20)
					printf ("file %%5d progdefs %%5d -> %%s, want %%s\n", f,
						gens[i], got, want ? want : "Third-party");
			}
		}
	for (i = 0; i < sizeof(tiers) / sizeof(tiers[0]); i++)
	{
		/* 12345 under 38488 is not a Raven row (the sweep proves it) */
		const char *got = classify (12345, 38488, tiers[i].stamp, tiers[i].backpack);
		probes++;
		if (strcmp (got, tiers[i].want))
		{
			printf ("tier stamp=%%s backpack=%%d -> %%s, want %%s\n",
				tiers[i].stamp ? tiers[i].stamp : "(none)",
				tiers[i].backpack, got, tiers[i].want);
			bad++;
		}
	}
	for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
	{
		const char *got = classify (known[i].f, known[i].g, "_20260815", 1);
		probes++;
		if (strcmp (got, known[i].want))
		{
			printf ("Raven %%d/%%d with a marker -> %%s, want %%s\n",
				known[i].f, known[i].g, got, known[i].want);
			bad++;
		}
	}
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (pr_raven_images[i].file_crc == pr_raven_images[j].file_crc &&
			    pr_raven_images[i].progdefs_crc == pr_raven_images[j].progdefs_crc)
			{
				printf ("duplicate row %%u/%%u\n", pr_raven_images[i].file_crc,
					pr_raven_images[i].progdefs_crc);
				bad++;
			}
	printf ("rows %%u probes %%ld mismatches %%d\n", (unsigned)n, probes, bad);
	return bad != 0;
}
''' % (defs, table, func, rows, tiers, ", ".join(map(str, gens)))

    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / "t.c"
        exe = Path(d) / "t"
        c.write_text(prog)
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall",
                            "-Wextra", "-Werror", "-o", str(exe), str(c)],
                           capture_output=True, text=True)
        if r.returncode:
            print(r.stderr)
            fail("lifted classifier does not compile")
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end="")
        if r.returncode:
            fail("PR_ClassifyGamecode gave a wrong answer (listed above)")
        rows = int(re.search(r"rows (\d+)", r.stdout).group(1))
        if rows != len(known):
            fail("pr_raven_images[] has %d rows, KNOWN_RAVEN %d" % (rows, len(known)))

    archive = os.environ.get("HCODE_ARCHIVE")
    if archive:
        for key, rel in sorted(progs_crc.ARCHIVE_ROWS.items()):
            got = progs_crc.describe(os.path.join(archive, rel))
            if got != key:
                fail("%s reads %s, table says %s" % (rel, got, key))
            print("archive %-28s %5d %5d  %s" % (rel, got[0], got[1], known[key][0]))
    else:
        print("HCODE_ARCHIVE unset: archive rows not re-read")

    print("PASS")


if __name__ == "__main__":
    main()
