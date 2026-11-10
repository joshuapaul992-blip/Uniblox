/* Uniblox - joints from scripts (milestone 8C): Weld, ManualWeld, Snap,
 * Motor, Motor6D and WeldConstraint. Part0, Part1, C0, C1, Transform and
 * Enabled are kept; once a joint in the game joins two parts, the loose
 * one is carried by the other (game/ub_rig.c), which is how tools, hats
 * and things scripts stick on characters stay put. The character's own
 * Motor6Ds are the rig's: changing their C0, C1 or Transform moves its
 * limbs. */
#include <string.h>

#include "physics/ub_pmath.h"
#include "script/ub_sapi.h"

#define W (&S.g->world)

void get_extra_value(lua_State *L, i32 idx, const char *key);
void set_extra_value(lua_State *L, i32 idx, const char *key, int v);

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

bool is_joint_class(const char *cls)
{
    return streq(cls, "Weld") || streq(cls, "ManualWeld") || streq(cls, "Snap") || streq(cls, "Motor") ||
           streq(cls, "Motor6D") || streq(cls, "WeldConstraint") || streq(cls, "Glue");
}

static const char *cls_of(i32 i) { return ub_world_str(W, W->inst[i].class_name); }

static i32 ref_prop(i32 i, const char *k)
{
    const ub_value *v = ub_world_prop_named(W, i, k);
    return v && v->type == UB_PV_REF && v->v.ref >= 0 && ub_world_alive(W, v->v.ref) ? v->v.ref : -1;
}

/* C0 / C1 / Transform of a non-rig joint (Roblox axes), identity if unset */
static void get_cf(lua_State *L, i32 i, const char *k, vec3 *p, mat3 *r)
{
    get_extra_value(L, i, k);
    if (is_cf(L, -1)) {
        scf *c = check_cf(L, -1);
        *p = c->p;
        *r = c->r;
    } else {
        *p = v3(0, 0, 0);
        *r = mat3_identity();
    }
    lua_pop(L, 1);
}

static bool enabled(lua_State *L, i32 i)
{
    get_extra_value(L, i, "Enabled");
    bool on = lua_isnil(L, -1) || lua_toboolean(L, -1);
    lua_pop(L, 1);
    return on;
}

static bool anchored(i32 i)
{
    return W->inst[i].part >= 0 && (W->parts[W->inst[i].part].flags & UB_PART_ANCHORED);
}

/* Joins (or lets go of) the parts of joint i, as it is now. */
void joint_changed(lua_State *L, i32 i)
{
    if (i < 0 || !ub_world_alive(W, i) || !is_joint_class(cls_of(i))) return;
    if (ub_rig_joint(S.g, i)) return; /* the rig's own */
    ub_game_uncarry(S.g, -1, i);
    i32 p0 = ref_prop(i, "Part0"), p1 = ref_prop(i, "Part1");
    if (p0 < 0 || p1 < 0 || p0 == p1 || W->inst[p0].part < 0 || W->inst[p1].part < 0) return;
    if (!enabled(L, i) || !sapi_in_game(i)) return;
    /* who carries whom: the character, or an anchored part, carries the other */
    bool c0 = ub_rig_is_character_part(S.g, p0) || anchored(p0);
    bool c1 = ub_rig_is_character_part(S.g, p1) || anchored(p1);
    if (c0 && c1 && !ub_rig_is_character_part(S.g, p0) && !ub_rig_is_character_part(S.g, p1)) return; /* both fixed */
    bool part1_follows = c0 || !c1;
    i32 follow = part1_follows ? p0 : p1, part = part1_follows ? p1 : p0;
    vec3 off_p;
    mat3 off_r;
    const ub_part *fp = &W->parts[W->inst[follow].part], *pp = &W->parts[W->inst[part].part];
    if (streq(cls_of(i), "WeldConstraint")) { /* keeps them as they are */
        mat3 inv = mat3_transpose(&fp->rot);
        off_r = mat3_mul(&inv, &pp->rot);
        off_p = mat3_apply(&inv, v3_sub(pp->pos, fp->pos));
    } else { /* part1 = part0 * C0 * C1^-1 (Roblox axes), so part1 sits at C0 C1^-1 from part0 */
        vec3 c0p, c1p;
        mat3 c0r, c1r;
        get_cf(L, i, "C0", &c0p, &c0r);
        get_cf(L, i, "C1", &c1p, &c1r);
        mat3 c1i = mat3_transpose(&c1r);
        mat3 m = mat3_mul(&c0r, &c1i);
        vec3 mp = v3_sub(c0p, mat3_apply(&m, c1p));
        if (!part1_follows) { /* part0 follows part1: the inverse */
            mat3 mi = mat3_transpose(&m);
            mp = v3_scale(mat3_apply(&mi, mp), -1.0f);
            m = mi;
        }
        off_r = ub_rbx_rot(&m);
        off_p = ub_rbx_vec(mp);
    }
    ub_game_carry(S.g, part, follow, off_p, &off_r, i);
}

