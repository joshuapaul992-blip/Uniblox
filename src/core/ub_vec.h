/* Uniblox - 3D vectors and rotation matrices.
 *
 * Coordinate system (left-handed, same as the camera):
 *   +X right, +Y up, +Z forward (away from the viewer at yaw 0).
 * mat3 stores the three local axes as columns expressed in world space,
 * so world = pos + m.x * local.x + m.y * local.y + m.z * local.z. */
#ifndef UB_VEC_H
#define UB_VEC_H

#include "core/ub_math.h"

typedef struct {
    f32 x, y, z;
} vec3;

typedef struct {
    vec3 x, y, z; /* columns: where local X, Y, Z point in world space */
} mat3;

/* There is no libm, and GCC only inlines sqrtf as the x87 fsqrt
 * instruction under unsafe-math flags (because of double rounding through
 * 80-bit precision, which is harmless here), so use the instruction
 * directly on 32-bit x86. Host builds (unit tests) use the builtin. */
static inline f32 ub_sqrtf(f32 x)
{
#if defined(__i386__)
    f32 r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
#else
    return __builtin_sqrtf(x);
#endif
}

static inline vec3 v3(f32 x, f32 y, f32 z) { vec3 r = { x, y, z }; return r; }
static inline vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3_scale(vec3 a, f32 s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline vec3 v3_mul(vec3 a, vec3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline f32 v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3_cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline f32 v3_length(vec3 a) { return ub_sqrtf(v3_dot(a, a)); }
static inline vec3 v3_normalize(vec3 a)
{
    f32 len = v3_length(a);
    return len > 1e-12f ? v3_scale(a, 1.0f / len) : v3(0, 0, 0);
}

static inline mat3 mat3_identity(void)
{
    mat3 m = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    return m;
}

/* local -> world direction */
static inline vec3 mat3_apply(const mat3 *m, vec3 v)
{
    return v3(m->x.x * v.x + m->y.x * v.y + m->z.x * v.z,
              m->x.y * v.x + m->y.y * v.y + m->z.y * v.z,
              m->x.z * v.x + m->y.z * v.y + m->z.z * v.z);
}

/* Rotation: roll about Z, then pitch about X, then yaw about Y.
 * Positive yaw turns +Z towards +X; positive pitch tilts +Z upwards. */
mat3 mat3_from_euler(f32 yaw, f32 pitch, f32 roll);

#endif
