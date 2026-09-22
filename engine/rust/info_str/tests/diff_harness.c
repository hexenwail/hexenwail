// SPDX-License-Identifier: GPL-2.0-or-later
//
// Differential harness for engine/hexenworld/shared/info_str.c and the Rust
// info_str module.
//
// The C original is compiled exactly as hwsv compiles it -- -DH2W -DSERVERONLY,
// hexenworld/server and hexenworld/shared ahead of h2shared on the include
// path -- and all six exported names are renamed with a c_ prefix so the Rust
// module can define the unprefixed ABI in the same binary.  The two are then
// driven through one table of calls and must agree byte for byte.
//
// Why the C is compiled with SERVERONLY rather than both ways.  info_str.c
// holds two variants split by `#ifndef SERVERONLY`, and only one of them has
// ever been compiled: the file appears in exactly one CMake source list
// (HWSV_SOURCES, engine/CMakeLists.txt) and hwsv is the only target built with
// SERVERONLY.  Every Hexen II caller of Info_* sits inside `#if defined(H2W)`,
// so glhexen2 and h2ded never reference these symbols.  Comparing against the
// client variant would compare against code no binary in this tree contains.
//
// What is covered, against issue #231's list:
//
//   * empty and malformed info strings: "", "abc" (no separator at all), a
//     string that is one backslash, a first key with no leading separator, and
//     an empty key from a doubled separator;
//   * missing keys, duplicate keys (first wins), and keys with no value;
//   * the >63-character key and value rejection, with the 63/64 boundary shown
//     on both sides, and the message it prints;
//   * the `>= maxsize` rejection and its off-by-one: the same append is shown
//     accepted at maxsize and rejected at maxsize - 1;
//   * the ascii and high-bit filtering, including the `c > 13` retention --
//     0x01 and 0x0d dropped, 0x0e kept -- for both sv_highchars states;
//   * sv_highchars itself, flipped through the cvar the C reads, plus the
//     pre-registration state where the lookup misses and the answer must be 0;
//   * the four-buffer rotation and its aliasing: successive lookups are
//     compared by returned *pointer identity* as well as by text, an earlier
//     value is re-read after later calls to show it was not stomped, and a miss
//     is shown to consume a slot;
//   * the in-place rewrites Info_RemoveKey and Info_RemovePrefixedKeys perform,
//     by recording the caller's whole buffer (poisoned beyond the string) after
//     each call, so the rewrite boundary -- and any scribble past it -- is
//     visible;
//   * Info_Print's padding to 20 columns, its MISSING VALUE path and its
//     skipped leading separator, compared through the CON_Printf sink.
//
// Every case is a two-way comparison driven through one info_impl_t table, not
// an assertion against a hand-written expectation alone: two implementations
// that are both wrong would still pass a golden-only test.  Goldens are checked
// as well, so a shared mistake is caught too.
//
// The sv_highchars route.  The Rust module cannot reference the hwsv-only
// global directly (the consolidated archive is a single object member, so
// glhexen2 and h2ded would inherit the undefined reference), and reads it
// through Cvar_FindVar instead.  This harness is what makes that a checked
// equivalence rather than an assumption: it defines the global the C reads,
// defines a Cvar_FindVar that name-checks its argument and returns that same
// object, and requires the lookup's answer to be 0 when the lookup misses --
// the pre-registration state, where the C's `.integer` is 0 from the brace
// initialiser.  A lookup for any other name is recorded and fails the case, so
// a Rust module that looked up the wrong cvar cannot pass.

#include "quakedef.h"
#include "q_ctype.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------
 * declarations
 *--------------------------------------------------------------------------*/

// The C original, renamed so it can share the binary with the Rust ABI.
#define DECLARE_INFO_IMPL(P) \
	extern const char *P##Info_ValueForKey(const char *s, const char *key); \
	extern void P##Info_RemoveKey(char *s, const char *key); \
	extern void P##Info_RemovePrefixedKeys(char *start, char prefix); \
	extern void P##Info_SetValueForKey(char *s, const char *key, \
			const char *value, size_t maxsize); \
	extern void P##Info_SetValueForStarKey(char *s, const char *key, \
			const char *value, size_t maxsize); \
	extern void P##Info_Print(const char *s)

DECLARE_INFO_IMPL(c_);
DECLARE_INFO_IMPL();

