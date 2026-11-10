/* Uniblox - joints and forces (milestone 8C): what Roblox's constraints
 * and body movers need. Solved with sequential impulses inside the
 * physics substeps, with a position correction (Baumgarte) term; springs
 * and movers are forces. Joined bodies are kept awake while a motor,
 * spring or mover acts on them. */
#include "physics/ub_physics.h"
#include "core/ub_math.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

#define BETA 0.2f     /* how much of an error is corrected per substep */

static vec3 cross(vec3 a, vec3 b) { return v3_cross(a, b); }

static mat3 skew(vec3 r)
{
    mat3 m;
    m.x = v3(0, r.z, -r.y);
    m.y = v3(-r.z, 0, r.x);
    m.z = v3(r.y, -r.x, 0);
    return m;
}

static f32 inv_m(const ub_body *b) { return b->type == UB_BODY_DYNAMIC && b->awake ? b->inv_mass : 0; }
static mat3 inv_i(const ub_body *b) { return b->type == UB_BODY_DYNAMIC && b->awake ? b->inv_inertia : mat3_zero(); }

static void apply(ub_body *b, vec3 imp, vec3 r, f32 sign)
{
    if (b->type != UB_BODY_DYNAMIC || !b->awake) return;
    b->v = v3_madd(b->v, imp, sign * b->inv_mass);
    b->w = v3_madd(b->w, mat3_apply(&b->inv_inertia, cross(r, imp)), sign);
}

static void apply_ang(ub_body *b, vec3 l, f32 sign)
{
    if (b->type != UB_BODY_DYNAMIC || !b->awake) return;
    b->w = v3_madd(b->w, mat3_apply(&b->inv_inertia, l), sign);
}

/* K = (ma + mb) E - [ra] Ia [ra] - [rb] Ib [rb], for a point between two bodies */
static mat3 point_mass(const ub_body *a, const ub_body *b, vec3 ra, vec3 rb)
{
    f32 m = inv_m(a) + inv_m(b);
    mat3 k = mat3_zero();
    k.x.x = k.y.y = k.z.z = m;
    mat3 sa = skew(ra), sb = skew(rb), ia = inv_i(a), ib = inv_i(b);
    mat3 t = mat3_mul(&sa, &ia);
    t = mat3_mul(&t, &sa);
    mat3 u = mat3_mul(&sb, &ib);
    u = mat3_mul(&u, &sb);
    k = mat3_add(&k, &(mat3){ v3_neg(t.x), v3_neg(t.y), v3_neg(t.z) });
    k = mat3_add(&k, &(mat3){ v3_neg(u.x), v3_neg(u.y), v3_neg(u.z) });
    return k;
}

static vec3 vel_at(const ub_body *b, vec3 r) { return v3_add(b->v, cross(b->w, r)); }

static void wp(const ub_body *b, vec3 l, vec3 *r, vec3 *p)
{
    *r = mat3_apply(&b->rot, l);
    *p = v3_add(b->pos, *r);
}

/* the anchors meet: velocity and position error to zero */
static void solve_point(ub_body *a, ub_body *b, vec3 ra, vec3 rb, vec3 err, f32 h)
{
    mat3 k = point_mass(a, b, ra, rb);
    if (k.x.x + k.y.y + k.z.z < 1e-12f) return;
    mat3 ki = mat3_inverse(&k);
    vec3 dv = v3_sub(vel_at(b, rb), vel_at(a, ra));
    vec3 bias = v3_scale(err, BETA / h);
    vec3 imp = v3_neg(mat3_apply(&ki, v3_add(dv, bias)));
    apply(a, imp, ra, -1);
    apply(b, imp, rb, 1);
}

/* along one direction: impulse lambda with optional bounds */
static f32 solve_dir(ub_body *a, ub_body *b, vec3 ra, vec3 rb, vec3 n, f32 target_v, f32 err, f32 h, f32 lo, f32 hi, f32 *acc)
{
    f32 k = inv_m(a) + inv_m(b) + v3_dot(n, mat3_apply(&a->inv_inertia, cross(cross(ra, n), ra))) * (inv_m(a) > 0) +
            v3_dot(n, mat3_apply(&b->inv_inertia, cross(cross(rb, n), rb))) * (inv_m(b) > 0);
    if (k < 1e-12f) return 0;
    f32 dv = v3_dot(v3_sub(vel_at(b, rb), vel_at(a, ra)), n);
    f32 lam = -(dv - target_v + err * BETA / h) / k;
    f32 old = *acc;
    *acc = ub_clampf(old + lam, lo, hi);
    lam = *acc - old;
    vec3 imp = v3_scale(n, lam);
    apply(a, imp, ra, -1);
    apply(b, imp, rb, 1);
    return lam;
}

