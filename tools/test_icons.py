#!/usr/bin/env python3
"""Every build ships the Hammer of Thyrion "XII" icon, at real resolution.

The source is engine/resource/hexenwail.svg (from the uHexen2 Flathub package);
tools/gen_icons.sh renders everything else from it.  Before this, the builds
used what uHexen2 left behind: a 32 px .ico on Windows, a one-bit 32 px xbm as
the SDL window icon on Linux, the same 32 px PNG installed into Flatpak's
hicolor/128x128 directory and used as the AppImage icon, and a placeholder "H"
on the web PWA.

Held here, standard library only:
  - hexenwail.ico holds 16, 24, 32, 48, 64, 128 (32-bit BMP) and 256 (PNG);
  - each PNG has the size its consumer declares, and is RGBA;
  - hexenwail_icon.h embeds a 128 px PNG and states that size;
  - web/icons/icon.svg is the same file as the source SVG;
  - win32res.rc, gl_vidsdl.c, the Flatpak manifest and the AppImage recipe all
    point at the new files, and nothing still includes the old xbm;
  - Sys_SDLInit sets the app id the .desktop file is named after, before
    SDL_Init, so Wayland compositors can find the icon.
"""
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
RES = ROOT / "engine/resource"
APP_ID = "io.github.hexenwail.hexenwail"
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


def png_info(data, what):
    """(width, height, color_type) of a PNG, or None after recording a failure."""
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        fails.append(f"{what}: not a PNG")
        return None
    w, h, _depth, ctype = struct.unpack(">IIBB", data[16:26])
    return w, h, ctype


def check_png(path, size, what=None):
    what = what or str(path.relative_to(ROOT))
    if not path.exists():
        fails.append(f"{what}: missing")
        return
    info = png_info(path.read_bytes(), what)
    if info:
        check(info[:2] == (size, size), f"{what}: {info[0]}x{info[1]}, want {size}x{size}")
        check(info[2] in (6,) or what.endswith("icon-180.png"),
              f"{what}: colour type {info[2]}, want 6 (RGBA)")


# --- Windows .ico ----------------------------------------------------------
ico = (RES / "hexenwail.ico").read_bytes()
reserved, kind, count = struct.unpack("<HHH", ico[:6])
check((reserved, kind) == (0, 1), "hexenwail.ico: bad ICONDIR header")
sizes = {}
for i in range(count):
    w, h, _c, _r, _planes, bpp, length, off = struct.unpack(
        "<BBBBHHII", ico[6 + 16 * i:22 + 16 * i])
    w, h = w or 256, h or 256
    blob = ico[off:off + length]
    check(len(blob) == length, f"hexenwail.ico: {w}px entry runs past end of file")
    if blob[:4] == b"\x89PNG":
        info = png_info(blob, f"hexenwail.ico {w}px")
        check(info is not None and info[:2] == (w, h), f"hexenwail.ico: {w}px PNG size mismatch")
        sizes[w] = "PNG"
    else:
        hdr, bw, bh, _p, bbpp = struct.unpack("<IiiHH", blob[:16])
        check(hdr == 40 and bw == w and bh == 2 * h and bbpp == 32,
              f"hexenwail.ico: {w}px BMP entry is not a 32-bit DIB of that size")
        sizes[w] = "BMP"
    check(bpp == 32, f"hexenwail.ico: {w}px directory entry says {bpp} bpp")
want = {16: "BMP", 24: "BMP", 32: "BMP", 48: "BMP", 64: "BMP", 128: "BMP", 256: "PNG"}
check(sizes == want, f"hexenwail.ico: entries {sizes}, want {want}")

# --- rasters ---------------------------------------------------------------
check_png(RES / "hexenwail-256.png", 256)
check_png(ROOT / "web/icons/icon-180.png", 180)
check_png(ROOT / "web/icons/icon-192.png", 192)
check_png(ROOT / "web/icons/icon-512.png", 512)

