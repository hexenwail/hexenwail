/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * abi_layout.c -- every struct the Rust engine archive shares with C, laid
 * out by the C compiler of the target being built, compared field by field
 * with the layout rustc chose for the same target.
 *
 * The per-subsystem differential harnesses already do this, but only on the
 * build host: they fork, install signal handlers and map guard pages, so they
 * cannot run under node.  Rust is linked into every target now, including the
 * ILP32 WebAssembly client, and on wasm32 a pointer is 4 bytes, so a layout
 * that was only ever checked at 8 would be a silent heap corruption rather
 * than a build failure.  This file is deliberately plain C99 with no OS
 * calls, so the one source runs natively and, built with emcc, under node.
 *
 * The Rust side exports <Struct>_sizeof / _alignof / _offsetof_<field>
 * accessors (see the modules under engine/rust/src); this program calls each and
 * compares it with sizeof / offsetof on the engine's own header.  Built by
 * scripts/check-rust-abi.sh, against the HexenWorld include order that the
 * msg_io and info_str harnesses use.
 */

#include "q_stdinc.h"
#include "sizebuf.h"
#include "link_ops.h"
#include "hashindex.h"
struct cvar_s;		/* cvar.h names it in a prototype before defining it */
#include "cvar.h"
#include "protocol.h"		/* hexenworld/shared: the H2W usercmd_t */
#include "wad.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* mplane_t lives in three model headers (gl_model.h, model.h, sv_model.h)
 * that each drag in a renderer or the whole server.  The mathlib harness
 * copies the definition for the same reason.  All three headers carry the
 * same four fields and pad, and gl_model.h warns that the layout is shared
 * with assembly, so it does not drift quietly. */
typedef struct abi_mplane_s {
	vec3_t	normal;
	float	dist;
	byte	type;
	byte	signbits;
	byte	pad[2];
} abi_mplane_t;

#define RUST_ACCESSOR(name) extern size_t name(void);
#define STRUCT_ACCESSORS(S) RUST_ACCESSOR(S##_sizeof)

STRUCT_ACCESSORS(SizeBufC)
RUST_ACCESSOR(SizeBufC_alignof)
RUST_ACCESSOR(SizeBufC_offsetof_allowoverflow)
RUST_ACCESSOR(SizeBufC_offsetof_overflowed)
RUST_ACCESSOR(SizeBufC_offsetof_data)
RUST_ACCESSOR(SizeBufC_offsetof_maxsize)
RUST_ACCESSOR(SizeBufC_offsetof_cursize)
RUST_ACCESSOR(SizeBufC_offsetof_name)

STRUCT_ACCESSORS(LinkC)
RUST_ACCESSOR(LinkC_alignof)
RUST_ACCESSOR(LinkC_offsetof_prev)
RUST_ACCESSOR(LinkC_offsetof_next)

STRUCT_ACCESSORS(HashIndexC)
RUST_ACCESSOR(HashIndexC_offsetof_hash_size)
RUST_ACCESSOR(HashIndexC_offsetof_hash)
RUST_ACCESSOR(HashIndexC_offsetof_index_chain)
RUST_ACCESSOR(HashIndexC_offsetof_hash_mask)

STRUCT_ACCESSORS(MathlibMPlane)
RUST_ACCESSOR(MathlibMPlane_offsetof_normal)
RUST_ACCESSOR(MathlibMPlane_offsetof_dist)
RUST_ACCESSOR(MathlibMPlane_offsetof_type)
RUST_ACCESSOR(MathlibMPlane_offsetof_signbits)

STRUCT_ACCESSORS(CvarC)
RUST_ACCESSOR(CvarC_alignof)
RUST_ACCESSOR(CvarC_offsetof_name)
RUST_ACCESSOR(CvarC_offsetof_string)
RUST_ACCESSOR(CvarC_offsetof_flags)
RUST_ACCESSOR(CvarC_offsetof_value)
RUST_ACCESSOR(CvarC_offsetof_integer)
RUST_ACCESSOR(CvarC_offsetof_callback)
RUST_ACCESSOR(CvarC_offsetof_next)
RUST_ACCESSOR(CvarC_offsetof_default_string)

