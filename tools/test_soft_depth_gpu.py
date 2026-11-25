#!/usr/bin/env python3
"""The soft-particle depth snapshot is taken under -gpu, from whatever the
scene is drawing into (#340 P4b).

GL_SoftDepth_Capture (engine/h2shared/gl_postprocess.c) copies the opaque
scene's depth into a texture of its own after the opaque pass, and the
sprites' fade samples that.  Through P4a-2 it returned at once under -gpu;
P4b lifts that gate, and the copy runs on rg_target.c's depth arm, which
makes only a whole 1:1 copy between textures of one format.  So under -gpu
the snapshot is GL_DEPTH24_STENCIL8 whatever the source: that is the scene's
own depth format there (rg_resource.c RGT_Format), framebuffer 0's as much
as pp_fbo's.  GL keeps GL_DEPTH_COMPONENT24 for framebuffer 0.

This lifts GL_SoftDepth_Capture, GL_SoftDepth_GetTex and PP_DeleteSoftDepth
out of gl_postprocess.c and compiles them, with gl_fbo.c's -gpu arms, against
the real engine/h2shared/rg_target.c and tools/test_rg_target.py's fake
SDL_GPU, frame recorder and resource table, then checks:

  * post-process off: the snapshot is taken, and from the scene's own depth
    (framebuffer 0);
  * post-process on: from pp_fbo's depth, the framebuffer bound for drawing,
    not framebuffer 0's (a stale copy of the last frame drawn straight);
  * r_scale: at the scaled size, a new texture; and a size the source does
    not have is a copy declined, with no snapshot -- valid only if the copy
    was made;
  * the mirror pass's second snapshot in a frame reuses the texture;
  * a scene depth format the device cannot sample takes none: the
    placeholder bound in its place reads as the near plane, and every soft
    sprite would fade out;
  * r_softparticles 0 takes none;
  * every capture puts both framebuffer bindings back, the read one too
    when it is not the draw one.

On the pre-P4b code the first check fails: the gate leaves no snapshot.

Usage: test_soft_depth_gpu.py [<gl_postprocess.c>]   (default: the tree's)

Stock python3 and a C compiler (cc / gcc / clang); lint.yml.
"""
from pathlib import Path
import re
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_rg_target as rgt  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PP = ROOT / "engine/h2shared/gl_postprocess.c"

