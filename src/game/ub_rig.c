/* Uniblox - the character's rig and carried parts (milestone 8C).
 *
 * The player's character is built like Roblox's: an R15 rig (LowerTorso,
 * UpperTorso, Head, the arms in three parts and the legs in three parts)
 * or an R6 one (Torso, Head, Left Arm, Right Arm, Left Leg, Right Leg),
 * joined to the HumanoidRootPart by Motor6Ds named as in Roblox, with the
 * attachments tools and hats look for. Its proportions are Uniblox's own.
 * Each frame the joints are solved (part1 = part0 * C0 * Transform *
 * C1^-1, in Roblox's axes) from the root, with the legs and arms swinging
 * as it walks and the whole figure tipping over when it dies. Scripts can
 * change C0, C1 and Transform.
 *
 * Parts welded at run time (tools, hats, things scripts stick on) are
 * carried: put at what they follow times their offset each frame. */
#include "game/ub_game.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "physics/ub_pmath.h"
#include "world/ub_classes.h"
#include "crt/ub_crt.h"

#define ROOT_HEIGHT 3.0f

static i32 add_part(ub_game *g, const char *name, vec3 size, u32 color, ub_shape shape)
{
    ub_part p;
    memset(&p, 0, sizeof p);
    p.rot = mat3_identity();
    p.size = size;
    p.color = color;
    p.shape = (u8)shape;
    p.material = UB_MAT_PLASTIC;
    p.flags = 0; /* not anchored, not solid: the character body does the colliding */
    i32 i = ub_world_add_part(&g->world, g->char_model, UB_CLASS_PART, "Part", name, &p);
    if (i >= 0) {
        g->world.inst[i].flags |= UB_INST_RUNTIME;
        g->world.parts[g->world.inst[i].part].flags |= UB_PART_CARRIED;
    }
    return i;
}

static void set_ref(ub_world *w, i32 inst, const char *k, i32 ref)
{
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_REF;
    v.v.ref = ref;
    ub_world_set_prop_named(w, inst, k, &v);
}

static void attach(ub_game *g, i32 part, const char *name, vec3 pos)
{
    i32 a = ub_world_add(&g->world, part, UB_CLASS_OTHER, "Attachment", name);
    if (a < 0) return;
    g->world.inst[a].flags |= UB_INST_RUNTIME;
    ub_cdb_fill_defaults(&g->world, a, "Attachment");
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_VECTOR3;
    v.v.v3.x = pos.x;
    v.v.v3.y = pos.y;
    v.v.v3.z = pos.z;
    ub_world_set_prop_named(&g->world, a, "Position", &v);
}

/* A joint: centres of both parts and the joint point, in the figure's
 * own axes (Roblox's: y up, facing -z), feet at 0. */
static void joint(ub_game *g, const char *name, i32 p0, vec3 c0, i32 p1, vec3 c1, vec3 at, u8 swing)
{
    if (g->joint_count >= UB_RIG_JOINTS || p0 < 0 || p1 < 0) return;
    ub_joint *j = &g->joints[g->joint_count++];
    memset(j, 0, sizeof *j);
    j->motor = ub_world_add(&g->world, p1, UB_CLASS_OTHER, "Motor6D", name);
    if (j->motor >= 0) {
        g->world.inst[j->motor].flags |= UB_INST_RUNTIME;
        set_ref(&g->world, j->motor, "Part0", p0);
        set_ref(&g->world, j->motor, "Part1", p1);
    }
    j->part0 = p0;
    j->part1 = p1;
    j->c0p = v3_sub(at, c0);
    j->c1p = v3_sub(at, c1);
    j->c0r = j->c1r = j->tr = mat3_identity();
    j->swing = swing;
    ub_rig_joint_saved(g, j);
}

/* A CFrame property (Roblox's axes: position, then the rotation by rows). */
static void set_cf(ub_world *w, i32 inst, const char *k, vec3 p, const mat3 *r)
{
    f32 m[12] = { p.x, p.y, p.z, r->x.x, r->y.x, r->z.x, r->x.y, r->y.y, r->z.y, r->x.z, r->y.z, r->z.z };
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_CFRAME;
    v.v.cf.m = m;
    ub_world_set_prop_named(w, inst, k, &v);
}

static bool get_cf(const ub_world *w, i32 inst, const char *k, vec3 *p, mat3 *r)
{
    const ub_value *v = ub_world_prop_named(w, inst, k);
    if (!v || v->type != UB_PV_CFRAME || !v->v.cf.m) return false;
    const f32 *m = v->v.cf.m;
    *p = v3(m[0], m[1], m[2]);
    r->x = v3(m[3], m[6], m[9]);
    r->y = v3(m[4], m[7], m[10]);
    r->z = v3(m[5], m[8], m[11]);
    return true;
}

