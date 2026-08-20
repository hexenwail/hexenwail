# WASM/Emscripten development shell
# Usage: nix develop . -f shell-wasm.nix
# This shell allows network access for Emscripten port downloads

{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    emscripten
    cmake
    pkg-config
    nodejs
    sdl3
    python3
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
    echo "This shell has network access enabled for Emscripten port downloads."
    echo ""
  '';
}
