#include "physics/ub_hull.h"
#include "physics/ub_pmath.h"
#include "core/ub_math.h"

#define CYL_SIDES 12

static ub_hull g_hulls[UB_SHAPE_COUNT];
static bool g_ready;

static int vert(ub_hull *h, f32 x, f32 y, f32 z)
{
    h->v[h->vert_count] = v3(x, y, z);
    return h->vert_count++;
}

/* Adds a face; its vertices may be given in either winding. */
static void face(ub_hull *h, int count, const int *idx)
{
    vec3 n = v3(0, 0, 0), c = v3(0, 0, 0), inside = v3(0, 0, 0);
    for (int i = 0; i < count; ++i) {
        vec3 a = h->v[idx[i]], b = h->v[idx[(i + 1) % count]];
        n.x += (a.y - b.y) * (a.z + b.z);
        n.y += (a.z - b.z) * (a.x + b.x);
        n.z += (a.x - b.x) * (a.y + b.y);
        c = v3_add(c, a);
    }
    for (int i = 0; i < h->vert_count; ++i) inside = v3_add(inside, h->v[i]);
    inside = v3_scale(inside, 1.0f / (f32)h->vert_count);
    c = v3_scale(c, 1.0f / (f32)count);
    /* Newell's normal follows the winding (counter-clockwise = towards the
     * viewer in a right-handed sense); flip the order if it points in */
    bool reverse = v3_dot(n, v3_sub(c, inside)) < 0;
    int fi = h->face_count++;
    h->n[fi] = v3_normalize(reverse ? v3_neg(n) : n);
    h->f[fi].first = (u8)(fi == 0 ? 0 : h->f[fi - 1].first + h->f[fi - 1].count);
    h->f[fi].count = (u8)count;
    for (int i = 0; i < count; ++i) h->fv[h->f[fi].first + i] = (u8)idx[reverse ? count - 1 - i : i];
}

static void face4(ub_hull *h, int a, int b, int c, int d)
{
    int idx[4] = { a, b, c, d };
    face(h, 4, idx);
}

static void face3(ub_hull *h, int a, int b, int c)
{
    int idx[3] = { a, b, c };
    face(h, 3, idx);
}

/* Edges from the faces, and the solid's mass properties. */
static void finish(ub_hull *h)
{
    h->edge_count = 0;
    for (int f = 0; f < h->face_count; ++f) {
        const ub_hull_face *fc = &h->f[f];
        for (int i = 0; i < fc->count; ++i) {
            u8 a = h->fv[fc->first + i], b = h->fv[fc->first + (i + 1) % fc->count];
            int found = -1;
            for (int e = 0; e < h->edge_count; ++e)
                if (h->e[e].a == b && h->e[e].b == a) found = e;
            if (found >= 0) {
                h->e[found].f1 = (u8)f;
            } else {
                ub_hull_edge *e = &h->e[h->edge_count++];
                e->a = a;
                e->b = b;
                e->f0 = e->f1 = (u8)f;
            }
        }
    }

    /* Tetrahedra from the origin to each face triangle (Blow & Binstock):
     * for a tetrahedron (0, a, b, c), volume = det/6 and the second moment
     * is det * A K Aᵀ with A = [a b c], K = [[2 1 1] [1 2 1] [1 1 2]] / 120. */
    f32 vol = 0;
    vec3 cen = v3(0, 0, 0);
    f32 m[3][3] = { { 0 } };
    for (int f = 0; f < h->face_count; ++f) {
        const ub_hull_face *fc = &h->f[f];
        vec3 a = h->v[h->fv[fc->first]];
        for (int i = 1; i + 1 < fc->count; ++i) {
            vec3 b = h->v[h->fv[fc->first + i]], c = h->v[h->fv[fc->first + i + 1]];
            f32 det = v3_dot(a, v3_cross(b, c));
            vol += det / 6.0f;
            cen = v3_add(cen, v3_scale(v3_add(v3_add(a, b), c), det / 24.0f));
            const f32 p[3][3] = { { a.x, b.x, c.x }, { a.y, b.y, c.y }, { a.z, b.z, c.z } };
            for (int r = 0; r < 3; ++r)
                for (int s = 0; s < 3; ++s) {
                    /* (A K Aᵀ)[r][s] = sum_ij A[r][i] K[i][j] A[s][j] */
                    f32 sum = 0;
                    for (int i2 = 0; i2 < 3; ++i2)
                        for (int j2 = 0; j2 < 3; ++j2)
                            sum += p[r][i2] * (i2 == j2 ? 2.0f : 1.0f) * p[s][j2];
                    m[r][s] += det * sum / 120.0f;
                }
        }
    }
    h->volume = vol;
    h->centroid = vol > 0 ? v3_scale(cen, 1.0f / vol) : v3(0, 0, 0);
    h->second_moment.x = v3(m[0][0], m[1][0], m[2][0]);
    h->second_moment.y = v3(m[0][1], m[1][1], m[2][1]);
    h->second_moment.z = v3(m[0][2], m[1][2], m[2][2]);
}

