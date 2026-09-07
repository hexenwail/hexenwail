# Hi-Z Occlusion-Cull Acceptance Sweep — Runbook (uhexen2-8pzr)

> **This sweep was run and it passed.** `gl_hiz_cull` now defaults to `1`
> (`engine/h2shared/gl_worldcull.c:86`), and the sweep measured a 44–58% cull
> rate — see the Hi-Z row in `history/ARE_WE_IRONWAIL_YET.md`. The document is
> kept because the four gates below are the reusable part: run them again after
> any change to the culling path, or to re-qualify the default on new hardware.
>
> `uhexen2-8pzr` was an id in the **beads** tracker, which is retired. It is a
> breadcrumb, not a ticket.

Goal, as originally written: validate gates 1–4 before flipping `gl_hiz_cull`
default from 0 to 1.

## Prereqs
- Build at HEAD with `screenhash` command (commit `e140b4c3d` or later).
- Pick a save or demo with a heavy vista (Egypt/Cathedral exterior) AND a closet (Hall of Heroes interior).

## Gate 1 — cull rate ≥10% of post-frustum surfaces

In a heavy vista, stationary:
```
gl_hiz_cull 1
gl_hiz_stats 60          ; readback every 60 frames
wait 600                  ; 10 readbacks
gl_hiz_stats 0
```
Console prints `hiz cull rate = X.X% of post-frustum (gate 10.0%)`.  **PASS if median X ≥ 10.0%.**

## Gate 2 — frame time delta on heavy vista vs closet

Heavy vista timedemo (capture median ms/frame; example using `demo1` — pick whichever demo runs through the heaviest map):
```
gl_hiz_cull 0
timedemo demo1
; record reported FPS and ms/frame

gl_hiz_cull 1
timedemo demo1
; record again
```
**PASS if heavy-vista median frame time ≥3% faster with cull on.**

Repeat in a closet-like scene.  **PASS if closet median ≤1% slower with cull on.**

## Gate 3 — visual-regression hash (stationary camera, multiple maps)

At a known vantage on each of: demo1 start, demo2 start, demo3 start, Cathedral start, Hall of Heroes hub:
```
gl_hiz_cull 0
screenhash               ; record hex value
gl_hiz_cull 1
screenhash               ; record hex value
```
**PASS if hashes match exactly per camera.**

Note: any moving sprites, animated water, etc. in frame will break the hash — re-run with `host_framerate 0; pause` if needed.

## Gate 4 — no GL errors with OIT

```
r_oit 1
gl_hiz_cull 1
developer 1
; play through demo1
```
**PASS if no `glGetError` or framebuffer-incomplete log lines appear.**

## Result handling
- All 4 PASS → set the `gl_hiz_cull` default to `"1"` in
  `engine/h2shared/gl_worldcull.c` and commit. *(Done: the default is `"1"` as
  of the sweep that closed this out.)*
- Any FAIL → record the failing gate and its numbers alongside the sweep, and
  leave the default at 0.

If you are re-running these gates, open a GitHub issue to carry the result;
there is no beads tracker to record it in any more.
