#!/usr/bin/env python3
"""The 128^3 softemu palette LUT's CPU builder is exact (GitHub #80).

PP_FillPaletteGrid replaces a scan of all 255 entries per cell with a scan
of per-block candidates, pruned by box distance bounds.  This compiles the
REAL metric functions and the walk out of gl_postprocess.c and checks, over
the full 128^3 grid and all three metrics, that it returns exactly what the
brute-force scan returns (first strict minimum, so ties included) -- on a
palette built to have exact duplicates and grey ramps, where ties are common.
It also reports the speed-up.  If the Hexen II palette is present
(HEXEN2_PALETTE=path/to/palette.lmp), it is checked too.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "engine/h2shared/gl_postprocess.c").read_text()

def function(sig):
    start = SRC.index(sig)
    brace = SRC.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (SRC[end] == "{") - (SRC[end] == "}")
        end += 1
    return SRC[start:end]

enum_start = SRC.index("typedef enum", SRC.index("SOFTEMU_METRIC_NAIVE") - 400)
enum_end = SRC.index(";", SRC.index("}", enum_start)) + 1
parts = [SRC[enum_start:enum_end]]
for sig in ("static float PP_SRGBToLinear (float v)",
            "static float PP_NormalizeChannel (softemu_metric_t metric",
            "static void PP_MixChannels (softemu_metric_t metric",
            "static void PP_NormalizeColor (softemu_metric_t metric",
            "static float PP_ColorDistanceSquared (softemu_metric_t metric",
            "static void PP_MetricWeights (softemu_metric_t metric",
            "static void PP_FillPaletteGrid (softemu_metric_t metric"):
    parts.append(function(sig))

harness = r'''
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef int qboolean;
#define PP_LUT_DIM 128
#define PP_GRID_TO_8BIT(i) (((i) * 255) / (PP_LUT_DIM - 1))
#define PP_LUT_BLOCK 8
''' + "\n".join(parts) + r'''
static unsigned char pal[256][3];
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static unsigned char scan_lut[PP_LUT_DIM*PP_LUT_DIM*PP_LUT_DIM], walk_lut[PP_LUT_DIM*PP_LUT_DIM*PP_LUT_DIM];
static int check(const char *name){
  static float pc[256][3];
  long bad=0;
  for (int m=0;m<3;m++){
    for(int i=0;i<255;i++) PP_NormalizeColor((softemu_metric_t)m,pal[i][0],pal[i][1],pal[i][2],pc[i]);
    double t0=now();
    for(int b=0;b<PP_LUT_DIM;b++) for(int g=0;g<PP_LUT_DIM;g++) for(int r=0;r<PP_LUT_DIM;r++){
      float t[3]; PP_NormalizeColor((softemu_metric_t)m,PP_GRID_TO_8BIT(r),PP_GRID_TO_8BIT(g),PP_GRID_TO_8BIT(b),t);
      int best=0; float bd=1e30f;
      for(int i=0;i<255;i++){float d=PP_ColorDistanceSquared((softemu_metric_t)m,t,pc[i]); if(d<bd){bd=d;best=i;}}
      scan_lut[(b*PP_LUT_DIM+g)*PP_LUT_DIM+r]=best; }
    double t1=now();
    PP_FillPaletteGrid((softemu_metric_t)m,pc,255,walk_lut);
    double t2=now();
    long mb=0; for(long k=0;k<PP_LUT_DIM*PP_LUT_DIM*PP_LUT_DIM;k++) if(scan_lut[k]!=walk_lut[k]){ if(bad+mb<5) fprintf(stderr,"%s metric %d texel %ld: walk %d scan %d\n",name,m,k,walk_lut[k],scan_lut[k]); mb++; }
    bad+=mb;
    printf("%s metric %d: %ld mismatches; scan %.2fs, walk %.2fs (%.1fx)\n",name,m,mb,t1-t0,t2-t1,(t1-t0)/(t2-t1));
  }
  return bad!=0;
}
int main(int argc,char**argv){
  int fail=0; unsigned s=12345;
  for(int i=0;i<255;i++){ for(int c=0;c<3;c++){ s=s*1103515245u+12345u; pal[i][c]=(s>>16)&255; } }
  for(int i=0;i<32;i++){ pal[i][0]=pal[i][1]=pal[i][2]=i*8; }          /* grey ramp */
  for(int i=200;i<230;i++) memcpy(pal[i],pal[i-100],3);               /* exact duplicates: ties */
  fail|=check("synthetic");
  if(argc>1){ FILE*f=fopen(argv[1],"rb"); if(f){ fread(pal,3,256,f); fclose(f); fail|=check("hexen2 palette"); } }
  return fail;
}
'''
with tempfile.TemporaryDirectory() as t:
    c = Path(t) / "lut.c"
    c.write_text(harness)
    exe = Path(t) / "lut"
    subprocess.run(["cc", "-O2", "-std=c99", "-D_POSIX_C_SOURCE=199309L", str(c), "-o", str(exe), "-lm"], check=True)
    args = [str(exe)]
    if os.environ.get("HEXEN2_PALETTE"):
        args.append(os.environ["HEXEN2_PALETTE"])
    r = subprocess.run(args, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode:
        sys.exit("FAIL: the pruned walk disagrees with the brute-force scan")
print("PASS softemu LUT walk == brute-force scan, all metrics, 128^3 (#80)")
