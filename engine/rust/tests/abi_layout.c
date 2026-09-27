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

/* The zone structures are private to zone.c and never appear in zone.h, and
 * cache_user_t's block in zone.h is behind `#if !defined(SERVERONLY)` -- which
 * this program is compiled with.  So they are copied here, as mplane_t is
 * above, and the zone port's own differential harness is what proves the copy
 * did not drift: it compares the bytes either implementation leaves in the
 * hunk, which follow from these sizes and offsets. */
typedef struct abi_memblock_s {
	int	size;
	int	tag;
	int	magic;
	int	pad;
	struct abi_memblock_s	*next, *prev;
} abi_memblock_t;

typedef struct abi_memzone_s {
	int		size;
	abi_memblock_t	blocklist;
	abi_memblock_t	*rover;
} abi_memzone_t;

typedef struct abi_zonelist_s {
	int		id, magic;
	const char	*name;
	abi_memzone_t	*zone;
	struct abi_zonelist_s	*next;
} abi_zonelist_t;

typedef struct abi_hunk_s {
	int	sentinal;
	int	size;
	char	name[24];
} abi_hunk_t;

typedef struct abi_cache_user_s {
	void	*data;
} abi_cache_user_t;

typedef struct abi_cache_system_s {
	int			size;
	abi_cache_user_t		*user;
	char			name[32];
	struct abi_cache_system_s	*prev, *next;
	struct abi_cache_system_s	*lru_prev, *lru_next;
} abi_cache_system_t;

/* quakeparms_t from engine/hexen2/host.h -- hexen2/server/host.h and
 * hexenworld/server/host.h carry the same definition.  zone.c reaches the
 * command line through the com_argc/com_argv macros, which are
 * `host_parms->argc` and `host_parms->argv` (common.h:92-93). */
typedef struct abi_quakeparms_s {
	const char	*basedir;
	const char	*userdir;
	int		argc;
	char		**argv;
	void		*membase;
	int		memsize;
	int		errstate;
} abi_quakeparms_t;

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

STRUCT_ACCESSORS(ZonelistC)
RUST_ACCESSOR(ZonelistC_offsetof_id)
RUST_ACCESSOR(ZonelistC_offsetof_magic)
RUST_ACCESSOR(ZonelistC_offsetof_name)
RUST_ACCESSOR(ZonelistC_offsetof_zone)
RUST_ACCESSOR(ZonelistC_offsetof_next)

STRUCT_ACCESSORS(MemBlockC)
RUST_ACCESSOR(MemBlockC_alignof)
RUST_ACCESSOR(MemBlockC_offsetof_size)
RUST_ACCESSOR(MemBlockC_offsetof_tag)
RUST_ACCESSOR(MemBlockC_offsetof_magic)
RUST_ACCESSOR(MemBlockC_offsetof_pad)
RUST_ACCESSOR(MemBlockC_offsetof_next)
RUST_ACCESSOR(MemBlockC_offsetof_prev)

STRUCT_ACCESSORS(MemZoneC)
RUST_ACCESSOR(MemZoneC_offsetof_size)
RUST_ACCESSOR(MemZoneC_offsetof_blocklist)
RUST_ACCESSOR(MemZoneC_offsetof_rover)

STRUCT_ACCESSORS(HunkC)
RUST_ACCESSOR(HunkC_offsetof_sentinal)
RUST_ACCESSOR(HunkC_offsetof_size)
RUST_ACCESSOR(HunkC_offsetof_name)

STRUCT_ACCESSORS(CacheUserC)
RUST_ACCESSOR(CacheUserC_offsetof_data)

STRUCT_ACCESSORS(CacheSystemC)
RUST_ACCESSOR(CacheSystemC_alignof)
RUST_ACCESSOR(CacheSystemC_offsetof_size)
RUST_ACCESSOR(CacheSystemC_offsetof_user)
RUST_ACCESSOR(CacheSystemC_offsetof_name)
RUST_ACCESSOR(CacheSystemC_offsetof_prev)
RUST_ACCESSOR(CacheSystemC_offsetof_next)
RUST_ACCESSOR(CacheSystemC_offsetof_lru_prev)
RUST_ACCESSOR(CacheSystemC_offsetof_lru_next)

STRUCT_ACCESSORS(QuakeParmsC)
RUST_ACCESSOR(QuakeParmsC_offsetof_basedir)
RUST_ACCESSOR(QuakeParmsC_offsetof_userdir)
RUST_ACCESSOR(QuakeParmsC_offsetof_argc)
RUST_ACCESSOR(QuakeParmsC_offsetof_argv)
RUST_ACCESSOR(QuakeParmsC_offsetof_membase)
RUST_ACCESSOR(QuakeParmsC_offsetof_memsize)
RUST_ACCESSOR(QuakeParmsC_offsetof_errstate)

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
byte *FS_LoadZoneFile (const char *path, int zone_id, unsigned int *path_id)
{
	(void)path; (void)zone_id; (void)path_id; abort();
}
sizebuf_t net_message;

