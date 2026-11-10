/* Uniblox - the GUI's 2D rasteriser (see ub_canvas.h).
 *
 * Every primitive works the same way: walk the canvas pixels the shape's
 * transformed box covers (inside the clip), map each pixel centre back
 * into the box, and work out coverage (from a signed distance to the
 * rounded rectangle, which also anti-aliases rotated edges), colour
 * (paint, gradient, image or mask) and blend. Unrotated rectangles with
 * square corners skip the distance maths. */
#include "gui/ub_canvas.h"
#include "core/ub_math.h"
#include "core/ub_vec.h"
#include "crt/ub_crt.h"

void ub_canvas_init(ub_canvas *c, u32 *px, int w, int h, int pitch)
{
    c->px = px;
    c->w = w;
    c->h = h;
    c->pitch = pitch;
    c->cx0 = c->cy0 = 0;
    c->cx1 = w;
    c->cy1 = h;
}

void ub_canvas_clip(ub_canvas *c, int x0, int y0, int x1, int y1)
{
    c->cx0 = x0 < 0 ? 0 : x0;
    c->cy0 = y0 < 0 ? 0 : y0;
    c->cx1 = x1 > c->w ? c->w : x1;
    c->cy1 = y1 > c->h ? c->h : y1;
}

void ub_canvas_clear(ub_canvas *c, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int y = y0; y < y1; ++y) memset(c->px + (size_t)y * c->pitch + x0, 0, sizeof(u32) * (size_t)(x1 > x0 ? x1 - x0 : 0));
}

/* ---------------------------------------------------------- transforms */

ub_xform ub_xform_place(f32 x, f32 y)
{
    ub_xform t = { 1, 0, 0, 1, x, y };
    return t;
}

ub_xform ub_xform_rotated(f32 x, f32 y, f32 w, f32 h, f32 degrees)
{
    f32 r = degrees * (UB_PI / 180.0f), cs = ub_cosf(r), sn = ub_sinf(r);
    if (degrees == 0) cs = 1, sn = 0;
    f32 cx = x + w * 0.5f, cy = y + h * 0.5f;
    ub_xform t;
    /* screen y points down, so a positive angle turns clockwise */
    t.a = cs;
    t.b = sn;
    t.c = -sn;
    t.d = cs;
    t.tx = cx - (cs * w * 0.5f - sn * h * 0.5f);
    t.ty = cy - (sn * w * 0.5f + cs * h * 0.5f);
    return t;
}

ub_xform ub_xform_mul(const ub_xform *o, const ub_xform *i)
{
    ub_xform t;
    t.a = o->a * i->a + o->c * i->b;
    t.b = o->b * i->a + o->d * i->b;
    t.c = o->a * i->c + o->c * i->d;
    t.d = o->b * i->c + o->d * i->d;
    t.tx = o->a * i->tx + o->c * i->ty + o->tx;
    t.ty = o->b * i->tx + o->d * i->ty + o->ty;
    return t;
}

bool ub_xform_is_plain(const ub_xform *t) { return t->a == 1 && t->d == 1 && t->b == 0 && t->c == 0; }

bool ub_xform_inverse(const ub_xform *t, f32 X, f32 Y, f32 *x, f32 *y)
{
    f32 det = t->a * t->d - t->b * t->c;
    if (det > -1e-12f && det < 1e-12f) return false;
    f32 dx = X - t->tx, dy = Y - t->ty;
    *x = (t->d * dx - t->c * dy) / det;
    *y = (-t->b * dx + t->a * dy) / det;
    return true;
}

/* ----------------------------------------------------------- gradients */

void ub_gradient_color(const f32 *k, int n, f32 t, f32 *r, f32 *g, f32 *b)
{
    if (n <= 0) { *r = *g = *b = 1; return; }
    if (t <= k[0] || n == 1) { *r = k[1]; *g = k[2]; *b = k[3]; return; }
    for (int i = 1; i < n; ++i) {
        const f32 *p = k + (i - 1) * 4, *q = k + i * 4;
        if (t <= q[0]) {
            f32 span = q[0] - p[0], f = span > 1e-6f ? (t - p[0]) / span : 1;
            *r = p[1] + (q[1] - p[1]) * f;
            *g = p[2] + (q[2] - p[2]) * f;
            *b = p[3] + (q[3] - p[3]) * f;
            return;
        }
    }
    const f32 *l = k + (n - 1) * 4;
    *r = l[1]; *g = l[2]; *b = l[3];
}

