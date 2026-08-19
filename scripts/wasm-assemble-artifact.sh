#!/usr/bin/env bash
# Assembles the static PWA artifact (the "dist" directory) from the built
# Emscripten output plus the web/ PWA shell.
#
# Shared by the PR "PWA / WebAssembly" check, which assembles and validates it
# without publishing, and by pages.yml, which uploads it.  Assembly is where
# the two used to be able to drift: a file that exists in the deploy but not
# in the check is a file no PR ever tests.
#
# Usage: scripts/wasm-assemble-artifact.sh [dist-dir] [build-version] [build-bin]
set -euo pipefail

cd "$(dirname "$0")/.."

DIST_DIR="${1:-dist}"
BUILD_VERSION="${2:-${GITHUB_SHA:-$(git rev-parse --verify HEAD)}}"
BUILD_BIN="${3:-engine/build/bin}"

# Which renderer the .wasm was built with, stamped by CMake.  The launcher gates
# startup on what its renderer needs, and only the build knows which that is.
BUILD_RENDERER="webgl2"
if [ -f "$BUILD_BIN/hexenwail-renderer.txt" ]; then
	BUILD_RENDERER="$(cat "$BUILD_BIN/hexenwail-renderer.txt")"
fi
if [ "$BUILD_RENDERER" != "webgl2" ] && [ "$BUILD_RENDERER" != "software" ]; then
	echo "Unknown renderer build stamp: $BUILD_RENDERER" >&2
	exit 1
fi

if [[ ! "$BUILD_VERSION" =~ ^[A-Za-z0-9._-]+$ ]]; then
	echo "Invalid PWA build version: $BUILD_VERSION" >&2
	exit 1
fi

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

cp -r web/. "$DIST_DIR/"
sed -i.bak "s/__HEXENWAIL_BUILD_VERSION__/$BUILD_VERSION/g" "$DIST_DIR/sw.js"
rm -f "$DIST_DIR/sw.js.bak"
sed -i.bak "s/__HEXENWAIL_RENDERER__/$BUILD_RENDERER/g" "$DIST_DIR/app.js"
rm -f "$DIST_DIR/app.js.bak"

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

echo "Assembled PWA artifact in $DIST_DIR (renderer: $BUILD_RENDERER):"
ls -la "$DIST_DIR"