STRUCT_ACCESSORS(UsercmdC)
RUST_ACCESSOR(UsercmdC_alignof)
RUST_ACCESSOR(UsercmdC_offsetof_msec)
RUST_ACCESSOR(UsercmdC_offsetof_angles)
RUST_ACCESSOR(UsercmdC_offsetof_forwardmove)
RUST_ACCESSOR(UsercmdC_offsetof_sidemove)
RUST_ACCESSOR(UsercmdC_offsetof_upmove)
RUST_ACCESSOR(UsercmdC_offsetof_buttons)
RUST_ACCESSOR(UsercmdC_offsetof_impulse)
RUST_ACCESSOR(UsercmdC_offsetof_light_level)

STRUCT_ACCESSORS(WadInfoC)
RUST_ACCESSOR(WadInfoC_offsetof_identification)
RUST_ACCESSOR(WadInfoC_offsetof_numlumps)
RUST_ACCESSOR(WadInfoC_offsetof_infotableofs)

STRUCT_ACCESSORS(LumpInfoC)
RUST_ACCESSOR(LumpInfoC_alignof)
RUST_ACCESSOR(LumpInfoC_offsetof_filepos)
RUST_ACCESSOR(LumpInfoC_offsetof_disksize)
RUST_ACCESSOR(LumpInfoC_offsetof_size)
RUST_ACCESSOR(LumpInfoC_offsetof_type)
RUST_ACCESSOR(LumpInfoC_offsetof_compression)
RUST_ACCESSOR(LumpInfoC_offsetof_pad1)
RUST_ACCESSOR(LumpInfoC_offsetof_pad2)
RUST_ACCESSOR(LumpInfoC_offsetof_name)

STRUCT_ACCESSORS(QPicC)
RUST_ACCESSOR(QPicC_alignof)
RUST_ACCESSOR(QPicC_offsetof_width)
RUST_ACCESSOR(QPicC_offsetof_height)
RUST_ACCESSOR(QPicC_offsetof_data)

/* The archive's other members reference engine functions and globals.  The
 * linker pulls a member in whole, so the accessors bring those references
 * along; none of them is ever called here.  Defining them keeps this link
 * self-contained on every target instead of relying on a platform-specific
 * "ignore undefined" flag.  The list is exactly what the archive leaves
 * undefined; a new reference fails this link loudly, which is the point. */
void Sys_Error (const char *error, ...) { (void)error; abort(); }
void CON_Printf (unsigned int flags, const char *fmt, ...) { (void)flags; (void)fmt; abort(); }
cvar_t *Cvar_FindVar (const char *var_name) { (void)var_name; abort(); }
void *Hunk_AllocName (int size, const char *name) { (void)size; (void)name; abort(); }
void Z_Free (void *ptr) { (void)ptr; abort(); }
byte *FS_LoadZoneFile (const char *path, int zone_id, unsigned int *path_id)
{
	(void)path; (void)zone_id; (void)path_id; abort();
}
sizebuf_t net_message;

static int checked, failures;

static void check (const char *what, size_t c, size_t rust)
{
	checked++;
	if (c != rust)
	{
		failures++;
		printf("FAIL %-36s C=%zu Rust=%zu\n", what, c, rust);
	}
}

#define SIZE(ctype, S)          check(#S " size", sizeof(ctype), S##_sizeof())
#define ALIGN(ctype, S)         check(#S " align", _Alignof(ctype), S##_alignof())
#define OFF(ctype, cf, S, rf)   check(#S "." #rf, offsetof(ctype, cf), S##_offsetof_##rf())

