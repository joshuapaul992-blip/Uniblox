/* Uniblox - Roblox's constraints and body movers (milestone 8C).
 *
 * Constraints in the Workspace (BallSocket, Hinge with its motor and
 * servo, Prismatic, Cylindrical, Rope, Rod, Spring) become physics joints
 * between the bodies of their attachments' parts; they are rebuilt when
 * the tree or a part's physics changes, and their motors' settings are
 * read again every step so scripts can drive them. Movers push their
 * part's body every step: BodyVelocity, BodyAngularVelocity,
 * BodyPosition, BodyGyro, BodyForce, BodyThrust, VectorForce, Torque,
 * LinearVelocity, AngularVelocity, AlignPosition, AlignOrientation. */
#include "game/ub_game.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "physics/ub_pmath.h"
#include "crt/ub_crt.h"

static bool streq(const char *a, const char *b) { return ub_strcmp(a, b) == 0; }
static const char *cls(const ub_world *w, i32 i) { return ub_world_str(w, w->inst[i].class_name); }

static f32 fprop(const ub_world *w, i32 i, const char *k, f32 d)
{
    const ub_value *v = ub_world_prop_named(w, i, k);
    if (!v) return d;
    if (v->type == UB_PV_FLOAT) return v->v.f;
    if (v->type == UB_PV_DOUBLE) return (f32)v->v.d;
    if (v->type == UB_PV_INT || v->type == UB_PV_ENUM) return (f32)v->v.i;
    if (v->type == UB_PV_BOOL) return v->v.b ? 1.0f : 0.0f;
    return d;
}

static bool bprop(const ub_world *w, i32 i, const char *k, bool d)
{
    const ub_value *v = ub_world_prop_named(w, i, k);
    return v && v->type == UB_PV_BOOL ? v->v.b : d;
}

static vec3 vprop(const ub_world *w, i32 i, const char *k, vec3 d)
{
    const ub_value *v = ub_world_prop_named(w, i, k);
    return v && v->type == UB_PV_VECTOR3 ? v3(v->v.v3.x, v->v.v3.y, v->v.v3.z) : d;
}

static i32 rprop(const ub_world *w, i32 i, const char *k)
{
    const ub_value *v = ub_world_prop_named(w, i, k);
    return v && v->type == UB_PV_REF && v->v.ref >= 0 && ub_world_alive(w, v->v.ref) ? v->v.ref : -1;
}

/* a CFrame property in Uniblox's axes */
static bool cfprop(const ub_world *w, i32 i, const char *k, vec3 *p, mat3 *r)
{
    const ub_value *v = ub_world_prop_named(w, i, k);
    if (!v || v->type != UB_PV_CFRAME) return false;
    const f32 *m = v->v.cf.m;
    mat3 rr;
    rr.x = v3(m[3], m[6], m[9]);
    rr.y = v3(m[4], m[7], m[10]);
    rr.z = v3(m[5], m[8], m[11]);
    *r = ub_rbx_rot(&rr);
    *p = ub_rbx_vec(v3(m[0], m[1], m[2]));
    return true;
}

/* An attachment's part, and its frame in the world */
static i32 attach_world(ub_game *g, i32 a, vec3 *pos, mat3 *rot)
{
    ub_world *w = &g->world;
    if (a < 0 || ub_strcmp(cls(w, a), "Attachment") != 0) return -1;
    i32 part = w->inst[a].parent;
    if (part < 0 || w->inst[part].part < 0) return -1;
    vec3 pp;
    mat3 pr;
    ub_game_part_pose(g, w->inst[part].part, &pp, &pr);
    vec3 lp = v3(0, 0, 0);
    mat3 lr = mat3_identity();
    if (!cfprop(w, a, "CFrame", &lp, &lr)) lp = ub_rbx_vec(vprop(w, a, "Position", v3(0, 0, 0)));
    *pos = v3_add(pp, mat3_apply(&pr, lp));
    *rot = mat3_mul(&pr, &lr);
    return part;
}

