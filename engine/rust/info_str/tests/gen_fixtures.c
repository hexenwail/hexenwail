// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fixture generator for the info_str replay.  Compiles the C original
// (engine/hexenworld/shared/info_str.c, unmodified) exactly as hwsv does
// (-DH2W -DSERVERONLY, hexenworld/shared ahead of h2shared) and prints every
// expected value from it, so tests/fixtures/info_str.txt can be audited
// against C at any time:
//
//   bash engine/rust/info_str/tests/gen_fixtures.sh          # rewrite the fixture
//   bash engine/rust/info_str/tests/gen_fixtures.sh --check  # fail if it drifted
//
// Only the SERVERONLY variant exists to freeze: info_str.c is in HWSV_SOURCES
// alone, and the `#ifndef SERVERONLY` client branch is compiled into no binary
// (see the module comment in engine/rust/src/info_str.rs).
//
// Deterministic: a fixed-seed xorshift32, no clock, no pid.  Format: see
// tests/fixtures/README.md.

#include "quakedef.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*----------------------------------------------------------------------------
 * the engine symbols the C original reaches
 *--------------------------------------------------------------------------*/

// The global the C reads: `cvar_t sv_highchars` from
// engine/hexenworld/server/sv_main.c.
cvar_t sv_highchars = { "sv_highchars", "1", CVAR_NONE };

// The function behind the `Con_Printf(fmt, ...)` macro (printsys.h).  Records
// "[flags]" and the formatted text, one call after another.
static char prlog[8192];
static size_t prlen;

void CON_Printf(unsigned int flags, const char *fmt, ...)
{
	va_list ap;
	int n;

	n = snprintf(prlog + prlen, sizeof prlog - prlen, "[%u]", flags);
	if (n > 0)
		prlen += (size_t)n;
	va_start(ap, fmt);
	n = vsnprintf(prlog + prlen, sizeof prlog - prlen, fmt, ap);
	va_end(ap);
	if (n > 0)
		prlen += (size_t)n;
	if (prlen > sizeof prlog - 1) {
		fprintf(stderr, "gen_fixtures: CON_Printf log overflow\n");
		abort();
	}
}

/*----------------------------------------------------------------------------
 * output helpers
 *--------------------------------------------------------------------------*/

// Strings are written one token each: printable ASCII as itself except '%'
// and '~', everything else (NUL, space, control bytes, high bytes) as %xx,
// and the empty string as "~".  A backslash is literal, so an info string
// reads as an info string.
static void esc(const void *vp, size_t n)
{
	const unsigned char *p = vp;
	size_t i;

	if (n == 0) {
		putchar('~');
		return;
	}
	for (i = 0; i < n; ++i) {
		unsigned char c = p[i];

		if (c > 0x20 && c < 0x7f && c != '%' && c != '~')
			putchar(c);
		else
			printf("%%%02x", c);
	}
}

static void esc_str(const char *s)
{
	esc(s, strlen(s));
}

static uint32_t rng_state = 0x1f2e3d4cu;

static uint32_t rng(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rng_state = x;
}

static unsigned rnd(unsigned n)
{
	return rng() % n;
}

/*----------------------------------------------------------------------------
 * buffer-writing operations
 *--------------------------------------------------------------------------*/

#define BUFSZ 1024
#define POISON 0xcc

static unsigned char iobuf[BUFSZ];

static void buf_set(const char *init)
{
	size_t n = strlen(init);

	if (n >= BUFSZ - 8) {
		fprintf(stderr, "gen_fixtures: init string too long\n");
		abort();
	}
	memset(iobuf, POISON, sizeof iobuf);
	memcpy(iobuf, init, n + 1);
}

// Records the log and the window of the buffer the call can have touched:
// 0 .. max(old, new) + 1, so the NUL and any stale bytes memmove left behind
// are part of the record.  Everything after the window must still be poison,
// which is asserted here against the C, not assumed.
static void finish(size_t oldlen)
{
	size_t newlen = strlen((char *)iobuf);
	size_t win = (oldlen > newlen ? oldlen : newlen) + 1;
	size_t i;

	for (i = win; i < BUFSZ; ++i)
		if (iobuf[i] != POISON) {
			fprintf(stderr, "gen_fixtures: C wrote past its window\n");
			abort();
		}
	putchar(' ');
	esc(prlog, prlen);
	putchar(' ');
	esc(iobuf, win);
	putchar('\n');
}

