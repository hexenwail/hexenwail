// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for engine/h2shared/msg_io.c and the Rust msg_io
// module.
//
// msg_io.c is the one engine source compiled with different code per target:
// hwsv compiles it with -DH2W and glhexen2/h2ded without, and the H2W build is
// the only one that defines MSG_WriteAngle16, MSG_WriteUsercmd,
// MSG_ReadStringLine, MSG_ReadAngle16 and MSG_ReadUsercmd.  The consolidated
// Rust static library is built once and linked into all three, so it defines
// the union.  This harness is the evidence for that claim: it compiles the C
// original twice, as c_* without H2W and as w_* with it, links both against
// the single Rust union, and requires the implementations to agree byte for
// byte.  The c_* object has no MSG_WriteAngle16 to point at, so the five
// H2W-only cases below are compared between w_* and Rust only -- which is
// exactly what "the Hexen II targets never call them" means in linkable form.
//
// Every case is a real three-way comparison driven through one msg_impl_t
// table, not an assertion against a hand-written expectation alone: two
// implementations that are both wrong would still pass a golden-only test.
// The golden packets are checked as well, so a shared mistake is caught too.
//
// What is covered, against issue #227's list:
//
//   * a golden packet for every primitive, plus the signed/unsigned
//     boundaries of each (MSG_WriteChar -128..255, MSG_WriteShort
//     -32769..65536, MSG_WriteLong INT_MIN..INT_MAX);
//   * NaN and Inf bit patterns, including a signalling NaN and a denormal,
//     through MSG_WriteFloat/MSG_ReadFloat (exact bits, no conversion);
//   * truncation: every fixed-width reader at cursize one byte short, plus the
//     one reader that has no bounds check at all (MSG_ReadFloat) and the
//     2047-byte cap on MSG_ReadString;
//   * bad-read state: the -1 return, the unchanged read cursor, the sticky
//     flag, and the rewind cl_hw.c performs by assigning msg_readcount;
//   * strings without terminators, empty strings, and strings whose bytes are
//     above 0x7f;
//   * coordinate and angle quantization at the rounding and wraparound edges;
//   * the exact usercmd_s layout, checked against the real HexenWorld struct,
//     with the short and long message variants written and read back;
//   * deterministic round trips through the opposite implementation.
//
// ONE DELIBERATE DIVERGENCE, pinned below rather than hidden.  The quantizing
// writers convert float to int, and C leaves that conversion undefined when
// the value is out of range (C11 6.3.1.4): x86-64 answers with INT_MIN,
// AArch64 saturates.  Rust's `as` saturates, so the port matches AArch64
// exactly and matches x86-64 for every in-range input.  NaN and -Inf agree on
// both (INT_MIN and 0 both truncate to the same bytes), so they are compared
// normally; +Inf and large positive coordinates are the only inputs that
// differ, and no engine caller can produce one.  quantizer_boundary() states
// the boundary and pins the Rust answer for those two.

#include "q_stdinc.h"
#include "sizebuf.h"
#include "protocol.h"		/* hexenworld/shared: the H2W usercmd_t */

#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------
 * declarations
 *--------------------------------------------------------------------------*/

// The C original, twice: c_* is the Hexen II build (no H2W), w_* the
// HexenWorld build.  Both are the same source file.
#define DECLARE_MSG_IMPL(P) \
	extern void P##MSG_WriteChar(sizebuf_t *, int); \
	extern void P##MSG_WriteByte(sizebuf_t *, int); \
	extern void P##MSG_WriteShort(sizebuf_t *, int); \
	extern void P##MSG_WriteLong(sizebuf_t *, int); \
	extern void P##MSG_WriteFloat(sizebuf_t *, float); \
	extern void P##MSG_WriteString(sizebuf_t *, const char *); \
	extern void P##MSG_WriteCoord(sizebuf_t *, float); \
	extern void P##MSG_WriteAngle(sizebuf_t *, float); \
	extern void P##MSG_BeginReading(void); \
	extern void P##MSG_BeginReadingFrom(sizebuf_t *); \
	extern int P##MSG_ReadChar(void); \
	extern int P##MSG_ReadByte(void); \
	extern int P##MSG_ReadShort(void); \
	extern int P##MSG_ReadLong(void); \
	extern float P##MSG_ReadFloat(void); \
	extern const char *P##MSG_ReadString(void); \
	extern float P##MSG_ReadCoord(void); \
	extern float P##MSG_ReadAngle(void); \
	extern int P##msg_readcount; \
	extern int P##msg_badread

// The five functions that exist only in the -DH2W build.
#define DECLARE_MSG_H2W_ONLY(P) \
	extern void P##MSG_WriteAngle16(sizebuf_t *, float); \
	extern void P##MSG_WriteUsercmd(sizebuf_t *, const usercmd_t *, qboolean); \
	extern const char *P##MSG_ReadStringLine(void); \
	extern float P##MSG_ReadAngle16(void); \
	extern void P##MSG_ReadUsercmd(usercmd_t *, qboolean)

DECLARE_MSG_IMPL(c_);
DECLARE_MSG_IMPL(w_);
DECLARE_MSG_IMPL();
DECLARE_MSG_H2W_ONLY(w_);
DECLARE_MSG_H2W_ONLY();

// The Rust usercmd layout accessors, so the harness can check the struct it
// compiles against without restating Rust's offsets in C.
extern size_t UsercmdC_sizeof(void);
extern size_t UsercmdC_alignof(void);
extern size_t UsercmdC_offsetof_msec(void);
extern size_t UsercmdC_offsetof_angles(void);
extern size_t UsercmdC_offsetof_forwardmove(void);
extern size_t UsercmdC_offsetof_sidemove(void);
extern size_t UsercmdC_offsetof_upmove(void);
extern size_t UsercmdC_offsetof_buttons(void);
extern size_t UsercmdC_offsetof_impulse(void);
extern size_t UsercmdC_offsetof_light_level(void);
extern size_t SizeBufC_sizeof(void);

// Engine callbacks the consolidated archive references.  msg_io needs none of
// them; they exist so the link succeeds whichever codegen unit the linker
// pulls, and must never be reached.
void Sys_Error(const char *fmt, ...) { (void)fmt; abort(); }
void CON_Printf(unsigned int flags, const char *fmt, ...) { (void)flags; (void)fmt; abort(); }
void *Hunk_AllocName(int size, const char *name) { (void)size; (void)name; abort(); }

// The buffer the readers default to.  hexen2/net_main.c defines it for the
// Hexen II targets and hexenworld/shared/net_udp.c for hwsv; here it is the
// one buffer all three implementations read through.
sizebuf_t net_message;

/*----------------------------------------------------------------------------
 * the three implementations
 *--------------------------------------------------------------------------*/

typedef struct msg_impl_s {
	const char *name;
	int has_h2w;			// the -DH2W build defines the five extras
	void (*WriteChar)(sizebuf_t *, int);
	void (*WriteByte)(sizebuf_t *, int);
	void (*WriteShort)(sizebuf_t *, int);
	void (*WriteLong)(sizebuf_t *, int);
	void (*WriteFloat)(sizebuf_t *, float);
	void (*WriteString)(sizebuf_t *, const char *);
	void (*WriteCoord)(sizebuf_t *, float);
	void (*WriteAngle)(sizebuf_t *, float);
	void (*WriteAngle16)(sizebuf_t *, float);
	void (*WriteUsercmd)(sizebuf_t *, const usercmd_t *, qboolean);
	void (*BeginReading)(void);
	void (*BeginReadingFrom)(sizebuf_t *);
	int (*ReadChar)(void);
	int (*ReadByte)(void);
	int (*ReadShort)(void);
	int (*ReadLong)(void);
	float (*ReadFloat)(void);
	const char *(*ReadString)(void);
	const char *(*ReadStringLine)(void);
	float (*ReadCoord)(void);
	float (*ReadAngle)(void);
	float (*ReadAngle16)(void);
	void (*ReadUsercmd)(usercmd_t *, qboolean);
	int *readcount;
	int *badread;
} msg_impl_t;

