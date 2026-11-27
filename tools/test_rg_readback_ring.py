#!/usr/bin/env python3
"""rg_frame.c's asynchronous readback ring keeps its promises (GitHub #340,
phase P5c).

gl_hiz_stats under -gpu reads its counters without stalling the frame: each
read is a download recorded into the frame's command buffer and handed over
once the fence of the submit that carried it has signalled.  A ring like
that fails quietly -- a read mapped before its download ran prints the
transfer buffer's old bytes, a fence released twice or queried after its
release is a use-after-free in the driver, a slot never freed refuses every
later read -- and none of that shows on a frame.  So this lifts the ring
(the "Asynchronous readbacks" section, RGF_Submit and RGF_SubmitMidFrame)
out of engine/h2shared/rg_frame.c, compiles it against a fake SDL_GPU whose
fences signal when the test says and whose downloads copy only once their
submit's fence has signalled, and checks:

  * a read is not collectable while recorded or submitted, and is the bytes
    the buffer held when it was recorded once its fence signalled;
  * reads of one submit share one fence, released exactly once, never queried
    after its release, whichever read is collected first or whether the frame
    start (RGF_ReadbacksAge) sees it first;
  * the frame's submit takes a fence only with a read aboard;
  * a mid-frame submit's wait completes what it carried; a failed submit or
    wait fails it (collect answers -1 and frees the slot);
  * a completed read nobody collects is freed RG_READBACK_ABANDON frames
    later, a read still in flight never, however many frames pass;
  * the ring full refuses the next request (0, counted), and frees room as
    reads are collected; a request outside a frame is refused;
  * a discarded read keeps its slot while in flight and frees it once done;
    one already done is freed at once;
  * shutdown releases a pending fence once and every transfer buffer.

Stock python3 and a C compiler; lint.yml.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "engine/h2shared/rg_frame.c"

PRELUDE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t Uint8; typedef uint32_t Uint32; typedef int qboolean;
#define true 1
#define false 0
#define SDL_zero(x) memset (&(x), 0, sizeof(x))
#define q_min(a, b) ((a) < (b) ? (a) : (b))
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUCommandBuffer { int id; } SDL_GPUCommandBuffer;
typedef struct SDL_GPUCopyPass { int x; } SDL_GPUCopyPass;
typedef struct SDL_GPUFence { int signalled, released, queried_after_release; } SDL_GPUFence;
typedef struct SDL_GPUBuffer { Uint8 data[256]; } SDL_GPUBuffer;
typedef struct SDL_GPUTransferBuffer { Uint8 data[512]; Uint32 size; int released; } SDL_GPUTransferBuffer;
typedef struct { SDL_GPUBuffer *buffer; Uint32 offset, size; } SDL_GPUBufferRegion;
typedef struct { SDL_GPUTransferBuffer *transfer_buffer; Uint32 offset; } SDL_GPUTransferBufferLocation;
typedef struct { int usage; Uint32 size; Uint32 props; } SDL_GPUTransferBufferCreateInfo;
#define SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD 1
static SDL_GPUDevice *rg_dev;
static int failures;
#define CHECK(c, what) do { if (!(c)) { printf ("FAIL %d: %s\n", __LINE__, what); failures++; } } while (0)

/* the frame recorder around the ring */
static SDL_GPUCommandBuffer cmds[64], *rgf_cmd;
static int ncmds, rgf_numdraws, rgf_frame_waits, rgf_frame_areads, ends, copies;
static unsigned long rgf_waits, rgf_frames;
static qboolean rgf_active;
static void RGF_Warn (const char *fmt, ...) { (void) fmt; }
static const char *SDL_GetError (void) { return "fake"; }
static qboolean RGF_EnsureCmd (void) { if (!rgf_cmd) { rgf_cmd = &cmds[ncmds++]; rgf_cmd->id = ncmds; } return true; }
static void RGF_EndSegment (void) { ends++; RGF_EnsureCmd (); rgf_numdraws = 0; }
static qboolean RGF_CopyOnly (void) { copies++; return RGF_EnsureCmd (); }
static void RGF_EndOfFrame (void) { }

/* downloads run when the fence of their command buffer signals */
typedef struct { SDL_GPUCommandBuffer *cmd; SDL_GPUBufferRegion src; SDL_GPUTransferBuffer *tb; Uint8 bytes[256]; int done; SDL_GPUFence *fence; } dl_t;
static dl_t dls[64]; static int ndls;
static SDL_GPUFence fences[64]; static int nfences, fail_submit, fail_wait;
static SDL_GPUCopyPass thepass;
static SDL_GPUCopyPass *SDL_BeginGPUCopyPass (SDL_GPUCommandBuffer *c) { (void) c; return &thepass; }
static void SDL_EndGPUCopyPass (SDL_GPUCopyPass *p) { (void) p; }
static void SDL_DownloadFromGPUBuffer (SDL_GPUCopyPass *p, const SDL_GPUBufferRegion *src, const SDL_GPUTransferBufferLocation *d)
{
	dl_t *x = &dls[ndls++];
	(void) p;
	x->cmd = rgf_cmd; x->src = *src; x->tb = d->transfer_buffer; x->done = 0; x->fence = NULL;
	/* what the buffer holds at this point of the command buffer; it reaches
	 * the transfer buffer only once the submit's fence signals */
	memcpy (x->bytes, src->buffer->data + src->offset, src->size);
}
static void Attach (SDL_GPUCommandBuffer *c, SDL_GPUFence *f)
{
	int i;
	for (i = 0; i < ndls; i++)
		if (dls[i].cmd == c && !dls[i].fence)
			dls[i].fence = f;
}
static void Signal (SDL_GPUFence *f)
{
	int i;
	f->signalled = 1;
	for (i = 0; i < ndls; i++)
		if (dls[i].fence == f && !dls[i].done)
		{
			memcpy (dls[i].tb->data, dls[i].bytes, dls[i].src.size);
			dls[i].done = 1;
		}
}
static qboolean SDL_SubmitGPUCommandBuffer (SDL_GPUCommandBuffer *c) { Attach (c, NULL); return !fail_submit; }
static SDL_GPUFence *SDL_SubmitGPUCommandBufferAndAcquireFence (SDL_GPUCommandBuffer *c)
{
	SDL_GPUFence *f;
	if (fail_submit) return NULL;
	f = &fences[nfences++];
	Attach (c, f);
	return f;
}
static qboolean SDL_WaitForGPUFences (SDL_GPUDevice *d, qboolean all, SDL_GPUFence *const *f, Uint32 n)
{
	(void) d; (void) all; (void) n;
	if (fail_wait) return false;
	Signal (f[0]);
	return true;
}
static qboolean SDL_QueryGPUFence (SDL_GPUDevice *d, SDL_GPUFence *f)
{
	(void) d;
	if (f->released) f->queried_after_release++;
	return f->signalled;
}
static void SDL_ReleaseGPUFence (SDL_GPUDevice *d, SDL_GPUFence *f) { (void) d; f->released++; }
static SDL_GPUTransferBuffer tbs[64]; static int ntbs;
static SDL_GPUTransferBuffer *SDL_CreateGPUTransferBuffer (SDL_GPUDevice *d, const SDL_GPUTransferBufferCreateInfo *ci)
{ (void) d; tbs[ntbs].size = ci->size; return &tbs[ntbs++]; }
static void SDL_ReleaseGPUTransferBuffer (SDL_GPUDevice *d, SDL_GPUTransferBuffer *t) { (void) d; t->released++; }
static int mapped_early;
static void *SDL_MapGPUTransferBuffer (SDL_GPUDevice *d, SDL_GPUTransferBuffer *t, qboolean cycle)
{
	int i;
	(void) d; (void) cycle;
	for (i = 0; i < ndls; i++)
		if (dls[i].tb == t && !dls[i].done)
			mapped_early++;
	return t->data;
}
static void SDL_UnmapGPUTransferBuffer (SDL_GPUDevice *d, SDL_GPUTransferBuffer *t) { (void) d; (void) t; }
"""