static void do_rm(const char *init, const char *key)
{
	buf_set(init);
	prlen = 0;
	Info_RemoveKey((char *)iobuf, key);
	printf("rm ");
	esc_str(init);
	putchar(' ');
	esc_str(key);
	finish(strlen(init));
}

static void do_rmp(const char *init, int prefix)
{
	buf_set(init);
	prlen = 0;
	Info_RemovePrefixedKeys((char *)iobuf, (char)prefix);
	printf("rmp ");
	esc_str(init);
	printf(" %02x", prefix & 255);
	finish(strlen(init));
}

// star: 1 = Info_SetValueForStarKey, 0 = Info_SetValueForKey.
// hc < 0: sv_highchars not registered (the Rust lookup misses, the C global's
// .integer is 0); otherwise the registered cvar's .integer.
static void do_set(int star, int hc, size_t maxsize, const char *init,
		const char *key, const char *value)
{
	buf_set(init);
	prlen = 0;
	sv_highchars.integer = hc < 0 ? 0 : hc;
	if (star)
		Info_SetValueForStarKey((char *)iobuf, key, value, maxsize);
	else
		Info_SetValueForKey((char *)iobuf, key, value, maxsize);
	sv_highchars.integer = 1;
	printf("set %c ", star ? 'S' : 'K');
	if (hc < 0)
		fputs("none", stdout);
	else
		printf("%d", hc);
	printf(" %zu ", maxsize);
	esc_str(init);
	putchar(' ');
	esc_str(key);
	putchar(' ');
	esc_str(value);
	finish(strlen(init));
}

static void do_print(const char *s)
{
	prlen = 0;
	Info_Print(s);
	printf("print ");
	esc_str(s);
	putchar(' ');
	esc(prlog, prlen);
	putchar('\n');
}

/*----------------------------------------------------------------------------
 * lookup runs
 *
 * Info_ValueForKey keeps four static result buffers and rotates through them
 * on every call, misses included.  A run is a sequence of lookups; each record
 * gives the pointer's class (`E` for the empty literal a miss returns, else the
 * index of the first earlier return of the same address within the run, in order
 * of first appearance) and the current text
 * of every rotation slot seen so far, so a value overwritten by a later call is
 * visible.
 *--------------------------------------------------------------------------*/

static const char *seen[64];
static int nseen;

// The two places Info_ValueForKey can `return ""`.  Whether a compiler gives
// them one address or two is not part of any contract, so a miss is recorded
// as the class `E` (found by probing both sites once) and never as a slot.
static const char *lit[2];

static void run_begin(void)
{
	nseen = 0;
	puts("rot");
}

static void do_look(const char *info, const char *key)
{
	const char *p = Info_ValueForKey(info, key);
	int cls, i;

	printf("look ");
	esc_str(info);
	putchar(' ');
	esc_str(key);
	if (p == lit[0] || p == lit[1]) {
		fputs(" E", stdout);
	} else {
		for (cls = 0; cls < nseen; ++cls)
			if (seen[cls] == p)
				break;
		if (cls == nseen) {
			if (nseen == 64)
				abort();
			seen[nseen++] = p;
		}
		printf(" %d", cls);
	}
	for (i = 0; i < nseen; ++i) {
		putchar(' ');
		esc_str(seen[i]);
	}
	putchar('\n');
}

/*----------------------------------------------------------------------------
 * random strings
 *--------------------------------------------------------------------------*/

static const char *const key_pool[] = {
	"a", "b", "name", "team", "_pw", "_x", "*gamedir", "*spectator", "topcolor",
	"k", "", "*", "_",
};
#define NKEYS ((unsigned)(sizeof key_pool / sizeof key_pool[0]))

static const char *const val_pool[] = {
	"1", "0", "two words", "hexenwail", "x", "", "Z", "12345678", "-",
};
#define NVALS ((unsigned)(sizeof val_pool / sizeof val_pool[0]))

static const unsigned char odd_bytes[] = { 1, 13, 14, 31, 127, 128, 200, 255 };

