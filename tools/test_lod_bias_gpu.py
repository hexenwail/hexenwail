#!/usr/bin/env python3
"""gl_lodbias reaches the SDL_GPU sampler, and `auto` reads the samples in use.

GitHub #521.  Two halves:

  - Under -gpu, GL_Init_NullGL cleared gl_renderer_caps and never set
    texture_lod_bias, so GL_ApplyLodBias returned early and an explicit
    `gl_lodbias -1` never reached RG_TexParameter, t->lod_bias or the
    sampler's mip_lod_bias.  This checks the cap is set there and that the
    backend carries the value all the way into SDL_GPUSamplerCreateInfo,
    invalidating the texture's cached sampler so a change applies live.

  - `gl_lodbias auto` scaled from vid_config_fsaa, the samples the player
    asked for, not the ones the scene FBO is built with.  With the cap on,
    -gpu (no MSAA yet) would get -0.5 at the default 4.  This lifts
    GL_ResolveLodBias and PP_SceneSamples out of the source, compiles them
    against stubs, and runs them: auto must be 0 under -gpu, follow the
    driver clamp on GL, and an explicit value must pass through untouched.
"""

from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
H2 = ROOT / "engine/h2shared"
DRAW = H2 / "gl_draw.c"
PP = H2 / "gl_postprocess.c"
VID = H2 / "gl_vidsdl.c"
RES = H2 / "rg_resource.c"


def strip_comments(text):
    def blank(match):
        return re.sub(r"[^\n]", " ", match.group(0))
    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)


def function(text, head_rx, what):
    m = re.search(head_rx, text, flags=re.M)
    if not m:
        raise AssertionError(f"cannot find {what}")
    brace = text.index("{", m.end() - 1 if text[m.end() - 1] == "{" else m.end())
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[m.start():end]


HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include <strings.h>
typedef struct { const char *string; float value; int integer; } cvar_t;
typedef int qboolean;
#define q_strcasecmp strcasecmp
#define USE_SDL_GPU 1
static int rg_active;
#define R_GPU() (rg_active)
static cvar_t gl_lodbias, vid_config_fsaa;
static int gl_max_samples;

%(funcs)s

static int fails;
static void check (const char *what, int gpu, int fsaa, int maxs,
		   const char *bias, float want)
{
	float got;
	rg_active = gpu;
	vid_config_fsaa.integer = fsaa;
	gl_max_samples = maxs;
	gl_lodbias.string = bias;
	sscanf (bias, "%%f", &gl_lodbias.value);
	got = GL_ResolveLodBias ();
	if (got != want)
	{
		printf ("FAIL %%s: got %%g want %%g\n", what, got, want);
		fails++;
	}
}

