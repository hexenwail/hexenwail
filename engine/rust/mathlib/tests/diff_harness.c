/* Minimal mathlib differential harness for Phase 2 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "q_stdinc.h"
#include "model.h"

/* Forward declarations - we assume the C and Rust implementations are linked */
extern int Q_isnan(float x);
extern float anglemod(float a);
extern int GreatestCommonDivisor(int i1, int i2);
extern int NearestMultiple(int num, int mul);
extern int Q_log2(int val);
extern fixed16_t Invert24To16(fixed16_t val);
extern void BOPS_Error(void);
extern void FloorDivMod(double numer, double denom, int *quotient, int *rem);
extern void AngleVectors(vec3_t angles, vec3_t forward, vec3_t right, vec3_t up);
extern int BoxOnPlaneSide(vec3_t emins, vec3_t emaxs, mplane_t *plane);

int main() {
    printf("=== Phase 2 Mathlib Differential Harness (minimal) ===\n");
    
    /* Test basic functionality */
    int test1 = Q_isnan(1.0f);
    printf("Q_isnan(1.0) = %d\n", test1);
    
    float test2 = anglemod(360.0f);
    printf("anglemod(360.0) = %f\n", test2);
    
    int test3 = GreatestCommonDivisor(48, 18);
    printf("GreatestCommonDivisor(48, 18) = %d\n", test3);
    
    int test4 = NearestMultiple(10, 3);
    printf("NearestMultiple(10, 3) = %d\n", test4);
    
    int test5 = Q_log2(16);
    printf("Q_log2(16) = %d\n", test5);
    
    fixed16_t test6 = Invert24To16(1024);
    printf("Invert24To16(1024) = %d\n", test6);
    
    printf("=== Basic harness test completed ===\n");
    return 0;
}