/* a world point and axes into a body's own space */
static void to_body(const ub_physics *ph, i32 b, vec3 wpnt, vec3 *local)
{
    const ub_body *bd = &ph->bodies[b];
    if (b == 0) {
        *local = wpnt;
        return;
    }
    *local = mat3_apply_t(&bd->rot, v3_sub(wpnt, bd->pos));
}

static vec3 axis_to_body(const ub_physics *ph, i32 b, vec3 axis)
{
    return b == 0 ? axis : mat3_apply_t(&ph->bodies[b].rot, axis);
}

static bool is_constraint(const char *c)
{
    return streq(c, "BallSocketConstraint") || streq(c, "HingeConstraint") || streq(c, "PrismaticConstraint") ||
           streq(c, "CylindricalConstraint") || streq(c, "RopeConstraint") || streq(c, "RodConstraint") ||
           streq(c, "SpringConstraint");
}

static bool is_mover(const char *c)
{
    return streq(c, "BodyVelocity") || streq(c, "BodyAngularVelocity") || streq(c, "BodyPosition") ||
           streq(c, "BodyGyro") || streq(c, "BodyForce") || streq(c, "BodyThrust") || streq(c, "VectorForce") ||
           streq(c, "Torque") || streq(c, "LinearVelocity") || streq(c, "AngularVelocity") ||
           streq(c, "AlignPosition") || streq(c, "AlignOrientation");
}

bool ub_constraint_class(const char *c) { return is_constraint(c) || streq(c, "Attachment"); }

#define DEG (UB_PI / 180.0f)

/* motor settings (read each step: scripts change them) */
static void motor_of(const ub_world *w, i32 c, ub_pjoint *j)
{
    bool hinge = j->kind == UB_PJ_HINGE;
    int act = (int)fprop(w, c, "ActuatorType", 0); /* None 0, Motor 1, Servo 2 */
    j->motor = (u8)(act == 1 || act == 2 ? act : 0);
    j->limits = (u8)bprop(w, c, "LimitsEnabled", false);
    if (hinge) {
        j->lower = fprop(w, c, "LowerAngle", -45) * DEG;
        j->upper = fprop(w, c, "UpperAngle", 45) * DEG;
        if (act == 1) {
            j->motor_speed = fprop(w, c, "AngularVelocity", 0);
            j->motor_max = fprop(w, c, "MotorMaxTorque", 0);
        } else if (act == 2) {
            j->motor_speed = fprop(w, c, "AngularSpeed", 0);
            j->motor_max = fprop(w, c, "ServoMaxTorque", 0);
            j->target = fprop(w, c, "TargetAngle", 0) * DEG;
        }
    } else {
        j->lower = fprop(w, c, "LowerLimit", 0);
        j->upper = fprop(w, c, "UpperLimit", 5);
        if (act == 1) {
            j->motor_speed = fprop(w, c, "Velocity", 0);
            j->motor_max = fprop(w, c, "MotorMaxForce", 0);
        } else if (act == 2) {
            j->motor_speed = fprop(w, c, "Speed", 0);
            j->motor_max = fprop(w, c, "ServoMaxForce", 0);
            j->target = fprop(w, c, "TargetPosition", 0);
        }
    }
    if (j->motor_max > 1e9f) j->motor_max = 1e9f; /* "inf" */
}

