#!/usr/bin/env python3
"""warp_recon.py -- how well a given gl_subdivide_size reconstructs the liquid warp.

Companion to bsp_subdiv.py.  That tool answers what a subdivision size COSTS;
this one answers whether it WORKS, which is the half uhexen2-9o7u left open as
"a visual judgement".  It is not only a visual judgement: the warp is a sine
evaluated per vertex and linearly interpolated across each tile, so the error is
a sampling question with an exact answer.

The warp (EmitWaterPolys, engine/h2shared/gl_warp.c):

    s = os + warpamt * turbsin[(int)((ot*0.125 + wtime) * TURBSCALE) & 255]
    t = ot + warpamt * turbsin[(int)((os*0.125 + wtime) * TURBSCALE) & 255]

turbsin[i] is 8*sin(2*pi*i/256), so the displacement is a sine of amplitude 8
texture units whose argument advances 0.125 rad per texture unit -- a spatial
period of 2*pi/0.125 = 16*pi = 50.265 units.  GL_SubdivideSurface samples it at
tile corners gl_subdivide_size apart and the rasteriser interpolates linearly
between them.

Two distinct failures, and they need different sample rates to fix:

  ALIASING, below 2 samples/period.  The reconstruction is not a degraded sine,
  it is a different and slower wave.  Fixed by clearing Nyquist.

  RECONSTRUCTION ERROR, at any rate.  Nyquist guarantees the signal is
  RECOVERABLE, by an ideal (sinc) filter.  Linear interpolation is not that
  filter, and its error falls as (omega*h)^2 -- so clearing Nyquist barely
  helps.  This is the part that makes "pick a bigger subdivision" a dead end.

Usage:  tools/warp_recon.py [--sizes 64,32,24,16,8] [--warpamt 0.5]
"""
import math, sys

A         = 8.0     # turbsin amplitude, texture units (gl_warp_sin.h)
OMEGA     = 0.125   # rad per texture unit (EmitWaterPolys: ot*0.125)
PERIOD    = 2 * math.pi / OMEGA

def f(x):
    return A * math.sin(OMEGA * x)

def lin_error(h, samples=200000):
    """Max and RMS error of piecewise-linear interpolation of f at spacing h."""
    span = PERIOD * 8
    worst = 0.0
    acc   = 0.0
    for i in range(samples):
        x  = span * i / samples
        x0 = math.floor(x / h) * h
        fr = (x - x0) / h
        e  = abs(f(x0) * (1 - fr) + f(x0 + h) * fr - f(x))
        worst = max(worst, e)
        acc  += e * e
    return worst, math.sqrt(acc / samples)

def apparent_period(h):
    """Period the surface actually shows, after aliasing.  None if it reads flat."""
    cps = h / PERIOD                    # cycles advanced per sample
    a   = abs(cps - round(cps))         # aliased fractional frequency
    return None if a < 1e-9 else h / a

def main(argv):
    sizes   = [64, 32, 24, 16, 8]
    warpamt = 0.5                       # R_WaterWarpAmount default; vanilla is 1.0
    i = 0
    while i < len(argv):
        if argv[i] == '--sizes':
            i += 1; sizes = [float(x) for x in argv[i].split(',')]
        elif argv[i] == '--warpamt':
            i += 1; warpamt = float(argv[i])
        i += 1

    print(f"warp period {PERIOD:.2f} texture units, amplitude {A:.1f} "
          f"(x{warpamt} = {A*warpamt:.1f} units of throw)\n")
    print(f"{'size':>5} {'samp/period':>11} {'max err':>9} {'rms err':>9} "
          f"{'%of amp':>8}  {'apparent period':>17}")
    for h in sizes:
        mx, rms = lin_error(h)
        ap = apparent_period(h)
        aps = "flat" if ap is None else f"{ap:7.1f} ({ap/PERIOD:.1f}x)"
        print(f"{h:>5.0f} {PERIOD/h:>11.2f} {mx*warpamt:>9.3f} {rms*warpamt:>9.3f} "
              f"{100*mx/A:>7.1f}% {aps:>17}")

    print(f"""
Below 2.00 samples/period the apparent period is wrong -- the surface shows an
alias, not the ripple.  Above it the period is right but the SHAPE is not: the
error column is the peak deviation from the intended sine, as a fraction of its
own amplitude.  Useful reference points at warpamt {warpamt}:

  size 64  the shipped default, and 0.79 samples/period -- aliased outright
  size 24  clears Nyquist, and still {100*lin_error(24.0)[0]/A:.0f}% peak error
  size  8  the first size that gets under ~15%, at a geometry cost
           bsp_subdiv.py can price and no one will want to pay

That gap is the argument for evaluating the warp per pixel instead: a fragment
shader has no reconstruction error at any tile size, so gl_subdivide_size stops
mattering for turb surfaces entirely.""")
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
