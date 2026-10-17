#!/usr/bin/env python3
"""headless-drive.sh keeps the engine's userdir where it reads it back.

The sandbox binds a throwaway dir over $HOME.  Anything the script hands the
engine by path and later reads back must be bound again AFTER that, or an
outdir under $HOME (a job's scratch dir) puts it behind the HOME bind: the
userdir then landed in $WORK/home/<path>/userdir, qconsole.log was never
copied out, and hw-drive.sh reported "the client never signed on" for a
session that had.  Same rule the basedir and engine binds already follow.
"""
from pathlib import Path
import re
import sys

src = (Path(__file__).resolve().parents[1] / "tools/headless-drive.sh").read_text()
m = re.search(r"bwrap --dev-bind / / \\\n(.*?)\n  env -u LD_LIBRARY_PATH", src, re.S)
if not m:
    sys.exit("FAIL: headless-drive.sh: no engine bwrap invocation found")
binds = re.findall(r'--(?:ro-)?bind "([^"]+)" "([^"]+)"', m.group(1))
targets = [dst for _, dst in binds]
errors = []
if "$HOME" not in targets:
    errors.append("the engine sandbox no longer binds a throwaway dir over $HOME")
else:
    home = targets.index("$HOME")
    for need in ("$WORK/userdir", "$BASEDIR", "$ENGINE_DIR"):
        if need not in targets:
            errors.append(f"{need} is not bound into the engine sandbox")
        elif targets.index(need) < home:
            errors.append(f"{need} is bound before $HOME, so the HOME bind hides it")
if '-userdir "$WORK/userdir"' not in src:
    errors.append("the engine is no longer pointed at $WORK/userdir")
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS the engine sandbox re-binds userdir, basedir and engine over the HOME bind")
