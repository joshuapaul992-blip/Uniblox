#include "physics/ub_physics.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "render/ub_texture.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

/* ------------------------------------------------------------ tuning */

#define SUBSTEPS 6             /* solver substeps per collision step */
#define CONTACT_HERTZ 180.0f   /* contact stiffness: a spring this fast (capped at a quarter of the substep rate)... */
#define CONTACT_DAMPING 3.0f   /* ...damped this much (soft step, as in Box2D 3) */
#define MAX_PUSH 10.0f         /* studs/s: fastest overlap correction */
#define IMPACT_SPEED 1.0f      /* studs/s: faster approaches are caught evenly first */
#define MARGIN 0.04f           /* studs: contacts found this far apart */
#define BOUNCE_SPEED 12.0f     /* studs/s: slower impacts do not bounce */
#define SLEEP_LINEAR 0.35f     /* studs/s */
#define SLEEP_ANGULAR 0.35f    /* rad/s */
#define SLEEP_TIME 0.5f        /* seconds still before a group sleeps */
#define MAX_SPEED 1500.0f
#define MAX_SPIN 150.0f
#define ANGULAR_DAMPING 0.1f   /* per second */
#define ROLLING_RESISTANCE 0.02f /* balls: rolling torque limit = this x radius x pressing force */
#define MATCH_DIST2 (0.15f * 0.15f)

struct ub_bvh_node {
    vec3 lo, hi;
    i32 left;                  /* leaf: first index in static_ids; else left child (right = left + 1) */
    i32 count;                 /* > 0 for a leaf */
};

typedef struct {
    vec3 la, lb;               /* contact point in each body's space, for matching */
    vec3 ra, rb;               /* from each centre of mass at the start of the step */
    f32 sep;                   /* separation at the start of the step */
    f32 jn;                    /* normal impulse per substep (kept for warm starting) */
    f32 jn_max;
    f32 mn;                    /* normal effective mass */
    f32 vn0;                   /* approach speed before solving, for bounce */
} contact;

struct ub_manifold {
    u64 key;
    i32 sa, sb, ba, bb;
    vec3 n, t1, t2;            /* normal from a to b, and tangents */
    f32 friction, restitution;
    i32 count;
    contact c[UB_MAX_CONTACTS];
    /* Friction acts once per manifold, at the middle of the contact patch:
     * two sliding directions and a twist about the normal. (Friction at
     * every corner couples the corners through the bodies' spin and makes
     * tall stacks under strong gravity wobble apart.) */
    u8 touching;               /* really touching (not just a speculative contact) */
    vec3 ra, rb;               /* patch centre from each centre of mass */
    f32 radius;                /* patch size, for the twist limit */
    f32 jt1, jt2, jtw;         /* accumulated friction impulses */
    f32 mt1, mt2, mtw;
    f32 mc;                    /* normal effective mass at the patch centre */
    f32 vn0;                   /* approach speed at the centre before solving */
    f32 roll;                  /* rolling resistance arm (0 = none) */
    f32 jr1, jr2, mr1, mr2;    /* rolling resistance impulses and masses */
};

/* ------------------------------------------------------------ helpers */

static bool grow(void **p, i32 *cap, i32 need, size_t elem)
{
    if (need <= *cap) return true;
    i32 nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    void *np = plat_alloc((size_t)nc * elem);
    if (!np) return false;
    if (*p) {
        memcpy(np, *p, (size_t)*cap * elem);
        plat_free(*p);
    }
    *p = np;
    *cap = nc;
    return true;
}

void ub_material_physics(u8 material, f32 *density, f32 *friction, f32 *restitution)
{
    /* density in mass units per cubic stud; plastic is 0.7 */
    static const f32 k[UB_MAT_COUNT][3] = {
        { 0.70f, 0.30f, 0.30f },  /* plastic */
        { 0.70f, 0.30f, 0.30f },  /* grid */
        { 1.90f, 0.80f, 0.15f },  /* brick */
        { 0.40f, 0.48f, 0.20f },  /* wood */
        { 0.90f, 0.40f, 0.10f },  /* grass */
        { 7.85f, 0.40f, 0.25f },  /* metal */
        { 2.40f, 0.60f, 0.15f },  /* stone */
    };
    u32 m = material < UB_MAT_COUNT ? material : 0;
    *density = k[m][0];
    *friction = k[m][1];
    *restitution = k[m][2];
}

static void tangents(vec3 n, vec3 *t1, vec3 *t2)
{
    if (ub_absf(n.x) >= 0.57735f) *t1 = v3_normalize(v3(n.y, -n.x, 0));
    else *t1 = v3_normalize(v3(0, n.z, -n.y));
    *t2 = v3_cross(n, *t1);
}

static void shape_update(ub_physics *ph, ub_pshape *s)
{
    const ub_body *b = &ph->bodies[s->body];
    s->wpos = v3_add(b->pos, mat3_apply(&b->rot, s->lpos));
    s->wrot = mat3_mul(&b->rot, &s->lrot);
    vec3 half;
    if (s->shape == UB_SHAPE_BALL) {
        half = v3(s->radius, s->radius, s->radius);
    } else {
        const mat3 *r = &s->wrot;
        vec3 h = v3_scale(s->size, 0.5f);
        half = v3(ub_absf(r->x.x) * h.x + ub_absf(r->y.x) * h.y + ub_absf(r->z.x) * h.z,
                  ub_absf(r->x.y) * h.x + ub_absf(r->y.y) * h.y + ub_absf(r->z.y) * h.z,
                  ub_absf(r->x.z) * h.x + ub_absf(r->y.z) * h.y + ub_absf(r->z.z) * h.z);
    }
    /* grow by how far it can move this step, so fast things are caught */
    f32 reach = MARGIN;
    if (b->type != UB_BODY_STATIC)
        reach += (v3_length(b->v) + v3_length(b->w) * s->bound) * UB_PHYS_DT;
    half = v3_add(half, v3(reach, reach, reach));
    s->lo = v3_sub(s->wpos, half);
    s->hi = v3_add(s->wpos, half);
}

static void cshape(const ub_pshape *s, ub_cshape *c)
{
    c->sphere = s->shape == UB_SHAPE_BALL;
    c->hull = c->sphere ? NULL : s->hull ? s->hull : ub_hull_get((ub_shape)s->shape);
    c->pos = s->wpos;
    c->rot = s->wrot;
    c->size = s->size;
    c->radius = s->radius;
}

static bool body_active(const ub_body *b)
{
    if (b->fallen) return false;
    if (b->type == UB_BODY_DYNAMIC) return b->awake;
    if (b->type == UB_BODY_CHARACTER) return true;
    if (b->type == UB_BODY_KINEMATIC) return b->has_target || v3_length2(b->v) > 0 || v3_length2(b->w) > 0;
    return false;
}

static void update_inertia(ub_body *b)
{
    if (b->type == UB_BODY_DYNAMIC) b->inv_inertia = mat3_rotate_tensor(&b->rot, &b->inv_inertia_local);
    else b->inv_inertia = mat3_zero();
}

/* ---------------------------------------------------------------- BVH */

static f32 axis_of(vec3 v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }

/* Partially sort ids[lo..hi) so ids[k] has the k-th smallest centre on `axis`. */
static void select_kth(const ub_physics *ph, i32 *ids, i32 lo, i32 hi, i32 k, int axis)
{
    while (hi - lo > 1) {
        i32 mid = lo + (hi - lo) / 2;
        f32 pivot = axis_of(ph->shapes[ids[mid]].wpos, axis);
        i32 i = lo, j = hi - 1;
        while (i <= j) {
            while (axis_of(ph->shapes[ids[i]].wpos, axis) < pivot) ++i;
            while (axis_of(ph->shapes[ids[j]].wpos, axis) > pivot) --j;
            if (i <= j) {
                i32 t = ids[i];
                ids[i] = ids[j];
                ids[j] = t;
                ++i;
                --j;
            }
        }
        if (k <= j) hi = j + 1;
        else if (k >= i) lo = i;
        else return;
    }
}

static void bvh_build(ub_physics *ph, i32 node, i32 first, i32 count, int depth)
{
    ub_bvh_node *n = &ph->nodes[node];
    vec3 lo = v3(3e38f, 3e38f, 3e38f), hi = v3(-3e38f, -3e38f, -3e38f);
    vec3 clo = lo, chi = hi;
    for (i32 i = first; i < first + count; ++i) {
        const ub_pshape *s = &ph->shapes[ph->static_ids[i]];
        lo = v3_min(lo, s->lo);
        hi = v3_max(hi, s->hi);
        clo = v3_min(clo, s->wpos);
        chi = v3_max(chi, s->wpos);
    }
    n->lo = lo;
    n->hi = hi;
    if (count <= 4 || depth > 40) {
        n->left = first;
        n->count = count;
        return;
    }
    vec3 ext = v3_sub(chi, clo);
    int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
    i32 half = count / 2;
    select_kth(ph, ph->static_ids, first, first + count, first + half, axis);
    i32 l = ph->node_count;
    ph->node_count += 2;
    n->left = l;
    n->count = 0;
    bvh_build(ph, l, first, half, depth + 1);
    bvh_build(ph, l + 1, first + half, count - half, depth + 1);
}

static bool box_overlap(vec3 alo, vec3 ahi, vec3 blo, vec3 bhi)
{
    return alo.x <= bhi.x && blo.x <= ahi.x && alo.y <= bhi.y && blo.y <= ahi.y && alo.z <= bhi.z && blo.z <= ahi.z;
}