// The Rust cvar_t layout accessors, so the harness can check the struct it
// compiles against without restating Rust's offsets in C.
extern size_t CvarC_sizeof(void);
extern size_t CvarC_alignof(void);
extern size_t CvarC_offsetof_name(void);
extern size_t CvarC_offsetof_string(void);
extern size_t CvarC_offsetof_flags(void);
extern size_t CvarC_offsetof_value(void);
extern size_t CvarC_offsetof_integer(void);
extern size_t CvarC_offsetof_callback(void);
extern size_t CvarC_offsetof_next(void);
extern size_t CvarC_offsetof_default_string(void);

/*----------------------------------------------------------------------------
 * the engine symbols the two implementations call
 *--------------------------------------------------------------------------*/

// CON_Printf is the function behind the `Con_Printf(fmt, ...)` macro in
// engine/h2shared/printsys.h; both implementations reach the error paths and
// Info_Print through it.  Here it records into a log the case runner can
// compare, so a difference in *what* gets printed is a failure and not just a
// difference in state.
static char prlog[8192];
static size_t prlen;
static int pr_overflow;

void CON_Printf(unsigned int flags, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (prlen + 16 >= sizeof prlog) {
		pr_overflow = 1;
		return;
	}

	// The flags word is part of the call: the C's macro always passes
	// _PRINT_NORMAL, and a port that passed something else would print to a
	// different sink.  Recording it makes that a failure.
	n = snprintf(prlog + prlen, sizeof prlog - prlen, "[%u]", flags);
	if (n > 0)
		prlen += (size_t)n;

	va_start(ap, fmt);
	n = vsnprintf(prlog + prlen, sizeof prlog - prlen, fmt, ap);
	va_end(ap);
	if (n > 0)
		prlen += (size_t)n;
	if (prlen > sizeof prlog - 1)
		prlen = sizeof prlog - 1;
}

// The global the C original reads: `cvar_t sv_highchars` from
// engine/hexenworld/server/sv_main.c:62, registered by pointer in SV_InitLocal.
cvar_t sv_highchars = { "sv_highchars", "1", CVAR_NONE };

// Engine symbols the consolidated Rust archive references from the other ports
// (sizebuf, msg_io).  None of them is reachable from info_str -- they exist so
// the link succeeds whichever object the linker pulls, and must never be
// reached.
FUNC_NORETURN void Sys_Error(const char *fmt, ...) { (void)fmt; abort(); }
void *Hunk_AllocName(int size, const char *name)
{
	(void)size;
	(void)name;
	abort();
}

// The buffer msg_io's readers default to: hexen2/net_main.c defines it for the
// Hexen II targets, hexenworld/shared/net_udp.c for hwsv.  Unused here.
sizebuf_t net_message;

// Stands in for engine/h2shared/cvar.c.  It name-checks its argument and
// returns the very object the C original reads, so the two routes are provably
// the same read; `lookup_enabled` OFF models the pre-registration state, where
// the C's `.integer` is 0 and the lookup misses.
static int lookup_enabled;
static int lookup_bad_name;

cvar_t *Cvar_FindVar(const char *var_name)
{
	if (strcmp(var_name, "sv_highchars") != 0) {
		lookup_bad_name++;
		return NULL;
	}

	return lookup_enabled ? &sv_highchars : NULL;
}

/*----------------------------------------------------------------------------
 * the two implementations
 *--------------------------------------------------------------------------*/

typedef struct info_impl_s {
	const char *name;
	const char *(*ValueForKey)(const char *s, const char *key);
	void (*RemoveKey)(char *s, const char *key);
	void (*RemovePrefixedKeys)(char *start, char prefix);
	void (*SetValueForKey)(char *s, const char *key, const char *value,
			size_t maxsize);
	void (*SetValueForStarKey)(char *s, const char *key, const char *value,
			size_t maxsize);
	void (*Print)(const char *s);
} info_impl_t;

enum { IMPL_C = 0, IMPL_RUST = 1, IMPL_COUNT = 2 };

static const info_impl_t impls[IMPL_COUNT] = {
	{
		"C",
		c_Info_ValueForKey, c_Info_RemoveKey, c_Info_RemovePrefixedKeys,
		c_Info_SetValueForKey, c_Info_SetValueForStarKey, c_Info_Print,
	},
	{
		"Rust",
		Info_ValueForKey, Info_RemoveKey, Info_RemovePrefixedKeys,
		Info_SetValueForKey, Info_SetValueForStarKey, Info_Print,
	},
};

static const info_impl_t *cur;

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

#define TRACE_MAX (64 * 1024)

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

// The returned *pointer* is part of the contract: Info_ValueForKey hands back
// one of four static buffers, and callers rely on a value they already hold not
// being overwritten.  Two implementations cannot be compared by address
// directly, so each return is recorded as the index of the earliest call in
// this run that returned the same address: a miss's shared literal and the
// four rotation slots then have comparable identities.
static const char *seen_ptr[64];
static int seen_count;

