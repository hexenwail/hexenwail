# WASM/Emscripten development shell
# Usage: nix develop .#wasm        (or, equivalently: nix-shell shell-wasm.nix)
# This shell allows network access for Emscripten port downloads
#
# The flake is the source of truth: devShells.wasm imports this file with the
# flake's pinned nixpkgs and the Rust toolchain named by
# engine/rust/rust-toolchain.toml, which carries the wasm32-unknown-emscripten
# standard library the engine's Rust archive is built against.  Called on its
# own (nix-shell) it defers to that same shell, so both routes get one Rust.

{ pkgs ? null, rustToolchain ? null }:

if pkgs == null then
  (builtins.getFlake (toString ./.)).devShells.${builtins.currentSystem}.wasm
else
pkgs.mkShell {
  buildInputs = with pkgs; [
    emscripten
    cmake
    pkg-config
    nodejs
    sdl3
    python3
    rustToolchain
  ];

  shellHook = ''
    echo "Emscripten development environment for Hexenwail WASM builds"
    echo "=========================================="
    echo ""
    echo "Quick commands:"
    echo "  ./scripts/wasm-build.sh                       # webgl2 (shipping)"
    echo "  ./scripts/wasm-build.sh software engine/build-soft"
    echo ""
    echo "Or by hand:"
    echo "  cd engine && mkdir -p build && cd build"
    echo "  emcmake cmake -DCMAKE_BUILD_TYPE=Release -DWEB_RENDERER=webgl2 -DUSE_CODEC_VORBIS=OFF -DUSE_ALSA=OFF .."
    echo "  emmake make"
    echo ""
    echo "Rust: $(rustc --version) (engine archive target wasm32-unknown-emscripten)"
    echo "This shell has network access enabled for Emscripten port downloads."
    echo ""
  '';
}
