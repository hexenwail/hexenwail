#!/usr/bin/env python3
"""gl_texbind.c's shadow never disagrees with GL (GitHub #338).

tools/test_texture_bind_routing.py proves nothing binds a texture behind
gl_texbind.c's back.  This proves the other half: that what the module
does itself keeps its shadow true.  It compiles the real
engine/h2shared/gl_texbind.c against a fake GL that records, per unit and
target, what is really bound, which unit is really active, and which names
are alive -- deleting a texture unbinds it everywhere and frees the name
for the next glGenTextures, lowest first, the way drivers recycle them.

Then a seeded run of random operations through the public entry points
(slot, target and array binds, bind-for-edit, delete, gen, reset) checks
after every one:
  * texture unit 0 is active (the header's invariant: 193 glTexParameter
    calls act on "whatever the active unit holds");
  * every bind took effect -- GL really holds the requested texture on
    the requested unit and target.  A shadow gone stale shows up exactly
    here: the bind folds away against a belief GL no longer shares;
  * an edit bind left the texture on unit 0's target, where the
    glTexImage / glTexParameter that follows it acts;
  * a repeated bind issues no GL call (the shadow is actually in use --
    a module that always rebinds would pass the rest and save nothing).
Plus fixed sequences for the cases that motivated the module: an
edit bind between two slot-0 binds of the same texture, deleting a bound
texture and binding the recycled name, a 2D bind not disturbing the
3D binding on the same unit, and a context loss (the fake starts, and
restarts, with a non-zero unit active and every binding cleared behind the
shadow's back -- R_TexBind_Init has to put both right).
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "engine/h2shared/gl_texbind.c"
HDR = ROOT / "engine/h2shared/gl_texbind.h"

QUAKEDEF = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_TEXTURE0			0x84C0
#define GL_TEXTURE_2D			0x0DE1
#define GL_TEXTURE_3D			0x806F
#define GL_TEXTURE_CUBE_MAP		0x8513
#define GL_TEXTURE_CUBE_MAP_ARRAY	0x9009
#define GL_UNUSED_TEXTURE		(~(GLuint)0)
extern int c_texbinds;
void glActiveTexture_fp (GLenum unit);
void glBindTexture_fp (GLenum target, GLuint texture);
void glDeleteTextures_fp (GLsizei n, const GLuint *textures);
void glBindImageTexture_fp (GLuint unit, GLuint texture, GLint level, GLboolean layered,
			    GLint layer, GLenum access, GLenum format);
'''

HARNESS = r'''
#include "quakedef.h"
#include "gl_texbind.h"

#define GL_TEXTURE_2D_MULTISAMPLE	0x9100
#define NU		RT_MAX_TEXTURE_UNITS
#define NAMES		64

static const GLenum targets[] = { GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_2D_MULTISAMPLE,
				  GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_ARRAY };
#define NT	(int)(sizeof(targets) / sizeof(targets[0]))

int c_texbinds;
static GLuint truth[NU][NT], images[8];
static GLenum active = GL_TEXTURE0 + 7;	/* a fresh context promises unit 0; R_TexBind_Init must not rely on it */
static int alive[NAMES + 1];
static long gl_calls;

static int TI (GLenum t)
{
	int i;
	for (i = 0; i < NT; i++)
		if (targets[i] == t)
			return i;
	fprintf (stderr, "unknown target 0x%x\n", t);
	abort ();
}

void glActiveTexture_fp (GLenum unit)
{
	assert (unit >= GL_TEXTURE0 && unit < GL_TEXTURE0 + NU);
	active = unit;
	gl_calls++;
}

void glBindTexture_fp (GLenum target, GLuint texture)
{
	/* GL: binding a deleted (never generated) name is legal and creates
	 * it; the engine never does it, so treat it as a test failure. */
	assert (texture == 0 || (texture <= NAMES && alive[texture]));
	truth[active - GL_TEXTURE0][TI (target)] = texture;
	gl_calls++;
}

void glDeleteTextures_fp (GLsizei n, const GLuint *textures)
{
	GLsizei i;
	int u, t;
	for (i = 0; i < n; i++)
	{
		GLuint name = textures[i];
		if (!name || name > NAMES)
			continue;
		alive[name] = 0;
		for (u = 0; u < NU; u++)
			for (t = 0; t < NT; t++)
				if (truth[u][t] == name)
					truth[u][t] = 0;	/* deleting unbinds everywhere */
	}
	gl_calls++;
}

void glBindImageTexture_fp (GLuint unit, GLuint texture, GLint level, GLboolean layered,
			    GLint layer, GLenum access, GLenum format)
{
	(void)level; (void)layered; (void)layer; (void)access; (void)format;
	assert (unit < 8);
	images[unit] = texture;
	gl_calls++;
}

static GLuint Gen (void)	/* lowest free name, as drivers recycle them */
{
	GLuint n;
	for (n = 1; n <= NAMES; n++)
		if (!alive[n])
		{
			alive[n] = 1;
			return n;
		}
	return 0;
}

static unsigned long seed = 338;
static unsigned Rand (unsigned n)
{
	seed = seed * 6364136223846793005UL + 1442695040888963407UL;
	return (unsigned)((seed >> 33) % n);
}

static GLuint LiveOrZero (void)
{
	int tries;
	for (tries = 0; tries < 8; tries++)
	{
		GLuint n = 1 + Rand (NAMES);
		if (alive[n])
			return n;
	}
	return 0;
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf (stderr, __VA_ARGS__); fputc ('\n', stderr); failures++; } } while (0)

static void Invariant (const char *after, long step)
{
	CHECK (active == GL_TEXTURE0, "step %ld, after %s: unit %u left active, not 0",
	       step, after, active - GL_TEXTURE0);
}

static void Fixed (void)
{
	GLuint a, b, c;
	long before;

	R_TexBind_Init ();
	Invariant ("R_TexBind_Init", 0);

	/* 1. bind for edit between two slot-0 binds of the same texture */
	a = Gen (); b = Gen ();
	R_BindTextureSlot (0, a);
	R_BindTextureForEdit (GL_TEXTURE_2D, b);
	CHECK (truth[0][TI (GL_TEXTURE_2D)] == b, "edit bind did not put the texture on unit 0");
	R_BindTextureSlot (0, a);
	CHECK (truth[0][TI (GL_TEXTURE_2D)] == a,
	       "slot-0 bind after an edit bind folded away: the edit bind left the shadow stale");
	Invariant ("fixed 1", 0);

	/* 2. delete a bound texture, bind the recycled name */
	R_BindTextureSlot (3, b);
	R_DeleteTextures (1, &b);
	c = Gen ();
	CHECK (c == b, "fake GL did not recycle the name (test bug)");
	R_BindTextureSlot (3, c);
	CHECK (truth[3][TI (GL_TEXTURE_2D)] == c,
	       "bind of a recycled name folded away: the delete left the shadow stale");

	/* 3. targets are separate bindings on one unit */
	R_BindTextureTarget (1, GL_TEXTURE_3D, a);
	R_BindTextureSlot (1, c);
	R_BindTextureTarget (1, GL_TEXTURE_3D, a);
	CHECK (truth[1][TI (GL_TEXTURE_3D)] == a && truth[1][TI (GL_TEXTURE_2D)] == c,
	       "a 2D bind disturbed the 3D binding on the same unit (or vice versa)");

	/* 4. the shadow is in use: a repeat bind is free */
	R_BindTextureSlot (5, a);
	before = gl_calls;
	R_BindTextureSlot (5, a);
	R_BindTextures (5, 1, &a);
	CHECK (gl_calls == before, "a repeated bind issued %ld GL calls; the shadow folds nothing",
	       gl_calls - before);
	Invariant ("fixed 4", 0);

	/* 5. image units go straight through */
	R_BindImageTexture (2, a, 0, GL_FALSE, 0, 0, 0);
	CHECK (images[2] == a, "R_BindImageTexture did not bind the image unit");
	Invariant ("fixed 5", 0);

	R_DeleteTextures (1, &a);
	R_DeleteTextures (1, &c);
	R_TexBind_Shutdown ();
}

static void Random (long steps)
{
	long i;
	int k;

	R_TexBind_Init ();
	for (k = 0; k < 8; k++)
		Gen ();

	for (i = 1; i <= steps && failures < 20; i++)
	{
		unsigned op = Rand (100);
		GLuint u = Rand (NU), tex = LiveOrZero ();
		GLenum tg = targets[Rand (NT)];
		const char *what;

		if (op < 30)
		{
			what = "R_BindTextureSlot";
			R_BindTextureSlot (u, tex);
			CHECK (truth[u][TI (GL_TEXTURE_2D)] == tex, "step %ld: %s (%u, %u) did not take: GL has %u",
			       i, what, u, tex, truth[u][TI (GL_TEXTURE_2D)]);
		}
		else if (op < 50)
		{
			what = "R_BindTextureTarget";
			R_BindTextureTarget (u, tg, tex);
			CHECK (truth[u][TI (tg)] == tex, "step %ld: %s (%u, 0x%x, %u) did not take: GL has %u",
			       i, what, u, tg, tex, truth[u][TI (tg)]);
		}
		else if (op < 62)
		{
			GLuint arr[4];
			GLsizei n = 1 + Rand (4), j;
			GLuint first = Rand (NU - n + 1);
			what = "R_BindTextures";
			for (j = 0; j < n; j++)
				arr[j] = LiveOrZero ();
			R_BindTextures (first, n, arr);
			for (j = 0; j < n; j++)
				CHECK (truth[first + j][TI (GL_TEXTURE_2D)] == arr[j],
				       "step %ld: %s slot %u did not take", i, what, first + j);
		}
		else if (op < 80)
		{
			what = "R_BindTextureForEdit";
			R_BindTextureForEdit (tg, tex);
			CHECK (truth[0][TI (tg)] == tex,
			       "step %ld: %s (0x%x, %u) did not leave it on unit 0", i, what, tg, tex);
		}
		else if (op < 88)
		{
			what = "R_DeleteTextures";
			if (tex)
				R_DeleteTextures (1, &tex);
		}
		else if (op < 97)
		{
			what = "glGenTextures";
			Gen ();
		}
		else
		{
			what = "R_ResetTextureBindings";
			R_ResetTextureBindings ();
		}
		Invariant (what, i);
	}
	R_TexBind_Shutdown ();
}

/* vid_restart: a new context with every binding back at 0, while the
 * shadow still remembers the old one.  Init must forget it. */
static void Restart (void)
{
	GLuint a;
	int u, t;

	R_TexBind_Init ();
	a = Gen ();
	R_BindTextureSlot (0, a);
	R_BindTextureSlot (4, a);
	for (u = 0; u < NU; u++)
		for (t = 0; t < NT; t++)
			truth[u][t] = 0;
	active = GL_TEXTURE0 + 3;
	/* No R_TexBind_Shutdown: a lost context gives no orderly shutdown,
	 * and Init alone has to be enough. */
	R_TexBind_Init ();
	Invariant ("R_TexBind_Init after a context loss", 0);
	R_BindTextureSlot (0, a);
	R_BindTextureSlot (4, a);
	CHECK (truth[0][TI (GL_TEXTURE_2D)] == a && truth[4][TI (GL_TEXTURE_2D)] == a,
	       "after a new context, a bind of the texture the old shadow held folded away");
	R_DeleteTextures (1, &a);
}

int main (void)
{
	Fixed ();
	Restart ();
	Random (200000);
	if (failures)
	{
		fprintf (stderr, "FAIL: %d shadow/GL disagreements\n", failures);
		return 1;
	}
	printf ("PASS gl_texbind.c shadow agrees with a fake GL across 200000 random ops "
		"and the edit/delete/target sequences (#338)\n");
	return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "quakedef.h").write_text(QUAKEDEF)
        (tmp / "sdl_inc.h").write_text("")
        (tmp / "gl_texbind.h").write_text(HDR.read_text())
        (tmp / "gl_texbind.c").write_text(SRC.read_text())
        (tmp / "harness.c").write_text(HARNESS)
        exe = tmp / "harness"
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall", "-Werror",
                            "-I", str(tmp), str(tmp / "gl_texbind.c"), str(tmp / "harness.c"),
                            "-o", str(exe)], capture_output=True, text=True)
        if r.returncode:
            sys.exit("FAIL: gl_texbind.c does not build against the fake GL:\n" + r.stderr)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        if r.returncode:
            sys.exit((r.stderr or r.stdout).strip())
        print(r.stdout.strip())


if __name__ == "__main__":
    main()