bool joint_get(lua_State *L, i32 i, const char *k)
{
    if (!is_joint_class(cls_of(i))) return false;
    ub_joint *j = ub_rig_joint(S.g, i);
    if (streq(k, "Part0") || streq(k, "Part1")) {
        push_inst(L, ref_prop(i, k));
        return true;
    }
    if (streq(k, "C0") || streq(k, "C1") || streq(k, "Transform")) {
        if (j) {
            bool c0 = streq(k, "C0"), c1 = streq(k, "C1");
            push_cf(L, c0 ? j->c0p : c1 ? j->c1p : j->tp, c0 ? &j->c0r : c1 ? &j->c1r : &j->tr);
        } else {
            vec3 p;
            mat3 r;
            get_cf(L, i, k, &p, &r);
            push_cf(L, p, &r);
        }
        return true;
    }
    if (streq(k, "Enabled") || streq(k, "Active")) {
        lua_pushboolean(L, enabled(L, i));
        return true;
    }
    if (streq(k, "CurrentAngle") || streq(k, "DesiredAngle") || streq(k, "MaxVelocity")) {
        get_extra_value(L, i, k);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushnumber(L, 0);
        }
        return true;
    }
    return false;
}

bool joint_set(lua_State *L, i32 i, const char *k, int v)
{
    if (!is_joint_class(cls_of(i))) return false;
    ub_joint *j = ub_rig_joint(S.g, i);
    if (streq(k, "Part0") || streq(k, "Part1")) {
        i32 p = opt_inst(L, v);
        if (p >= 0 && W->inst[p].part < 0) return sapi_error(L, "%s must be a BasePart", k), true;
        ub_value val;
        memset(&val, 0, sizeof val);
        val.type = UB_PV_REF;
        val.v.ref = p;
        ub_world_set_prop_named(W, i, k, &val);
        if (j) {
            if (streq(k, "Part0")) j->part0 = p;
            else j->part1 = p;
        }
    } else if (streq(k, "C0") || streq(k, "C1") || streq(k, "Transform")) {
        scf *c = check_cf(L, v);
        if (j) {
            bool c0 = streq(k, "C0"), c1 = streq(k, "C1");
            vec3 *pp = c0 ? &j->c0p : c1 ? &j->c1p : &j->tp;
            mat3 *rr = c0 ? &j->c0r : c1 ? &j->c1r : &j->tr;
            *pp = c->p;
            *rr = c->r;
        } else {
            set_extra_value(L, i, k, v);
        }
    } else if (streq(k, "Enabled") || streq(k, "Active")) {
        lua_pushboolean(L, lua_toboolean(L, v));
        set_extra_value(L, i, "Enabled", lua_gettop(L));
        lua_pop(L, 1);
    } else if (streq(k, "CurrentAngle") || streq(k, "DesiredAngle") || streq(k, "MaxVelocity")) {
        luaL_checknumber(L, v);
        set_extra_value(L, i, k, v);
        return true;
    } else {
        return false;
    }
    joint_changed(L, i);
    prop_changed(L, i, k);
    return true;
}

void joints_moved(lua_State *L, i32 root)
{
    if (root < 0 || !ub_world_alive(W, root)) return;
    i32 n;
    i32 *sub = sapi_subtree(root, &n);
    if (!sub) return;
    for (i32 k = 0; k < n; ++k)
        if (is_joint_class(cls_of(sub[k]))) joint_changed(L, sub[k]);
    plat_free(sub);
}

/* ---------------------------------------------------------- attachments
 * Position, Axis, SecondaryAxis, WorldPosition, WorldCFrame and
 * WorldAxis are all its CFrame (Roblox's axes, in its part's space). */

static bool attach_cf(i32 i, vec3 *p, mat3 *r)
{
    const ub_value *v = ub_world_prop_named(W, i, "CFrame");
    if (!v || v->type != UB_PV_CFRAME) {
        *p = v3(0, 0, 0);
        *r = mat3_identity();
        return false;
    }
    const f32 *m = v->v.cf.m;
    *p = v3(m[0], m[1], m[2]);
    r->x = v3(m[3], m[6], m[9]);
    r->y = v3(m[4], m[7], m[10]);
    r->z = v3(m[5], m[8], m[11]);
    return true;
}

