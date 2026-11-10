#include "render/ub_mesh.h"
#include "platform/platform.h"

static const int k_cyl_sides[UB_DETAIL_COUNT] = { 12, 32 };
static const int k_ball_stacks[UB_DETAIL_COUNT] = { 6, 16 };
static const int k_ball_slices[UB_DETAIL_COUNT] = { 10, 32 };

static ub_mesh g_meshes[UB_DETAIL_COUNT][UB_SHAPE_COUNT];
static vec3 g_verts[UB_DETAIL_COUNT][UB_SHAPE_COUNT][UB_MESH_MAX_VERTS];
static vec3 g_vnormals[UB_DETAIL_COUNT][UB_SHAPE_COUNT][UB_MESH_MAX_VERTS];
static ub_face g_faces[UB_DETAIL_COUNT][UB_SHAPE_COUNT][UB_MESH_MAX_FACES];

static int add_vert(ub_mesh *m, f32 x, f32 y, f32 z)
{
    m->verts[m->vert_count] = v3(x, y, z);
    m->vnormals[m->vert_count] = v3_normalize(v3(x, y, z));
    return m->vert_count++;
}

/* Adds a face and computes its outward normal (Newell's method, then
 * flipped if it points towards the inside). Every shape is convex, so the
 * average of its vertices is strictly inside. (The origin is not enough:
 * the wedge's slope passes right through it.) All vertices must be added
 * before the faces. */