enum { IMPL_C_H2 = 0, IMPL_C_H2W = 1, IMPL_RUST = 2, IMPL_COUNT = 3 };

// The C-H2 entry's five NULLs are the point of the whole harness: the object
// compiled without H2W has no MSG_WriteAngle16 to point at, so a table that
// referenced one would not link.  Every case that needs those functions asks
// for has_h2w and is therefore compared between C-H2W and Rust only.
static const msg_impl_t impls[IMPL_COUNT] = {
	{
		"C-H2", 0,
		c_MSG_WriteChar, c_MSG_WriteByte, c_MSG_WriteShort, c_MSG_WriteLong,
		c_MSG_WriteFloat, c_MSG_WriteString, c_MSG_WriteCoord, c_MSG_WriteAngle,
		NULL, NULL,
		c_MSG_BeginReading, c_MSG_BeginReadingFrom,
		c_MSG_ReadChar, c_MSG_ReadByte, c_MSG_ReadShort, c_MSG_ReadLong,
		c_MSG_ReadFloat, c_MSG_ReadString, NULL,
		c_MSG_ReadCoord, c_MSG_ReadAngle, NULL, NULL,
		&c_msg_readcount, &c_msg_badread
	}, {
		"C-H2W", 1,
		w_MSG_WriteChar, w_MSG_WriteByte, w_MSG_WriteShort, w_MSG_WriteLong,
		w_MSG_WriteFloat, w_MSG_WriteString, w_MSG_WriteCoord, w_MSG_WriteAngle,
		w_MSG_WriteAngle16, w_MSG_WriteUsercmd,
		w_MSG_BeginReading, w_MSG_BeginReadingFrom,
		w_MSG_ReadChar, w_MSG_ReadByte, w_MSG_ReadShort, w_MSG_ReadLong,
		w_MSG_ReadFloat, w_MSG_ReadString, w_MSG_ReadStringLine,
		w_MSG_ReadCoord, w_MSG_ReadAngle, w_MSG_ReadAngle16, w_MSG_ReadUsercmd,
		&w_msg_readcount, &w_msg_badread
	}, {
		"Rust", 1,
		MSG_WriteChar, MSG_WriteByte, MSG_WriteShort, MSG_WriteLong,
		MSG_WriteFloat, MSG_WriteString, MSG_WriteCoord, MSG_WriteAngle,
		MSG_WriteAngle16, MSG_WriteUsercmd,
		MSG_BeginReading, MSG_BeginReadingFrom,
		MSG_ReadChar, MSG_ReadByte, MSG_ReadShort, MSG_ReadLong,
		MSG_ReadFloat, MSG_ReadString, MSG_ReadStringLine,
		MSG_ReadCoord, MSG_ReadAngle, MSG_ReadAngle16, MSG_ReadUsercmd,
		&msg_readcount, &msg_badread
	},
};

