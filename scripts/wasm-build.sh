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
# shell-wasm.nix (`nix develop .#wasm`) prints: that is the third caller, a
# human at a terminal.
#
# `nix build .#wasm` is deliberately NOT what CI runs.  Emscripten fetches its
# SDL3 port over the network at configure time and the nix sandbox has none,
# which is why that package carries its own "use nix develop .#wasm" note.
#
# Requires: emcmake/emmake on PATH (i.e. `source "$EMSDK/emsdk_env.sh"` first,
# or a `nix develop .#wasm` shell), and cargo with the
# wasm32-unknown-emscripten Rust target: the engine's Rust archive is linked
# into the web client as into every other target.  The version is pinned by
# engine/rust/rust-toolchain.toml; the nix shell provides it, and CI installs
# it with rustup.
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

# Before the Rust archive was mandatory this script configured every web build
# with -DUSE_*_RS=OFF, and engine/CMakeLists.txt now refuses that.  A build
# directory from then would stop at configure on its next use, so drop its
# cache once; everything else in it is rebuilt from the fresh configure.
if [ -f CMakeCache.txt ] && grep -qE '^USE_(RUST_HASHINDEX|[A-Z_]+_RS):BOOL=OFF' CMakeCache.txt; then
	echo "wasm-build.sh: $BUILD_DIR was configured with the removed USE_*_RS=OFF switches; clearing its CMake cache" >&2
	rm -f CMakeCache.txt
fi

emcmake cmake \
	-DCMAKE_BUILD_TYPE=Release \
	-DWEB_RENDERER="$RENDERER" \
	-DUSE_CODEC_VORBIS=OFF \
	-DUSE_ALSA=OFF \
	"$SOURCE_DIR"

emmake make -j"$(nproc)"
