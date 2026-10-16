#!/usr/bin/env python3
"""hcc must not end the globals block before the engine stops reading it (#120).

Two reads in a compiled image run past the last global hcc allocates:

  - OP_RETURN, OP_DONE, the OP_CALLn argument copies and the vector opcodes
    read three globals from an operand whatever its type, so a final 1-wide
    global is read as the first of three.
  - a builtin allocates nothing, but its function record puts parm_start
    where the globals stood when it was declared, and its parameter words
    are checked from there.

hcc used to end the block at the last global it allocated, so a small image
that ends on either (a HUD-only csprogs.dat) was refused by the engine's
PR_ValidateBytecode:

    CSQC: ignoring csprogs.dat: statement 2 reaches global 32 of 33
    CSQC: ignoring csprogs.dat: function 2 has 0 locals at global 32 of 32

This test compiles the two fixtures in tools/hcc_tail_slack/ in both progs
formats and applies the engine's rules to them:

  - every statement operand, with the operand roles read from pr_operandrole
    in engine/h2shared/pr_edict.c, so the check cannot drift from the engine;
  - every function record's parm_start + max(locals, parameter words).

Any extra progs.dat paths given are checked the same way.

Usage:
  test_hcc_tail_slack.py HCC [progs.dat ...]
"""

from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from qcdis import OPS, Progs  # noqa: E402

OP_RETURN = OPS.index("RETURN")

FIXTURE = ROOT / "tools/hcc_tail_slack"
PR_EDICT = ROOT / "engine/h2shared/pr_edict.c"
WIDTH = {"PRO_VECTOR": 3, "PRO_GLOBAL": 1}


def operand_roles():
    """pr_operandrole rows, in opcode order, as tuples of role names."""
    text = PR_EDICT.read_text()
    m = re.search(r"pr_operandrole\[\]\[3\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        sys.exit("FAIL: pr_operandrole table not found in %s" % PR_EDICT)
    rows = re.findall(r"\{\s*(PRO_\w+)\s*,\s*(PRO_\w+)\s*,\s*(PRO_\w+)\s*\}",
                      re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S))
    # One row per opcode, exactly: a row the regex missed would shift every
    # later opcode onto its neighbour's roles, and the check would pass
    # without checking anything.
    if len(rows) != len(OPS):
        sys.exit("FAIL: parsed %d pr_operandrole rows, expected %d (one per "
                 "opcode in qcdis.OPS)" % (len(rows), len(OPS)))
    return rows


def function_records(progs):
    """(first_statement, parm_start, locals, parmwords) per function."""
    out = []
    for i in range(progs.n_fn):
        o = progs.ofs_fn + i * 36
        f = struct.unpack("<7i8B", progs.buf[o:o + 36])
        numparms = max(0, min(f[6], 8))
        out.append((f[0], f[1], f[2], sum(f[7:7 + numparms])))
    return out


def extent_errors(progs, roles):
    """PR_ValidateBytecode's statement-operand and function-record rules."""
    errors = []
    for i, (op, *operands) in enumerate(progs.statements):
        if op >= len(roles):
            errors.append("statement %d has opcode %d" % (i, op))
            continue
        for role, ofs in zip(roles[op], operands):
            width = WIDTH.get(role)
            if width and ofs > progs.n_gl - width:
                errors.append("statement %d reaches global %d of %d (%s)"
                              % (i, ofs, progs.n_gl, role))
    for i, (_, parm_start, locals_, parmwords) in \
            enumerate(function_records(progs)):
        if i == 0:
            continue
        if parm_start < 0 or locals_ < 0 or \
                max(locals_, parmwords) > progs.n_gl - parm_start:
            errors.append("function %d has %d locals at global %d of %d "
                          "(parmwords %d)"
                          % (i, locals_, parm_start, progs.n_gl, parmwords))
    return errors


def allocated_top(progs, roles):
    """Highest global the image names: a def or a non-jump operand."""
    top = max((d[1] for d in progs.globaldefs), default=-1)
    for op, *operands in progs.statements:
        for role, ofs in zip(roles[op], operands):
            if role in WIDTH:
                top = max(top, ofs)
    return top


def compile_fixture(hcc, version_flag, work, sources, out_name):
    """Compile the first of `sources` this compiler accepts."""
    src = Path(work) / "src"
    shutil.copytree(FIXTURE, src)
    out = Path(work) / out_name
    logs = []
    for name in sources:
        r = subprocess.run([hcc, "-src", str(src), "-name", name,
                            version_flag], capture_output=True, text=True)
        if r.returncode == 0 and out.exists():
            return out
        logs.append("%s:\n%s%s" % (name, r.stdout[-800:], r.stderr))
    sys.exit("FAIL: %s %s compiled none of %s:\n%s"
             % (hcc, version_flag, sources, "\n".join(logs)))


def guard_return(progs, roles):
    """The returned value must be the last global the image names."""
    returned = [st[1] for st in progs.statements if st[0] == OP_RETURN]
    top = allocated_top(progs, roles)
    if not returned or max(returned) != top:
        sys.exit("FAIL: return fixture no longer returns its last global "
                 "(returns %r, highest named %d)" % (returned, top))
    return "OP_RETURN operand %d" % max(returned)


def guard_builtin(progs, roles):
    """The last function must be a 3-word builtin past every named global."""
    _, parm_start, _, parmwords = function_records(progs)[-1]
    top = allocated_top(progs, roles)
    if function_records(progs)[-1][0] >= 0 or parmwords != 3 \
            or parm_start <= top:
        sys.exit("FAIL: builtin fixture no longer ends on a vector builtin "
                 "(last function %r, highest named global %d)"
                 % (function_records(progs)[-1], top))
    return "builtin parm_start %d, %d parm words" % (parm_start, parmwords)


FIXTURES = (
    ("return", ["progs.src"], "csprogs.dat", guard_return),
    ("builtin", ["builtin.src", "builtin_qcc.src"], "builtin.dat",
     guard_builtin),
)


def report(label, errs):
    print("%s: %s" % (label, "ok" if not errs else "BAD"))
    for e in errs:
        print("  " + e)
    return bool(errs)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    hcc, extra = argv[1], argv[2:]
    roles = operand_roles()
    failed = False

    for name, sources, out_name, guard in FIXTURES:
        for flag, version in (("-v6", 6), ("-v7", 7)):
            with tempfile.TemporaryDirectory() as work:
                progs = Progs(str(compile_fixture(hcc, flag, work, sources,
                                                  out_name)))
                if progs.version != version:
                    sys.exit("FAIL: %s wrote version %d"
                             % (flag, progs.version))
                # Each fixture exists to end the image on one of the two
                # reads.  If hcc lays it out differently the test would pass
                # without testing anything, so the guard refuses that.
                what = guard(progs, roles)
                failed |= report("%s %s: numglobals %d, %s"
                                 % (name, flag, progs.n_gl, what),
                                 extent_errors(progs, roles))

    for path in extra:
        failed |= report(path, extent_errors(Progs(path), roles))

    if failed:
        sys.exit("FAIL: hcc ended the globals block before the engine "
                 "stops reading it")
    print("PASS")


if __name__ == "__main__":
    main(sys.argv)
