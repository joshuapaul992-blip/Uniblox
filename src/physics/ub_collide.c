#include "physics/ub_collide.h"
#include "physics/ub_pmath.h"
#include "core/ub_math.h"

#define CLIP_MAX 64
#define NO_SEP (-3.0e38f)

/* A hull placed in the world. */
typedef struct {
    const ub_hull *h;
    vec3 v[UB_HULL_MAX_VERTS];
    vec3 n[UB_HULL_MAX_FACES];
    f32 d[UB_HULL_MAX_FACES];     /* plane: dot(n, p) = d */
    vec3 c;                       /* a point strictly inside */
} whull;

static void make_whull(const ub_cshape *s, whull *w)
{
    const ub_hull *h = s->hull;
    w->h = h;
    vec3 size = v3(UB_MAX(s->size.x, 0.01f), UB_MAX(s->size.y, 0.01f), UB_MAX(s->size.z, 0.01f));
    vec3 c = v3(0, 0, 0);
    for (int i = 0; i < h->vert_count; ++i) {
        w->v[i] = v3_add(s->pos, mat3_apply(&s->rot, v3_mul(h->v[i], size)));
        c = v3_add(c, w->v[i]);
    }
    w->c = v3_scale(c, 1.0f / (f32)h->vert_count);
    for (int f = 0; f < h->face_count; ++f) {
        vec3 n = h->n[f];
        /* normals scale by the inverse size */
        n = v3_normalize(v3(n.x / size.x, n.y / size.y, n.z / size.z));
        w->n[f] = mat3_apply(&s->rot, n);
        w->d[f] = v3_dot(w->n[f], w->v[h->fv[h->f[f].first]]);
    }
}

/* Largest separation of b from a's face planes. */
static f32 face_query(const whull *a, const whull *b, int *best)
{
    f32 max = NO_SEP;
    *best = 0;
    for (int f = 0; f < a->h->face_count; ++f) {
        vec3 n = a->n[f];
        f32 lo = 3.0e38f;
        for (int i = 0; i < b->h->vert_count; ++i) {
            f32 x = v3_dot(n, b->v[i]);
            if (x < lo) lo = x;
        }
        f32 s = lo - a->d[f];
        if (s > max) {
            max = s;
            *best = f;
        }
    }
    return max;
}

/* Edge pairs are only tested if their arcs on the Gauss map cross, i.e.
 * they form a face of the Minkowski difference (Gregorius, GDC 2013). */
typedef struct {
    f32 sep;
    int ea, eb;
    vec3 n;
} edge_result;

static void edge_query(const whull *a, const whull *b, edge_result *r)
{
    r->sep = NO_SEP;
    r->ea = r->eb = -1;
    /* the Gauss-map arcs of b's edges (negated normals), worked out once */
    vec3 c2[UB_HULL_MAX_EDGES], d2s[UB_HULL_MAX_EDGES], dxc[UB_HULL_MAX_EDGES];
    for (int j = 0; j < b->h->edge_count; ++j) {
        const ub_hull_edge *e2 = &b->h->e[j];
        c2[j] = v3_neg(b->n[e2->f0]);
        d2s[j] = v3_neg(b->n[e2->f1]);
        dxc[j] = v3_cross(d2s[j], c2[j]);
    }
    for (int i = 0; i < a->h->edge_count; ++i) {
        const ub_hull_edge *e1 = &a->h->e[i];
        vec3 p1 = a->v[e1->a], d1 = v3_sub(a->v[e1->b], p1);
        vec3 u1 = a->n[e1->f0], v1 = a->n[e1->f1];
        vec3 bxa = v3_cross(v1, u1);
        f32 d1len2 = v3_length2(d1);
        for (int j = 0; j < b->h->edge_count; ++j) {
            /* inline minkowski_face(u1, v1, c2[j], d2s[j]) with the crosses hoisted */
            f32 cba = v3_dot(c2[j], bxa), dba = v3_dot(d2s[j], bxa);
            if (cba * dba >= 0) continue;
            f32 adc = v3_dot(u1, dxc[j]), bdc = v3_dot(v1, dxc[j]);
            if (adc * bdc >= 0 || cba * bdc <= 0) continue;
            const ub_hull_edge *e2 = &b->h->e[j];
            vec3 p2 = b->v[e2->a], d2 = v3_sub(b->v[e2->b], p2);
            vec3 n = v3_cross(d1, d2);
            f32 len = v3_length(n);
            if (len < 1e-4f * ub_sqrtf(d1len2 * v3_length2(d2))) continue; /* parallel */
            n = v3_scale(n, 1.0f / len);
            if (v3_dot(n, v3_sub(p1, a->c)) < 0) n = v3_neg(n);
            f32 s = v3_dot(n, v3_sub(p2, p1));
            if (s > r->sep) {
                r->sep = s;
                r->ea = i;
                r->eb = j;
                r->n = n;
            }
        }
    }
}

