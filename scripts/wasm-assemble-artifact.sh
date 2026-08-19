#!/usr/bin/env bash
# Assembles the static PWA artifact (the "dist" directory) from the built
# Emscripten output plus the web/ PWA shell.
#
# Shared by the PR "PWA / WebAssembly" check, which assembles and validates it
# without publishing, and by pages.yml, which uploads it.  Assembly is where
# the two used to be able to drift: a file that exists in the deploy but not
# in the check is a file no PR ever tests.
#
# Usage: scripts/wasm-assemble-artifact.sh [dist-dir]
set -euo pipefail

cd "$(dirname "$0")/.."

DIST_DIR="${1:-dist}"
BUILD_BIN="engine/build/bin"

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

cp -r web/. "$DIST_DIR/"

cp "$BUILD_BIN/hexenwail.js" "$DIST_DIR/"
cp "$BUILD_BIN/hexenwail.wasm" "$DIST_DIR/"
# Present only when the build preloads game data (-DGAME_DATA_DIR) or uses
# pthreads; the default browser build ships neither and imports paks at
# runtime instead.
if [ -f "$BUILD_BIN/hexenwail.data" ]; then
	cp "$BUILD_BIN/hexenwail.data" "$DIST_DIR/"
fi
if [ -f "$BUILD_BIN/hexenwail.worker.js" ]; then
	cp "$BUILD_BIN/hexenwail.worker.js" "$DIST_DIR/"
fi
# emcc's own shell template (engine/web/shell.html) is a bare canvas useful
# for bisecting "is it the engine or the PWA?".  The real entry point users
# get is web/index.html, so the generated shell is kept under a name that
# cannot win the index.html slot.
cp "$BUILD_BIN/hexenwail.html" "$DIST_DIR/engine-shell-debug.html"

echo "Assembled PWA artifact in $DIST_DIR:"
ls -la "$DIST_DIR"