/* angular: relative turning to `target` (rad/s) about `axis`, within +-max */
static void solve_twist(ub_body *a, ub_body *b, vec3 axis, f32 target, f32 err, f32 h, f32 maxl, f32 *acc)
{
    mat3 ia = inv_i(a), ib = inv_i(b);
    f32 k = v3_dot(axis, mat3_apply(&ia, axis)) + v3_dot(axis, mat3_apply(&ib, axis));
    if (k < 1e-12f) return;
    f32 dw = v3_dot(v3_sub(b->w, a->w), axis);
    f32 lam = -(dw - target + err * BETA / h) / k;
    f32 old = *acc;
    *acc = maxl > 0 ? ub_clampf(old + lam, -maxl, maxl) : old + lam;
    lam = *acc - old;
    apply_ang(a, v3_scale(axis, lam), -1);
    apply_ang(b, v3_scale(axis, lam), 1);
}

/* keeps all relative turning (or all but about `free_axis`) at zero */
static void solve_lock(ub_body *a, ub_body *b, vec3 err, const vec3 *free_axis, f32 h)
{
    mat3 ia = inv_i(a), ib = inv_i(b), k = mat3_add(&ia, &ib);
    if (k.x.x + k.y.y + k.z.z < 1e-12f) return;
    vec3 dw = v3_add(v3_sub(b->w, a->w), v3_scale(err, BETA / h));
    if (free_axis) dw = v3_sub(dw, v3_scale(*free_axis, v3_dot(dw, *free_axis)));
    mat3 ki = mat3_inverse(&k);
    vec3 l = v3_neg(mat3_apply(&ki, dw));
    if (free_axis) l = v3_sub(l, v3_scale(*free_axis, v3_dot(l, *free_axis)));
    apply_ang(a, l, -1);
    apply_ang(b, l, 1);
}

static f32 hinge_angle(const ub_body *a, const ub_body *b, const ub_pjoint *j, vec3 axis)
{
    vec3 ra = mat3_apply(&a->rot, j->refa), rb = mat3_apply(&b->rot, j->refb);
    return ub_atan2f(v3_dot(cross(ra, rb), axis), v3_dot(ra, rb));
}