/* Does the segment origin + t*dir, t in [0, max], touch the box? */
static bool ray_box(vec3 o, vec3 inv, f32 max, vec3 lo, vec3 hi)
{
    f32 t0 = 0, t1 = max;
    const f32 os[3] = { o.x, o.y, o.z }, is[3] = { inv.x, inv.y, inv.z };
    const f32 l[3] = { lo.x, lo.y, lo.z }, h[3] = { hi.x, hi.y, hi.z };
    for (int a = 0; a < 3; ++a) {
        f32 ta = (l[a] - os[a]) * is[a], tb = (h[a] - os[a]) * is[a];
        if (ta > tb) { f32 t = ta; ta = tb; tb = t; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return false;
    }
    return true;
}

/* ------------------------------------------------------------- sorting */

static void sort_u64(u64 *a, u64 *tmp, i32 n)
{
    for (i32 width = 1; width < n; width *= 2) {
        for (i32 i = 0; i < n; i += 2 * width) {
            i32 m = UB_MIN(i + width, n), e = UB_MIN(i + 2 * width, n);
            i32 p = i, q = m, k = i;
            while (p < m && q < e) tmp[k++] = a[p] <= a[q] ? a[p++] : a[q++];
            while (p < m) tmp[k++] = a[p++];
            while (q < e) tmp[k++] = a[q++];
        }
        memcpy(a, tmp, sizeof(u64) * (size_t)n);
    }
}

/* ---------------------------------------------------------------- build */

static i32 uf_find(i32 *p, i32 x)
{
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

static i32 new_body(ub_physics *ph, ub_body_type type)
{
    if (!grow((void **)&ph->bodies, &ph->body_cap, ph->body_count + 1, sizeof(ub_body))) return -1;
    ub_body *b = &ph->bodies[ph->body_count];
    memset(b, 0, sizeof *b);
    b->type = (u8)type;
    b->q = b->prev_q = b->target_q = quat_identity();
    b->rot = mat3_identity();
    b->awake = type != UB_BODY_STATIC;
    b->sleep_next = b->sleep_head = -1;
    return ph->body_count++;
}

static bool dyn_add(ub_physics *ph, i32 shape)
{
    if (ph->shapes[shape].in_dyn) return true;
    if (!grow((void **)&ph->dyn, &ph->dyn_cap, ph->dyn_count + 1, sizeof(i32))) return false;
    ph->dyn[ph->dyn_count++] = shape;
    ph->shapes[shape].in_dyn = 1;
    return true;
}

static void dyn_remove(ub_physics *ph, i32 shape)
{
    if (!ph->shapes[shape].in_dyn) return;
    for (i32 i = 0; i < ph->dyn_count; ++i)
        if (ph->dyn[i] == shape) {
            memmove(ph->dyn + i, ph->dyn + i + 1, sizeof(i32) * (size_t)(ph->dyn_count - i - 1));
            ph->dyn_count--;
            break;
        }
    ph->shapes[shape].in_dyn = 0;
}

/* The collision hulls of a mesh's pieces, made on first use and kept with
 * the world's mesh (one allocation: [count][hulls]). NULL if none. */
#define MAX_PIECES 64
static const ub_hull *mesh_hulls(ub_world *w, u16 id, i32 *count)
{
    *count = 0;
    if (id == 0 || (i32)id > w->mesh_count) return NULL;
    ub_wmesh *m = &w->meshes[id - 1];
    if (m->flags & UB_WMESH_WOUND) return NULL; /* a look, not convex pieces */
    if (!m->phys_cache) {
        u32 np = m->npieces < MAX_PIECES ? m->npieces : MAX_PIECES;
        u8 *block = (u8 *)plat_alloc(sizeof(ub_hull) * (np + 1));
        if (!block) return NULL;
        i32 *n = (i32 *)block;
        ub_hull *h = (ub_hull *)(block + sizeof(ub_hull)); /* (the count gets a hull-sized slot) */
        *n = 0;
        u32 v0 = 0, t0 = 0;
        vec3 pts[256];
        u32 tri[3 * 512];
        for (u32 p = 0; p < m->npieces && (u32)*n < np; ++p) {
            u32 nv = m->piece_v[p] - v0, nt = m->piece_t[p] - t0;
            if (nv >= 4 && nv <= 256 && nt <= 512) {
                for (u32 i = 0; i < nv; ++i) pts[i] = v3(m->v[(v0 + i) * 3], m->v[(v0 + i) * 3 + 1], m->v[(v0 + i) * 3 + 2]);
                for (u32 i = 0; i < nt * 3; ++i) tri[i] = m->tri[t0 * 3 + i] - v0;
                if (ub_hull_build(&h[*n], pts, (int)nv, tri, (int)nt)) (*n)++;
            } else if (nv > 256) { /* a big piece: its sampled points, fitted (no exact faces) */
                u32 step = (nv + 255) / 256, k = 0;
                for (u32 i = 0; i < nv && k < 256; i += step, ++k)
                    pts[k] = v3(m->v[(v0 + i) * 3], m->v[(v0 + i) * 3 + 1], m->v[(v0 + i) * 3 + 2]);
                if (ub_hull_build(&h[*n], pts, (int)k, tri, 0)) (*n)++;
            }
            v0 = m->piece_v[p];
            t0 = m->piece_t[p];
        }
        m->phys_cache = block;
    }
    *count = *(const i32 *)m->phys_cache;
    return *count ? (const ub_hull *)((const u8 *)m->phys_cache + sizeof(ub_hull)) : NULL;
}

bool ub_phys_init(ub_physics *ph, ub_world *w, const i32 *kinematic_parts, int kinematic_count)
{
    memset(ph, 0, sizeof *ph);
    ph->world = w;
    ph->gravity = v3(0, -w->gravity, 0);
    ph->up = w->gravity >= 0 ? v3(0, 1, 0) : v3(0, -1, 0);
    ph->fallen_height = w->fallen_height;
    ph->part_count = w->part_count;
    i32 np = w->part_count;
    ph->part_shape = (i32 *)plat_alloc(sizeof(i32) * (size_t)(np + 1));
    i32 *group = (i32 *)plat_alloc(sizeof(i32) * (size_t)(np + 1));
    i32 *inst_of = (i32 *)plat_alloc(sizeof(i32) * (size_t)(np + 1));
    u8 *flags = (u8 *)plat_alloc((size_t)np + 1);   /* 1 in workspace, 2 anchored group, 4 kinematic group */
    i32 *body_of = (i32 *)plat_alloc(sizeof(i32) * (size_t)(np + 1));
    bool ok = ph->part_shape && group && inst_of && flags && body_of;
    if (!ok) goto done;

    for (i32 i = 0; i < np; ++i) {
        ph->part_shape[i] = -1;
        group[i] = i;
        inst_of[i] = -1;
        body_of[i] = -1;
    }
    for (i32 i = 0; i < w->count; ++i) {
        i32 p = w->inst[i].part;
        if (p >= 0 && p < np && ub_world_is_descendant(w, i, w->workspace) && !(w->parts[p].flags & UB_PART_CARRIED)) {
            flags[p] = 1;
            inst_of[p] = i;
        }
    }
    /* welded parts form assemblies */
    i32 welds = 0;
    for (i32 i = 0; i < w->weld_count; ++i) {
        i32 a = w->inst[w->welds[i].a].part, b = w->inst[w->welds[i].b].part;
        if (a < 0 || b < 0 || !flags[a] || !flags[b]) continue;
        i32 ra = uf_find(group, a), rb = uf_find(group, b);
        if (ra != rb) group[ra] = rb;
        welds++;
    }
    for (i32 i = 0; i < kinematic_count; ++i) {
        i32 p = kinematic_parts[i];
        if (p >= 0 && p < np && flags[p]) flags[uf_find(group, p)] |= 4;
    }
    for (i32 i = 0; i < np; ++i)
        if (flags[i] && (w->parts[i].flags & UB_PART_ANCHORED)) flags[uf_find(group, i)] |= 2;

    /* body 0: everything anchored */
    ok = new_body(ph, UB_BODY_STATIC) == 0;
    /* count shapes: static collidable parts, plus every part of a moving group */
    i32 nshapes = 0;
    for (i32 i = 0; i < np && ok; ++i) {
        if (!flags[i]) continue;
        u8 gf = flags[uf_find(group, i)];
        bool moving = (gf & 4) || !(gf & 2);
        if (moving || (w->parts[i].flags & UB_PART_CAN_COLLIDE)) nshapes++;
        if (!moving && (w->parts[i].flags & UB_PART_CAN_COLLIDE) && w->parts[i].mesh) {
            i32 pieces;
            mesh_hulls(w, w->parts[i].mesh, &pieces);
            if (pieces > 1) nshapes += pieces - 1;
        }
    }
    ok = ok && grow((void **)&ph->shapes, &ph->shape_cap, nshapes + 8, sizeof(ub_pshape));
    if (!ok) goto done;

    /* static shapes first (body 0 owns shapes 0..n-1) */
    for (i32 i = 0; i < np; ++i) {
        if (!flags[i]) continue;
        u8 gf = flags[uf_find(group, i)];
        if ((gf & 4) || !(gf & 2) || !(w->parts[i].flags & UB_PART_CAN_COLLIDE)) continue;
        const ub_part *p = &w->parts[i];
        ub_pshape *s = &ph->shapes[ph->shape_count];
        memset(s, 0, sizeof *s);
        s->body = 0;
        s->part = i;
        s->shape = p->shape < UB_SHAPE_COUNT ? p->shape : UB_SHAPE_BOX;
        s->flags = UB_SHAPE_COLLIDES;
        s->size = p->size;
        s->radius = 0.5f * UB_MIN(p->size.x, UB_MIN(p->size.y, p->size.z));
        s->bound = 0.5f * v3_length(p->size);
        s->lpos = p->pos;
        quat q = quat_from_mat3(&p->rot);
        s->lrot = quat_to_mat3(q);
        f32 dens;
        ub_material_physics(p->material, &dens, &s->friction, &s->restitution);
        s->next_piece = -1;
        i32 pieces = 0;
        const ub_hull *hulls = p->mesh ? mesh_hulls(w, p->mesh, &pieces) : NULL;
        if (hulls) s->hull = &hulls[0];
        shape_update(ph, s);
        ph->part_shape[i] = ph->shape_count++;
        /* the rest of its pieces: more shapes for the same part */
        for (i32 k = 1; k < pieces; ++k) {
            ub_pshape *t = &ph->shapes[ph->shape_count];
            *t = *s;
            t->hull = &hulls[k];
            t->next_piece = -1;
            ph->shapes[ph->shape_count - 1].next_piece = ph->shape_count;
            shape_update(ph, t);
            ph->shape_count++;
        }
    }
    ph->bodies[0].shape_count = ph->static_count = ph->shape_count;

    /* moving groups: one body each, its shapes together */
    for (i32 i = 0; i < np && ok; ++i) {
        if (!flags[i]) continue;
        i32 root = uf_find(group, i);
        u8 gf = flags[root];
        if (!(gf & 4) && (gf & 2)) continue;
        if (body_of[root] < 0) {
            /* a new group: gather all its parts (they follow in index order) */
            i32 b = new_body(ph, (gf & 4) ? UB_BODY_KINEMATIC : UB_BODY_DYNAMIC);
            if (b < 0) { ok = false; break; }
            body_of[root] = b;
            ub_body *body = &ph->bodies[b];
            body->first_shape = ph->shape_count;
            const ub_part *first = &w->parts[i];
            body->q = quat_from_mat3(&first->rot);
            body->rot = quat_to_mat3(body->q);
            /* mass properties */
            f32 mass = 0;
            vec3 com = v3(0, 0, 0);
            for (i32 j = i; j < np; ++j) {
                if (!flags[j] || uf_find(group, j) != root) continue;
                const ub_part *p = &w->parts[j];
                f32 vol, dens, fr, re;
                vec3 c;
                mat3 inertia;
                ub_shape_mass((ub_shape)(p->shape < UB_SHAPE_COUNT ? p->shape : 0), p->size, &vol, &c, &inertia);
                ub_material_physics(p->material, &dens, &fr, &re);
                f32 m = UB_MAX(vol * dens, 1e-4f);
                mass += m;
                com = v3_madd(com, v3_add(p->pos, mat3_apply(&p->rot, c)), m);
            }
            com = v3_scale(com, 1.0f / mass);
            mat3 I = mat3_zero();
            for (i32 j = i; j < np; ++j) {
                if (!flags[j] || uf_find(group, j) != root) continue;
                const ub_part *p = &w->parts[j];
                f32 vol, dens, fr, re;
                vec3 c;
                mat3 inertia;
                ub_shape_mass((ub_shape)(p->shape < UB_SHAPE_COUNT ? p->shape : 0), p->size, &vol, &c, &inertia);
                ub_material_physics(p->material, &dens, &fr, &re);
                f32 m = UB_MAX(vol * dens, 1e-4f);
                mat3 pr = quat_to_mat3(quat_from_mat3(&p->rot));
                mat3 lrot = mat3_mul_tn(&body->rot, &pr);       /* part -> body */
                mat3 il = mat3_scale(&inertia, dens);
                mat3 ib = mat3_rotate_tensor(&lrot, &il);
                vec3 d = mat3_apply_t(&body->rot, v3_sub(v3_add(p->pos, mat3_apply(&p->rot, c)), com));
                f32 dd = v3_dot(d, d);
                /* parallel axis: m (|d|^2 E - d dᵀ) */
                ib.x = v3_add(ib.x, v3(m * (dd - d.x * d.x), -m * d.y * d.x, -m * d.z * d.x));
                ib.y = v3_add(ib.y, v3(-m * d.x * d.y, m * (dd - d.y * d.y), -m * d.z * d.y));
                ib.z = v3_add(ib.z, v3(-m * d.x * d.z, -m * d.y * d.z, m * (dd - d.z * d.z)));
                I = mat3_add(&I, &ib);

                ub_pshape *s = &ph->shapes[ph->shape_count];
                memset(s, 0, sizeof *s);
                s->next_piece = -1;
                s->body = b;
                s->part = j;
                s->shape = p->shape < UB_SHAPE_COUNT ? p->shape : UB_SHAPE_BOX;
                s->flags = (p->flags & UB_PART_CAN_COLLIDE) ? UB_SHAPE_COLLIDES : 0;
                s->size = p->size;
                s->radius = 0.5f * UB_MIN(p->size.x, UB_MIN(p->size.y, p->size.z));
                s->bound = 0.5f * v3_length(p->size);
                s->lpos = mat3_apply_t(&body->rot, v3_sub(p->pos, com));
                s->lrot = lrot;
                s->friction = fr;
                s->restitution = re;
                ph->part_shape[j] = ph->shape_count++;
                body->shape_count++;
            }
            /* thin plates have almost no inertia about one axis; keep it sane */
            f32 floor_i = mass * 0.01f;
            I.x.x += floor_i;
            I.y.y += floor_i;
            I.z.z += floor_i;
            body->mass = mass;
            body->pos = body->prev_pos = body->target_pos = com;
            body->prev_q = body->target_q = body->q;
            if (body->type == UB_BODY_DYNAMIC) {
                body->inv_mass = 1.0f / mass;
                body->inv_inertia_local = mat3_inverse(&I);
            }
            update_inertia(body);
            for (i32 k = 0; k < body->shape_count; ++k) {
                ub_pshape *s = &ph->shapes[body->first_shape + k];
                shape_update(ph, s);
                if ((s->flags & UB_SHAPE_COLLIDES) && !dyn_add(ph, body->first_shape + k)) ok = false;
            }
        }
    }
    if (!ok) goto done;

    /* the static tree */
    ph->static_ids = (i32 *)plat_alloc(sizeof(i32) * (size_t)(ph->static_count + 1));
    ph->nodes = (ub_bvh_node *)plat_alloc(sizeof(ub_bvh_node) * (size_t)(2 * ph->static_count + 2));
    if (!ph->static_ids || !ph->nodes) { ok = false; goto done; }
    for (i32 i = 0; i < ph->static_count; ++i) ph->static_ids[i] = i;
    ph->node_count = 1;
    if (ph->static_count > 0) bvh_build(ph, 0, 0, ph->static_count, 0);
    else ph->node_count = 0;
    ph->stats.welds = welds;

done:
    plat_free(group);
    plat_free(inst_of);
    plat_free(flags);
    plat_free(body_of);
    if (!ok) ub_phys_free(ph);
    return ok;
}

void ub_phys_free(ub_physics *ph)
{
    plat_free(ph->joints);
    plat_free(ph->force); /* (torque, the drives share it) */
    plat_free(ph->free_body);
    plat_free(ph->bodies);
    plat_free(ph->shapes);
    plat_free(ph->part_shape);
    plat_free(ph->nodes);
    plat_free(ph->static_ids);
    plat_free(ph->dyn);
    plat_free(ph->pairs);
    plat_free(ph->pairs_tmp);
    plat_free(ph->man);
    plat_free(ph->man_prev);
    plat_free(ph->scratch);
    plat_free(ph->scratch_f);
    plat_free(ph->touches);
    plat_free(ph->sensor_keys);
    plat_free(ph->sensor_prev);
    memset(ph, 0, sizeof *ph);
}

i32 ub_phys_add_character(ub_physics *ph, vec3 pos, const vec3 *offsets, int count, f32 radius, f32 mass)
{
    i32 b = new_body(ph, UB_BODY_CHARACTER);
    if (b < 0 || !grow((void **)&ph->shapes, &ph->shape_cap, ph->shape_count + count, sizeof(ub_pshape)))
        return -1;
    ub_body *body = &ph->bodies[b];
    body->pos = body->prev_pos = pos;
    body->mass = mass;
    body->inv_mass = 1.0f / mass;
    body->first_shape = ph->shape_count;
    for (int i = 0; i < count; ++i) {
        ub_pshape *s = &ph->shapes[ph->shape_count];
        memset(s, 0, sizeof *s);
        s->next_piece = -1;
        s->body = b;
        s->part = -1;
        s->shape = UB_SHAPE_BALL;
        s->flags = UB_SHAPE_COLLIDES;
        s->radius = radius;
        s->size = v3(2 * radius, 2 * radius, 2 * radius);
        s->bound = radius;
        s->lpos = offsets[i];
        s->lrot = mat3_identity();
        s->friction = 0;      /* slides along walls */
        s->restitution = 0;
        shape_update(ph, s);
        if (!dyn_add(ph, ph->shape_count)) return -1;
        ph->shape_count++;
        body->shape_count++;
    }
    return b;
}

void ub_phys_teleport(ub_physics *ph, i32 body, vec3 pos)
{
    ub_body *b = &ph->bodies[body];
    b->pos = b->prev_pos = pos;
    b->v = b->w = v3(0, 0, 0);
    for (i32 k = 0; k < b->shape_count; ++k) shape_update(ph, &ph->shapes[b->first_shape + k]);
}

void ub_phys_set_position(ub_physics *ph, i32 body, vec3 pos)
{
    ub_body *b = &ph->bodies[body];
    b->pos = pos;
    for (i32 k = 0; k < b->shape_count; ++k) shape_update(ph, &ph->shapes[b->first_shape + k]);
}

/* ----------------------------------------------------- run-time changes */

void ub_phys_set_gravity(ub_physics *ph, f32 gravity)
{
    ph->gravity = v3(0, -gravity, 0);
    ph->up = gravity >= 0 ? v3(0, 1, 0) : v3(0, -1, 0);
    for (i32 i = 1; i < ph->body_count; ++i) ub_phys_wake(ph, i);
}

static void shape_from_part(ub_pshape *s, const ub_part *p)
{
    s->shape = p->shape < UB_SHAPE_COUNT ? p->shape : UB_SHAPE_BOX;
    s->size = p->size;
    s->radius = 0.5f * UB_MIN(p->size.x, UB_MIN(p->size.y, p->size.z));
    s->bound = 0.5f * v3_length(p->size);
    f32 dens;
    ub_material_physics(p->material, &dens, &s->friction, &s->restitution);
}

/* Mass, centre of mass and inertia from the body's shapes where they are
 * now. Keeps the body's orientation; moves its origin to the new centre. */
static void body_set_mass(ub_physics *ph, ub_body *b)
{
    f32 mass = 0;
    vec3 com = v3(0, 0, 0);
    for (i32 k = 0; k < b->shape_count; ++k) {
        const ub_pshape *s = &ph->shapes[b->first_shape + k];
        if (s->part < 0) continue;
        f32 vol, dens, fr, re;
        vec3 c;
        mat3 in;
        ub_shape_mass((ub_shape)s->shape, s->size, &vol, &c, &in);
        ub_material_physics(ph->world->parts[s->part].material, &dens, &fr, &re);
        f32 m = UB_MAX(vol * dens, 1e-4f);
        mass += m;
        com = v3_madd(com, v3_add(s->wpos, mat3_apply(&s->wrot, c)), m);
    }
    if (mass <= 0) return;
    com = v3_scale(com, 1.0f / mass);
    mat3 I = mat3_zero();
    for (i32 k = 0; k < b->shape_count; ++k) {
        ub_pshape *s = &ph->shapes[b->first_shape + k];
        if (s->part < 0) continue;
        f32 vol, dens, fr, re;
        vec3 c;
        mat3 in;
        ub_shape_mass((ub_shape)s->shape, s->size, &vol, &c, &in);
        ub_material_physics(ph->world->parts[s->part].material, &dens, &fr, &re);
        f32 m = UB_MAX(vol * dens, 1e-4f);
        mat3 il = mat3_scale(&in, dens);
        mat3 ib = mat3_rotate_tensor(&s->lrot, &il);
        vec3 d = mat3_apply_t(&b->rot, v3_sub(v3_add(s->wpos, mat3_apply(&s->wrot, c)), com));
        f32 dd = v3_dot(d, d);
        ib.x = v3_add(ib.x, v3(m * (dd - d.x * d.x), -m * d.y * d.x, -m * d.z * d.x));
        ib.y = v3_add(ib.y, v3(-m * d.x * d.y, m * (dd - d.y * d.y), -m * d.z * d.y));
        ib.z = v3_add(ib.z, v3(-m * d.x * d.z, -m * d.y * d.z, m * (dd - d.z * d.z)));
        I = mat3_add(&I, &ib);
        s->lpos = mat3_apply_t(&b->rot, v3_sub(s->wpos, com));
    }
    f32 floor_i = mass * 0.01f;
    I.x.x += floor_i;
    I.y.y += floor_i;
    I.z.z += floor_i;
    b->pos = b->prev_pos = com;
    b->mass = mass;
    b->inv_mass = 1.0f / mass;
    b->inv_inertia_local = mat3_inverse(&I);
    update_inertia(b);
}

static bool close3(vec3 a, vec3 b) { return v3_length2(v3_sub(a, b)) < 1e-8f; }
static bool close_rot(const mat3 *a, const mat3 *b)
{
    return close3(a->x, b->x) && close3(a->y, b->y) && close3(a->z, b->z);
}

static void set_flags(ub_physics *ph, i32 si, u8 flags)
{
    ph->shapes[si].flags = flags;
    if (flags & (UB_SHAPE_COLLIDES | UB_SHAPE_SENSOR)) dyn_add(ph, si);
    else dyn_remove(ph, si);
}

void ub_phys_sync_part(ub_physics *ph, i32 part, bool alive, bool sensor)
{
    ub_world *w = ph->world;
    if (part < 0 || part >= w->part_count) return;
    if (part >= ph->part_count) { /* new parts since the physics was built */
        i32 *np = (i32 *)plat_alloc(sizeof(i32) * (size_t)(w->part_count + 1));
        if (!np) return;
        for (i32 i = 0; i < w->part_count; ++i) np[i] = i < ph->part_count ? ph->part_shape[i] : -1;
        plat_free(ph->part_shape);
        ph->part_shape = np;
        ph->part_count = w->part_count;
    }
    const ub_part *p = &w->parts[part];
    alive = alive && !(p->flags & (UB_PART_REMOVED | UB_PART_CARRIED));
    bool collide = alive && (p->flags & UB_PART_CAN_COLLIDE);
    bool touch = alive && sensor && !collide;
    bool anchored = (p->flags & UB_PART_ANCHORED) != 0;
    u8 want = (u8)((collide ? UB_SHAPE_COLLIDES : 0) | (touch ? UB_SHAPE_SENSOR : 0));
    mat3 prot = quat_to_mat3(quat_from_mat3(&p->rot));
    i32 si = ph->part_shape[part];
    if (si >= 0) {
        ub_pshape *s = &ph->shapes[si];
        bool same_geom = s->shape == p->shape && close3(s->size, p->size);
        if (s->body == 0) {
            if (alive && anchored && !touch && same_geom && close3(s->wpos, p->pos) && close_rot(&s->wrot, &prot)) {
                for (i32 k = si; k >= 0; k = ph->shapes[k].next_piece) {
                    ph->shapes[k].flags = want; /* just solid or not: stays in the static tree */
                    shape_from_part(&ph->shapes[k], p);
                }
                return;
            }
            for (i32 k = si; k >= 0; k = ph->shapes[k].next_piece) {
                ph->shapes[k].flags = 0; /* leaves the static tree for good (as one box, if it comes back) */
                ph->shapes[k].part = -1;
            }
            ph->part_shape[part] = -1;
            si = -1;
        } else {
            i32 bi = s->body;
            ub_body *b = &ph->bodies[bi];
            if (!alive) {
                set_flags(ph, si, 0);
                s->part = -1;
                ph->part_shape[part] = -1;
                bool any = false;
                for (i32 k = 0; k < b->shape_count; ++k) any |= ph->shapes[b->first_shape + k].part >= 0;
                if (!any) {
                    /* nothing left in it: what it slept with wakes (which
                     * also unlinks it from that group), and a one-shape
                     * body is reused after the next step */
                    ub_phys_wake(ph, bi);
                    b->fallen = 1;
                    b->awake = 0;
                    b->sleep_next = b->sleep_head = -1;
                    if (b->shape_count == 1 && b->type != UB_BODY_CHARACTER &&
                        grow((void **)&ph->free_body, &ph->free_body_cap, ph->free_body_count + 1, sizeof(i32)))
                        ph->free_body[ph->free_body_count++] = bi;
                }
                return;
            }
            shape_from_part(s, p);
            set_flags(ph, si, want);
            if (anchored && b->type == UB_BODY_DYNAMIC) {
                b->type = UB_BODY_KINEMATIC;
                b->v = b->w = v3(0, 0, 0);
                b->inv_mass = 0;
                b->inv_inertia_local = mat3_zero();
                b->has_target = 0;
                update_inertia(b);
            } else if (!anchored && b->type == UB_BODY_KINEMATIC) {
                b->type = UB_BODY_DYNAMIC;
                b->has_target = 0;
                body_set_mass(ph, b);
                b->awake = 0;
                ub_phys_wake(ph, s->body);
            } else if (!same_geom && b->type == UB_BODY_DYNAMIC) {
                body_set_mass(ph, b);
            }
            /* moved by the script? the body follows */
            vec3 cur_pos = v3_add(b->pos, mat3_apply(&b->rot, s->lpos));
            mat3 cur_rot = mat3_mul(&b->rot, &s->lrot);
            if (!close3(cur_pos, p->pos) || !close_rot(&cur_rot, &prot)) {
                mat3 lt = mat3_transpose(&s->lrot);
                mat3 brot = mat3_mul(&prot, &lt);
                vec3 bpos = v3_sub(p->pos, mat3_apply(&brot, s->lpos));
                if (b->type == UB_BODY_KINEMATIC && v3_length(v3_sub(bpos, b->pos)) < 20.0f) {
                    ub_phys_set_target(ph, s->body, bpos, &brot); /* glides, pushing things */
                } else {
                    b->pos = b->prev_pos = bpos;
                    b->q = b->prev_q = quat_from_mat3(&brot);
                    b->rot = quat_to_mat3(b->q);
                    update_inertia(b);
                    if (b->type == UB_BODY_DYNAMIC) ub_phys_wake(ph, s->body);
                }
            }
            for (i32 k = 0; k < b->shape_count; ++k) shape_update(ph, &ph->shapes[b->first_shape + k]);
            return;
        }
    }
    if (!alive || (anchored && !collide && !touch)) return; /* nothing to simulate: only drawn */
    /* a body of its own: an emptied one if there is one (keeps its shape slot) */
    i32 bi, shape_slot;
    if (ph->free_body_ready > 0) {
        bi = ph->free_body[0];
        ph->free_body[0] = ph->free_body[--ph->free_body_ready];
        ph->free_body[ph->free_body_ready] = ph->free_body[--ph->free_body_count];
        shape_slot = ph->bodies[bi].first_shape;
        ub_body *rb = &ph->bodies[bi];
        memset(rb, 0, sizeof *rb);
        rb->type = (u8)(anchored ? UB_BODY_KINEMATIC : UB_BODY_DYNAMIC);
        rb->awake = 1;
        rb->sleep_next = rb->sleep_head = -1;
    } else {
        bi = new_body(ph, anchored ? UB_BODY_KINEMATIC : UB_BODY_DYNAMIC);
        if (bi < 0 || !grow((void **)&ph->shapes, &ph->shape_cap, ph->shape_count + 1, sizeof(ub_pshape))) return;
        shape_slot = ph->shape_count++;
    }
    ub_body *b = &ph->bodies[bi];
    b->q = b->prev_q = b->target_q = quat_from_mat3(&prot);
    b->rot = quat_to_mat3(b->q);
    b->pos = b->prev_pos = b->target_pos = p->pos;
    b->first_shape = shape_slot;
    b->shape_count = 1;
    b->woke_step = ph->stats.steps;
    ub_pshape *s = &ph->shapes[shape_slot];
    memset(s, 0, sizeof *s);
    s->next_piece = -1;
    s->body = bi;
    s->part = part;
    shape_from_part(s, p);
    s->lpos = v3(0, 0, 0);
    s->lrot = mat3_identity();
    ph->part_shape[part] = shape_slot;
    shape_update(ph, s);
    if (b->type == UB_BODY_DYNAMIC) body_set_mass(ph, b);
    else update_inertia(b);
    shape_update(ph, s);
    set_flags(ph, ph->part_shape[part], want);
}

void ub_phys_set_target(ub_physics *ph, i32 body, vec3 pos, const mat3 *rot)
{
    ub_body *b = &ph->bodies[body];
    if (b->type != UB_BODY_KINEMATIC) return;
    b->target_pos = pos;
    b->target_q = quat_from_mat3(rot);
    b->has_target = 1;
}

void ub_phys_wake(ub_physics *ph, i32 body)
{
    ub_body *b = &ph->bodies[body];
    if (b->type != UB_BODY_DYNAMIC || b->awake || b->fallen) return;
    if (b->sleep_head < 0) b->sleep_next = -1; /* not part of a group */
    i32 i = b->sleep_head >= 0 ? b->sleep_head : body;
    while (i >= 0) {
        ub_body *x = &ph->bodies[i];
        i32 next = x->sleep_next;
        x->awake = 1;
        x->woke_step = ph->stats.steps;
        x->sleep_time = 0;
        x->sleep_next = x->sleep_head = -1;
        i = next;
    }
    ph->all_asleep_logged = false;
}

void ub_phys_apply_impulse(ub_physics *ph, i32 body, vec3 impulse, vec3 point)
{
    ub_body *b = &ph->bodies[body];
    if (b->type != UB_BODY_DYNAMIC || b->fallen) return;
    ub_phys_wake(ph, body);
    b->v = v3_madd(b->v, impulse, b->inv_mass);
    b->w = v3_add(b->w, mat3_apply(&b->inv_inertia, v3_cross(v3_sub(point, b->pos), impulse)));
}

vec3 ub_phys_point_velocity(const ub_physics *ph, i32 body, vec3 point)
{
    const ub_body *b = &ph->bodies[body];
    return v3_add(b->v, v3_cross(b->w, v3_sub(point, b->pos)));
}

/* ----------------------------------------------------------- broadphase */

static bool add_pair(ub_physics *ph, i32 a, i32 b)
{
    if (!grow((void **)&ph->pairs, &ph->pair_cap, ph->pair_count + 1, sizeof(u64))) return false;
    if (a > b) { i32 t = a; a = b; b = t; }
    ph->pairs[ph->pair_count++] = ((u64)(u32)a << 32) | (u32)b;
    return true;
}

static bool pair_wanted(const ub_physics *ph, const ub_pshape *a, const ub_pshape *b)
{
    if (a->body == b->body) return false;
    if (!((a->flags | b->flags) & UB_SHAPE_COLLIDES)) return false;   /* two sensors */
    if (!(a->flags & (UB_SHAPE_COLLIDES | UB_SHAPE_SENSOR)) || !(b->flags & (UB_SHAPE_COLLIDES | UB_SHAPE_SENSOR)))
        return false;
    const ub_body *ba = &ph->bodies[a->body], *bb = &ph->bodies[b->body];
    if (ba->fallen || bb->fallen) return false;
    bool da = ba->type == UB_BODY_DYNAMIC || ba->type == UB_BODY_CHARACTER;
    bool db = bb->type == UB_BODY_DYNAMIC || bb->type == UB_BODY_CHARACTER;
    if (!da && !db) return false;                          /* kinematic vs kinematic */
    return body_active(ba) || body_active(bb);              /* not two sleepers */
}

static void broadphase(ub_physics *ph)
{
    ph->pair_count = 0;
    /* insertion sort by low x (the order barely changes between steps) */
    i32 *d = ph->dyn;
    for (i32 i = 1; i < ph->dyn_count; ++i) {
        i32 s = d[i];
        f32 x = ph->shapes[s].lo.x;
        i32 j = i - 1;
        while (j >= 0 && ph->shapes[d[j]].lo.x > x) {
            d[j + 1] = d[j];
            --j;
        }
        d[j + 1] = s;
    }
    for (i32 i = 0; i < ph->dyn_count; ++i) {
        const ub_pshape *a = &ph->shapes[d[i]];
        for (i32 j = i + 1; j < ph->dyn_count; ++j) {
            const ub_pshape *b = &ph->shapes[d[j]];
            if (b->lo.x > a->hi.x) break;
            if (box_overlap(a->lo, a->hi, b->lo, b->hi) && pair_wanted(ph, a, b)) add_pair(ph, d[i], d[j]);
        }
    }
    /* moving shapes against the static tree */
    if (ph->node_count > 0) {
        i32 stack[64];
        for (i32 i = 0; i < ph->dyn_count; ++i) {
            const ub_pshape *a = &ph->shapes[d[i]];
            const ub_body *b = &ph->bodies[a->body];
            if (b->type == UB_BODY_KINEMATIC || !body_active(b)) continue;
            int sp = 0;
            stack[sp++] = 0;
            while (sp > 0) {
                const ub_bvh_node *n = &ph->nodes[stack[--sp]];
                if (!box_overlap(a->lo, a->hi, n->lo, n->hi)) continue;
                if (n->count > 0) {
                    for (i32 k = 0; k < n->count; ++k) {
                        i32 sid = ph->static_ids[n->left + k];
                        const ub_pshape *s = &ph->shapes[sid];
                        if ((s->flags & UB_SHAPE_COLLIDES) && (a->flags & UB_SHAPE_COLLIDES) &&
                            box_overlap(a->lo, a->hi, s->lo, s->hi))
                            add_pair(ph, sid, d[i]);
                    }
                } else if (sp + 2 <= 64) {
                    stack[sp++] = n->left;
                    stack[sp++] = n->left + 1;
                }
            }
        }
    }
}

/* --------------------------------------------------------- narrowphase */

static void narrowphase(ub_physics *ph)
{
    /* swap manifold buffers: last step's become man_prev */
    ub_manifold *t = ph->man_prev;
    i32 tc = ph->man_prev_cap;
    ph->man_prev = ph->man;
    ph->man_prev_cap = ph->man_cap;
    ph->man_prev_count = ph->man_count;
    ph->man = t;
    ph->man_cap = tc;
    ph->man_count = 0;
    if (!grow((void **)&ph->man, &ph->man_cap, ph->pair_count + 1, sizeof(ub_manifold))) return;
    /* sensor pairs: this step's list becomes last step's */
    u64 *tk = ph->sensor_prev;
    i32 tkc = ph->sensor_prev_cap;
    ph->sensor_prev = ph->sensor_keys;
    ph->sensor_prev_cap = ph->sensor_cap;
    ph->sensor_prev_count = ph->sensor_count;
    ph->sensor_keys = tk;
    ph->sensor_cap = tkc;
    ph->sensor_count = 0;
    i32 old = 0, old_sensor = 0;
    for (i32 i = 0; i < ph->pair_count; ++i) {
        u64 key = ph->pairs[i];
        if (i > 0 && key == ph->pairs[i - 1]) continue; /* duplicate */
        i32 sa = (i32)(key >> 32), sb = (i32)(key & 0xFFFFFFFFu);
        const ub_pshape *A = &ph->shapes[sa], *B = &ph->shapes[sb];
        ub_body *ba = &ph->bodies[A->body], *bb = &ph->bodies[B->body];
        ub_cshape ca, cb;
        cshape(A, &ca);
        cshape(B, &cb);
        vec3 rel = v3_sub(bb->v, ba->v);
        f32 margin = MARGIN + (v3_length(rel) + v3_length(ba->w) * A->bound + v3_length(bb->w) * B->bound) * UB_PHYS_DT;
        vec3 n;
        ub_cpoint pts[UB_MAX_CONTACTS];
        bool sensor = !(A->flags & UB_SHAPE_COLLIDES) || !(B->flags & UB_SHAPE_COLLIDES);
        int count = ub_collide(&ca, &cb, sensor ? 0.0f : margin, &n, pts);
        if (count == 0) continue;
        if (sensor) { /* only report the touch, never push */
            if (!grow((void **)&ph->sensor_keys, &ph->sensor_cap, ph->sensor_count + 1, sizeof(u64))) continue;
            ph->sensor_keys[ph->sensor_count++] = key;
            while (old_sensor < ph->sensor_prev_count && ph->sensor_prev[old_sensor] < key) ++old_sensor;
            bool was = old_sensor < ph->sensor_prev_count && ph->sensor_prev[old_sensor] == key;
            if (!was && grow((void **)&ph->touches, &ph->touch_cap, ph->touch_count + 1, sizeof(ub_touch))) {
                ph->touches[ph->touch_count].a = A->part;
                ph->touches[ph->touch_count].b = B->part;
                ph->touch_count++;
            }
            continue;
        }

        /* a sleeper touched by something awake wakes up */
        bool touching = false;
        for (int k = 0; k < count; ++k) touching |= pts[k].sep < MARGIN;
        if (touching) {
            if (ba->type == UB_BODY_DYNAMIC && !ba->awake && body_active(bb)) ub_phys_wake(ph, A->body);
            if (bb->type == UB_BODY_DYNAMIC && !bb->awake && body_active(ba)) ub_phys_wake(ph, B->body);
        }

        ub_manifold *m = &ph->man[ph->man_count++];
        m->key = key;
        m->sa = sa;
        m->sb = sb;
        m->ba = A->body;
        m->bb = B->body;
        m->n = n;
        tangents(n, &m->t1, &m->t2);
        m->friction = ub_sqrtf(A->friction * B->friction);
        m->restitution = 0.5f * (A->restitution + B->restitution);
        m->count = count;
        while (old < ph->man_prev_count && ph->man_prev[old].key < key) ++old;
        const ub_manifold *pm = old < ph->man_prev_count && ph->man_prev[old].key == key ? &ph->man_prev[old] : NULL;
        /* a new touch: really touching now, and not last step (contacts
         * that come back because something just woke are not new) */
        m->touching = 0;
        for (int k = 0; k < count; ++k) m->touching |= pts[k].sep < 0.02f;
        bool was_touching = pm && pm->touching;
        bool just_woke = ba->woke_step == ph->stats.steps || bb->woke_step == ph->stats.steps;
        if (m->touching && !was_touching && !just_woke &&
            grow((void **)&ph->touches, &ph->touch_cap, ph->touch_count + 1, sizeof(ub_touch))) {
            ph->touches[ph->touch_count].a = A->part;
            ph->touches[ph->touch_count].b = B->part;
            ph->touch_count++;
        }
        if (pm && v3_dot(pm->n, n) < 0.9f) pm = NULL;
        m->jt1 = m->jt2 = m->jtw = m->jr1 = m->jr2 = 0;
        if (pm) {
            m->jt1 = pm->jt1;
            m->jt2 = pm->jt2;
            m->jtw = pm->jtw;
            m->jr1 = pm->jr1;
            m->jr2 = pm->jr2;
        }
        m->roll = 0;
        if (A->shape == UB_SHAPE_BALL && A->part >= 0) m->roll = ROLLING_RESISTANCE * A->radius;
        if (B->shape == UB_SHAPE_BALL && B->part >= 0) m->roll = UB_MAX(m->roll, ROLLING_RESISTANCE * B->radius);
        u32 used = 0; /* each old contact passes its impulse on at most once */
        for (int k = 0; k < count; ++k) {
            contact *c = &m->c[k];
            memset(c, 0, sizeof *c);
            c->sep = pts[k].sep;
            c->la = mat3_apply_t(&ba->rot, v3_sub(pts[k].point, ba->pos));
            c->lb = mat3_apply_t(&bb->rot, v3_sub(pts[k].point, bb->pos));
            c->ra = v3_sub(pts[k].point, ba->pos);
            c->rb = v3_sub(pts[k].point, bb->pos);
            if (pm) { /* warm start from the matching contact of last step */
                for (int j = 0; j < pm->count; ++j) {
                    if (!(used & (1u << j)) && v3_length2(v3_sub(pm->c[j].la, c->la)) < MATCH_DIST2 &&
                        v3_length2(v3_sub(pm->c[j].lb, c->lb)) < MATCH_DIST2) {
                        c->jn = pm->c[j].jn;
                        used |= 1u << j;
                        break;
                    }
                }
            }
        }
        vec3 centre = v3(0, 0, 0);
        for (int k = 0; k < count; ++k) centre = v3_add(centre, pts[k].point);
        centre = v3_scale(centre, 1.0f / (f32)count);
        f32 rad = 0;
        for (int k = 0; k < count; ++k) rad += v3_length(v3_sub(pts[k].point, centre));
        m->radius = UB_MAX(rad / (f32)count, m->roll); /* a ball's one point still resists spinning */
        m->ra = v3_sub(centre, ba->pos);
        m->rb = v3_sub(centre, bb->pos);
    }
}

/* -------------------------------------------------------------- solver */

static void apply(ub_body *a, ub_body *b, vec3 ra, vec3 rb, vec3 p)
{
    a->v = v3_madd(a->v, p, -a->inv_mass);
    a->w = v3_sub(a->w, mat3_apply(&a->inv_inertia, v3_cross(ra, p)));
    b->v = v3_madd(b->v, p, b->inv_mass);
    b->w = v3_add(b->w, mat3_apply(&b->inv_inertia, v3_cross(rb, p)));
}

static f32 eff_mass(const ub_body *a, const ub_body *b, vec3 ra, vec3 rb, vec3 n)
{
    vec3 ia = v3_cross(mat3_apply(&a->inv_inertia, v3_cross(ra, n)), ra);
    vec3 ib = v3_cross(mat3_apply(&b->inv_inertia, v3_cross(rb, n)), rb);
    f32 k = a->inv_mass + b->inv_mass + v3_dot(n, v3_add(ia, ib));
    return k > 1e-12f ? 1.0f / k : 0.0f;
}

static vec3 rel_vel(const ub_body *a, const ub_body *b, vec3 ra, vec3 rb)
{
    vec3 va = v3_add(a->v, v3_cross(a->w, ra)), vb = v3_add(b->v, v3_cross(b->w, rb));
    return v3_sub(vb, va);
}

static f32 twist_mass(const ub_body *a, const ub_body *b, vec3 n)
{
    f32 k = v3_dot(n, mat3_apply(&a->inv_inertia, n)) + v3_dot(n, mat3_apply(&b->inv_inertia, n));
    return k > 1e-12f ? 1.0f / k : 0.0f;
}

static void apply_twist(ub_body *a, ub_body *b, vec3 n, f32 j)
{
    a->w = v3_sub(a->w, mat3_apply(&a->inv_inertia, v3_scale(n, j)));
    b->w = v3_add(b->w, mat3_apply(&b->inv_inertia, v3_scale(n, j)));
}

typedef struct {
    f32 inv_h;
    f32 bias_rate, mass_scale, impulse_scale;
} soft;

/* Soft contact: a stiff, heavily damped spring instead of a rigid
 * constraint (Catto, "Solver2D" / Box2D 3), made from a frequency and a
 * damping ratio for substep h. */
static soft make_soft(f32 h)
{
    soft k;
    f32 hz = UB_MIN(CONTACT_HERTZ, 0.25f / h);
    f32 omega = UB_TWO_PI * hz;
    f32 a1 = 2.0f * CONTACT_DAMPING + h * omega;
    f32 a2 = h * omega * a1;
    f32 a3 = 1.0f / (1.0f + a2);
    k.inv_h = 1.0f / h;
    k.bias_rate = omega / a1;
    k.mass_scale = a2 * a3;
    k.impulse_scale = a3;
    return k;
}

static void prepare(ub_physics *ph)
{
    for (i32 i = 0; i < ph->man_count; ++i) {
        ub_manifold *m = &ph->man[i];
        ub_body *a = &ph->bodies[m->ba], *b = &ph->bodies[m->bb];
        for (int k = 0; k < m->count; ++k) {
            contact *c = &m->c[k];
            c->mn = eff_mass(a, b, c->ra, c->rb, m->n);
            c->vn0 = v3_dot(rel_vel(a, b, c->ra, c->rb), m->n);
            c->jn_max = 0;
        }
        m->mt1 = eff_mass(a, b, m->ra, m->rb, m->t1);
        m->mt2 = eff_mass(a, b, m->ra, m->rb, m->t2);
        m->mtw = twist_mass(a, b, m->n);
        m->mc = eff_mass(a, b, m->ra, m->rb, m->n);
        m->vn0 = v3_dot(rel_vel(a, b, m->ra, m->rb), m->n);
        if (m->roll > 0) {
            m->mr1 = twist_mass(a, b, m->t1);
            m->mr2 = twist_mass(a, b, m->t2);
        }
    }
}

static void warm_start(ub_physics *ph)
{
    for (i32 i = 0; i < ph->man_count; ++i) {
        ub_manifold *m = &ph->man[i];
        ub_body *a = &ph->bodies[m->ba], *b = &ph->bodies[m->bb];
        for (int k = 0; k < m->count; ++k) apply(a, b, m->c[k].ra, m->c[k].rb, v3_scale(m->n, m->c[k].jn));
        apply(a, b, m->ra, m->rb, v3_add(v3_scale(m->t1, m->jt1), v3_scale(m->t2, m->jt2)));
        apply_twist(a, b, m->n, m->jtw);
        if (m->roll > 0) {
            apply_twist(a, b, m->t1, m->jr1);
            apply_twist(a, b, m->t2, m->jr2);
        }
    }
}

/* One pass over the contacts. With bias, overlap is pushed out softly;
 * without (the relax pass), only velocities are fixed, so the push does
 * not linger as bounce. */
static void solve_contacts(ub_physics *ph, const soft *sf, bool use_bias)
{
    for (i32 i = 0; i < ph->man_count; ++i) {
        ub_manifold *m = &ph->man[i];
        ub_body *a = &ph->bodies[m->ba], *b = &ph->bodies[m->bb];
        f32 total = 0, sep[UB_MAX_CONTACTS], avg = 0;
        for (int k = 0; k < m->count; ++k) {
            /* separation now: the anchors moved with their bodies */
            const contact *c = &m->c[k];
            vec3 pa = v3_add(a->pos, mat3_apply(&a->rot, c->la)), pb = v3_add(b->pos, mat3_apply(&b->rot, c->lb));
            sep[k] = c->sep + v3_dot(m->n, v3_sub(pb, pa));
            avg += sep[k];
        }
        avg /= (f32)m->count;
        f32 vc = m->count > 1 ? v3_dot(rel_vel(a, b, m->ra, m->rb), m->n) : 0;
        if (vc < -IMPACT_SPEED) {
            /* An impact: first stop the patch as a whole, shared out evenly,
             * so a hard landing is caught flat. One corner at a time would let
             * the first corner take the blow and set the part spinning. */
            f32 bias = avg > 0 ? avg * sf->inv_h : 0;
            f32 lc = -m->mc * (vc + bias);
            if (lc > 0) {
                f32 each = lc / (f32)m->count;
                for (int k = 0; k < m->count; ++k) m->c[k].jn += each;
                apply(a, b, m->ra, m->rb, v3_scale(m->n, lc));
            }
        }
        for (int k = 0; k < m->count; ++k) {
            contact *c = &m->c[k];
            f32 s = sep[k];
            f32 bias = 0, ms = 1, is = 0;
            if (s > 0) {
                bias = s * sf->inv_h;                     /* may close the gap, no more */
            } else if (use_bias) {
                bias = UB_MAX(sf->bias_rate * s, -MAX_PUSH);
                ms = sf->mass_scale;
                is = sf->impulse_scale;
            }
            f32 vn = v3_dot(rel_vel(a, b, c->ra, c->rb), m->n);
            f32 ln = -c->mn * ms * (vn + bias) - is * c->jn;
            f32 nn = UB_MAX(c->jn + ln, 0.0f);            /* push, never pull */
            ln = nn - c->jn;
            c->jn = nn;
            c->jn_max = UB_MAX(c->jn_max, nn);
            total += nn;
            apply(a, b, c->ra, c->rb, v3_scale(m->n, ln));
        }
        /* friction at the patch centre, limited by the total pressing impulse */
        f32 lim = m->friction * total;
        vec3 dv = rel_vel(a, b, m->ra, m->rb);
        f32 n1 = m->jt1 - m->mt1 * v3_dot(dv, m->t1), n2 = m->jt2 - m->mt2 * v3_dot(dv, m->t2);
        f32 len = ub_sqrtf(n1 * n1 + n2 * n2);
        if (len > lim && len > 0) { /* circular friction cone */
            n1 *= lim / len;
            n2 *= lim / len;
        }
        f32 l1 = n1 - m->jt1, l2 = n2 - m->jt2;
        m->jt1 = n1;
        m->jt2 = n2;
        apply(a, b, m->ra, m->rb, v3_add(v3_scale(m->t1, l1), v3_scale(m->t2, l2)));
        /* twist: stops things spinning in place on a surface */
        f32 tl = lim * m->radius;
        f32 nw = ub_clampf(m->jtw - m->mtw * v3_dot(v3_sub(b->w, a->w), m->n), -tl, tl);
        f32 lw = nw - m->jtw;
        m->jtw = nw;
        apply_twist(a, b, m->n, lw);
        if (m->roll > 0) { /* rolling resistance: balls slow down and stop */
            vec3 wr = v3_sub(b->w, a->w);
            f32 r1 = m->jr1 - m->mr1 * v3_dot(wr, m->t1), r2 = m->jr2 - m->mr2 * v3_dot(wr, m->t2);
            f32 rl = ub_sqrtf(r1 * r1 + r2 * r2), rlim = m->roll * total;
            if (rl > rlim && rl > 0) {
                r1 *= rlim / rl;
                r2 *= rlim / rl;
            }
            apply_twist(a, b, m->t1, r1 - m->jr1);
            apply_twist(a, b, m->t2, r2 - m->jr2);
            m->jr1 = r1;
            m->jr2 = r2;
        }
    }
}

/* Bounce, once per step: contacts that were hit fast enough get the
 * material's share of their approach speed back. */
static void restitution(ub_physics *ph)
{
    for (i32 i = 0; i < ph->man_count; ++i) {
        ub_manifold *m = &ph->man[i];
        if (m->restitution <= 0) continue;
        ub_body *a = &ph->bodies[m->ba], *b = &ph->bodies[m->bb];
        if (m->count > 1 && m->vn0 < -BOUNCE_SPEED) { /* evenly first, as in solve_contacts */
            f32 vn = v3_dot(rel_vel(a, b, m->ra, m->rb), m->n);
            f32 lc = -m->mc * (vn + m->restitution * m->vn0);
            if (lc > 0) {
                for (int k = 0; k < m->count; ++k) m->c[k].jn += lc / (f32)m->count;
                apply(a, b, m->ra, m->rb, v3_scale(m->n, lc));
            }
        }
        for (int k = 0; k < m->count; ++k) {
            contact *c = &m->c[k];
            if (c->vn0 > -BOUNCE_SPEED || c->jn_max <= 0) continue;
            f32 vn = v3_dot(rel_vel(a, b, c->ra, c->rb), m->n);
            f32 ln = -c->mn * (vn + m->restitution * c->vn0);
            f32 nn = UB_MAX(c->jn + ln, 0.0f);
            ln = nn - c->jn;
            c->jn = nn;
            apply(a, b, c->ra, c->rb, v3_scale(m->n, ln));
        }
    }
}

static bool moves(const ub_body *b)
{
    return !b->fallen && (b->type == UB_BODY_CHARACTER || b->type == UB_BODY_KINEMATIC ||
                          (b->type == UB_BODY_DYNAMIC && b->awake));
}

static void integrate_velocities(ub_physics *ph, f32 h)
{
    f32 damp = 1.0f / (1.0f + h * ANGULAR_DAMPING);
    for (i32 i = 1; i < ph->body_count; ++i) {
        ub_body *b = &ph->bodies[i];
        if (b->type != UB_BODY_DYNAMIC || !b->awake || b->fallen) continue;
        b->v = v3_madd(b->v, ph->gravity, h);
        b->w = v3_scale(b->w, damp);
    }
}

static void integrate_positions(ub_physics *ph, f32 h)
{
    for (i32 i = 1; i < ph->body_count; ++i) {
        ub_body *b = &ph->bodies[i];
        if (!moves(b)) continue;
        f32 sp = v3_length(b->v);
        if (sp > MAX_SPEED || !(sp == sp)) b->v = sp == sp ? v3_scale(b->v, MAX_SPEED / sp) : v3(0, 0, 0);
        f32 sw = v3_length(b->w);
        if (sw > MAX_SPIN || !(sw == sw)) b->w = sw == sw ? v3_scale(b->w, MAX_SPIN / sw) : v3(0, 0, 0);
        b->pos = v3_madd(b->pos, b->v, h);
        if (b->type == UB_BODY_CHARACTER) continue;
        b->q = quat_integrate(b->q, b->w, h);
        b->rot = quat_to_mat3(b->q);
        update_inertia(b);
    }
}

/* ------------------------------------------------------------- islands */

static void sleep_islands(ub_physics *ph)
{
    i32 n = ph->body_count;
    if (!grow((void **)&ph->scratch, &ph->scratch_cap, n * 2 + 1, sizeof(i32))) return;
    if (!grow((void **)&ph->scratch_f, &ph->scratch_f_cap, n + 1, sizeof(f32))) return;
    i32 *uf = ph->scratch, *head = ph->scratch + n;
    f32 *min_t = ph->scratch_f;
    for (i32 i = 0; i < n; ++i) {
        uf[i] = i;
        head[i] = -1;
        min_t[i] = 3e38f;
    }
    for (i32 i = 0; i < ph->man_count; ++i) {
        const ub_manifold *m = &ph->man[i];
        ub_body *a = &ph->bodies[m->ba], *b = &ph->bodies[m->bb];
        bool touching = false;
        for (int k = 0; k < m->count; ++k) touching |= m->c[k].sep < MARGIN;
        if (!touching) continue;
        bool da = a->type == UB_BODY_DYNAMIC, db = b->type == UB_BODY_DYNAMIC;
        if (da && db) {
            i32 ra = uf_find(uf, m->ba), rb = uf_find(uf, m->bb);
            if (ra != rb) uf[ra] = rb;
        } else {
            /* touching something that moves by itself keeps it awake */
            if (da && body_active(b) && b->type != UB_BODY_STATIC) a->sleep_time = 0;
            if (db && body_active(a) && a->type != UB_BODY_STATIC) b->sleep_time = 0;
        }
    }
    for (i32 i = 1; i < n; ++i) {
        const ub_body *b = &ph->bodies[i];
        if (b->type != UB_BODY_DYNAMIC || !b->awake || b->fallen) continue;
        i32 r = uf_find(uf, i);
        if (b->sleep_time < min_t[r]) min_t[r] = b->sleep_time;
    }
    for (i32 i = 1; i < n; ++i) {
        ub_body *b = &ph->bodies[i];
        if (b->type != UB_BODY_DYNAMIC || !b->awake || b->fallen) continue;
        i32 r = uf_find(uf, i);
        if (min_t[r] < SLEEP_TIME) continue;
        b->awake = 0;
        b->v = b->w = v3(0, 0, 0);
        b->prev_pos = b->pos;
        b->prev_q = b->q;
        b->sleep_next = head[r];
        head[r] = i;
    }
    /* each new sleeping group: every member remembers the group's first */
    for (i32 r = 1; r < n; ++r) {
        if (head[r] < 0) continue;
        for (i32 i = head[r]; i >= 0; i = ph->bodies[i].sleep_next) ph->bodies[i].sleep_head = head[r];
    }
}

/* ---------------------------------------------------------------- step */

static void write_back(ub_physics *ph, ub_body *b)
{
    for (i32 k = 0; k < b->shape_count; ++k) {
        ub_pshape *s = &ph->shapes[b->first_shape + k];
        if (s->part < 0) continue;
        ub_part *p = &ph->world->parts[s->part];
        p->pos = s->wpos;
        p->rot = s->wrot;
    }
}

void ub_pjoints_solve(ub_physics *ph, f32 h);          /* ub_pjoint.c */
void ub_pforces_apply(ub_physics *ph, f32 h);
void ub_pforces_clear(ub_physics *ph);
void ub_pjoints_keep_awake(ub_physics *ph);

void ub_phys_step(ub_physics *ph)
{
    const f32 dt = UB_PHYS_DT, h = dt / (f32)SUBSTEPS;
    ph->touch_count = 0;
    ph->free_body_ready = ph->free_body_count; /* emptied before this step: no contacts refer to them after it */
    for (i32 i = 1; i < ph->body_count; ++i) {
        ub_body *b = &ph->bodies[i];
        if (b->fallen) continue;
        b->prev_pos = b->pos;
        b->prev_q = b->q;
        if (b->type == UB_BODY_KINEMATIC) {
            if (b->has_target) {
                b->v = v3_scale(v3_sub(b->target_pos, b->pos), 1.0f / dt);
                b->w = quat_angular_velocity(b->q, b->target_q, dt);
            } else {
                b->v = b->w = v3(0, 0, 0);
            }
        }
        update_inertia(b);
        if (body_active(b))
            for (i32 k = 0; k < b->shape_count; ++k) shape_update(ph, &ph->shapes[b->first_shape + k]);
    }

    broadphase(ph);
    /* sort pairs so manifolds can be matched with last step's in one pass */
    if (!grow((void **)&ph->pairs_tmp, &ph->pairs_tmp_cap, ph->pair_count + 1, sizeof(u64))) ph->pair_count = 0;
    sort_u64(ph->pairs, ph->pairs_tmp, ph->pair_count);
    narrowphase(ph);

    soft sf = make_soft(h);
    ub_pjoints_keep_awake(ph);
    prepare(ph);
    for (int sub = 0; sub < SUBSTEPS; ++sub) {
        integrate_velocities(ph, h);
        ub_pforces_apply(ph, h);
        warm_start(ph);
        solve_contacts(ph, &sf, true);
        ub_pjoints_solve(ph, h);
        integrate_positions(ph, h);
        solve_contacts(ph, &sf, false);
    }
    restitution(ph);
    ub_pforces_clear(ph);

    i32 fallen = 0, kin = 0;
    for (i32 i = 1; i < ph->body_count; ++i) {
        ub_body *b = &ph->bodies[i];
        if (b->fallen) { fallen++; continue; }
        if (b->type == UB_BODY_KINEMATIC) {
            kin++;
            if (!b->has_target) continue;
            b->pos = b->target_pos; /* exactly where the game asked */
            b->q = b->target_q;
            b->rot = quat_to_mat3(b->q);
            b->has_target = 0;
        } else if (b->type == UB_BODY_DYNAMIC) {
            if (!b->awake) continue;
            if (v3_length2(b->v) < SLEEP_LINEAR * SLEEP_LINEAR && v3_length2(b->w) < SLEEP_ANGULAR * SLEEP_ANGULAR)
                b->sleep_time += dt;
            else
                b->sleep_time = 0;
            if (b->pos.y < ph->fallen_height) { /* fell out of the world */
                b->fallen = 1;
                b->awake = 0;
                fallen++;
                continue;
            }
        } else if (b->type == UB_BODY_CHARACTER) {
            b->w = v3(0, 0, 0);
        }
        for (i32 k = 0; k < b->shape_count; ++k) shape_update(ph, &ph->shapes[b->first_shape + k]);
        write_back(ph, b);
    }
    sleep_islands(ph);

    /* drop fallen bodies' shapes from the moving list */
    if (fallen != ph->stats.fallen) {
        i32 k = 0;
        for (i32 i = 0; i < ph->dyn_count; ++i) {
            if (!ph->bodies[ph->shapes[ph->dyn[i]].body].fallen) ph->dyn[k++] = ph->dyn[i];
            else ph->shapes[ph->dyn[i]].in_dyn = 0;
        }
        if (fallen > ph->stats.fallen) ub_info("physics: %d loose part group(s) fell out of the world", fallen - ph->stats.fallen);
        ph->dyn_count = k;
    }

    i32 contacts = 0;
    for (i32 i = 0; i < ph->man_count; ++i) contacts += ph->man[i].count;
    ph->stats.bodies = ph->body_count - 1;
    ph->stats.kinematic = kin;
    ph->stats.fallen = fallen;
    ph->stats.static_shapes = ph->static_count;
    ph->stats.moving_shapes = ph->dyn_count;
    ph->stats.pairs = ph->pair_count;
    ph->stats.manifolds = ph->man_count;
    ph->stats.contacts = contacts;
    ph->stats.steps++;
    i32 now_awake = 0;
    for (i32 i = 1; i < ph->body_count; ++i)
        now_awake += ph->bodies[i].type == UB_BODY_DYNAMIC && ph->bodies[i].awake && !ph->bodies[i].fallen;
    ph->stats.awake = now_awake;
}

/* ------------------------------------------------------------- queries */

static bool test_ray(const ub_physics *ph, i32 sid, vec3 o, vec3 dir, f32 *best, ub_phys_hit *hit)
{
    const ub_pshape *s = &ph->shapes[sid];
    if (!(s->flags & UB_SHAPE_COLLIDES)) return false;
    ub_cshape c;
    cshape(s, &c);
    f32 t;
    vec3 n;
    if (!ub_shape_raycast(&c, o, dir, *best, &t, &n) || t > *best) return false;
    *best = t;
    hit->t = t;
    hit->normal = n;
    hit->body = s->body;
    hit->shape = sid;
    hit->part = s->part;
    return true;
}

bool ub_phys_raycast(const ub_physics *ph, vec3 o, vec3 dir, f32 max_dist, i32 ignore_body, ub_phys_hit *hit)
{
    f32 best = max_dist;
    bool found = false;
    vec3 inv = v3(dir.x != 0 ? 1.0f / dir.x : 3e38f, dir.y != 0 ? 1.0f / dir.y : 3e38f, dir.z != 0 ? 1.0f / dir.z : 3e38f);
    if (ph->node_count > 0) {
        i32 stack[64];
        int sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const ub_bvh_node *n = &ph->nodes[stack[--sp]];
            if (!ray_box(o, inv, best, n->lo, n->hi)) continue;
            if (n->count > 0) {
                for (i32 k = 0; k < n->count; ++k)
                    found |= test_ray(ph, ph->static_ids[n->left + k], o, dir, &best, hit);
            } else if (sp + 2 <= 64) {
                stack[sp++] = n->left;
                stack[sp++] = n->left + 1;
            }
        }
    }
    for (i32 i = 0; i < ph->dyn_count; ++i) {
        const ub_pshape *s = &ph->shapes[ph->dyn[i]];
        if (s->body == ignore_body || !ray_box(o, inv, best, s->lo, s->hi)) continue;
        found |= test_ray(ph, ph->dyn[i], o, dir, &best, hit);
    }
    if (found) hit->point = v3_madd(o, dir, hit->t);
    return found;
}

i32 ub_phys_part_body(const ub_physics *ph, i32 part)
{
    if (part < 0 || part >= ph->part_count) return -1;
    i32 s = ph->part_shape[part];
    return s >= 0 ? ph->shapes[s].body : -1;
}

void ub_phys_body_pose(const ub_physics *ph, i32 body, f32 alpha, vec3 *pos, mat3 *rot)
{
    const ub_body *b = &ph->bodies[body];
    *pos = v3_add(b->prev_pos, v3_scale(v3_sub(b->pos, b->prev_pos), alpha));
    *rot = quat_to_mat3(quat_nlerp(b->prev_q, b->q, alpha));
}

bool ub_phys_part_pose(const ub_physics *ph, i32 part, f32 alpha, vec3 *pos, mat3 *rot)
{
    if (part < 0 || part >= ph->part_count || ph->part_shape[part] < 0) return false;
    const ub_pshape *s = &ph->shapes[ph->part_shape[part]];
    const ub_body *b = &ph->bodies[s->body];
    if (s->body == 0 || b->fallen) return false;
    vec3 bp;
    mat3 br;
    ub_phys_body_pose(ph, s->body, alpha, &bp, &br);
    *pos = v3_add(bp, mat3_apply(&br, s->lpos));
    *rot = mat3_mul(&br, &s->lrot);
    return true;
}

void ub_phys_get_stats(const ub_physics *ph, ub_phys_stats *out)
{
    *out = ph->stats;
}