f32 ub_gradient_number(const f32 *k, int n, f32 t)
{
    if (n <= 0) return 0;
    if (t <= k[0] || n == 1) return k[1];
    for (int i = 1; i < n; ++i) {
        const f32 *p = k + (i - 1) * 3, *q = k + i * 3;
        if (t <= q[0]) {
            f32 span = q[0] - p[0], f = span > 1e-6f ? (t - p[0]) / span : 1;
            return p[1] + (q[1] - p[1]) * f;
        }
    }
    return k[(n - 1) * 3 + 1];
}

/* ------------------------------------------------------------ blending */

static inline u32 div255(u32 v) { return (v + 128 + ((v + 128) >> 8)) >> 8; }

/* src: premultiplied channels 0..255 */
static inline void blend_px(u32 *d, u32 a, u32 r, u32 g, u32 b)
{
    if (!a) return;
    u32 dst = *d;
    if (a >= 255) {
        *d = 0xFF000000u | (r << 16) | (g << 8) | b;
        return;
    }
    u32 ia = 255 - a;
    u32 da = dst >> 24, dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
    da = a + div255(da * ia);
    dr = r + div255(dr * ia);
    dg = g + div255(dg * ia);
    db = b + div255(db * ia);
    *d = (UB_MIN(da, 255u) << 24) | (UB_MIN(dr, 255u) << 16) | (UB_MIN(dg, 255u) << 8) | UB_MIN(db, 255u);
}

/* ---------------------------------------------------------- the walker */

typedef struct {
    /* shape */
    f32 w, h, radius;
    f32 inner, outer;        /* ring: inset / outset; both 0 = filled */
    bool ring;
    /* source */
    const ub_image_px *img;
    const ub_axis_map *mx, *my;
    bool nearest;
    const u8 *mask;
    f32 mask_x, mask_y;
    int mask_w, mask_h;
    f32 bw, bh;              /* gradient box (mask) */
    const ub_paint *p;
} job;