// A string of `n` printable characters drawn from `c`, for overlong values.
static void fill(char *out, size_t n, char c)
{
	memset(out, c, n);
	out[n] = 0;
}

static void rand_key(char *out)
{
	unsigned r = rnd(100);

	if (r < 75) {
		strcpy(out, key_pool[rnd(NKEYS)]);
	} else if (r < 85) {
		fill(out, 60 + rnd(8), 'k');
	} else {
		size_t n = 1 + rnd(5), i;

		for (i = 0; i < n; ++i)
			out[i] = (char)('a' + rnd(6));
		out[n] = 0;
	}
}

static void rand_val(char *out)
{
	unsigned r = rnd(100);

	if (r < 55) {
		strcpy(out, val_pool[rnd(NVALS)]);
	} else if (r < 65) {
		fill(out, 60 + rnd(8), 'v');
	} else if (r < 85) {
		size_t n = 1 + rnd(4), i;

		for (i = 0; i < n; ++i)
			out[i] = rnd(2) ? (char)odd_bytes[rnd(sizeof odd_bytes)]
					: (char)('a' + rnd(6));
		out[n] = 0;
	} else {
		size_t n = 1 + rnd(6), i;

		for (i = 0; i < n; ++i)
			out[i] = (char)('A' + rnd(8));
		out[n] = 0;
	}
}

// A well-formed or deliberately malformed info string.
static void rand_info(char *out, int pairs)
{
	size_t n = 0;
	int i, nkeys = pairs;
	char k[128], v[128];

	if (rnd(10) == 0)
		nkeys = 0;
	for (i = 0; i < nkeys; ++i) {
		rand_key(k);
		rand_val(v);
		// the odd byte values above may be quote-free; add quotes and
		// spaces only through the pools
		if (i > 0 || rnd(10) != 0)
			out[n++] = '\\';
		n += (size_t)sprintf(out + n, "%s", k);
		if (rnd(25) == 0)
			out[n++] = '\\';	// doubled separator
		out[n++] = '\\';
		if (!(i == nkeys - 1 && rnd(8) == 0))	// odd field count
			n += (size_t)sprintf(out + n, "%s", v);
	}
	if (rnd(10) == 0)
		out[n++] = '\\';	// trailing backslash
	out[n] = 0;
}

/*----------------------------------------------------------------------------
 * directed cases
 *--------------------------------------------------------------------------*/

#define SET1 "\\hostname\\hexenwail\\maxclients\\8\\_password\\secret"

static void directed_lookups(void)
{
	char key[101], value[101], info[512];

	run_begin();
	do_look(SET1, "hostname");
	do_look(SET1, "maxclients");
	do_look(SET1, "_password");
	do_look(SET1, "nope");
	do_look(SET1, "nope");
	do_look("", "a");
	do_look("\\", "a");
	do_look("abc", "abc");
	do_look("a\\b", "a");
	do_look("\\\\a\\\\b", "");
	do_look("\\\\a\\\\b", "b");

	run_begin();
	do_look("k\\v", "k");
	do_look("\\a\\", "a");
	do_look("\\a", "a");
	do_look("\\\\", "");
	do_look("\\\\", "");
	do_look("\\a\\\\b", "a");
	do_look("\\a\\b\\c", "b");
	do_look("\\a\\b\\c", "c");
	do_look("\\k\\first\\k\\second", "k");
	do_look("\\k\\second\\k\\first", "k");
	do_look("\\Key\\v", "key");
	do_look("\\a\\two words\\b\\1", "a");

	fill(key, 100, 'k');
	fill(value, 100, 'v');
	snprintf(info, sizeof info, "\\%s\\%s\\1\\2", key, value);
	run_begin();
	do_look(info, key);
	do_look(info, "1");
	do_look(info, "kkk");
	do_look(info, key);

	// The rotation.  The miss in the middle still takes a slot, and the
	// parse leaves the last candidate value in it.
	run_begin();
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "a");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "b");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "c");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "d");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "zz");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "e");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "zz");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "zz");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "a");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "b");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "c");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "d");
	do_look("\\a\\A\\b\\B\\c\\C\\d\\D\\e\\E", "e");
	// A miss that overwrote a held slot with different infos.
	run_begin();
	do_look("\\p\\one\\q\\two", "p");
	do_look("\\r\\three\\s\\four", "zz");
	do_look("\\r\\three\\s\\four", "r");
	do_look("\\x\\5", "x");
	do_look("\\x\\5", "y");
	do_look("\\x\\5", "x");
	do_look("\\x\\5", "x");
}

