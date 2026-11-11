/* Uniblox - bytes for the network (see ub_netbuf.h). */
#include "net/ub_netbuf.h"
#include "core/ub_math.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

void ub_wbuf_init(ub_wbuf *b, u8 *fixed, u32 cap)
{
    b->p = fixed;
    b->len = 0;
    b->cap = fixed ? cap : 0;
    b->err = false;
    b->fixed = fixed != NULL;
}

void ub_wbuf_free(ub_wbuf *b)
{
    if (!b->fixed) plat_free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static bool room(ub_wbuf *b, u32 n)
{
    if (b->err) return false;
    if (b->len + n <= b->cap) return true;
    if (b->fixed || b->len + n < b->len) {
        b->err = true;
        return false;
    }
    u32 nc = b->cap ? b->cap : 256;
    while (nc < b->len + n) nc *= 2;
    u8 *np = (u8 *)plat_alloc(nc);
    if (!np) {
        b->err = true;
        return false;
    }
    if (b->len) memcpy(np, b->p, b->len);
    plat_free(b->p);
    b->p = np;
    b->cap = nc;
    return true;
}

void ub_w_bytes(ub_wbuf *b, const void *d, u32 n)
{
    if (!n || !room(b, n)) return;
    memcpy(b->p + b->len, d, n);
    b->len += n;
}

void ub_w_u8(ub_wbuf *b, u8 v)
{
    if (room(b, 1)) b->p[b->len++] = v;
}

void ub_w_u16(ub_wbuf *b, u16 v)
{
    if (!room(b, 2)) return;
    b->p[b->len++] = (u8)v;
    b->p[b->len++] = (u8)(v >> 8);
}

void ub_w_u32(ub_wbuf *b, u32 v)
{
    if (!room(b, 4)) return;
    for (int i = 0; i < 4; ++i) b->p[b->len++] = (u8)(v >> (8 * i));
}

void ub_w_f32(ub_wbuf *b, f32 v)
{
    u32 u;
    memcpy(&u, &v, 4);
    ub_w_u32(b, u);
}

void ub_w_f64(ub_wbuf *b, f64 v)
{
    u32 u[2];
    memcpy(u, &v, 8);
    ub_w_u32(b, u[0]);
    ub_w_u32(b, u[1]);
}

void ub_w_varu(ub_wbuf *b, u32 v)
{
    while (v >= 0x80) {
        ub_w_u8(b, (u8)(v | 0x80));
        v >>= 7;
    }
    ub_w_u8(b, (u8)v);
}

void ub_w_vari(ub_wbuf *b, i32 v) { ub_w_varu(b, ((u32)v << 1) ^ (u32)(v >> 31)); }

void ub_w_str(ub_wbuf *b, const char *s, u32 n)
{
    ub_w_varu(b, n);
    ub_w_bytes(b, s, n);
}

void ub_w_vec3(ub_wbuf *b, vec3 v)
{
    ub_w_f32(b, v.x);
    ub_w_f32(b, v.y);
    ub_w_f32(b, v.z);
}

/* ------------------------------------------------------------ reading */

const u8 *ub_r_span(ub_rbuf *r, u32 n)
{
    if (r->err || n > r->len - r->pos) {
        r->err = true;
        return NULL;
    }
    const u8 *p = r->p + r->pos;
    r->pos += n;
    return p;
}

bool ub_r_bytes(ub_rbuf *r, void *d, u32 n)
{
    const u8 *p = ub_r_span(r, n);
    if (!p) {
        if (n) memset(d, 0, n);
        return false;
    }
    if (n) memcpy(d, p, n);
    return true;
}

u8 ub_r_u8(ub_rbuf *r)
{
    const u8 *p = ub_r_span(r, 1);
    return p ? p[0] : 0;
}

u16 ub_r_u16(ub_rbuf *r)
{
    const u8 *p = ub_r_span(r, 2);
    return p ? (u16)(p[0] | (p[1] << 8)) : 0;
}

u32 ub_r_u32(ub_rbuf *r)
{
    const u8 *p = ub_r_span(r, 4);
    return p ? (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24) : 0;
}

f32 ub_r_f32(ub_rbuf *r)
{
    u32 u = ub_r_u32(r);
    f32 v;
    memcpy(&v, &u, 4);
    return v;
}

f64 ub_r_f64(ub_rbuf *r)
{
    u32 u[2];
    u[0] = ub_r_u32(r);
    u[1] = ub_r_u32(r);
    f64 v;
    memcpy(&v, u, 8);
    return v;
}

u32 ub_r_varu(ub_rbuf *r)
{
    u32 v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        u8 c = ub_r_u8(r);
        if (r->err) return 0;
        if (shift == 28 && c > 15) break; /* more than 32 bits */
        v |= (u32)(c & 0x7F) << shift;
        if (!(c & 0x80)) return v;
    }
    r->err = true;
    return 0;
}

