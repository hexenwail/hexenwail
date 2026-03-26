#!/usr/bin/env python3
"""
Extract textures, model skins, and GFX from Hexen II PAK files.

Outputs PNG files organized into:
  textures/  - BSP wall/floor textures
  gfx/       - HUD and menu graphics (with subdirs)
  models/    - MDL model skins

Usage:
  pak_extract.py <pak_file> <output_dir> [--palette <palette_pak>]

If --palette is not given, the palette is read from the pak file itself
(gfx/palette.lmp). For pak1/pak3 which may not contain the palette,
pass --palette pointing to pak0.pak.
"""

import struct
import os
import sys
import argparse
from PIL import Image


def read_pak_directory(f):
    """Read PAK file directory, return dict of name -> (offset, size)."""
    f.seek(0)
    magic = f.read(4)
    if magic != b"PACK":
        raise ValueError(f"Not a PAK file (magic: {magic})")
    dir_offset, dir_length = struct.unpack("<ii", f.read(8))
    num_entries = dir_length // 64
    f.seek(dir_offset)
    entries = {}
    for _ in range(num_entries):
        name = f.read(56).split(b"\x00")[0].decode("ascii", errors="replace")
        offset, size = struct.unpack("<ii", f.read(8))
        entries[name] = (offset, size)
    return entries


def read_entry(f, entries, name):
    """Read a named entry from the PAK file."""
    off, sz = entries[name]
    f.seek(off)
    return f.read(sz)


def load_palette(pak_path):
    """Load the 256-color palette from a PAK file."""
    with open(pak_path, "rb") as f:
        entries = read_pak_directory(f)
        if "gfx/palette.lmp" not in entries:
            raise ValueError(f"No gfx/palette.lmp in {pak_path}")
        pal_data = read_entry(f, entries, "gfx/palette.lmp")
    return [(pal_data[i * 3], pal_data[i * 3 + 1], pal_data[i * 3 + 2]) for i in range(256)]


def pixels_to_image(pixels, width, height, palette):
    """Convert paletted pixels to RGBA Image. Index 255 = transparent."""
    img = Image.new("RGBA", (width, height))
    pix = img.load()
    for y in range(height):
        for x in range(width):
            idx = pixels[y * width + x]
            r, g, b = palette[idx]
            pix[x, y] = (r, g, b, 0 if idx == 255 else 255)
    return img


def extract_lmps(f, entries, palette, out_dir):
    """Extract LMP graphics (HUD, menus, etc.)."""
    # These are data tables, not images
    skip = {
        "gfx/palette.lmp", "gfx/colormap.lmp", "gfx/tinttab.lmp",
        "gfx/tinttab2.lmp", "gfx/invpal.lmp",
    }
    count = 0
    for name in sorted(entries.keys()):
        if not name.endswith(".lmp") or name in skip:
            continue
        data = read_entry(f, entries, name)
        if len(data) < 9:
            continue
        w, h = struct.unpack("<ii", data[:8])
        pixels = data[8:]
        if w <= 0 or h <= 0 or w > 1024 or h > 1024 or len(pixels) < w * h:
            continue

        rel = name.replace(".lmp", ".png")
        out_path = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)

        img = pixels_to_image(pixels, w, h, palette)
        img.save(out_path)
        count += 1
    return count


def extract_bsp_textures(f, entries, palette, out_dir):
    """Extract miptex textures from BSP files."""
    tex_dir = os.path.join(out_dir, "textures")
    os.makedirs(tex_dir, exist_ok=True)

    seen = set()
    count = 0
    for name in sorted(entries.keys()):
        if not name.endswith(".bsp"):
            continue
        bsp_data = read_entry(f, entries, name)
        tex_offset, tex_length = struct.unpack("<ii", bsp_data[4 + 2 * 8 : 4 + 2 * 8 + 8])
        num_tex = struct.unpack("<i", bsp_data[tex_offset : tex_offset + 4])[0]
        tex_offsets = struct.unpack(
            f"<{num_tex}i", bsp_data[tex_offset + 4 : tex_offset + 4 + num_tex * 4]
        )

        for toff in tex_offsets:
            if toff == -1:
                continue
            abs_off = tex_offset + toff
            tex_name = (
                bsp_data[abs_off : abs_off + 16].split(b"\x00")[0].decode("ascii", errors="replace")
            )
            w, h = struct.unpack("<ii", bsp_data[abs_off + 16 : abs_off + 24])
            mip0_off = struct.unpack("<i", bsp_data[abs_off + 24 : abs_off + 28])[0]

            if w <= 0 or h <= 0 or w > 2048 or h > 2048:
                continue
            if tex_name in seen:
                continue
            seen.add(tex_name)

            pixel_start = abs_off + mip0_off
            pixels = bsp_data[pixel_start : pixel_start + w * h]
            if len(pixels) < w * h:
                continue

            # Sanitize: '*' -> '#' for filesystem (engine convention)
            safe = tex_name.replace("*", "#")
            img = pixels_to_image(pixels, w, h, palette)
            img.save(os.path.join(tex_dir, f"{safe}.png"))
            count += 1
    return count