void ub_pjoints_solve(ub_physics *ph, f32 h)
{
    for (i32 i = 0; i < ph->joint_count; ++i) {
        ub_pjoint *j = &ph->joints[i];
        ub_body *a = &ph->bodies[j->a], *b = &ph->bodies[j->b];
        if (a->fallen || b->fallen || (inv_m(a) == 0 && inv_m(b) == 0)) continue;
        vec3 ra, rb, pa, pb;
        wp(a, j->la, &ra, &pa);
        wp(b, j->lb, &rb, &pb);
        vec3 d = v3_sub(pb, pa);
        f32 acc = 0;
        switch (j->kind) {
        case UB_PJ_BALL: solve_point(a, b, ra, rb, d, h); break;
        case UB_PJ_HINGE: {
            vec3 axa = mat3_apply(&a->rot, j->axa), axb = mat3_apply(&b->rot, j->axb);
            solve_lock(a, b, cross(axb, axa), &axa, h); /* the axes together */
            j->angle = hinge_angle(a, b, j, axa);
            if (j->motor == 1) {
                f32 acc2 = 0;
                solve_twist(a, b, axa, j->motor_speed, 0, h, j->motor_max * h, &acc2);
            } else if (j->motor == 2) {
                f32 e = ub_wrap_angle(j->target - j->angle);
                f32 speed = ub_clampf(e / h * 0.2f, -j->motor_speed, j->motor_speed);
                f32 acc2 = 0;
                solve_twist(a, b, axa, speed, 0, h, j->motor_max * h, &acc2);
            }
            if (j->limits) {
                f32 acc2 = 0;
                if (j->angle < j->lower) solve_twist(a, b, axa, 0, j->angle - j->lower, h, 0, &acc2);
                else if (j->angle > j->upper) solve_twist(a, b, axa, 0, j->angle - j->upper, h, 0, &acc2);
            }
            solve_point(a, b, ra, rb, d, h);
            break;
        }
        case UB_PJ_PRISMATIC: {
            vec3 axa = mat3_apply(&a->rot, j->axa);
            vec3 qa = mat3_apply(&a->rot, j->refa), qb = mat3_apply(&b->rot, j->refb);
            vec3 axb = mat3_apply(&b->rot, j->axb);
            solve_lock(a, b, v3_add(cross(axb, axa), cross(qb, qa)), NULL, h);
            /* off the axis: no; along it: free, limited, or driven */
            vec3 n1 = v3_normalize(cross(axa, ub_absf(axa.y) < 0.9f ? v3(0, 1, 0) : v3(1, 0, 0)));
            vec3 n2 = cross(axa, n1);
            solve_dir(a, b, ra, rb, n1, 0, v3_dot(d, n1), h, -1e30f, 1e30f, &acc);
            acc = 0;
            solve_dir(a, b, ra, rb, n2, 0, v3_dot(d, n2), h, -1e30f, 1e30f, &acc);
            f32 x = v3_dot(d, axa);
            j->angle = x;
            if (j->motor == 1) {
                acc = 0;
                solve_dir(a, b, ra, rb, axa, j->motor_speed, 0, h, -j->motor_max * h, j->motor_max * h, &acc);
            } else if (j->motor == 2) {
                f32 speed = ub_clampf((j->target - x) / h * 0.2f, -j->motor_speed, j->motor_speed);
                acc = 0;
                solve_dir(a, b, ra, rb, axa, speed, 0, h, -j->motor_max * h, j->motor_max * h, &acc);
            }
            if (j->limits && (x < j->lower || x > j->upper)) {
                acc = 0;
                solve_dir(a, b, ra, rb, axa, 0, x < j->lower ? x - j->lower : x - j->upper, h, -1e30f, 1e30f, &acc);
            }
            break;
        }
        case UB_PJ_ROPE: case UB_PJ_ROD: {
            f32 len = v3_length(d);
            if (len < 1e-5f) break;
            vec3 n = v3_scale(d, 1.0f / len);
            f32 err = len - j->length;
            if (j->kind == UB_PJ_ROPE && err <= 0) break; /* slack */
            solve_dir(a, b, ra, rb, n, 0, err, h, j->kind == UB_PJ_ROPE ? -1e30f : -1e30f, j->kind == UB_PJ_ROPE ? 0 : 1e30f, &acc);
            break;
        }
        case UB_PJ_SPRING: {
            f32 len = v3_length(d);
            if (len < 1e-5f) break;
            vec3 n = v3_scale(d, 1.0f / len);
            f32 v = v3_dot(v3_sub(vel_at(b, rb), vel_at(a, ra)), n);
            f32 f = -j->stiffness * (len - j->length) - j->damping * v;
            vec3 imp = v3_scale(n, f * h);
            apply(a, imp, ra, -1);
            apply(b, imp, rb, 1);
            break;
        }
        default: break;
        }
    }
}

bool ub_phys_set_joints(ub_physics *ph, const ub_pjoint *j, i32 count)
{
    if (count > ph->joint_cap) {
        ub_pjoint *n = (ub_pjoint *)plat_alloc(sizeof(ub_pjoint) * (size_t)count);
        if (!n) return false;
        plat_free(ph->joints);
        ph->joints = n;
        ph->joint_cap = count;
    }
    ph->joint_count = 0;
    for (i32 i = 0; i < count; ++i) {
        if (j[i].a < 0 || j[i].b < 0 || j[i].a >= ph->body_count || j[i].b >= ph->body_count || j[i].a == j[i].b) continue;
        ph->joints[ph->joint_count++] = j[i];
        ub_phys_wake(ph, j[i].a);
        ub_phys_wake(ph, j[i].b);
    }
    return true;
}

static bool forces_room(ub_physics *ph)
{
    if (ph->force_cap >= ph->body_count) return true;
    i32 nc = ph->body_count + 64;
    u8 *blk = (u8 *)plat_alloc(sizeof(vec3) * (size_t)nc * 6 + (size_t)nc);
    if (!blk) return false;
    vec3 *f = (vec3 *)blk;
    if (ph->force) {
        for (int k = 0; k < 6; ++k) memcpy(f + nc * k, ph->force + ph->force_cap * k, sizeof(vec3) * (size_t)ph->force_cap);
        memcpy(f + nc * 6, ph->driven, (size_t)ph->force_cap);
        plat_free(ph->force);
    }
    ph->force = f;
    ph->torque = f + nc;
    ph->drive_v = f + nc * 2;
    ph->drive_f = f + nc * 3;
    ph->drive_w = f + nc * 4;
    ph->drive_t = f + nc * 5;
    ph->driven = (u8 *)(f + nc * 6);
    ph->force_cap = nc;
    return true;
}