static int ptr_class(const char *p)
{
	int i;

	for (i = 0; i < seen_count; ++i)
		if (seen_ptr[i] == p)
			return i;

	seen_ptr[seen_count++] = p;
	return seen_count - 1;
}

static void rec_log(void)
{
	rec(prlog, prlen);
}

/*----------------------------------------------------------------------------
 * scenario helpers
 *--------------------------------------------------------------------------*/

#define IOBUF_SIZE 512

static char iobuf[IOBUF_SIZE];

static void buf_set(const char *init)
{
	memset(iobuf, 0xcc, sizeof iobuf);
	if (init)
		memcpy(iobuf, init, strlen(init) + 1);

	// The recorded bytes past the string are the poisoned 0xcc, which is what
	// makes a rewrite past the terminator visible; the last byte stays 0 so a
	// port that forgot to terminate cannot push the comparison out of bounds.
	iobuf[IOBUF_SIZE - 1] = 0;
}

// The whole buffer goes into the trace, not just the string: the in-place
// rewrite boundary is part of the contract, and a port that scribbled past the
// terminator would otherwise look identical.
static void rec_iobuf(void)
{
	rec(iobuf, sizeof iobuf);
}

static void buf_expect(const char *what, const char *want)
{
	check(strcmp(iobuf, want) == 0, "%s [%s]: buffer \"%s\", want \"%s\"",
		what, cur->name, iobuf, want);
	rec_iobuf();
}

static void buf_expect_bytes(const char *what, const unsigned char *want,
		size_t len)
{
	size_t i;

	check(memcmp(iobuf, want, len) == 0,
		"%s [%s]: buffer does not match the %zu expected bytes",
		what, cur->name, len);
	if (memcmp(iobuf, want, len) != 0) {
		fprintf(stderr, "      got  ");
		for (i = 0; i < len; ++i)
			fprintf(stderr, " %02x", (unsigned char)iobuf[i]);
		fprintf(stderr, "\n      want ");
		for (i = 0; i < len; ++i)
			fprintf(stderr, " %02x", want[i]);
		fprintf(stderr, "\n");
	}
	rec_iobuf();
}

static const char *look(const char *what, const char *s, const char *key,
		const char *want)
{
	const char *p = cur->ValueForKey(s, key);

	rec_int(ptr_class(p));
	rec_int((int)strlen(p));
	rec(p, strlen(p) + 1);

	if (want)
		check(strcmp(p, want) == 0, "%s [%s]: got \"%s\", want \"%s\"",
			what, cur->name, p, want);

	return p;
}

static void remove_key(const char *what, const char *init, const char *key,
		const char *want)
{
	buf_set(init);
	cur->RemoveKey(iobuf, key);
	rec_log();
	buf_expect(what, want);
}

static void remove_prefixed(const char *what, const char *init, char prefix,
		const char *want)
{
	buf_set(init);
	cur->RemovePrefixedKeys(iobuf, prefix);
	rec_log();
	buf_expect(what, want);
}

static void set_key(const char *what, const char *init, const char *key,
		const char *value, size_t maxsize, const char *want)
{
	buf_set(init);
	cur->SetValueForKey(iobuf, key, value, maxsize);
	rec_log();
	buf_expect(what, want);
}

static void set_star_key(const char *what, const char *init, const char *key,
		const char *value, size_t maxsize, const char *want)
{
	buf_set(init);
	cur->SetValueForStarKey(iobuf, key, value, maxsize);
	rec_log();
	buf_expect(what, want);
}

static void print_info(const char *what, const char *s, const char *want_log)
{
	prlen = 0;
	cur->Print(s);

	if (want_log)
		check(prlen == strlen(want_log)
			&& memcmp(prlog, want_log, prlen) == 0,
			"%s [%s]: printed %zu bytes, want \"%s\"",
			what, cur->name, prlen, want_log);

	rec_int((int)prlen);
	rec(prlog, prlen);
}

/*----------------------------------------------------------------------------
 * scenarios: lookup
 *--------------------------------------------------------------------------*/

#define SET1 "\\hostname\\hexenwail\\maxclients\\8\\_password\\secret"

static void scenario_lookup_basic(void)
{
	look("value for the first key", SET1, "hostname", "hexenwail");
	look("value for a middle key", SET1, "maxclients", "8");
	look("value for the last key", SET1, "_password", "secret");
	look("missing key", SET1, "nope", "");
	look("missing key again (shares the literal)", SET1, "nope", "");
	look("empty info string", "", "a", "");
	look("one separator, no key", "\\", "a", "");
	look("a key with no separator anywhere", "abc", "abc", "");
	look("first key with no leading separator", "a\\b", "a", "b");
	// The doubled separator makes the empty string a key in its own right, and
	// it is the pair that matches first.
	look("empty key from a doubled separator", "\\\\a\\\\b", "", "a");
	look("the key after that doubled separator", "\\\\a\\\\b", "b", "");
}

