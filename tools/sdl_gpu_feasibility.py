#!/usr/bin/env python3
"""Check an SDL_gpu.h against the SDL_GPU backend's feasibility list (#77).

#77 chose SDL_GPU for the second render backend on the strength of a list
of capabilities read out of the vendored header: per-attachment blend for
the OIT MRT, compute and indirect dispatch for gl_worldcull.c, indexed
indirect draws, storage buffers in both graphics stages, the blend factors
the renderer uses, and 8x MSAA.  That list was checked once by hand against
3.2.12 and again against 3.4.12 for #452.  This makes it a script, so the
next SDL bump re-checks it rather than trusting the last reading.

Usage:
  sdl_gpu_feasibility.py [HEADER]          check one header (default: the
                                           vendored Windows SDL3)
  sdl_gpu_feasibility.py OLD.h NEW.h       also diff the declared functions

Exits 1 if any REQUIRED item is missing from the (newest) header.  The
CEILING items are reported, never fatal: they are what #77 recorded SDL_GPU
as lacking, and one turning up is news for #77 rather than a failure.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
VENDORED = ROOT / "oslibs/windows/SDL3/include/SDL3/SDL_gpu.h"

# (what #77 relies on it for, the identifiers that must all be declared)
REQUIRED = [
    ("per-target blend state (OIT accum/revealage MRT)",
     ["SDL_GPUColorTargetBlendState", "SDL_GPUColorTargetDescription"]),
    ("compute pipelines (gl_worldcull.c, SSAO, Hi-Z)",
     ["SDL_CreateGPUComputePipeline", "SDL_BeginGPUComputePass",
      "SDL_DispatchGPUCompute"]),
    ("indirect compute dispatch",
     ["SDL_DispatchGPUComputeIndirect"]),
    ("indexed indirect draws (MultiDrawElementsIndirect path)",
     ["SDL_DrawGPUIndexedPrimitivesIndirect"]),
    ("storage buffers in vertex and fragment stages (SSBO particles, instanced alias)",
     ["SDL_BindGPUVertexStorageBuffers", "SDL_BindGPUFragmentStorageBuffers"]),
    ("blend factors the renderer uses",
     ["SDL_GPU_BLENDFACTOR_ZERO", "SDL_GPU_BLENDFACTOR_ONE",
      "SDL_GPU_BLENDFACTOR_SRC_ALPHA", "SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA",
      "SDL_GPU_BLENDFACTOR_DST_COLOR"]),
    ("8x MSAA",
     ["SDL_GPU_SAMPLECOUNT_8"]),
    ("shader formats for all three drivers (#448)",
     ["SDL_GPU_SHADERFORMAT_SPIRV", "SDL_GPU_SHADERFORMAT_DXIL",
      "SDL_GPU_SHADERFORMAT_MSL"]),
]

# (what #77 recorded as missing, a pattern that would mean it now exists)
CEILING = [
    ("GPU timer / timestamp queries (#451)", r"SDL_\w*(Timestamp|TimerQuery|QueryPool)\w*"),
    ("bindless / descriptor indexing", r"SDL_\w*(Bindless|DescriptorIndexing)\w*"),
    ("mesh shaders", r"SDL_\w*MeshShader\w*|SDL_GPU_SHADERSTAGE_MESH"),
    ("ray tracing", r"SDL_\w*(RayTracing|AccelerationStructure)\w*"),
    ("WebGPU driver (web build)", r'"webgpu"|SDL_\w*WEBGPU\w*'),
]

FUNC_RE = re.compile(r"extern\s+SDL_DECLSPEC\b[^;(]*?\b(SDL_\w+)\s*\(", re.S)
COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def code_of(text):
    """The header with comments removed, so prose never counts as API."""
    return COMMENT_RE.sub(" ", text)


def version_of(header):
    ver = header.parent / "SDL_version.h"
    if not ver.exists():
        return "?"
    t = ver.read_text()
    parts = [re.search(rf"#define\s+SDL_{k}_VERSION\s+(\d+)", t) for k in ("MAJOR", "MINOR", "MICRO")]
    return ".".join(m.group(1) if m else "?" for m in parts)


def check(code):
    """[(label, missing identifiers)] and [(label, ceiling hits)]."""
    req = []
    for label, names in REQUIRED:
        missing = [n for n in names if not re.search(rf"\b{re.escape(n)}\b", code)]
        req.append((label, missing))
    ceil = [(label, sorted(set(m.group(0) for m in re.finditer(pat, code))))
            for label, pat in CEILING]
    return req, ceil


def functions(code):
    return set(FUNC_RE.findall(code))


def main(argv):
    paths = [Path(a) for a in argv] or [VENDORED]
    if len(paths) > 2:
        sys.exit(__doc__)
    for p in paths:
        if not p.exists():
            sys.exit(f"FAIL: no such header: {p}")
    new = paths[-1]
    code = code_of(new.read_text())
    req, ceil = check(code)

    print(f"SDL_gpu.h {version_of(new)}: {new}")
    bad = 0
    for label, missing in req:
        print(f"  {'ok  ' if not missing else 'MISS'} {label}"
              + (f": {', '.join(missing)}" if missing else ""))
        bad += bool(missing)
    for label, hits in ceil:
        print(f"  {'NEW ' if hits else 'none'} ceiling: {label}"
              + (f": {', '.join(hits)} -- tell #77" if hits else ""))

    if len(paths) == 2:
        old = paths[0]
        f_old, f_new = functions(code_of(old.read_text())), functions(code)
        gone, added = sorted(f_old - f_new), sorted(f_new - f_old)
        print(f"functions {version_of(old)} -> {version_of(new)}: "
              f"{len(f_old)} -> {len(f_new)}, {len(gone)} removed, {len(added)} added")
        for f in gone:
            print(f"  - {f}")
        for f in added:
            print(f"  + {f}")

    if bad:
        print(f"FAIL: {bad} of {len(REQUIRED)} required capabilities missing (#77)")
        return 1
    print(f"PASS all {len(REQUIRED)} required capabilities present (#77)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
