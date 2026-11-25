#!/usr/bin/env python3
"""r_pp_time, the post-process warp's clock, is a test hook that changes
nothing by default (#340 P4c).

The screen warp's phase (spp_frag.glsl's `time`, PpParams p1.z) was
realtime, which a pause does not stop, so no two runs caught it at one phase
and tools/sweep-warp-gpu.steps could not compare GL with -gpu.  r_pp_time >= 0
holds it; the default -1 must be exactly the old realtime, or every GL
frame with the warp on moves.  This checks:

  * r_pp_time is defined once, in gl_postprocess.c, default "-1", not
    archived (a config left frozen would stop the warp for good), and
    registered in GL_PostProcess_Init;
  * PP_Time is `r_pp_time.value >= 0 ? r_pp_time.value : (float)realtime`:
    the default path is the cast the old sites made;
  * every PpParams time write (`pp_params.p1[2] = ...`) goes through
    PP_Time, in PP_BlitWith3DEffects (End3D's 3D-effects pass) and in
    GL_PostProcess_EndFrame (the copyback path's warp), and no other
    pp_params member reads realtime;
  * the shader still reads its warp phase from p1.z alone;
  * tools/sweep-warp-gpu.steps sets r_pp_time >= 0 before its first shot
    and never back to -1 before a later one;
  * tools/warp-model.py --selftest passes (the sweep's model of the warp
    tells the honest warp from each broken one);
  * on a stub harness: PP_Time, compiled from the source, returns realtime
    at -1, at -0.5 and at -1000, and the cvar's value at 0 and 3.7.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PP = ROOT / "engine/h2shared/gl_postprocess.c"
FRAG = ROOT / "engine/shaders/spp_frag.glsl"
STEPS = ROOT / "tools/sweep-warp-gpu.steps"
MODEL = ROOT / "tools/warp-model.py"
errors = []


def strip_comments(t):
    return re.sub(r"//[^\n]*", " ", re.sub(r"/\*.*?\*/", " ", t, flags=re.S))


def body(code, name):
    m = re.search(r"^[\w \t\*]*\b%s\s*\([^;{]*\)\s*\{" % re.escape(name), code, re.M)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth and i < len(code):
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.start():i]


src = PP.read_text()
code = strip_comments(src)

defs = re.findall(r'cvar_t\s+r_pp_time\s*=\s*\{\s*"r_pp_time"\s*,\s*"([^"]*)"\s*,\s*(\w+)\s*\}', code)
if defs != [("-1", "CVAR_NONE")]:
    errors.append('gl_postprocess.c: r_pp_time must be defined once as {"r_pp_time", "-1", CVAR_NONE}, '
                  "found %r" % defs)
for p in sorted((ROOT / "engine").rglob("*.c")):
    if p != PP and re.search(r'"r_pp_time"', strip_comments(p.read_text())):
        errors.append("%s: defines or names r_pp_time; it is gl_postprocess.c's alone" % p.relative_to(ROOT))
init = body(code, "GL_PostProcess_Init") or ""
if not re.search(r"Cvar_RegisterVariable\s*\(\s*&r_pp_time\s*\)", init):
    errors.append("GL_PostProcess_Init does not register r_pp_time")

ppt = body(code, "PP_Time")
if ppt is None:
    errors.append("gl_postprocess.c: no PP_Time")
elif not re.search(r"\{\s*return\s*\(\s*r_pp_time\.value\s*>=\s*0\.0f\s*\)\s*\?\s*r_pp_time\.value\s*:"
                   r"\s*\(\s*float\s*\)\s*realtime\s*;\s*\}$", ppt):
    errors.append("PP_Time must be just `return (r_pp_time.value >= 0.0f) ? r_pp_time.value : (float)realtime;` "
                  "-- the default path is the old sites' cast, so GL stays pixel-identical")

writes = re.findall(r"pp_params\.p1\s*\[\s*2\s*\]\s*=\s*([^;]*);", code)
if not writes:
    errors.append("no pp_params.p1[2] write found")
for w in writes:
    if w.strip() != "PP_Time ()":
        errors.append("pp_params.p1[2] = %s; -- every PpParams time write goes through PP_Time ()" % w.strip())
for fn, n in (("PP_BlitWith3DEffects", 1), ("GL_PostProcess_EndFrame", 1)):
    b = body(code, fn) or ""
    got = len(re.findall(r"pp_params\.p1\s*\[\s*2\s*\]\s*=\s*PP_Time\s*\(\s*\)", b))
    if got != n:
        errors.append("%s writes the warp time through PP_Time %d times, expected %d" % (fn, got, n))
for m in re.finditer(r"pp_params\.\w+\s*\[\s*\d\s*\]\s*=\s*[^;]*\brealtime\b", code):
    errors.append("a PpParams member reads realtime directly: %s" % m.group(0))

frag = strip_comments(FRAG.read_text()) if FRAG.exists() else ""
if not re.search(r"#define\s+time\s+pp_p1\.z\b", frag):
    errors.append("spp_frag.glsl: `time` is no longer pp_p1.z")
if len(re.findall(r"sin\s*\(\s*uv\.[xy]\s*\*\s*10\.0\s*\+\s*time\s*\*", frag)) != 2:
    errors.append("spp_frag.glsl: the warp's two sin() terms no longer read `time` -- r_pp_time would "
                  "hold nothing")

if not STEPS.exists():
    errors.append("tools/sweep-warp-gpu.steps is missing")
else:
    frozen = None
    for n, line in enumerate(STEPS.read_text().splitlines(), 1):
        m = re.match(r"\s*cmd\s+r_pp_time\s+(\S+)", line)
        if m:
            frozen = float(m.group(1)) >= 0
        elif re.match(r"\s*shot\s", line) and not frozen:
            errors.append("tools/sweep-warp-gpu.steps:%d: a shot with r_pp_time not held >= 0" % n)

r = subprocess.run([sys.executable, str(MODEL), "--selftest"], stdout=subprocess.PIPE,
                   stderr=subprocess.STDOUT, text=True)
if r.returncode:
    errors.append("tools/warp-model.py --selftest failed:\n" + r.stdout)


def harness():
    """PP_Time compiled out of the source, against a stub cvar and realtime."""
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc or ppt is None:
        return "no C compiler" if not cc else None
    c = """