static void scenario_lookup_malformed(void)
{
	look("value with no separator before it", "k\\v", "k", "v");
	look("trailing separator", "\\a\\", "a", "");
	look("key with nothing after it", "\\a", "a", "");
	look("empty key, empty value", "\\\\", "", "");
	look("empty key, empty value (same lookup again)", "\\\\", "", "");
	look("a repeated separator", "\\a\\\\b", "a", "");
	look("value that looks like a key", "\\a\\b\\c", "b", "");
	look("the same string, key c", "\\a\\b\\c", "c", "");
	look("duplicate keys: first wins", "\\k\\first\\k\\second", "k", "first");
	look("duplicate keys, other order", "\\k\\second\\k\\first", "k", "second");
	look("case-sensitive key match", "\\Key\\v", "key", "");
	look("value containing spaces", "\\a\\two words\\b\\1", "a", "two words");
}

static void scenario_lookup_long(void)
{
	// 100-character key and value: well inside the 512-byte parse buffers, and
	// past anything MAX_INFO_STRING would let a caller build.
	static char key[101];
	static char value[101];
	static char info[512];

	memset(key, 'k', sizeof key - 1);
	key[sizeof key - 1] = 0;
	memset(value, 'v', sizeof value - 1);
	value[sizeof value - 1] = 0;

	snprintf(info, sizeof info, "\\%s\\%s\\1\\2", key, value);

	look("100-char key", info, key, value);
	look("the short key beside the long pair", info, "1", "2");
	look("a key that is a prefix of the long one", info, "kkk", "");
	look("the long key again (rotation slot reuse)", info, key, value);
}

// The rotation and its aliasing.  valueindex advances at the top of every call
// -- including the calls that return "" -- so the miss below shifts every later
// call by one.  Two things a caller can see are pinned here: the four buffers,
// so a value already handed out survives until its slot comes round again, and
// the fact that a *miss* is not read-only.  It returns the shared literal, but
// the parse still wrote every candidate value into the slot it advanced to, so
// the buffer the first call handed out reads the last candidate's value
// afterwards.
static void scenario_lookup_rotation(void)
{
	static const char *info = "\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E";
	const char *p[8];
	int i;

	p[0] = look("slot 1", info, "a", "A");
	p[1] = look("slot 2", info, "b", "B");
	p[2] = look("slot 3", info, "c", "C");
	p[3] = look("slot 0", info, "d", "D");

	// Four calls, four different buffers, all four still readable.  This is the
	// behaviour sv_main.c relies on when it compares a value taken before a
	// write with one taken after it.
	check(p[0] != p[1] && p[1] != p[2] && p[2] != p[3] && p[0] != p[3],
		"[%s] the four slots are not distinct", cur->name);
	check(strcmp(p[0], "A") == 0 && strcmp(p[1], "B") == 0
		&& strcmp(p[2], "C") == 0 && strcmp(p[3], "D") == 0,
		"[%s] a live slot was overwritten: \"%s\" \"%s\" \"%s\" \"%s\"",
		cur->name, p[0], p[1], p[2], p[3]);

	// A miss: the literal comes back, a slot is consumed, and that slot now
	// holds the value of the last pair the parse walked over.
	p[4] = look("a miss still advances the rotation", info, "zz", "");
	check(strcmp(p[0], "E") == 0,
		"[%s] the miss left slot 1 reading \"%s\"", cur->name, p[0]);
	check(p[4] != p[0], "[%s] a miss returned a rotation slot", cur->name);

	p[5] = look("slot 2 again, one place on because of the miss", info, "e",
		"E");
	check(p[5] == p[1],
		"[%s] the sixth lookup must reuse the second slot's buffer", cur->name);
	check(strcmp(p[1], "E") == 0, "[%s] the reused slot reads \"%s\"",
		cur->name, p[1]);
	// The two slots the miss did not touch are untouched -- checked before the
	// two misses below, which consume them.
	check(strcmp(p[2], "C") == 0 && strcmp(p[3], "D") == 0,
		"[%s] slot 3 or slot 0 was overwritten: \"%s\" \"%s\"", cur->name,
		p[2], p[3]);

	// Two misses in a row hand back the same object, which is not one of the
	// rotation slots -- that is what makes `return ""` a stable pointer.
	for (i = 0; i < 2; ++i)
		p[6 + i] = look("miss", info, "zz", "");
	check(p[6] == p[7], "[%s] two misses returned different pointers", cur->name);
}

