#!/usr/bin/env bash
#
# upscale-pak.sh — Extract and AI-upscale all textures from Hexen II PAK files.
#
# Produces override directories (textures/, gfx/, models/) with TGA files
# ready to drop into a game directory.
#
# Usage:
#   ./upscale-pak.sh <pak_file> <output_dir> [--palette <pak0_path>] [--scale 4] [--upscaler realcugan]
#
# Requires: nix-shell (pulls python3+pillow, realcugan/realesrgan automatically)
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCALE=4
UPSCALER=realcugan  # realcugan or realesrgan
PALETTE=""
DENOISE=0

usage() {
    echo "Usage: $0 <pak_file> <output_dir> [options]"
    echo ""
    echo "Options:"
    echo "  --palette <pak>    PAK file with gfx/palette.lmp (default: same as input)"
    echo "  --scale <n>        Upscale factor: 2, 3, or 4 (default: 4)"
    echo "  --upscaler <name>  realcugan or realesrgan (default: realcugan)"
    echo "  --denoise <n>      Denoise level for realcugan: -1/0/1/2/3 (default: 0)"
    echo "  --help             Show this help"
    exit 1
}

# Parse args
PAK_FILE=""
OUTPUT_DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --palette)   PALETTE="$2"; shift 2 ;;
        --scale)     SCALE="$2"; shift 2 ;;
        --upscaler)  UPSCALER="$2"; shift 2 ;;
        --denoise)   DENOISE="$2"; shift 2 ;;
        --help|-h)   usage ;;
        *)
            if [[ -z "$PAK_FILE" ]]; then
                PAK_FILE="$1"
            elif [[ -z "$OUTPUT_DIR" ]]; then
                OUTPUT_DIR="$1"
            else
                echo "Error: unexpected argument '$1'"
                usage
            fi
            shift
            ;;
    esac
done

if [[ -z "$PAK_FILE" || -z "$OUTPUT_DIR" ]]; then
    usage
fi

PAK_FILE="$(realpath "$PAK_FILE")"
OUTPUT_DIR="$(realpath "$OUTPUT_DIR")"
EXTRACT_DIR="$(mktemp -d)"
UPSCALED_DIR="$(mktemp -d)"

cleanup() {
    rm -rf "$EXTRACT_DIR" "$UPSCALED_DIR"
}
trap cleanup EXIT

echo "=== Hexen II PAK Upscaler ==="
echo "  Input:    $PAK_FILE"
echo "  Output:   $OUTPUT_DIR"
echo "  Scale:    ${SCALE}x"
echo "  Upscaler: $UPSCALER"
echo ""

# Step 1: Extract PNGs
echo "--- Step 1: Extracting textures ---"
PALETTE_ARG=""
if [[ -n "$PALETTE" ]]; then
    PALETTE_ARG="--palette $(realpath "$PALETTE")"
fi

nix-shell -p python3 python3Packages.pillow --run \
    "python3 '$SCRIPT_DIR/pak_extract.py' '$PAK_FILE' '$EXTRACT_DIR' $PALETTE_ARG"

# Step 2: Upscale each directory
echo ""
echo "--- Step 2: Upscaling with $UPSCALER (${SCALE}x) ---"

upscale_dir() {
    local src="$1"
    local dst="$2"
    local count
    count=$(find "$src" -maxdepth 1 -name '*.png' 2>/dev/null | wc -l)
    if [[ $count -eq 0 ]]; then
        return
    fi
    mkdir -p "$dst"
    echo "  Upscaling $(basename "$src")/ ($count images)..."

    if [[ "$UPSCALER" == "realcugan" ]]; then
        nix-shell -p realcugan-ncnn-vulkan --run \
            "realcugan-ncnn-vulkan -i '$src' -o '$dst' -s $SCALE -n $DENOISE -f png 2>/dev/null" || true
    elif [[ "$UPSCALER" == "realesrgan" ]]; then
        nix-shell -p realesrgan-ncnn-vulkan --run \
            "realesrgan-ncnn-vulkan -i '$src' -o '$dst' -n realesrgan-x4plus -s $SCALE -f png 2>/dev/null" || true
    else
        echo "Error: unknown upscaler '$UPSCALER'"
        exit 1
    fi

    # Retry any that failed with realesrgan as fallback
    local failed=0
    for f in "$src"/*.png; do
        local base
        base="$(basename "$f")"
        if [[ ! -f "$dst/$base" ]]; then
            echo "    Retrying $base with realesrgan fallback..."
            nix-shell -p realesrgan-ncnn-vulkan --run \
                "realesrgan-ncnn-vulkan -i '$f' -o '$dst/$base' -n realesrgan-x4plus -s $SCALE -f png 2>/dev/null" || true
            if [[ ! -f "$dst/$base" ]]; then
                echo "    WARNING: Failed to upscale $base, copying original"
                cp "$f" "$dst/$base"
                failed=$((failed + 1))
            fi
        fi
    done
    if [[ $failed -gt 0 ]]; then
        echo "    $failed images could not be upscaled"
    fi
}

# Process each subdirectory
SUBDIRS=$(find "$EXTRACT_DIR" -type d | sort)
for subdir in $SUBDIRS; do
    rel="${subdir#$EXTRACT_DIR}"
    rel="${rel#/}"
    # Only process dirs that directly contain PNGs
    png_count=$(find "$subdir" -maxdepth 1 -name '*.png' 2>/dev/null | wc -l)
    if [[ $png_count -gt 0 ]]; then
        upscale_dir "$subdir" "$UPSCALED_DIR${rel:+/$rel}" || echo "    WARNING: upscale_dir failed for $rel"
    fi
done

# Step 3: Convert to TGA and install
echo ""
echo "--- Step 3: Converting to TGA ---"

nix-shell -p python3 python3Packages.pillow --run "python3 -c \"
import os, sys
from PIL import Image

upscaled = '$UPSCALED_DIR'
output = '$OUTPUT_DIR'

count = 0
for root, dirs, files in os.walk(upscaled):
    for f in sorted(files):
        if not f.endswith('.png'):
            continue
        rel_dir = os.path.relpath(root, upscaled)
        name = f.replace('.png', '')

        # For textures, restore '*' from '#'
        if rel_dir == 'textures' or rel_dir.startswith('textures/'):
            name = name.replace('#', '*')

        dst_dir = os.path.join(output, rel_dir)
        os.makedirs(dst_dir, exist_ok=True)

        img = Image.open(os.path.join(root, f))
        img.save(os.path.join(dst_dir, name + '.tga'))
        count += 1

print(f'  Installed {count} TGA files to {output}')
\""

echo ""
echo "=== Done! ==="
echo "Copy the contents of $OUTPUT_DIR into your Hexen II game directory."
