#include "render/ub_mesh.h"

#define CYL_SIDES 12
#define BALL_STACKS 6
#define BALL_SLICES 10

static ub_mesh g_meshes[UB_SHAPE_COUNT];

static int add_vert(ub_mesh *m, f32 x, f32 y, f32 z)
{
    m->verts[m->vert_count] = v3(x, y, z);
    return m->vert_count++;
}

/* Adds a face and computes its outward normal (Newell's method, then
 * flipped if it points towards the centre: every shape is convex and
 * centred on the origin). */
static void add_face(ub_mesh *m, int count, const int *idx)
{
    ub_face *f = &m->faces[m->face_count++];
    f->count = (u8)count;
    vec3 n = v3(0, 0, 0), c = v3(0, 0, 0);
    for (int i = 0; i < count; ++i) {
        f->v[i] = (u8)idx[i];
        vec3 a = m->verts[idx[i]], b = m->verts[idx[(i + 1) % count]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
        c = v3_add(c, a);
    }
    n = v3_normalize(n);
    if (v3_dot(n, c) < 0) n = v3_scale(n, -1.0f);
    f->normal = n;
}

static void face4(ub_mesh *m, int a, int b, int c, int d)
{
    int idx[4] = { a, b, c, d };
    add_face(m, 4, idx);
}

static void face3(ub_mesh *m, int a, int b, int c)
{
    int idx[3] = { a, b, c };
    add_face(m, 3, idx);
}

static void build_box(ub_mesh *m)
{
    /* corner i: x = bit 0, y = bit 1, z = bit 2 */
    for (int i = 0; i < 8; ++i)
        add_vert(m, (i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
    /* side faces start with a horizontal edge so textures stay upright */
    face4(m, 1, 5, 7, 3); /* +X */
    face4(m, 0, 4, 6, 2); /* -X */
    face4(m, 2, 3, 7, 6); /* +Y */
    face4(m, 0, 1, 5, 4); /* -Y */
    face4(m, 4, 5, 7, 6); /* +Z */
    face4(m, 0, 1, 3, 2); /* -Z */
}

static void build_wedge(ub_mesh *m)
{
    int b0 = add_vert(m, -0.5f, -0.5f, -0.5f), b1 = add_vert(m, 0.5f, -0.5f, -0.5f);
    int b2 = add_vert(m, 0.5f, -0.5f, 0.5f), b3 = add_vert(m, -0.5f, -0.5f, 0.5f);
    int t0 = add_vert(m, -0.5f, 0.5f, 0.5f), t1 = add_vert(m, 0.5f, 0.5f, 0.5f);
    face4(m, b0, b1, b2, b3); /* bottom */
    face4(m, b3, b2, t1, t0); /* back wall */
    face4(m, b0, b1, t1, t0); /* slope */
    face3(m, b0, b3, t0);     /* -X side */
    face3(m, b1, b2, t1);     /* +X side */
}

static void build_cylinder(ub_mesh *m)
{
    int bot[CYL_SIDES], top[CYL_SIDES];
    for (int i = 0; i < CYL_SIDES; ++i) {
        f32 a = UB_TWO_PI * (f32)i / CYL_SIDES;
        f32 x = 0.5f * ub_cosf(a), z = 0.5f * ub_sinf(a);
        bot[i] = add_vert(m, x, -0.5f, z);
        top[i] = add_vert(m, x, 0.5f, z);
    }
    for (int i = 0; i < CYL_SIDES; ++i) {
        int j = (i + 1) % CYL_SIDES;
        face4(m, bot[i], bot[j], top[j], top[i]);
    }
    add_face(m, CYL_SIDES, top);
    add_face(m, CYL_SIDES, bot);
}

static void build_ball(ub_mesh *m)
{
    int ring[BALL_STACKS - 1][BALL_SLICES];
    int north = add_vert(m, 0, 0.5f, 0);
    for (int s = 1; s < BALL_STACKS; ++s) {
        f32 lat = UB_HALF_PI - UB_PI * (f32)s / BALL_STACKS;
        f32 r = 0.5f * ub_cosf(lat), y = 0.5f * ub_sinf(lat);
        for (int i = 0; i < BALL_SLICES; ++i) {
            f32 a = UB_TWO_PI * (f32)i / BALL_SLICES;
            ring[s - 1][i] = add_vert(m, r * ub_cosf(a), y, r * ub_sinf(a));
        }
    }
    int south = add_vert(m, 0, -0.5f, 0);
    for (int i = 0; i < BALL_SLICES; ++i) {
        int j = (i + 1) % BALL_SLICES;
        face3(m, ring[0][i], ring[0][j], north);
        for (int s = 0; s < BALL_STACKS - 2; ++s)
            face4(m, ring[s + 1][i], ring[s + 1][j], ring[s][j], ring[s][i]);
        face3(m, ring[BALL_STACKS - 2][i], ring[BALL_STACKS - 2][j], south);
    }
}

void ub_meshes_init(void)
{
    for (int i = 0; i < UB_SHAPE_COUNT; ++i) {
        g_meshes[i].vert_count = 0;
        g_meshes[i].face_count = 0;
    }
    build_box(&g_meshes[UB_SHAPE_BOX]);
    build_wedge(&g_meshes[UB_SHAPE_WEDGE]);
    build_cylinder(&g_meshes[UB_SHAPE_CYLINDER]);
    build_ball(&g_meshes[UB_SHAPE_BALL]);
}

const ub_mesh *ub_mesh_get(ub_shape s)
{
    return &g_meshes[(unsigned)s < UB_SHAPE_COUNT ? s : UB_SHAPE_BOX];
}