svg = (RES / "hexenwail.svg").read_bytes()
check(b"<svg" in svg and b"<script" not in svg, "hexenwail.svg: not a plain SVG")
check((ROOT / "web/icons/icon.svg").read_bytes() == svg,
      "web/icons/icon.svg differs from engine/resource/hexenwail.svg; rerun tools/gen_icons.sh")

# --- embedded window icon --------------------------------------------------
hdr = (RES / "hexenwail_icon.h").read_text()
m = re.search(r"#define HEXENWAIL_ICON_SIZE (\d+)", hdr)
check(m is not None, "hexenwail_icon.h: no HEXENWAIL_ICON_SIZE")
body = re.search(r"hexenwail_icon_png\[\] = \{(.*?)\};", hdr, re.S)
check(body is not None, "hexenwail_icon.h: no hexenwail_icon_png array")
if m and body:
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body.group(1)))
    info = png_info(data, "hexenwail_icon.h")
    size = int(m.group(1))
    check(info is not None and info == (size, size, 6),
          f"hexenwail_icon.h: embedded PNG is {info}, header says {size}x{size} RGBA")

# --- every build points at the new files ----------------------------------
rc_h = (ROOT / "engine/h2shared/resource.h").read_text()
check('#define RES_ICONFILE\t"../resource/hexenwail.ico"' in rc_h,
      "resource.h: RES_ICONFILE is not ../resource/hexenwail.ico")
check("RES_ICONFILE" in (ROOT / "engine/h2shared/win32res.rc").read_text(),
      "win32res.rc no longer uses RES_ICONFILE")

vid = (ROOT / "engine/h2shared/gl_vidsdl.c").read_text()
check('#include "../resource/hexenwail_icon.h"' in vid, "gl_vidsdl.c: does not include hexenwail_icon.h")
check("SDL_PIXELFORMAT_RGBA32" in vid, "gl_vidsdl.c: icon surface is not RGBA32 (stb_image byte order)")
check("PLATFORM_OSX" not in vid.split("static void VID_SetIcon")[1].split("\n}\n")[0],
      "gl_vidsdl.c: VID_SetIcon skips macOS again; a bare binary has no other Dock icon")
for p in (ROOT / "engine").rglob("*.[ch]"):
    if "xbm_icon.h" in p.read_text(errors="replace"):
        fails.append(f"{p.relative_to(ROOT)}: still includes the retired xbm_icon.h")

sdl = (ROOT / "engine/h2shared/sys_sdl.c").read_text()
init = sdl.split("void Sys_SDLInit (void)")[1].split("\n}\n")[0]
meta = init.find(f'SDL_SetAppMetadata("Hexenwail", HW_BASE_VERSION, "{APP_ID}")')
check(meta != -1 and meta < init.find("SDL_Init("),
      f"sys_sdl.c: Sys_SDLInit must call SDL_SetAppMetadata(..., \"{APP_ID}\") before SDL_Init")

fp = (ROOT / "flatpak/io.github.hexenwail.hexenwail.yml").read_text()
check(f"../engine/resource/hexenwail.svg /app/share/icons/hicolor/scalable/apps/{APP_ID}.svg" in fp,
      "flatpak manifest: scalable SVG icon not installed")
check(f"../engine/resource/hexenwail-256.png /app/share/icons/hicolor/256x256/apps/{APP_ID}.png" in fp,
      "flatpak manifest: 256 px icon not installed")
check("hexen2.png" not in fp, "flatpak manifest: still installs the 32 px hexen2.png")
check((ROOT / f"flatpak/{APP_ID}.desktop").read_text().count(f"Icon={APP_ID}") == 1,
      "flatpak .desktop: Icon= is not the app id")

flake = (ROOT / "flake.nix").read_text()
check(f"install -m644 ${{./engine/resource/hexenwail-256.png}} $app/{APP_ID}.png" in flake,
      "flake.nix: AppImage icon is not hexenwail-256.png")
check("resource/hexen2.png" not in flake, "flake.nix: still references the 32 px hexen2.png")

if fails:
    print("FAIL: application icons")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print(f"OK: application icons ({count} ico entries, 4 rasters, window icon, 4 build paths)")