static void directed_rm(void)
{
	char k[64];

	do_rm("\\a\\1\\b\\2\\c\\3", "b");
	do_rm("\\a\\1\\b\\2", "a");
	do_rm("\\a\\1\\b\\2", "b");
	do_rm("\\a\\1", "a");
	do_rm("\\a\\1\\b\\2", "zz");
	do_rm("", "a");
	do_rm("abc", "abc");
	do_rm("a\\1\\b\\2", "a");
	do_rm("a\\1\\b\\2", "b");
	do_rm("\\a\\\\b\\2", "a");
	do_rm("\\\\a\\1", "");
	do_rm("\\a\\1\\b\\", "b");
	do_rm("\\a\\1\\b", "b");
	do_rm("\\a\\1\\", "a");
	do_rm("\\a\\1\\", "");
	do_rm("\\A\\1\\a\\2", "a");
	do_rm("\\k\\first\\k\\second", "k");
	do_rm("\\a\\1", "b\\c");
	do_rm("\\a\\1\\b\\2", "b\\");
	do_rm("\\", "a");
	do_rm("\\\\\\\\", "");
	do_rm("\\a", "a");
	do_rm("a", "a");
	do_rm("\\a\\1\\a\\2\\a\\3", "a");
	fill(k, 63, 'k');
	do_rm("\\a\\1", k);
}

static void directed_rmp(void)
{
	do_rmp("\\_pw\\x\\name\\y\\_z\\w", '_');
	do_rmp("\\_a\\1\\b\\2", '_');
	do_rmp("\\b\\2\\_a\\1", '_');
	do_rmp("\\_a\\1\\_b\\2", '_');
	do_rmp("\\a\\1\\b\\2", '_');
	do_rmp("", '_');
	do_rmp("\\_a\\1\\_A\\2", '_');
	do_rmp("\\a_b\\1\\b\\2", '_');
	do_rmp("\\\\a\\1\\\\b\\2", '\\');
	do_rmp("\\*gamedir\\hw\\name\\y", '*');
	do_rmp("\\*a\\1\\*b\\2\\*c\\3", '*');
	do_rmp("\\*a\\1\\x\\*b\\*c\\3", '*');
	do_rmp("\\_a\\1\\_a\\2\\b\\3", '_');
	do_rmp("\\_a\\1\\_", '_');
	do_rmp("\\_a", '_');
	do_rmp("_a\\1\\b\\2", '_');
	do_rmp("\\a\\1\\_b\\", '_');
	do_rmp("\\a\\1\\\\x", 0);
	do_rmp("\\a\\1\\\\x\\y\\z", 0);
	do_rmp("\\\\v\\a\\1", 0);
	do_rmp("\\\xe9x\\1\\b\\2", 0xe9);
	do_rmp("\\a\\1\\\xe9x\\1", 0xe9);
}

