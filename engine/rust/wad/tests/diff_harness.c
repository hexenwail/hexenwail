// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for engine/h2shared/wad.c and the Rust wad module.
//
// The C original is compiled exactly as glhexen2 compiles it -- -DGLQUAKE, the
// Hexen II include order -- and every exported name is renamed with a c_ prefix
// so the Rust module can define the unprefixed ABI in the same binary.  The two
// are driven through one table of calls and must agree byte for byte.
//
// Why one C variant is enough here.  wad.c is in COMMON_SOURCES and is removed
// again for h2ded ("gfx.wad lumps are renderer-only data",
// engine/CMakeLists.txt:1308); HWSV_SOURCES never lists it.  glhexen2 is
// therefore the only target that compiles it, and there is no second variant to
// reconcile.  The file has no #ifdefs of its own.
//
// What is covered, against issue #245's list:
//
//   * a wad built inside the harness, byte for byte: the header, the lump table
//     and the lump data, with little-endian fields written explicitly so the
//     file's bytes do not depend on the host;
//   * lump counts of zero and one, including the zero-lump case where
//     wad_lumps points just past the header and every lookup is a miss;
//   * the WAD2 identification check and its fatal path: "WAD3", the byte-swapped
//     "2DAW", and each of the four bytes wrong on its own -- all four reach
//     Sys_Error, and the state left behind (wad_base already repointed, the
//     previous wad_numlumps and wad_lumps still in place, the mapping unedited)
//     is recorded;
//   * the "couldn't load" path, where FS_LoadZoneFile returns NULL and wad_base
//     is left NULL rather than stale;
//   * the endian fields: the header's numlumps and infotableofs, the table's
//     filepos and size (read through LittleLong), disksize/type/compression,
//     which are deliberately NOT converted, and each TYP_QPIC lump's width and
//     height through SwapPic -- all with byte patterns whose byte-swap is a
//     different, absurd value;
//   * the in-place name cleaning: names shorter than 16 bytes (including empty
//     and one character), names already clean, mixed case, a name of exactly 16
//     non-zero bytes (which the C leaves unterminated), and a byte >= 0x80,
//     which is negative as a signed char and must not be lowercased;
//   * lookup of present lumps in every case form, of absent ones, of the empty
//     name, and of a name longer than the 16 bytes the table can hold -- the
//     last one truncates before comparing and misses, while the message still
//     reports the caller's name;
//   * SwapPic called directly, the way gl_draw.c, draw.c and menu.c call it,
//     with the picture's four data bytes checked untouched.
//
// The whole mapping is recorded after every call, not just the return value:
// the C edits the mapped bytes in place (W_CleanupName's "CAUTION: in-place
// editing!!!" and SwapPic's writes), so a port that copied the wad, cleaned
// names into a local buffer, or stopped one byte short of the 16-byte padding
// would otherwise look identical.  Each implementation gets a FRESH copy of the
// same template on every load, so the two see the same input bytes.
//
// One case is deliberately not looked up.  A 16-byte name has no terminator, so
// strcmp over it reads the byte after it; the C's caller-side buffer would be
// uninitialised stack in that comparison and the two implementations have
// different stacks.  That name is still covered -- through the mapping dump,
// which shows whether a terminator was written into the next table entry.
//
// Every case is a two-way comparison driven through one wad_impl_t table, not
// an assertion against a hand-written expectation alone: two implementations
// that are both wrong would still pass a golden-only test.  Goldens are checked
// as well, so a shared mistake is caught too.

#include "quakedef.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------
 * declarations
 *--------------------------------------------------------------------------*/

// The C original, renamed so it can share the binary with the Rust ABI.  The
// three globals are part of it: wad_numlumps, wad_lumps and wad_base are
// exported state, not internal bookkeeping.
#define DECLARE_WAD_IMPL(P) \
	extern void P##W_LoadWadFile(const char *filename); \
	extern lumpinfo_t *P##W_GetLumpinfo(const char *name); \
	extern void *P##W_GetLumpName(const char *name); \
	extern void P##SwapPic(qpic_t *pic); \
	extern int P##wad_numlumps; \
	extern lumpinfo_t *P##wad_lumps; \
	extern byte *P##wad_base

DECLARE_WAD_IMPL(c_);
DECLARE_WAD_IMPL();

// The Rust layout accessors, so the harness can check the structs it compiles
// against without restating Rust's offsets in C.
extern size_t WadInfoC_sizeof(void);
extern size_t WadInfoC_offsetof_identification(void);
extern size_t WadInfoC_offsetof_numlumps(void);
extern size_t WadInfoC_offsetof_infotableofs(void);
extern size_t LumpInfoC_sizeof(void);
extern size_t LumpInfoC_alignof(void);
extern size_t LumpInfoC_offsetof_filepos(void);
extern size_t LumpInfoC_offsetof_disksize(void);
extern size_t LumpInfoC_offsetof_size(void);
extern size_t LumpInfoC_offsetof_type(void);
extern size_t LumpInfoC_offsetof_compression(void);
extern size_t LumpInfoC_offsetof_pad1(void);
extern size_t LumpInfoC_offsetof_pad2(void);
extern size_t LumpInfoC_offsetof_name(void);
extern size_t QPicC_sizeof(void);
extern size_t QPicC_alignof(void);
extern size_t QPicC_offsetof_width(void);
extern size_t QPicC_offsetof_height(void);
extern size_t QPicC_offsetof_data(void);
// The endian helper the port uses in place of the C's LittleLong macro.
extern int Wad_LittleLong(int v);

/*----------------------------------------------------------------------------
 * failures
 *--------------------------------------------------------------------------*/

static int failures;
static int checks;

