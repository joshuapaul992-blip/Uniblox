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
/* Angle of the vector (x, y) from the +x axis, in [-pi, pi]; error < 2e-5. */
f32 ub_atan2f(f32 y, f32 x);

/* Wrap an angle into [-pi, pi]. */
f32 ub_wrap_angle(f32 x);

static inline f32 ub_absf(f32 x) { return x < 0 ? -x : x; }
static inline f32 ub_clampf(f32 x, f32 lo, f32 hi) { return x < lo ? lo : x > hi ? hi : x; }

/* Float to integer, rounded to nearest (halves to even), for |x| < 2^31;
 * as u32 for 0 <= x < 2^32. Adding 1.5 * 2^52 leaves the integer in the
 * low bits of the double. A C cast truncates, and on the x87 (Pentium
 * code) that switches the rounding mode and back each time: two fldcw,
 * which stall a modern processor's pipeline. */
static inline i32 ub_rint(f64 x)
{
    union { f64 d; u32 w[2]; } u;
    u.d = x + 6755399441055744.0;
    return (i32)u.w[0]; /* (little-endian: the low word first) */
}
static inline u32 ub_rintu(f64 x)
{
    union { f64 d; u32 w[2]; } u;
    u.d = x + 6755399441055744.0;
    return u.w[0];
}

/* Exact floor and ceiling for |x| < 2^31 (without a truncating cast). */
static inline i32 ub_floorf(f32 x) { i32 i = ub_rint(x); return (x < (f32)i) ? i - 1 : i; }
static inline i32 ub_ceilf(f32 x) { i32 i = ub_rint(x); return (x > (f32)i) ? i + 1 : i; }

/* Round to nearest integer, halves away from zero. */
static inline i32 ub_roundf(f32 x) { return (i32)(x < 0 ? x - 0.5f : x + 0.5f); }

#endif