/* cvar.rs's own calls.  Same rule as the block above: the engine provides all
 * of these, the archive leaves them undefined, and the layout program never
 * reaches them. */
const char *Cmd_Argv (int arg) { (void)arg; abort(); }
int Cmd_Argc (void) { abort(); }
void Cmd_AddCommand (const char *cmd_name, void (*function)(void))
{
	(void)cmd_name; (void)function; abort();
}
int Cmd_Exists (const char *cmd_name) { (void)cmd_name; abort(); }
int q_strcasecmp (const char *s1, const char *s2) { (void)s1; (void)s2; abort(); }
int q_snprintf (char *str, size_t size, const char *format, ...)
{
	(void)str; (void)size; (void)format; abort();
}

/* zone.rs's own calls.  Same rule again; Z_Malloc, Z_Strdup, Z_Free and
 * Hunk_AllocName are no longer here because the zone port now defines them --
 * the archive exporting them is what removed those four stubs. */
int COM_CheckParm (const char *parm) { (void)parm; abort(); }
abi_quakeparms_t *host_parms;

/* The zone port's four per-target values come from engine/rust/zone_target.c,
 * which each engine target compiles with its own defines; the layout program
 * links the archive without it and never calls Memory_Init, so it stubs them.
 * The zone gate is the place that proves what each target's shim answers. */
int Zone_TargetDefSize (void) { abort(); }
int Zone_TargetSecSize (void) { abort(); }
int Zone_TargetHasCache (void) { abort(); }
int Zone_TargetDedicated (void) { abort(); }

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

	SIZE(abi_zonelist_t, ZonelistC);
	OFF(abi_zonelist_t, id, ZonelistC, id);
	OFF(abi_zonelist_t, magic, ZonelistC, magic);
	OFF(abi_zonelist_t, name, ZonelistC, name);
	OFF(abi_zonelist_t, zone, ZonelistC, zone);
	OFF(abi_zonelist_t, next, ZonelistC, next);

	SIZE(abi_memblock_t, MemBlockC);
	ALIGN(abi_memblock_t, MemBlockC);
	OFF(abi_memblock_t, size, MemBlockC, size);
	OFF(abi_memblock_t, tag, MemBlockC, tag);
	OFF(abi_memblock_t, magic, MemBlockC, magic);
	OFF(abi_memblock_t, pad, MemBlockC, pad);
	OFF(abi_memblock_t, next, MemBlockC, next);
	OFF(abi_memblock_t, prev, MemBlockC, prev);

	SIZE(abi_memzone_t, MemZoneC);
	OFF(abi_memzone_t, size, MemZoneC, size);
	OFF(abi_memzone_t, blocklist, MemZoneC, blocklist);
	OFF(abi_memzone_t, rover, MemZoneC, rover);

	SIZE(abi_hunk_t, HunkC);
	OFF(abi_hunk_t, sentinal, HunkC, sentinal);
	OFF(abi_hunk_t, size, HunkC, size);
	OFF(abi_hunk_t, name, HunkC, name);

	SIZE(abi_cache_user_t, CacheUserC);
	OFF(abi_cache_user_t, data, CacheUserC, data);

	SIZE(abi_cache_system_t, CacheSystemC);
	ALIGN(abi_cache_system_t, CacheSystemC);
	OFF(abi_cache_system_t, size, CacheSystemC, size);
	OFF(abi_cache_system_t, user, CacheSystemC, user);
	OFF(abi_cache_system_t, name, CacheSystemC, name);
	OFF(abi_cache_system_t, prev, CacheSystemC, prev);
	OFF(abi_cache_system_t, next, CacheSystemC, next);
	OFF(abi_cache_system_t, lru_prev, CacheSystemC, lru_prev);
	OFF(abi_cache_system_t, lru_next, CacheSystemC, lru_next);

	SIZE(abi_quakeparms_t, QuakeParmsC);
	OFF(abi_quakeparms_t, basedir, QuakeParmsC, basedir);
	OFF(abi_quakeparms_t, userdir, QuakeParmsC, userdir);
	OFF(abi_quakeparms_t, argc, QuakeParmsC, argc);
	OFF(abi_quakeparms_t, argv, QuakeParmsC, argv);
	OFF(abi_quakeparms_t, membase, QuakeParmsC, membase);
	OFF(abi_quakeparms_t, memsize, QuakeParmsC, memsize);
	OFF(abi_quakeparms_t, errstate, QuakeParmsC, errstate);

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
