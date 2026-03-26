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
    echo "  cd engine && mkdir -p build && cd build"
    echo "  emcmake cmake -DCMAKE_BUILD_TYPE=Release -DUSE_CODEC_VORBIS=OFF -DUSE_ALSA=OFF -DUSE_SDL3_STATIC=ON .."
    echo "  emmake make"
    echo ""
    echo "This shell has network access enabled for Emscripten port downloads."
    echo ""
  '';
}