static void check(int ok, const char *fmt, ...)
{
	va_list ap;

	checks++;
	if (ok)
		return;

	failures++;
	fprintf(stderr, "FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

/*----------------------------------------------------------------------------
 * trace
 *--------------------------------------------------------------------------*/

#define TRACE_MAX (1024 * 1024)

static unsigned char trace[TRACE_MAX];
static size_t trace_len;

static void rec(const void *p, size_t n)
{
	if (trace_len + n > sizeof trace) {
		failures++;
		fprintf(stderr, "FAIL: trace overflow\n");
		return;
	}
	memcpy(trace + trace_len, p, n);
	trace_len += n;
}

static void rec_int(int v) { rec(&v, sizeof v); }

static void rec_str(const char *s) { rec(s, strlen(s) + 1); }

/*----------------------------------------------------------------------------
 * the engine symbols the two implementations call
 *--------------------------------------------------------------------------*/

// One wad at a time is enough: W_LoadWadFile maps exactly one file, and the
// templates below are all small.
#define ZONE_SIZE 4096
#define WAD_MAX 1024

static byte zone[ZONE_SIZE];
static byte wad_tmpl[WAD_MAX];
static int wad_len;

// FS_LoadZoneFile is the only way the wad reaches W_LoadWadFile, so this is
// where the harness controls the input.  Every load gets a fresh copy of the
// current template, so the two implementations always start from the same
// bytes even though both edit the mapping in place.
static int fs_fail;		// return NULL, as a missing gfx.wad would
static int fs_calls;
static char fs_name[64];
static int fs_bad_zone;
static int fs_bad_pathid;

byte *FS_LoadZoneFile(const char *path, int zone_id, unsigned int *path_id)
{
	fs_calls++;
	snprintf(fs_name, sizeof fs_name, "%s", path);
	if (zone_id != Z_SECZONE)
		fs_bad_zone++;
	if (path_id != NULL)
		fs_bad_pathid++;

	if (fs_fail)
		return NULL;

	memset(zone, 0xee, sizeof zone);
	memcpy(zone, wad_tmpl, (size_t)wad_len);
	return zone;
}

// The wad is allocated in the zone, so the only legitimate Z_Free is the one
// W_LoadWadFile performs on the previously mapped wad.  Recording the offset
// catches a port that frees the wrong pointer or forgets to free at all.
static int zf_calls;
static int zf_off;
static int zf_bad;

void Z_Free(void *ptr)
{
	uintptr_t p = (uintptr_t)ptr;
	uintptr_t lo = (uintptr_t)zone;
	uintptr_t hi = lo + sizeof zone;

	zf_calls++;
	if (p >= lo && p < hi)
		zf_off = (int)(p - lo);
	else
		zf_bad++;
}

// Sys_Error is FUNC_NORETURN in the engine; here it records the formatted
// message and unwinds to the guarded call that armed it, so the fatal paths can
// be compared and the run can continue.  A call with nothing armed is a harness
// bug, not a port difference.
static jmp_buf err_env;
static int err_armed;
static int err_calls;
static char err_buf[256];

// W_LoadWadFile and W_GetLumpinfo report themselves through __thisfunc__, which
// is __func__ -- so the renamed C says "c_W_GetLumpinfo: ..." where the build
// that ships says "W_GetLumpinfo: ...".  The rename is a harness artifact, so it
// is removed here and the goldens below are written for the shipping build.
static void normalize_err(void)
{
	if (strncmp(err_buf, "c_", 2) == 0)
		memmove(err_buf, err_buf + 2, strlen(err_buf + 2) + 1);
}

FUNC_NORETURN void Sys_Error(const char *fmt, ...)
{
	va_list ap;

	err_calls++;
	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof err_buf, fmt, ap);
	va_end(ap);
	normalize_err();

	if (!err_armed) {
		fprintf(stderr, "harness bug: Sys_Error outside a guarded call: %s\n",
			err_buf);
		abort();
	}

	longjmp(err_env, 1);
}

/*----------------------------------------------------------------------------
 * the wad templates
 *--------------------------------------------------------------------------*/

typedef struct tmpl_lump_s {
	const char name[16];		// all 16 bytes; shorter names are zero padded
	char type;
	const byte *data;
	int datalen;
	int size;			// the table's `size`, not always datalen
} tmpl_lump_t;

// Pictures whose width and height byte-swap to a different, absurd value, so a
// port that swapped when it should not (or not when it should) cannot pass.
static const byte pic_font[12] = {
	0x10, 0x00, 0x00, 0x00,		// width 16
	0x08, 0x00, 0x00, 0x00,		// height 8
	'F', 'O', 'N', 'T'
};

static const byte pic_menu[12] = {
	0x02, 0x01, 0x00, 0x00,		// width 0x00000102
	0x04, 0x03, 0x00, 0x00,		// height 0x00000304
	1, 2, 3, 4
};

static const byte palette[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

static const tmpl_lump_t lumps_gfx[] = {
	{ "TINYFONT", TYP_QPIC, pic_font, 12, 12 },
	{ "Gfx/Pic1", TYP_QPIC, pic_menu, 12, 12 },
	{ "MiXtEd", TYP_NONE, NULL, 0, 0 },
	{ "PALETTE", TYP_PALETTE, palette, 8, 8 },
};

static const tmpl_lump_t lumps_one[] = {
	{ "GAUNTLET", TYP_QPIC, pic_font, 12, 12 },
};

// The name cases.  "ABCDEFGHIJKLMNOP" is exactly 16 non-zero bytes, which the C
// leaves unterminated; "AB\x11..." has junk after its terminator, which the C
// overwrites with the zero padding -- the one part of the cleanup that a
// zero-padded wad would hide; "\x80""A" puts a byte that is negative as a
// signed char ahead of an upper-case one.
static const tmpl_lump_t lumps_names[] = {
	{ "", TYP_NONE, NULL, 0, 0 },
	{ "a", TYP_NONE, NULL, 0, 0 },
	{ "Z", TYP_NONE, NULL, 0, 0 },
	{ "MiXeDcAsE", TYP_NONE, NULL, 0, 0 },
	{ "abcdefghijklmno", TYP_NONE, NULL, 0, 0 },
	{ "ABCDEFGHIJKLMNOP", TYP_NONE, NULL, 0, 0 },
	{ "\x80""A", TYP_NONE, NULL, 0, 0 },
	{ { 'A', 'B', 0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		(char)0x88, (char)0x99, (char)0xaa, (char)0xbb, (char)0xcc,
		(char)0xdd }, TYP_NONE, NULL, 0, 0 },
};

// A size field whose four bytes are all different, and a disksize that the C
// never converts.
static const tmpl_lump_t lumps_endian[] = {
	{ "BIGVALS", TYP_NONE, palette, 8, 0x01020304 },
};

#define LUMPINFO_SIZE 32
#define WAD_TABLE_OFS 12

enum {
	T_GFX = 0,
	T_ZERO,
	T_ONE,
	T_NAMES,
	T_ENDIAN,
	T_ID_WAD3,
	T_ID_2DAW,
	T_ID_XAD2,
	T_ID_WXD2,
	T_ID_WAX2,
	T_ID_WADX,
};

static void put_le32(byte *p, int v)
{
	p[0] = (byte)v;
	p[1] = (byte)(v >> 8);
	p[2] = (byte)(v >> 16);
	p[3] = (byte)(v >> 24);
}

static void build_wad(const char *id, const tmpl_lump_t *lumps, int n)
{
	int dataofs = WAD_TABLE_OFS + LUMPINFO_SIZE * n;
	int i;

	memset(wad_tmpl, 0, sizeof wad_tmpl);
	memcpy(wad_tmpl, id, 4);
	put_le32(wad_tmpl + 4, n);
	put_le32(wad_tmpl + 8, WAD_TABLE_OFS);

	for (i = 0; i < n; ++i) {
		byte *e = wad_tmpl + WAD_TABLE_OFS + LUMPINFO_SIZE * i;

		put_le32(e + 0, dataofs);		// filepos
		put_le32(e + 4, lumps[i].datalen);	// disksize
		put_le32(e + 8, lumps[i].size);		// size
		e[12] = (byte)lumps[i].type;
		e[13] = CMP_NONE;
		e[14] = 0;
		e[15] = 0;
		memcpy(e + 16, lumps[i].name, 16);

		if (lumps[i].datalen)
			memcpy(wad_tmpl + dataofs, lumps[i].data,
				(size_t)lumps[i].datalen);
		dataofs += lumps[i].datalen;
	}

	wad_len = dataofs;
	if (wad_len > WAD_MAX) {
		fprintf(stderr, "harness bug: template is %d bytes\n", wad_len);
		abort();
	}
}

static void select_wad(int which)
{
	switch (which) {
	case T_GFX:
		build_wad("WAD2", lumps_gfx, 4);
		break;
	case T_ZERO:
		build_wad("WAD2", NULL, 0);
		break;
	case T_ONE:
		build_wad("WAD2", lumps_one, 1);
		break;
	case T_NAMES:
		build_wad("WAD2", lumps_names,
			(int)(sizeof lumps_names / sizeof lumps_names[0]));
		break;
	case T_ENDIAN:
		build_wad("WAD2", lumps_endian, 1);
		break;
	case T_ID_WAD3:
		build_wad("WAD3", lumps_one, 1);
		break;
	case T_ID_2DAW:
		build_wad("2DAW", lumps_one, 1);
		break;
	case T_ID_XAD2:
		build_wad("XAD2", lumps_one, 1);
		break;
	case T_ID_WXD2:
		build_wad("WXD2", lumps_one, 1);
		break;
	case T_ID_WAX2:
		build_wad("WAX2", lumps_one, 1);
		break;
	case T_ID_WADX:
		build_wad("WADX", lumps_one, 1);
		break;
	default:
		fprintf(stderr, "harness bug: unknown template %d\n", which);
		abort();
	}
}

/*----------------------------------------------------------------------------
 * the two implementations
 *--------------------------------------------------------------------------*/

typedef struct wad_impl_s {
	const char *name;
	void (*LoadWadFile)(const char *filename);
	lumpinfo_t *(*GetLumpinfo)(const char *name);
	void *(*GetLumpName)(const char *name);
	void (*SwapPic)(qpic_t *pic);
	int *numlumps;
	lumpinfo_t **lumps;
	byte **base;
} wad_impl_t;

enum { IMPL_C = 0, IMPL_RUST = 1, IMPL_COUNT = 2 };

static const wad_impl_t impls[IMPL_COUNT] = {
	{
		"C",
		c_W_LoadWadFile, c_W_GetLumpinfo, c_W_GetLumpName, c_SwapPic,
		&c_wad_numlumps, &c_wad_lumps, &c_wad_base,
	},
	{
		"Rust",
		W_LoadWadFile, W_GetLumpinfo, W_GetLumpName, SwapPic,
		&wad_numlumps, &wad_lumps, &wad_base,
	},
};

static const wad_impl_t *cur;

/*----------------------------------------------------------------------------
 * state recording
 *--------------------------------------------------------------------------*/

// Every call is followed by this, so the trace carries the whole observable
// state and not just the return value: the mapping as the implementation left
// it, the three exported globals, the diagnostic, and what the implementation
// asked the engine for.
static void rec_state(void)
{
	rec(zone, (size_t)wad_len);
	rec_int(cur->base[0] != NULL);
	rec_int(cur->base[0] ? (int)(cur->base[0] - zone) : -1);
	rec_int(cur->numlumps[0]);
	rec_int(cur->lumps[0] ? (int)((const byte *)cur->lumps[0] - zone) : -1);
	rec_int(err_calls);
	rec_str(err_buf);
	rec_int(fs_calls);
	rec_str(fs_name);
	rec_int(fs_bad_zone);
	rec_int(fs_bad_pathid);
	rec_int(zf_calls);
	rec_int(zf_off);
	rec_int(zf_bad);
}

static void op_reset(void)
{
	err_calls = 0;
	err_buf[0] = 0;
	fs_calls = 0;
	fs_name[0] = 0;
	fs_bad_zone = 0;
	fs_bad_pathid = 0;
	fs_fail = 0;
	zf_calls = 0;
	zf_off = -1;
	zf_bad = 0;
}

/*----------------------------------------------------------------------------
 * operations
 *--------------------------------------------------------------------------*/

// W_LoadWadFile, with the template that FS_LoadZoneFile will hand it.  The
// guarded call is what makes the fatal paths testable: Sys_Error unwinds here
// instead of ending the run.
static void load_wad(const char *what, int which, const char *filename,
		int fail, const char *want_err, int want_frees, int want_free_off)
{
	select_wad(which);
	op_reset();
	fs_fail = fail;

	err_armed = 1;
	if (setjmp(err_env) == 0)
		cur->LoadWadFile(filename);
	err_armed = 0;

	if (want_err) {
		check(err_calls == 1 && strcmp(err_buf, want_err) == 0,
			"%s [%s]: Sys_Error \"%s\", want \"%s\"", what, cur->name,
			err_buf, want_err);
	} else {
		check(err_calls == 0,
			"%s [%s]: unexpected Sys_Error \"%s\"", what, cur->name,
			err_buf);
	}

	check(fs_calls == 1, "%s [%s]: FS_LoadZoneFile called %d time(s), want 1",
		what, cur->name, fs_calls);
	check(strcmp(fs_name, filename) == 0,
		"%s [%s]: FS_LoadZoneFile got \"%s\", want \"%s\"", what, cur->name,
		fs_name, filename);
	check(fs_bad_zone == 0,
		"%s [%s]: FS_LoadZoneFile was not asked for Z_SECZONE", what,
		cur->name);
	check(fs_bad_pathid == 0,
		"%s [%s]: FS_LoadZoneFile path_id was not NULL", what, cur->name);
	check(zf_calls == want_frees,
		"%s [%s]: Z_Free called %d time(s), want %d", what, cur->name,
		zf_calls, want_frees);
	if (want_frees)
		check(zf_off == want_free_off,
			"%s [%s]: Z_Free got offset %d, want %d", what, cur->name,
			zf_off, want_free_off);

	rec_state();
}

static void numlumps_is(const char *what, int want)
{
	check(cur->numlumps[0] == want,
		"%s [%s]: wad_numlumps is %d, want %d", what, cur->name,
		cur->numlumps[0], want);
	rec_int(cur->numlumps[0]);
}

static void lumps_at(const char *what, int want_off)
{
	int off = cur->lumps[0] ? (int)((const byte *)cur->lumps[0] - zone) : -1;

	check(off == want_off, "%s [%s]: wad_lumps is at offset %d, want %d",
		what, cur->name, off, want_off);
	rec_int(off);
}

// W_GetLumpinfo: the returned pointer is an offset into the mapping (the table
// entry, not the lump data), and a miss is a Sys_Error carrying the caller's
// own spelling of the name.
static void lookup(const char *what, const char *name, const char *want_err,
		int want_off)
{
	int off = -1;

	op_reset();
	err_armed = 1;
	if (setjmp(err_env) == 0) {
		lumpinfo_t *p = cur->GetLumpinfo(name);
		off = p ? (int)((const byte *)p - zone) : -1;
	}
	err_armed = 0;

	if (want_err) {
		check(err_calls == 1 && strcmp(err_buf, want_err) == 0,
			"%s [%s]: Sys_Error \"%s\", want \"%s\"", what, cur->name,
			err_buf, want_err);
	} else {
		check(err_calls == 0,
			"%s [%s]: unexpected Sys_Error \"%s\"", what, cur->name,
			err_buf);
		check(off == want_off,
			"%s [%s]: lumpinfo at offset %d, want %d", what, cur->name,
			off, want_off);
	}

	rec_int(off);
	rec_state();
}

// W_GetLumpName: the lump's data, i.e. wad_base + filepos.
static void lumpname(const char *what, const char *name, const char *want_err,
		int want_off)
{
	int off = -1;

	op_reset();
	err_armed = 1;
	if (setjmp(err_env) == 0) {
		void *p = cur->GetLumpName(name);
		off = p ? (int)((const byte *)p - zone) : -1;
	}
	err_armed = 0;

	if (want_err) {
		check(err_calls == 1 && strcmp(err_buf, want_err) == 0,
			"%s [%s]: Sys_Error \"%s\", want \"%s\"", what, cur->name,
			err_buf, want_err);
	} else {
		check(err_calls == 0,
			"%s [%s]: unexpected Sys_Error \"%s\"", what, cur->name,
			err_buf);
		check(off == want_off,
			"%s [%s]: lump data at offset %d, want %d", what, cur->name,
			off, want_off);
	}

	rec_int(off);
	rec_state();
}

// A little-endian read of four bytes, to be compared against the C's own
// LittleLong so the check is right on a big-endian host too.
static int le32_at(const byte *p)
{
	return (int)((unsigned)p[0] | ((unsigned)p[1] << 8)
		| ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}

// SwapPic called directly, on a picture of the harness's own -- the way
// gl_draw.c, draw.c and menu.c call it on data that did not come from a wad.
static void swappic(const char *what, int w, int h)
{
	static byte pic[64];

	op_reset();
	memset(pic, 0x5a, sizeof pic);
	put_le32(pic + 0, w);
	put_le32(pic + 4, h);

	cur->SwapPic((qpic_t *)pic);

	check(le32_at(pic + 0) == (int)LittleLong(w),
		"%s [%s]: width %d (0x%08x), want LittleLong(%d) (0x%08x)", what,
		cur->name, le32_at(pic + 0), (unsigned)le32_at(pic + 0), w,
		(unsigned)LittleLong(w));
	check(le32_at(pic + 4) == (int)LittleLong(h),
		"%s [%s]: height %d (0x%08x), want LittleLong(%d) (0x%08x)", what,
		cur->name, le32_at(pic + 4), (unsigned)le32_at(pic + 4), h,
		(unsigned)LittleLong(h));
	check(pic[8] == 0x5a && pic[11] == 0x5a && pic[12] == 0x5a,
		"%s [%s]: SwapPic wrote past height into the picture data", what,
		cur->name);

	rec_int(err_calls);
	rec(pic, 16);
	rec_state();
}

static void lump_field(const char *what, int index, const char *field,
		int got, int want)
{
	check(got == want, "%s [%s]: lump %d %s = %d (0x%08x), want %d (0x%08x)",
		what, cur->name, index, field, got, (unsigned)got, want,
		(unsigned)want);
	rec_int(got);
}

/*----------------------------------------------------------------------------
 * scenarios
 *--------------------------------------------------------------------------*/

// Zero lumps: the table is empty, wad_lumps still points just past the header,
// and every lookup is a miss -- including the empty name, which is a real key
// in a wad whose names are all zeros.
static void scenario_zero_lumps(void)
{
	load_wad("a wad with no lumps", T_ZERO, "gfx.wad", 0, NULL, 0, 0);
	numlumps_is("no lumps", 0);
	lumps_at("the table starts right after the header", WAD_TABLE_OFS);
	lookup("a name in an empty wad", "tinyfont",
		"W_GetLumpinfo: tinyfont not found", -1);
	lookup("the empty name in an empty wad", "",
		"W_GetLumpinfo:  not found", -1);
	lumpname("no data to point at", "tinyfont",
		"W_GetLumpinfo: tinyfont not found", -1);
}

// One lump, the smallest wad that can be looked up in: the TYP_QPIC picture is
// byte-swapped in place by the load, and the lookup is case-insensitive because
// W_CleanupName lowercases both sides.
static void scenario_one_lump(void)
{
	load_wad("a wad with one lump", T_ONE, "gfx.wad", 0, NULL, 0, 0);
	numlumps_is("one lump", 1);
	lumps_at("the table starts right after the header", WAD_TABLE_OFS);

	lump_field("the lump's filepos was read little-endian", 0, "filepos",
		cur->lumps[0][0].filepos, WAD_TABLE_OFS + LUMPINFO_SIZE);
	lump_field("the lump's size was read little-endian", 0, "size",
		cur->lumps[0][0].size, 12);
	lump_field("disksize is not converted", 0, "disksize",
		cur->lumps[0][0].disksize, 12);
	lump_field("the type is not converted", 0, "type",
		cur->lumps[0][0].type, TYP_QPIC);

	lookup("the lump by its cleaned name", "gauntlet", NULL,
		WAD_TABLE_OFS);
	lookup("the same lump in upper case", "GAUNTLET", NULL, WAD_TABLE_OFS);
	lookup("the same lump in mixed case", "GaUnTlEt", NULL, WAD_TABLE_OFS);
	lookup("a name that is not there", "nope",
		"W_GetLumpinfo: nope not found", -1);
	lookup("a name that starts like it", "gauntle",
		"W_GetLumpinfo: gauntle not found", -1);
	lookup("a name longer than 16 bytes is truncated before comparing",
		"GauntletAndMore", "W_GetLumpinfo: GauntletAndMore not found", -1);
	lumpname("the lump's data", "gauntlet", NULL,
		WAD_TABLE_OFS + LUMPINFO_SIZE);
	lumpname("the lump's data, other case", "GAUNTLET", NULL,
		WAD_TABLE_OFS + LUMPINFO_SIZE);
}

// The name table: empty, one character, already clean, mixed case, 15 bytes,
// exactly 16 bytes, and a byte above 0x7f.  The mapping dump is what shows the
// cleaning, including the 16-byte name that the C leaves unterminated.
static void scenario_names(void)
{
	load_wad("a wad of names", T_NAMES, "gfx.wad", 0, NULL, 0, 0);
	numlumps_is("eight lumps", 8);

	lookup("the empty name", "", NULL, WAD_TABLE_OFS + 0 * LUMPINFO_SIZE);
	lookup("a one-character name", "a", NULL, WAD_TABLE_OFS + 1 * LUMPINFO_SIZE);
	lookup("a one-character name, upper case", "A", NULL,
		WAD_TABLE_OFS + 1 * LUMPINFO_SIZE);
	lookup("a one-character name, the other lump", "Z", NULL,
		WAD_TABLE_OFS + 2 * LUMPINFO_SIZE);
	lookup("a mixed-case name, already cleaned", "mixedcase", NULL,
		WAD_TABLE_OFS + 3 * LUMPINFO_SIZE);
	lookup("a mixed-case name, as written", "MiXeDcAsE", NULL,
		WAD_TABLE_OFS + 3 * LUMPINFO_SIZE);
	lookup("the 15-byte name", "abcdefghijklmno", NULL,
		WAD_TABLE_OFS + 4 * LUMPINFO_SIZE);
	lookup("the 15-byte name, upper case", "ABCDEFGHIJKLMNO", NULL,
		WAD_TABLE_OFS + 4 * LUMPINFO_SIZE);
	// The 16-byte name is deliberately not looked up here: its cleaned form has
	// no terminator, and a caller-side name of 16 bytes or more -- which
	// W_CleanupName also leaves unterminated -- would make the strcmp read past
	// both buffers.  That comparison is covered on a table whose names all
	// terminate, in scenario_pictures and scenario_one_lump.
	lookup("a name with a byte above 0x7f", "\x80""A", NULL,
		WAD_TABLE_OFS + 6 * LUMPINFO_SIZE);
	lookup("a name with junk after its terminator", "AB", NULL,
		WAD_TABLE_OFS + 7 * LUMPINFO_SIZE);
}

// The endian fields, with byte patterns whose byte-swap is a different value.
// `size` is the one table field the C converts besides filepos, and the one the
// harness can set freely; disksize sits next to it and must NOT be converted.
static void scenario_endian(void)
{
	load_wad("a wad with big-endian-looking fields", T_ENDIAN, "gfx.wad", 0,
		NULL, 0, 0);
	numlumps_is("one lump", 1);

	lump_field("filepos is little-endian", 0, "filepos",
		cur->lumps[0][0].filepos, WAD_TABLE_OFS + LUMPINFO_SIZE);
	lump_field("size is little-endian", 0, "size",
		cur->lumps[0][0].size, 0x01020304);
	lump_field("disksize is left alone", 0, "disksize",
		cur->lumps[0][0].disksize, 8);
	lump_field("compression is left alone", 0, "compression",
		cur->lumps[0][0].compression, CMP_NONE);
	lump_field("pad1 is left alone", 0, "pad1", cur->lumps[0][0].pad1, 0);
	lump_field("pad2 is left alone", 0, "pad2", cur->lumps[0][0].pad2, 0);
	lump_field("the type is left alone", 0, "type",
		cur->lumps[0][0].type, TYP_NONE);

	// The helper itself, against the C's own macro.
	check(Wad_LittleLong(0x01020304) == (int)LittleLong(0x01020304),
		"Wad_LittleLong(0x01020304) = 0x%08x, want 0x%08x",
		(unsigned)Wad_LittleLong(0x01020304),
		(unsigned)LittleLong(0x01020304));
	check(Wad_LittleLong(1) == (int)LittleLong(1),
		"Wad_LittleLong(1) = 0x%08x, want 0x%08x",
		(unsigned)Wad_LittleLong(1), (unsigned)LittleLong(1));
	check(Wad_LittleLong(-1) == (int)LittleLong(-1),
		"Wad_LittleLong(-1) = 0x%08x, want 0x%08x",
		(unsigned)Wad_LittleLong(-1), (unsigned)LittleLong(-1));
	check(Wad_LittleLong(0) == (int)LittleLong(0),
		"Wad_LittleLong(0) = 0x%08x, want 0x%08x",
		(unsigned)Wad_LittleLong(0), (unsigned)LittleLong(0));
	check(Wad_LittleLong(0x7f000001) == (int)LittleLong(0x7f000001),
		"Wad_LittleLong(0x7f000001) = 0x%08x, want 0x%08x",
		(unsigned)Wad_LittleLong(0x7f000001),
		(unsigned)LittleLong(0x7f000001));
	check(Wad_LittleLong(-2147483647 - 1) == (int)LittleLong(-2147483647 - 1),
		"Wad_LittleLong(INT_MIN) = 0x%08x, want 0x%08x",
		(unsigned)Wad_LittleLong(-2147483647 - 1),
		(unsigned)LittleLong(-2147483647 - 1));
}

// The pictures in a loaded wad are swapped by the load itself, and the swap is
// visible in the mapping: this is the same bytes SwapPic writes when gl_draw.c
// calls it, reached through W_LoadWadFile's TYP_QPIC branch.
static void scenario_pictures(void)
{
	// Layout of T_GFX: four 32-byte entries from offset 12, data from 140:
	// tinyfont at 140, gfx/pic1 at 152, mixed (no data), palette at 164.
	load_wad("a wad with two pictures", T_GFX, "gfx.wad", 0, NULL, 0, 0);
	numlumps_is("four lumps", 4);

	check(le32_at(zone + 140) == (int)LittleLong(16),
		"[%s]: tinyfont width is 0x%08x, want 16", cur->name,
		(unsigned)le32_at(zone + 140));
	check(le32_at(zone + 144) == (int)LittleLong(8),
		"[%s]: tinyfont height is 0x%08x, want 8", cur->name,
		(unsigned)le32_at(zone + 144));
	check(le32_at(zone + 152) == (int)LittleLong(0x00000102),
		"[%s]: gfx/pic1 width is 0x%08x, want 0x00000102", cur->name,
		(unsigned)le32_at(zone + 152));
	check(le32_at(zone + 156) == (int)LittleLong(0x00000304),
		"[%s]: gfx/pic1 height is 0x%08x, want 0x00000304", cur->name,
		(unsigned)le32_at(zone + 156));
	check(zone[160] == 1 && zone[163] == 4,
		"[%s]: the picture data after the header was rewritten", cur->name);

	// The non-picture lumps are untouched: only TYP_QPIC is swapped.
	check(memcmp(zone + 164, palette, sizeof palette) == 0,
		"[%s]: the palette lump was modified", cur->name);

	lookup("the tinyfont lump", "tinyfont", NULL, WAD_TABLE_OFS);
	lookup("the picture lump, mixed case", "GFX/pIC1", NULL,
		WAD_TABLE_OFS + LUMPINFO_SIZE);
	lookup("the mixed-case lump", "mixted", NULL,
		WAD_TABLE_OFS + 2 * LUMPINFO_SIZE);
	lookup("a name longer than the 16-byte table field", "TINYFONTANDMORE",
		"W_GetLumpinfo: TINYFONTANDMORE not found", -1);
	lookup("the palette lump", "palette", NULL,
		WAD_TABLE_OFS + 3 * LUMPINFO_SIZE);
	lumpname("tinyfont's data", "tinyfont", NULL, 140);
	lumpname("the palette's data", "palette", NULL, 164);
	rec_state();
}

// The identification check and its fatal path.  The mapping is already in place
// when the check runs, and nothing in it has been edited yet -- but wad_base
// has been repointed, so the previous wad is only reachable through the next
// load's Z_Free.
static void scenario_bad_id(void)
{
	load_wad("a wad that is not a wad", T_ID_WAD3, "gfx.wad", 0,
		"Wad file gfx.wad doesn't have WAD2 id\n", 0, 0);
	numlumps_is("wad_numlumps is untouched by the failure", 0);
	lumps_at("wad_lumps is untouched by the failure", -1);

	load_wad("the same id byte-reversed", T_ID_2DAW, "gfx.wad", 0,
		"Wad file gfx.wad doesn't have WAD2 id\n", 1, 0);
	load_wad("first id byte wrong", T_ID_XAD2, "gfx.wad", 0,
		"Wad file gfx.wad doesn't have WAD2 id\n", 1, 0);
	load_wad("second id byte wrong", T_ID_WXD2, "gfx.wad", 0,
		"Wad file gfx.wad doesn't have WAD2 id\n", 1, 0);
	load_wad("third id byte wrong", T_ID_WAX2, "gfx.wad", 0,
		"Wad file gfx.wad doesn't have WAD2 id\n", 1, 0);
	load_wad("fourth id byte wrong", T_ID_WADX, "gfx.wad", 0,
		"Wad file gfx.wad doesn't have WAD2 id\n", 1, 0);
	load_wad("a good wad after six failures", T_GFX, "gfx.wad", 0, NULL, 1, 0);
	numlumps_is("the good wad's lump count", 4);
	lookup("a lump in the good wad", "tinyfont", NULL, WAD_TABLE_OFS);
}

// The other fatal path: the file could not be loaded at all.  wad_base is left
// NULL here, not stale, because the C assigns the return value before testing
// it -- so the next load has nothing to free.
static void scenario_load_failure(void)
{
	load_wad("a missing wad", T_GFX, "NOTHERE.wad", 1,
		"W_LoadWadFile: couldn't load NOTHERE.wad", 0, 0);
	numlumps_is("wad_numlumps is untouched by the failure", 0);
	lumps_at("wad_lumps is untouched by the failure", -1);
	lookup("a lookup with no wad loaded", "tinyfont",
		"W_GetLumpinfo: tinyfont not found", -1);

	load_wad("a good wad after the failure", T_ONE, "gfx.wad", 0, NULL, 0, 0);
	numlumps_is("the good wad's lump count", 1);
}

// Reloading: each successful load frees the previous mapping first, and the new
// wad replaces the old one in the same zone buffer.
static void scenario_reload(void)
{
	load_wad("the first load", T_GFX, "gfx.wad", 0, NULL, 0, 0);
	numlumps_is("four lumps", 4);

	load_wad("the second load", T_ONE, "gfx.wad", 0, NULL, 1, 0);
	numlumps_is("one lump now", 1);
	lookup("a name from the old wad", "tinyfont",
		"W_GetLumpinfo: tinyfont not found", -1);
	lookup("a name from the new wad", "gauntlet", NULL, WAD_TABLE_OFS);

	load_wad("the third load, back to the first wad", T_GFX, "gfx.wad", 0,
		NULL, 1, 0);
	numlumps_is("four lumps again", 4);
	lumpname("the tinyfont data is back", "tinyfont", NULL, 140);
}

// SwapPic on its own, with values whose byte-swap differs: the two header words
// are converted, the picture data is not touched.
static void scenario_swappic(void)
{
	swappic("a picture header", 0x00000102, 0x00000304);
	swappic("a picture header with the high bit set", 0x80000001, 0xffffffff);
	swappic("an already byte-swapped header", 0x02010000, 0x04030000);
	swappic("a zero-sized picture", 0, 0);
	swappic("a one-pixel picture", 1, 1);
}

/*----------------------------------------------------------------------------
 * struct layouts
 *--------------------------------------------------------------------------*/

static void struct_layout(void)
{
	check(WadInfoC_sizeof() == sizeof(wadinfo_t),
		"WadInfoC_sizeof: Rust %zu, C %zu", WadInfoC_sizeof(),
		sizeof(wadinfo_t));
	check(WadInfoC_offsetof_identification() == offsetof(wadinfo_t, identification),
		"offsetof(wadinfo_t.identification): Rust %zu, C %zu",
		WadInfoC_offsetof_identification(),
		offsetof(wadinfo_t, identification));
	check(WadInfoC_offsetof_numlumps() == offsetof(wadinfo_t, numlumps),
		"offsetof(wadinfo_t.numlumps): Rust %zu, C %zu",
		WadInfoC_offsetof_numlumps(), offsetof(wadinfo_t, numlumps));
	check(WadInfoC_offsetof_infotableofs() == offsetof(wadinfo_t, infotableofs),
		"offsetof(wadinfo_t.infotableofs): Rust %zu, C %zu",
		WadInfoC_offsetof_infotableofs(),
		offsetof(wadinfo_t, infotableofs));

	check(LumpInfoC_sizeof() == sizeof(lumpinfo_t),
		"LumpInfoC_sizeof: Rust %zu, C %zu", LumpInfoC_sizeof(),
		sizeof(lumpinfo_t));
	check(LumpInfoC_alignof() == _Alignof(lumpinfo_t),
		"LumpInfoC_alignof: Rust %zu, C %zu", LumpInfoC_alignof(),
		_Alignof(lumpinfo_t));
	check(LumpInfoC_offsetof_filepos() == offsetof(lumpinfo_t, filepos),
		"offsetof(lumpinfo_t.filepos): Rust %zu, C %zu",
		LumpInfoC_offsetof_filepos(), offsetof(lumpinfo_t, filepos));
	check(LumpInfoC_offsetof_disksize() == offsetof(lumpinfo_t, disksize),
		"offsetof(lumpinfo_t.disksize): Rust %zu, C %zu",
		LumpInfoC_offsetof_disksize(), offsetof(lumpinfo_t, disksize));
	check(LumpInfoC_offsetof_size() == offsetof(lumpinfo_t, size),
		"offsetof(lumpinfo_t.size): Rust %zu, C %zu",
		LumpInfoC_offsetof_size(), offsetof(lumpinfo_t, size));
	check(LumpInfoC_offsetof_type() == offsetof(lumpinfo_t, type),
		"offsetof(lumpinfo_t.type): Rust %zu, C %zu",
		LumpInfoC_offsetof_type(), offsetof(lumpinfo_t, type));
	check(LumpInfoC_offsetof_compression() == offsetof(lumpinfo_t, compression),
		"offsetof(lumpinfo_t.compression): Rust %zu, C %zu",
		LumpInfoC_offsetof_compression(),
		offsetof(lumpinfo_t, compression));
	check(LumpInfoC_offsetof_pad1() == offsetof(lumpinfo_t, pad1),
		"offsetof(lumpinfo_t.pad1): Rust %zu, C %zu",
		LumpInfoC_offsetof_pad1(), offsetof(lumpinfo_t, pad1));
	check(LumpInfoC_offsetof_pad2() == offsetof(lumpinfo_t, pad2),
		"offsetof(lumpinfo_t.pad2): Rust %zu, C %zu",
		LumpInfoC_offsetof_pad2(), offsetof(lumpinfo_t, pad2));
	check(LumpInfoC_offsetof_name() == offsetof(lumpinfo_t, name),
		"offsetof(lumpinfo_t.name): Rust %zu, C %zu",
		LumpInfoC_offsetof_name(), offsetof(lumpinfo_t, name));

	check(QPicC_sizeof() == sizeof(qpic_t),
		"QPicC_sizeof: Rust %zu, C %zu", QPicC_sizeof(),
		sizeof(qpic_t));
	check(QPicC_alignof() == _Alignof(qpic_t),
		"QPicC_alignof: Rust %zu, C %zu", QPicC_alignof(),
		_Alignof(qpic_t));
	check(QPicC_offsetof_width() == offsetof(qpic_t, width),
		"offsetof(qpic_t.width): Rust %zu, C %zu",
		QPicC_offsetof_width(), offsetof(qpic_t, width));
	check(QPicC_offsetof_height() == offsetof(qpic_t, height),
		"offsetof(qpic_t.height): Rust %zu, C %zu",
		QPicC_offsetof_height(), offsetof(qpic_t, height));
	check(QPicC_offsetof_data() == offsetof(qpic_t, data),
		"offsetof(qpic_t.data): Rust %zu, C %zu",
		QPicC_offsetof_data(), offsetof(qpic_t, data));

	// The table entry is also the on-disk layout, and the lump table is walked
	// with pointer arithmetic, so the stride is part of the ABI.
	check(sizeof(lumpinfo_t) == LUMPINFO_SIZE,
		"lumpinfo_t is %zu bytes, want %d", sizeof(lumpinfo_t),
		LUMPINFO_SIZE);
}

/*----------------------------------------------------------------------------
 * case runner
 *--------------------------------------------------------------------------*/

// Each case starts from the state the engine starts in: the three globals hold
// exactly what the C's own initialisers give them (wad_numlumps and wad_lumps
// zero, wad_base NULL) and the zone is fresh, so no case inherits the previous
// one's mapping.  Both implementations get the same reset, which is what makes
// the first load in a case always the one with nothing to free.
static void scenario_reset(void)
{
	memset(zone, 0xee, sizeof zone);
	cur->numlumps[0] = 0;
	cur->lumps[0] = NULL;
	cur->base[0] = NULL;
	op_reset();
}

static void run_case(const char *what, void (*scenario)(void))
{
	static unsigned char out[IMPL_COUNT][TRACE_MAX];
	static size_t len[IMPL_COUNT];
	int i;

	for (i = 0; i < IMPL_COUNT; ++i) {
		cur = &impls[i];

		trace_len = 0;
		scenario_reset();
		scenario();

		if (trace_len > sizeof trace) {
			failures++;
			fprintf(stderr, "FAIL: %s [%s]: trace too long\n", what,
				impls[i].name);
			trace_len = sizeof trace;
		}

		len[i] = trace_len;
		memcpy(out[i], trace, trace_len);
	}

	for (i = IMPL_C + 1; i < IMPL_COUNT; ++i) {
		size_t k;

		if (len[i] == len[IMPL_C]
			&& memcmp(out[i], out[IMPL_C], len[IMPL_C]) == 0)
			continue;

		failures++;
		fprintf(stderr, "FAIL: %s: %s and %s produced different traces\n",
			what, impls[IMPL_C].name, impls[i].name);
		for (k = 0; k < len[IMPL_C] && k < len[i]; ++k)
			if (out[IMPL_C][k] != out[i][k])
				break;
		fprintf(stderr, "      first difference at trace byte %zu "
			"(0x%02x vs 0x%02x), lengths %zu vs %zu\n", k,
			k < len[IMPL_C] ? out[IMPL_C][k] : 0,
			k < len[i] ? out[i][k] : 0, len[IMPL_C], len[i]);
	}
}

/*----------------------------------------------------------------------------
 * main
 *--------------------------------------------------------------------------*/

int main(void)
{
	printf("wad differential harness: the C as glhexen2 compiles it (-DGLQUAKE) "
		"and the Rust module\n");

	struct_layout();

	run_case("load: a wad with no lumps", scenario_zero_lumps);
	run_case("load: a wad with one lump", scenario_one_lump);
	run_case("load: the lump names", scenario_names);
	run_case("load: the endian fields", scenario_endian);
	run_case("load: the picture lumps", scenario_pictures);
	run_case("load: the WAD2 identification check", scenario_bad_id);
	run_case("load: the couldn't-load path", scenario_load_failure);
	run_case("load: reloading", scenario_reload);
	run_case("SwapPic", scenario_swappic);

	printf("checked %d expectations, %d failures\n", checks, failures);
	if (failures != 0) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
