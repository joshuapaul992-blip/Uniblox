/* Uniblox - physics tests: collision, stacking, sleeping, welds. */
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "physics/ub_character.h"
#include "physics/ub_collide.h"
#include "physics/ub_hull.h"
#include "physics/ub_physics.h"
#include "render/ub_texture.h"
#include "world/ub_world.h"
#include "test.h"

static i32 add(ub_world *w, ub_shape shape, vec3 pos, vec3 size, bool anchored, ub_material mat)
{
    ub_part p;
    memset(&p, 0, sizeof p);
    p.pos = pos;
    p.rot = mat3_identity();
    p.size = size;
    p.shape = (u8)shape;
    p.material = (u8)mat;
    p.color = 0x808080;
    p.flags = UB_PART_CAN_COLLIDE | (anchored ? UB_PART_ANCHORED : 0);
    return ub_world_add_part(w, ub_world_workspace(w), UB_CLASS_PART, NULL, "P", &p);
}

static const ub_part *part_of(const ub_world *w, i32 inst) { return &w->parts[w->inst[inst].part]; }

static void ground(ub_world *w)
{
    ub_world_init(w);
    add(w, UB_SHAPE_BOX, v3(0, -1, 0), v3(400, 2, 400), true, UB_MAT_GRID); /* top at y = 0 */
}

/* Run until every loose body sleeps (or the time runs out); returns seconds. */
static f32 settle(ub_physics *ph, f32 max_seconds)
{
    int steps = (int)(max_seconds * UB_PHYS_HZ);
    for (int i = 0; i < steps; ++i) {
        ub_phys_step(ph);
        if (ph->stats.awake == 0 && i > 10) return (f32)i * UB_PHYS_DT;
    }
    return max_seconds + 1;
}

static f32 rnd(u32 *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return (f32)((*seed >> 8) & 0xFFFF) / 65535.0f;
}

static bool finite3(vec3 v) { return isfinite(v.x) && isfinite(v.y) && isfinite(v.z); }

/* ---------------------------------------------------------- hull basics */

static void test_hulls(void)
{
    f32 vol;
    vec3 c;
    mat3 I;
    ub_shape_mass(UB_SHAPE_BOX, v3(2, 4, 6), &vol, &c, &I);
    CHECK(fabsf(vol - 48) < 1e-3f && v3_length(c) < 1e-5f);
    CHECK(fabsf(I.x.x - 48 * (16 + 36) / 12.0f) < 1e-2f && fabsf(I.y.y - 48 * (4 + 36) / 12.0f) < 1e-2f);
    CHECK(fabsf(I.x.y) < 1e-3f && fabsf(I.y.z) < 1e-3f);
    ub_shape_mass(UB_SHAPE_WEDGE, v3(2, 2, 2), &vol, &c, &I);
    CHECK(fabsf(vol - 4) < 1e-3f);
    CHECK(fabsf(c.y + 1.0f / 3.0f) < 1e-3f && fabsf(c.z - 1.0f / 3.0f) < 1e-3f && fabsf(c.x) < 1e-4f);
    ub_shape_mass(UB_SHAPE_CORNER_WEDGE, v3(3, 3, 3), &vol, &c, &I);
    CHECK(fabsf(vol - 9) < 1e-3f);  /* a pyramid: base x height / 3 */
    ub_shape_mass(UB_SHAPE_CYLINDER, v3(2, 5, 2), &vol, &c, &I);
    CHECK(vol > 0.95f * 3.14159f * 5 && vol < 3.14159f * 5); /* 12 sides: just under a true cylinder */
    for (int s = 0; s < UB_SHAPE_COUNT; ++s) {
        if (s == UB_SHAPE_BALL) continue;
        const ub_hull *h = ub_hull_get((ub_shape)s);
        int closed = 0;
        for (int e = 0; e < h->edge_count; ++e) closed += h->e[e].f0 != h->e[e].f1;
        CHECK(closed == h->edge_count);                       /* every edge joins two faces */
        CHECK(h->vert_count - h->edge_count + h->face_count == 2); /* Euler */
        for (int f = 0; f < h->face_count; ++f) {             /* counter-clockwise from outside */
            const ub_hull_face *fc = &h->f[f];
            vec3 a = h->v[h->fv[fc->first]], b = h->v[h->fv[fc->first + 1]], cc = h->v[h->fv[fc->first + 2]];
            CHECK(v3_dot(v3_cross(v3_sub(b, a), v3_sub(cc, b)), h->n[f]) > 0);
        }
    }
}

