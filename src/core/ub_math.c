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

f32 ub_atan2f(f32 y, f32 x)
{
    f32 ax = ub_absf(x), ay = ub_absf(y);
    if (ax < 1e-30f && ay < 1e-30f) return 0.0f;
    /* atan on [0, 1] (Abramowitz & Stegun 4.4.49, error <= 1e-5), then
     * unfold the octants */
    f32 a = ax > ay ? ay / ax : ax / ay;
    f32 s = a * a;
    f32 r = a * (0.9998660f + s * (-0.3302995f + s * (0.1801410f + s * (-0.0851330f + s * 0.0208351f))));
    if (ay > ax) r = UB_HALF_PI - r;
    if (x < 0) r = UB_PI - r;
    if (y < 0) r = -r;
    return r;
}

#include "core/ub_vec.h"

mat3 mat3_from_euler(f32 yaw, f32 pitch, f32 roll)
{
    f32 sy = ub_sinf(yaw), cy = ub_cosf(yaw);
    f32 sp = ub_sinf(pitch), cp = ub_cosf(pitch);
    f32 sr = ub_sinf(roll), cr = ub_cosf(roll);
    /* Columns of Ry(yaw) * Rx(pitch) * Rz(roll), where
     *   Rz maps X -> (cr, sr, 0), Y -> (-sr, cr, 0)
     *   Rx maps Y -> (0, cp, -sp), Z -> (0, sp, cp)   (pitch tilts +Z up)
     *   Ry maps X -> (cy, 0, -sy), Z -> (sy, 0, cy)   (yaw turns +Z to +X)
     * Forward (local +Z) is (sy*cp, sp, cy*cp), matching the camera. */
    mat3 m;
    m.x = v3(cy * cr - sy * sp * sr, cp * sr, -sy * cr - cy * sp * sr);
    m.y = v3(-cy * sr - sy * sp * cr, cp * cr, sy * sr - cy * sp * cr);
    m.z = v3(sy * cp, sp, cy * cp);
    return m;
}