static void build(void)
{
    ub_hull *h = &g_hulls[UB_SHAPE_BOX];
    for (int i = 0; i < 8; ++i) vert(h, (i & 1) ? 0.5f : -0.5f, (i & 2) ? 0.5f : -0.5f, (i & 4) ? 0.5f : -0.5f);
    face4(h, 1, 5, 7, 3);
    face4(h, 0, 4, 6, 2);
    face4(h, 2, 3, 7, 6);
    face4(h, 0, 1, 5, 4);
    face4(h, 4, 5, 7, 6);
    face4(h, 0, 1, 3, 2);
    finish(h);

    h = &g_hulls[UB_SHAPE_WEDGE];
    int b0 = vert(h, -0.5f, -0.5f, -0.5f), b1 = vert(h, 0.5f, -0.5f, -0.5f);
    int b2 = vert(h, 0.5f, -0.5f, 0.5f), b3 = vert(h, -0.5f, -0.5f, 0.5f);
    int t0 = vert(h, -0.5f, 0.5f, 0.5f), t1 = vert(h, 0.5f, 0.5f, 0.5f);
    face4(h, b0, b1, b2, b3);
    face4(h, b3, b2, t1, t0);
    face4(h, b0, b1, t1, t0);
    face3(h, b0, b3, t0);
    face3(h, b1, b2, t1);
    finish(h);

    h = &g_hulls[UB_SHAPE_CORNER_WEDGE];
    b0 = vert(h, -0.5f, -0.5f, -0.5f);
    b1 = vert(h, 0.5f, -0.5f, -0.5f);
    b2 = vert(h, 0.5f, -0.5f, 0.5f);
    b3 = vert(h, -0.5f, -0.5f, 0.5f);
    int apex = vert(h, -0.5f, 0.5f, 0.5f);
    face4(h, b0, b1, b2, b3);
    face3(h, b3, b2, apex);
    face3(h, b3, b0, apex);
    face3(h, b0, b1, apex);
    face3(h, b1, b2, apex);
    finish(h);

    h = &g_hulls[UB_SHAPE_CYLINDER];
    int bot[CYL_SIDES], top[CYL_SIDES];
    for (int i = 0; i < CYL_SIDES; ++i) {
        f32 a = UB_TWO_PI * (f32)i / (f32)CYL_SIDES;
        bot[i] = vert(h, 0.5f * ub_cosf(a), -0.5f, 0.5f * ub_sinf(a));
        top[i] = vert(h, 0.5f * ub_cosf(a), 0.5f, 0.5f * ub_sinf(a));
    }
    for (int i = 0; i < CYL_SIDES; ++i) face4(h, bot[i], bot[(i + 1) % CYL_SIDES], top[(i + 1) % CYL_SIDES], top[i]);
    face(h, CYL_SIDES, top);
    face(h, CYL_SIDES, bot);
    finish(h);

    g_hulls[UB_SHAPE_BALL] = g_hulls[UB_SHAPE_BOX];
    g_ready = true;
}

const ub_hull *ub_hull_get(ub_shape s)
{
    if (!g_ready) build();
    return &g_hulls[(unsigned)s < UB_SHAPE_COUNT ? s : UB_SHAPE_BOX];
}

void ub_shape_mass(ub_shape s, vec3 size, f32 *volume, vec3 *centroid, mat3 *inertia)
{
    if (s == UB_SHAPE_BALL) {
        f32 r = 0.5f * UB_MIN(size.x, UB_MIN(size.y, size.z));
        f32 v = 4.0f / 3.0f * UB_PI * r * r * r;
        *volume = v;
        *centroid = v3(0, 0, 0);
        f32 i = 0.4f * v * r * r;
        mat3 m = { { i, 0, 0 }, { 0, i, 0 }, { 0, 0, i } };
        *inertia = m;
        return;
    }
    const ub_hull *h = ub_hull_get(s);
    /* scaling by S = diag(size): volume * det S, centroid S c, moment det S * S M S */
    f32 det = size.x * size.y * size.z;
    f32 v = h->volume * det;
    vec3 c = v3_mul(h->centroid, size);
    const f32 sz[3] = { size.x, size.y, size.z };
    const vec3 *cols[3] = { &h->second_moment.x, &h->second_moment.y, &h->second_moment.z };
    f32 m[3][3];
    for (int col = 0; col < 3; ++col) {
        const f32 *cv = &cols[col]->x;
        for (int row = 0; row < 3; ++row) m[row][col] = det * sz[row] * cv[row] * sz[col];
    }
    /* about the centre of mass, then I = trace(M) E - M */
    const f32 cc[3] = { c.x, c.y, c.z };
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) m[r][k] -= v * cc[r] * cc[k];
    f32 tr = m[0][0] + m[1][1] + m[2][2];
    mat3 out;
    out.x = v3(tr - m[0][0], -m[1][0], -m[2][0]);
    out.y = v3(-m[0][1], tr - m[1][1], -m[2][1]);
    out.z = v3(-m[0][2], -m[1][2], tr - m[2][2]);
    *volume = v;
    *centroid = c;
    *inertia = out;
}
