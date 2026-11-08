/* Uniblox - maths helpers. There is no libm, so trig is done here with
 * polynomials. Accuracy is about 1e-5, plenty for games, and the results
 * are identical on every machine (unlike the x87 fsin instruction). */
#ifndef UB_MATH_H
#define UB_MATH_H

#include "core/ub_types.h"

#define UB_PI      3.14159265358979f
#define UB_TWO_PI  6.28318530717959f
#define UB_HALF_PI 1.57079632679490f

f32 ub_sinf(f32 x);
f32 ub_cosf(f32 x);

/* Wrap an angle into [-pi, pi]. */
f32 ub_wrap_angle(f32 x);

static inline f32 ub_absf(f32 x) { return x < 0 ? -x : x; }
static inline f32 ub_clampf(f32 x, f32 lo, f32 hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Round to nearest integer, halves away from zero. */
static inline i32 ub_roundf(f32 x) { return (i32)(x < 0 ? x - 0.5f : x + 0.5f); }

#endif