/*----------------------------------------------------------------------------
 * scenarios: writing
 *--------------------------------------------------------------------------*/

static void scenario_set_basic(void)
{
	set_key("append to an empty string", "", "a", "1", 196, "\\a\\1");
	set_key("append after an existing pair", "\\a\\1", "b", "2", 196,
		"\\a\\1\\b\\2");
	// A rewrite is a removal followed by an append, so the pair moves to the end
	// of the string rather than staying where it was.  Callers see this: the
	// server's userinfo ordering changes with every re-set.
	set_key("replace an existing key", "\\a\\1\\b\\2", "a", "9", 196,
		"\\b\\2\\a\\9");
	set_key("replace the last key", "\\a\\1\\b\\2", "b", "9", 196,
		"\\a\\1\\b\\9");
	set_key("delete a key by setting it to \"\"", "\\a\\1\\b\\2", "a", "", 196,
		"\\b\\2");
	set_key("delete a key that is not there", "\\a\\1", "zz", "", 196, "\\a\\1");

	// A key with a value that only exists as the empty string is not the same
	// as a missing key: the pair is rewritten with nothing after the separator.
	set_key("set a key to the empty string from empty", "", "a", "", 196, "");
}

static void scenario_set_star_keys(void)
{
	// Info_SetValueForKey refuses '*'; Info_SetValueForStarKey is the one
	// sv_main.c and quakefs.c use for the *gamedir / *spectator / *version keys.
	set_key("Info_SetValueForKey rejects a * key", "\\a\\1", "*gamedir", "hw",
		512, "\\a\\1");
	set_star_key("Info_SetValueForStarKey accepts it", "\\a\\1", "*gamedir",
		"hw", 512, "\\a\\1\\*gamedir\\hw");
	set_star_key("replace a * key", "\\a\\1\\*gamedir\\hw", "*gamedir", "data1",
		512, "\\a\\1\\*gamedir\\data1");
	set_star_key("a literal * key is allowed through the setter",
		"", "*", "v", 512, "\\*\\v");
}

static void scenario_set_reject(void)
{
	// The two strstr refusals, in both arguments, and the info string left
	// exactly as it was.
	set_key("backslash in the key", "\\a\\1", "b\\c", "2", 196, "\\a\\1");
	set_key("backslash in the value", "\\a\\1", "b", "2\\3", 196, "\\a\\1");
	set_key("quote in the key", "\\a\\1", "b\"c", "2", 196, "\\a\\1");
	set_key("quote in the value", "\\a\\1", "b", "2\"3", 196, "\\a\\1");
	set_star_key("backslash in the value, star setter", "\\a\\1", "b", "2\\3",
		512, "\\a\\1");

	// RemoveKey refuses a backslash in the key too, and prints a different
	// message.
	remove_key("backslash in the key", "\\a\\1", "b\\c", "\\a\\1");
}

static void scenario_set_length_limit(void)
{
	static char k63[64];
	static char k64[65];
	static char v63[64];
	static char v64[65];
	static char want[160];

	memset(k63, 'k', 63);
	k63[63] = 0;
	memset(k64, 'k', 64);
	k64[64] = 0;
	memset(v63, 'v', 63);
	v63[63] = 0;
	memset(v64, 'v', 64);
	v64[64] = 0;

	snprintf(want, sizeof want, "\\%s\\1", k63);
	set_key("63-char key is accepted", "", k63, "1", 512, want);

	snprintf(want, sizeof want, "\\%s\\%s", k63, v63);
	set_key("63-char value is accepted", "", k63, v63, 512, want);

	set_key("64-char key is rejected", "", k64, "1", 512, "");
	set_key("64-char value is rejected", "", "a", v64, 512, "");
	set_key("64-char key, existing string untouched", "\\a\\1", k64, "2", 512,
		"\\a\\1");
	set_key("64-char value, existing string untouched", "\\a\\1", "b", v64, 512,
		"\\a\\1");
}

