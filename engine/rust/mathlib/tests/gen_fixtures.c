// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fixture generator for the mathlib replay.  Links the C original
// (engine/h2shared/mathlib.c, unmodified) and prints every expected value from
// it, so tests/fixtures/mathlib.txt can be audited against C at any time:
//
//   bash engine/rust/mathlib/tests/gen_fixtures.sh          # rewrite the fixture
//   bash engine/rust/mathlib/tests/gen_fixtures.sh --check  # fail if it drifted
//
// Deterministic: a fixed-seed xorshift32, no clock, no pid.  Floats are written
// as the 8-digit hex of their IEEE-754 bit pattern, so the file carries exact
// values and no decimal rounding.  Format: see tests/fixtures/README.md.
//
// Inputs are mostly dyadic rationals (multiples of 1/8) so every product and
// sum in the matrix and plane routines is exact in a float: the expected bits
// do not depend on whether a compiler contracts a*b+c into an fma.  The build
// line in gen_fixtures.sh also passes -ffp-contract=off.

#include "quakedef.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void Sys_Error(const char *fmt, ...)
{
	fprintf(stderr, "Sys_Error: %s\n", fmt);
	abort();
}

static uint32_t rng_state = 0x2545f491u;

static uint32_t rng(void)
{
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return rng_state = x;
}

static uint32_t bits(float f)
{
	uint32_t u;
	memcpy(&u, &f, sizeof u);
	return u;
}

static float from_bits(uint32_t u)
{
	float f;
	memcpy(&f, &u, sizeof f);
	return f;
}

static void fl(float f)
{
	printf(" %08x", bits(f));
}

static void fls(const float *p, int n)
{
	int i;

	for (i = 0; i < n; ++i)
		fl(p[i]);
}

/* a multiple of 1/8 in [-range/8, range/8] */
static float dy(int range)
{
	return (float)((int)(rng() % (2 * range + 1)) - range) / 8.0f;
}

static void emit_layout(void)
{
	printf("layout %zu %zu %zu %zu %zu\n", sizeof(mplane_t),
	       offsetof(mplane_t, normal), offsetof(mplane_t, dist),
	       offsetof(mplane_t, type), offsetof(mplane_t, signbits));
}

static void emit_isnan(float f)
{
	printf("isnan %08x %d\n", bits(f), Q_isnan(f));
}

static void emit_anglemod(float f)
{
	printf("anglemod %08x %08x\n", bits(f), bits(anglemod(f)));
}

static void emit_anglevec(const float *ang)
{
	vec3_t f, r, u;

	AngleVectors((float *)ang, f, r, u);
	printf("anglevec");
	fls(ang, 3);
	fls(f, 3);
	fls(r, 3);
	fls(u, 3);
	putchar('\n');
}

static void emit_rot(float a[3][3], float b[3][3])
{
	float o[3][3];

	R_ConcatRotations(a, b, o);
	printf("rot");
	fls(&a[0][0], 9);
	fls(&b[0][0], 9);
	fls(&o[0][0], 9);
	putchar('\n');
}

static void emit_xform(float a[3][4], float b[3][4])
{
	float o[3][4];

	R_ConcatTransforms(a, b, o);
	printf("xform");
	fls(&a[0][0], 12);
	fls(&b[0][0], 12);
	fls(&o[0][0], 12);
	putchar('\n');
}

static void emit_bops(const float *n, float dist, const float *mins,
                      const float *maxs, int signbits)
{
	mplane_t p;
	vec3_t mn, mx;

	memset(&p, 0, sizeof p);
	memcpy(p.normal, n, sizeof p.normal);
	p.dist = dist;
	p.signbits = (byte)signbits;
	memcpy(mn, mins, sizeof mn);
	memcpy(mx, maxs, sizeof mx);
	printf("bops %d", signbits);
	fls(n, 3);
	fl(dist);
	fls(mins, 3);
	fls(maxs, 3);
	printf(" %d\n", BoxOnPlaneSide(mn, mx, &p));
}