HARNESS = r"""
static SDL_GPUBuffer buf;
static void FrameStart (void) { rgf_frames++; rgf_active = true; RGF_ReadbacksAge (); }
static void FrameEnd (void) { RGF_EnsureCmd (); RGF_Submit (); rgf_active = false; }
static unsigned int Ask (Uint8 v)
{
	SDL_GPUBufferRegion r;
	memset (buf.data, v, sizeof(buf.data));
	r.buffer = &buf; r.offset = 0; r.size = 32;
	return RG_Frame_ReadbackRequest (&r);
}
static int Get (unsigned int t, Uint8 *v)
{
	Uint8 out[32];
	int r;
	memset (out, 0xEE, sizeof(out));
	r = RG_ReadbackCollect (t, out, sizeof(out));
	*v = out[0];
	return r;
}
static int Released (void) { int i, n = 0; for (i = 0; i < nfences; i++) n += fences[i].released; return n; }
static int Overreleased (void) { int i, n = 0; for (i = 0; i < nfences; i++) n += fences[i].released > 1; return n; }
static int Queried (void) { int i, n = 0; for (i = 0; i < nfences; i++) n += fences[i].queried_after_release; return n; }
static int Free (void) { int i, n = 0; for (i = 0; i < RG_READBACKS; i++) n += rgr_ring[i].state == RGR_FREE; return n; }

int main (void)
{
	unsigned int t1, t2, ts[RG_READBACKS + 1];
	SDL_GPUFence *pending;
	Uint8 v;
	int i, f0;

	/* 1. one read: pending until its fence signals, then the recorded bytes */
	FrameStart ();
	t1 = Ask (0x11);
	CHECK (t1 != 0 && copies == 1 && ends == 0, "a request with no draws lands the uploads alone");
	CHECK (Get (t1, &v) == 0, "recorded: not collectable");
	memset (buf.data, 0x99, sizeof(buf.data));	/* changed after it was recorded, before submit */
	FrameEnd ();
	CHECK (nfences == 1, "the frame's submit took a fence: a read was aboard");
	CHECK (Get (t1, &v) == 0, "submitted, unsignalled: not collectable");
	Signal (&fences[0]);
	CHECK (Get (t1, &v) == 1 && v == 0x11, "signalled: collected, the bytes it was recorded against");
	CHECK (Released () == 1 && Overreleased () == 0, "its fence released once");
	CHECK (Get (t1, &v) == -1, "a collected ticket is spent");
	CHECK (mapped_early == 0, "nothing mapped before its download ran");
	CHECK (Free () == RG_READBACKS, "the slot is free again");

	/* 2. a frame with no read submits without a fence */
	f0 = nfences;
	FrameStart (); FrameEnd ();
	CHECK (nfences == f0, "no read aboard: no fence");

	/* 3. two reads of one submit share its fence; the frame start sees it first */
	FrameStart ();
	t1 = Ask (0x21);
	rgf_numdraws = 3;
	t2 = Ask (0x22);
	CHECK (ends == 1, "a request after draws ends the segment first");
	FrameEnd ();
	CHECK (nfences == f0 + 1, "one fence for the submit");
	Signal (&fences[f0]);
	FrameStart ();
	CHECK (Released () == 2 && Overreleased () == 0, "the frame start found it signalled and released it once");
	CHECK (Get (t2, &v) == 1 && v == 0x22 && Get (t1, &v) == 1 && v == 0x21, "both reads, each its own bytes, any order");
	CHECK (Queried () == 0, "no fence queried after its release");
	FrameEnd ();

	/* 4. the same, collected before any frame start */
	FrameStart ();
	t1 = Ask (0x31); t2 = Ask (0x32);
	FrameEnd ();
	Signal (&fences[nfences - 1]);
	CHECK (Get (t1, &v) == 1 && v == 0x31, "first read collected");
	CHECK (Get (t2, &v) == 1 && v == 0x32, "its sibling done by the same fence");
	FrameStart ();
	CHECK (Queried () == 0 && Overreleased () == 0, "the sibling's fence neither queried nor released again");
	FrameEnd ();

	/* 5. a mid-frame submit's wait completes what it carried */
	FrameStart ();
	t1 = Ask (0x41);
	CHECK (RGF_SubmitMidFrame (), "mid-frame submit");
	CHECK (Get (t1, &v) == 1 && v == 0x41, "done once the mid-frame wait returned");
	FrameEnd ();

	/* 6. failures */
	FrameStart ();
	t1 = Ask (0x51);
	fail_submit = 1; FrameEnd (); fail_submit = 0;
	CHECK (Get (t1, &v) == -1 && Free () == RG_READBACKS, "a failed submit fails the read; collecting frees it");
	FrameStart ();
	t1 = Ask (0x52);
	fail_wait = 1; CHECK (!RGF_SubmitMidFrame (), "the wait fails"); fail_wait = 0;
	CHECK (Get (t1, &v) == -1 && Free () == RG_READBACKS, "a failed wait fails the read");
	rgf_active = false;
	CHECK (Ask (0x54) == 0 && Free () == RG_READBACKS, "outside a frame: refused");
	rgf_active = true;
	FrameEnd ();

	/* 7. abandoned: a done read nobody collects is freed later; one in flight never */
	FrameStart ();
	t1 = Ask (0x71);
	FrameEnd ();
	Signal (&fences[nfences - 1]);
	FrameStart ();
	t2 = Ask (0x72);
	FrameEnd ();
	pending = &fences[nfences - 1];
	for (i = 0; i < RG_READBACK_ABANDON + 2; i++) { FrameStart (); FrameEnd (); }
	CHECK (Get (t1, &v) == -1, "a done read nobody collected is freed after RG_READBACK_ABANDON frames");
	CHECK (Get (t2, &v) == 0, "a read whose fence has not signalled keeps its slot however long");
	Signal (pending);
	CHECK (Get (t2, &v) == 1 && v == 0x72 && Free () == RG_READBACKS, "and is collected once it signals");

	/* 8. the ring full */
	FrameStart ();
	for (i = 0; i <= RG_READBACKS; i++)
		ts[i] = Ask ((Uint8) i);
	CHECK (Free () == 0 && ts[RG_READBACKS] == 0, "the ninth request is refused");
	FrameEnd ();
	Signal (&fences[nfences - 1]);
	CHECK (Get (ts[0], &v) == 1 && v == 0 && Free () == 1, "a collect frees room");
	for (i = 1; i < RG_READBACKS; i++)
		CHECK (Get (ts[i], &v) == 1 && v == i, "each read its own bytes");

	/* 9. discard */
	FrameStart ();
	t1 = Ask (0x91);
	RG_ReadbackDiscard (t1);
	FrameEnd ();
	pending = &fences[nfences - 1];
	FrameStart ();
	CHECK (Free () == RG_READBACKS - 1, "a discarded read in flight keeps its slot");
	FrameEnd ();
	Signal (pending);
	FrameStart ();
	CHECK (Free () == RG_READBACKS, "freed at the frame start once done");
	t2 = Ask (0x92);
	CHECK (RGF_SubmitMidFrame (), "mid-frame submit");
	RG_ReadbackDiscard (t2);
	CHECK (Free () == RG_READBACKS && Get (t2, &v) == -1, "a done read discarded is freed at once");
	FrameEnd ();

	/* 10. shutdown */
	FrameStart ();
	t1 = Ask (0xA1);
	FrameEnd ();
	f0 = Released ();
	RGF_ReadbacksShutdown ();
	CHECK (Released () == f0 + 1 && Overreleased () == 0, "shutdown releases the pending fence once");
	for (i = 0; i < ntbs; i++)
		CHECK (tbs[i].released == 1, "every transfer buffer released once");
	CHECK (Queried () == 0, "no fence queried after its release");
	if (failures) return 1;
	puts ("ok");
	return 0;
}
"""