static const msg_impl_t *cur;

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
	fputs("FAIL: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*----------------------------------------------------------------------------
 * write cases: all three write into their own buffer, then the images are
 * compared byte for byte against each other and against a golden packet
 *--------------------------------------------------------------------------*/

enum { SB_SIZE = 8192 };

static unsigned char wbuf[SB_SIZE];
static sizebuf_t wsb;

static int g_int;
static float g_float;
static const char *g_str;
static const usercmd_t *g_cmd;
static qboolean g_long;

static void sb_reset(void)
{
	memset(wbuf, 0xcc, sizeof wbuf);
	wsb.allowoverflow = 0;
	wsb.overflowed = 0;
	wsb.data = wbuf;
	wsb.maxsize = (int)sizeof wbuf;
	wsb.cursize = 0;
	wsb.name = NULL;
}

static unsigned char outs[IMPL_COUNT][SB_SIZE];
static int out_len[IMPL_COUNT];

typedef void (*emit_fn)(void);

static void emit_char(void) { cur->WriteChar(&wsb, g_int); }
static void emit_byte(void) { cur->WriteByte(&wsb, g_int); }
static void emit_short(void) { cur->WriteShort(&wsb, g_int); }
static void emit_long(void) { cur->WriteLong(&wsb, g_int); }
static void emit_float(void) { cur->WriteFloat(&wsb, g_float); }
static void emit_string(void) { cur->WriteString(&wsb, g_str); }
static void emit_coord(void) { cur->WriteCoord(&wsb, g_float); }
static void emit_angle(void) { cur->WriteAngle(&wsb, g_float); }
static void emit_angle16(void) { cur->WriteAngle16(&wsb, g_float); }
static void emit_usercmd(void) { cur->WriteUsercmd(&wsb, g_cmd, g_long); }

// Runs emit on every implementation that has it and compares the results.
// `golden` may be NULL for the cases whose expected bytes are already pinned
// by another case of the same primitive.
static void write_case(const char *what, emit_fn emit,
	const unsigned char *golden, int golden_len, int needs_h2w)
{
	int i, k, first = needs_h2w ? IMPL_C_H2W : IMPL_C_H2;

	for (i = 0; i < IMPL_COUNT; ++i) {
		out_len[i] = -1;
		if (needs_h2w && !impls[i].has_h2w)
			continue;
		cur = &impls[i];
		sb_reset();
		emit();
		out_len[i] = wsb.cursize;
		memcpy(outs[i], wbuf, sizeof wbuf);
	}

	for (i = first + 1; i < IMPL_COUNT; ++i) {
		if (out_len[i] < 0)
			continue;
		if (out_len[i] != out_len[first]
			|| memcmp(outs[i], outs[first], sizeof wbuf) != 0) {
			for (k = 0; k < SB_SIZE; ++k) {
				if (outs[i][k] != outs[first][k])
					break;
			}
			failures++;
			fprintf(stderr, "FAIL: %s: %s and %s disagree (len %d vs %d,"
				" first byte %d: 0x%02x vs 0x%02x)\n", what,
				impls[first].name, impls[i].name, out_len[first],
				out_len[i], k, outs[first][k], outs[i][k]);
		}
	}

	if (golden != NULL) {
		if (out_len[first] != golden_len
			|| memcmp(outs[first], golden, (size_t)golden_len) != 0) {
			failures++;
			fprintf(stderr, "FAIL: %s: golden packet mismatch (got %d bytes"
				" [", what, out_len[first]);
			for (k = 0; k < out_len[first] && k < 16; ++k)
				fprintf(stderr, "%s0x%02x", k ? " " : "", outs[first][k]);
			fprintf(stderr, "], want %d [", golden_len);
			for (k = 0; k < golden_len && k < 16; ++k)
				fprintf(stderr, "%s0x%02x", k ? " " : "", golden[k]);
			fprintf(stderr, "])\n");
		} else {
			checks++;
		}
	}
}

static float fbits(unsigned int bits)
{
	union { unsigned int u; float f; } v;

	v.u = bits;
	return v.f;
}

static void write_cases(void)
{
	static const unsigned char gc_127[] = { 0x7f };
	static const unsigned char gc_m128[] = { 0x80 };
	static const unsigned char gc_m129[] = { 0x7f };
	static const unsigned char gc_m1[] = { 0xff };
	static const unsigned char gc_255[] = { 0xff };
	static const unsigned char gc_256[] = { 0x00 };
	static const unsigned char gc_zero[] = { 0x00 };
	static const unsigned char gs_1234[] = { 0x34, 0x12 };
	static const unsigned char gs_5678[] = { 0x78, 0x56 };
	static const unsigned char gs_7fff[] = { 0xff, 0x7f };
	static const unsigned char gs_8000[] = { 0x00, 0x80 };
	static const unsigned char gs_ffff[] = { 0xff, 0xff };
	static const unsigned char gs_m32769[] = { 0xff, 0x7f };
	static const unsigned char gl_12345678[] = { 0x78, 0x56, 0x34, 0x12 };
	static const unsigned char gl_m1[] = { 0xff, 0xff, 0xff, 0xff };
	static const unsigned char gl_min[] = { 0x00, 0x00, 0x00, 0x80 };
	static const unsigned char gl_max[] = { 0xff, 0xff, 0xff, 0x7f };
	static const unsigned char gl_zero[] = { 0x00, 0x00, 0x00, 0x00 };
	static const unsigned char gf_1[] = { 0x00, 0x00, 0x80, 0x3f };
	static const unsigned char gf_p0[] = { 0x00, 0x00, 0x00, 0x00 };
	static const unsigned char gf_m0[] = { 0x00, 0x00, 0x00, 0x80 };
	static const unsigned char gf_pi[] = { 0xdb, 0x0f, 0x49, 0x40 };
	static const unsigned char gf_nan[] = { 0x00, 0x00, 0xc0, 0x7f };
	static const unsigned char gf_mnan[] = { 0x00, 0x00, 0xc0, 0xff };
	static const unsigned char gf_pinf[] = { 0x00, 0x00, 0x80, 0x7f };
	static const unsigned char gf_ninf[] = { 0x00, 0x00, 0x80, 0xff };
	static const unsigned char gf_snan[] = { 0x01, 0x00, 0x80, 0x7f };
	static const unsigned char gf_denorm[] = { 0x01, 0x00, 0x00, 0x00 };
	static const unsigned char gst_empty[] = { 0x00 };
	static const unsigned char gst_a[] = { 0x61, 0x00 };
	static const unsigned char gst_abc[] = { 0x61, 0x62, 0x63, 0x00 };
	static const unsigned char gst_high[] = { 0xff, 0x80, 0x00 };
	static const unsigned char gco_zero[] = { 0x00, 0x00 };
	static const unsigned char gco_1[] = { 0x08, 0x00 };
	static const unsigned char gco_m1[] = { 0xf8, 0xff };
	static const unsigned char gco_half[] = { 0x04, 0x00 };
	static const unsigned char gco_mhalf[] = { 0xfc, 0xff };
	static const unsigned char gco_min[] = { 0x01, 0x00 };
	static const unsigned char gco_mmin[] = { 0xff, 0xff };
	static const unsigned char gco_32767[] = { 0xff, 0x7f };
	static const unsigned char gco_m32767[] = { 0x01, 0x80 };
	static const unsigned char gco_wrap[] = { 0x00, 0x80 };
	static const unsigned char gco_mwrap[] = { 0x00, 0x80 };
	static const unsigned char gan_45[] = { 0x20 };
	static const unsigned char gan_90[] = { 0x40 };
	static const unsigned char gan_180[] = { 0x80 };
	static const unsigned char gan_360[] = { 0x00 };
	static const unsigned char gan_m45[] = { 0xe0 };
	static const unsigned char gan_1[] = { 0x01 };
	static const unsigned char ga16_90[] = { 0x00, 0x40 };
	static const unsigned char ga16_180[] = { 0x00, 0x80 };
	static const unsigned char ga16_360[] = { 0x00, 0x00 };
	static const unsigned char ga16_m90[] = { 0x00, 0xc0 };
	static const unsigned char ga16_45[] = { 0x00, 0x20 };
	static const unsigned char ga16_1[] = { 0xb6, 0x00 };
	static const char long_str[] = "0123456789abcdefghijklmnopqrstuvwxyz";

	// MSG_WriteChar: the signed and unsigned boundaries, and what a value
	// outside both does (the C stores a byte, so the result is a truncation
	// rather than a range error -- see the PARANOID note in msg_io.rs).
	g_int = 0; write_case("MSG_WriteChar(0)", emit_char, gc_zero, 1, 0);
	g_int = 127; write_case("MSG_WriteChar(127)", emit_char, gc_127, 1, 0);
	g_int = -128; write_case("MSG_WriteChar(-128)", emit_char, gc_m128, 1, 0);
	g_int = -1; write_case("MSG_WriteChar(-1)", emit_char, gc_m1, 1, 0);
	g_int = 255; write_case("MSG_WriteChar(255)", emit_char, gc_255, 1, 0);
	g_int = -129; write_case("MSG_WriteChar(-129)", emit_char, gc_m129, 1, 0);
	g_int = 256; write_case("MSG_WriteChar(256)", emit_char, gc_256, 1, 0);

	g_int = 0; write_case("MSG_WriteByte(0)", emit_byte, gc_zero, 1, 0);
	g_int = 255; write_case("MSG_WriteByte(255)", emit_byte, gc_255, 1, 0);
	g_int = -1; write_case("MSG_WriteByte(-1)", emit_byte, gc_m1, 1, 0);
	g_int = 256; write_case("MSG_WriteByte(256)", emit_byte, gc_256, 1, 0);
	g_int = -128; write_case("MSG_WriteByte(-128)", emit_byte, gc_m128, 1, 0);

	g_int = 0x1234; write_case("MSG_WriteShort(0x1234)", emit_short, gs_1234, 2, 0);
	g_int = 32767; write_case("MSG_WriteShort(32767)", emit_short, gs_7fff, 2, 0);
	g_int = -32768; write_case("MSG_WriteShort(-32768)", emit_short, gs_8000, 2, 0);
	g_int = -1; write_case("MSG_WriteShort(-1)", emit_short, gs_ffff, 2, 0);
	g_int = 65535; write_case("MSG_WriteShort(65535)", emit_short, gs_ffff, 2, 0);
	g_int = -32769; write_case("MSG_WriteShort(-32769)", emit_short, gs_m32769, 2, 0);
	g_int = 0x12345678; write_case("MSG_WriteShort(0x12345678)", emit_short, gs_5678, 2, 0);

	g_int = 0x12345678; write_case("MSG_WriteLong(0x12345678)", emit_long, gl_12345678, 4, 0);
	g_int = -1; write_case("MSG_WriteLong(-1)", emit_long, gl_m1, 4, 0);
	g_int = (int)0x80000000; write_case("MSG_WriteLong(INT_MIN)", emit_long, gl_min, 4, 0);
	g_int = 0x7fffffff; write_case("MSG_WriteLong(INT_MAX)", emit_long, gl_max, 4, 0);
	g_int = 0; write_case("MSG_WriteLong(0)", emit_long, gl_zero, 4, 0);

	// MSG_WriteFloat is a bit copy: every one of these must survive as the
	// exact little-endian bit pattern, NaN payloads included.
	g_float = 1.0f; write_case("MSG_WriteFloat(1.0)", emit_float, gf_1, 4, 0);
	g_float = 0.0f; write_case("MSG_WriteFloat(0.0)", emit_float, gf_p0, 4, 0);
	g_float = -0.0f; write_case("MSG_WriteFloat(-0.0)", emit_float, gf_m0, 4, 0);
	g_float = fbits(0x40490fdb); write_case("MSG_WriteFloat(pi)", emit_float, gf_pi, 4, 0);
	g_float = fbits(0x7fc00000); write_case("MSG_WriteFloat(qNaN)", emit_float, gf_nan, 4, 0);
	g_float = fbits(0xffc00000); write_case("MSG_WriteFloat(-qNaN)", emit_float, gf_mnan, 4, 0);
	g_float = fbits(0x7f800001); write_case("MSG_WriteFloat(sNaN)", emit_float, gf_snan, 4, 0);
	g_float = fbits(0x7f800000); write_case("MSG_WriteFloat(+Inf)", emit_float, gf_pinf, 4, 0);
	g_float = fbits(0xff800000); write_case("MSG_WriteFloat(-Inf)", emit_float, gf_ninf, 4, 0);
	g_float = fbits(0x00000001); write_case("MSG_WriteFloat(denormal)", emit_float, gf_denorm, 4, 0);

	g_str = NULL; write_case("MSG_WriteString(NULL)", emit_string, gst_empty, 1, 0);
	g_str = ""; write_case("MSG_WriteString(\"\")", emit_string, gst_empty, 1, 0);
	g_str = "a"; write_case("MSG_WriteString(\"a\")", emit_string, gst_a, 2, 0);
	g_str = "abc"; write_case("MSG_WriteString(\"abc\")", emit_string, gst_abc, 4, 0);
	g_str = "\xff\x80"; write_case("MSG_WriteString(high bytes)", emit_string, gst_high, 3, 0);
	g_str = long_str; write_case("MSG_WriteString(36 chars)", emit_string, NULL, 0, 0);
	// 2100 characters: MSG_WriteString has no cap, so all 2101 bytes go out.
	g_str = NULL;
	{
		static char big[2101];
		memset(big, 'A', 2100);
		big[2100] = '\0';
		g_str = big;
		write_case("MSG_WriteString(2100 chars)", emit_string, NULL, 0, 0);
		check(out_len[IMPL_C_H2] == 2101,
			"MSG_WriteString(2100 chars) wrote %d bytes, want 2101",
			out_len[IMPL_C_H2]);
	}

	// MSG_WriteCoord: round-to-nearest away from zero on the sign, and the
	// short truncation past +/-4096.
	g_float = 0.0f; write_case("MSG_WriteCoord(0.0)", emit_coord, gco_zero, 2, 0);
	g_float = 1.0f; write_case("MSG_WriteCoord(1.0)", emit_coord, gco_1, 2, 0);
	g_float = -1.0f; write_case("MSG_WriteCoord(-1.0)", emit_coord, gco_m1, 2, 0);
	g_float = 0.5f; write_case("MSG_WriteCoord(0.5)", emit_coord, gco_half, 2, 0);
	g_float = -0.5f; write_case("MSG_WriteCoord(-0.5)", emit_coord, gco_mhalf, 2, 0);
	g_float = 0.0625f; write_case("MSG_WriteCoord(0.0625)", emit_coord, gco_min, 2, 0);
	g_float = -0.0625f; write_case("MSG_WriteCoord(-0.0625)", emit_coord, gco_mmin, 2, 0);
	g_float = 0.0624f; write_case("MSG_WriteCoord(0.0624)", emit_coord, gco_zero, 2, 0);
	g_float = -0.0624f; write_case("MSG_WriteCoord(-0.0624)", emit_coord, gco_zero, 2, 0);
	g_float = 4095.875f; write_case("MSG_WriteCoord(4095.875)", emit_coord, gco_32767, 2, 0);
	g_float = -4095.875f; write_case("MSG_WriteCoord(-4095.875)", emit_coord, gco_m32767, 2, 0);
	g_float = 4096.0f; write_case("MSG_WriteCoord(4096.0 wraps)", emit_coord, gco_wrap, 2, 0);
	g_float = -4096.0f; write_case("MSG_WriteCoord(-4096.0 wraps)", emit_coord, gco_mwrap, 2, 0);
	g_float = fbits(0x7fc00000); write_case("MSG_WriteCoord(NaN)", emit_coord, gco_zero, 2, 0);
	g_float = fbits(0xff800000); write_case("MSG_WriteCoord(-Inf)", emit_coord, gco_zero, 2, 0);

	g_float = 0.0f; write_case("MSG_WriteAngle(0.0)", emit_angle, gc_zero, 1, 0);
	g_float = 1.0f; write_case("MSG_WriteAngle(1.0)", emit_angle, gan_1, 1, 0);
	g_float = 45.0f; write_case("MSG_WriteAngle(45)", emit_angle, gan_45, 1, 0);
	g_float = 90.0f; write_case("MSG_WriteAngle(90)", emit_angle, gan_90, 1, 0);
	g_float = 180.0f; write_case("MSG_WriteAngle(180)", emit_angle, gan_180, 1, 0);
	g_float = 359.9f; write_case("MSG_WriteAngle(359.9)", emit_angle, gan_360, 1, 0);
	g_float = 360.0f; write_case("MSG_WriteAngle(360 wraps)", emit_angle, gan_360, 1, 0);
	g_float = -45.0f; write_case("MSG_WriteAngle(-45)", emit_angle, gan_m45, 1, 0);
	g_float = 0.5f; write_case("MSG_WriteAngle(0.5)", emit_angle, gc_zero, 1, 0);
	g_float = -0.5f; write_case("MSG_WriteAngle(-0.5)", emit_angle, gc_zero, 1, 0);
	g_float = fbits(0x7fc00000); write_case("MSG_WriteAngle(NaN)", emit_angle, gc_zero, 1, 0);
	g_float = fbits(0xff800000); write_case("MSG_WriteAngle(-Inf)", emit_angle, gc_zero, 1, 0);

	g_float = 0.0f; write_case("MSG_WriteAngle16(0.0)", emit_angle16, gco_zero, 2, 1);
	g_float = 1.0f; write_case("MSG_WriteAngle16(1.0)", emit_angle16, ga16_1, 2, 1);
	g_float = 45.0f; write_case("MSG_WriteAngle16(45)", emit_angle16, ga16_45, 2, 1);
	g_float = 90.0f; write_case("MSG_WriteAngle16(90)", emit_angle16, ga16_90, 2, 1);
	g_float = 180.0f; write_case("MSG_WriteAngle16(180)", emit_angle16, ga16_180, 2, 1);
	g_float = 360.0f; write_case("MSG_WriteAngle16(360 wraps)", emit_angle16, ga16_360, 2, 1);
	g_float = -90.0f; write_case("MSG_WriteAngle16(-90)", emit_angle16, ga16_m90, 2, 1);
	g_float = fbits(0x7fc00000); write_case("MSG_WriteAngle16(NaN)", emit_angle16, gco_zero, 2, 1);
	g_float = fbits(0xff800000); write_case("MSG_WriteAngle16(-Inf)", emit_angle16, gco_zero, 2, 1);
}

/*----------------------------------------------------------------------------
 * usercmd: layout, then the short and long message variants
 *--------------------------------------------------------------------------*/

// msec 13, angles {10, 90, 0}, moves {100, -50, 0}, buttons 3, impulse 7,
// light_level 64.  bits = CM_ANGLE1|CM_FORWARD|CM_SIDE|CM_BUTTONS|CM_IMPULSE|
// CM_MSEC = 0xed, so angle[0] and angle[1] go out as angle16, angle[2] does
// not go out at all, and every remaining field is present.
//
//   angle16(10)  = (int)(10 * 65536/360 + 0.5) = 1820  = 0x071c -> 1c 07
//   angle16(90)  = (int)(90 * 65536/360 + 0.5) = 16384 = 0x4000 -> 00 40
//   char(100 * 0.25) = 25 = 0x19
//   char(-50 * 0.25) = -12 = 0xf4
static const usercmd_t cmd_full = {
	13, { 10.0f, 90.0f, 0.0f }, 100, -50, 0, 3, 7, 64
};
static const unsigned char usercmd_long_golden[] = {
	0xed, 0x40, 0x1c, 0x07, 0x00, 0x40, 0x19, 0xf4, 0x03, 0x07, 0x0d
};
static const unsigned char usercmd_short_golden[] = {
	0xed, 0x1c, 0x07, 0x00, 0x40, 0x19, 0xf4, 0x03, 0x07, 0x0d
};

// The opposite end: no bits set, so only the flag byte goes out (plus the
// light_level byte in the long variant) and angle[1] is still written because
// MSG_WriteUsercmd always sends it.
static const usercmd_t cmd_zero = {
	0, { 0.0f, 0.0f, 0.0f }, 0, 0, 0, 0, 0, 0
};
static const unsigned char usercmd_zero_short_golden[] = { 0x00, 0x00, 0x00 };
static const unsigned char usercmd_zero_long_golden[] = { 0x00, 0x00, 0x00, 0x00 };

// Only CM_ANGLE3: angle[0] is zero so CM_ANGLE1 stays clear and angle[0] is
// skipped entirely, while angle[2] is sent after the always-present angle[1].
static const usercmd_t cmd_angle3 = {
	0, { 0.0f, 90.0f, 180.0f }, 0, 0, 0, 0, 0, 0
};
static const unsigned char usercmd_angle3_golden[] = {
	0x02, 0x00, 0x40, 0x00, 0x80
};

static void usercmd_layout(void)
{
	check(UsercmdC_sizeof() == sizeof(usercmd_t),
		"usercmd layout: size C=%zu Rust=%zu", sizeof(usercmd_t), UsercmdC_sizeof());
	check(UsercmdC_alignof() == _Alignof(usercmd_t),
		"usercmd layout: align C=%zu Rust=%zu", _Alignof(usercmd_t), UsercmdC_alignof());
	check(UsercmdC_offsetof_msec() == offsetof(usercmd_t, msec),
		"usercmd layout: msec C=%zu Rust=%zu", offsetof(usercmd_t, msec), UsercmdC_offsetof_msec());
	check(UsercmdC_offsetof_angles() == offsetof(usercmd_t, angles),
		"usercmd layout: angles C=%zu Rust=%zu", offsetof(usercmd_t, angles), UsercmdC_offsetof_angles());
	check(UsercmdC_offsetof_forwardmove() == offsetof(usercmd_t, forwardmove),
		"usercmd layout: forwardmove C=%zu Rust=%zu", offsetof(usercmd_t, forwardmove), UsercmdC_offsetof_forwardmove());
	check(UsercmdC_offsetof_sidemove() == offsetof(usercmd_t, sidemove),
		"usercmd layout: sidemove C=%zu Rust=%zu", offsetof(usercmd_t, sidemove), UsercmdC_offsetof_sidemove());
	check(UsercmdC_offsetof_upmove() == offsetof(usercmd_t, upmove),
		"usercmd layout: upmove C=%zu Rust=%zu", offsetof(usercmd_t, upmove), UsercmdC_offsetof_upmove());
	check(UsercmdC_offsetof_buttons() == offsetof(usercmd_t, buttons),
		"usercmd layout: buttons C=%zu Rust=%zu", offsetof(usercmd_t, buttons), UsercmdC_offsetof_buttons());
	check(UsercmdC_offsetof_impulse() == offsetof(usercmd_t, impulse),
		"usercmd layout: impulse C=%zu Rust=%zu", offsetof(usercmd_t, impulse), UsercmdC_offsetof_impulse());
	check(UsercmdC_offsetof_light_level() == offsetof(usercmd_t, light_level),
		"usercmd layout: light_level C=%zu Rust=%zu", offsetof(usercmd_t, light_level), UsercmdC_offsetof_light_level());
	check(SizeBufC_sizeof() == sizeof(sizebuf_t),
		"sizebuf layout: C=%zu Rust=%zu", sizeof(sizebuf_t), SizeBufC_sizeof());

	// The Hexen II struct has the same size and a different layout, which is
	// why the port must use the HexenWorld one.  Recorded here as a comment
	// rather than a check, because both protocol.h headers define
	// usercmd_s and cannot be included in one translation unit:
	//   Hexen II    sizeof 28, viewangles 0, forwardmove 12, lightlevel 24
	//   HexenWorld  sizeof 28, msec 0, angles 4, forwardmove 16, light_level 24
}

static void usercmd_cases(void)
{
	g_cmd = &cmd_full;
	g_long = true;
	write_case("MSG_WriteUsercmd(full, long)", emit_usercmd, usercmd_long_golden, 11, 1);
	g_long = false;
	write_case("MSG_WriteUsercmd(full, short)", emit_usercmd, usercmd_short_golden, 10, 1);

	g_cmd = &cmd_zero;
	g_long = true;
	write_case("MSG_WriteUsercmd(zero, long)", emit_usercmd, usercmd_zero_long_golden, 4, 1);
	g_long = false;
	write_case("MSG_WriteUsercmd(zero, short)", emit_usercmd, usercmd_zero_short_golden, 3, 1);

	g_cmd = &cmd_angle3;
	g_long = false;
	write_case("MSG_WriteUsercmd(angle3, short)", emit_usercmd, usercmd_angle3_golden, 5, 1);
}

/*----------------------------------------------------------------------------
 * read cases: each implementation reads the same buffer through its own
 * cursor, and a byte trace of every result plus the final state is compared
 *--------------------------------------------------------------------------*/

static unsigned char rbuf[SB_SIZE];

static void net_reset(const void *data, int len)
{
	memset(rbuf, 0xcc, sizeof rbuf);
	if (len > 0)
		memcpy(rbuf, data, (size_t)len);
	net_message.allowoverflow = 0;
	net_message.overflowed = 0;
	net_message.data = rbuf;
	net_message.maxsize = (int)sizeof rbuf;
	net_message.cursize = len;
	net_message.name = NULL;
}

static unsigned char trace[SB_SIZE];
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
static void rec_cstr(const char *s) { rec(s, strlen(s) + 1); }

// Every read case ends with this: the cursor and the sticky flag are part of
// the contract, not an implementation detail.
static void rec_state(void)
{
	rec_int(*cur->readcount);
	rec_int(*cur->badread);
}

static void expect_int(const char *what, int got, int want)
{
	rec_int(got);
	check(got == want, "%s [%s]: got %d, want %d", what, cur->name, got, want);
}

static void expect_bits(const char *what, float got, unsigned int want)
{
	union { float f; unsigned int u; } v;

	v.f = got;
	rec(&v.u, sizeof v.u);
	check(v.u == want, "%s [%s]: got 0x%08x, want 0x%08x", what, cur->name, v.u, want);
}

static void expect_str(const char *what, const char *got, const char *want)
{
	rec_cstr(got);
	check(strcmp(got, want) == 0, "%s [%s]: got \"%s\" (%zu bytes), want \"%s\"",
		what, cur->name, got, strlen(got), want);
}

static void expect_state(const char *what, int readcount, int badread)
{
	rec_state();
	check(*cur->readcount == readcount && *cur->badread == badread,
		"%s [%s]: state readcount=%d badread=%d, want %d/%d", what, cur->name,
		*cur->readcount, *cur->badread, readcount, badread);
}

static void run_read_case(const char *what, void (*scenario)(void), int needs_h2w)
{
	static unsigned char out[IMPL_COUNT][SB_SIZE];
	static size_t len[IMPL_COUNT];
	int i, j, first = needs_h2w ? IMPL_C_H2W : IMPL_C_H2;

	for (i = 0; i < IMPL_COUNT; ++i) {
		len[i] = 0;
		if (needs_h2w && !impls[i].has_h2w)
			continue;
		cur = &impls[i];
		trace_len = 0;
		scenario();
		len[i] = trace_len;
		memcpy(out[i], trace, trace_len);
	}

	for (j = first + 1; j < IMPL_COUNT; ++j) {
		size_t k;

		if (len[j] != len[first] || memcmp(out[j], out[first], len[first]) != 0) {
			failures++;
			fprintf(stderr, "FAIL: %s: %s and %s produced different traces\n",
				what, impls[first].name, impls[j].name);
			for (k = 0; k < len[first] && k < len[j]; ++k) {
				if (out[first][k] != out[j][k])
					break;
			}
			fprintf(stderr, "      first difference at trace byte %zu"
				" (0x%02x vs 0x%02x), lengths %zu vs %zu\n", k,
				k < len[first] ? out[first][k] : 0,
				k < len[j] ? out[j][k] : 0, len[first], len[j]);
		}
	}
}

static void scenario_read_char(void)
{
	static const unsigned char data[] = { 0x00, 0x7f, 0x80, 0xff };

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_int("MSG_ReadChar 0x00", cur->ReadChar(), 0);
	expect_int("MSG_ReadChar 0x7f", cur->ReadChar(), 127);
	expect_int("MSG_ReadChar 0x80", cur->ReadChar(), -128);
	expect_int("MSG_ReadChar 0xff", cur->ReadChar(), -1);
	expect_state("MSG_ReadChar exhausted", 4, 0);
	expect_int("MSG_ReadChar past end", cur->ReadChar(), -1);
	expect_state("MSG_ReadChar badread", 4, 1);
}

static void scenario_read_byte(void)
{
	static const unsigned char data[] = { 0x00, 0x7f, 0x80, 0xff };

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_int("MSG_ReadByte 0x00", cur->ReadByte(), 0);
	expect_int("MSG_ReadByte 0x7f", cur->ReadByte(), 127);
	expect_int("MSG_ReadByte 0x80", cur->ReadByte(), 128);
	expect_int("MSG_ReadByte 0xff", cur->ReadByte(), 255);
	expect_state("MSG_ReadByte exhausted", 4, 0);
}

static void scenario_read_short(void)
{
	static const unsigned char data[] = { 0x34, 0x12, 0xff, 0xff, 0x00, 0x80, 0xff, 0x7f };

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_int("MSG_ReadShort 0x1234", cur->ReadShort(), 0x1234);
	expect_int("MSG_ReadShort -1", cur->ReadShort(), -1);
	expect_int("MSG_ReadShort -32768", cur->ReadShort(), -32768);
	expect_int("MSG_ReadShort 32767", cur->ReadShort(), 32767);
	expect_state("MSG_ReadShort exhausted", 8, 0);
}

static void scenario_read_long(void)
{
	static const unsigned char data[] = {
		0x78, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0xff, 0x7f
	};

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_int("MSG_ReadLong 0x12345678", cur->ReadLong(), 0x12345678);
	expect_int("MSG_ReadLong -1", cur->ReadLong(), -1);
	expect_int("MSG_ReadLong INT_MIN", cur->ReadLong(), (int)0x80000000);
	expect_int("MSG_ReadLong INT_MAX", cur->ReadLong(), 0x7fffffff);
	expect_state("MSG_ReadLong exhausted", 16, 0);
}

static void scenario_read_float(void)
{
	static const unsigned char data[] = {
		0xdb, 0x0f, 0x49, 0x40,		/* pi */
		0x00, 0x00, 0xc0, 0x7f,		/* qNaN */
		0x01, 0x00, 0x80, 0x7f,		/* sNaN */
		0x00, 0x00, 0x80, 0x7f,		/* +Inf */
		0x00, 0x00, 0x80, 0xff,		/* -Inf */
		0x01, 0x00, 0x00, 0x00,		/* denormal */
		0x00, 0x00, 0x00, 0x80		/* -0.0 */
	};

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_bits("MSG_ReadFloat(pi)", cur->ReadFloat(), 0x40490fdb);
	expect_bits("MSG_ReadFloat(qNaN)", cur->ReadFloat(), 0x7fc00000);
	expect_bits("MSG_ReadFloat(sNaN)", cur->ReadFloat(), 0x7f800001);
	expect_bits("MSG_ReadFloat(+Inf)", cur->ReadFloat(), 0x7f800000);
	expect_bits("MSG_ReadFloat(-Inf)", cur->ReadFloat(), 0xff800000);
	expect_bits("MSG_ReadFloat(denormal)", cur->ReadFloat(), 0x00000001);
	expect_bits("MSG_ReadFloat(-0.0)", cur->ReadFloat(), 0x80000000);
	expect_state("MSG_ReadFloat exhausted", 28, 0);
}

static void scenario_read_coord_angle(void)
{
	static const unsigned char data[] = {
		0x00, 0x00,		/* ReadCoord 0.0 */
		0x08, 0x00,		/* ReadCoord 1.0 */
		0xf8, 0xff,		/* ReadCoord -1.0 */
		0x01, 0x00,		/* ReadCoord 0.125 */
		0x00, 0x80,		/* ReadCoord -4096.0 */
		0x00,			/* ReadAngle 0 */
		0x40,			/* ReadAngle 90 */
		0xe0,			/* ReadAngle -45 */
		0x80			/* ReadAngle -180 */
	};

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_bits("MSG_ReadCoord 0.0", cur->ReadCoord(), 0x00000000);
	expect_bits("MSG_ReadCoord 1.0", cur->ReadCoord(), 0x3f800000);
	expect_bits("MSG_ReadCoord -1.0", cur->ReadCoord(), 0xbf800000);
	expect_bits("MSG_ReadCoord 0.125", cur->ReadCoord(), 0x3e000000);
	expect_bits("MSG_ReadCoord -4096.0", cur->ReadCoord(), 0xc5800000);
	expect_bits("MSG_ReadAngle 0", cur->ReadAngle(), 0x00000000);
	expect_bits("MSG_ReadAngle 90", cur->ReadAngle(), 0x42b40000);
	expect_bits("MSG_ReadAngle -45", cur->ReadAngle(), 0xc2340000);
	expect_bits("MSG_ReadAngle -180", cur->ReadAngle(), 0xc3340000);
	expect_state("coord/angle reads exhausted", 14, 0);
}

static void scenario_read_angle16(void)
{
	static const unsigned char data[] = {
		0x00, 0x40,		/* ReadAngle16 90 */
		0x00, 0xc0,		/* ReadAngle16 -90 */
		0xee, 0xff		/* ReadAngle16 0xffee, i.e. -18 as a short */
	};

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_bits("MSG_ReadAngle16 90", cur->ReadAngle16(), 0x42b40000);
	expect_bits("MSG_ReadAngle16 -90", cur->ReadAngle16(), 0xc2b40000);
	// MSG_ReadAngle16 sign-extends what MSG_WriteAngle16 wrote: the writer's
	// 359.9 is the short 0xffee, and the reader turns that back into -18
	// sixteenths of a degree.  The engine round-trips angles through
	// anglemod, so this is the C's behaviour, not a bug in the port.
	expect_bits("MSG_ReadAngle16 0xffee", cur->ReadAngle16(), 0xbdca8000);
	expect_state("angle16 reads exhausted", 6, 0);
}

static void scenario_read_string(void)
{
	static const unsigned char data[] = {
		0x61, 0x62, 0x63, 0x00,			/* "abc" */
		0x00,					/* "" */
		0xff, 0x80, 0x41, 0x00,			/* high bytes */
		0x78, 0x79				/* no terminator */
	};

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_str("MSG_ReadString(\"abc\")", cur->ReadString(), "abc");
	expect_state("after \"abc\"", 4, 0);
	expect_str("MSG_ReadString(\"\")", cur->ReadString(), "");
	expect_state("after \"\"", 5, 0);
	expect_str("MSG_ReadString(high bytes)", cur->ReadString(), "\xff\x80" "A");
	expect_state("after high bytes", 9, 0);
	expect_str("MSG_ReadString(no terminator)", cur->ReadString(), "xy");
	expect_state("after unterminated string", 11, 1);
	// A bad read leaves the cursor where it stopped, and the flag sticky.
	expect_str("MSG_ReadString(still badread)", cur->ReadString(), "");
	expect_state("after repeated badread", 11, 1);
}

static void scenario_read_string_cap(void)
{
	static unsigned char data[2100];
	int i;

	// No terminator anywhere, and more bytes than the 2048-byte static
	// buffer: the C stops at 2047 characters and leaves the rest unread.
	for (i = 0; i < (int)sizeof data; ++i)
		data[i] = (unsigned char)('A' + (i % 26));

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	{
		const char *s = cur->ReadString();

		rec_cstr(s);
		check(strlen(s) == 2047, "MSG_ReadString cap [%s]: got %zu chars, want 2047",
			cur->name, strlen(s));
		check(s[0] == 'A' && s[2046] == "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[2046 % 26],
			"MSG_ReadString cap [%s]: wrong characters", cur->name);
	}
	expect_state("after capped string", 2047, 0);
	// The remaining 53 characters come out on the next call.
	{
		const char *s = cur->ReadString();

		rec_cstr(s);
		check(strlen(s) == 53, "MSG_ReadString remainder [%s]: got %zu chars, want 53",
			cur->name, strlen(s));
	}
	expect_state("after remainder", 2100, 1);
}

static void scenario_read_string_line(void)
{
	static const unsigned char data[] = {
		0x61, 0x62, 0x0a,		/* "ab\n" */
		0x63, 0x64, 0x00,		/* "cd" terminated by NUL */
		0x65, 0x0a,			/* "e\n" */
		0x66, 0x0d, 0x0a		/* "f\r\n" -- the \r is kept */
	};

	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_str("MSG_ReadStringLine(\"ab\\n\")", cur->ReadStringLine(), "ab");
	expect_state("after \"ab\\n\"", 3, 0);
	expect_str("MSG_ReadStringLine(NUL-terminated)", cur->ReadStringLine(), "cd");
	expect_state("after \"cd\"", 6, 0);
	expect_str("MSG_ReadStringLine(\"e\\n\")", cur->ReadStringLine(), "e");
	expect_state("after \"e\\n\"", 8, 0);
	expect_str("MSG_ReadStringLine(\"f\\r\\n\")", cur->ReadStringLine(), "f\r");
	expect_state("after \"f\\r\\n\"", 11, 0);
	// The two string readers own separate buffers, as the C's two statics do.
	expect_str("MSG_ReadStringLine(eof)", cur->ReadStringLine(), "");
	expect_state("line reader eof", 11, 1);
}

static void scenario_read_truncated(void)
{
	static const unsigned char data[] = { 0x34, 0x12, 0x56, 0x78, 0x9a, 0xbc };

	// One byte short of a short, one of a long, and an empty message.
	net_reset(data, 1);
	cur->BeginReadingFrom(&net_message);
	expect_int("MSG_ReadShort(1 byte)", cur->ReadShort(), -1);
	expect_state("ReadShort truncated", 0, 1);

	net_reset(data, 3);
	cur->BeginReadingFrom(&net_message);
	expect_int("MSG_ReadLong(3 bytes)", cur->ReadLong(), -1);
	expect_state("ReadLong truncated", 0, 1);

	net_reset(data, 2);
	cur->BeginReadingFrom(&net_message);
	expect_int("MSG_ReadShort(2 bytes)", cur->ReadShort(), 0x1234);
	expect_int("MSG_ReadLong(0 left)", cur->ReadLong(), -1);
	expect_state("ReadLong at end", 2, 1);

	// A cursor that is not at zero: readcount+2 > cursize, not cursize < 2.
	net_reset(data, 3);
	cur->BeginReadingFrom(&net_message);
	expect_int("MSG_ReadByte", cur->ReadByte(), 0x34);
	expect_int("MSG_ReadShort(2 left, 1 read)", cur->ReadShort(), 0x5612);
	expect_state("cursor not at zero", 3, 0);
	expect_int("MSG_ReadByte(at end)", cur->ReadByte(), -1);
	expect_state("byte at end", 3, 1);

	net_reset(data, 0);
	cur->BeginReadingFrom(&net_message);
	expect_int("MSG_ReadChar(empty)", cur->ReadChar(), -1);
	expect_int("MSG_ReadByte(empty)", cur->ReadByte(), -1);
	expect_int("MSG_ReadShort(empty)", cur->ReadShort(), -1);
	expect_int("MSG_ReadLong(empty)", cur->ReadLong(), -1);
	expect_state("empty message", 0, 1);
}

static void scenario_read_float_unchecked(void)
{
	// MSG_ReadFloat has no bounds check at all: it takes the four bytes past
	// the end of the message and leaves msg_badread clear.  The bytes here
	// are the 0xcc fill, i.e. the bit pattern 0xcccccccc.
	static const unsigned char data[] = { 0x00 };

	net_reset(data, 0);
	cur->BeginReadingFrom(&net_message);
	expect_bits("MSG_ReadFloat(empty, unchecked)", cur->ReadFloat(), 0xcccccccc);
	expect_state("ReadFloat past the end", 4, 0);
	expect_bits("MSG_ReadFloat(again)", cur->ReadFloat(), 0xcccccccc);
	expect_state("ReadFloat past the end twice", 8, 0);
}

static void scenario_begin_reading(void)
{
	static const unsigned char data[] = { 0xaa, 0xbb, 0xcc, 0xdd };

	// MSG_BeginReading() with no argument reads the global net_message; the
	// harness's net_message is that global for all three implementations.
	net_reset(data, (int)sizeof data);
	cur->BeginReading();
	expect_int("MSG_BeginReading then ReadByte", cur->ReadByte(), 0xaa);
	expect_state("MSG_BeginReading state", 1, 0);

	// A rewind by direct assignment, which is what cl_hw.c does to re-read a
	// packet: the cursor is a plain exported int.
	*cur->readcount = 0;
	*cur->badread = 0;
	expect_int("after rewind", cur->ReadByte(), 0xaa);
	expect_state("after rewind state", 1, 0);
}

static void scenario_badread_sticky(void)
{
	static const unsigned char data[] = { 0x01 };

	net_reset(data, 0);
	cur->BeginReadingFrom(&net_message);
	*cur->badread = 1;		/* as a previous failed read would have */
	expect_int("sticky ReadByte(empty)", cur->ReadByte(), -1);
	expect_state("sticky state", 0, 1);

	// MSG_BeginReadingFrom clears it.
	net_reset(data, (int)sizeof data);
	cur->BeginReadingFrom(&net_message);
	expect_state("BeginReadingFrom clears badread", 0, 0);
	expect_int("ReadByte after reset", cur->ReadByte(), 1);
	expect_state("after reset read", 1, 0);
}

static void scenario_read_usercmd(void)
{
	usercmd_t m;

	// The golden packets from the write half, read back field by field.  The
	// struct is poisoned first so a missing memset(0) shows up.
	net_reset(usercmd_long_golden, (int)sizeof usercmd_long_golden);
	cur->BeginReadingFrom(&net_message);
	memset(&m, 0xa5, sizeof m);
	cur->ReadUsercmd(&m, true);
	rec(&m, sizeof m);
	check(m.msec == 13, "MSG_ReadUsercmd(long) msec [%s]: got %d, want 13", cur->name, m.msec);
	check(m.forwardmove == 100, "MSG_ReadUsercmd(long) forwardmove [%s]: got %d, want 100",
		cur->name, m.forwardmove);
	check(m.sidemove == -48, "MSG_ReadUsercmd(long) sidemove [%s]: got %d, want -48"
		" (the writer quantizes -50 to -12 and the reader multiplies by 4)",
		cur->name, m.sidemove);
	check(m.upmove == 0, "MSG_ReadUsercmd(long) upmove [%s]: got %d, want 0", cur->name, m.upmove);
	check(m.buttons == 3, "MSG_ReadUsercmd(long) buttons [%s]: got %d, want 3", cur->name, m.buttons);
	check(m.impulse == 7, "MSG_ReadUsercmd(long) impulse [%s]: got %d, want 7", cur->name, m.impulse);
	check(m.light_level == 64, "MSG_ReadUsercmd(long) light_level [%s]: got %d, want 64",
		cur->name, m.light_level);
	check(m.angles[0] > 9.9f && m.angles[0] < 10.1f,
		"MSG_ReadUsercmd(long) angles[0] [%s]: got %f, want ~10", cur->name, m.angles[0]);
	check(m.angles[1] == 90.0f, "MSG_ReadUsercmd(long) angles[1] [%s]: got %f, want 90",
		cur->name, m.angles[1]);
	check(m.angles[2] == 0.0f, "MSG_ReadUsercmd(long) angles[2] [%s]: got %f, want 0",
		cur->name, m.angles[2]);
	expect_state("after ReadUsercmd(long)", 11, 0);

	// The short variant omits the light_level byte and leaves it zero.
	net_reset(usercmd_short_golden, (int)sizeof usercmd_short_golden);
	cur->BeginReadingFrom(&net_message);
	memset(&m, 0xa5, sizeof m);
	cur->ReadUsercmd(&m, false);
	rec(&m, sizeof m);
	check(m.light_level == 0, "MSG_ReadUsercmd(short) light_level [%s]: got %d, want 0",
		cur->name, m.light_level);
	check(m.msec == 13 && m.buttons == 3 && m.impulse == 7,
		"MSG_ReadUsercmd(short) fields [%s]: msec=%d buttons=%d impulse=%d",
		cur->name, m.msec, m.buttons, m.impulse);
	check(m.forwardmove == 100 && m.sidemove == -48,
		"MSG_ReadUsercmd(short) moves [%s]: %d/%d", cur->name, m.forwardmove, m.sidemove);
	expect_state("after ReadUsercmd(short)", 10, 0);

	// No bits set: angle[1] is still on the wire, everything else stays zero.
	net_reset(usercmd_zero_long_golden, (int)sizeof usercmd_zero_long_golden);
	cur->BeginReadingFrom(&net_message);
	memset(&m, 0xa5, sizeof m);
	cur->ReadUsercmd(&m, true);
	rec(&m, sizeof m);
	check(m.msec == 0 && m.forwardmove == 0 && m.sidemove == 0 && m.upmove == 0
		&& m.buttons == 0 && m.impulse == 0 && m.light_level == 0
		&& m.angles[0] == 0.0f && m.angles[1] == 0.0f && m.angles[2] == 0.0f,
		"MSG_ReadUsercmd(zero) [%s]: not all zero", cur->name);
	expect_state("after ReadUsercmd(zero)", 4, 0);
}

static void read_cases(void)
{
	run_read_case("MSG_ReadChar", scenario_read_char, 0);
	run_read_case("MSG_ReadByte", scenario_read_byte, 0);
	run_read_case("MSG_ReadShort", scenario_read_short, 0);
	run_read_case("MSG_ReadLong", scenario_read_long, 0);
	run_read_case("MSG_ReadFloat", scenario_read_float, 0);
	run_read_case("MSG_ReadCoord/MSG_ReadAngle", scenario_read_coord_angle, 0);
	run_read_case("MSG_ReadAngle16", scenario_read_angle16, 1);
	run_read_case("MSG_ReadString", scenario_read_string, 0);
	run_read_case("MSG_ReadString cap", scenario_read_string_cap, 0);
	run_read_case("MSG_ReadStringLine", scenario_read_string_line, 1);
	run_read_case("truncation", scenario_read_truncated, 0);
	run_read_case("MSG_ReadFloat unchecked", scenario_read_float_unchecked, 0);
	run_read_case("MSG_BeginReading/rewind", scenario_begin_reading, 0);
	run_read_case("badread stickiness", scenario_badread_sticky, 0);
	run_read_case("MSG_ReadUsercmd", scenario_read_usercmd, 1);
}

/*----------------------------------------------------------------------------
 * round trips, and the documented quantization boundary
 *--------------------------------------------------------------------------*/

// Writes a packet with `from` and reads it back with `to`, so a matched pair
// of bugs in one implementation cannot hide behind the other.
static void round_trip(const char *what, const msg_impl_t *from, const msg_impl_t *to)
{
	static const char text[] = "hello world";
	unsigned char data[256];
	int len, i;
	float coords[] = { 0.0f, 1.0f, -1.0f, 123.5f, -123.5f };
	float angles[] = { 0.0f, 45.0f, 90.0f, 180.0f, 270.0f, -45.0f };

	sb_reset();
	from->WriteByte(&wsb, 0x5a);
	from->WriteChar(&wsb, -7);
	from->WriteShort(&wsb, -12345);
	from->WriteLong(&wsb, 0x0badf00d);
	from->WriteFloat(&wsb, 3.5f);
	from->WriteString(&wsb, text);
	for (i = 0; i < 5; ++i)
		from->WriteCoord(&wsb, coords[i]);
	for (i = 0; i < 6; ++i)
		from->WriteAngle(&wsb, angles[i]);

	len = wsb.cursize;
	memcpy(data, wbuf, (size_t)len);

	net_reset(data, len);
	to->BeginReadingFrom(&net_message);
	check(to->ReadByte() == 0x5a, "%s: ReadByte [%s]", what, to->name);
	check(to->ReadChar() == -7, "%s: ReadChar [%s]", what, to->name);
	check(to->ReadShort() == -12345, "%s: ReadShort [%s]", what, to->name);
	check(to->ReadLong() == 0x0badf00d, "%s: ReadLong [%s]", what, to->name);
	{
		union { float f; unsigned int u; } v;

		v.f = to->ReadFloat();
		check(v.u == 0x40600000u, "%s: ReadFloat [%s] got 0x%08x", what, to->name, v.u);
	}
	check(strcmp(to->ReadString(), text) == 0, "%s: ReadString [%s]", what, to->name);
	for (i = 0; i < 5; ++i) {
		float want = coords[i] * 8.0f;
		float got;

		want = (want >= 0.0f ? (float)(int)(want + 0.5f) : (float)(int)(want - 0.5f)) / 8.0f;
		got = to->ReadCoord();
		check(got == want, "%s: ReadCoord(%f) [%s] got %f want %f",
			what, coords[i], to->name, got, want);
	}
	for (i = 0; i < 6; ++i) {
		float want = angles[i] * (256.0f / 360.0f);
		int byte;
		float got;

		// The writer masks into a byte and the reader sign-extends it again,
		// so the top half of the byte comes back negative.  Both steps are
		// the C's, spelled out here rather than guessed.
		byte = (want >= 0.0f ? (int)(want + 0.5f) : (int)(want - 0.5f)) & 255;
		want = (float)(signed char)byte * (360.0f / 256.0f);
		got = to->ReadAngle();
		check(got == want, "%s: ReadAngle(%f) [%s] got %f want %f",
			what, angles[i], to->name, got, want);
	}
	check(*to->readcount == len && *to->badread == 0,
		"%s: round trip consumed %d of %d bytes, badread=%d [%s]",
		what, *to->readcount, len, *to->badread, to->name);
	checks++;
}

static void round_trip_cases(void)
{
	round_trip("Rust writes, C-H2 reads", &impls[IMPL_RUST], &impls[IMPL_C_H2]);
	round_trip("C-H2 writes, Rust reads", &impls[IMPL_C_H2], &impls[IMPL_RUST]);
	round_trip("Rust writes, C-H2W reads", &impls[IMPL_RUST], &impls[IMPL_C_H2W]);
	round_trip("C-H2W writes, Rust reads", &impls[IMPL_C_H2W], &impls[IMPL_RUST]);
	round_trip("C-H2 writes, C-H2W reads", &impls[IMPL_C_H2], &impls[IMPL_C_H2W]);

	// Determinism: the same input twice must produce the same bytes.
	{
		int i;

		g_int = 0x12345678;
		sb_reset();
		impls[IMPL_RUST].WriteShort(&wsb, g_int);
		i = wsb.cursize;
		memcpy(outs[IMPL_RUST], wbuf, sizeof wbuf);
		sb_reset();
		impls[IMPL_RUST].WriteShort(&wsb, g_int);
		check(i == wsb.cursize && memcmp(outs[IMPL_RUST], wbuf, sizeof wbuf) == 0,
			"deterministic write: repeated MSG_WriteShort differs");
		checks++;
	}
}

// The one place the port does not reproduce the C bit for bit, stated out
// loud.  See the header comment.
static void quantizer_boundary(void)
{
	float pinf = fbits(0x7f800000);

	// NaN and -Inf are in range for the comparison: both implementations land
	// on the same bytes (see the MSG_WriteCoord/MSG_WriteAngle NaN and -Inf
	// cases above, which are compared three ways).

	// +Inf and a coordinate past 2^31/8 are the only inputs where the C's
	// undefined conversion and Rust's saturating one differ on x86-64: the C
	// produces INT_MIN (0x00 0x00 here), the port saturates to INT_MAX, and
	// MSG_WriteShort truncates that to 0xff 0xff.  Pin the port's answer so a
	// change to it is visible.
	g_float = pinf;
	sb_reset();
	impls[IMPL_RUST].WriteCoord(&wsb, g_float);
	check(wsb.cursize == 2 && wbuf[0] == 0xff && wbuf[1] == 0xff,
		"quantizer boundary: Rust MSG_WriteCoord(+Inf) wrote %02x %02x,"
		" expected the saturated 0x7fffffff truncated to 0xffff", wbuf[0], wbuf[1]);
	sb_reset();
	impls[IMPL_RUST].WriteAngle(&wsb, g_float);
	check(wsb.cursize == 1 && wbuf[0] == 0xff,
		"quantizer boundary: Rust MSG_WriteAngle(+Inf) wrote %02x,"
		" expected the saturated 0xff", wbuf[0]);

	printf("NOTE: +Inf and out-of-range coordinates hit C's undefined"
		" float-to-int conversion (C11 6.3.1.4); the port saturates instead\n");
	printf("      and matches the C everywhere the conversion is defined.\n");
}

/*----------------------------------------------------------------------------
 * main
 *--------------------------------------------------------------------------*/

int main(void)
{
	printf("msg_io differential harness: C without H2W (c_*), C with H2W (w_*),"
		" and the Rust union\n");

	usercmd_layout();
	write_cases();
	usercmd_cases();
	read_cases();
	round_trip_cases();
	quantizer_boundary();

	printf("checked %d expectations, %d failures\n", checks, failures);
	if (failures != 0) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}
