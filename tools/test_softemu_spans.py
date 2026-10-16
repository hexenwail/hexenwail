#!/usr/bin/env python3
"""r_softemu_spans: the 1997 span rasteriser's texture coordinates (#176).

D_DrawSpans8 (engine/h2shared/d_scan.c) divided by z every 8 pixels of a
320-wide frame and stepped linearly between; Turbulent8 every 16.  The world
fragment shaders reproduce that with SpanUV, and the brush polys and liquids
that draw through gl_shader_alias get it around their batches.  Checked at
source level, since the effect itself is a picture (the live check -- an
on-versus-off difference zero at every cell start and peaking mid-cell -- is
in the commit that made the change):

  - SpanUV is highp in and out (the ES tier defaults to mediump) and falls
    back to the perspective coordinate when a cell's depth changes 8x;
  - every world program and the alias program declare u_spans and route their
    texture (and the world's lightmap) coordinate through SpanUV;
  - R_SpanCell keeps 1997's proportion, 8/320 of the render width, doubled for
    liquids, 0 when off;
  - every alias-program brush/liquid batch and the lit-liquid world batch set
    the cell before drawing and put it back after; underwater walls (the
    DrawGLWaterPoly* pair) are D_DrawSpans8 faces and take the 8-pixel cell,
    liquids (EmitWaterPolys) Turbulent8's 16;
  - the cvar defaults off and the menu row is gated out of the software build.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []
read = lambda p: (ROOT / p).read_text()


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


shader = read("engine/h2shared/gl_shader.c")
fn = shader[shader.index("#define GLSL_SPAN_UV_FN"):]
fn = fn[:fn.index('"}\\n"') + 6]
if "highp vec2 SpanUV(highp vec2 uv, highp float span)" not in fn:
    errors.append("SpanUV must take and return highp (ES defaults to mediump)")
if "if (min(w0, w1) < 0.125 * max(w0, w1)) return uv;" not in fn:
    errors.append("SpanUV must fall back when a cell's depth changes 8x")
if "dFdx(a)" not in fn or "dFdx(w)" not in fn or "-d0 / span" not in fn:
    errors.append("SpanUV no longer interpolates the screen-linear terms between cell starts")

for name in ("sworld_frag", "sworld_frag_opaque"):
    start = shader.index(f"static const char {name}[] =")
    body = shader[start:shader.index("\";\n", start)]
    for want in ('"uniform float u_spans;', "GLSL_SPAN_UV_FN", "tc = SpanUV(v_texcoord, u_spans);",
                 "lmc = SpanUV(v_lmcoord, u_spans);", "highp vec2 lmc", "highp vec2 lmuv = lmc",
                 '"in highp vec2 v_lmcoord;', "lmc = v_lmcoord + clamp(lmc - v_lmcoord, -lmlim, lmlim);"):
        if want not in body:
            errors.append(f"{name}: missing {want}")
start = shader.index("static const char salias_frag[] =")
alias = shader[start:shader.index("\";\n", start)]
for want in ('"uniform float u_spans;', "GLSL_SPAN_UV_FN", "if (u_spans > 0.0) uv = SpanUV(v_texcoord, u_spans);",
             "highp vec2 uv = v_texcoord"):
    if want not in alias:
        errors.append(f"salias_frag: missing {want}")

rmain = strip(read("engine/hexen2/gl_rmain.c"))
m = re.search(r"float R_SpanCell \(qboolean liquid\)\s*\{(.*?)\n\}", rmain, re.S)
if not m:
    errors.append("gl_rmain.c: no R_SpanCell")
else:
    cell = m.group(1)
    for want in (r"if\s*\(\s*!r_softemu_spans\.integer\s*\)\s*return\s+0\.0f",
                 r"8\.0f\s*\*\s*\(float\)\s*\(int\)\s*\(\s*glwidth\s*\*\s*scale\s*\)\s*/\s*320\.0f",
                 r"cell\s*=\s*2\.0f\s*\*\s*floorf\s*\(\s*cell\s*\*\s*0\.5f\s*\+\s*0\.5f\s*\)",
                 r"liquid\s*\?\s*2\.0f\s*\*\s*cell\s*:\s*cell"):
        if not re.search(want, cell):
            errors.append(f"R_SpanCell: missing {want}")

rsurf = strip(read("engine/hexen2/gl_rsurf.c"))
warp = strip(read("engine/h2shared/gl_warp.c"))
for label, text, before, after in (
        # Underwater walls, not liquids (EmitWaterPolys draws those): 8, not 16.
        ("DrawGLWaterPoly", rsurf, "R_SetSpanCell (prog, R_SpanCell (false));", "R_SetSpanCell (prog, 0.0f);"),
        ("DrawGLPoly", rsurf, "R_SetSpanCell (prog, R_SpanCell (false));", "R_SetSpanCell (prog, 0.0f);"),
        ("EmitWaterPolys", warp, "R_SetSpanCell (prog, R_SpanCell (true));", "R_SetSpanCell (prog, lit ? R_SpanCell (false) : 0.0f);")):
    i = text.find(f"void {label} (")
    if i < 0:
        errors.append(f"no {label}")
        continue
    j = text.find("\n}\n", i)
    body = text[i:j]
    if before not in body or after not in body or body.index(before) > body.index(after):
        errors.append(f"{label}: must set the span cell before its batch and put it back after")

# The lightmap clamp is LM_GUTTER - 0.5 luxels: the gutter, less bilinear's
# half-luxel reach.  Tied to the define so a gutter change cannot leave the
# shader reading past it.
gutter = re.search(r"#define\s+LM_GUTTER\s+(\d+)", read("engine/hexen2/gl_rsurf.c"))
# Bicubic reaches a whole luxel further than bilinear's half, so 1.0 there.
lim = re.search(r"highp vec2 lmlim = \(\(u_lightmap_bicubic > 0\.5\) \? ([0-9.]+) : ([0-9.]+)\) / vec2\(textureSize\(u_texture1, 0\)\);", shader)
if not gutter or not lim or float(lim.group(2)) != int(gutter.group(1)) - 0.5 \
        or float(lim.group(1)) != int(gutter.group(1)) - 1.0:
    errors.append("the lightmap clamp must be LM_GUTTER - 0.5 luxels (bilinear), LM_GUTTER - 1 (bicubic)")
# Underwater walls take the world program's own cell: no override there.
mt = rsurf[rsurf.index("void DrawGLWaterPolyMTexLM ("):]
mt = mt[:mt.index("\n}\n")]
if "R_SetSpanCell" in mt or "R_SpanCell (true)" in rsurf:
    errors.append("no surface in gl_rsurf.c is a liquid: none gets the liquid cell")

post = read("engine/h2shared/gl_postprocess.c")
if 'cvar_t	r_softemu_spans = {"r_softemu_spans", "0", CVAR_ARCHIVE};' not in post:
    errors.append("r_softemu_spans must default to 0")
menu = read("engine/hexen2/menu.c")
if not re.search(r"#ifndef WEBSOFT[^\n]*\n\s*Cvar_SetValue \(\"r_softemu_spans\"", menu):
    errors.append("menu.c: the software build has no r_softemu_spans; its row must be gated out")


# ---------------------------------------------------------------- numeric
# The real GLSL SpanUV, compiled as C++ with dual numbers standing in for
# GLSL's float/vec2 so that dFdx is the exact screen-x derivative.  Driven
# across a plane with depth varying along x, and compared with a 1997-style
# reference: the true (perspective) coordinate at every cell start, linear
# between them -- what D_DrawSpans8 computed for whole-pixel cells.
glsl = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', fn)).replace("\\n", "\n")
glsl = glsl.replace("highp ", "")
CPP = r"""
#include <cmath>
#include <cstdio>
struct f { double v, d; };          /* value and d/dx */
static f F(double v) { return {v, 0}; }
static f operator+(f a, f b) { return {a.v + b.v, a.d + b.d}; }
static f operator-(f a, f b) { return {a.v - b.v, a.d - b.d}; }
static f operator*(f a, f b) { return {a.v * b.v, a.d * b.v + a.v * b.d}; }
static f operator/(f a, f b) { return {a.v / b.v, (a.d * b.v - a.v * b.d) / (b.v * b.v)}; }
static f operator-(f a) { return {-a.v, -a.d}; }
static f operator*(double k, f a) { return F(k) * a; }
static f operator+(f a, double k) { return a + F(k); }
static bool operator<(f a, f b) { return a.v < b.v; }
static f floor(f a) { return {std::floor(a.v), 0}; }
static f min(f a, f b) { return a.v < b.v ? a : b; }
static f max(f a, f b) { return a.v > b.v ? a : b; }
static f dFdx(f a) { return F(a.d); }
struct vec2 { f x, y; };
static vec2 operator*(vec2 a, f k) { return {a.x * k, a.y * k}; }
static vec2 operator+(vec2 a, vec2 b) { return {a.x + b.x, a.y + b.y}; }
static vec2 operator/(vec2 a, f k) { return {a.x / k, a.y / k}; }
static vec2 dFdx(vec2 a) { return {dFdx(a.x), dFdx(a.y)}; }
static vec2 mix(vec2 a, vec2 b, f t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
static struct { f x, w; } gl_FragCoord;
@SPANUV@
/* A plane: s/z and 1/z linear in screen x, so s = (A0 + Ax x) / (W0 + Wx x). */
static const double A0 = 3.0, Ax = 0.21, W0 = 0.9, Wx = -0.0011;
static double truth(double x) { return (A0 + Ax * x) / (W0 + Wx * x); }
int main() {
	const double cells[] = { 8, 20, 48 };
	int bad = 0;
	for (double cell : cells)
		for (int n = 0; n < 400; n++) {
			double x = n + 0.5;
			gl_FragCoord.x = {x, 1.0};
			gl_FragCoord.w = {W0 + Wx * x, Wx};
			f s = F(truth(x)); s.d = (Ax * (W0 + Wx * x) - (A0 + Ax * x) * Wx) / std::pow(W0 + Wx * x, 2);
			vec2 got = SpanUV(vec2{s, s}, F(cell));
			double x0 = std::floor(x / cell) * cell + 0.5;
			double ref = truth(x0) + (truth(x0 + cell) - truth(x0)) * (x - x0) / cell;
			if (std::fabs(got.x.v - ref) > 1e-9) { if (bad++ < 5) printf("cell %g x %g got %.9f ref %.9f\n", cell, x, got.x.v, ref); }
			if (x == x0 && std::fabs(got.x.v - truth(x)) > 1e-9) bad++;
		}
	/* The guard: a cell spanning an 8x change of 1/z keeps the perspective
	 * coordinate rather than divide by the near-zero end. */
	gl_FragCoord.x = {0.5, 1.0};
	gl_FragCoord.w = {1.0, -0.12};          /* 1/z falls by 8x and more within 8 px */
	f s = {5.0, 0.3};
	vec2 got = SpanUV(vec2{s, s}, F(8));
	if (got.x.v != 5.0) { printf("guard did not fall back\n"); bad++; }
	if (bad) { printf("%d mismatches\n", bad); return 1; }
	printf("ok   SpanUV (compiled from the GLSL) matches the 1997 span reference for cells 8, 20, 48\n");
	return 0;
}
"""
import os, subprocess, tempfile
cpp = CPP.replace("@SPANUV@", glsl.replace("vec2 SpanUV(vec2 uv, float span)", "static vec2 SpanUV(vec2 uv, f span)")
                  .replace("float ", "f ").replace("vec2  ", "vec2 ").replace(" 0.125 *", " F(0.125) *")
                  .replace("+ 0.5 -", "+ F(0.5) -"))
with tempfile.TemporaryDirectory() as tmp:
    src, exe = Path(tmp) / "t.cpp", Path(tmp) / "t"
    src.write_text(cpp)
    r = subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-o", str(exe), str(src)],
                       capture_output=True, text=True)
    if r.returncode:
        errors.append("SpanUV did not compile as C++:\n" + r.stderr[-2000:] + "\n" + cpp[cpp.index("static vec2 SpanUV"):][:900])
    else:
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        if r.returncode:
            errors.append("SpanUV disagrees with the 1997 span reference")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS r_softemu_spans reaches the world, brush polys and liquids, at 1997's proportion (#176)")
