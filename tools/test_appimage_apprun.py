#!/usr/bin/env python3
"""appimage/AppRun picks the basedir the way the Linux zip is used (#173).

Runs the real AppRun against a stub tree whose glhexen2 echoes its arguments:
  - data1/pak0.pak beside the .AppImage  -> -basedir <that dir>
  - otherwise ~/.hexen2/data1/pak0.pak   -> no -basedir (the engine's
    userdir finds the paks, as in the Flatpak)
  - neither                              -> a message naming both places and
    --get-demo, exit 1, engine not started
  - an explicit -basedir, -help, -version -> passed straight through
  - --get-demo [DIR]                     -> get_demo.sh into DIR or ~/.hexen2
And the flake's appimage output pins a tagged type2-runtime, not "continuous".

Verified end to end on the real AppImage in a distro-like FHS: retail data
beside it loads the bundled gamecode from inside the image and renders
demo1; the demo data loads Raven's demo progs; ~/.hexen2 data runs; no data
prints the message and exits.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
errors = []

def run(args, appimage_dir=None, home_data=False):
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        app = t / "AppDir"
        (app / "usr/bin").mkdir(parents=True)
        shutil.copy(ROOT / "appimage/AppRun", app / "AppRun")
        (app / "usr/bin/glhexen2").write_text('#!/bin/sh\necho "ENGINE $*"\n')
        (app / "usr/bin/get_demo.sh").write_text('#!/bin/sh\necho "GETDEMO $*"\n')
        for f in ("AppRun", "usr/bin/glhexen2", "usr/bin/get_demo.sh"):
            os.chmod(app / f, 0o755)
        home = t / "home"
        home.mkdir()
        if home_data:
            (home / ".hexen2/data1").mkdir(parents=True)
            (home / ".hexen2/data1/pak0.pak").write_bytes(b"P")
        game = t / "game"
        game.mkdir()
        if appimage_dir == "data":
            (game / "data1").mkdir()
            (game / "data1/pak0.pak").write_bytes(b"P")
        env = {"PATH": "/usr/bin:/bin:" + os.environ.get("PATH", ""), "HOME": str(home),
               "APPIMAGE": str(game / "H.AppImage")}
        r = subprocess.run(["sh", str(app / "AppRun")] + args, env=env, capture_output=True, text=True)
        return r.returncode, r.stdout.replace(str(t), "$T") + r.stderr.replace(str(t), "$T")

rc, out = run([], appimage_dir="data")
if "ENGINE -basedir $T/game" not in out:
    errors.append(f"data beside the AppImage must become -basedir: {out!r}")
rc, out = run(["+map", "demo1"], home_data=True)
if "ENGINE +map demo1" not in out or "-basedir" in out:
    errors.append(f"~/.hexen2 data must run the engine without -basedir: {out!r}")
rc, out = run([])
if rc == 0 or "ENGINE" in out or "--get-demo" not in out or "$T/home/.hexen2/data1/" not in out:
    errors.append(f"no data must explain both places and --get-demo, exit nonzero: rc={rc} {out!r}")
for flag in ("-help", "-version"):
    rc, out = run([flag])
    if f"ENGINE {flag}" not in out:
        errors.append(f"{flag} must pass straight through: {out!r}")
rc, out = run(["-basedir", "/x"], appimage_dir="data")
if "ENGINE -basedir /x" not in out or "$T/game" in out:
    errors.append(f"an explicit -basedir must win: {out!r}")
rc, out = run(["--get-demo"])
if "GETDEMO $T/home/.hexen2" not in out:
    errors.append(f"--get-demo must fetch into ~/.hexen2: {out!r}")
rc, out = run(["--get-demo", "/d"])
if "GETDEMO /d" not in out:
    errors.append(f"--get-demo DIR must fetch into DIR: {out!r}")

flake = (ROOT / "flake.nix").read_text()
if "type2-runtime/releases/download/20251108/runtime-x86_64" not in flake or "download/continuous" in flake:
    errors.append("the AppImage runtime must be pinned to a tagged type2-runtime release")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS AppImage AppRun: beside-data, ~/.hexen2, no-data, pass-through, --get-demo (#173)")