/* Closest points between segments p1+s*d1 and p2+t*d2, s, t in [0, 1]. */
static void segment_closest(vec3 p1, vec3 d1, vec3 p2, vec3 d2, vec3 *c1, vec3 *c2)
{
    vec3 r = v3_sub(p1, p2);
    f32 a = v3_dot(d1, d1), e = v3_dot(d2, d2), f = v3_dot(d2, r);
    f32 s = 0, t = 0;
    if (a > 1e-12f && e > 1e-12f) {
        f32 b = v3_dot(d1, d2), c = v3_dot(d1, r);
        f32 den = a * e - b * b;
        s = den > 1e-12f ? ub_clampf((b * f - c * e) / den, 0, 1) : 0;
        t = (b * s + f) / e;
        if (t < 0) { t = 0; s = ub_clampf(-c / a, 0, 1); }
        else if (t > 1) { t = 1; s = ub_clampf((b - c) / a, 0, 1); }
    } else if (e > 1e-12f) {
        t = ub_clampf(f / e, 0, 1);
    } else if (a > 1e-12f) {
        s = ub_clampf(-v3_dot(d1, r) / a, 0, 1);
    }
    *c1 = v3_madd(p1, d1, s);
    *c2 = v3_madd(p2, d2, t);
}

/* Keep the polygon on the inner side of a plane (dot(n, p) <= d). */
static int clip(const vec3 *in, int n_in, vec3 *out, vec3 n, f32 d)
{
    int n_out = 0;
    if (n_in == 0) return 0;
    vec3 a = in[n_in - 1];
    f32 da = v3_dot(n, a) - d;
    for (int i = 0; i < n_in; ++i) {
        vec3 b = in[i];
        f32 db = v3_dot(n, b) - d;
        if (da <= 0 && db <= 0) {
            if (n_out < CLIP_MAX) out[n_out++] = b;
        } else if (da <= 0 || db <= 0) {
            vec3 x = v3_add(a, v3_scale(v3_sub(b, a), da / (da - db)));
            if (n_out < CLIP_MAX) out[n_out++] = x;
            if (db <= 0 && n_out < CLIP_MAX) out[n_out++] = b;
        }
        a = b;
        da = db;
    }
    return n_out;
}

/* Down to four points that keep the deepest one and the largest area. */
static int reduce(ub_cpoint *pts, int n, vec3 normal, ub_cpoint out[UB_MAX_CONTACTS])
{
    if (n <= UB_MAX_CONTACTS) {
        for (int i = 0; i < n; ++i) out[i] = pts[i];
        return n;
    }
    int i0 = 0;
    for (int i = 1; i < n; ++i)
        if (pts[i].sep < pts[i0].sep) i0 = i;
    int i1 = -1;
    f32 best = -1;
    for (int i = 0; i < n; ++i) {
        f32 d = v3_length2(v3_sub(pts[i].point, pts[i0].point));
        if (i != i0 && d > best) { best = d; i1 = i; }
    }
    int i2 = -1, i3 = -1;
    f32 hi = 0, lo = 0;
    vec3 a = pts[i0].point, b = pts[i1].point;
    for (int i = 0; i < n; ++i) {
        if (i == i0 || i == i1) continue;
        f32 area = v3_dot(v3_cross(v3_sub(b, a), v3_sub(pts[i].point, a)), normal);
        if (area > hi) { hi = area; i2 = i; }
        if (area < lo) { lo = area; i3 = i; }
    }
    int k = 0;
    out[k++] = pts[i0];
    out[k++] = pts[i1];
    if (i2 >= 0) out[k++] = pts[i2];
    if (i3 >= 0) out[k++] = pts[i3];
    return k;
}

