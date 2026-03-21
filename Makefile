# Hexenwail Build System - Convenience Makefile
# This provides convenient targets for common build tasks

.PHONY: help build release clean nix-build nix-release nix-linux nix-win64

help:
	@echo "Hexenwail Build Targets"
	@echo "======================"
	@echo ""
	@echo "Nix builds (recommended):"
	@echo "  make nix-build      - Build Linux version with Nix"
	@echo "  make nix-release    - Build all platforms (Linux, Win64)"
	@echo "  make nix-linux      - Build Linux version only"
	@echo "  make nix-win64      - Build Windows 64-bit only"
	@echo ""
	@echo "CMake builds:"
	@echo "  make build          - Build Linux version with CMake"
	@echo "  make release        - Build all platforms with CMake (requires mingw-w64)"
	@echo ""
	@echo "Utility:"
	@echo "  make clean          - Clean all build artifacts"
	@echo ""

# Default target
all: help

# Nix builds (recommended)
nix-build:
	nix build .#default --print-build-logs

nix-release:
	./build-release.sh nix

nix-linux:
	nix build .#default --print-build-logs

nix-win64:
	nix build .#win64 --print-build-logs

# CMake build (Linux)
build:
	mkdir -p engine/build
	cd engine/build && cmake .. \
		-DCMAKE_BUILD_TYPE=Release \
		-DUSE_CODEC_MP3=ON \
		-DUSE_CODEC_VORBIS=ON \
		-DUSE_CODEC_FLAC=ON \
		-DUSE_ALSA=ON
	cd engine/build && make -j$$(nproc)
	@echo ""
	@echo "Build complete: engine/build/bin/glhexen2"

# Multi-platform release
release:
	./build-release.sh cmake

# Clean
clean:
	rm -rf engine/build engine/build-* release result
	@echo "Clean complete"