int main (void)
{
	check ("auto under -gpu, fsaa 4", 1, 4, 0, "auto", 0.0f);
	check ("auto under -gpu, fsaa 4, nonzero max", 1, 4, 32, "auto", 0.0f);
	check ("auto on GL, fsaa 4 granted", 0, 4, 32, "auto", -0.5f);
	check ("auto on GL, fsaa 2", 0, 2, 32, "auto", -0.25f);
	check ("auto on GL, 16 capped to 8", 0, 16, 8, "auto", -0.75f);
	check ("auto on GL, fsaa 0", 0, 0, 32, "auto", 0.0f);
	check ("AUTO spelling", 0, 4, 32, "AUTO", -0.5f);
	check ("explicit -1 under -gpu", 1, 4, 0, "-1", -1.0f);
	check ("explicit -0.5 on GL", 0, 0, 32, "-0.5", -0.5f);
	check ("explicit 0", 1, 4, 0, "0", 0.0f);
	if (!fails)
		printf ("ok\n");
	return fails != 0;
}
"""


def run_lifted(failures):
    draw = strip_comments(DRAW.read_text())
    pp = strip_comments(PP.read_text())
    try:
        resolve = function(draw, r"^static float GL_ResolveLodBias \(void\)\s*\{",
                           "GL_ResolveLodBias in gl_draw.c")
        scene = function(pp, r"^int PP_SceneSamples \(void\)\s*\{",
                         "int PP_SceneSamples (void) in gl_postprocess.c")
    except AssertionError as err:
        failures.append(str(err))
        return
    if "vid_config_fsaa" in resolve:
        failures.append("GL_ResolveLodBias reads vid_config_fsaa; `auto` must scale "
                        "from PP_SceneSamples(), the samples actually in use")
    if "PP_SceneSamples" not in resolve:
        failures.append("GL_ResolveLodBias's auto arm must call PP_SceneSamples()")
    create = function(pp, r"^static qboolean PP_CreateFBO \(int width, int height\)\s*\{",
                      "PP_CreateFBO")
    if not re.search(r"int\s+samples\s*=\s*PP_SceneSamples\s*\(\s*\)\s*;", create):
        failures.append("PP_CreateFBO must take its sample count from PP_SceneSamples(), "
                        "or the LOD bias and the FBO can disagree")

    src = HARNESS % {"funcs": "int PP_SceneSamples (void);\n" + resolve + "\n" + scene}
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "h.c"
        exe = Path(td) / "h"
        c.write_text(src)
        cc = os.environ.get("CC", "cc")
        r = subprocess.run([cc, "-std=gnu99", "-w", "-o", str(exe), str(c)],
                           capture_output=True, text=True)
        if r.returncode:
            failures.append("lifted GL_ResolveLodBias/PP_SceneSamples do not compile:\n"
                            + r.stderr)
            return
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            failures.append("lifted GL_ResolveLodBias:\n" + r.stdout)


def check_gpu_plumbing(failures):
    vid = strip_comments(VID.read_text())
    null = function(vid, r"^static void GL_Init_NullGL \(void\)\s*\{", "GL_Init_NullGL")
    wipe = null.find("memset (&gl_renderer_caps")
    cap = re.search(r"gl_renderer_caps\.texture_lod_bias\s*=\s*([^;]+);", null)
    if cap is None or cap.group(1).strip() == "false":
        failures.append("GL_Init_NullGL must set gl_renderer_caps.texture_lod_bias, "
                        "or GL_ApplyLodBias drops gl_lodbias under -gpu")
    elif wipe < 0 or cap.start() < wipe:
        failures.append("GL_Init_NullGL sets texture_lod_bias before the caps memset")

    res = strip_comments(RES.read_text())
    tp = function(res, r"^void RG_TexParameter \(GLenum target, GLuint tex, GLenum pname, "
                       r"GLint ivalue, GLfloat fvalue\)\s*\{", "RG_TexParameter")
    if not re.search(r"case\s+GL_TEXTURE_LOD_BIAS\s*:\s*t->lod_bias\s*=\s*fvalue\s*;\s*break\s*;", tp):
        failures.append("RG_TexParameter must store GL_TEXTURE_LOD_BIAS in t->lod_bias")
    if not re.search(r"t->sampler\s*=\s*NULL\s*;\s*\}\s*$", tp):
        failures.append("RG_TexParameter must drop t->sampler after a change, or "
                        "Draw_LodBias_f's re-apply never reaches the bound sampler")
    smp = function(res, r"^static SDL_GPUSampler \*RGT_Sampler \(rgtex_t \*t\)\s*\{", "RGT_Sampler")
    if not re.search(r"info\.mip_lod_bias\s*=\s*t->lod_bias\s*;", smp):
        failures.append("RGT_Sampler must put t->lod_bias into info.mip_lod_bias")
    lookup = function(res, r"^static SDL_GPUSampler \*RGT_SamplerFor \(", "RGT_SamplerFor")
    if not re.search(r"memcmp\s*\(\s*&rgs_cache\[i\]\.info\s*,\s*info\s*,\s*sizeof\s*\(\s*\*info\s*\)", lookup):
        failures.append("RGT_SamplerFor must key its cache on the whole create info, "
                        "or two biases share one sampler")

    # Each bias value adds a set of samplers, so the 64-entry cache must be
    # emptied, and only at the frame's end: mid-frame, a draw's binding array
    # can already hold a sampler the flush would release.
    full = re.search(r"if\s*\(\s*rgs_count\s*==\s*RG_MAX_SAMPLERS\s*\)\s*\{([^}]*)\}", lookup)
    if not full or not re.search(r"rgs_overflowed\s*=\s*true\s*;", full.group(1)):
        failures.append("RGT_SamplerFor must flag a full cache (rgs_overflowed) so the frame end empties it")
    try:
        flush = function(res, r"^static void RGT_FlushSamplers \(void\)\s*\{", "RGT_FlushSamplers")
        endf = function(res, r"^void RG_Res_EndFrame \(void\)\s*\{", "RG_Res_EndFrame")
    except AssertionError as err:
        failures.append(str(err))
        return
    for rx, why in ((r"SDL_ReleaseGPUSampler\s*\(\s*rg_dev\s*,\s*rgs_cache\[i\]\.sampler\s*\)", "release the cached samplers"),
                    (r"rgs_count\s*=\s*0\s*;", "empty the cache"),
                    (r"rgt\[i\]\.sampler\s*=\s*NULL\s*;", "forget every texture's sampler"),
                    (r"RGT_CreatePhSamplers\s*\(\s*\)\s*;", "rebuild the placeholder samplers"),
                    (r"rgs_overflowed\s*=\s*false\s*;", "clear the overflow flag")):
        if not re.search(rx, flush):
            failures.append(f"RGT_FlushSamplers must {why}")
    if not re.search(r"if\s*\(\s*rgs_overflowed\s*\|\|\s*rgs_count\s*>=\s*RG_SAMPLER_FLUSH_AT\s*\)\s*"
                     r"RGT_FlushSamplers\s*\(\s*\)\s*;", endf):
        failures.append("RG_Res_EndFrame must flush the sampler cache when it overflowed or is 3/4 full")
    calls = len(re.findall(r"\bRGT_FlushSamplers\s*\(\s*\)\s*;", res))
    if calls != 1:
        failures.append(f"RGT_FlushSamplers is called {calls} times; only RG_Res_EndFrame, after the "
                        "submit, may release samplers a binding array could hold")


def main():
    failures = []
    run_lifted(failures)
    check_gpu_plumbing(failures)
    if failures:
        print("FAIL:\n  " + "\n  ".join(failures), file=sys.stderr)
        return 1
    print("PASS: gl_lodbias reaches the SDL_GPU sampler; auto reads the samples in use")
    return 0


if __name__ == "__main__":
    sys.exit(main())