static i32 get_ref(const ub_world *w, i32 inst, const char *k)
{
    const ub_value *v = ub_world_prop_named(w, inst, k);
    return v && v->type == UB_PV_REF && ub_world_alive(w, v->v.ref) ? v->v.ref : -1;
}

void ub_rig_joint_saved(ub_game *g, const ub_joint *j)
{
    /* (as the script layer keeps a joint's C0 and C1: replication takes them to the client) */
    if (j->motor < 0 || !ub_world_alive(&g->world, j->motor)) return;
    set_cf(&g->world, j->motor, "~C0", j->c0p, &j->c0r);
    set_cf(&g->world, j->motor, "~C1", j->c1p, &j->c1r);
    ub_value sw = ub_vint(j->swing);
    ub_world_set_prop_named(&g->world, j->motor, "~Swing", &sw);
}

void ub_rig_adopt(ub_game *g)
{
    ub_world *w = &g->world;
    g->joint_count = 0;
    if (g->char_model < 0 || !ub_world_alive(w, g->char_model)) return;
    const ub_value *rt = g->humanoid >= 0 ? ub_world_prop_named(w, g->humanoid, "RigType") : NULL;
    g->r15 = !rt || rt->type != UB_PV_ENUM || rt->v.e != 0;
    i32 n;
    i32 *sub = ub_world_subtree(w, g->char_model, &n);
    for (i32 k = 0; sub && k < n && g->joint_count < UB_RIG_JOINTS; ++k) {
        i32 m = sub[k];
        if (ub_strcmp(ub_world_str(w, w->inst[m].class_name), "Motor6D") != 0) continue;
        ub_joint j;
        memset(&j, 0, sizeof j);
        j.motor = m;
        j.part0 = get_ref(w, m, "Part0");
        j.part1 = get_ref(w, m, "Part1");
        j.tr = mat3_identity();
        const ub_value *sw = ub_world_prop_named(w, m, "~Swing");
        j.swing = (u8)(sw && sw->type == UB_PV_INT ? sw->v.i : 0);
        if (j.part0 < 0 || j.part1 < 0 || !get_cf(w, m, "~C0", &j.c0p, &j.c0r) || !get_cf(w, m, "~C1", &j.c1p, &j.c1r))
            continue;
        g->joints[g->joint_count++] = j;
    }
    plat_free(sub);
    ub_rig_pose(g, 0);
}

