#!/usr/bin/env python3
"""r_speeds 2's GPU profiler and the backend it runs on (GitHub #540).

Two ways the profiler's timer queries outlived the thing they were for:

  - -gpu: GL_Init_NullGL says "SDL_GPU has no timer queries" through
    R_ProfileSetGPUUnavailable, but the null GL table it installs has query
    stubs, so R_ProfileInit saw a non-NULL glGenQueries_fp, allocated, and the
    report printed "GPU 0.0 ... world 0.0" instead of naming the reason (#451).
  - vid_restart: the context dies with the query names in it, and the profiler
    kept using them.  NVIDIA and llvmpipe log one GL_INVALID_OPERATION ("Query
    object not found") and recover; by the spec every call is an error.

Held here:
  - R_ProfileInit bails on RPROF_GPU_UNAVAILABLE before it looks at
    glGenQueries_fp; R_ProfileContextLost forgets the queries;
  - VID_ChangeVideoMode (what vid_restart runs) calls R_ProfileContextLost
    with the other context-lost hooks, before the context is destroyed, in
    live code (comments and #if 0 stripped); glquake.h declares it;
  - R_ProfileContextLost also clears the #503 ring: no set left pending (its
    names are dead), none claimed, the cursor back at 0;
  - compiled against a counting glGenQueries_fp: a backend's unavailability
    -> never allocates, not even after a context loss; available -> allocates
    once, again after a context loss, and not again without one; entry points
    missing -> unavailable, and asked again after a context loss.

The live counterpart, which needs a GPU (and Xvfb for -gpu), is
tools/rprof-context-check.sh.
"""
from pathlib import Path
import os
import shlex
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
rm = (ROOT / "engine/hexen2/gl_rmain.c").read_text()
vid = (ROOT / "engine/h2shared/gl_vidsdl.c").read_text()
glq = (ROOT / "engine/h2shared/glquake.h").read_text()
errors = []


def body_of(src, head):
    m = re.search(head, src)
    if not m:
        return None
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[m.start():j + 1]
    return None


fns = {}
for key, head in (("unavail", r"void R_ProfileSetGPUUnavailable \(const char \*reason\)\s*\{"),
                  ("init", r"static void R_ProfileInit \(void\)\s*\{"),
                  ("lost", r"void R_ProfileContextLost \(void\)\s*\{")):
    fns[key] = body_of(rm, head)
    if fns[key] is None:
        errors.append(f"gl_rmain.c is missing {head.split(chr(92))[0].strip()}")

if fns.get("init"):
    init = fns["init"]
    i_un = init.find("if (rprof_gpu_source == RPROF_GPU_UNAVAILABLE)")
    i_gen = init.find("if (!glGenQueries_fp)")
    if i_un < 0 or i_gen < 0 or i_un > i_gen:
        errors.append("R_ProfileInit must bail on RPROF_GPU_UNAVAILABLE before it tests glGenQueries_fp: "
                      "-gpu's null table has query stubs (#540)")

if "void\tR_ProfileContextLost (void);" not in glq:
    errors.append("glquake.h must declare R_ProfileContextLost")