GLUE = r'''
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_INT_24_8 0x84FA
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
GLuint RG_FramebufferBinding (GLenum pname);

static int rg_active = 1;
#define R_GPU() (rg_active)
typedef struct { int integer; float value; } cvar_t;
static cvar_t r_softparticles = { 1, 1.0f };
static int glwidth = 64, glheight = 32;
static int pp_samples;
static GLuint scene_fbo;	/* GL_GetSceneFBO: pp_fbo while the chain is up */
static void (*glBlitFramebuffer_fp) (void) = (void (*) (void)) 1;	/* the null table's, not NULL */
GLuint GL_GetSceneFBO (void) { return scene_fbo; }
static qboolean depth_sampled = true;	/* rg_frame.c: the scene's depth format can be sampled */
static qboolean RG_DepthSampled (void) { return depth_sampled; }
static void Con_DPrintf (const char *fmt, ...) { (void) fmt; }

/* gl_fbo.c's and gl_texcreate.c's -gpu arms */
#define R_GenFramebuffers RG_GenFramebuffers
#define R_DeleteFramebuffers RG_DeleteFramebuffers
#define R_BindFramebuffer RG_BindFramebuffer
#define R_FramebufferTexture2D RG_FramebufferTexture2D
#define R_DrawBuffers RG_DrawBuffers
#define R_CheckFramebufferStatus RG_CheckFramebufferStatus
#define R_BlitFramebuffer RG_BlitFramebuffer
#define R_FramebufferBinding RG_FramebufferBinding
#define R_GenTextures RG_GenTextures
#define R_DeleteTextures RG_DeleteTextures
static void R_TexImage2D (GLenum target, GLuint tex, GLint level, GLint ifmt, GLsizei w, GLsizei h,
			  GLenum format, GLenum type, const void *pixels)
{
	RG_TexImage (target, tex, level, ifmt, w, h, 1, format, type, pixels);
}
static void R_BindTextureForEdit (GLenum target, GLuint tex) { (void) target; (void) tex; }
static void R_TexSetFilter (GLenum target, GLuint tex, GLint mn, GLint mg) { (void) target; (void) tex; (void) mn; (void) mg; }
static void R_TexSetWrap (GLenum target, GLuint tex, GLint w) { (void) target; (void) tex; (void) w; }

static GLuint	soft_depth_fbo;
static GLuint	soft_depth_tex;
static int	soft_depth_w, soft_depth_h;
static GLenum	soft_depth_fmt;
static qboolean	soft_depth_valid;

%(lifted)s

/* a pp_fbo: colour and GL_DEPTH24_STENCIL8 depth, as PP_CreateFBO makes it */
static GLuint SceneFbo (int w, int h, GLuint *depth)
{
	GLuint	fbo, c;

	RG_GenTextures (1, &c);
	RG_TexImage (GL_TEXTURE_2D, c, 0, GL_RGBA8, w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	RG_GenTextures (1, depth);
	RG_TexImage (GL_TEXTURE_2D, *depth, 0, GL_DEPTH24_STENCIL8, w, h, 1, GL_DEPTH_STENCIL,
		     GL_UNSIGNED_INT_24_8, NULL);
	RG_GenFramebuffers (1, &fbo);
	RG_BindFramebuffer (GL_FRAMEBUFFER, fbo);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
	RG_FramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, *depth, 0);
	return fbo;
}

static qboolean Bound (GLuint draw, GLuint read)
{
	return RG_FramebufferBinding (GL_DRAW_FRAMEBUFFER_BINDING) == draw &&
	       RG_FramebufferBinding (GL_READ_FRAMEBUFFER_BINDING) == read;
}

int main (void)
{
	GLuint	pp, pp_depth, small, small_depth, tex0;
	int	n;

	RG_Target_Init ();

	/* post-process off: the scene is framebuffer 0 */
	RG_BindFramebuffer (GL_FRAMEBUFFER, 0);
	ncopies = 0;
	GL_SoftDepth_Capture ();
	CHECK (GL_SoftDepth_GetTex () != 0, "post-process off: a snapshot is taken under -gpu");
	CHECK (ncopies == 1 && last_copy_src == &scene_depth,
	       "post-process off: copied from framebuffer 0's depth, the scene's");
	CHECK (GL_SoftDepth_GetTex () && last_copy_dst == &tex[GL_SoftDepth_GetTex ()].tex,
	       "into the snapshot texture");
	CHECK (Bound (0, 0), "both bindings put back");
	tex0 = GL_SoftDepth_GetTex ();

	/* the mirror pass's second snapshot, same frame */
	n = ntex;
	GL_SoftDepth_Capture ();
	CHECK (GL_SoftDepth_GetTex () == tex0 && ncopies == 2 && ntex == n,
	       "a second snapshot in a frame copies again into the same texture");

	/* post-process on: pp_fbo is bound for drawing */
	pp = SceneFbo (64, 32, &pp_depth);
	scene_fbo = pp;
	RG_BindFramebuffer (GL_FRAMEBUFFER, pp);
	RG_BindFramebuffer (GL_READ_FRAMEBUFFER, 0);	/* read apart from draw */
	ncopies = 0;
	GL_SoftDepth_Capture ();
	CHECK (GL_SoftDepth_GetTex () != 0 && ncopies == 1, "post-process on: a snapshot is taken");
	CHECK (last_copy_src == &tex[pp_depth].tex,
	       "post-process on: copied from pp_fbo's depth (the draw binding), not the read binding's");
	CHECK (Bound (pp, 0), "both bindings put back, the read one apart from the draw one");
	RG_BindFramebuffer (GL_READ_FRAMEBUFFER, pp);

	/* r_scale 0.5: pp_fbo and glwidth / glheight at the scaled size */
	small = SceneFbo (32, 16, &small_depth);
	scene_fbo = small;
	RG_BindFramebuffer (GL_FRAMEBUFFER, small);
	glwidth = 32; glheight = 16;
	ncopies = 0;
	n = ntex;
	GL_SoftDepth_Capture ();
	CHECK (GL_SoftDepth_GetTex () != 0 && ntex > n && tex[GL_SoftDepth_GetTex ()].w == 32 &&
	       tex[GL_SoftDepth_GetTex ()].h == 16 && ncopies == 1 &&
	       last_copy_src == &tex[small_depth].tex,
	       "r_scale: a snapshot at the scaled size, from the scaled pp_fbo");

	/* a size the bound source does not have: declined, no snapshot */
	glwidth = 64; glheight = 32;
	ncopies = 0;
	GL_SoftDepth_Capture ();
	CHECK (GL_SoftDepth_GetTex () == 0 && ncopies == 0,
	       "a copy the backend declines leaves no snapshot (valid only if the copy was made)");
	CHECK (Bound (small, small), "both bindings put back after a declined copy");

	/* a scene depth format the device cannot sample: no snapshot */
	glwidth = 32; glheight = 16;
	GL_SoftDepth_Capture ();
	CHECK (GL_SoftDepth_GetTex () != 0, "sampled again: the snapshot is back");
	depth_sampled = false;
	ncopies = 0;
	GL_SoftDepth_Capture ();
	CHECK (GL_SoftDepth_GetTex () == 0 && ncopies == 0,
	       "an unsampleable depth format takes no snapshot (its placeholder reads as the near plane)");
	depth_sampled = true;

	/* r_softparticles 0 */
	RG_BindFramebuffer (GL_FRAMEBUFFER, 0);
	scene_fbo = 0;
	r_softparticles.integer = 0;
	ncopies = 0;
	GL_SoftDepth_Capture ();
	CHECK (GL_SoftDepth_GetTex () == 0 && ncopies == 0, "r_softparticles 0: no snapshot");

	RG_Target_Shutdown ();
	if (failures)
		return 1;
	printf ("ok\n");
	return 0;
}
'''