static ub_face *add_face(ub_mesh *m, int count, const int *idx)
{
    ub_face *f = &m->faces[m->face_count++];
    f->count = (u8)count;
    f->smooth = false;
    vec3 n = v3(0, 0, 0), c = v3(0, 0, 0);
    for (int i = 0; i < count; ++i) {
        f->v[i] = (u16)idx[i];
        vec3 a = m->verts[idx[i]], b = m->verts[idx[(i + 1) % count]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
        c = v3_add(c, a);
    }
    n = v3_normalize(n);
    vec3 inside = v3(0, 0, 0);
    for (int i = 0; i < m->vert_count; ++i) inside = v3_add(inside, m->verts[i]);
    inside = v3_scale(inside, 1.0f / (f32)m->vert_count);
    c = v3_scale(c, 1.0f / (f32)count);
    if (v3_dot(n, v3_sub(c, inside)) < 0) n = v3_scale(n, -1.0f);
    f->normal = n;
    return f;
}

static ub_face *face4(ub_mesh *m, int a, int b, int c, int d)
{
    int idx[4] = { a, b, c, d };
    return add_face(m, 4, idx);
}

static ub_face *face3(ub_mesh *m, int a, int b, int c)
{
    int idx[3] = { a, b, c };
    return add_face(m, 3, idx);
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

/* Apex above the (-X, +Z) bottom corner: two upright triangles (back and
 * left) and two slopes. This matches an imported Roblox CornerWedgePart
 * whose apex is at its back-right corner, as best understood. */
static void build_corner_wedge(ub_mesh *m)
{
    int b0 = add_vert(m, -0.5f, -0.5f, -0.5f), b1 = add_vert(m, 0.5f, -0.5f, -0.5f);
    int b2 = add_vert(m, 0.5f, -0.5f, 0.5f), b3 = add_vert(m, -0.5f, -0.5f, 0.5f);
    int apex = add_vert(m, -0.5f, 0.5f, 0.5f);
    face4(m, b0, b1, b2, b3); /* bottom */
    face3(m, b3, b2, apex);   /* back wall (+Z) */
    face3(m, b3, b0, apex);   /* left wall (-X) */
    face3(m, b0, b1, apex);   /* front slope */
    face3(m, b1, b2, apex);   /* right slope */
}

static void build_cylinder(ub_mesh *m, int sides)
{
    int bot[UB_FACE_MAX_VERTS], top[UB_FACE_MAX_VERTS];
    for (int i = 0; i < sides; ++i) {
        f32 a = UB_TWO_PI * (f32)i / (f32)sides;
        f32 x = 0.5f * ub_cosf(a), z = 0.5f * ub_sinf(a);
        bot[i] = add_vert(m, x, -0.5f, z);
        top[i] = add_vert(m, x, 0.5f, z);
        /* side normals point straight out, not towards the corners */
        m->vnormals[bot[i]] = m->vnormals[top[i]] = v3_normalize(v3(x, 0, z));
    }
    for (int i = 0; i < sides; ++i) {
        int j = (i + 1) % sides;
        face4(m, bot[i], bot[j], top[j], top[i])->smooth = true;
    }
    add_face(m, sides, top);
    add_face(m, sides, bot);
}

static void build_ball(ub_mesh *m, int stacks, int slices)
{
    int ring[16][32];
    int north = add_vert(m, 0, 0.5f, 0);
    for (int s = 1; s < stacks; ++s) {
        f32 lat = UB_HALF_PI - UB_PI * (f32)s / (f32)stacks;
        f32 r = 0.5f * ub_cosf(lat), y = 0.5f * ub_sinf(lat);
        for (int i = 0; i < slices; ++i) {
            f32 a = UB_TWO_PI * (f32)i / (f32)slices;
            ring[s - 1][i] = add_vert(m, r * ub_cosf(a), y, r * ub_sinf(a));
        }
    }
    int south = add_vert(m, 0, -0.5f, 0);
    for (int i = 0; i < slices; ++i) {
        int j = (i + 1) % slices;
        face3(m, ring[0][i], ring[0][j], north)->smooth = true;
        for (int s = 0; s < stacks - 2; ++s)
            face4(m, ring[s + 1][i], ring[s + 1][j], ring[s][j], ring[s][i])->smooth = true;
        face3(m, ring[stacks - 2][i], ring[stacks - 2][j], south)->smooth = true;
    }
}

static bool g_meshes_ready;

void ub_meshes_init(void)
{
    g_meshes_ready = true;
    for (int d = 0; d < UB_DETAIL_COUNT; ++d) {
        for (int i = 0; i < UB_SHAPE_COUNT; ++i) {
            g_meshes[d][i].vert_count = 0;
            g_meshes[d][i].face_count = 0;
            g_meshes[d][i].verts = g_verts[d][i];
            g_meshes[d][i].vnormals = g_vnormals[d][i];
            g_meshes[d][i].faces = g_faces[d][i];
        }
        build_box(&g_meshes[d][UB_SHAPE_BOX]);
        build_wedge(&g_meshes[d][UB_SHAPE_WEDGE]);
        build_cylinder(&g_meshes[d][UB_SHAPE_CYLINDER], k_cyl_sides[d]);
        build_ball(&g_meshes[d][UB_SHAPE_BALL], k_ball_stacks[d], k_ball_slices[d]);
        build_corner_wedge(&g_meshes[d][UB_SHAPE_CORNER_WEDGE]);
    }
}

const ub_mesh *ub_mesh_get(ub_shape s, ub_detail d)
{
    if (!g_meshes_ready) ub_meshes_init(); /* first use (queries can come before any renderer) */
    return &g_meshes[(unsigned)d < UB_DETAIL_COUNT ? d : UB_DETAIL_LOW]
                    [(unsigned)s < UB_SHAPE_COUNT ? s : UB_SHAPE_BOX];
}

ub_mesh *ub_mesh_from_triangles(const f32 *v, u32 nv, const u32 *tri, u32 nt, const u32 *piece_v,
                                const u32 *piece_t, u32 npieces, bool wound)
{
    if (nv > 0xFFFF) return NULL; /* faces index vertices with 16 bits */
    size_t bytes = sizeof(ub_mesh) + sizeof(vec3) * 2 * nv + sizeof(ub_face) * nt;
    u8 *block = (u8 *)plat_alloc(bytes);
    if (!block) return NULL;
    ub_mesh *m = (ub_mesh *)block;
    m->verts = (vec3 *)(block + sizeof(ub_mesh));
    m->vnormals = m->verts + nv;
    m->faces = (ub_face *)(m->vnormals + nv);
    m->vert_count = (int)nv;
    for (u32 i = 0; i < nv; ++i) {
        m->verts[i] = v3(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
        m->vnormals[i] = v3(0, 1, 0);
    }
    u32 v0 = 0, t0 = 0;
    for (u32 p = 0; p < npieces; ++p) {
        vec3 c = v3(0, 0, 0);
        for (u32 i = v0; i < piece_v[p]; ++i) c = v3_add(c, m->verts[i]);
        if (piece_v[p] > v0) c = v3_scale(c, 1.0f / (f32)(piece_v[p] - v0));
        for (u32 t = t0; t < piece_t[p]; ++t) {
            u32 a = tri[t * 3], b = tri[t * 3 + 1], cc = tri[t * 3 + 2];
            vec3 n = v3_cross(v3_sub(m->verts[b], m->verts[a]), v3_sub(m->verts[cc], m->verts[a]));
            f32 len = v3_length(n);
            if (len < 1e-12f) continue; /* degenerate */
            n = v3_scale(n, 1.0f / len);
            ub_face *f = &m->faces[m->face_count++];
            f->count = 3;
            f->smooth = false;
            bool flip = !wound && v3_dot(n, v3_sub(m->verts[a], c)) < 0; /* convex piece: out is away from its middle */
            f->v[0] = (u16)a;
            f->v[1] = (u16)(flip ? cc : b);
            f->v[2] = (u16)(flip ? b : cc);
            f->normal = flip ? v3_scale(n, -1.0f) : n;
        }
        v0 = piece_v[p];
        t0 = piece_t[p];
    }
    return m;
}