void ub_phys_add_force(ub_physics *ph, i32 body, vec3 force, vec3 torque)
{
    if (body <= 0 || body >= ph->body_count || !forces_room(ph)) return;
    ph->force[body] = v3_add(ph->force[body], force);
    ph->torque[body] = v3_add(ph->torque[body], torque);
    ub_phys_wake(ph, body);
    ph->bodies[body].sleep_time = 0;
}

void ub_phys_drive(ub_physics *ph, i32 body, vec3 v_target, vec3 max_force, vec3 w_target, vec3 max_torque)
{
    if (body <= 0 || body >= ph->body_count || !forces_room(ph)) return;
    ub_body *b = &ph->bodies[body];
    if (b->type != UB_BODY_DYNAMIC || b->fallen) return;
    ub_phys_wake(ph, body);
    b->sleep_time = 0;
    /* several drives on one body: the components each one limits */
    vec3 *v = &ph->drive_v[body], *f = &ph->drive_f[body], *w = &ph->drive_w[body], *t = &ph->drive_t[body];
    if (!ph->driven[body]) *v = *f = *w = *t = v3(0, 0, 0);
    ph->driven[body] = 1;
    for (int k = 0; k < 3; ++k) {
        if ((&max_force.x)[k] > 0) {
            (&v->x)[k] = (&v_target.x)[k];
            (&f->x)[k] += (&max_force.x)[k];
        }
        if ((&max_torque.x)[k] > 0) {
            (&w->x)[k] = (&w_target.x)[k];
            (&t->x)[k] += (&max_torque.x)[k];
        }
    }
}

/* each substep: forces and drives into velocities (as gravity is) */
void ub_pforces_apply(ub_physics *ph, f32 h)
{
    if (!ph->force) return;
    for (i32 i = 1; i < ph->body_count && i < ph->force_cap; ++i) {
        ub_body *b = &ph->bodies[i];
        if (b->type != UB_BODY_DYNAMIC || !b->awake || b->fallen) continue;
        b->v = v3_madd(b->v, ph->force[i], h * b->inv_mass);
        b->w = v3_add(b->w, mat3_apply(&b->inv_inertia, v3_scale(ph->torque[i], h)));
        if (!ph->driven[i]) continue;
        f32 *v = &b->v.x, *w = &b->w.x;
        const f32 *vt = &ph->drive_v[i].x, *mf = &ph->drive_f[i].x, *wt = &ph->drive_w[i].x, *mt = &ph->drive_t[i].x;
        for (int k = 0; k < 3; ++k) {
            if (mf[k] > 0) {
                f32 lim = mf[k] * h * b->inv_mass;
                v[k] += ub_clampf(vt[k] - v[k], -lim, lim);
            }
            if (mt[k] > 0) {
                f32 lim = mt[k] * h * (&b->inv_inertia.x.x)[k * 4];
                w[k] += ub_clampf(wt[k] - w[k], -lim, lim);
            }
        }
    }
}

void ub_pforces_clear(ub_physics *ph)
{
    if (!ph->force) return;
    for (i32 i = 0; i < ph->force_cap; ++i) {
        ph->force[i] = ph->torque[i] = v3(0, 0, 0);
        ph->driven[i] = 0;
    }
}

/* bodies joined by a motor or spring stay awake */
void ub_pjoints_keep_awake(ub_physics *ph)
{
    for (i32 i = 0; i < ph->joint_count; ++i) {
        const ub_pjoint *j = &ph->joints[i];
        bool active = j->motor || j->kind == UB_PJ_SPRING;
        ub_body *a = &ph->bodies[j->a], *b = &ph->bodies[j->b];
        /* one awake: both awake (they move together) */
        if (a->type == UB_BODY_DYNAMIC && b->type == UB_BODY_DYNAMIC && (a->awake != b->awake)) {
            ub_phys_wake(ph, j->a);
            ub_phys_wake(ph, j->b);
        }
        if (active) {
            if (a->type == UB_BODY_DYNAMIC) { ub_phys_wake(ph, j->a); a->sleep_time = 0; }
            if (b->type == UB_BODY_DYNAMIC) { ub_phys_wake(ph, j->b); b->sleep_time = 0; }
        }
    }
}