static void emit_fdm(int numer, int denom)
{
	int q, r;

	FloorDivMod((double)numer, (double)denom, &q, &r);
	printf("fdm %d %d %d %d\n", numer, denom, q, r);
}

int main(void)
{
	static const float angs[] = {-720.5f, -1.0f, -0.0f, 0.0f, 0.5f, 1.0f, 89.9f,
	                             90.0f, 180.0f, 359.9f, 360.0f, 721.0f};
	static const int na = sizeof angs / sizeof angs[0];
	static const int ints[] = {0, 1, 2, 3, 7, 16, 255, 65536};
	static const int ni = sizeof ints / sizeof ints[0];
	/* bit patterns: +-0, denormals, 1.0, max normal, +-inf, quiet and
	 * signalling NaNs of both signs, the NaN payload edges. */
	static const uint32_t nanbits[] = {
		0x00000000, 0x80000000, 0x00000001, 0x007fffff, 0x00800000,
		0x3f800000, 0x7f7fffff, 0x7f800000, 0xff800000, 0x7f800001,
		0x7fc00000, 0x7fffffff, 0xff800001, 0xffc00000, 0xffffffff,
		0x7fbfffff, 0x7f80ffff, 0x7fa00000
	};
	int i, j, k;

	printf("# mathlib fixtures: frozen output of engine/h2shared/mathlib.c\n"
	       "# regenerate: bash engine/rust/mathlib/tests/gen_fixtures.sh\n"
	       "# format: tests/fixtures/README.md\n");

	emit_layout();

	/* Q_isnan: directed bit patterns, then seeded random patterns */
	for (i = 0; i < (int)(sizeof nanbits / sizeof nanbits[0]); ++i)
		emit_isnan(from_bits(nanbits[i]));
	for (i = 0; i < 24; ++i)
		emit_isnan(from_bits(rng()));
	for (i = 0; i < 8; ++i)	/* every NaN exponent with a random payload */
		emit_isnan(from_bits(0x7f800000u | ((rng() & 0x7fffff) | 1)));

	/* anglemod: directed, wrap boundaries, then seeded */
	for (i = 0; i < na; ++i)
		emit_anglemod(angs[i]);
	{
		static const float more[] = {0.0054931640625f, 0.005f, 0.0055f, 359.994f,
		                             359.99451f, 360.0054f, -0.0054f, -0.0055f,
		                             -360.0f, -359.999f, 1e5f, -1e5f, 32767.5f};
		for (i = 0; i < (int)(sizeof more / sizeof more[0]); ++i)
			emit_anglemod(more[i]);
	}
	for (i = 0; i < 32; ++i)
		emit_anglemod(dy(8000) * 1.0f);
	for (i = 0; i < 16; ++i)
		emit_anglemod((float)(rng() % 7200000) / 1000.0f - 3600.0f);

	/* Q_log2 and Invert24To16: powers of two and neighbours, boundaries */
	for (i = 0; i < ni; ++i)
		printf("log2 %d %d\n", ints[i], Q_log2(ints[i]));
	for (i = 1; i < 31; ++i) {
		int v = 1 << i;

		printf("log2 %d %d\n", v, Q_log2(v));
		printf("log2 %d %d\n", v - 1, Q_log2(v - 1));
		printf("log2 %d %d\n", v + 1, Q_log2(v + 1));
	}
	printf("log2 %d %d\n", 0x7fffffff, Q_log2(0x7fffffff));
	{
		/* 256..512 are left out on purpose: 2^40/val does not fit an int
		 * there, and C's double-to-int conversion of it is undefined
		 * (x86 gives INT_MIN, Rust saturates to INT_MAX).  513 is the
		 * first value whose result fits. */
		static const int inv[] = {-1000, -1, 0, 1, 254, 255, 513, 514, 1000,
		                          65535, 65536, 65537, 0x01000000, 0x01000001,
		                          0x7fffffff, 3, 7, 12345, 99999999};
		for (i = 0; i < (int)(sizeof inv / sizeof inv[0]); ++i)
			printf("inv %d %d\n", inv[i], Invert24To16(inv[i]));
		for (i = 0; i < 24; ++i) {
			int v = (int)(rng() >> (rng() % 24 + 1)) + 513;

			printf("inv %d %d\n", v, Invert24To16(v));
		}
	}

	/* GreatestCommonDivisor / NearestMultiple: grid, then seeded pairs */
	for (i = 0; i < ni; ++i)
		for (j = 0; j < ni; ++j) {
			printf("gcd %d %d %d\n", ints[i], ints[j],
			       GreatestCommonDivisor(ints[i], ints[j]));
			if (ints[j] > 0)
				printf("nearest %d %d %d\n", ints[i], ints[j],
				       NearestMultiple(ints[i], ints[j]));
		}
	for (i = 0; i < 24; ++i) {
		int a = (int)(rng() % 100000), b = (int)(rng() % 3000) + 1;

		printf("gcd %d %d %d\n", a, b, GreatestCommonDivisor(a, b));
		printf("gcd %d %d %d\n", b, a, GreatestCommonDivisor(b, a));
	}
	/* NearestMultiple on negatives: the (num - mul + 1) branch the retired
	 * harness never reached, and the zero boundary */
	for (i = -20; i <= 20; ++i) {
		static const int muls[] = {1, 2, 3, 5, 8};

		for (j = 0; j < 5; ++j)
			printf("nearest %d %d %d\n", i, muls[j],
			       NearestMultiple(i, muls[j]));
	}
	for (i = 0; i < 16; ++i) {
		int a = -(int)(rng() % 100000) - 1, b = (int)(rng() % 500) + 1;

		printf("nearest %d %d %d\n", a, b, NearestMultiple(a, b));
	}

	/* AngleVectors: the harness's rotating 12-value triples, then every
	 * axis-aligned and diagonal angle, then seeded */
	for (i = 0; i < na; ++i) {
		float a[3] = {angs[i], angs[(i + 3) % na], angs[(i + 7) % na]};

		emit_anglevec(a);
	}
	for (i = 0; i < 8; ++i) {
		float a[3] = {(float)(45 * (i & 1)) * 2, (float)(45 * (i >> 1 & 1)) * 2,
		              (float)(45 * (i >> 2 & 1)) * 2};

		emit_anglevec(a);
	}
	for (i = 0; i < 4; ++i)
		for (j = 0; j < 4; ++j) {
			float a[3] = {(float)(90 * i), (float)(90 * j), 0.0f};

			emit_anglevec(a);
			a[2] = 30.0f * (i + 1);
			emit_anglevec(a);
		}
	for (i = 0; i < 80; ++i) {
		float a[3];

		for (k = 0; k < 3; ++k)
			a[k] = (float)(rng() % 7200) / 10.0f - 360.0f;
		emit_anglevec(a);
	}

	/* R_ConcatRotations / R_ConcatTransforms: the harness's matrices (so its
	 * fixed case is kept), identity, then seeded */
	{
		float a[3][3] = {{1,2,3},{-4,5,6},{7,8,-9}}, b[3][3] = {{2,-1,0},{3,4,5},{6,7,8}};
		float id[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
		float ta[3][4] = {{1,2,3,4},{-4,5,6,7},{8,9,-10,11}};
		float tb[3][4] = {{2,-1,0,3},{3,4,5,6},{6,7,8,9}};
		float tid[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};

		emit_rot(a, b);
		emit_rot(b, a);
		emit_rot(a, id);
		emit_rot(id, b);
		emit_xform(ta, tb);
		emit_xform(tb, ta);
		emit_xform(ta, tid);
		emit_xform(tid, tb);
	}
	for (i = 0; i < 40; ++i) {
		float a[3][3], b[3][3];

		for (j = 0; j < 3; ++j)
			for (k = 0; k < 3; ++k) {
				a[j][k] = dy(40);
				b[j][k] = dy(40);
			}
		emit_rot(a, b);
	}
	for (i = 0; i < 40; ++i) {
		float a[3][4], b[3][4];

		for (j = 0; j < 3; ++j)
			for (k = 0; k < 4; ++k) {
				a[j][k] = dy(40);
				b[j][k] = dy(40);
			}
		emit_xform(a, b);
	}
	/* non-dyadic values: rounding is part of the contract too */
	{
		float a[3][3] = {{0.1f,0.2f,0.3f},{-0.7f,0.5f,0.11f},{1.3f,-2.9f,0.01f}};
		float b[3][3] = {{0.3f,-0.1f,0.9f},{0.33f,0.4f,-0.5f},{0.6f,0.07f,0.8f}};
		float ta[3][4] = {{0.1f,0.2f,0.3f,0.4f},{-0.7f,0.5f,0.11f,0.7f},{1.3f,-2.9f,0.01f,1.1f}};
		float tb[3][4] = {{0.3f,-0.1f,0.9f,0.3f},{0.33f,0.4f,-0.5f,0.6f},{0.6f,0.07f,0.8f,0.9f}};

		emit_rot(a, b);
		emit_xform(ta, tb);
	}

	/* BoxOnPlaneSide: every signbits value (0..7) against seeded boxes and
	 * planes; and the exact-boundary cases dist1 == dist, dist2 == dist */
	for (i = 0; i < 8; ++i) {
		for (j = 0; j < 14; ++j) {
			float n[3], mn[3], mx[3], d;

			for (k = 0; k < 3; ++k) {
				float lo = dy(80), hi = dy(80);

				n[k] = dy(24);
				mn[k] = lo < hi ? lo : hi;
				mx[k] = lo < hi ? hi : lo;
			}
			d = dy(160);
			emit_bops(n, d, mn, mx, i);
		}
	}
	{
		/* box (-3,-2,-1)..(5,7,11) against plane (1.25,-2,0.75): the harness's
		 * fixed case, for every signbits value */
		float n[3] = {1.25f, -2.0f, 0.75f}, mn[3] = {-3.0f, -2.0f, -1.0f}, mx[3] = {5.0f, 7.0f, 11.0f};

		for (i = 0; i < 8; ++i)
			emit_bops(n, 2.0f, mn, mx, i);
	}
	{
		/* planes sitting exactly on a corner of the unit box.  signbits 0
		 * means dist1 = n.max, dist2 = n.min. */
		float n[3] = {1.0f, 2.0f, 4.0f}, mn[3] = {0.0f, 0.0f, 0.0f}, mx[3] = {1.0f, 1.0f, 1.0f};
		static const float ds[] = {-0.5f, 0.0f, 0.5f, 6.5f, 7.0f, 7.5f};

		for (i = 0; i < 6; ++i)
			emit_bops(n, ds[i], mn, mx, 0);
		/* all-negative normal: signbits 7, dist1 = n.min, dist2 = n.max */
		n[0] = -1.0f; n[1] = -2.0f; n[2] = -4.0f;
		for (i = 0; i < 6; ++i)
			emit_bops(n, -ds[i], mn, mx, 7);
	}

	/* FloorDivMod: both branches, exact multiples (remainder 0, the negative
	 * branch that does not adjust), zero, and 32-bit extremes */
	for (i = 0; i < na; ++i)
		emit_fdm((int)angs[i], 7);
	for (i = -24; i <= 24; ++i) {
		emit_fdm(i, 1);
		emit_fdm(i, 5);
		emit_fdm(i, 8);
	}
	{
		static const int big[] = {2147483647, -2147483647, 1000000, -1000000, 65536, -65536};

		for (i = 0; i < 6; ++i) {
			emit_fdm(big[i], 3);
			emit_fdm(big[i], 65536);
			emit_fdm(big[i], 1);
		}
	}
	for (i = 0; i < 24; ++i)
		emit_fdm((int)(rng() % 2000001) - 1000000, (int)(rng() % 999) + 1);

	return 0;
}