void ub_constraints_rebuild(ub_game *g)
{
    ub_world *w = &g->world;
    g->constraints_dirty = false;
    g->mover_count = 0;
    if (!g->phys_ok) return;
    i32 ws = ub_world_workspace(w);
    ub_pjoint js[512];
    i32 nj = 0;
    for (i32 i = 0; i < w->count; ++i) {
        if (!ub_world_alive(w, i) || w->inst[i].part >= 0) continue;
        const char *c = cls(w, i);
        bool con = is_constraint(c), mov = !con && is_mover(c);
        if ((!con && !mov) || !ub_world_is_descendant(w, i, ws)) continue;
        if (mov) {
            if (g->mover_count == g->mover_cap) {
                i32 nc = g->mover_cap ? g->mover_cap * 2 : 32;
                i32 *n = (i32 *)plat_alloc(sizeof(i32) * (size_t)nc);
                if (!n) continue;
                if (g->movers) memcpy(n, g->movers, sizeof(i32) * (size_t)g->mover_count);
                plat_free(g->movers);
                g->movers = n;
                g->mover_cap = nc;
            }
            g->movers[g->mover_count++] = i;
            continue;
        }
        if (!bprop(w, i, "Enabled", true) || nj >= 512) continue;
        vec3 pa, pb;
        mat3 ra, rb;
        i32 parta = attach_world(g, rprop(w, i, "Attachment0"), &pa, &ra);
        i32 partb = attach_world(g, rprop(w, i, "Attachment1"), &pb, &rb);
        if (parta < 0 || partb < 0) continue;
        i32 ba = ub_phys_part_body(&g->phys, w->inst[parta].part), bb = ub_phys_part_body(&g->phys, w->inst[partb].part);
        if (ba < 0 || bb < 0 || ba == bb) continue;
        ub_pjoint *j = &js[nj];
        memset(j, 0, sizeof *j);
        j->a = ba;
        j->b = bb;
        to_body(&g->phys, ba, pa, &j->la);
        to_body(&g->phys, bb, pb, &j->lb);
        j->axa = axis_to_body(&g->phys, ba, ra.x);
        j->axb = axis_to_body(&g->phys, bb, rb.x);
        j->refa = axis_to_body(&g->phys, ba, ra.y);
        j->refb = axis_to_body(&g->phys, bb, rb.y);
        f32 dist = v3_length(v3_sub(pb, pa));
        if (streq(c, "BallSocketConstraint")) j->kind = UB_PJ_BALL;
        else if (streq(c, "HingeConstraint")) j->kind = UB_PJ_HINGE;
        else if (streq(c, "PrismaticConstraint") || streq(c, "CylindricalConstraint")) j->kind = UB_PJ_PRISMATIC;
        else if (streq(c, "RopeConstraint")) {
            j->kind = UB_PJ_ROPE;
            j->length = fprop(w, i, "Length", dist);
        } else if (streq(c, "RodConstraint")) {
            j->kind = UB_PJ_ROD;
            j->length = fprop(w, i, "Length", dist);
        } else {
            j->kind = UB_PJ_SPRING;
            j->length = fprop(w, i, "FreeLength", 1);
            j->stiffness = fprop(w, i, "Stiffness", 0);
            j->damping = fprop(w, i, "Damping", 0);
        }
        if (j->kind == UB_PJ_HINGE || j->kind == UB_PJ_PRISMATIC) motor_of(w, i, j);
        g->joint_inst[nj] = i;
        nj++;
    }
    g->pjoint_count = nj;
    ub_phys_set_joints(&g->phys, js, nj);
}

/* per axis: a Roblox-axes vector of limits as Uniblox ones (only z flips, magnitudes stay) */
static vec3 limits(vec3 v) { return v3(ub_absf(v.x), ub_absf(v.y), ub_absf(v.z)); }

static vec3 axis_angle(const mat3 *from, const mat3 *to)
{
    mat3 ft = mat3_transpose(from);
    mat3 d = mat3_mul(to, &ft); /* world rotation from `from` to `to` */
    quat q = quat_from_mat3(&d);
    if (q.w < 0) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
    f32 s = ub_sqrtf(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-6f) return v3(0, 0, 0);
    f32 ang = 2.0f * ub_atan2f(s, q.w);
    return v3_scale(v3(q.x, q.y, q.z), ang / s);
}

static f32 big(f32 x) { return x > 1e9f || !(x == x) ? 1e9f : x; }