static void set_attach_cf(i32 i, vec3 p, const mat3 *r)
{
    f32 m[12] = { p.x, p.y, p.z, r->x.x, r->y.x, r->z.x, r->x.y, r->y.y, r->z.y, r->x.z, r->y.z, r->z.z };
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_CFRAME;
    v.v.cf.m = m;
    ub_world_set_prop_named(W, i, "CFrame", &v);
    S.g->constraints_dirty = true;
}

/* its part's CFrame (Roblox axes) */
static void part_cf(i32 part, vec3 *p, mat3 *r)
{
    vec3 pp;
    mat3 pr;
    ub_game_part_pose(S.g, W->inst[part].part, &pp, &pr);
    *p = ub_rbx_vec(pp);
    *r = ub_rbx_rot(&pr);
}

bool attach_get(lua_State *L, i32 i, const char *k)
{
    if (streq(k, "CurrentAngle") || streq(k, "CurrentPosition") || streq(k, "CurrentDistance")) {
        if (!ub_constraint_class(cls_of(i))) return false;
        f32 v;
        ub_constraint_reading(S.g, i, &v);
        lua_pushnumber(L, v);
        return true;
    }
    if (!streq(cls_of(i), "Attachment")) return false;
    vec3 p;
    mat3 r;
    attach_cf(i, &p, &r);
    i32 part = W->inst[i].parent;
    bool on_part = part >= 0 && W->inst[part].part >= 0;
    if (streq(k, "Position")) push_v3(L, p);
    else if (streq(k, "Axis")) push_v3(L, r.x);
    else if (streq(k, "SecondaryAxis")) push_v3(L, r.y);
    else if (streq(k, "CFrame")) push_cf(L, p, &r);
    else if (streq(k, "WorldPosition") || streq(k, "WorldCFrame") || streq(k, "WorldAxis") ||
             streq(k, "WorldSecondaryAxis")) {
        vec3 pp = v3(0, 0, 0);
        mat3 pr = mat3_identity();
        if (on_part) part_cf(part, &pp, &pr);
        vec3 wp = v3_add(pp, mat3_apply(&pr, p));
        mat3 wr = mat3_mul(&pr, &r);
        if (streq(k, "WorldPosition")) push_v3(L, wp);
        else if (streq(k, "WorldCFrame")) push_cf(L, wp, &wr);
        else push_v3(L, streq(k, "WorldAxis") ? wr.x : wr.y);
    } else return false;
    return true;
}

bool attach_set(lua_State *L, i32 i, const char *k, int v)
{
    if (!streq(cls_of(i), "Attachment")) return false;
    vec3 p;
    mat3 r;
    attach_cf(i, &p, &r);
    i32 part = W->inst[i].parent;
    if (streq(k, "Position")) p = check_v3(L, v);
    else if (streq(k, "CFrame")) {
        scf *c = check_cf(L, v);
        p = c->p;
        r = c->r;
    } else if (streq(k, "WorldPosition") || streq(k, "WorldCFrame")) {
        vec3 pp = v3(0, 0, 0);
        mat3 pr = mat3_identity();
        if (part >= 0 && W->inst[part].part >= 0) part_cf(part, &pp, &pr);
        mat3 prt = mat3_transpose(&pr);
        if (streq(k, "WorldPosition")) p = mat3_apply(&prt, v3_sub(check_v3(L, v), pp));
        else {
            scf *c = check_cf(L, v);
            p = mat3_apply(&prt, v3_sub(c->p, pp));
            r = mat3_mul(&prt, &c->r);
        }
    } else if (streq(k, "Axis") || streq(k, "SecondaryAxis")) {
        vec3 a = v3_normalize(check_v3(L, v));
        if (streq(k, "Axis")) {
            vec3 up = r.y;
            if (ub_absf(v3_dot(up, a)) > 0.99f) up = ub_absf(a.y) < 0.9f ? v3(0, 1, 0) : v3(1, 0, 0);
            r.x = a;
            r.z = v3_normalize(v3_cross(a, up));
            r.y = v3_cross(r.z, a);
        } else {
            r.y = v3_normalize(v3_sub(a, v3_scale(r.x, v3_dot(a, r.x))));
            r.z = v3_cross(r.x, r.y);
        }
    } else return false;
    set_attach_cf(i, p, &r);
    prop_changed(L, i, k);
    return true;
}