int main (void)
{
	SIZE(sizebuf_t, SizeBufC);
	ALIGN(sizebuf_t, SizeBufC);
	OFF(sizebuf_t, allowoverflow, SizeBufC, allowoverflow);
	OFF(sizebuf_t, overflowed, SizeBufC, overflowed);
	OFF(sizebuf_t, data, SizeBufC, data);
	OFF(sizebuf_t, maxsize, SizeBufC, maxsize);
	OFF(sizebuf_t, cursize, SizeBufC, cursize);
	OFF(sizebuf_t, name, SizeBufC, name);

	SIZE(link_t, LinkC);
	ALIGN(link_t, LinkC);
	OFF(link_t, prev, LinkC, prev);
	OFF(link_t, next, LinkC, next);

	SIZE(hashindex_t, HashIndexC);
	OFF(hashindex_t, hashSize, HashIndexC, hash_size);
	OFF(hashindex_t, hash, HashIndexC, hash);
	OFF(hashindex_t, indexChain, HashIndexC, index_chain);
	OFF(hashindex_t, hashMask, HashIndexC, hash_mask);

	SIZE(abi_mplane_t, MathlibMPlane);
	OFF(abi_mplane_t, normal, MathlibMPlane, normal);
	OFF(abi_mplane_t, dist, MathlibMPlane, dist);
	OFF(abi_mplane_t, type, MathlibMPlane, type);
	OFF(abi_mplane_t, signbits, MathlibMPlane, signbits);

	SIZE(cvar_t, CvarC);
	ALIGN(cvar_t, CvarC);
	OFF(cvar_t, name, CvarC, name);
	OFF(cvar_t, string, CvarC, string);
	OFF(cvar_t, flags, CvarC, flags);
	OFF(cvar_t, value, CvarC, value);
	OFF(cvar_t, integer, CvarC, integer);
	OFF(cvar_t, callback, CvarC, callback);
	OFF(cvar_t, next, CvarC, next);
	OFF(cvar_t, default_string, CvarC, default_string);

	SIZE(usercmd_t, UsercmdC);
	ALIGN(usercmd_t, UsercmdC);
	OFF(usercmd_t, msec, UsercmdC, msec);
	OFF(usercmd_t, angles, UsercmdC, angles);
	OFF(usercmd_t, forwardmove, UsercmdC, forwardmove);
	OFF(usercmd_t, sidemove, UsercmdC, sidemove);
	OFF(usercmd_t, upmove, UsercmdC, upmove);
	OFF(usercmd_t, buttons, UsercmdC, buttons);
	OFF(usercmd_t, impulse, UsercmdC, impulse);
	OFF(usercmd_t, light_level, UsercmdC, light_level);

	SIZE(wadinfo_t, WadInfoC);
	OFF(wadinfo_t, identification, WadInfoC, identification);
	OFF(wadinfo_t, numlumps, WadInfoC, numlumps);
	OFF(wadinfo_t, infotableofs, WadInfoC, infotableofs);

	SIZE(lumpinfo_t, LumpInfoC);
	ALIGN(lumpinfo_t, LumpInfoC);
	OFF(lumpinfo_t, filepos, LumpInfoC, filepos);
	OFF(lumpinfo_t, disksize, LumpInfoC, disksize);
	OFF(lumpinfo_t, size, LumpInfoC, size);
	OFF(lumpinfo_t, type, LumpInfoC, type);
	OFF(lumpinfo_t, compression, LumpInfoC, compression);
	OFF(lumpinfo_t, pad1, LumpInfoC, pad1);
	OFF(lumpinfo_t, pad2, LumpInfoC, pad2);
	OFF(lumpinfo_t, name, LumpInfoC, name);

	SIZE(qpic_t, QPicC);
	ALIGN(qpic_t, QPicC);
	OFF(qpic_t, width, QPicC, width);
	OFF(qpic_t, height, QPicC, height);
	OFF(qpic_t, data, QPicC, data);

	printf("abi_layout: %d fields checked, %d mismatches (sizeof(void *) = %zu)\n",
		checked, failures, sizeof(void *));
	if (failures)
	{
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
