/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Differential test: mathlib.c (symbols c_*) versus mathlib-rs. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char byte;
typedef float vec_t;
typedef int fixed16_t;
typedef vec_t vec3_t[3];
typedef struct mplane_s {
	vec3_t normal;
	float dist;
	byte type;
	byte signbits;
	byte pad[2];
} mplane_t;

/* The original needs this engine callback only for invalid inputs. */
void Sys_Error(const char *fmt, ...) { (void)fmt; abort(); }
void rust_eh_personality(void) {}

#define DECLARE(ret, name, args) extern ret name args; extern ret c_##name args
DECLARE(int, Q_isnan, (float));
DECLARE(float, anglemod, (float));
DECLARE(void, AngleVectors, (const vec3_t, vec3_t, vec3_t, vec3_t));
DECLARE(void, R_ConcatRotations, (float [3][3], float [3][3], float [3][3]));
DECLARE(void, R_ConcatTransforms, (float [3][4], float [3][4], float [3][4]));
DECLARE(void, FloorDivMod, (double, double, int *, int *));
DECLARE(int, GreatestCommonDivisor, (int, int));
DECLARE(int, NearestMultiple, (int, int));
DECLARE(int, Q_log2, (int));
DECLARE(fixed16_t, Invert24To16, (fixed16_t));
DECLARE(int, BoxOnPlaneSide, (const vec3_t, const vec3_t, mplane_t *));

extern size_t MathlibMPlane_sizeof(void);
extern size_t MathlibMPlane_offsetof_normal(void);
extern size_t MathlibMPlane_offsetof_dist(void);
extern size_t MathlibMPlane_offsetof_type(void);
extern size_t MathlibMPlane_offsetof_signbits(void);

// Comparisons actually made.  The gate floors this rather than pinning it, so
// that a regression which empties an enumeration cannot exit 0 and still read
// as green.
static unsigned long cases;
static int failures;
static int feq(float a, float b) { return a == b || (isnan(a) && isnan(b)) || fabsf(a - b) <= 1e-6f; }
static void check(int ok, const char *what) { ++cases; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }
static int veq(const float *a, const float *b, size_t n) { while (n--) if (!feq(a[n], b[n])) return 0; return 1; }

int main(void)
{
	static const float fs[] = {-720.5f, -1.0f, -0.0f, 0.0f, 0.5f, 1.0f, 89.9f, 90.0f, 180.0f, 359.9f, 360.0f, 721.0f};
	static const int is[] = {0, 1, 2, 3, 7, 16, 255, 65536};
	vec3_t mins = {-3.0f, -2.0f, -1.0f}, maxs = {5.0f, 7.0f, 11.0f};
	float a[3][3] = {{1,2,3},{-4,5,6},{7,8,-9}}, b[3][3] = {{2,-1,0},{3,4,5},{6,7,8}}, r[3][3], c[3][3];
	float ta[3][4] = {{1,2,3,4},{-4,5,6,7},{8,9,-10,11}}, tb[3][4] = {{2,-1,0,3},{3,4,5,6},{6,7,8,9}}, tr[3][4], tc[3][4];
	size_t i, j;

	check(MathlibMPlane_sizeof() == sizeof(mplane_t), "mplane sizeof");
	check(MathlibMPlane_offsetof_normal() == offsetof(mplane_t, normal), "mplane normal offset");
	check(MathlibMPlane_offsetof_dist() == offsetof(mplane_t, dist), "mplane dist offset");
	check(MathlibMPlane_offsetof_type() == offsetof(mplane_t, type), "mplane type offset");
	check(MathlibMPlane_offsetof_signbits() == offsetof(mplane_t, signbits), "mplane signbits offset");

	for (i = 0; i < sizeof(fs)/sizeof(fs[0]); ++i) {
		check(Q_isnan(fs[i]) == c_Q_isnan(fs[i]), "Q_isnan");
		check(feq(anglemod(fs[i]), c_anglemod(fs[i])), "anglemod");
	}
	check(Q_isnan(NAN) == c_Q_isnan(NAN), "Q_isnan NaN");
	for (i = 0; i < sizeof(is)/sizeof(is[0]); ++i) {
		check(Q_log2(is[i]) == c_Q_log2(is[i]), "Q_log2");
		check(Invert24To16(is[i]) == c_Invert24To16(is[i]), "Invert24To16");
		for (j = 1; j < sizeof(is)/sizeof(is[0]); ++j) {
			check(GreatestCommonDivisor(is[i], is[j]) == c_GreatestCommonDivisor(is[i], is[j]), "GreatestCommonDivisor");
			check(NearestMultiple(is[i], is[j]) == c_NearestMultiple(is[i], is[j]), "NearestMultiple");
		}
	}
	for (i = 0; i < sizeof(fs)/sizeof(fs[0]); ++i) {
		vec3_t angles = {fs[i], fs[(i + 3) % (sizeof(fs)/sizeof(fs[0]))], fs[(i + 7) % (sizeof(fs)/sizeof(fs[0]))]};
		vec3_t rf, rr, ru, cf, cr, cu;
		AngleVectors(angles, rf, rr, ru); c_AngleVectors(angles, cf, cr, cu);
		check(veq(rf, cf, 3) && veq(rr, cr, 3) && veq(ru, cu, 3), "AngleVectors");
	}
	R_ConcatRotations(a, b, r); c_R_ConcatRotations(a, b, c);
	check(veq(&r[0][0], &c[0][0], 9), "R_ConcatRotations");
	R_ConcatTransforms(ta, tb, tr); c_R_ConcatTransforms(ta, tb, tc);
	check(veq(&tr[0][0], &tc[0][0], 12), "R_ConcatTransforms");
	for (i = 0; i < 8; ++i) {
		mplane_t p = {{1.25f, -2.0f, 0.75f}, 2.0f, 3, (byte)i, {0,0}};
		check(BoxOnPlaneSide(mins, maxs, &p) == c_BoxOnPlaneSide(mins, maxs, &p), "BoxOnPlaneSide");
	}
	for (i = 0; i < sizeof(fs)/sizeof(fs[0]); ++i) {
		int rq, rr, cq, cr;
		double n = (double)((int)fs[i]);
		FloorDivMod(n, 7.0, &rq, &rr); c_FloorDivMod(n, 7.0, &cq, &cr);
		check(rq == cq && rr == cr, "FloorDivMod");
	}
	if (failures) { fprintf(stderr, "%d differential failures\n", failures); return 1; }
	printf("mathlib-rs differential harness: PASS (%lu cases)\n", cases);
	return 0;
}