static void directed_set(void)
{
	char k63[64], k64[65], v63[64], v64[65];
	int hc;

	fill(k63, 63, 'k');
	fill(k64, 64, 'k');
	fill(v63, 63, 'v');
	fill(v64, 64, 'v');

	do_set(0, 1, 196, "", "a", "1");
	do_set(0, 1, 196, "\\a\\1", "b", "2");
	do_set(0, 1, 196, "\\a\\1\\b\\2", "a", "9");
	do_set(0, 1, 196, "\\a\\1\\b\\2", "b", "9");
	do_set(0, 1, 196, "\\a\\1\\b\\2", "a", "");
	do_set(0, 1, 196, "\\a\\1", "zz", "");
	do_set(0, 1, 196, "", "a", "");

	do_set(0, 1, 512, "\\a\\1", "*gamedir", "hw");
	do_set(1, 1, 512, "\\a\\1", "*gamedir", "hw");
	do_set(1, 1, 512, "\\a\\1\\*gamedir\\hw", "*gamedir", "data1");
	do_set(1, 1, 512, "", "*", "v");
	do_set(0, 1, 512, "", "*", "v");
	do_set(0, 1, 512, "", "", "v");
	do_set(0, 1, 512, "\\\\x\\y", "", "v");

	do_set(0, 1, 196, "\\a\\1", "b\\c", "2");
	do_set(0, 1, 196, "\\a\\1", "b", "2\\3");
	do_set(0, 1, 196, "\\a\\1", "b\"c", "2");
	do_set(0, 1, 196, "\\a\\1", "b", "2\"3");
	do_set(1, 1, 512, "\\a\\1", "b", "2\\3");
	do_set(1, 1, 512, "\\a\\1", "b", "\"");
	do_set(1, 1, 512, "\\a\\1", "\\", "v");
	do_set(1, 1, 512, "\\a\\1", "b", "\\");

	do_set(0, 1, 512, "", k63, "1");
	do_set(0, 1, 512, "", k63, v63);
	do_set(0, 1, 512, "", k64, "1");
	do_set(0, 1, 512, "", "a", v64);
	do_set(0, 1, 512, "\\a\\1", k64, "2");
	do_set(0, 1, 512, "\\a\\1", "b", v64);
	do_set(0, 1, 512, "\\a\\1", k64, "");
	do_set(0, 1, 512, "\\a\\1", k63, "");

	// The >= maxsize boundary, both sides.
	do_set(0, 1, 8, "\\a\\1", "b", "v");
	do_set(0, 1, 9, "\\a\\1", "b", "v");
	do_set(0, 1, 32, "\\a\\1", "b", "v");
	do_set(0, 1, 7, "\\a\\1", "b", "v");
	do_set(0, 1, 4, "", "b", "v");
	do_set(0, 1, 5, "", "b", "v");
	do_set(0, 1, 0, "", "b", "v");
	do_set(0, 1, 1, "", "b", "v");
	do_set(0, 1, 3, "\\a\\1", "b", "v");
	do_set(0, 1, 8, "\\a\\1\\b\\12345678", "b", "v");
	do_set(0, 1, 9, "\\a\\1\\b\\12345678", "b", "v");
	do_set(0, 1, 0, "\\a\\1", "a", "");
	do_set(0, 1, 100, "\\a\\1", "a", "");

	// The high-bit filter.  0x01 and 0x0d dropped, 0x0e first kept.
	for (hc = -1; hc <= 2; ++hc) {
		do_set(1, hc, 512, "", "k", "\x01\x0d\x0e\x1f\x7f\x80\xff" "A");
		do_set(1, hc, 512, "\\a\\1", "b", "\x01\x02");
		do_set(1, hc, 512, "", "\x0d", "v");
		do_set(1, hc, 512, "", "k\xe9", "v\xe9\x81");
		do_set(1, hc, 512, "", "\x80", "\x80");
		do_set(0, hc, 512, "", "n", "A B\tC");
		do_set(1, hc, 512, "", "k", "\x0b\x0c\x0e\x0f");
	}
	do_set(1, -1, 512, "\\a\\1", "b", "2");
	do_set(1, 0, 512, "\\a\\1", "b", "2");
	do_set(1, 3, 512, "\\a\\1", "b", "\x80\x90");
	do_set(1, -2, 512, "\\a\\1", "b", "\x80\x90");
}

static void directed_print(void)
{
	char k20[21], k21[22], k19[20];

	fill(k19, 19, 'q');
	fill(k20, 20, 'r');
	fill(k21, 21, 's');

	do_print("\\a\\1");
	do_print("\\a\\1\\b\\2");
	{
		char s[160];

		snprintf(s, sizeof s, "\\%s\\1\\%s\\2\\%s\\3", k19, k20, k21);
		do_print(s);
	}
	do_print("\\\\a\\\\b");
	do_print("");
	do_print("\\\\");
	do_print("\\");
	do_print("\\a");
	do_print("\\a\\1\\b");
	do_print("\\a\\");
	do_print("a\\1");
	do_print("a");
	do_print("\\a\\1\\");
	do_print("\\a\\1\\\\");
	do_print("\\a\\1\\\\x");
	do_print("\\\\\\");
	do_print("\\a\\\\\\b");
	do_print("\\k\\v w\\x");
}

