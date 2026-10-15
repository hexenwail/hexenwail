#!/usr/bin/env bash
#
# serve.sh — Drive the dedicated server (h2ded) with no X server at all.
#
# The fast lane.  h2ded is the SERVERONLY half of the engine: no renderer, no
# video, no sound, no input.  It links only libm/libc, boots in about a second,
# and takes console commands on stdin — so for anything below the renderer
# (gamecode, physics, savegames, protocol, filesystem, cvar plumbing) this
# beats headless-drive.sh by two orders of magnitude of wall-clock and needs
# neither Xvfb nor xdotool.
#
# It cannot tell you what anything LOOKS like.  For that use
# tools/headless-drive.sh, the other half of this pair.
#
# Usage:
#   serve.sh <outdir> <basedir> [extra engine args...]   < commands
#
# Commands arrive on stdin, one console command per line.  Two extras are
# handled here rather than by the engine:
#   wait N      sleep N seconds before sending the next line.  The engine's own
#               `wait` command yields a single frame, which is never what you
#               want when you are waiting on a map to finish loading.
#   # ...       comment; blank lines ignored
# A final `quit` is appended for you — without one h2ded runs forever.
#
# Output:
#   <outdir>/qconsole.log   everything the server printed.  READ THIS: h2ded
#                           prints nothing to stdout when stdout is not a tty.
#   <outdir>/userdir/       the engine's userdir for this run (-userdir)
#
# Environment:
#   ENGINE    path to h2ded   (default: ./result-h2ded/bin/h2ded)
#   TIMEOUT   hard kill after this many seconds (default: 120)
#   NO_BWRAP  set to 1 to skip bubblewrap even when it is installed
#
# Example:
#   nix build .#h2ded-bundled -o result-h2ded
#   nix build .#demodata      -o result-demodata
#   printf 'map demo1\nwait 2\nstatus\n' | \
#     tools/serve.sh /tmp/out result-demodata/share/hexenwail
#
# Requires: nothing beyond the engine.  bwrap (bubblewrap) is used when present,
# for extra containment; see the comment above the launch.
#
set -uo pipefail

usage() { sed -n '2,44p' "$0"; exit 2; }
[ $# -ge 2 ] || usage

OUT="$1"; shift
BASEDIR="$1"; shift

ENGINE=${ENGINE:-./result-h2ded/bin/h2ded}
TIMEOUT=${TIMEOUT:-120}

[ -x "$ENGINE" ]  || { echo "serve: no h2ded at $ENGINE (set ENGINE=)" >&2; exit 3; }
[ -d "$BASEDIR" ] || { echo "serve: no basedir at $BASEDIR" >&2; exit 3; }

# readlink -f, not `cd && pwd`: both of these are normally `nix build -o`
# symlinks into the read-only store, and bwrap cannot create a --ro-bind mount
# point underneath a symlink it did not resolve.  It fails with the misleading
# "Can't mkdir parents for <path>: No such file or directory".
ENGINE=$(readlink -f "$ENGINE")
BASEDIR=$(readlink -f "$BASEDIR")
ENGINE_DIR=$(dirname "$ENGINE")

rm -rf "$OUT"; mkdir -p "$OUT/userdir" "$OUT/home"
OUT=$(cd "$OUT" && pwd)

# Pace stdin, honouring `wait`, and always finish with a quit.
#
# bash's builtin printf writes straight through to the pipe, so each line
# reaches the engine when feed() emits it rather than when feed() exits -- do
# not "optimise" this into a single here-doc, which would collapse the whole
# script into one frame and silently discard every `wait`.
feed() {
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      ''|'#'*)  continue ;;
      'wait '*) sleep "${line#wait }"; continue ;;
    esac
    printf '%s\n' "$line"
  done
  sleep 1
  printf 'quit\n'
  sleep 1          # let quit be read before stdin hits EOF
}

# Containment.  -userdir is what keeps a run out of the real userdir: h2ded
# writes its config and qconsole.log there, and the flag is the one thing that
# can redirect it -- the engine reads the home directory from getpwuid(), not
# $HOME, and an existing ~/.hexen2 beats XDG_DATA_HOME (uhexen2-stbe, GitHub
# #131).  So it is passed on every run, and is enough on its own.
#
# bwrap, when installed, adds what -userdir cannot: a throwaway $HOME so
# nothing else the process might touch lands in the real one, and the basedir
# and binary bound read-only so a run cannot modify the game data.  The data and
# binary are re-bound ON TOP of the $HOME bind because a source checkout usually
# puts both inside $HOME, and without that the sandbox hides the very files it
# is about to open.  Without bwrap (CI images, kernels with unprivileged user
# namespaces disabled) the run still works; it is just less fenced in.
SANDBOX=()
if [ "${NO_BWRAP:-0}" != 1 ] && command -v bwrap >/dev/null 2>&1; then
  SANDBOX=(bwrap --dev-bind / /
                 --bind "$OUT/home" "$HOME"
                 --ro-bind "$BASEDIR" "$BASEDIR"
                 --ro-bind "$ENGINE_DIR" "$ENGINE_DIR")
else
  echo "serve: running without bwrap (-userdir only)" >&2
fi

feed | timeout "$TIMEOUT" \
  "${SANDBOX[@]}" "$ENGINE" -basedir "$BASEDIR" -userdir "$OUT/userdir" -condebug "$@" \
  > "$OUT/engine.stdout" 2>&1
rc=$?

cp "$OUT/userdir/qconsole.log" "$OUT/qconsole.log" 2>/dev/null
if [ $rc -eq 124 ]; then
  echo "serve: TIMED OUT after ${TIMEOUT}s -- the server never quit." >&2
  echo "       see $OUT/qconsole.log" >&2
elif [ $rc -ne 0 ]; then
  echo "serve: h2ded exited $rc -- see $OUT/qconsole.log" >&2
fi
echo "done -> $OUT (rc=$rc)"
exit $rc