/* Signed distance from (x, y) to a rounded rectangle [x0, x1] x [y0, y1]. */
static f32 rr_dist(f32 x, f32 y, f32 x0, f32 y0, f32 x1, f32 y1, f32 r)
{
    f32 hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
    if (hw < 0 || hh < 0) return 1e9f;
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    if (r < 0) r = 0;
    f32 qx = ub_absf(x - (x0 + hw)) - (hw - r), qy = ub_absf(y - (y0 + hh)) - (hh - r);
    f32 ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    f32 in = qx > qy ? qx : qy;
    return ub_sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static f32 cover(f32 d) { return d <= -0.5f ? 1 : d >= 0.5f ? 0 : 0.5f - d; }

/* Shape coverage at local (u, v); `px` = local units per canvas pixel. */
static f32 shape_cov(const job *j, f32 u, f32 v, f32 px)
{
    f32 inv = 1.0f / px;
    f32 c = cover(rr_dist(u, v, -j->outer, -j->outer, j->w + j->outer, j->h + j->outer,
                          j->radius > 0 ? j->radius + j->outer : 0) * inv);
    if (j->ring && c > 0) {
        f32 ri = j->radius - j->inner;
        f32 ci = cover(rr_dist(u, v, j->inner, j->inner, j->w - j->inner, j->h - j->inner, ri > 0 ? ri : 0) * inv);
        c -= ci;
        if (c < 0) c = 0;
    }
    return c;
}

static f32 map_axis(const ub_axis_map *m, f32 d)
{
    if (m->tile > 0) {
        f32 t = d / m->tile;
        t -= (f32)ub_floorf(t);
        return m->s0[0] + (m->s1[0] - m->s0[0]) * t;
    }
    for (int i = 0; i < m->n; ++i)
        if (d <= m->d1[i] || i == m->n - 1) {
            f32 span = m->d1[i] - m->d0[i];
            f32 f = span > 1e-6f ? (d - m->d0[i]) / span : 0;
            return m->s0[i] + (m->s1[i] - m->s0[i]) * f;
        }
    return 0;
}

static u32 sample(const ub_image_px *img, f32 sx, f32 sy, bool nearest)
{
    if (nearest) {
        int x = ub_floorf(sx), y = ub_floorf(sy);
        x = UB_CLAMP(x, 0, img->w - 1);
        y = UB_CLAMP(y, 0, img->h - 1);
        return img->px[(size_t)y * img->w + x];
    }
    sx -= 0.5f;
    sy -= 0.5f;
    int x0 = ub_floorf(sx), y0 = ub_floorf(sy);
    f32 fx = sx - (f32)x0, fy = sy - (f32)y0;
    int x1 = UB_CLAMP(x0 + 1, 0, img->w - 1), y1 = UB_CLAMP(y0 + 1, 0, img->h - 1);
    x0 = UB_CLAMP(x0, 0, img->w - 1);
    y0 = UB_CLAMP(y0, 0, img->h - 1);
    u32 p00 = img->px[(size_t)y0 * img->w + x0], p10 = img->px[(size_t)y0 * img->w + x1];
    u32 p01 = img->px[(size_t)y1 * img->w + x0], p11 = img->px[(size_t)y1 * img->w + x1];
    u32 wx = (u32)(fx * 256), wy = (u32)(fy * 256), out = 0;
    for (int s = 0; s < 32; s += 8) {
        u32 a = (p00 >> s) & 255, b = (p10 >> s) & 255, c = (p01 >> s) & 255, d = (p11 >> s) & 255;
        u32 top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + d * wx;
        u32 v = (top * (256 - wy) + bot * wy) >> 16;
        out |= (v > 255 ? 255 : v) << s;
    }
    return out;
}

static void walk(ub_canvas *c, const ub_xform *t, const job *j)
{
    const ub_paint *p = j->p;
    if (p->alpha <= 0.0f) return;
    /* the box (grown by the ring's outset and a pixel for edges) on the canvas */
    f32 g = j->outer + 1;
    f32 cx[4] = { -g, j->w + g, j->w + g, -g }, cy[4] = { -g, -g, j->h + g, j->h + g };
    if (j->mask) {
        cx[0] = cx[3] = j->mask_x - 1;
        cx[1] = cx[2] = j->mask_x + (f32)j->mask_w + 1;
        cy[0] = cy[1] = j->mask_y - 1;
        cy[2] = cy[3] = j->mask_y + (f32)j->mask_h + 1;
    }
    f32 X0 = 1e9f, Y0 = 1e9f, X1 = -1e9f, Y1 = -1e9f;
    for (int k = 0; k < 4; ++k) {
        f32 X = t->a * cx[k] + t->c * cy[k] + t->tx, Y = t->b * cx[k] + t->d * cy[k] + t->ty;
        X0 = X < X0 ? X : X0;
        X1 = X > X1 ? X : X1;
        Y0 = Y < Y0 ? Y : Y0;
        Y1 = Y > Y1 ? Y : Y1;
    }
    if (!(X1 >= X0) || !(Y1 >= Y0)) return; /* NaN */
    int x0 = X0 < (f32)c->cx0 ? c->cx0 : ub_floorf(X0), x1 = X1 > (f32)c->cx1 ? c->cx1 : ub_ceilf(X1);
    int y0 = Y0 < (f32)c->cy0 ? c->cy0 : ub_floorf(Y0), y1 = Y1 > (f32)c->cy1 ? c->cy1 : ub_ceilf(Y1);
    if (x0 >= x1 || y0 >= y1) return;
    f32 det = t->a * t->d - t->b * t->c;
    if (det > -1e-9f && det < 1e-9f) return;
    f32 ia = t->d / det, ib = -t->b / det, ic = -t->c / det, id = t->a / det; /* inverse linear part */
    f32 pxs = ub_sqrtf(ub_absf(det)); /* canvas pixels per local unit */
    f32 unit = pxs > 0 ? 1.0f / pxs : 1;
    bool plain = t->b == 0 && t->c == 0 && t->a == 1 && t->d == 1;
    bool grad = p->gcol_n > 0 || p->gtr_n > 0;
    u32 pr = (p->color >> 16) & 255, pg = (p->color >> 8) & 255, pb = p->color & 255;
    f32 alpha = p->alpha > 1 ? 1 : p->alpha;
    f32 gw = j->mask ? j->bw : j->w, gh = j->mask ? j->bh : j->h;

    /* fast path: unrotated, square-cornered, solid fill */
    if (plain && !j->img && !j->mask && !grad && j->radius <= 0 && !j->ring && j->outer == 0) {
        f32 bx0 = t->tx, bx1 = t->tx + j->w, by0 = t->ty, by1 = t->ty + j->h;
        for (int y = y0; y < y1; ++y) {
            f32 cyv = UB_MIN((f32)(y + 1), by1) - UB_MAX((f32)y, by0);
            if (cyv <= 0) continue;
            u32 *row = c->px + (size_t)y * c->pitch;
            for (int x = x0; x < x1; ++x) {
                f32 cxv = UB_MIN((f32)(x + 1), bx1) - UB_MAX((f32)x, bx0);
                if (cxv <= 0) continue;
                f32 cov = cxv * (cyv > 1 ? 1 : cyv) * alpha;
                u32 a = (u32)(cov * 255.0f + 0.5f);
                blend_px(&row[x], a, div255(pr * a), div255(pg * a), div255(pb * a));
            }
        }
        return;
    }

    for (int y = y0; y < y1; ++y) {
        u32 *row = c->px + (size_t)y * c->pitch;
        f32 dX = (f32)x0 + 0.5f - t->tx, dY = (f32)y + 0.5f - t->ty;
        f32 u = ia * dX + ic * dY, v = ib * dX + id * dY;
        for (int x = x0; x < x1; ++x, u += ia, v += ib) {
            f32 cov;
            if (j->mask) {
                /* masks: sampled at the pixel (nearest when unrotated) */
                f32 mu = u - j->mask_x, mv = v - j->mask_y;
                if (mu < 0 || mv < 0 || mu >= (f32)j->mask_w || mv >= (f32)j->mask_h) continue;
                int mxi = (int)mu, myi = (int)mv;
                cov = j->mask[(size_t)myi * j->mask_w + mxi] * (1.0f / 255.0f);
            } else if (plain && j->radius <= 0 && !j->ring && j->outer == 0) {
                if (u < -0.5f || v < -0.5f || u > j->w + 0.5f || v > j->h + 0.5f) continue;
                f32 cu = UB_MIN(u + 0.5f, j->w) - UB_MAX(u - 0.5f, 0.0f);
                f32 cv = UB_MIN(v + 0.5f, j->h) - UB_MAX(v - 0.5f, 0.0f);
                cov = (cu > 1 ? 1 : cu) * (cv > 1 ? 1 : cv);
            } else {
                cov = shape_cov(j, u, v, unit);
            }
            if (cov <= 0) continue;
            f32 r = (f32)pr, gg = (f32)pg, b = (f32)pb, a = cov * alpha;
            if (grad) {
                f32 nu = gw > 0 ? u / gw : 0, nv = gh > 0 ? v / gh : 0;
                f32 tt = (nu - 0.5f - p->gox) * p->gdx + (nv - 0.5f - p->goy) * p->gdy + 0.5f;
                tt = ub_clampf(tt, 0, 1);
                if (p->gcol_n > 0) {
                    f32 gr, ggr, gb;
                    ub_gradient_color(p->gcol, p->gcol_n, tt, &gr, &ggr, &gb);
                    r *= gr;
                    gg *= ggr;
                    b *= gb;
                }
                if (p->gtr_n > 0) a *= 1.0f - ub_clampf(ub_gradient_number(p->gtr, p->gtr_n, tt), 0, 1);
            }
            if (j->img) {
                f32 sx = map_axis(j->mx, u), sy = map_axis(j->my, v);
                u32 s = sample(j->img, sx, sy, j->nearest);
                f32 sa = (f32)(s >> 24) * (1.0f / 255.0f);
                if (sa <= 0) continue;
                /* the image is premultiplied: un-premultiply, tint, re-premultiply */
                f32 inv = 1.0f / sa;
                r = r * (f32)((s >> 16) & 255) * inv * (1.0f / 255.0f);
                gg = gg * (f32)((s >> 8) & 255) * inv * (1.0f / 255.0f);
                b = b * (f32)(s & 255) * inv * (1.0f / 255.0f);
                a *= sa;
            }
            if (a <= 0) continue;
            u32 ai = (u32)(a * 255.0f + 0.5f);
            if (ai > 255) ai = 255;
            u32 ri = (u32)(r * a + 0.5f), gi = (u32)(gg * a + 0.5f), bi = (u32)(b * a + 0.5f);
            blend_px(&row[x], ai, UB_MIN(ri, ai), UB_MIN(gi, ai), UB_MIN(bi, ai));
        }
    }
}

void ub_canvas_rect(ub_canvas *c, const ub_xform *t, f32 w, f32 h, f32 radius, const ub_paint *p)
{
    if (!(w > 0) || !(h > 0)) return;
    job j;
    memset(&j, 0, sizeof j);
    j.w = w;
    j.h = h;
    j.radius = radius;
    j.p = p;
    walk(c, t, &j);
}

void ub_canvas_ring(ub_canvas *c, const ub_xform *t, f32 w, f32 h, f32 radius, f32 inner, f32 outer,
                    const ub_paint *p)
{
    if (!(w >= 0) || !(h >= 0) || !(inner + outer > 0)) return;
    job j;
    memset(&j, 0, sizeof j);
    j.w = w;
    j.h = h;
    j.radius = radius;
    j.inner = inner > 0 ? inner : 0;
    j.outer = outer > 0 ? outer : 0;
    j.ring = true;
    j.p = p;
    walk(c, t, &j);
}

void ub_canvas_image(ub_canvas *c, const ub_xform *t, f32 w, f32 h, f32 radius, const ub_image_px *img,
                     const ub_axis_map *mx, const ub_axis_map *my, bool nearest, const ub_paint *p)
{
    if (!(w > 0) || !(h > 0) || !img || !img->px || img->w <= 0 || img->h <= 0) return;
    job j;
    memset(&j, 0, sizeof j);
    j.w = w;
    j.h = h;
    j.radius = radius;
    j.img = img;
    j.mx = mx;
    j.my = my;
    j.nearest = nearest;
    j.p = p;
    walk(c, t, &j);
}

void ub_canvas_mask(ub_canvas *c, const ub_xform *t, f32 x, f32 y, const u8 *mask, int mw, int mh, f32 bw, f32 bh,
                    const ub_paint *p)
{
    if (!mask || mw <= 0 || mh <= 0) return;
    job j;
    memset(&j, 0, sizeof j);
    j.mask = mask;
    j.mask_x = x;
    j.mask_y = y;
    j.mask_w = mw;
    j.mask_h = mh;
    j.bw = bw;
    j.bh = bh;
    j.p = p;
    walk(c, t, &j);
}

/* ----------------------------------------------------------- axis maps */

void ub_axis_stretch(ub_axis_map *m, f32 box, f32 s0, f32 s1)
{
    memset(m, 0, sizeof *m);
    m->n = 1;
    m->d0[0] = 0;
    m->d1[0] = box;
    m->s0[0] = s0;
    m->s1[0] = s1;
}

void ub_axis_slice(ub_axis_map *m, f32 box, f32 s0, f32 s1, f32 lo, f32 hi, f32 scale)
{
    memset(m, 0, sizeof *m);
    f32 a = (lo - s0) * scale, b = (s1 - hi) * scale;
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (a + b > box && a + b > 0) { /* too small for the corners: shrink them together */
        f32 k = box / (a + b);
        a *= k;
        b *= k;
    }
    m->n = 3;
    m->d0[0] = 0;       m->d1[0] = a;        m->s0[0] = s0; m->s1[0] = lo;
    m->d0[1] = a;       m->d1[1] = box - b;  m->s0[1] = lo; m->s1[1] = hi;
    m->d0[2] = box - b; m->d1[2] = box;      m->s0[2] = hi; m->s1[2] = s1;
}

void ub_axis_tile(ub_axis_map *m, f32 box, f32 s0, f32 s1, f32 tile)
{
    ub_axis_stretch(m, box, s0, s1);
    m->tile = tile > 0.5f ? tile : 0.5f;
}