def main():
    code = SRC.read_text()
    a = code.find("/* Asynchronous readbacks (#340 P5c)")
    a = code.rfind("/* ----", 0, a)
    b = code.find("/* What a frame's end releases")
    if a < 0 or b < 0:
        sys.exit("FAIL: cannot find the readback ring in rg_frame.c")
    ring = code[a:b]
    subs = []
    for name in ("RGF_Submit", "RGF_SubmitMidFrame"):
        m = re.search(r"^static (?:void|qboolean) %s \(void\)\n\{.*?^\}\n" % name, code, re.M | re.S)
        if not m:
            sys.exit("FAIL: cannot lift %s" % name)
        subs.append(m.group(0))
    src = PRELUDE + ring + "\n".join(subs) + HARNESS
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "h.c").write_text(src)
        cc = os.environ.get("CC", "cc")
        r = subprocess.run([cc, "-std=gnu99", "-Wall", "-Wno-unused-function", "-Werror=implicit-function-declaration",
                            "-o", str(td / "h"), str(td / "h.c")], capture_output=True, text=True)
        if r.returncode:
            sys.exit("FAIL: the lifted ring does not compile:\n" + r.stderr[-3000:])
        r = subprocess.run([str(td / "h")], capture_output=True, text=True)
        if r.returncode:
            sys.exit("FAIL: the readback ring breaks a promise:\n" + (r.stdout + r.stderr)[-3000:])
    print("PASS rg_frame.c's readback ring: reads mapped only after their fence, one fence per submit "
          "released once and never queried after, mid-frame waits and failures, abandoned and in-flight "
          "slots, the ring full, discards and shutdown (#340 P5c)")


if __name__ == "__main__":
    main()