def main():
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        sys.exit("FAIL: no C compiler")
    pp = Path(sys.argv[1]) if len(sys.argv) > 1 else PP
    code = rgt.strip_comments(pp.read_text())
    try:
        lifted = "\n".join(rgt.function(code, rx, what) for rx, what in (
            (r"^static void PP_DeleteSoftDepth \(void\)\s*$", "PP_DeleteSoftDepth in gl_postprocess.c"),
            (r"^void GL_SoftDepth_Capture \(void\)\s*$", "GL_SoftDepth_Capture in gl_postprocess.c"),
            (r"^GLuint GL_SoftDepth_GetTex \(void\)\s*$", "GL_SoftDepth_GetTex in gl_postprocess.c")))
    except AssertionError as e:
        sys.exit("FAIL: " + str(e))
    # The harness defines USE_SDL_GPU, as the -gpu build does; the lifted
    # body's own `#ifdef USE_SDL_GPU` arms are what is under test.
    if not re.search(r"#\s*ifdef\s+USE_SDL_GPU", lifted):
        sys.exit("FAIL: GL_SoftDepth_Capture has no USE_SDL_GPU arm -- under -gpu the snapshot must be "
                 "GL_DEPTH24_STENCIL8, the scene's own depth format there (#340 P4b)")
    with tempfile.TemporaryDirectory() as td:
        t = Path(td)
        (t / "quakedef.h").write_text(rgt.QUAKEDEF)
        (t / "rg_internal.h").write_text(rgt.RG_INTERNAL % {"rgtarget": rgt.rgtarget_block()})
        (t / "harness.c").write_text(
            rgt.FAKES
            + '\n#define CHECK(c, what) do { if (!(c)) { printf ("FAIL %s:%d: %s\\n", __func__, '
              '__LINE__, what); failures++; } } while (0)\n'
            + GLUE % {"lifted": lifted})
        shutil.copy2(rgt.SRC, t / "rg_target.c")
        err = rgt.compile_and_run(cc, td, [t / "harness.c", t / "rg_target.c"], "GL_SoftDepth_Capture")
    if err:
        sys.exit("FAIL: " + err)
    print("PASS the soft-particle snapshot is taken under -gpu, from the bound scene target "
          "(framebuffer 0 or pp_fbo), at r_scale's size, twice in a mirror frame, and only when the "
          "copy was made (#340 P4b)")


if __name__ == "__main__":
    main()