// The `>= maxsize` check, with the same append shown on both sides of the
// boundary: 1 + key + 1 + value + strlen(s) is compared against maxsize, so the
// append that needs exactly maxsize bytes (terminator included) is refused and
// the one that needs maxsize - 1 is taken.
static void scenario_set_maxsize_boundary(void)
{
	// "\\a\\1" is 4 bytes; adding "\b\v" needs 1+1+1+1+4 = 8 more.
	set_key("needs exactly maxsize: refused", "\\a\\1", "b", "v", 8, "\\a\\1");
	set_key("needs maxsize - 1: accepted", "\\a\\1", "b", "v", 9,
		"\\a\\1\\b\\v");
	set_key("needs one byte less again: accepted", "\\a\\1", "b", "v", 32,
		"\\a\\1\\b\\v");
	set_key("one below the boundary: refused", "\\a\\1", "b", "v", 7, "\\a\\1");
	set_key("empty string, needs exactly maxsize: refused", "", "b", "v", 4, "");
	set_key("empty string, needs maxsize - 1: accepted", "", "b", "v", 5,
		"\\b\\v");

	// Reaching the check with a removal already done: the length test uses the
	// caller's string *after* Info_RemoveKey ran, so the same write is refused
	// or accepted against the shrunken string, not the original one.
	set_key("replacement measured after the removal: refused",
		"\\a\\1\\b\\12345678", "b", "v", 8, "\\a\\1");
	set_key("replacement measured after the removal: accepted",
		"\\a\\1\\b\\12345678", "b", "v", 9, "\\a\\1\\b\\v");
}

/*----------------------------------------------------------------------------
 * scenarios: the high-bit filter
 *--------------------------------------------------------------------------*/

// 0x01 and 0x0d are dropped by `c > 13`, 0x0e is the first byte kept, and with
// sv_highchars set that is the only filter: 0x1f, 0x7f, 0x80 and 0xff all
// survive.  With sv_highchars clear the high bit is stripped first and anything
// below 32 is dropped, so 0x0e and 0x1f go as well, 0x80 becomes 0 and goes,
// and 0xff becomes 0x7f and stays.
static const unsigned char HI_IN[] = { 1, 13, 14, 31, 127, 128, 255, 'A', 0 };

static const unsigned char HI_WANT_ON[] = {
	'\\', 'k', '\\', 14, 31, 127, 128, 255, 'A', 0
};

static const unsigned char HI_WANT_OFF[] = {
	'\\', 'k', '\\', 127, 127, 'A', 0
};

static void hi_chars_set(const char *what, int integer, int enabled,
		const unsigned char *want, size_t wantlen)
{
	sv_highchars.integer = integer;
	lookup_enabled = enabled;

	buf_set("");
	cur->SetValueForStarKey(iobuf, "k", (const char *)HI_IN, 512);
	rec_log();
	buf_expect_bytes(what, want, wantlen);
}

static void scenario_highchars_on(void)
{
	// The engine's default: sv_highchars is "1" and registered, so the filter
	// is skipped entirely and every byte above 13 survives.
	hi_chars_set("sv_highchars=1 keeps bytes above 13", 1, 1, HI_WANT_ON,
		sizeof HI_WANT_ON);
}

static void scenario_highchars_off(void)
{
	// sv_highchars set to 0: the high bit is stripped and anything below 32
	// dropped, which is what turns 0x80 into nothing and 0xff into 0x7f.
	hi_chars_set("sv_highchars=0 strips the high bit", 0, 1, HI_WANT_OFF,
		sizeof HI_WANT_OFF);
}

static void scenario_highchars_pre_registration(void)
{
	// Before Cvar_RegisterVariable runs, the C reads the zero-initialised
	// .integer and the Rust's lookup misses; both must filter.
	hi_chars_set("pre-registration: lookup misses, .integer is 0", 0, 0,
		HI_WANT_OFF, sizeof HI_WANT_OFF);
}

static void scenario_highchars_other_keys(void)
{
	// The filter applies to the key as well as the value, and to every pair
	// appended, not just the first.
	sv_highchars.integer = 1;
	lookup_enabled = 1;

	set_key("a second pair is filtered too", "\\a\\1", "b", "2", 512,
		"\\a\\1\\b\\2");
	set_key("a value that is all dropped leaves the separator behind",
		"\\a\\1", "b", "\x01\x02", 512, "\\a\\1\\b\\");
	set_key("a key that is all dropped", "", "\x0d", "v", 512, "\\\\v");
}

/*----------------------------------------------------------------------------
 * scenarios: removal
 *--------------------------------------------------------------------------*/

