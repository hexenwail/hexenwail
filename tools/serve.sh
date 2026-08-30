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
#   <outdir>/home/          the sandboxed $HOME the run wrote to
#
# Environment:
#   ENGINE    path to h2ded   (default: ./result-h2ded/bin/h2ded)
#   TIMEOUT   hard kill after this many seconds (default: 120)
#
# Example:
#   nix build .#h2ded-bundled -o result-h2ded
#   nix build .#demodata      -o result-demodata
#   printf 'map demo1\nwait 2\nstatus\n' | \
#     tools/serve.sh /tmp/out result-demodata/share/hexenwail
#
# Requires: bwrap (bubblewrap).  Nothing else.
#
set -uo pipefail

usage() { sed -n '2,42p' "$0"; exit 2; }
[ $# -ge 2 ] || usage

OUT="$1"; shift
BASEDIR="$1"; shift

ENGINE=${ENGINE:-./result-h2ded/bin/h2ded}
TIMEOUT=${TIMEOUT:-120}

command -v bwrap >/dev/null 2>&1 || { echo "serve: missing bwrap" >&2; exit 3; }
[ -x "$ENGINE" ]  || { echo "serve: no h2ded at $ENGINE (set ENGINE=)" >&2; exit 3; }
[ -d "$BASEDIR" ] || { echo "serve: no basedir at $BASEDIR" >&2; exit 3; }

# readlink -f, not `cd && pwd`: both of these are normally `nix build -o`
# symlinks into the read-only store, and bwrap cannot create a --ro-bind mount
# point underneath a symlink it did not resolve.  It fails with the misleading
# "Can't mkdir parents for <path>: No such file or directory".
ENGINE=$(readlink -f "$ENGINE")
BASEDIR=$(readlink -f "$BASEDIR")
ENGINE_DIR=$(dirname "$ENGINE")

rm -rf "$OUT"; mkdir -p "$OUT/home"
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

# h2ded writes config and qconsole.log under the userdir, which for a fresh
# throwaway $HOME is now $HOME/.local/share/hexen2 -- uhexen2-7b1s moved it
# there and the legacy $HOME/.hexen2 only wins when it already exists, which it
# never does here.  XDG_DATA_HOME is pinned below so a host value cannot
# redirect the run out of the sandbox.  Bind a throwaway dir
# over $HOME so a run cannot touch the real one, then re-bind the data and the
# binary read-only ON TOP: a source checkout usually puts both inside $HOME,
# and without this the sandbox hides the very files it is about to open.
feed | timeout "$TIMEOUT" \
  bwrap --dev-bind / / \
        --bind "$OUT/home" "$HOME" \
        --ro-bind "$BASEDIR" "$BASEDIR" \
        --ro-bind "$ENGINE_DIR" "$ENGINE_DIR" \
    env HOME="$HOME" XDG_DATA_HOME="$HOME/.local/share" \
    "$ENGINE" -basedir "$BASEDIR" -condebug "$@" \
  > "$OUT/engine.stdout" 2>&1
rc=$?

cp "$OUT/home/.local/share/hexen2/qconsole.log" "$OUT/qconsole.log" 2>/dev/null
if [ $rc -eq 124 ]; then
  echo "serve: TIMED OUT after ${TIMEOUT}s -- the server never quit." >&2
  echo "       see $OUT/qconsole.log" >&2
elif [ $rc -ne 0 ]; then
  echo "serve: h2ded exited $rc -- see $OUT/qconsole.log" >&2
fi
echo "done -> $OUT (rc=$rc)"
exit $rc
