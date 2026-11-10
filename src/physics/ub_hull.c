#include "physics/ub_hull.h"
#include "physics/ub_pmath.h"
#include "core/ub_math.h"
#include "crt/ub_crt.h"

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

/* ------------------------------------------------- hulls from points
 * For the convex pieces of unions and MeshParts (milestone 8C). */

#define BUILD_EPS 1e-4f

typedef struct {
    vec3 n;
    f32 d;
} hplane;

/* 2D convex hull (monotone chain) of the points on a plane, as indices,
 * counter-clockwise in the plane's (u, w) basis, collinear points dropped. */
static int plane_polygon(const vec3 *pts, int n, const int *on, int count, vec3 normal, int *out)
{
    vec3 u = ub_absf(normal.x) < 0.9f ? v3(1, 0, 0) : v3(0, 1, 0);
    u = v3_normalize(v3_sub(u, v3_scale(normal, v3_dot(u, normal))));
    vec3 wv = v3_cross(normal, u);
    f32 px[64], py[64];
    int idx[64];
    if (count > 64) return 0;
    for (int i = 0; i < count; ++i) {
        idx[i] = on[i];
        px[i] = v3_dot(pts[on[i]], u);
        py[i] = v3_dot(pts[on[i]], wv);
    }
    (void)n;
    /* sort by x then y (insertion: few points) */
    for (int i = 1; i < count; ++i)
        for (int j = i; j > 0 && (px[j] < px[j - 1] || (px[j] == px[j - 1] && py[j] < py[j - 1])); --j) {
            f32 tx = px[j]; px[j] = px[j - 1]; px[j - 1] = tx;
            f32 ty = py[j]; py[j] = py[j - 1]; py[j - 1] = ty;
            int ti = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = ti;
        }
    int hull[130], k = 0;
#define CROSS(o, a, b) ((px[a] - px[o]) * (py[b] - py[o]) - (py[a] - py[o]) * (px[b] - px[o]))
    for (int i = 0; i < count; ++i) {
        while (k >= 2 && CROSS(hull[k - 2], hull[k - 1], i) <= 1e-7f) k--;
        hull[k++] = i;
    }
    for (int i = count - 2, t = k + 1; i >= 0; --i) {
        while (k >= t && CROSS(hull[k - 2], hull[k - 1], i) <= 1e-7f) k--;
        hull[k++] = i;
    }
#undef CROSS
    k--; /* the first point again */
    for (int i = 0; i < k; ++i) out[i] = idx[hull[i]];
    return k < 3 ? 0 : k;
}

/* Hull from points and the planes of its faces; false if it does not fit
 * the limits or comes out inconsistent. */
static bool from_planes(ub_hull *h, const vec3 *pts, int n, const hplane *pl, int np)
{
    memset(h, 0, sizeof *h);
    int map[256];
    for (int i = 0; i < n && i < 256; ++i) map[i] = -1;
    if (n > 256) return false;
    int faces[UB_HULL_MAX_FACES][64], counts[UB_HULL_MAX_FACES], nf = 0, total = 0;
    for (int p = 0; p < np; ++p) {
        int on[64], c = 0;
        for (int i = 0; i < n && c < 64; ++i)
            if (ub_absf(v3_dot(pl[p].n, pts[i]) - pl[p].d) < BUILD_EPS * 4) on[c++] = i;
        if (c < 3) continue;
        if (nf >= UB_HULL_MAX_FACES) return false;
        int k = plane_polygon(pts, n, on, c, pl[p].n, faces[nf]);
        if (k < 3) continue;
        counts[nf++] = k;
        total += k;
        if (total > UB_HULL_MAX_FACE_VERTS) return false;
    }
    if (nf < 4) return false;
    for (int f = 0; f < nf; ++f)
        for (int i = 0; i < counts[f]; ++i) {
            int v = faces[f][i];
            if (map[v] < 0) {
                if (h->vert_count >= UB_HULL_MAX_VERTS) return false;
                map[v] = h->vert_count;
                h->v[h->vert_count++] = pts[v];
            }
        }
    if (h->vert_count + nf - 2 > UB_HULL_MAX_EDGES) return false; /* Euler: E = V + F - 2 */
    for (int f = 0; f < nf; ++f) {
        int idx[64];
        for (int i = 0; i < counts[f]; ++i) idx[i] = map[faces[f][i]];
        face(h, counts[f], idx);
    }
    if (total / 2 > UB_HULL_MAX_EDGES) return false;
    finish(h);
    for (int e = 0; e < h->edge_count; ++e)
        if (h->e[e].f0 == h->e[e].f1) return false; /* an edge with one face: not closed */
    return h->volume > 1e-9f;
}