static ub_cshape box_shape(vec3 pos, vec3 size, mat3 rot)
{
    ub_cshape s;
    s.sphere = false;
    s.hull = ub_hull_get(UB_SHAPE_BOX);
    s.pos = pos;
    s.rot = rot;
    s.size = size;
    s.radius = 0;
    return s;
}

static void test_collide(void)
{
    vec3 n;
    ub_cpoint pts[UB_MAX_CONTACTS];
    /* box resting 0.05 into a floor: four corner contacts, normal up */
    ub_cshape floor = box_shape(v3(0, -1, 0), v3(20, 2, 20), mat3_identity());
    ub_cshape b = box_shape(v3(0, 0.95f, 0), v3(2, 2, 2), mat3_identity());
    int c = ub_collide(&floor, &b, 0.02f, &n, pts);
    CHECK(c == 4 && n.y > 0.999f);
    for (int i = 0; i < c; ++i) CHECK(fabsf(pts[i].sep + 0.05f) < 1e-4f);
    /* swapped: normal flips */
    c = ub_collide(&b, &floor, 0.02f, &n, pts);
    CHECK(c == 4 && n.y < -0.999f);
    /* apart by more than the margin: nothing; within it: speculative */
    b.pos.y = 1.1f;
    CHECK(ub_collide(&floor, &b, 0.02f, &n, pts) == 0);
    c = ub_collide(&floor, &b, 0.2f, &n, pts);
    CHECK(c == 4 && fabsf(pts[0].sep - 0.1f) < 1e-4f);
    /* a box on its edge: two contacts */
    b.rot = mat3_from_euler(0, 0, 0.7853982f);
    b.pos.y = 1.40f;
    c = ub_collide(&floor, &b, 0.02f, &n, pts);
    CHECK(c == 2 && n.y > 0.999f);
    /* edge against edge: two boxes crossed and tilted */
    ub_cshape e1 = box_shape(v3(0, 0, 0), v3(2, 2, 2), mat3_from_euler(0, 0, 0.7853982f));
    ub_cshape e2 = box_shape(v3(0, 2.75f, 0), v3(2, 2, 2), mat3_from_euler(1.5707963f, 0, 0.7853982f));
    c = ub_collide(&e1, &e2, 0.02f, &n, pts);
    CHECK(c == 1 && n.y > 0.99f && fabsf(pts[0].sep - (2.75f - 2.828427f)) < 1e-3f);
    /* sphere on a box face, and pushed into its edge */
    ub_cshape sph = { true, NULL, v3(0, 0.9f, 0), { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, v3(2, 2, 2), 1.0f };
    c = ub_collide(&floor, &sph, 0.02f, &n, pts);
    CHECK(c == 1 && n.y > 0.999f && fabsf(pts[0].sep + 0.1f) < 1e-4f);
    c = ub_collide(&sph, &floor, 0.02f, &n, pts);
    CHECK(c == 1 && n.y < -0.999f);
    sph.pos = v3(10.5f, 0.5f, 0);
    c = ub_collide(&floor, &sph, 0.02f, &n, pts);
    CHECK(c == 1 && n.x > 0.7f && n.y > 0.7f && fabsf(pts[0].sep - (sqrtf(0.5f) - 1)) < 1e-3f);
    /* ray hits */
    f32 t;
    CHECK(ub_shape_raycast(&floor, v3(1, 5, 1), v3(0, -1, 0), 100, &t, &n) && fabsf(t - 5) < 1e-4f && n.y > 0.999f);
    CHECK(!ub_shape_raycast(&floor, v3(1, 5, 1), v3(0, 1, 0), 100, &t, &n));
    sph.pos = v3(0, 0, 10);
    CHECK(ub_shape_raycast(&sph, v3(0, 0, 0), v3(0, 0, 1), 100, &t, &n) && fabsf(t - 9) < 1e-4f && n.z < -0.999f);
}

/* ------------------------------------------------------------- stacking */

static void test_box_rests(void)
{
    ub_world w;
    ground(&w);
    i32 b = add(&w, UB_SHAPE_BOX, v3(0, 3, 0), v3(4, 1, 2), false, UB_MAT_PLASTIC);
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    CHECK(ph.body_count == 2 && ph.static_count == 1);
    f32 t = settle(&ph, 5);
    CHECK(t < 3);
    const ub_part *p = part_of(&w, b);
    CHECK(fabsf(p->pos.y - 0.5f) < 0.03f && fabsf(p->pos.x) < 0.05f && fabsf(p->pos.z) < 0.05f);
    CHECK(p->rot.y.y > 0.9999f);
    printf("  box dropped 2.5 studs: asleep after %.2f s at %.3f, %.3f, %.3f\n", (double)t, (double)p->pos.x, (double)p->pos.y, (double)p->pos.z);
    ub_phys_free(&ph);
    ub_world_free(&w);
}

/* n parts of this size stacked with small gaps, dropped together */
static void test_tower(int n, vec3 size)
{
    ub_world w;
    ground(&w);
    i32 first = -1, last = -1;
    for (int i = 0; i < n; ++i) {
        last = add(&w, UB_SHAPE_BOX, v3(0, size.y * 0.5f + (size.y + 0.02f) * (f32)i, 0), size, false,
                   i % 2 ? UB_MAT_WOOD : UB_MAT_PLASTIC);
        if (first < 0) first = last;
    }
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    f32 t = settle(&ph, 10);
    CHECK(t < 6);
    const ub_part *top = part_of(&w, last), *bot = part_of(&w, first);
    f32 want = size.y * ((f32)n - 0.5f);
    CHECK(fabsf(top->pos.y - want) < 0.02f * (f32)n);    /* contacts give a little under the weight */
    CHECK(fabsf(top->pos.x) < 0.005f * (f32)n && fabsf(top->pos.z) < 0.005f * (f32)n); /* stays upright */
    CHECK(fabsf(bot->pos.y - size.y * 0.5f) < 0.03f);
    printf("  tower of %d %gx%gx%g parts: asleep after %.2f s, top at y = %.3f (ideal %.1f), drift %.4f\n", n, (double)size.x, (double)size.y, (double)size.z,
           (double)t, (double)top->pos.y, (double)want, (double)sqrtf(top->pos.x * top->pos.x + top->pos.z * top->pos.z));
    /* asleep means asleep: more steps change nothing */
    vec3 before = top->pos;
    for (int i = 0; i < 240; ++i) ub_phys_step(&ph);
    CHECK(top->pos.x == before.x && top->pos.y == before.y && top->pos.z == before.z);
    ub_phys_free(&ph);
    ub_world_free(&w);
}

static void test_pyramid(void)
{
    ub_world w;
    ground(&w);
    int rows = 5, count = 0;
    for (int r = 0; r < rows; ++r)
        for (int i = 0; i < rows - r; ++i, ++count)
            add(&w, UB_SHAPE_BOX, v3((f32)i * 2.1f - (f32)(rows - r - 1) * 1.05f, 1.0f + 2.01f * (f32)r, 0), v3(2, 2, 2),
                false, UB_MAT_WOOD);
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    f32 t = settle(&ph, 10);
    CHECK(t < 6);
    /* the top box is still on top, roughly central */
    const ub_part *top = &w.parts[w.part_count - 1];
    CHECK(fabsf(top->pos.y - (1.0f + 2.0f * (f32)(rows - 1))) < 0.2f && fabsf(top->pos.x) < 0.3f);
    printf("  pyramid of %d boxes: asleep after %.2f s, top at %.3f, %.3f\n", count, (double)t, (double)top->pos.x,
           (double)top->pos.y);
    ub_phys_free(&ph);
    ub_world_free(&w);
}

static void test_shapes_rest(void)
{
    ub_world w;
    ground(&w);
    i32 ball = add(&w, UB_SHAPE_BALL, v3(0, 5, 0), v3(2, 2, 2), false, UB_MAT_PLASTIC);
    i32 cyl = add(&w, UB_SHAPE_CYLINDER, v3(6, 3, 0), v3(2, 3, 2), false, UB_MAT_METAL);
    i32 wedge = add(&w, UB_SHAPE_WEDGE, v3(-6, 2, 0), v3(3, 2, 4), false, UB_MAT_STONE);
    i32 corner = add(&w, UB_SHAPE_CORNER_WEDGE, v3(0, 2, 8), v3(2, 2, 2), false, UB_MAT_PLASTIC);
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    f32 t = settle(&ph, 12);
    CHECK(t < 8);
    CHECK(fabsf(part_of(&w, ball)->pos.y - 1.0f) < 0.03f);
    CHECK(fabsf(part_of(&w, cyl)->pos.y - 1.5f) < 0.03f && part_of(&w, cyl)->rot.y.y > 0.999f);
    CHECK(fabsf(part_of(&w, wedge)->pos.y - 1.0f) < 0.03f && part_of(&w, wedge)->rot.y.y > 0.999f);
    CHECK(fabsf(part_of(&w, corner)->pos.y - 1.0f) < 0.03f);
    printf("  ball, cylinder, wedge, corner wedge: asleep after %.2f s\n", (double)t);
    ub_phys_free(&ph);
    ub_world_free(&w);
}

static void test_slopes(void)
{
    /* a 14-degree ramp: plastic (friction 0.3 > tan 14 = 0.25) stays, a ball rolls */
    ub_world w;
    ground(&w);
    ub_part ramp;
    memset(&ramp, 0, sizeof ramp);
    ramp.pos = v3(0, 1, 0);
    ramp.rot = mat3_identity();
    ramp.size = v3(10, 2, 8);
    ramp.shape = UB_SHAPE_WEDGE;
    ramp.flags = UB_PART_ANCHORED | UB_PART_CAN_COLLIDE;
    ub_world_add_part(&w, w.workspace, UB_CLASS_PART, NULL, "Ramp", &ramp);
    /* slope surface: y = 1 + 0.25 z... rises towards +z: at z the top is 1 + z * 2/8 */
    f32 n_y = 8.0f / sqrtf(68.0f), n_z = -2.0f / sqrtf(68.0f);
    vec3 on = v3(-2, 1.0f + 0.25f * 1.0f, 1.0f);
    i32 box = add(&w, UB_SHAPE_BOX, v3_add(on, v3(0, n_y * 0.52f, n_z * 0.52f)), v3(1, 1, 1), false, UB_MAT_PLASTIC);
    w.parts[w.inst[box].part].rot = mat3_from_euler(0, atan2f(2, 8), 0); /* +Z tilted up, along the slope */
    i32 ball = add(&w, UB_SHAPE_BALL, v3(2, 1.0f + 0.25f * 2.0f + 0.6f, 2.0f), v3(1, 1, 1), false, UB_MAT_PLASTIC);
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    vec3 b0 = part_of(&w, box)->pos;
    for (int i = 0; i < 240; ++i) ub_phys_step(&ph);
    vec3 b1 = part_of(&w, box)->pos, l1 = part_of(&w, ball)->pos;
    CHECK(v3_length(v3_sub(b1, b0)) < 0.1f);   /* friction holds the box */
    CHECK(l1.z < 0.5f);                          /* the ball rolled down towards -z */
    printf("  ramp: box moved %.3f studs in 1 s, ball rolled to z = %.2f\n", (double)v3_length(v3_sub(b1, b0)), (double)l1.z);
    ub_phys_free(&ph);
    ub_world_free(&w);
}

/* ---------------------------------------------------------- assemblies */

static void test_welds(void)
{
    ub_world w;
    ground(&w);
    /* an L of two boxes welded: one body; it lands without falling apart */
    i32 a = add(&w, UB_SHAPE_BOX, v3(0, 3, 0), v3(4, 1, 1), false, UB_MAT_PLASTIC);
    i32 b = add(&w, UB_SHAPE_BOX, v3(1.5f, 4.5f, 0), v3(1, 2, 1), false, UB_MAT_PLASTIC);
    CHECK(ub_world_add_weld(&w, a, b));
    /* a loose box welded to an anchored one stays put */
    i32 c = add(&w, UB_SHAPE_BOX, v3(10, 5, 0), v3(2, 2, 2), true, UB_MAT_PLASTIC);
    i32 d = add(&w, UB_SHAPE_BOX, v3(10, 7, 0), v3(2, 2, 2), false, UB_MAT_PLASTIC);
    CHECK(ub_world_add_weld(&w, c, d));
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    CHECK(ph.body_count == 2);                                  /* static + the L */
    CHECK(ub_phys_part_body(&ph, w.inst[a].part) == ub_phys_part_body(&ph, w.inst[b].part));
    CHECK(ub_phys_part_body(&ph, w.inst[d].part) == 0);
    f32 t = settle(&ph, 8);
    CHECK(t < 6);
    vec3 pa = part_of(&w, a)->pos, pb = part_of(&w, b)->pos;
    CHECK(fabsf(v3_length(v3_sub(pa, pb)) - sqrtf(1.5f * 1.5f + 1.5f * 1.5f)) < 1e-3f); /* still rigid */
    CHECK(part_of(&w, d)->pos.y == 7.0f);
    CHECK(pa.y < 1.0f && pa.y > 0.3f);
    printf("  welded L: one body, asleep after %.2f s\n", (double)t);
    ub_phys_free(&ph);
    ub_world_free(&w);
}

static void test_fallen_and_wake(void)
{
    ub_world w;
    ground(&w);
    w.fallen_height = -50;
    i32 off = add(&w, UB_SHAPE_BOX, v3(300, 5, 0), v3(2, 2, 2), false, UB_MAT_PLASTIC); /* beside the ground */
    i32 box = add(&w, UB_SHAPE_BOX, v3(0, 1, 0), v3(2, 2, 2), false, UB_MAT_PLASTIC);
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    for (int i = 0; i < 3 * UB_PHYS_HZ; ++i) ub_phys_step(&ph);
    CHECK(ph.stats.fallen == 1 && ph.stats.awake == 0);
    vec3 pos;
    mat3 rot;
    CHECK(!ub_phys_part_pose(&ph, w.inst[off].part, 1, &pos, &rot));
    CHECK(ub_phys_part_pose(&ph, w.inst[box].part, 1, &pos, &rot));
    /* a push wakes the sleeping box and it slides, then sleeps again */
    i32 body = ub_phys_part_body(&ph, w.inst[box].part);
    ub_phys_apply_impulse(&ph, body, v3(ph.bodies[body].mass * 10, 0, 0), ph.bodies[body].pos);
    CHECK(ph.bodies[body].awake);
    f32 t = settle(&ph, 6);
    CHECK(t < 5 && part_of(&w, box)->pos.x > 0.5f);
    ub_phys_free(&ph);
    ub_world_free(&w);
}

static void test_kinematic(void)
{
    /* a kinematic paddle sweeps a sleeping box along */
    ub_world w;
    ground(&w);
    i32 paddle = add(&w, UB_SHAPE_BOX, v3(-4, 1, 0), v3(1, 2, 4), true, UB_MAT_METAL);
    i32 box = add(&w, UB_SHAPE_BOX, v3(0, 1, 0), v3(2, 2, 2), false, UB_MAT_PLASTIC);
    ub_physics ph;
    i32 kp = w.inst[paddle].part;
    CHECK(ub_phys_init(&ph, &w, &kp, 1));
    settle(&ph, 3);
    i32 kb = ub_phys_part_body(&ph, kp);
    CHECK(kb > 0 && ph.bodies[kb].type == UB_BODY_KINEMATIC);
    mat3 id = mat3_identity();
    for (int i = 0; i < UB_PHYS_HZ; ++i) ub_phys_set_target(&ph, kb, v3(-4 + 8.0f * (f32)(i + 1) / UB_PHYS_HZ, 1, 0), &id), ub_phys_step(&ph);
    CHECK(part_of(&w, paddle)->pos.x > 3.9f);
    CHECK(part_of(&w, box)->pos.x > 4.0f);  /* pushed ahead of the paddle */
    ub_phys_free(&ph);
    ub_world_free(&w);
}

static void test_rubble(void)
{
    /* 300 random parts dropped into a walled pit: nothing explodes or goes NaN */
    ub_world w;
    ground(&w);
    add(&w, UB_SHAPE_BOX, v3(-11, 20, 0), v3(2, 40, 24), true, UB_MAT_STONE);
    add(&w, UB_SHAPE_BOX, v3(11, 20, 0), v3(2, 40, 24), true, UB_MAT_STONE);
    add(&w, UB_SHAPE_BOX, v3(0, 20, -11), v3(24, 40, 2), true, UB_MAT_STONE);
    add(&w, UB_SHAPE_BOX, v3(0, 20, 11), v3(24, 40, 2), true, UB_MAT_STONE);
    u32 seed = 7;
#define RND() rnd(&seed)
    for (int i = 0; i < 300; ++i) {
        vec3 pos = v3(RND() * 16 - 8, 2 + (f32)i * 0.6f, RND() * 16 - 8);
        vec3 size = v3(0.5f + RND() * 2.5f, 0.5f + RND() * 2.5f, 0.5f + RND() * 2.5f);
        i32 k = add(&w, (ub_shape)(i % UB_SHAPE_COUNT), pos, size, false, (ub_material)(i % UB_MAT_COUNT));
        w.parts[w.inst[k].part].rot = mat3_from_euler(RND() * 6, RND() * 6, RND() * 6);
    }
#undef RND
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    clock_t c0 = clock();
    f32 t = settle(&ph, 30);
    f64 secs = (f64)(clock() - c0) / CLOCKS_PER_SEC;
    int bad = 0, escaped = 0, awake = 0;
    for (i32 i = 1; i < ph.body_count; ++i) awake += ph.bodies[i].awake;
    for (i32 i = 0; i < w.part_count; ++i) {
        bad += !finite3(w.parts[i].pos);
        escaped += w.parts[i].pos.y < -1 || fabsf(w.parts[i].pos.x) > 12 || fabsf(w.parts[i].pos.z) > 12;
    }
    CHECK(bad == 0);
    CHECK(escaped == 0);
    CHECK(t < 30);
    if (t >= 30) printf("  still awake: %d of %d\n", awake, ph.body_count - 1);
    printf("  300 random parts in a pit: asleep after %.1f s (%u steps, %.1f ms of CPU per simulated second)\n",
           (double)t, ph.stats.steps, secs * 1000.0 / (double)(t < 30 ? t : 30));
    ub_phys_free(&ph);
    ub_world_free(&w);
}

/* ------------------------------------------------- touches and changes */

static int count_touch(const ub_physics *ph, i32 pa, i32 pb)
{
    int n = 0;
    for (i32 i = 0; i < ph->touch_count; ++i)
        n += (ph->touches[i].a == pa && ph->touches[i].b == pb) || (ph->touches[i].a == pb && ph->touches[i].b == pa);
    return n;
}

static void test_touch_and_sync(void)
{
    ub_world w;
    ground(&w);
    i32 floor_part = w.inst[1].part;
    i32 box = add(&w, UB_SHAPE_BOX, v3(0, 3, 0), v3(2, 2, 2), false, UB_MAT_PLASTIC);
    i32 zone = add(&w, UB_SHAPE_BOX, v3(10, 3, 0), v3(4, 1, 4), true, UB_MAT_PLASTIC); /* a trigger */
    w.parts[w.inst[zone].part].flags &= (u8)~UB_PART_CAN_COLLIDE;
    i32 faller = add(&w, UB_SHAPE_BOX, v3(10, 8, 0), v3(1, 1, 1), false, UB_MAT_PLASTIC);
    i32 bp = w.inst[box].part, zp = w.inst[zone].part, fp = w.inst[faller].part;
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    ub_phys_sync_part(&ph, zp, true, true); /* watched by a script: a sensor */
    int landed = 0, zone_hits = 0;
    for (int i = 0; i < 3 * UB_PHYS_HZ; ++i) {
        ub_phys_step(&ph);
        landed += count_touch(&ph, floor_part, bp);
        zone_hits += count_touch(&ph, zp, fp);
    }
    CHECK(landed >= 1 && landed <= 3);  /* when it lands (and after a bounce or two), not every step while resting */
    CHECK(zone_hits == 1);       /* the trigger noticed the falling part... */
    CHECK(part_of(&w, faller)->pos.y < 1.0f); /* ...which fell right through it */
    CHECK(ph.stats.awake == 0);
    /* waking it does not count as a new touch */
    ub_phys_apply_impulse(&ph, ub_phys_part_body(&ph, bp), v3(0, 1, 0), ph.bodies[ub_phys_part_body(&ph, bp)].pos);
    landed = 0;
    for (int i = 0; i < 10; ++i) {
        ub_phys_step(&ph);
        landed += count_touch(&ph, floor_part, bp);
    }
    CHECK(landed == 0);

    /* a script moves an anchored wall into the box: it pushes it */
    i32 wall = add(&w, UB_SHAPE_BOX, v3(-6, 2, 0), v3(1, 4, 6), true, UB_MAT_PLASTIC);
    i32 wp = w.inst[wall].part;
    ub_phys_sync_part(&ph, wp, true, false);
    settle(&ph, 3);
    for (int i = 0; i < UB_PHYS_HZ; ++i) {
        w.parts[wp].pos.x = -6.0f + 8.0f * (f32)(i + 1) / UB_PHYS_HZ;
        ub_phys_sync_part(&ph, wp, true, false);
        ub_phys_step(&ph);
    }
    CHECK(ph.bodies[ub_phys_part_body(&ph, wp)].type == UB_BODY_KINEMATIC);
    CHECK(fabsf(w.parts[wp].pos.x - 2.0f) < 1e-3f && part_of(&w, box)->pos.x > 2.4f);
    /* the floor stops being solid: everything on it falls */
    w.parts[floor_part].flags &= (u8)~UB_PART_CAN_COLLIDE;
    ub_phys_sync_part(&ph, floor_part, true, false);
    CHECK(ub_phys_part_body(&ph, floor_part) == 0); /* still static, just not solid */
    ub_phys_wake(&ph, ub_phys_part_body(&ph, bp));
    for (int i = 0; i < UB_PHYS_HZ / 2; ++i) ub_phys_step(&ph);
    CHECK(part_of(&w, box)->pos.y < -5.0f);
    w.parts[floor_part].flags |= UB_PART_CAN_COLLIDE;
    ub_phys_sync_part(&ph, floor_part, true, false);
    /* a new part made by a script falls and lands; a destroyed one stops colliding */
    i32 fresh = add(&w, UB_SHAPE_BALL, v3(20, 5, 0), v3(2, 2, 2), false, UB_MAT_PLASTIC);
    ub_phys_sync_part(&ph, w.inst[fresh].part, true, false);
    settle(&ph, 4);
    CHECK(fabsf(part_of(&w, fresh)->pos.y - 1.0f) < 0.05f);
    w.parts[floor_part].flags |= UB_PART_REMOVED;
    ub_phys_sync_part(&ph, floor_part, false, false);
    ub_phys_wake(&ph, ub_phys_part_body(&ph, w.inst[fresh].part));
    for (int i = 0; i < UB_PHYS_HZ / 2; ++i) ub_phys_step(&ph);
    CHECK(part_of(&w, fresh)->pos.y < -5.0f);
    /* rays ignore what is not solid */
    ub_phys_hit h;
    CHECK(!ub_phys_raycast(&ph, v3(10, 10, 0), v3(0, -1, 0), 20, -1, &h) || h.part != zp);
    ub_phys_free(&ph);
    ub_world_free(&w);
}

/* ----------------------------------------------------------- character */

static void walk(ub_physics *ph, ub_character *c, vec3 move, bool jump, f32 seconds, f32 *max_y)
{
    int steps = (int)(seconds * UB_PHYS_HZ);
    for (int i = 0; i < steps; ++i) {
        ub_char_before_step(c, ph, move, jump);
        ub_phys_step(ph);
        ub_char_after_step(c, ph);
        if (max_y && ph->bodies[c->body].pos.y > *max_y) *max_y = ph->bodies[c->body].pos.y;
    }
}

static void test_character(void)
{
    ub_world w;
    ground(&w);
    add(&w, UB_SHAPE_BOX, v3(31, 6, 0), v3(2, 12, 40), true, UB_MAT_BRICK);        /* wall, face at x = 30 */
    add(&w, UB_SHAPE_BOX, v3(0, 0.75f, 30), v3(10, 1.5f, 10), true, UB_MAT_WOOD);  /* 1.5 step, z 25..35 */
    add(&w, UB_SHAPE_BOX, v3(-30, 2, 0), v3(10, 4, 10), true, UB_MAT_STONE);       /* 4 high ledge, x -35..-25 */
    add(&w, UB_SHAPE_BOX, v3(0, 6.5f, -30), v3(10, 1, 10), true, UB_MAT_STONE);    /* ceiling, underside at 6 */
    i32 crate = add(&w, UB_SHAPE_BOX, v3(30, 1.5f, 30), v3(3, 3, 3), false, UB_MAT_WOOD);
    i32 slab = add(&w, UB_SHAPE_BOX, v3(-30, 1, 30), v3(6, 2, 6), false, UB_MAT_WOOD);
    ub_physics ph;
    CHECK(ub_phys_init(&ph, &w, NULL, 0));
    settle(&ph, 3);
    ub_character c;
    CHECK(ub_char_init(&c, &ph, v3(0, 1, 0)));
    walk(&ph, &c, v3(0, 0, 0), false, 0.5f, NULL);
    vec3 f = ub_char_feet(&c, &ph, 1);
    CHECK(c.grounded && fabsf(f.y) < 0.01f);

    /* walking speed */
    walk(&ph, &c, v3(1, 0, 0), false, 1.0f, NULL);
    f = ub_char_feet(&c, &ph, 1);
    CHECK(f.x > 14.5f && f.x < 16.5f && fabsf(f.z) < 0.01f);
    printf("  character: walked %.2f studs in 1 s\n", (double)f.x);
    /* into the wall: stops one body-radius short of it */
    walk(&ph, &c, v3(1, 0, 0), false, 2.0f, NULL);
    f = ub_char_feet(&c, &ph, 1);
    CHECK(f.x < 30.0f - UB_CHAR_RADIUS + 0.05f && f.x > 28.5f && c.grounded);
    /* jump height */
    f32 top = 0;
    walk(&ph, &c, v3(0, 0, 0), true, 0.05f, &top);
    walk(&ph, &c, v3(0, 0, 0), false, 1.5f, &top);
    CHECK(fabsf(top - UB_CHAR_JUMP_HEIGHT) < 0.3f && c.grounded);
    printf("  character: jumped %.2f studs high (Roblox: 7.2)\n", (double)top);

    /* up a 1.5 step without jumping */
    ub_char_teleport(&c, &ph, v3(0, 0, 18));
    walk(&ph, &c, v3(0, 0, 1), false, 0.8f, NULL);  /* about 12 studs: onto the step (z 25..35) */
    f = ub_char_feet(&c, &ph, 1);
    CHECK(f.z > 26 && fabsf(f.y - 1.5f) < 0.02f && c.grounded);
    /* a 4 high ledge blocks walking; a jump gets on top */
    ub_char_teleport(&c, &ph, v3(-15, 0, 0));
    walk(&ph, &c, v3(-1, 0, 0), false, 2.0f, NULL);
    f = ub_char_feet(&c, &ph, 1);
    CHECK(f.x > -25 + UB_CHAR_RADIUS - 0.1f && f.y < 0.05f);
    walk(&ph, &c, v3(-1, 0, 0), true, 0.05f, NULL);
    walk(&ph, &c, v3(-1, 0, 0), false, 0.3f, NULL);
    walk(&ph, &c, v3(0, 0, 0), false, 1.0f, NULL);
    f = ub_char_feet(&c, &ph, 1);
    CHECK(f.x < -26 && fabsf(f.y - 4.0f) < 0.02f && c.grounded);
    /* a ceiling stops a jump */
    ub_char_teleport(&c, &ph, v3(0, 0, -30));
    walk(&ph, &c, v3(0, 0, 0), false, 0.3f, NULL);
    top = 0;
    walk(&ph, &c, v3(0, 0, 0), true, 0.05f, &top);
    walk(&ph, &c, v3(0, 0, 0), false, 1.0f, &top);
    CHECK(top < 6.0f - UB_CHAR_HEIGHT + 0.1f && c.grounded);

    /* pushing a crate taller than a step: it slides along */
    vec3 c0 = part_of(&w, crate)->pos;
    ub_char_teleport(&c, &ph, v3(22, 0, 30));
    walk(&ph, &c, v3(1, 0, 0), false, 2.0f, NULL);
    vec3 c1 = part_of(&w, crate)->pos;
    CHECK(c1.x > c0.x + 2 && c1.y > 1.4f && c1.y < 1.6f);
    printf("  character: pushed a crate %.2f studs\n", (double)(c1.x - c0.x));
    /* stepping onto a loose slab: stands on it, the slab stays put */
    vec3 s0 = part_of(&w, slab)->pos;
    ub_char_teleport(&c, &ph, v3(-30, 0, 20));
    walk(&ph, &c, v3(0, 0, 1), false, 0.6f, NULL);  /* onto the slab (z 27..33) */
    walk(&ph, &c, v3(0, 0, 0), false, 1.0f, NULL);
    f = ub_char_feet(&c, &ph, 1);
    vec3 s1 = part_of(&w, slab)->pos;
    CHECK(c.grounded && fabsf(f.y - 2.0f) < 0.05f && v3_length(v3_sub(s1, s0)) < 0.5f && s1.y > 0.95f);

    /* falling off the world is noticed by the game (fallen height) */
    CHECK(ph.fallen_height < -100);
    ub_phys_free(&ph);
    ub_world_free(&w);

    /* a moving platform carries the character along */
    ground(&w);
    i32 plat = add(&w, UB_SHAPE_BOX, v3(0, 3, 0), v3(8, 1, 8), true, UB_MAT_METAL);
    i32 pp = w.inst[plat].part;
    CHECK(ub_phys_init(&ph, &w, &pp, 1));
    CHECK(ub_char_init(&c, &ph, v3(0, 3.5f, 0)));
    i32 pb = ub_phys_part_body(&ph, pp);
    mat3 id = mat3_identity();
    for (int i = 0; i < UB_PHYS_HZ; ++i) {
        ub_phys_set_target(&ph, pb, v3(0, 3, 8.0f * (f32)(i + 1) / UB_PHYS_HZ), &id);
        ub_char_before_step(&c, &ph, v3(0, 0, 0), false);
        ub_phys_step(&ph);
        ub_char_after_step(&c, &ph);
    }
    f = ub_char_feet(&c, &ph, 1);
    CHECK(c.grounded && fabsf(f.z - 8.0f) < 0.6f && fabsf(f.y - 3.5f) < 0.05f);
    printf("  character: rode a platform to z = %.2f (platform at 8)\n", (double)f.z);
    ub_phys_free(&ph);
    ub_world_free(&w);
}

void test_physics(void)
{
    test_hulls();
    test_collide();
    test_box_rests();
    test_tower(5, v3(2, 2, 2));
    test_tower(10, v3(2, 2, 2));
    test_tower(20, v3(2, 2, 2));
    test_tower(20, v3(4, 1, 2));   /* Roblox's default part size */
    test_tower(20, v3(8, 1, 8));
    test_tower(10, v3(1, 1, 1));
    test_pyramid();
    test_shapes_rest();
    test_slopes();
    test_welds();
    test_fallen_and_wake();
    test_kinematic();
    test_rubble();
    test_touch_and_sync();
    test_character();
}