void ub_rig_build(ub_game *g)
{
    ub_world *w = &g->world;
    g->joint_count = 0;
    if (g->char_model < 0 || g->char_root < 0) return;
    /* R15 unless the place asks for R6 (StarterPlayer.GameSettingsAvatar) */
    g->r15 = true;
    for (i32 i = 0; i < w->count; ++i)
        if (w->inst[i].parent < 0 && ub_world_alive(w, i) && !ub_strcmp(ub_world_str(w, w->inst[i].class_name), "StarterPlayer")) {
            const ub_value *v = ub_world_prop_named(w, i, "GameSettingsAvatar");
            if (v && v->type == UB_PV_ENUM && v->v.e == 0) g->r15 = false;
        }
    const u32 shirt = 0x2F8F9D, pants = 0x3B3F4A, skin = 0xF0C8A0;
    i32 hrp = g->char_root;
    vec3 root = v3(0, ROOT_HEIGHT, 0);
    attach(g, hrp, "RootAttachment", v3(0, 0, 0));
    if (g->r15) {
        i32 lt = add_part(g, "LowerTorso", v3(2, 0.4f, 1), pants, UB_SHAPE_BOX);
        i32 ut = add_part(g, "UpperTorso", v3(2, 1.6f, 1), shirt, UB_SHAPE_BOX);
        i32 head = add_part(g, "Head", v3(1.4f, 1.4f, 1.4f), skin, UB_SHAPE_BALL);
        joint(g, "Root", hrp, root, lt, v3(0, 2.2f, 0), v3(0, 2.0f, 0), 0);
        joint(g, "Waist", lt, v3(0, 2.2f, 0), ut, v3(0, 3.2f, 0), v3(0, 2.4f, 0), 0);
        joint(g, "Neck", ut, v3(0, 3.2f, 0), head, v3(0, 4.7f, 0), v3(0, 4.0f, 0), 0);
        attach(g, head, "HatAttachment", v3(0, 0.7f, 0));
        attach(g, head, "HairAttachment", v3(0, 0.7f, 0));
        attach(g, head, "FaceFrontAttachment", v3(0, 0, -0.7f));
        attach(g, ut, "NeckAttachment", v3(0, 0.8f, 0));
        attach(g, ut, "BodyFrontAttachment", v3(0, 0, -0.5f));
        attach(g, ut, "BodyBackAttachment", v3(0, 0, 0.5f));
        attach(g, lt, "WaistCenterAttachment", v3(0, 0, 0));
        for (int side = 0; side < 2; ++side) {
            f32 x = side ? 1.45f : -1.45f, lx = side ? 0.5f : -0.5f;
            const char *s = side ? "Right" : "Left";
            char n[40];
            ub_fmt(n, sizeof n, "%sUpperArm", s);
            i32 ua = add_part(g, n, v3(0.9f, 1.0f, 0.9f), skin, UB_SHAPE_BOX);
            ub_fmt(n, sizeof n, "%sLowerArm", s);
            i32 la = add_part(g, n, v3(0.9f, 0.8f, 0.9f), skin, UB_SHAPE_BOX);
            ub_fmt(n, sizeof n, "%sHand", s);
            i32 hand = add_part(g, n, v3(0.9f, 0.3f, 0.9f), skin, UB_SHAPE_BOX);
            ub_fmt(n, sizeof n, "%sUpperLeg", s);
            i32 ul = add_part(g, n, v3(0.9f, 0.9f, 1), pants, UB_SHAPE_BOX);
            ub_fmt(n, sizeof n, "%sLowerLeg", s);
            i32 ll = add_part(g, n, v3(0.9f, 0.8f, 1), pants, UB_SHAPE_BOX);
            ub_fmt(n, sizeof n, "%sFoot", s);
            i32 foot = add_part(g, n, v3(0.9f, 0.3f, 1), pants, UB_SHAPE_BOX);
            ub_fmt(n, sizeof n, "%sShoulder", s);
            joint(g, n, ut, v3(0, 3.2f, 0), ua, v3(x, 3.5f, 0), v3(x, 4.0f, 0), (u8)(side ? 4 : 3));
            ub_fmt(n, sizeof n, "%sElbow", s);
            joint(g, n, ua, v3(x, 3.5f, 0), la, v3(x, 2.6f, 0), v3(x, 3.0f, 0), 0);
            ub_fmt(n, sizeof n, "%sWrist", s);
            joint(g, n, la, v3(x, 2.6f, 0), hand, v3(x, 2.05f, 0), v3(x, 2.2f, 0), 0);
            ub_fmt(n, sizeof n, "%sHip", s);
            joint(g, n, lt, v3(0, 2.2f, 0), ul, v3(lx, 1.55f, 0), v3(lx, 2.0f, 0), (u8)(side ? 2 : 1));
            ub_fmt(n, sizeof n, "%sKnee", s);
            joint(g, n, ul, v3(lx, 1.55f, 0), ll, v3(lx, 0.7f, 0), v3(lx, 1.1f, 0), 0);
            ub_fmt(n, sizeof n, "%sAnkle", s);
            joint(g, n, ll, v3(lx, 0.7f, 0), foot, v3(lx, 0.15f, 0), v3(lx, 0.3f, 0), 0);
            ub_fmt(n, sizeof n, "%sGripAttachment", s);
            attach(g, hand, n, v3(0, -0.15f, 0));
            ub_fmt(n, sizeof n, "%sShoulderAttachment", s);
            attach(g, ua, n, v3(0, 0.5f, 0));
            ub_fmt(n, sizeof n, "%sFootAttachment", s);
            attach(g, foot, n, v3(0, -0.15f, 0));
        }
    } else {
        i32 torso = add_part(g, "Torso", v3(2, 2, 1), shirt, UB_SHAPE_BOX);
        i32 head = add_part(g, "Head", v3(1.4f, 1.4f, 1.4f), skin, UB_SHAPE_BALL);
        joint(g, "RootJoint", hrp, root, torso, v3(0, 3, 0), v3(0, 3, 0), 0);
        joint(g, "Neck", torso, v3(0, 3, 0), head, v3(0, 4.7f, 0), v3(0, 4, 0), 0);
        attach(g, head, "HatAttachment", v3(0, 0.7f, 0));
        attach(g, head, "HairAttachment", v3(0, 0.7f, 0));
        attach(g, head, "FaceFrontAttachment", v3(0, 0, -0.7f));
        attach(g, torso, "NeckAttachment", v3(0, 1, 0));
        attach(g, torso, "BodyFrontAttachment", v3(0, 0, -0.5f));
        attach(g, torso, "BodyBackAttachment", v3(0, 0, 0.5f));
        attach(g, torso, "WaistCenterAttachment", v3(0, -1, 0));
        for (int side = 0; side < 2; ++side) {
            f32 x = side ? 1.5f : -1.5f, lx = side ? 0.5f : -0.5f;
            i32 arm = add_part(g, side ? "Right Arm" : "Left Arm", v3(1, 2, 1), skin, UB_SHAPE_BOX);
            i32 leg = add_part(g, side ? "Right Leg" : "Left Leg", v3(1, 2, 1), pants, UB_SHAPE_BOX);
            joint(g, side ? "Right Shoulder" : "Left Shoulder", torso, v3(0, 3, 0), arm, v3(x, 3, 0), v3(x, 4, 0),
                  (u8)(side ? 4 : 3));
            joint(g, side ? "Right Hip" : "Left Hip", torso, v3(0, 3, 0), leg, v3(lx, 1, 0), v3(lx, 2, 0),
                  (u8)(side ? 2 : 1));
            attach(g, arm, side ? "RightGripAttachment" : "LeftGripAttachment", v3(0, -1, 0));
            attach(g, arm, side ? "RightShoulderAttachment" : "LeftShoulderAttachment", v3(0, 1, 0));
        }
    }
    /* (the Motor6Ds live in their part1, as Roblox's do; the Humanoid's rig type) */
    if (g->humanoid >= 0) {
        ub_value rt = ub_venum(g->r15 ? 1u : 0u);
        ub_world_set_prop_named(w, g->humanoid, "RigType", &rt);
    }
    ub_rig_pose(g, 0);
}

