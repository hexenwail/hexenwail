#!/usr/bin/env python3
"""The vendored SDL3 still carries every capability #77's plan needs (#452).

Runs tools/sdl_gpu_feasibility.py against oslibs/windows/SDL3, then checks
the checker itself: a header with one required entry point removed must
fail, and a ceiling feature named only in a comment must not count.
"""
from pathlib import Path
import importlib.util
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/sdl_gpu_feasibility.py"

spec = importlib.util.spec_from_file_location("feas", TOOL)
feas = importlib.util.module_from_spec(spec)
spec.loader.exec_module(feas)

failures = []

# 1. the vendored header passes, through the real CLI
r = subprocess.run([sys.executable, str(TOOL)], capture_output=True, text=True)
if r.returncode != 0:
    failures.append("vendored SDL_gpu.h fails the feasibility list:\n" + r.stdout)

text = feas.VENDORED.read_text()
code = feas.code_of(text)

# 2. drop one required declaration: the check must see it go
victim = "SDL_DispatchGPUComputeIndirect"
mutant = feas.code_of(text.replace(victim, "SDL_Removed_ForTest"))
req, _ = feas.check(mutant)
if not any(victim in missing for _, missing in req):
    failures.append(f"removing {victim} was not detected")

# 3. prose is not API: the header's FAQ mentions ray tracing and mesh
#    shaders, and a comment-only hit must not read as a new capability
probe = code + "\n/* SDL_GPUMeshShaderPipeline SDL_RayTracingThing */\n"
_, ceil = feas.check(feas.code_of(probe))
if any(hits for _, hits in ceil):
    failures.append(f"comment text counted as a ceiling capability: {ceil}")
_, ceil = feas.check(code + "\nextern SDL_DECLSPEC void SDLCALL SDL_CreateGPUMeshShader(void);\n")
if not any(hits for label, hits in ceil if label == "mesh shaders"):
    failures.append("a real mesh-shader declaration was not reported")

# 4. the function diff sees an added declaration
added = feas.functions(code + "\nextern SDL_DECLSPEC int SDLCALL SDL_NewForTest(int x);\n")
if added - feas.functions(code) != {"SDL_NewForTest"}:
    failures.append("function diff missed an added declaration")
# every function declaration starts a line with `extern SDL_DECLSPEC'; an
# independent count catches a regex that silently drops multi-line ones
lines = sum(1 for ln in text.splitlines() if ln.startswith("extern SDL_DECLSPEC"))
if len(feas.functions(code)) != lines:
    failures.append(f"parsed {len(feas.functions(code))} functions, header declares {lines}")

if failures:
    sys.exit("FAIL:\n  " + "\n  ".join(failures))
print(f"PASS vendored SDL {feas.version_of(feas.VENDORED)} meets #77's list; "
      f"checker catches a removal and ignores prose ({len(feas.functions(code))} functions)")