static int hull_hull(const ub_cshape *sa, const ub_cshape *sb, f32 margin, vec3 *normal, ub_cpoint out[UB_MAX_CONTACTS])
{
    whull A, B;
    make_whull(sa, &A);
    make_whull(sb, &B);
    int fa, fb;
    f32 sep_a = face_query(&A, &B, &fa);
    if (sep_a > margin) return 0;
    f32 sep_b = face_query(&B, &A, &fb);
    if (sep_b > margin) return 0;
    edge_result er;
    edge_query(&A, &B, &er);
    if (er.sep > margin) return 0;

    f32 face_sep = UB_MAX(sep_a, sep_b);
    if (er.ea >= 0 && er.sep > 0.98f * face_sep + 0.005f) {
        const ub_hull_edge *e1 = &A.h->e[er.ea], *e2 = &B.h->e[er.eb];
        vec3 p1 = A.v[e1->a], p2 = B.v[e2->a];
        vec3 c1, c2;
        segment_closest(p1, v3_sub(A.v[e1->b], p1), p2, v3_sub(B.v[e2->b], p2), &c1, &c2);
        *normal = er.n;
        out[0].point = v3_scale(v3_add(c1, c2), 0.5f);
        out[0].sep = er.sep;
        return 1;
    }

    /* face contact: the reference face is on the hull that separates more */
    bool flip = sep_b > 0.98f * sep_a + 0.001f;
    const whull *R = flip ? &B : &A, *I = flip ? &A : &B;
    int rf = flip ? fb : fa;
    vec3 nr = R->n[rf];
    int inc = 0;
    f32 lo = 3.0e38f;
    for (int f = 0; f < I->h->face_count; ++f) {
        f32 d = v3_dot(I->n[f], nr);
        if (d < lo) { lo = d; inc = f; }
    }
    vec3 buf1[CLIP_MAX], buf2[CLIP_MAX];
    const ub_hull_face *ifc = &I->h->f[inc];
    int n = 0;
    for (int i = 0; i < ifc->count; ++i) buf1[n++] = I->v[I->h->fv[ifc->first + i]];
    const ub_hull_face *rfc = &R->h->f[rf];
    vec3 *cur = buf1, *nxt = buf2;
    for (int k = 0; k < rfc->count && n > 0; ++k) {
        vec3 v0 = R->v[R->h->fv[rfc->first + k]], v1 = R->v[R->h->fv[rfc->first + (k + 1) % rfc->count]];
        vec3 sn = v3_normalize(v3_cross(v3_sub(v1, v0), nr));
        n = clip(cur, n, nxt, sn, v3_dot(sn, v0));
        vec3 *t = cur;
        cur = nxt;
        nxt = t;
    }
    ub_cpoint pts[CLIP_MAX];
    int np = 0;
    for (int i = 0; i < n; ++i) {
        f32 s = v3_dot(nr, cur[i]) - R->d[rf];
        if (s > margin) continue;
        /* clipping along an edge that lies on a side plane repeats points */
        bool dup = false;
        for (int j = 0; j < np && !dup; ++j) dup = v3_length2(v3_sub(pts[j].point, v3_madd(cur[i], nr, -0.5f * s))) < 1e-6f;
        if (dup) continue;
        pts[np].point = v3_madd(cur[i], nr, -0.5f * s);
        pts[np].sep = s;
        np++;
    }
    *normal = flip ? v3_neg(nr) : nr;
    return reduce(pts, np, nr, out);
}

/* Closest point on hull H to point c; returns the distance (c outside). */
static f32 hull_closest(const whull *H, vec3 c, vec3 *q)
{
    f32 best = 3.0e38f;
    for (int f = 0; f < H->h->face_count; ++f) {
        vec3 n = H->n[f];
        f32 s = v3_dot(n, c) - H->d[f];
        if (s <= 0) continue; /* c is behind this face: it cannot be the closest */
        const ub_hull_face *fc = &H->h->f[f];
        vec3 p = v3_madd(c, n, -s);
        bool inside = true;
        for (int k = 0; k < fc->count; ++k) {
            vec3 v0 = H->v[H->h->fv[fc->first + k]], v1 = H->v[H->h->fv[fc->first + (k + 1) % fc->count]];
            vec3 sn = v3_cross(v3_sub(v1, v0), n);
            if (v3_dot(sn, v3_sub(p, v0)) > 0) {
                inside = false;
                /* closest point on this edge */
                vec3 e = v3_sub(v1, v0);
                f32 t = ub_clampf(v3_dot(v3_sub(c, v0), e) / UB_MAX(v3_dot(e, e), 1e-12f), 0, 1);
                vec3 x = v3_madd(v0, e, t);
                f32 d = v3_length(v3_sub(c, x));
                if (d < best) { best = d; *q = x; }
            }
        }
        if (inside && s < best) {
            best = s;
            *q = p;
        }
    }
    return best;
}