ub_joint *ub_rig_joint(ub_game *g, i32 motor)
{
    for (int i = 0; i < g->joint_count; ++i)
        if (g->joints[i].motor == motor) return &g->joints[i];
    return NULL;
}

bool ub_game_is_carried(const ub_game *g, i32 part)
{
    for (i32 i = 0; i < g->carry_count; ++i)
        if (g->carry[i].part == part) return true;
    return false;
}

bool ub_rig_is_character_part(const ub_game *g, i32 inst)
{
    if (inst < 0 || g->char_model < 0) return false;
    if (inst == g->char_root) return true;
    for (int i = 0; i < g->joint_count; ++i)
        if (g->joints[i].part1 == inst) return true;
    for (i32 i = 0; i < g->carry_count; ++i)
        if (g->carry[i].part == inst && g->carry[i].follow != inst && ub_rig_is_character_part(g, g->carry[i].follow))
            return true;
    return false;
}

bool ub_game_carry(ub_game *g, i32 part, i32 follow, vec3 p, const mat3 *r, i32 joint_inst)
{
    ub_world *w = &g->world;
    if (part < 0 || follow < 0 || part == follow || w->inst[part].part < 0 || w->inst[follow].part < 0) return false;
    /* no loops: what it follows must not (in the end) follow it */
    for (i32 f = follow, guard = 0; guard < 64; ++guard) {
        i32 k = -1;
        for (i32 i = 0; i < g->carry_count; ++i)
            if (g->carry[i].part == f) k = i;
        if (k < 0) break;
        f = g->carry[k].follow;
        if (f == part) return false;
    }
    ub_game_uncarry(g, part, -1);
    if (g->carry_count == g->carry_cap) {
        i32 nc = g->carry_cap ? g->carry_cap * 2 : 16;
        ub_carry *n = (ub_carry *)plat_alloc(sizeof(ub_carry) * (size_t)nc);
        if (!n) return false;
        if (g->carry) memcpy(n, g->carry, sizeof(ub_carry) * (size_t)g->carry_count);
        plat_free(g->carry);
        g->carry = n;
        g->carry_cap = nc;
    }
    ub_carry *c = &g->carry[g->carry_count++];
    c->part = part;
    c->follow = follow;
    c->joint = joint_inst;
    c->p = p;
    c->r = *r;
    ub_part *pp = &w->parts[w->inst[part].part];
    pp->flags |= UB_PART_CARRIED;
    ub_game_part_changed(g, w->inst[part].part, UB_CHANGE_LOOK | UB_CHANGE_PHYS);
    return true;
}

