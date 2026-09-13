#!/usr/bin/env bash
#
# run_engine_smoke.sh -- run the REAL ENGINE with the Rust hashindex and with
# the C original, and diff the output.
#
# WHY THIS EXISTS, given the differential harness already passes:
#
#   The harness verifies hashindex as a MODULE, in isolation.  It says nothing
#   about the engine's actual use of it: the eight by-value embedding sites
#   (model.c, gl_model.c, draw.c, gl_draw.c, snd_dma.c, gl_rmisc.c and the two
#   members inside pack_t/zippack_t), the FS_LoadPackFile / FS_LoadZipFile
#   paths, or initialization order.  This runs the engine and compares.
#
#   It is the step to re-run before flipping USE_RUST_HASHINDEX to ON for
#   real, which is why it is a script and not a shell-history footnote.
#
# WHAT IT COVERS
#
#   h2ded   -- startup pak load.  FS_LoadPackFile calls Hash_Allocate and then
#              Hash_Add once per entry: 797 of them against the Nov 1997 demo
#              pak.  This is the densest hashindex traffic in the server.
#   glhexen2 -- the same pak load plus a real map load, which also drives the
#              MODEL and SOUND tables (model.c hash_mod, snd_dma.c hash_sfx)
#              and the texture table (gl_draw.c).  Those three are only
#              reachable in the client -- h2ded does not compile them.
#
# NOT covered: map/pak content beyond the demo, and pak loading on the
# dedicated HexenWorld server (hwsv), which needs hw/pak4.pak.
#
# USAGE
#
#   DEMO_DIR=<dir containing data1/> \
#   ON_BIN=<glhexen2 built with -DUSE_RUST_HASHINDEX=ON> \
#   OFF_BIN=<glhexen2 built with -DUSE_RUST_HASHINDEX=OFF> \
#   XVFB=<path to Xvfb> \
#   ./run_engine_smoke.sh
#
# The h2ded/hwsv binaries are taken from the same directories as ON_BIN/OFF_BIN.
# Requires Xvfb and the engine's runtime libraries, i.e. run it inside
# `nix develop`.  Exits non-zero on any behavioural difference.
#
# Both engines write config.cfg into their userdir, so each run gets a FRESH
# separate userdir.  Sharing one makes the second run pick up the first's
# config, which exec's keybindings and prints lines the other never had -- a
# difference that looks like a regression and is not.
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

: "${DEMO_DIR:?set DEMO_DIR to the basedir holding data1/}"
: "${ON_BIN:?set ON_BIN to the flag-ON glhexen2}"
: "${OFF_BIN:?set OFF_BIN to the flag-OFF glhexen2}"
XVFB="${XVFB:-Xvfb}"
WORK="${WORK:-$(mktemp -d)}"
mkdir -p "$WORK"
ON_DIR="$(dirname "$ON_BIN")"
OFF_DIR="$(dirname "$OFF_BIN")"

command -v "$XVFB" >/dev/null || { echo "error: no Xvfb at '$XVFB'" >&2; exit 2; }
[ -x "$ON_BIN" ]  || { echo "error: no binary at ON_BIN=$ON_BIN"  >&2; exit 2; }
[ -x "$OFF_BIN" ] || { echo "error: no binary at OFF_BIN=$OFF_BIN" >&2; exit 2; }
[ -d "$DEMO_DIR/data1" ] || { echo "error: $DEMO_DIR/data1 not found" >&2; exit 2; }

# Version/userdir/basedir necessarily differ between the two binaries (they are
# different builds, from different directories).  Everything else must match.
normalize() {
	sed -E \
		-e 's/^Hexenwail .*\(Linux\)/Hexenwail <VER> (Linux)/' \
		-e 's#^basedir is:.*#basedir is: <BD>#' \
		-e 's#^userdir is:.*#userdir is: <UD>#' \
		-e 's#^Exe:.*#Exe: <STAMP>#' "$1"
}

# run <tag> <bindir> <extra-args...>
run() {
	local tag="$1" bindir="$2"; shift 2
	local ud="$WORK/ud-$tag"
	local dpy=$(( 90 + RANDOM % 8 ))
	rm -rf "$ud"; mkdir -p "$ud/data1"
	printf 'map demo1\n' > "$ud/data1/autoexec.cfg"

	"$XVFB" ":$dpy" -screen 0 800x600x24 >/dev/null 2>&1 &
	local xp=$!
	sleep 3

	# -k 5: the engine ignores SIGTERM during shutdown.
	DISPLAY=":$dpy" timeout -k 5 45 stdbuf -o0 \
		"$bindir/glhexen2" -basedir "$DEMO_DIR" -userdir "$ud" \
		+map demo1 > "$WORK/gl-$tag.log" 2>&1 || true
	# The dedicated server needs no display and no input.
	timeout -k 5 25 stdbuf -o0 \
		"$bindir/h2ded" -basedir "$DEMO_DIR" -userdir "$ud" \
		+map demo1 > "$WORK/h2-$tag.log" 2>&1 || true
	kill "$xp" 2>/dev/null || true
	wait "$xp" 2>/dev/null || true
}

echo "== running OFF (C hashindex) =="
run off "$OFF_DIR"
echo "== running ON (Rust hashindex) =="
run on "$ON_DIR"

fail=0
for which in gl h2; do
	normalize "$WORK/$which-off.log" > "$WORK/$which-off.norm"
	normalize "$WORK/$which-on.log"  > "$WORK/$which-on.norm"
	label=$([ "$which" = gl ] && echo glhexen2 || echo h2ded)
	lines=$(wc -l < "$WORK/$which-on.norm")

	if diff -u "$WORK/$which-off.norm" "$WORK/$which-on.norm" > "$WORK/$which.diff"; then
		echo "  $label: IDENTICAL ($lines lines)"
	else
		echo "  $label: DIFFERS" >&2
		head -40 "$WORK/$which.diff" >&2
		fail=1
	fi

	# Guard against a vacuous pass: an engine that died at startup would
	# produce two empty logs that diff equal.
	if ! grep -q 'Added packfile' "$WORK/$which-on.norm"; then
		echo "  $label: no 'Added packfile' line -- did the engine actually start?" >&2
		fail=1
	fi
done

# The client must have reached a map, or it never exercised the model, sound
# and texture tables -- the call sites h2ded cannot reach at all.
if grep -q 'Precache:' "$WORK/gl-on.norm"; then
	echo "  glhexen2: map loaded ($(grep -o 'Precache: [0-9]*/[0-9]* models, [0-9]*/[0-9]* sounds' "$WORK/gl-on.norm" | head -1))"
else
	echo "  glhexen2: NO map load -- the model/sound/texture hash tables were not exercised" >&2
	fail=1
fi

if [ "$fail" -ne 0 ]; then
	echo "FAIL: engine output differs between the Rust and C hashindex" >&2
	exit 1
fi

echo
echo "PASS: the engine behaves identically with the Rust hashindex and the C"
echo "      original, on a real pak load and a real map load."
