#!/usr/bin/env python3
"""r_speeds 2 keeps its GPU timestamps in a ring of query sets (GitHub #503).

The GPU profiler used to own one set of timestamp queries, re-issue it every
frame and read it at the start of the next.  That only reports when the GPU
has drained the whole queue by then.  A GPU-bound frame leaves the driver two
or three frames behind, so the set was overwritten before it ever became
available: with r_ssao 1 at 1080p on a Tesla P4, r_speeds 2 printed 33 GPU
lines in 20 s where the game drew ~6600 frames.

Held here:
  - RPROF_RING sets of RPROF_QUERIES, allocated in one glGenQueries call;
  - the report reads GL_QUERY_RESULT only from the set R_ProfileNewestLanded
    returned, which asks GL_QUERY_RESULT_AVAILABLE first;
  - R_RenderView reports, then claims a set, and closes it after the last
    stamp; and
  - the ring logic, compiled against a model GPU that retires frames LAG
    behind the CPU, steadily and in bursts (a stall, then several at once): up to the driver's queue depth (DRIVER_LAG) every frame past
    the warm-up is reported, and it is the newest landed set; further behind,
    frames go unmeasured rather than a
    set being re-issued while unread; a result is never read before it
    landed, the one reported is always the newest landed, and reports never
    go backwards.

The live counterpart, which needs a GPU, is tools/rspeeds-ssao-check.sh.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
rm = (ROOT / "engine/hexen2/gl_rmain.c").read_text()
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


def define(name):
    m = re.search(r"#define " + name + r"\s+(.+)", rm)
    return m.group(1).split("/*")[0].strip() if m else None


# Frames a GPU-bound desktop driver can run behind the CPU: drivers commonly
# queue two or three, plus the one the GPU is drawing.
DRIVER_LAG = 4

ring = define("RPROF_RING")
if ring is None or not ring.isdigit() or not DRIVER_LAG <= int(ring) <= 16:
    errors.append(f"RPROF_RING must be defined, from {DRIVER_LAG} (the driver's queue) to 16 "
                  "(past that the GPU line drifts far from the CPU block under it)")
if "static GLuint\trprof_queries[RPROF_RING][RPROF_QUERIES];" not in rm:
    errors.append("rprof_queries must be RPROF_RING sets of RPROF_QUERIES")
if "glGenQueries_fp(RPROF_RING * RPROF_QUERIES, rprof_queries[0]);" not in rm:
    errors.append("R_ProfileInit must allocate every set")

fns = {}
for fn, head in (("begin", r"static void R_ProfileBeginFrame \(void\)\s*\{"),
                 ("stamp", r"static void R_ProfileTimestamp \(int idx\)\s*\{"),
                 ("end", r"static void R_ProfileEndFrame \(void\)\s*\{"),
                 ("landed", r"static int R_ProfileNewestLanded \(void\)\s*\{"),
                 ("report", r"static void R_ProfileReport \(void\)\s*\{"),
                 ("view", r"void R_RenderView \(void\)\s*\{")):
    fns[fn] = body_of(rm, head)
    if fns[fn] is None:
        errors.append(f"missing {head.split(chr(92))[0]}")

if not errors:
    rep = fns["report"]
    i_land = rep.find("newest = R_ProfileNewestLanded ();")
    i_res = rep.find("GL_QUERY_RESULT,")
    if i_land < 0 or i_res < i_land or "rprof_queries[newest][i], GL_QUERY_RESULT," not in rep:
        errors.append("R_ProfileReport must read GL_QUERY_RESULT only from the set R_ProfileNewestLanded returned")
    if "GL_QUERY_RESULT_AVAILABLE" in rep:
        errors.append("R_ProfileReport must leave the availability test to R_ProfileNewestLanded")
    land = fns["landed"]
    if "GL_QUERY_RESULT_AVAILABLE" not in land or re.search(r"GL_QUERY_RESULT\b(?!_)", land):
        errors.append("R_ProfileNewestLanded must test GL_QUERY_RESULT_AVAILABLE and never read GL_QUERY_RESULT")
    view = fns["view"]
    i_r = view.find("R_ProfileReport();")
    i_b = view.find("R_ProfileBeginFrame();")
    i_w = view.find("R_ProfileTimestamp(RPROF_WORLD);")
    i_e = view.find("R_ProfileTimestamp(RPROF_COUNT); R_ProfileEndFrame();")
    if not (0 <= i_r < i_b < i_w < i_e):
        errors.append("R_RenderView must report, claim a set, stamp, then close the set after RPROF_COUNT")
    if view.count("R_ProfileEndFrame") != 1 or view.count("R_ProfileBeginFrame") != 1:
        errors.append("R_RenderView must claim and close exactly one set per frame")

# ------------------------------------------------------------ model GPU
HARNESS = r"""
#include <stdio.h>
#include <string.h>
typedef int qboolean; enum { false, true };
typedef unsigned int GLuint; typedef int GLint; typedef unsigned int GLenum;
#define GL_TIMESTAMP 0x8E28
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define RPROF_COUNT %(count)s
#define RPROF_QUERIES %(queries)s
#define RPROF_RING %(ring)s
#define NQ (RPROF_RING * RPROF_QUERIES)

