#!/usr/bin/env python3
"""Particles stand still while the game is paused (found signing off #84).

R_UpdateParticles steps particles by the render frame time for smooth motion;
upstream stepped them by cl.time - cl.oldtime, which a pause holds at 0.  With
host_frametime alone, SoT winter's snow kept falling under the PAUSED plaque:
two paused frames differed in 18,846 px, 5,015 of them the real-time sky.  Now
13,831 of those are gone and only the sky band differs.
"""
from pathlib import Path
import re
import sys

src = (Path(__file__).resolve().parents[1] / "engine/hexen2/r_part.c").read_text()
fn = src[src.index("void R_UpdateParticles (void)\n{"):]
fn = fn[:fn.index("\n}\n")]
if not re.search(r"frametime = cl\.paused \? 0\.0f : host_frametime;", fn):
    sys.exit("FAIL: R_UpdateParticles must not advance particles while cl.paused")
print("PASS particles stand still while paused")