bool ub_hull_build(ub_hull *h, const vec3 *in, int n, const u32 *tri, int nt)
{
    if (!g_ready) build();
    /* unique points */
    vec3 pts[256];
    int np = 0;
    for (int i = 0; i < n && np <= 255; ++i) {
        bool dup = false;
        for (int j = 0; j < np && !dup; ++j) dup = v3_length(v3_sub(pts[j], in[i])) < BUILD_EPS;
        if (!dup) pts[np++] = in[i];
    }
    if (np < 4) return false;
    vec3 mid = v3(0, 0, 0);
    for (int i = 0; i < np; ++i) mid = v3_add(mid, pts[i]);
    mid = v3_scale(mid, 1.0f / (f32)np);
    /* exact: the planes of its triangles, coplanar ones merged */
    if (np <= 255) {
        hplane pl[UB_HULL_MAX_FACES + 1];
        int npl = 0;
        bool fits = true;
        for (int t = 0; t < nt && fits; ++t) {
            vec3 a = in[tri[t * 3]], b = in[tri[t * 3 + 1]], c = in[tri[t * 3 + 2]];
            vec3 nn = v3_cross(v3_sub(b, a), v3_sub(c, a));
            f32 len = v3_length(nn);
            if (len < 1e-9f) continue;
            nn = v3_scale(nn, 1.0f / len);
            if (v3_dot(nn, v3_sub(a, mid)) < 0) nn = v3_scale(nn, -1.0f);
            f32 d = v3_dot(nn, a);
            bool seen = false;
            for (int k = 0; k < npl && !seen; ++k) seen = v3_dot(pl[k].n, nn) > 0.9999f && ub_absf(pl[k].d - d) < BUILD_EPS * 4;
            if (seen) continue;
            if (npl >= UB_HULL_MAX_FACES) fits = false;
            else {
                pl[npl].n = nn;
                pl[npl].d = d;
                npl++;
            }
        }
        if (fits && from_planes(h, pts, np, pl, npl)) return true;
    }
    /* too detailed: the 18 planes around it (box, and its edges bevelled) */
    static const f32 dirs[9][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 }, { 1, -1, 0 },
                                    { 1, 0, 1 }, { 1, 0, -1 }, { 0, 1, 1 }, { 0, 1, -1 } };
    hplane pl[18];
    for (int k = 0; k < 18; ++k) {
        vec3 dn = v3_normalize(v3(dirs[k / 2][0], dirs[k / 2][1], dirs[k / 2][2]));
        if (k & 1) dn = v3_scale(dn, -1.0f);
        f32 best = -1e30f;
        for (int i = 0; i < np; ++i) best = UB_MAX(best, v3_dot(dn, pts[i]));
        pl[k].n = dn;
        pl[k].d = best;
    }
    /* its corners: where three planes meet, inside all of them */
    vec3 corners[256];
    int nc = 0;
    for (int a = 0; a < 18; ++a)
        for (int b = a + 1; b < 18; ++b)
            for (int c = b + 1; c < 18; ++c) {
                vec3 bc = v3_cross(pl[b].n, pl[c].n);
                f32 det = v3_dot(pl[a].n, bc);
                if (ub_absf(det) < 1e-6f) continue;
                vec3 x = v3_scale(v3_add(v3_add(v3_scale(bc, pl[a].d), v3_scale(v3_cross(pl[c].n, pl[a].n), pl[b].d)),
                                         v3_scale(v3_cross(pl[a].n, pl[b].n), pl[c].d)),
                                  1.0f / det);
                bool inside = true;
                for (int k = 0; k < 18 && inside; ++k) inside = v3_dot(pl[k].n, x) <= pl[k].d + BUILD_EPS * 4;
                if (!inside) continue;
                bool dup = false;
                for (int j = 0; j < nc && !dup; ++j) dup = v3_length(v3_sub(corners[j], x)) < BUILD_EPS * 4;
                if (!dup && nc < 256) corners[nc++] = x;
            }
    if (from_planes(h, corners, nc, pl, 18)) return true;
    /* last resort: its bounding box */
    vec3 lo = pts[0], hi = pts[0];
    for (int i = 1; i < np; ++i) {
        lo = v3(UB_MIN(lo.x, pts[i].x), UB_MIN(lo.y, pts[i].y), UB_MIN(lo.z, pts[i].z));
        hi = v3(UB_MAX(hi.x, pts[i].x), UB_MAX(hi.y, pts[i].y), UB_MAX(hi.z, pts[i].z));
    }
    memset(h, 0, sizeof *h);
    for (int i = 0; i < 8; ++i) vert(h, (i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
    face4(h, 1, 5, 7, 3);
    face4(h, 0, 4, 6, 2);
    face4(h, 2, 3, 7, 6);
    face4(h, 0, 1, 5, 4);
    face4(h, 4, 5, 7, 6);
    face4(h, 0, 1, 3, 2);
    finish(h);
    return h->volume > 1e-12f;
}
