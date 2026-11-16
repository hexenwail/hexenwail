#!/usr/bin/env python3
"""The Flatpak's FluidSynth decodes SF3 without libsndfile (#168).

-Denable-libsndfile=off alone turns SF3 soundfonts off: stock FluidSynth
refuses a v3 font with "compiled without support for (v3.x)".  The Flatpak
shipped exactly that until the vorbisfile patch was applied to it too.  If
the module disables libsndfile it must also carry the patch, the define
that activates it, the libraries it calls (for both link drivers), and its
own post-install check.  flake.nix's fluidsynthPortable must agree on the
patch and the define, so the two builds cannot drift apart.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "flatpak/io.github.hexenwail.hexenwail.yml"
PATCH = "patches/fluidsynth-sf3-vorbisfile.patch"

text = MANIFEST.read_text()
m = re.search(r"^  - name: fluidsynth\n(.*?)(?=^  - name: )", text, re.S | re.M)
if not m:
    sys.exit(f"FAIL: no fluidsynth module in {MANIFEST.name}")
mod = m.group(1)

def code(s):
    return "\n".join(ln for ln in s.splitlines() if not ln.lstrip().startswith("#"))

mod_code = code(mod)
fail = []
if "-Denable-libsndfile=off" in mod_code:
    want = {
        "the patch source": rf"type:\s*patch\s*\n\s*path:\s*\.\./{re.escape(PATCH)}",
        "the -DHEXENWAIL_VORBISFILE define": r"cflags:\s*-DHEXENWAIL_VORBISFILE\b",
        "the C link libraries": r"-DCMAKE_C_STANDARD_LIBRARIES=-lvorbisfile -lvorbis -logg",
        "the C++ link libraries": r"-DCMAKE_CXX_STANDARD_LIBRARIES=-lvorbisfile -lvorbis -logg",
        "the libvorbisfile post-install check": r"grep -qa libvorbisfile\.so\.3",
        "the no-libsndfile post-install check": r"grep -qa libsndfile\.so",
    }
    for what, pat in want.items():
        if not re.search(pat, mod_code):
            fail.append(f"fluidsynth module disables libsndfile but lacks {what}")
    if re.search(r"ldflags:.*-lvorbisfile", mod_code):
        fail.append("-lvorbisfile in ldflags is dropped by --as-needed; use *_STANDARD_LIBRARIES")

if not (ROOT / PATCH).exists():
    fail.append(f"{PATCH} is missing")
patch = (ROOT / PATCH).read_text() if (ROOT / PATCH).exists() else ""
if "HEXENWAIL_VORBISFILE" not in patch:
    fail.append(f"{PATCH} no longer keys on HEXENWAIL_VORBISFILE")

flake = code((ROOT / "flake.nix").read_text())
fp = re.search(r"fluidsynthPortable\s*=.*?\}\);", flake, re.S)
if not fp or PATCH.split("/")[-1] not in fp.group(0) or "-DHEXENWAIL_VORBISFILE" not in fp.group(0):
    fail.append("flake.nix fluidsynthPortable no longer applies the patch with -DHEXENWAIL_VORBISFILE")

if fail:
    sys.exit("FAIL:\n  " + "\n  ".join(fail))
print("PASS the Flatpak and the portable Linux build both decode SF3 through the vorbisfile patch (#168)")