static void scenario_remove_key(void)
{
	remove_key("remove a middle key", "\\a\\1\\b\\2\\c\\3", "b", "\\a\\1\\c\\3");
	remove_key("remove the first key", "\\a\\1\\b\\2", "a", "\\b\\2");
	remove_key("remove the last key", "\\a\\1\\b\\2", "b", "\\a\\1");
	remove_key("remove the only key", "\\a\\1", "a", "");
	remove_key("remove a key that is not there", "\\a\\1\\b\\2", "zz",
		"\\a\\1\\b\\2");
	remove_key("remove from an empty string", "", "a", "");
	remove_key("remove from a string with no separators", "abc", "abc", "abc");
	remove_key("remove a key with an empty value", "\\a\\\\b\\2", "a",
		"\\b\\2");
	// Removing the empty key removes the pair the doubled separator opened, and
	// the memmove starts at the leading separator: "\\\\a\\1" loses its first
	// three bytes and keeps "\\1".
	remove_key("remove the empty key", "\\\\a\\1", "", "\\1");
	remove_key("remove a key whose value is empty and last", "\\a\\1\\b\\",
		"b", "\\a\\1");
	remove_key("case-sensitive removal", "\\A\\1\\a\\2", "a", "\\A\\1");
	remove_key("remove a duplicate key drops the first only",
		"\\k\\first\\k\\second", "k", "\\k\\second");
}

static void scenario_remove_prefixed(void)
{
	remove_prefixed("drop the underscored keys", "\\_pw\\x\\name\\y\\_z\\w",
		'_', "\\name\\y");
	remove_prefixed("drop a prefix at the head", "\\_a\\1\\b\\2", '_',
		"\\b\\2");
	remove_prefixed("drop a prefix at the tail", "\\b\\2\\_a\\1", '_',
		"\\b\\2");
	remove_prefixed("every key is prefixed", "\\_a\\1\\_b\\2", '_', "");
	remove_prefixed("no key is prefixed", "\\a\\1\\b\\2", '_',
		"\\a\\1\\b\\2");
	remove_prefixed("empty info string", "", '_', "");
	remove_prefixed("keys differing only in case", "\\_a\\1\\_A\\2", '_', "");
	remove_prefixed("a key that merely contains the prefix", "\\a_b\\1\\b\\2",
		'_', "\\a_b\\1\\b\\2");
	remove_prefixed("the separator prefix", "\\\\a\\1\\\\b\\2", '\\',
		"\\\\a\\1\\\\b\\2");
	remove_prefixed("a non-underscore prefix", "\\*gamedir\\hw\\name\\y", '*',
		"\\name\\y");
}

/*----------------------------------------------------------------------------
 * scenarios: printing
 *--------------------------------------------------------------------------*/

static void scenario_print(void)
{
	print_info("one short key", "\\a\\1",
		"[0]a                   [0]1\n");
	print_info("two pairs", "\\a\\1\\b\\2",
		"[0]a                   [0]1\n[0]b                   [0]2\n");
	print_info("a key exactly 20 characters long",
		"\\abcdefghijklmnopqrst\\1",
		"[0]abcdefghijklmnopqrst[0]1\n");
	print_info("a key longer than 20 characters",
		"\\abcdefghijklmnopqrstu\\1",
		"[0]abcdefghijklmnopqrstu[0]1\n");
	print_info("a leading separator is skipped", "\\\\a\\\\b", NULL);
	print_info("an empty info string", "", "");
	print_info("a miss on both sides of the split", "\\\\", NULL);
}

static void scenario_print_missing_value(void)
{
	print_info("a key with no value at all", "\\a",
		"[0]a                   [0]MISSING VALUE\n");
	print_info("a key with no value after a pair", "\\a\\1\\b",
		"[0]a                   [0]1\n[0]b                   [0]MISSING VALUE\n");
	// A trailing separator is an empty *value*, not a missing one: the pair was
	// complete by the time the string ended.
	print_info("a trailing separator is an empty value", "\\a\\",
		"[0]a                   [0]\n");
}

/*----------------------------------------------------------------------------
 * cvar_t layout
 *--------------------------------------------------------------------------*/

