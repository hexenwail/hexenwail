#!/usr/bin/env python3
"""The offscreen video driver never goes fullscreen (#401).

SDL's offscreen driver keeps its EGL surface at the size it was created with,
while a fullscreen switch reports the "desktop" size: the engine took the
1024x768 it was told, drew into a 640x480 corner, and `screenshot` read back
1024x768 of it, garbage included (seen on every headless capture).  Measured:
master writes a 1024x768 shot from a 640x480 mode with vid_config_fscr 1; the
fix writes 640x480, a clean frame.  Checked: VID_SetMode skips both fullscreen
branches when SDL's current video driver is "offscreen".
"""
from pathlib import Path
import re
import sys

src = (Path(__file__).resolve().parents[1] / "engine/h2shared/gl_vidsdl.c").read_text()
m = re.search(r'if \(vid_config_fscr\.integer != 0 && SDL_GetCurrentVideoDriver \(\) &&\s*'
              r'!strcmp \(SDL_GetCurrentVideoDriver \(\), "offscreen"\)\)\s*\{[^}]*\}\s*'
              r'// Handle fullscreen modes after window creation\s*else if \(vid_config_fscr\.integer == 2\)', src)
if not m:
    sys.exit("FAIL: VID_SetMode must keep the offscreen driver windowed before its fullscreen branches (#401)")
print("PASS the offscreen video driver stays windowed (#401)")