static int frame, lag, burst, bad;
static int issued[NQ + 1];	/* frame a query was last stamped in, -1 never */
static int unread[NQ + 1];	/* stamped and its availability not yet seen */

/* In order, `lag` frames behind; under `burst` the GPU stalls and then
 * retires several frames at once, every fourth frame. */
static int landed (int at)
{
	return at >= 0 && at <= frame - lag - (burst ? frame %% 4 : 0);
}

static void glQueryCounter_fp (GLuint id, GLenum t)
{
	(void)t;
	if (unread[id] && (id - 1) %% RPROF_QUERIES == RPROF_COUNT) bad |= 1;	/* re-issued in flight */
	issued[id] = frame;
	unread[id] = 1;
}
static void glGetQueryObjectiv_fp (GLuint id, GLenum p, GLint *v)
{
	(void)p;
	if (issued[id] < 0) { bad |= 2; *v = 0; return; }	/* never written */
	*v = landed (issued[id]);
	if (*v) unread[id] = 0;
}

static GLuint	rprof_queries[RPROF_RING][RPROF_QUERIES];
static qboolean	rprof_pending;
static qboolean	rprof_set_pending[RPROF_RING];
static int	rprof_set = -1;
static int	rprof_next_set;
static qboolean	rprof_available = true;

%(begin)s
%(stamp)s
%(end)s
%(landed)s

int main (void)
{
	int s, q, f, got, reported, last, expect, frames = 400;
	for (burst = 0; burst <= 1; burst++)
	for (lag = 1; lag <= 6; lag++)
	{
		memset (unread, 0, sizeof(unread));
		for (q = 0; q <= NQ; q++) issued[q] = -1;
		for (s = 0; s < RPROF_RING; s++)
			for (q = 0; q < RPROF_QUERIES; q++)
				rprof_queries[s][q] = 1 + s * RPROF_QUERIES + q;
		memset (rprof_set_pending, 0, sizeof(rprof_set_pending));
		rprof_set = -1; rprof_next_set = 0; rprof_pending = false;
		reported = 0; last = -1;
		for (f = 0; f < frames; f++)
		{
			frame = f;
			expect = -1;
			for (s = 0; s < RPROF_RING; s++)
			{
				int at = issued[rprof_queries[s][RPROF_COUNT]];
				if (rprof_set_pending[s] && landed (at) && at > expect)
					expect = at;
			}
			got = R_ProfileNewestLanded ();
			if (got >= 0)
			{
				int at = issued[rprof_queries[got][RPROF_COUNT]];
				if (!landed (at)) bad |= 4;	/* read before it landed */
				if (at <= last) bad |= 8;	/* went backwards */
				if (at != expect) bad |= 16;	/* a landed set, not the newest */
				last = at;
				reported++;
			}
			else if (expect >= 0)
				bad |= 32;	/* one had landed, none reported */
			R_ProfileBeginFrame ();
			/* R_RenderView's order: world, the shadow pair inside it,
			 * the rest, RPROF_COUNT last. */
			R_ProfileTimestamp (0);
			R_ProfileTimestamp (RPROF_COUNT + 1);
			R_ProfileTimestamp (RPROF_COUNT + 2);
			for (q = 1; q <= RPROF_COUNT; q++)
				R_ProfileTimestamp (q);
			R_ProfileEndFrame ();
		}
		if (!burst)
			printf ("%%d %%d\n", lag, reported);
	}
	printf ("bad %%d\n", bad);
	return 0;
}
"""

if not errors:
    src = HARNESS % dict(count=define("RPROF_COUNT"), queries="(RPROF_COUNT + 3)", ring=ring,
                         begin=fns["begin"], stamp=fns["stamp"], end=fns["end"], landed=fns["landed"])
    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / "ring.c"
        exe = Path(d) / "ring"
        c.write_text(src)
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-w", "-o", str(exe), str(c)],
                           capture_output=True, text=True)
        if r.returncode:
            errors.append("ring harness failed to compile: " + r.stderr.strip()[:400])
        else:
            out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.split("\n")
            bad = [l for l in out if l.startswith("bad ")]
            if bad != ["bad 0"]:
                errors.append(f"ring harness: {bad} (1 re-issued an unread set, 2 read an unwritten one, "
                              "4 read before it landed, 8 reported out of order, 16 not the newest landed, 32 a landed one missed)")
            for line in out:
                parts = line.split()
                if len(parts) == 2 and parts[0].isdigit():
                    lag, got = int(parts[0]), int(parts[1])
                    # The driver queues up to DRIVER_LAG frames, whatever the
                    # ring is: up to it, every frame but the first `lag` (the
                    # report runs before the frame claims its set).  Past it
                    # the ring may fill; frames then go unmeasured, never a
                    # re-issued unread set.
                    if lag <= DRIVER_LAG:
                        want = 400 - lag
                    else:
                        want = 400 * min(int(ring), lag) // (lag + 1) - 2 * lag
                    if got < want:
                        errors.append(f"a GPU {lag} frame(s) behind got {got} of 400 frames reported, want >= {want}")

if errors:
    print("FAIL test_rprof_ring:")
    for e in errors:
        print("  " + e)
    sys.exit(1)
print("ok: r_speeds 2 rings its timestamp sets, reads only what has landed, and reports every frame of a "
      "GPU up to the driver's queue depth behind")