/*----------------------------------------------------------------------------
 * seeded random
 *--------------------------------------------------------------------------*/

// Copies the `nth` key (or the last one before it) of an info string into
// `out`; returns 0 when the string has no key at all.
static int pick_key(const char *info, char *out, unsigned nth)
{
	const char *s = info + (info[0] == '\\');
	unsigned i;

	for (i = 0;; ++i) {
		size_t n = strcspn(s, "\\");

		if (s[n] == 0)
			return 0;	// no value follows: not a pair
		if (i == nth || strchr(s + n + 1, '\\') == NULL) {
			memcpy(out, s, n);
			out[n] = 0;
			return 1;
		}
		s += n + 1;
		s += strcspn(s, "\\");
		if (*s == 0)
			return 0;
		s++;
	}
}

static void random_lookups(int runs)
{
	char a[1024], b[1024], key[128];
	int r, i, n;

	for (r = 0; r < runs; ++r) {
		rand_info(a, 1 + (int)rnd(4));
		rand_info(b, 1 + (int)rnd(4));
		run_begin();
		n = 6 + (int)rnd(10);
		for (i = 0; i < n; ++i) {
			const char *info = rnd(2) ? a : b;

			// 60%: a key the string really holds
			if (!(rnd(100) < 60 && pick_key(info, key, rnd(3))))
				rand_key(key);
			do_look(info, key);
		}
	}
}

static void random_rm(int count)
{
	char info[1024], key[128];
	int i;

	for (i = 0; i < count; ++i) {
		rand_info(info, (int)rnd(6));
		if (rnd(4) == 0 && info[0]) {
			// remove a key that is certainly present: the first one
			const char *s = info + (info[0] == '\\');
			size_t n = strcspn(s, "\\");

			memcpy(key, s, n);
			key[n] = 0;
		} else {
			rand_key(key);
		}
		do_rm(info, key);
	}
}

static void random_rmp(int count)
{
	static const int prefixes[] = { '_', '*', '_', 'a', 0, 'n', 0xe9 };
	char info[1024];
	int i;

	for (i = 0; i < count; ++i) {
		rand_info(info, (int)rnd(6));
		do_rmp(info, prefixes[rnd(sizeof prefixes / sizeof prefixes[0])]);
	}
}

static void random_set(int count)
{
	char info[1024], key[128], value[128];
	int i;

	for (i = 0; i < count; ++i) {
		unsigned r = rnd(100);
		size_t maxsize;
		int hc;

		rand_info(info, (int)rnd(5));
		rand_key(key);
		rand_val(value);
		if (rnd(12) == 0)
			strcpy(value, "");
		if (rnd(40) == 0)
			strcat(key, "\\");
		else if (rnd(40) == 0)
			strcat(value, "\"");

		if (r < 55)
			maxsize = strlen(info) + strlen(key) + strlen(value) + 2
					+ (size_t)rnd(7) - 3;
		else if (r < 80)
			maxsize = 8 + rnd(150);
		else
			maxsize = 1000;
		if (maxsize > BUFSZ - 8)
			maxsize = BUFSZ - 8;

		hc = (int)rnd(4) - 1;	// -1 none, 0, 1, 2
		do_set((int)rnd(2), hc, maxsize, info, key, value);
	}
}

static void random_print(int count)
{
	char info[1024];
	int i;

	for (i = 0; i < count; ++i) {
		rand_info(info, (int)rnd(5));
		do_print(info);
	}
}

/*----------------------------------------------------------------------------
 * the small-alphabet sweep
 *
 * Every string over { '\\', 'a', 'b', '*' } of length 0..=SWEEP_MAXLEN, each
 * run through ten operations, with every result (returned text, the whole
 * 64-byte buffer, the log) folded into one FNV-1a digest.  The replay folds
 * the Rust results the same way and compares, so malformed shapes the random
 * mixes miss (every placement of doubled, leading and trailing separators)
 * are frozen without a 200 KB fixture.  Serialisation must match
 * replay.rs::sweep_digest exactly.
 *--------------------------------------------------------------------------*/

#define SWEEP_MAXLEN 7
#define SWEEP_BUF 64

