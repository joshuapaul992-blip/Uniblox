#include "core/ub_math.h"

f32 ub_wrap_angle(f32 x)
{
    /* Large inputs lose precision anyway; keep the loop count bounded by
     * reducing with one multiply first. */
    f32 k = (f32)ub_roundf(x * (1.0f / UB_TWO_PI));
    x -= k * UB_TWO_PI;
    if (x > UB_PI) x -= UB_TWO_PI;
    if (x < -UB_PI) x += UB_TWO_PI;
    return x;
}

f32 ub_sinf(f32 x)
{
    x = ub_wrap_angle(x);
    /* Fold into [-pi/2, pi/2] where the Taylor series converges fast. */
    if (x > UB_HALF_PI) x = UB_PI - x;
    else if (x < -UB_HALF_PI) x = -UB_PI - x;
    f32 x2 = x * x;
    /* x - x^3/3! + x^5/5! - x^7/7! + x^9/9!, error < 4e-6 on this range */
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f +
           x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

f32 ub_cosf(f32 x)
{
    return ub_sinf(x + UB_HALF_PI);
}