def live(src):
    """Comments and #if 0 blocks out, so a disabled call does not count."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return re.sub(r"#if 0\b.*?#endif", "", src, flags=re.S)


change = live(body_of(vid, r"static void VID_ChangeVideoMode \(int newmode\)\s*\{") or "")
i_hook = change.find("GL_AliasGPUMeshes_ContextLost();")
i_lost = change.find("R_ProfileContextLost();")
i_kill = change.find("SDL_GL_DestroyContext(glcontext);")
if not change or i_hook < 0 or i_lost < i_hook or i_kill < 0 or i_lost > i_kill:
    errors.append("VID_ChangeVideoMode must call R_ProfileContextLost after GL_AliasGPUMeshes_ContextLost "
                  "and before SDL_GL_DestroyContext")
if len(re.findall(r"\bR_ProfileContextLost\s*\(\);", live(vid))) != 1:
    errors.append("gl_vidsdl.c must call R_ProfileContextLost exactly once (VID_ChangeVideoMode)")

HARNESS = r"""
#include <stdio.h>
typedef int qboolean; enum { false, true };
typedef unsigned int GLuint;
#define RPROF_QUERIES 9
#define RPROF_RING %(ring)s
static int gens;
static void stub_gen (int n, GLuint *ids) { int i; for (i = 0; i < n; i++) ids[i] = 1 + i; gens++; }
static void (*glGenQueries_fp) (int, GLuint *) = stub_gen;
static GLuint	rprof_queries[RPROF_RING][RPROF_QUERIES];
static qboolean	rprof_set_pending[RPROF_RING];
static int	rprof_set = -1;
static int	rprof_next_set;
static qboolean	rprof_pending;
static qboolean	rprof_available;
typedef enum { RPROF_GPU_TIMER_QUERIES, RPROF_GPU_UNAVAILABLE } rprof_gpu_source_t;
static rprof_gpu_source_t rprof_gpu_source = RPROF_GPU_TIMER_QUERIES;
static const char *rprof_gpu_unavailable_reason = "timer queries not available";
%(missing)s
%(unavail)s
%(init)s
%(lost)s
int main (int argc, char **argv)
{
	if (argc > 1 && argv[1][0] == 'n')	/* entry points missing */
	{
		void (*have) (int, GLuint *) = glGenQueries_fp;
		glGenQueries_fp = 0;
		R_ProfileInit ();
		printf ("null %%d %%d %%d", gens, rprof_available, rprof_gpu_source == RPROF_GPU_UNAVAILABLE);
		glGenQueries_fp = have;
		R_ProfileInit ();
		printf (" %%d", gens);	/* still latched until the context goes */
		R_ProfileContextLost ();
		R_ProfileInit ();
		printf (" %%d %%d %%d\n", gens, rprof_available, rprof_gpu_source == RPROF_GPU_UNAVAILABLE);
		return 0;
	}
	if (argc > 1)	/* -gpu */
	{
		R_ProfileSetGPUUnavailable ("SDL_GPU has no timer queries (#451)");
		R_ProfileInit (); R_ProfileInit ();
		R_ProfileContextLost ();
		R_ProfileInit ();
		printf ("gpu %%d %%d %%d\n", gens, rprof_available, rprof_gpu_source == RPROF_GPU_UNAVAILABLE);
		return 0;
	}
	R_ProfileInit (); R_ProfileInit ();
	printf ("gl %%d %%d", gens, rprof_available);
	rprof_pending = true;
	{	/* a ring mid-flight: sets pending, one claimed, the cursor moved on */
		int i, ring = 0;
		for (i = 0; i < RPROF_RING; i++)
			rprof_set_pending[i] = true;
		rprof_set = 1;
		rprof_next_set = 2;
		R_ProfileContextLost ();
		for (i = 0; i < RPROF_RING; i++)
			ring |= rprof_set_pending[i];
		/* a cleared ring reads 0 -1 0 */
		printf (" %%d %%d ring %%d %%d %%d", rprof_available, rprof_pending, ring, rprof_set, rprof_next_set);
	}
	R_ProfileInit (); R_ProfileInit ();
	printf (" %%d %%d\n", gens, rprof_available);
	return 0;
}
"""

ring = re.search(r"#define RPROF_RING\s+(\d+)", rm)
if not ring:
    errors.append("gl_rmain.c must define RPROF_RING (#503)")
else:
    fns["ring"] = ring.group(1)
missing = re.search(r"static const char rprof_missing_entry_points\[\] = \"[^\"]*\";", rm)
if not missing:
    errors.append("gl_rmain.c must name its own unavailability in rprof_missing_entry_points")
elif "rprof_gpu_unavailable_reason == rprof_missing_entry_points" not in (fns.get("lost") or ""):
    errors.append("R_ProfileContextLost must lift the missing-entry-points latch, and only that one")
else:
    fns["missing"] = missing.group(0)

if not errors:
    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / "ctx.c"
        exe = Path(d) / "ctx"
        c.write_text(HARNESS % fns)
        r = subprocess.run(shlex.split(os.environ.get("CC", "cc")) + ["-std=gnu99", "-w", "-o", str(exe), str(c)],
                           capture_output=True, text=True)
        if r.returncode:
            errors.append("harness failed to compile: " + r.stderr.strip()[:400])
        else:
            gl = subprocess.run([str(exe)], capture_output=True, text=True).stdout.split()
            gpu = subprocess.run([str(exe), "gpu"], capture_output=True, text=True).stdout.split()
            null = subprocess.run([str(exe), "null"], capture_output=True, text=True).stdout.split()
            if null != ["null", "0", "0", "1", "0", "1", "1", "0"]:
                errors.append(f"entry points missing: {null[1:]}, want unavailable without allocating, "
                              "still latched, then after a context loss allocated, available")
            if gl != ["gl", "1", "1", "0", "0", "ring", "0", "-1", "0", "2", "1"]:
                errors.append(f"GL: {gl[1:]}, want allocations 1, available, then lost (0 0) with the "
                              "#503 ring cleared (no set pending, none claimed, cursor 0), "
                              "then allocations 2, available")
            if gpu != ["gpu", "0", "0", "1"]:
                errors.append(f"-gpu: {gpu[1:]}, want no allocation, not available, still unavailable")

if errors:
    print("FAIL test_rprof_context:")
    for e in errors:
        print("  " + e)
    sys.exit(1)
print("ok: -gpu keeps r_speeds 2's GPU line named unavailable, and vid_restart re-allocates its queries")