void ub_constraints_step(ub_game *g)
{
    ub_world *w = &g->world;
    if (!g->phys_ok) return;
    if (g->constraints_dirty) ub_constraints_rebuild(g);
    /* motors and servos follow their properties */
    for (i32 k = 0; k < g->phys.joint_count && k < g->pjoint_count; ++k) {
        ub_pjoint *j = &g->phys.joints[k];
        i32 c = g->joint_inst[k];
        if (!ub_world_alive(w, c)) continue;
        if (j->kind == UB_PJ_HINGE || j->kind == UB_PJ_PRISMATIC) motor_of(w, c, j);
        else if (j->kind == UB_PJ_SPRING) {
            j->stiffness = fprop(w, c, "Stiffness", j->stiffness);
            j->damping = fprop(w, c, "Damping", j->damping);
            j->length = fprop(w, c, "FreeLength", j->length);
        } else if (j->kind == UB_PJ_ROPE || j->kind == UB_PJ_ROD) j->length = fprop(w, c, "Length", j->length);
    }
    for (i32 m = 0; m < g->mover_count; ++m) {
        i32 i = g->movers[m];
        if (!ub_world_alive(w, i)) continue;
        const char *c = cls(w, i);
        bool attach_based = streq(c, "VectorForce") || streq(c, "Torque") || streq(c, "LinearVelocity") ||
                            streq(c, "AngularVelocity") || streq(c, "AlignPosition") || streq(c, "AlignOrientation");
        if (attach_based && !bprop(w, i, "Enabled", true)) continue;
        vec3 apos;
        mat3 arot = mat3_identity();
        i32 part;
        if (attach_based) part = attach_world(g, rprop(w, i, "Attachment0"), &apos, &arot);
        else part = w->inst[i].parent >= 0 && w->inst[w->inst[i].parent].part >= 0 ? w->inst[i].parent : -1;
        if (part < 0) continue;
        i32 pi = w->inst[part].part, b = ub_phys_part_body(&g->phys, pi);
        if (b <= 0) continue;
        const ub_body *bd = &g->phys.bodies[b];
        vec3 pp;
        mat3 pr;
        ub_game_part_pose(g, pi, &pp, &pr);
        /* RelativeTo: Attachment0 0, Attachment1 1, World 2 */
        int rel = (int)fprop(w, i, "RelativeTo", 2);
        const mat3 *frame = rel == 2 ? NULL : &arot;
        vec3 zero = v3(0, 0, 0);
        if (streq(c, "BodyVelocity")) {
            ub_phys_drive(&g->phys, b, ub_rbx_vec(vprop(w, i, "Velocity", zero)), limits(vprop(w, i, "MaxForce", zero)), zero, zero);
        } else if (streq(c, "BodyAngularVelocity")) {
            ub_phys_drive(&g->phys, b, zero, zero, ub_rbx_vec(vprop(w, i, "AngularVelocity", zero)),
                          limits(vprop(w, i, "MaxTorque", zero)));
        } else if (streq(c, "BodyForce")) {
            ub_phys_add_force(&g->phys, b, ub_rbx_vec(vprop(w, i, "Force", zero)), zero);
        } else if (streq(c, "BodyThrust")) {
            ub_phys_add_force(&g->phys, b, mat3_apply(&pr, ub_rbx_vec(vprop(w, i, "Force", zero))), zero);
        } else if (streq(c, "BodyPosition")) {
            vec3 target = ub_rbx_vec(vprop(w, i, "Position", zero));
            f32 p = fprop(w, i, "P", 10000), d = fprop(w, i, "D", 1250);
            vec3 f = v3_sub(v3_scale(v3_sub(target, bd->pos), p), v3_scale(bd->v, d));
            vec3 mf = limits(vprop(w, i, "MaxForce", v3(4000, 4000, 4000)));
            f = v3(ub_clampf(f.x, -big(mf.x), big(mf.x)), ub_clampf(f.y, -big(mf.y), big(mf.y)), ub_clampf(f.z, -big(mf.z), big(mf.z)));
            ub_phys_add_force(&g->phys, b, f, zero);
        } else if (streq(c, "BodyGyro")) {
            vec3 tp;
            mat3 tr;
            if (!cfprop(w, i, "CFrame", &tp, &tr)) continue;
            f32 p = fprop(w, i, "P", 3000), d = fprop(w, i, "D", 500);
            vec3 err = axis_angle(&pr, &tr);
            vec3 t = v3_sub(v3_scale(err, p), v3_scale(bd->w, d));
            vec3 mt = limits(vprop(w, i, "MaxTorque", v3(400000, 0, 400000)));
            t = v3(ub_clampf(t.x, -big(mt.x), big(mt.x)), ub_clampf(t.y, -big(mt.y), big(mt.y)), ub_clampf(t.z, -big(mt.z), big(mt.z)));
            ub_phys_add_force(&g->phys, b, zero, t);
        } else if (streq(c, "VectorForce")) {
            vec3 f = ub_rbx_vec(vprop(w, i, "Force", zero));
            ub_phys_add_force(&g->phys, b, frame ? mat3_apply(frame, f) : f, zero);
        } else if (streq(c, "Torque")) {
            vec3 t = ub_rbx_vec(vprop(w, i, "Torque", zero));
            ub_phys_add_force(&g->phys, b, zero, frame ? mat3_apply(frame, t) : t);
        } else if (streq(c, "LinearVelocity")) {
            vec3 v = ub_rbx_vec(vprop(w, i, "VectorVelocity", zero));
            if (frame) v = mat3_apply(frame, v);
            f32 mf = big(fprop(w, i, "MaxForce", 1000));
            ub_phys_drive(&g->phys, b, v, v3(mf, mf, mf), zero, zero);
        } else if (streq(c, "AngularVelocity")) {
            vec3 v = ub_rbx_vec(vprop(w, i, "AngularVelocity", zero));
            if (frame) v = mat3_apply(frame, v);
            f32 mt = big(fprop(w, i, "MaxTorque", 0));
            ub_phys_drive(&g->phys, b, zero, zero, v, v3(mt, mt, mt));
        } else if (streq(c, "AlignPosition")) {
            vec3 target;
            mat3 trot;
            int mode = (int)fprop(w, i, "Mode", 1); /* OneAttachment 0, TwoAttachment 1 */
            if (mode == 0) target = ub_rbx_vec(vprop(w, i, "Position", zero));
            else if (attach_world(g, rprop(w, i, "Attachment1"), &target, &trot) < 0) continue;
            bool rigid = bprop(w, i, "RigidityEnabled", false);
            f32 resp = rigid ? 200.0f : fprop(w, i, "Responsiveness", 10);
            f32 maxv = big(fprop(w, i, "MaxVelocity", 1e9f));
            vec3 v = v3_scale(v3_sub(target, apos), resp);
            f32 sp = v3_length(v);
            if (sp > maxv) v = v3_scale(v, maxv / sp);
            f32 mf = rigid ? 1e9f : big(fprop(w, i, "MaxForce", 10000));
            ub_phys_drive(&g->phys, b, v, v3(mf, mf, mf), zero, zero);
        } else if (streq(c, "AlignOrientation")) {
            vec3 tp;
            mat3 tr;
            int mode = (int)fprop(w, i, "Mode", 1);
            if (mode == 0) {
                if (!cfprop(w, i, "CFrame", &tp, &tr)) continue;
            } else if (attach_world(g, rprop(w, i, "Attachment1"), &tp, &tr) < 0) continue;
            bool rigid = bprop(w, i, "RigidityEnabled", false);
            f32 resp = rigid ? 200.0f : fprop(w, i, "Responsiveness", 10);
            vec3 wv = v3_scale(axis_angle(&arot, &tr), resp);
            f32 mt = rigid ? 1e9f : big(fprop(w, i, "MaxTorque", 10000));
            ub_phys_drive(&g->phys, b, zero, zero, wv, v3(mt, mt, mt));
        }
    }
}

bool ub_constraint_reading(const ub_game *g, i32 inst, f32 *value)
{
    for (i32 k = 0; k < g->pjoint_count && k < g->phys.joint_count; ++k)
        if (g->joint_inst[k] == inst) {
            const ub_pjoint *j = &g->phys.joints[k];
            *value = j->kind == UB_PJ_HINGE ? j->angle * 180.0f / UB_PI : j->angle;
            return true;
        }
    *value = 0;
    return false;
}