static uint64_t fnv = 0xcbf29ce484222325ull;

static void dig(const void *vp, size_t n)
{
	const unsigned char *p = vp;
	size_t i;

	for (i = 0; i < n; ++i) {
		fnv ^= p[i];
		fnv *= 0x100000001b3ull;
	}
}

static void dig_byte(unsigned b)
{
	unsigned char c = (unsigned char)b;

	dig(&c, 1);
}

static void sweep_buf_op(int op, const char *s, void (*fn)(char *))
{
	unsigned char b[SWEEP_BUF];

	memset(b, POISON, sizeof b);
	memcpy(b, s, strlen(s) + 1);
	prlen = 0;
	fn((char *)b);
	dig_byte((unsigned)op);
	dig(b, sizeof b);
	dig_byte(0xfe);
	dig(prlog, prlen);
	dig_byte(0xff);
}

static void sw_rm_a(char *b) { Info_RemoveKey(b, "a"); }
static void sw_rm_empty(char *b) { Info_RemoveKey(b, ""); }
static void sw_rmp_star(char *b) { Info_RemovePrefixedKeys(b, '*'); }
static void sw_set_s(char *b) { Info_SetValueForStarKey(b, "a", "b", 16); }
static void sw_set_k(char *b) { Info_SetValueForKey(b, "*a", "b", 16); }
static void sw_set_hc0(char *b)
{
	sv_highchars.integer = 0;
	Info_SetValueForStarKey(b, "b", "\x80x\x01y", 12);
	sv_highchars.integer = 1;
}
static void sw_set_empty(char *b) { Info_SetValueForStarKey(b, "a", "", 16); }

static void sweep(void)
{
	static const char alphabet[4] = { '\\', 'a', 'b', '*' };
	char s[SWEEP_MAXLEN + 1];
	unsigned long total = 0, idx;
	size_t len, i;

	sv_highchars.integer = 1;

	for (len = 0; len <= SWEEP_MAXLEN; ++len) {
		unsigned long count = 1;

		for (i = 0; i < len; ++i)
			count *= 4;
		for (idx = 0; idx < count; ++idx) {
			unsigned long v = idx;
			const char *p;

			for (i = len; i-- > 0;) {
				s[i] = alphabet[v % 4];
				v /= 4;
			}
			s[len] = 0;
			total++;

			dig_byte(1);
			p = Info_ValueForKey(s, "a");
			dig(p, strlen(p) + 1);
			dig_byte(2);
			p = Info_ValueForKey(s, "");
			dig(p, strlen(p) + 1);
			dig_byte(3);
			p = Info_ValueForKey(s, "*");
			dig(p, strlen(p) + 1);
			sweep_buf_op(4, s, sw_rm_a);
			sweep_buf_op(5, s, sw_rm_empty);
			sweep_buf_op(6, s, sw_rmp_star);
			sweep_buf_op(7, s, sw_set_s);
			sweep_buf_op(8, s, sw_set_k);
			sweep_buf_op(9, s, sw_set_hc0);
			sweep_buf_op(10, s, sw_set_empty);
			prlen = 0;
			Info_Print(s);
			dig_byte(11);
			dig(prlog, prlen);
			dig_byte(0xff);
		}
	}
	printf("sweep %d %lu %016llx\n", SWEEP_MAXLEN, total,
		(unsigned long long)fnv);
}

int main(void)
{
	puts("# info_str fixture: frozen output of engine/hexenworld/shared/info_str.c");
	puts("# compiled as hwsv does (-DH2W -DSERVERONLY).  Do not edit by hand:");
	puts("# bash engine/rust/info_str/tests/gen_fixtures.sh   (--check to audit)");
	puts("# Format: tests/fixtures/README.md");

	sv_highchars.integer = 1;	// the brace initialiser leaves .integer 0
	lit[0] = Info_ValueForKey("", "x");
	lit[1] = Info_ValueForKey("\\a\\1", "x");

	directed_lookups();
	random_lookups(24);
	directed_rm();
	random_rm(110);
	directed_rmp();
	random_rmp(70);
	directed_set();
	random_set(220);
	directed_print();
	random_print(40);
	sweep();
	return 0;
}