i32 ub_r_vari(ub_rbuf *r)
{
    u32 u = ub_r_varu(r);
    return (i32)(u >> 1) ^ -(i32)(u & 1);
}

const char *ub_r_str(ub_rbuf *r, u32 *n)
{
    u32 len = ub_r_varu(r);
    const u8 *p = ub_r_span(r, len);
    *n = p ? len : 0;
    return (const char *)p;
}

vec3 ub_r_vec3(ub_rbuf *r)
{
    vec3 v;
    v.x = ub_r_f32(r);
    v.y = ub_r_f32(r);
    v.z = ub_r_f32(r);
    return v;
}

/* --------------------------------------------------------- rotations */

void ub_mat3_to_quat(const mat3 *m, f32 q[4])
{
    /* element (row, column): columns are the axes */
    f32 m00 = m->x.x, m10 = m->x.y, m20 = m->x.z;
    f32 m01 = m->y.x, m11 = m->y.y, m21 = m->y.z;
    f32 m02 = m->z.x, m12 = m->z.y, m22 = m->z.z;
    f32 tr = m00 + m11 + m22;
    if (tr > 0) {
        f32 s = ub_sqrtf(tr + 1.0f) * 2.0f;
        q[0] = 0.25f * s;
        q[1] = (m21 - m12) / s;
        q[2] = (m02 - m20) / s;
        q[3] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        f32 s = ub_sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q[0] = (m21 - m12) / s;
        q[1] = 0.25f * s;
        q[2] = (m01 + m10) / s;
        q[3] = (m02 + m20) / s;
    } else if (m11 > m22) {
        f32 s = ub_sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q[0] = (m02 - m20) / s;
        q[1] = (m01 + m10) / s;
        q[2] = 0.25f * s;
        q[3] = (m12 + m21) / s;
    } else {
        f32 s = ub_sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q[0] = (m10 - m01) / s;
        q[1] = (m02 + m20) / s;
        q[2] = (m12 + m21) / s;
        q[3] = 0.25f * s;
    }
    f32 len = ub_sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len < 1e-6f) {
        q[0] = 1;
        q[1] = q[2] = q[3] = 0;
        return;
    }
    for (int i = 0; i < 4; ++i) q[i] /= len;
}

mat3 ub_quat_to_mat3(const f32 q[4])
{
    f32 w = q[0], x = q[1], y = q[2], z = q[3];
    mat3 m;
    m.x = v3(1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y));
    m.y = v3(2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x));
    m.z = v3(2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y));
    return m;
}

/* The largest part is left out (it follows from the others, as the
 * quaternion has length 1) and its sign made positive; the other three,
 * each within +-1/sqrt(2), get 15 bits; 2 bits say which was left out. */
#define QSCALE 23169.0f /* 32767 * sqrt(2) / 2 */

