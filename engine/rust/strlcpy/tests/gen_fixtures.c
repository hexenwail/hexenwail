// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fixture generator for the strlcpy replay.  Links the C original
// (common/strlcpy.c, unmodified) and prints every expected value from it, so
// tests/fixtures/strlcpy.txt can be audited against C at any time:
//
//   bash engine/rust/strlcpy/tests/gen_fixtures.sh          # rewrite the fixture
//   bash engine/rust/strlcpy/tests/gen_fixtures.sh --check  # fail if it drifted
//
// Deterministic: the fixed-seed xorshift32 the retired differential harness
// used, no clock, no pid.  Format: see tests/fixtures/README.md.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern size_t q_strlcpy(char *dst, const char *src, size_t siz);

#define CANARY 0xa5
#define MAX_BUF 1024

static uint32_t rng_state = 0x6d2b79f5u;

static uint32_t rng(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rng_state = x;
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

/* One record.  The source buffer is `len` non-zero bytes, a NUL, then `junk`
 * more non-zero bytes after the terminator (which a correct copy never reads
 * into the result).  The destination starts as `siz` canary bytes. */
static void emit(size_t len, size_t siz, size_t junk)
{
	unsigned char src[MAX_BUF], dst[MAX_BUF];
	size_t i, ret, total = len + 1 + junk;

	for (i = 0; i < total; ++i) {
		unsigned char b;
		do {
			b = (unsigned char)rng();
		} while (b == 0);
		src[i] = b;
	}
	src[len] = 0;
	memset(dst, CANARY, sizeof(dst));

	/* C never dereferences dst when siz == 0, so pass NULL there. */
	ret = q_strlcpy(siz == 0 ? NULL : (char *)dst, (const char *)src, siz);

	printf("copy %zu", siz);
	hex(src, total);
	printf(" %zu", ret);
	hex(dst, siz);
	putchar('\n');
}

int main(void)
{
	size_t len, siz, i;
	/* Boundary sizes around a source of length len: room for nothing, for the
	 * terminator only, one short of the string, exact fit, one spare, plenty. */
	static const size_t long_len[] = { 63, 64, 127, 255, 256 };

	puts("# strlcpy fixtures, generated from common/strlcpy.c by tests/gen_fixtures.c.");
	puts("# Do not edit by hand: bash engine/rust/strlcpy/tests/gen_fixtures.sh");
	puts("# copy <siz> <src-bytes|-> <ret> <dst-bytes|->");
	puts("#   src-bytes: len non-zero bytes, a NUL, then optional junk after the NUL (hex)");
	puts("#   dst-bytes: the siz-byte destination after q_strlcpy, which started all a5");
	puts("#   ret: q_strlcpy's return value (strlen of the source)");

	/* Full grid: every source length 0..20 against every size 0..24.  Covers
	 * size 0, size 1, truncation, exact fit (siz == len + 1) and slack. */
	for (len = 0; len <= 20; ++len)
		for (siz = 0; siz <= 24; ++siz)
			emit(len, siz, 0);

	/* Junk after the terminator: the copy must stop at the first NUL. */
	for (len = 0; len <= 12; ++len)
		for (siz = 0; siz <= 16; siz += (siz < 3 ? 1 : 5))
			emit(len, siz, 1 + (rng() % 6));

	/* Long sources against sizes around the boundary, and sizes far smaller
	 * and larger than the string. */
	for (i = 0; i < sizeof(long_len) / sizeof(long_len[0]); ++i) {
		len = long_len[i];
		emit(len, 0, 0);
		emit(len, 1, 0);
		emit(len, 2, 0);
		emit(len, len - 1, 0);
		emit(len, len, 0);
		emit(len, len + 1, 0);
		emit(len, len + 2, 0);
		emit(len, len + 32, 3);
		emit(len, len / 2, 5);
	}

	/* Seeded random mix of everything above. */
	for (i = 0; i < 32; ++i) {
		len = rng() % 257;
		siz = rng() % 289;
		emit(len, siz, (rng() & 1) ? rng() % 8 : 0);
	}
	return 0;
}