void ub_game_uncarry(ub_game *g, i32 part, i32 joint_inst)
{
    ub_world *w = &g->world;
    for (i32 i = 0; i < g->carry_count;) {
        ub_carry *c = &g->carry[i];
        bool match = joint_inst >= 0 ? c->joint == joint_inst : c->part == part;
        if (!match) {
            ++i;
            continue;
        }
        i32 pi = c->part >= 0 && c->part < w->count ? w->inst[c->part].part : -1;
        *c = g->carry[--g->carry_count];
        if (pi >= 0) {
            w->parts[pi].flags &= (u8)~UB_PART_CARRIED;
            ub_game_part_changed(g, pi, UB_CHANGE_LOOK | UB_CHANGE_PHYS); /* simulated again */
        }
    }
}

/* Roblox CFrame (rotation, position) times another */
static void cf_mul(const mat3 *ar, vec3 ap, const mat3 *br, vec3 bp, mat3 *r, vec3 *p)
{
    *r = mat3_mul(ar, br);
    *p = v3_add(ap, mat3_apply(ar, bp));
}

static void set_part(ub_game *g, i32 inst, vec3 pos, const mat3 *rot)
{
    ub_world *w = &g->world;
    if (inst < 0 || !ub_world_alive(w, inst) || w->inst[inst].part < 0) return;
    ub_part *p = &w->parts[w->inst[inst].part];
    p->pos = pos;
    p->rot = *rot;
    ub_game_part_changed(g, w->inst[inst].part, UB_CHANGE_LOOK);
}

void ub_rig_pose(ub_game *g, f32 dt)
{
    ub_world *w = &g->world;
    if (g->char_root < 0 || !ub_world_alive(w, g->char_root)) return;
    /* without the app (tests, servers), face where it moves */
    if (!g->pose_from_app && g->phys_ok) {
        vec3 v = ub_char_velocity(&g->chr, &g->phys);
        f32 h = ub_sqrtf(v.x * v.x + v.z * v.z);
        if (h > 0.5f) g->pose_facing = ub_atan2f(v.x, v.z);
        g->pose_amount = ub_clampf(g->pose_amount + (h > 2.0f ? dt : -dt) * 6.0f, 0, 1);
        g->pose_phase = ub_wrap_angle(g->pose_phase + dt * 10.0f * h / 16.0f);
        g->pose_fallen = g->dead ? ub_clampf(g->pose_fallen + dt * 3.0f, 0, 1) : 0;
    }
    /* the root (Uniblox axes): at the feet, turned to face, tipped over when dead */
    mat3 body = mat3_from_euler(g->pose_facing, -UB_HALF_PI * g->pose_fallen, 0);
    vec3 feet = g->feet;
    vec3 root_pos = v3_add(feet, mat3_apply(&body, v3(0, ROOT_HEIGHT, 0)));
    ub_part *rp = &w->parts[w->inst[g->char_root].part];
    rp->pos = root_pos;
    rp->rot = body;
    /* the joints, in Roblox's axes */
    f32 swing = ub_sinf(g->pose_phase) * 0.7f * g->pose_amount;
    for (int i = 0; i < g->joint_count; ++i) {
        ub_joint *j = &g->joints[i];
        if (j->swing) {
            f32 a = (j->swing == 1 || j->swing == 4) ? swing : -swing;
            j->tr = mat3_from_euler(0, a, 0); /* about x */
            j->tp = v3(0, 0, 0);
        }
        i32 p0 = j->part0;
        if (p0 < 0 || !ub_world_alive(w, p0) || w->inst[p0].part < 0) continue;
        const ub_part *a = &w->parts[w->inst[p0].part];
        mat3 r0 = ub_rbx_rot(&a->rot), r, r2, c1i = mat3_transpose(&j->c1r);
        vec3 p, p2;
        cf_mul(&r0, ub_rbx_vec(a->pos), &j->c0r, j->c0p, &r, &p);
        cf_mul(&r, p, &j->tr, j->tp, &r2, &p2);
        vec3 c1ip = v3_scale(mat3_apply(&c1i, j->c1p), -1.0f);
        cf_mul(&r2, p2, &c1i, c1ip, &r, &p);
        mat3 ru = ub_rbx_rot(&r);
        set_part(g, j->part1, ub_rbx_vec(p), &ru);
    }
    /* carried parts: in order, so carried things can carry more */
    for (i32 i = 0; i < g->carry_count; ++i) {
        ub_carry *c = &g->carry[i];
        if (!ub_world_alive(w, c->follow) || !ub_world_alive(w, c->part) || w->inst[c->follow].part < 0) continue;
        vec3 fp;
        mat3 fr;
        ub_game_part_pose(g, w->inst[c->follow].part, &fp, &fr);
        mat3 r = mat3_mul(&fr, &c->r);
        set_part(g, c->part, v3_add(fp, mat3_apply(&fr, c->p)), &r);
    }
}
