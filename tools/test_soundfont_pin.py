#!/usr/bin/env python3
"""One TimGM6mb, pinned once in four places (#114).

The web build fetches its bundled SoundFont with scripts/fetch-soundfont.sh;
the linux-fhs release (flake.nix timgmTar) and the Flatpak bundle the same
file from the same Debian tarball; web/soundfont-NOTICE.txt states the hash
the site ships.  A pin bumped in one place and not the others would ship a
different SoundFont per platform, or a notice that names the wrong file.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
places = {
    "scripts/fetch-soundfont.sh": r'sha256="([0-9a-f]{64})"',
    "flake.nix": r'timgmTar\s*=\s*pkgs\.fetchurl\s*\{.*?sha256\s*=\s*"([0-9a-f]{64})"',
    "flatpak/io.github.hexenwail.hexenwail.yml": r'timgm6mb-soundfont_1\.3\.orig\.tar\.gz\s*\n\s*sha256:\s*([0-9a-f]{64})',
    "web/soundfont-NOTICE.txt": r'SHA-256\s+([0-9a-f]{64})',
}
found = {}
for path, pattern in places.items():
    m = re.search(pattern, (ROOT / path).read_text(), re.S)
    if not m:
        sys.exit(f"FAIL: no TimGM6mb pin found in {path}")
    found[path] = m.group(1)
if len(set(found.values())) != 1:
    sys.exit("FAIL: the TimGM6mb pins disagree:\n  " + "\n  ".join(f"{h}  {p}" for p, h in found.items()))
print(f"PASS one TimGM6mb pin in {len(found)} places: {next(iter(found.values()))[:16]}... (#114)")