void ub_w_rot(ub_wbuf *b, const mat3 *m)
{
    f32 q[4];
    ub_mat3_to_quat(m, q);
    int big = 0;
    for (int i = 1; i < 4; ++i)
        if (ub_absf(q[i]) > ub_absf(q[big])) big = i;
    f32 sign = q[big] < 0 ? -1.0f : 1.0f;
    u16 v[3];
    for (int i = 0, k = 0; i < 4; ++i) {
        if (i == big) continue;
        i32 n = ub_rint(q[i] * sign * QSCALE) + 32768;
        v[k++] = (u16)UB_CLAMP(n, 0, 65535);
    }
    /* the index rides in the low bits of the first two */
    v[0] = (u16)((v[0] & ~1u) | (u16)(big & 1));
    v[1] = (u16)((v[1] & ~1u) | (u16)(big >> 1));
    ub_w_u16(b, v[0]);
    ub_w_u16(b, v[1]);
    ub_w_u16(b, v[2]);
}

mat3 ub_r_rot(ub_rbuf *r)
{
    u16 v[3];
    v[0] = ub_r_u16(r);
    v[1] = ub_r_u16(r);
    v[2] = ub_r_u16(r);
    int big = (v[0] & 1) | ((v[1] & 1) << 1);
    f32 q[4], sum = 0;
    for (int i = 0, k = 0; i < 4; ++i) {
        if (i == big) continue;
        q[i] = (f32)((i32)v[k++] - 32768) / QSCALE;
        sum += q[i] * q[i];
    }
    q[big] = sum < 1.0f ? ub_sqrtf(1.0f - sum) : 0.0f;
    return ub_quat_to_mat3(q);
}

/* 4 bytes: the index in 2 bits, the three smaller parts in 10 bits each
 * (within about 0.0015 of the real value: fine for something moving). */
#define QSCALE10 (511.0f / 0.70710678f)

void ub_w_rot32(ub_wbuf *b, const mat3 *m)
{
    f32 q[4];
    ub_mat3_to_quat(m, q);
    int big = 0;
    for (int i = 1; i < 4; ++i)
        if (ub_absf(q[i]) > ub_absf(q[big])) big = i;
    f32 sign = q[big] < 0 ? -1.0f : 1.0f;
    u32 v = (u32)big;
    for (int i = 0, k = 0; i < 4; ++i) {
        if (i == big) continue;
        i32 n = ub_rint(q[i] * sign * QSCALE10) + 512;
        v |= (u32)UB_CLAMP(n, 0, 1023) << (2 + 10 * k++);
    }
    ub_w_u32(b, v);
}

mat3 ub_r_rot32(ub_rbuf *r)
{
    u32 v = ub_r_u32(r);
    int big = (int)(v & 3);
    f32 q[4], sum = 0;
    for (int i = 0, k = 0; i < 4; ++i) {
        if (i == big) continue;
        q[i] = (f32)((i32)((v >> (2 + 10 * k++)) & 1023) - 512) / QSCALE10;
        sum += q[i] * q[i];
    }
    q[big] = sum < 1.0f ? ub_sqrtf(1.0f - sum) : 0.0f;
    return ub_quat_to_mat3(q);
}

/* a position to 1/64 stud, each axis as a zigzag varint (3 bytes within 2048 studs) */
void ub_w_pos64(ub_wbuf *b, vec3 p)
{
    const f32 *a = &p.x;
    for (int i = 0; i < 3; ++i) {
        f32 f = a[i] * 64.0f;
        i32 n = f > 2.0e9f ? 2000000000 : f < -2.0e9f ? -2000000000 : ub_rint(f);
        ub_w_vari(b, n);
    }
}

vec3 ub_r_pos64(ub_rbuf *r)
{
    vec3 p;
    p.x = (f32)ub_r_vari(r) / 64.0f;
    p.y = (f32)ub_r_vari(r) / 64.0f;
    p.z = (f32)ub_r_vari(r) / 64.0f;
    return p;
}
