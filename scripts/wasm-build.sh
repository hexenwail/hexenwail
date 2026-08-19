#!/usr/bin/env bash
# Configures and builds the Emscripten/WebAssembly client.
#
# Usage: wasm-build.sh [renderer] [build-dir]
#   renderer   webgl2 (default, the shipping configuration) or software
#   build-dir  defaults to engine/build; pass a second directory to keep the
#              two renderer configurations from sharing a CMake cache
#
# One copy of the emcmake configure line, shared by the PR "PWA /
# WebAssembly" check and the Pages deployment, so the check cannot pass on a
# recipe the deploy does not use.  Keep it in sync with the invocation
# shell-wasm.nix prints: that is the third caller, a human at a terminal.
#
# `nix build .#wasm` is deliberately NOT what CI runs.  Emscripten fetches its
# SDL3 port over the network at configure time and the nix sandbox has none,
# which is why that package carries its own "use shell-wasm.nix" note.
#
# Requires: emcmake/emmake on PATH (i.e. `source "$EMSDK/emsdk_env.sh"` first,
# or a shell-wasm.nix shell).
set -euo pipefail

RENDERER="${1:-webgl2}"
BUILD_DIR="${2:-engine/build}"

if [ "$RENDERER" != "webgl2" ] && [ "$RENDERER" != "software" ]; then
	echo "wasm-build.sh: renderer must be 'webgl2' or 'software' (got '$RENDERER')" >&2
	exit 2
fi

cd "$(dirname "$0")/.."
SOURCE_DIR="$PWD/engine"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

emcmake cmake \
	-DCMAKE_BUILD_TYPE=Release \
	-DWEB_RENDERER="$RENDERER" \
	-DUSE_CODEC_VORBIS=OFF \
	-DUSE_ALSA=OFF \
	"$SOURCE_DIR"

emmake make -j"$(nproc)"
