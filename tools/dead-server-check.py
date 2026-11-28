#!/usr/bin/env python3
#
# dead-server-check.py -- live check that joining a server that never answers
# ends the loading plaque, for HexenWorld and plain Hexen II.
#
#   nix shell nixpkgs#xorg-server nixpkgs#xdotool nixpkgs#imagemagick \
#       nixpkgs#bubblewrap --command ./tools/dead-server-check.py [basedir]
#   ENGINE= the client (default ./result/bin/hexenwail)
#
# A UDP socket on 127.0.0.1 swallows every packet.  The client, under
# tools/headless-drive.sh (Xvfb, bwrap'd $HOME, its own -userdir), types
# `connect hw://127.0.0.1:<port>` and, in a second run, `connect
# 127.0.0.1:<port>`.  Each join must end within LIMIT seconds (default 35) of
# the connect: by the handshake's own error or by the plaque's 25 s "load
# timeout.".  A connect retry is the client resending, not the server
# answering, so it must never hold the plaque up: HexenWorld resends every
# 5 s, and before the fix each "(retry N)" restarted the load timeout.
#
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
basedir = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/hexen2")
engine = os.environ.get("ENGINE", os.path.join(ROOT, "result/bin/hexenwail"))
limit = float(os.environ.get("LIMIT", "35"))
ENDED = ("No HexenWorld response", "load timeout.", "connect failed",
         "Host_Error", "No Response", "CL_EstablishConnection: connect failed")

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 0))
port = sock.getsockname()[1]
seen = [0]
def swallow():
    while True:
        try:
            sock.recvfrom(8192)
            seen[0] += 1
        except OSError:
            return
threading.Thread(target=swallow, daemon=True).start()

def run(label, target):
    out = tempfile.mkdtemp(prefix=f"dead-server-{label}.")
    steps = os.path.join(out, "steps")
    with open(steps, "w") as f:
        f.write("sleep 15\nconsole\nenter connect %s\nsleep %d\n" % (target, int(limit) + 15))
    env = dict(os.environ, ENGINE=engine, STEPS=steps)
    p = subprocess.Popen([os.path.join(ROOT, "tools/headless-drive.sh"), "script",
                          os.path.join(out, "run"), basedir],
                         env=env, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    log = os.path.join(out, "run", "work", "userdir", "qconsole.log")
    t_connect = t_end = None
    end_line = None
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < limit + 120:
        try:
            text = open(log, encoding="latin-1").read()
        except FileNotFoundError:
            text = ""
        now = time.time()
        if t_connect is None and ("]connect " in text):
            t_connect = now
            mark = len(text)
        if t_connect is not None and t_end is None:
            for line in text[mark:].splitlines():
                if any(k in line for k in ENDED):
                    t_end, end_line = now, line.strip()
                    break
        time.sleep(0.1)
    p.wait()
    if t_connect is None:
        return f"{label}: FAIL, the connect never ran (see {out})", False
    if t_end is None:
        return f"{label}: FAIL, plaque still up {limit:.0f}s after connect (see {out})", False
    dt = t_end - t_connect
    ok = dt <= limit
    return (f"{label}: {'PASS' if ok else 'FAIL'}, ended {dt:.1f}s after connect: "
            f"\"{end_line}\""), ok

results = [run("hexenworld", f"hw://127.0.0.1:{port}"),
           run("hexen2", f"127.0.0.1:{port}")]
sock.close()
for msg, _ in results:
    print(msg)
print(f"(the dead server swallowed {seen[0]} packets)")
sys.exit(0 if all(ok for _, ok in results) else 1)
