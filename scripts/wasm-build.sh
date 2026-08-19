#!/usr/bin/env bash
# Configures and builds the Emscripten/WebAssembly client.
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

cd "$(dirname "$0")/.."

mkdir -p engine/build
cd engine/build

emcmake cmake \
	-DCMAKE_BUILD_TYPE=Release \
	-DUSE_CODEC_VORBIS=OFF \
	-DUSE_ALSA=OFF \
	-DUSE_SDL3_STATIC=ON \
	..

emmake make -j"$(nproc)"
