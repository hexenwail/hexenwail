// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fixture generator for the strlcat replay.  Links the C original
// (common/strlcat.c, unmodified) and prints every expected value from it, so
// tests/fixtures/strlcat.txt can be audited against C at any time:
//
//   bash engine/rust/strlcat/tests/gen_fixtures.sh          # rewrite the fixture
//   bash engine/rust/strlcat/tests/gen_fixtures.sh --check  # fail if it drifted
//
// Deterministic: a fixed-seed xorshift32, no clock, no pid.  Format: see
// tests/fixtures/README.md.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern size_t q_strlcat(char *dst, const char *src, size_t siz);

#define MAX_BUF 1024

static uint32_t rng_state = 0x2f6e2b1du;

static uint32_t rng(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rng_state = x;
}

static unsigned char nonzero(void)
{
	unsigned char b;

	do {
		b = (unsigned char)rng();
	} while (b == 0);
	return b;
}

static void hex(const unsigned char *p, size_t n)
{
	size_t i;

	if (n == 0) {
		fputs(" -", stdout);
		return;
	}
	putchar(' ');
	for (i = 0; i < n; ++i)
		printf("%02x", p[i]);
}

/* One record.  The destination is the `siz` bytes C may look at: `dlen`
 * non-zero bytes then a NUL then non-zero junk (dlen >= siz means every byte is
 * non-zero, so there is no terminator inside siz).  The source is `len`
 * non-zero bytes, a NUL, then `junk` more non-zero bytes after the terminator
 * (which a correct append never reads into the result). */
static void emit(size_t siz, size_t dlen, size_t len, size_t junk)
{
	unsigned char src[MAX_BUF], dst[MAX_BUF], init[MAX_BUF];
	size_t i, ret, total = len + 1 + junk;

	for (i = 0; i < total; ++i)
		src[i] = nonzero();
	src[len] = 0;
	for (i = 0; i < siz; ++i)
		dst[i] = nonzero();
	if (dlen < siz)
		dst[dlen] = 0;
	memcpy(init, dst, siz);

	/* C never dereferences dst when siz == 0, so pass NULL there. */
	ret = q_strlcat(siz == 0 ? NULL : (char *)dst, (const char *)src, siz);

	printf("cat %zu", siz);
	hex(init, siz);
	hex(src, total);
	printf(" %zu", ret);
	hex(dst, siz);
	putchar('\n');
}

int main(void)
{
	size_t len, siz, dlen, i, k;
	static const size_t long_len[] = { 63, 64, 255 };
	static const size_t long_dlen[] = { 0, 10 };

	puts("# strlcat fixtures, generated from common/strlcat.c by tests/gen_fixtures.c.");
	puts("# Do not edit by hand: bash engine/rust/strlcat/tests/gen_fixtures.sh");
	puts("# cat <siz> <dst-in|-> <src-bytes> <ret> <dst-out|->");
	puts("#   dst-in:  the siz-byte destination before the call; non-zero bytes, the");
	puts("#            first NUL (if any) ends the string, bytes after it are junk");
	puts("#   src-bytes: len non-zero bytes, a NUL, then optional junk after the NUL (hex)");
	puts("#   dst-out: the siz-byte destination after q_strlcat");
	puts("#   ret: q_strlcat's return value, strlen(src) + MIN(siz, strlen(dst))");

	/* Full grid: every source length 0..7 against every size 0..11 and every
	 * destination string length 0..siz (dlen == siz: no terminator within siz).
	 * Covers size 0, size 1, empty dst, room for the terminator only, exact fit,
	 * truncation, slack and the no-NUL-in-siz case. */
	for (len = 0; len <= 7; ++len)
		for (siz = 0; siz <= 11; ++siz)
			for (dlen = 0; dlen <= siz; ++dlen)
				emit(siz, dlen, len, 0);

	/* Junk after the source terminator: the append must stop at the first NUL. */
	for (len = 0; len <= 8; ++len) {
		static const size_t sizes[] = { 0, 1, 2, 3, 8, 13 };
		for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
			siz = sizes[i];
			emit(siz, 0, len, 1 + rng() % 6);
			emit(siz, siz / 2, len, 1 + rng() % 6);
			emit(siz, siz ? siz - 1 : 0, len, 1 + rng() % 6);
			emit(siz, siz, len, 1 + rng() % 6);
		}
	}

	/* Long sources against sizes around the boundary of dlen + len. */
	for (i = 0; i < sizeof(long_len) / sizeof(long_len[0]); ++i)
		for (k = 0; k < sizeof(long_dlen) / sizeof(long_dlen[0]); ++k) {
			size_t t;

			len = long_len[i];
			dlen = long_dlen[k];
			t = dlen + len;
			emit(0, dlen, len, 0);
			emit(1, dlen, len, 0);
			emit(dlen + 1, dlen, len, 0);
			emit(t - 1, dlen, len, 0);
			emit(t, dlen, len, 0);
			emit(t + 1, dlen, len, 3);
			emit(t + 33, dlen, len, 0);
		}

	/* Seeded random mix of everything above. */
	for (i = 0; i < 24; ++i) {
		len = rng() % 100;
		siz = rng() % 130;
		dlen = rng() % 90;
		emit(siz, dlen, len, (rng() & 1) ? rng() % 8 : 0);
	}
	return 0;
}