def extract_mdl_skins(f, entries, palette, out_dir):
    """Extract skins from MDL model files."""
    mdl_dir = os.path.join(out_dir, "models")
    os.makedirs(mdl_dir, exist_ok=True)

    # mdl_t header layout (both IDPO and RAPO):
    #   ident(4) version(4) scale(12) scale_origin(12) boundingradius(4) eyeposition(12) = 48
    #   numskins(4) skinwidth(4) skinheight(4) numverts(4) numtris(4) numframes(4)
    #   synctype(4) flags(4) size(4) = 36
    # mdl_t total = 84, newmdl_t (RAPO) = 88 (adds num_st_verts)

    count = 0
    for name in sorted(entries.keys()):
        if not name.endswith(".mdl"):
            continue
        off, sz = entries[name]
        f.seek(off)
        data = f.read(sz)
        if len(data) < 88:
            continue

        ident = data[:4]
        if ident == b"RAPO":
            hdr_size = 88
        elif ident == b"IDPO":
            hdr_size = 84
        else:
            continue

        if len(data) < hdr_size:
            continue

        numskins = struct.unpack("<i", data[48:52])[0]
        skinwidth = struct.unpack("<i", data[52:56])[0]
        skinheight = struct.unpack("<i", data[56:60])[0]

        if skinwidth <= 0 or skinheight <= 0 or skinwidth > 2048 or skinheight > 2048:
            continue
        if numskins <= 0 or numskins > 64:
            continue

        model_base = os.path.basename(name).replace(".mdl", "")
        pos = hdr_size
        skin_size = skinwidth * skinheight

        for skin_idx in range(numskins):
            if pos + 4 > len(data):
                break
            skin_type = struct.unpack("<i", data[pos : pos + 4])[0]
            pos += 4

            if skin_type == 0:  # single skin
                if pos + skin_size > len(data):
                    break
                pixels = data[pos : pos + skin_size]
                pos += skin_size
                img = pixels_to_image(pixels, skinwidth, skinheight, palette)
                img.save(os.path.join(mdl_dir, f"{model_base}_{skin_idx}.png"))
                count += 1
            else:  # skin group
                if pos + 4 > len(data):
                    break
                num_group = struct.unpack("<i", data[pos : pos + 4])[0]
                pos += 4
                if num_group <= 0 or num_group > 32:
                    break
                pos += num_group * 4  # skip intervals
                for gi in range(num_group):
                    if pos + skin_size > len(data):
                        break
                    pixels = data[pos : pos + skin_size]
                    pos += skin_size
                    img = pixels_to_image(pixels, skinwidth, skinheight, palette)
                    img.save(os.path.join(mdl_dir, f"{model_base}_{skin_idx}g{gi}.png"))
                    count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description="Extract textures from Hexen II PAK files")
    parser.add_argument("pak_file", help="Path to the PAK file")
    parser.add_argument("output_dir", help="Output directory for extracted PNGs")
    parser.add_argument(
        "--palette", help="PAK file containing gfx/palette.lmp (default: same as pak_file)"
    )
    args = parser.parse_args()

    palette_source = args.palette or args.pak_file
    print(f"Loading palette from {palette_source}...")
    palette = load_palette(palette_source)

    print(f"Extracting from {args.pak_file} -> {args.output_dir}")
    os.makedirs(args.output_dir, exist_ok=True)

    with open(args.pak_file, "rb") as f:
        entries = read_pak_directory(f)

        lmp_count = extract_lmps(f, entries, palette, args.output_dir)
        print(f"  LMP graphics: {lmp_count}")

        bsp_count = extract_bsp_textures(f, entries, palette, args.output_dir)
        print(f"  BSP textures: {bsp_count}")

        mdl_count = extract_mdl_skins(f, entries, palette, args.output_dir)
        print(f"  Model skins:  {mdl_count}")

        total = lmp_count + bsp_count + mdl_count
        print(f"  Total: {total} images")


if __name__ == "__main__":
    main()