#include <stdio.h>
typedef struct { const char *name, *string; int flags; float value; } cvar_t;
#define CVAR_NONE 0
double realtime = 12345.678;
cvar_t r_pp_time = {"r_pp_time", "-1", CVAR_NONE, -1.0f};
%s
int main (void)
{
	static const float probe[] = { -1.0f, -0.5f, -1000.0f, 0.0f, 3.7f };
	int i, bad = 0;
	for (i = 0; i < 5; i++)
	{
		float want = probe[i] >= 0.0f ? probe[i] : (float)realtime;
		r_pp_time.value = probe[i];
		if (PP_Time () != want)
		{
			printf ("PP_Time at %%g: %%.9g, want %%.9g\\n", probe[i], PP_Time (), want);
			bad = 1;
		}
	}
	return bad;
}
""" % ppt
    with tempfile.TemporaryDirectory() as d:
        open(d + "/t.c", "w").write(c)
        b = subprocess.run([cc, "-std=c99", "-Wall", "-Werror", "-o", d + "/t", d + "/t.c"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if b.returncode:
            return "harness did not compile:\n" + b.stdout
        r = subprocess.run([d + "/t"], stdout=subprocess.PIPE, text=True)
        if r.returncode:
            errors.append("PP_Time harness:\n" + r.stdout)
    return None


note = harness()
if note and "compile" in note:
    errors.append(note)

for e in errors:
    print("FAIL: " + e)
if errors:
    sys.exit(1)
print("PASS r_pp_time: default -1 is realtime, %d warp time writes through PP_Time, the sweep holds it%s"
      % (len(writes), "" if note is None else " (" + note + ", harness skipped)"))
