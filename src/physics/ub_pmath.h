/* Uniblox - extra vector maths for physics: matrices and quaternions. */
#ifndef UB_PMATH_H
#define UB_PMATH_H

#include "core/ub_vec.h"

typedef struct {
    f32 x, y, z, w;
} quat;

static inline f32 v3_length2(vec3 a) { return v3_dot(a, a); }
static inline vec3 v3_neg(vec3 a) { return v3(-a.x, -a.y, -a.z); }
static inline vec3 v3_madd(vec3 a, vec3 b, f32 s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
static inline vec3 v3_min(vec3 a, vec3 b)
{
    return v3(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z);
}
static inline vec3 v3_max(vec3 a, vec3 b)
{
    return v3(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z);
}

/* world -> local direction (rotation matrices are orthonormal) */
static inline vec3 mat3_apply_t(const mat3 *m, vec3 v)
{
    return v3(v3_dot(m->x, v), v3_dot(m->y, v), v3_dot(m->z, v));
}

/* a * b (apply b, then a) */
static inline mat3 mat3_mul(const mat3 *a, const mat3 *b)
{
    mat3 r;
    r.x = mat3_apply(a, b->x);
    r.y = mat3_apply(a, b->y);
    r.z = mat3_apply(a, b->z);
    return r;
}

static inline mat3 mat3_transpose(const mat3 *m)
{
    mat3 r;
    r.x = v3(m->x.x, m->y.x, m->z.x);
    r.y = v3(m->x.y, m->y.y, m->z.y);
    r.z = v3(m->x.z, m->y.z, m->z.z);
    return r;
}

/* aᵀ * b */
static inline mat3 mat3_mul_tn(const mat3 *a, const mat3 *b)
{
    mat3 r;
    r.x = mat3_apply_t(a, b->x);
    r.y = mat3_apply_t(a, b->y);
    r.z = mat3_apply_t(a, b->z);
    return r;
}

static inline mat3 mat3_zero(void)
{
    mat3 m = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
    return m;
}

static inline mat3 mat3_add(const mat3 *a, const mat3 *b)
{
    mat3 r;
    r.x = v3_add(a->x, b->x);
    r.y = v3_add(a->y, b->y);
    r.z = v3_add(a->z, b->z);
    return r;
}

static inline mat3 mat3_scale(const mat3 *a, f32 s)
{
    mat3 r;
    r.x = v3_scale(a->x, s);
    r.y = v3_scale(a->y, s);
    r.z = v3_scale(a->z, s);
    return r;
}

/* Inverse of a general 3x3 matrix; the zero matrix if singular. */
static inline mat3 mat3_inverse(const mat3 *m)
{
    vec3 r0 = v3_cross(m->y, m->z), r1 = v3_cross(m->z, m->x), r2 = v3_cross(m->x, m->y);
    f32 det = v3_dot(m->x, r0);
    if (det > -1e-20f && det < 1e-20f) return mat3_zero();
    f32 inv = 1.0f / det;
    /* rows of the inverse are r0, r1, r2 (scaled); store as columns */
    mat3 t;
    t.x = v3_scale(r0, inv);
    t.y = v3_scale(r1, inv);
    t.z = v3_scale(r2, inv);
    return mat3_transpose(&t);
}

/* R * M * Rᵀ */
static inline mat3 mat3_rotate_tensor(const mat3 *r, const mat3 *m)
{
    mat3 rm = mat3_mul(r, m);
    mat3 rt = mat3_transpose(r);
    return mat3_mul(&rm, &rt);
}

static inline quat quat_identity(void)
{
    quat q = { 0, 0, 0, 1 };
    return q;
}

static inline quat quat_mul(quat a, quat b)
{
    quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

static inline quat quat_normalize(quat q)
{
    f32 n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (n < 1e-20f) return quat_identity();
    f32 s = 1.0f / ub_sqrtf(n);
    q.x *= s;
    q.y *= s;
    q.z *= s;
    q.w *= s;
    return q;
}

static inline mat3 quat_to_mat3(quat q)
{
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    mat3 m;
    m.x = v3(1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy));
    m.y = v3(2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx));
    m.z = v3(2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy));
    return m;
}

/* From a rotation matrix (columns = axes). Re-orthonormalises first, since
 * matrices from files are only approximately orthonormal. */
static inline quat quat_from_mat3(const mat3 *in)
{
    vec3 x = v3_normalize(in->x);
    vec3 z = v3_normalize(v3_cross(x, in->y));
    vec3 y = v3_cross(z, x);
    if (v3_length2(x) < 0.5f || v3_length2(z) < 0.5f) return quat_identity();
    f32 m00 = x.x, m11 = y.y, m22 = z.z;
    /* m[row][col]: column vectors x, y, z */
    f32 tr = m00 + m11 + m22;
    quat q;
    if (tr > 0) {
        f32 s = ub_sqrtf(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (y.z - z.y) / s;
        q.y = (z.x - x.z) / s;
        q.z = (x.y - y.x) / s;
    } else if (m00 > m11 && m00 > m22) {
        f32 s = ub_sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (y.z - z.y) / s;
        q.x = 0.25f * s;
        q.y = (y.x + x.y) / s;
        q.z = (z.x + x.z) / s;
    } else if (m11 > m22) {
        f32 s = ub_sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (z.x - x.z) / s;
        q.x = (y.x + x.y) / s;
        q.y = 0.25f * s;
        q.z = (z.y + y.z) / s;
    } else {
        f32 s = ub_sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (x.y - y.x) / s;
        q.x = (z.x + x.z) / s;
        q.y = (z.y + y.z) / s;
        q.z = 0.25f * s;
    }
    return quat_normalize(q);
}

/* q advanced by angular velocity w (world space) over dt. */
static inline quat quat_integrate(quat q, vec3 w, f32 dt)
{
    quat d = { w.x * dt * 0.5f, w.y * dt * 0.5f, w.z * dt * 0.5f, 0 };
    quat dq = quat_mul(d, q);
    q.x += dq.x;
    q.y += dq.y;
    q.z += dq.z;
    q.w += dq.w;
    return quat_normalize(q);
}

/* Normalised linear interpolation along the shorter way round. */
static inline quat quat_nlerp(quat a, quat b, f32 t)
{
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) {
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        b.w = -b.w;
    }
    quat r = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    return quat_normalize(r);
}

/* Angular velocity that turns `from` into `to` in dt. */
static inline vec3 quat_angular_velocity(quat from, quat to, f32 dt)
{
    quat inv = { -from.x, -from.y, -from.z, from.w };
    quat d = quat_mul(to, inv);
    if (d.w < 0) {
        d.x = -d.x;
        d.y = -d.y;
        d.z = -d.z;
        d.w = -d.w;
    }
    /* small-angle: axis * angle ~= 2 * (x, y, z) */
    return v3(d.x * 2.0f / dt, d.y * 2.0f / dt, d.z * 2.0f / dt);
}

#endif