static void cvar_layout(void)
{
	check(CvarC_sizeof() == sizeof(cvar_t),
		"CvarC_sizeof: Rust %zu, C %zu", CvarC_sizeof(), sizeof(cvar_t));
	check(CvarC_alignof() == _Alignof(cvar_t),
		"CvarC_alignof: Rust %zu, C %zu", CvarC_alignof(), _Alignof(cvar_t));
	check(CvarC_offsetof_name() == offsetof(cvar_t, name),
		"offsetof(name): Rust %zu, C %zu", CvarC_offsetof_name(),
		offsetof(cvar_t, name));
	check(CvarC_offsetof_string() == offsetof(cvar_t, string),
		"offsetof(string): Rust %zu, C %zu", CvarC_offsetof_string(),
		offsetof(cvar_t, string));
	check(CvarC_offsetof_flags() == offsetof(cvar_t, flags),
		"offsetof(flags): Rust %zu, C %zu", CvarC_offsetof_flags(),
		offsetof(cvar_t, flags));
	check(CvarC_offsetof_value() == offsetof(cvar_t, value),
		"offsetof(value): Rust %zu, C %zu", CvarC_offsetof_value(),
		offsetof(cvar_t, value));
	check(CvarC_offsetof_integer() == offsetof(cvar_t, integer),
		"offsetof(integer): Rust %zu, C %zu", CvarC_offsetof_integer(),
		offsetof(cvar_t, integer));
	check(CvarC_offsetof_callback() == offsetof(cvar_t, callback),
		"offsetof(callback): Rust %zu, C %zu", CvarC_offsetof_callback(),
		offsetof(cvar_t, callback));
	check(CvarC_offsetof_next() == offsetof(cvar_t, next),
		"offsetof(next): Rust %zu, C %zu", CvarC_offsetof_next(),
		offsetof(cvar_t, next));
	check(CvarC_offsetof_default_string() == offsetof(cvar_t, default_string),
		"offsetof(default_string): Rust %zu, C %zu",
		CvarC_offsetof_default_string(),
		offsetof(cvar_t, default_string));

	// The one field the port reads.  This is the address the Rust Cvar_FindVar
	// lookup returns and the address the C global has.
	check(offsetof(cvar_t, integer) == 2 * sizeof(void *) + 8,
		"cvar_t.integer moved: offset %zu", offsetof(cvar_t, integer));
}

/*----------------------------------------------------------------------------
 * case runner
 *--------------------------------------------------------------------------*/

static void scenario_reset(void)
{
	sv_highchars.integer = 1;
	lookup_enabled = 1;
	lookup_bad_name = 0;
	prlen = 0;
	pr_overflow = 0;
}

static void run_case(const char *what, void (*scenario)(void))
{
	static unsigned char out[IMPL_COUNT][TRACE_MAX];
	static size_t len[IMPL_COUNT];
	static int bad[IMPL_COUNT];
	int i;

	for (i = 0; i < IMPL_COUNT; ++i) {
		cur = &impls[i];

		trace_len = 0;
		seen_count = 0;
		scenario_reset();
		scenario();

		// A lookup for any name other than sv_highchars is a failure: this is
		// what stops a port from "reading the cvar" through some other name.
		bad[i] = lookup_bad_name;

		if (pr_overflow) {
			failures++;
			fprintf(stderr, "FAIL: %s [%s]: CON_Printf log overflow\n",
				what, impls[i].name);
		}
		if (trace_len > sizeof trace) {
			failures++;
			fprintf(stderr, "FAIL: %s [%s]: trace too long\n", what,
				impls[i].name);
			trace_len = sizeof trace;
		}

		len[i] = trace_len;
		memcpy(out[i], trace, trace_len);
	}

	for (i = 0; i < IMPL_COUNT; ++i) {
		if (bad[i] != 0) {
			failures++;
			fprintf(stderr, "FAIL: %s [%s]: Cvar_FindVar was called %d "
				"time(s) with a name other than sv_highchars\n", what,
				impls[i].name, bad[i]);
		}
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
	printf("info_str differential harness: the C as hwsv compiles it (-DH2W "
		"-DSERVERONLY) and the Rust module\n");

	cvar_layout();

	run_case("lookup: basic and missing keys", scenario_lookup_basic);
	run_case("lookup: malformed strings", scenario_lookup_malformed);
	run_case("lookup: long keys and values", scenario_lookup_long);
	run_case("lookup: the four-buffer rotation and aliasing",
		scenario_lookup_rotation);
	run_case("set: append, replace and delete", scenario_set_basic);
	run_case("set: star keys", scenario_set_star_keys);
	run_case("set: refused keys and values", scenario_set_reject);
	run_case("set: the 63/64 character limit", scenario_set_length_limit);
	run_case("set: the >= maxsize boundary", scenario_set_maxsize_boundary);
	run_case("set: sv_highchars set", scenario_highchars_on);
	run_case("set: sv_highchars clear", scenario_highchars_off);
	run_case("set: before sv_highchars is registered",
		scenario_highchars_pre_registration);
	run_case("set: the filter on keys and later pairs",
		scenario_highchars_other_keys);
	run_case("remove: Info_RemoveKey", scenario_remove_key);
	run_case("remove: Info_RemovePrefixedKeys", scenario_remove_prefixed);
	run_case("print: Info_Print", scenario_print);
	run_case("print: Info_Print's MISSING VALUE path", scenario_print_missing_value);

	printf("checked %d expectations, %d failures\n", checks, failures);
	if (failures != 0) {
		printf("RESULT: FAIL\n");
		return 1;
	}
	printf("RESULT: PASS\n");
	return 0;
}