#!/usr/bin/env python3
#
# sp-loading-check.py -- live check that a slow single-player map load shows
# no multiplayer wording and no "load timeout." on the loading plaque.
#
#   ENGINE=./result/bin/hexenwail ./tools/sp-loading-check.py [basedir] [map]
#     basedir  a Hexen II install                 (default ~/hexen2)
#     map      a map in it                        (default demo1)
#
# The client runs offscreen (real GPU, no X) with `+map <map>`.  The moment
# the console log shows "SV_SpawnServer:" -- i.e. inside the blocking server
# side of the load -- the process is SIGSTOPped for STALL seconds (default 27)
# and resumed.  Wall clock runs on while it is stopped, exactly as through a
# long load on a slow machine (a cold cache, a big texture pack).
#
# PASS needs, from the console log (developer 1 logs every plaque stage as
# "loading: <text>"):
#   * no "load timeout." -- the 25 s timeout is for a server that stopped
#     answering, and nothing here is remote;
#   * no stage naming the network: "Connecting to local...", "Waiting for
#     server info", "Signon n/4", a retry;
#   * the load finished (CL_SignonReply reached 3).
#
# Exit 2 (INCONCLUSIVE) if the stop landed after the client connected.
#
# Master (0.8.0-beta.r35 plus #572) printed "load timeout." straight after the
# stall, at Host_Connect_f's plaque, then "Connecting to local...", "Waiting
# for server info" and "Signon 1/4" to "3/4".  The commit before #572 printed
# no timeout and had no stage text at all.
#
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

basedir = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/hexen2")
mapname = sys.argv[2] if len(sys.argv) > 2 else "demo1"
engine = os.environ.get("ENGINE", "./result/bin/hexenwail")
stall = float(os.environ.get("STALL", "27"))
NETWORDS = re.compile(r"connect|retry|signon|server info|waiting", re.I)

if not os.access(engine, os.X_OK):
    sys.exit(f"sp-loading-check: no engine at {engine} (set ENGINE=)")
if not os.path.isdir(basedir):
    sys.exit(f"sp-loading-check: no basedir at {basedir}")

out = tempfile.mkdtemp(prefix="sp-loading-check.")
ud = os.path.join(out, "userdir")
os.makedirs(ud)
env = dict(os.environ, SDL_VIDEODRIVER="offscreen", SDL_AUDIODRIVER="dummy")
# A nix-built engine carries its own rpath; a host value can kill it pre-main.
env.pop("LD_LIBRARY_PATH", None)
args = [engine, "-basedir", basedir, "-userdir", ud, "-window",
        "-width", "640", "-height", "480", "-nosound", "-condebug",
        "+developer", "1", "+map", mapname]
# The engine falls back to the passwd home for its userdir in some builds, so
# -userdir alone cannot keep it off the real ~/.hexen2: give it a throwaway
# $HOME, with the game data, the engine and our outdir bound back in.
if shutil.which("bwrap"):
    home = os.path.expanduser("~")
    fake = os.path.join(out, "home")
    os.makedirs(fake)
    eng_dir = os.path.dirname(os.path.realpath(engine))
    args = ["bwrap", "--dev-bind", "/", "/", "--die-with-parent",
            "--bind", fake, home,
            "--ro-bind", os.path.realpath(basedir), os.path.realpath(basedir),
            "--ro-bind", eng_dir, eng_dir,
            "--bind", out, out] + [os.path.realpath(engine)] + args[1:]
# Own process group: the stall stops the engine, not just bwrap above it.
p = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                     start_new_session=True)
log = os.path.join(ud, "qconsole.log")

def text():
    try:
        with open(log, encoding="latin-1") as f:
            return f.read()
    except FileNotFoundError:
        return ""

t0 = time.time()
stalled = False
late = None
done = None
try:
    while time.time() - t0 < 180 and p.poll() is None:
        t = text()
        if not stalled and "SV_SpawnServer:" in t:
            os.killpg(p.pid, signal.SIGSTOP)
            # The stop must land inside the server spawn, before the client
            # connects; past that point master passes too.
            late = re.search(r"CL_EstablishConnection: connected|CL_SignonReply:", text())
            time.sleep(stall)
            os.killpg(p.pid, signal.SIGCONT)
            stalled = True
        if stalled and done is None and "CL_SignonReply: 3" in t:
            done = time.time()
        if done and time.time() - done > 3:
            break
        time.sleep(0.05)
finally:
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)

t = text()
stages = [l[len("loading: "):] for l in t.splitlines() if l.startswith("loading: ")]
fails = []
if stalled and late:
    print(f"INCONCLUSIVE: the stall landed after the client connected (log in {out})")
    sys.exit(2)
if not stalled:
    fails.append("the load never reached SV_SpawnServer")
if done is None:
    fails.append("the load never reached CL_SignonReply 3")
if "load timeout." in t:
    fails.append('"load timeout." on a local load')
for s in stages:
    if NETWORDS.search(s):
        fails.append(f'network stage on a local load: "{s}"')

print(f"stall {stall:.0f} s inside SV_SpawnServer; plaque stages:")
for s in stages:
    print("  " + s)
if fails:
    print("FAIL (log kept in %s):\n  %s" % (out, "\n  ".join(fails)))
    sys.exit(1)
shutil.rmtree(out, ignore_errors=True)
print("PASS a slow single-player load shows no network stage and no load timeout")