static int sphere_hull(const ub_cshape *sph, const ub_cshape *hs, f32 margin, vec3 *normal, ub_cpoint *out)
{
    whull H;
    make_whull(hs, &H);
    vec3 c = sph->pos;
    f32 r = sph->radius;
    f32 max_s = NO_SEP;
    int max_f = 0;
    for (int f = 0; f < H.h->face_count; ++f) {
        f32 s = v3_dot(H.n[f], c) - H.d[f];
        if (s > max_s) { max_s = s; max_f = f; }
    }
    if (max_s > r + margin) return 0;
    if (max_s <= 0) { /* centre inside the hull: push out through the nearest face */
        *normal = H.n[max_f];
        out->sep = max_s - r;
        out->point = v3_madd(c, *normal, -0.5f * (max_s + r));
        return 1;
    }
    vec3 q = c;
    f32 dist = hull_closest(&H, c, &q);
    if (dist > r + margin) return 0;
    vec3 d = v3_sub(c, q);
    *normal = dist > 1e-6f ? v3_scale(d, 1.0f / dist) : H.n[max_f];
    out->sep = dist - r;
    out->point = v3_scale(v3_add(q, v3_madd(c, *normal, -r)), 0.5f);
    return 1;
}

int ub_collide(const ub_cshape *a, const ub_cshape *b, f32 margin, vec3 *normal, ub_cpoint out[UB_MAX_CONTACTS])
{
    if (a->sphere && b->sphere) {
        vec3 d = v3_sub(b->pos, a->pos);
        f32 dist = v3_length(d);
        f32 sep = dist - a->radius - b->radius;
        if (sep > margin) return 0;
        *normal = dist > 1e-6f ? v3_scale(d, 1.0f / dist) : v3(0, 1, 0);
        out[0].sep = sep;
        out[0].point = v3_madd(a->pos, *normal, a->radius + 0.5f * sep);
        return 1;
    }
    if (a->sphere) {
        int n = sphere_hull(a, b, margin, normal, out);
        *normal = v3_neg(*normal); /* it pointed from the hull to the sphere */
        return n;
    }
    if (b->sphere) return sphere_hull(b, a, margin, normal, out);
    /* quick reject with bounding spheres */
    f32 ra = 0.5f * v3_length(a->size), rb = 0.5f * v3_length(b->size);
    if (v3_length2(v3_sub(a->pos, b->pos)) > (ra + rb + margin) * (ra + rb + margin)) return 0;
    return hull_hull(a, b, margin, normal, out);
}

bool ub_shape_raycast(const ub_cshape *s, vec3 origin, vec3 dir, f32 max_dist, f32 *t, vec3 *normal)
{
    if (s->sphere) {
        vec3 oc = v3_sub(origin, s->pos);
        f32 b = v3_dot(oc, dir), c = v3_dot(oc, oc) - s->radius * s->radius;
        if (c <= 0) { /* inside */
            *t = 0;
            *normal = v3_normalize(oc);
            if (v3_length2(*normal) < 0.5f) *normal = v3(0, 1, 0);
            return true;
        }
        f32 disc = b * b - c;
        if (b > 0 || disc < 0) return false;
        f32 tt = -b - ub_sqrtf(disc);
        if (tt > max_dist) return false;
        *t = tt;
        *normal = v3_normalize(v3_madd(oc, dir, tt));
        return true;
    }
    whull H;
    make_whull(s, &H);
    f32 t_in = 0, t_out = max_dist;
    int in_face = -1, near_face = 0;
    f32 near_s = NO_SEP;
    for (int f = 0; f < H.h->face_count; ++f) {
        f32 num = H.d[f] - v3_dot(H.n[f], origin), den = v3_dot(H.n[f], dir);
        if (-num > near_s) { near_s = -num; near_face = f; }
        if (den > -1e-9f && den < 1e-9f) {
            if (num < 0) return false; /* parallel and outside */
            continue;
        }
        f32 tt = num / den;
        if (den < 0) {
            if (tt > t_in) { t_in = tt; in_face = f; }
        } else if (tt < t_out) {
            t_out = tt;
        }
        if (t_in > t_out) return false;
    }
    *t = t_in;
    *normal = H.n[in_face >= 0 ? in_face : near_face];
    return true;
}
