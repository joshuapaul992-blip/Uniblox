#include "render/ub_render_internal.h"
#include "render/ub_palette.h"
#include "crt/ub_crt.h"

#define NEAR_Z   0.1f
#define ZSCALE   268435456.0f   /* 2^28: 1/z (at most 1/NEAR_Z = 10) -> z-buffer units */
#define SUBDIV   16             /* pixels between perspective divides */
#define MAX_CLIP (UB_FACE_MAX_VERTS + 8) /* face verts + one per clip plane */

/* View-space vertex with texture coordinates in texels. */
typedef struct {
    f32 x, y, z, u, v;
} cvert;

/* Projected vertex: screen position plus attributes that are linear in
 * screen space (1/z, u/z, v/z). */
typedef struct {
    f32 sx, sy, iz, uz, vz;
} pvert;

/* Screen-space gradients of the attributes over the current polygon. */
typedef struct {
    f32 diz_dx, diz_dy, duz_dx, duz_dy, dvz_dx, dvz_dy;
    f32 x0, y0, iz0, uz0, vz0;
} grads;

/* How the current face turns texels into pixels. */
typedef struct {
    const u8 *tex;         /* NULL: flat colour */
    u32 lut32[UB_TEX_LEVELS];
    u8 lut8[4][UB_TEX_LEVELS];
    u32 flat32;
    u8 flat8[4];
} shading;

typedef void (*span_fn)(int y, int x0, int x1, f32 iz, f32 uz, f32 vz);

/* Colour lookup tables are cached: in a mostly static scene the same
 * (colour, material, light) combinations recur every frame, and building
 * an 8-bit table costs 128 palette searches. Direct-mapped, keyed exactly. */
#define LUT_CACHE 512
typedef struct {
    u32 color;
    i16 light_q;
    u8 material, format;
    bool used;
    shading s;
} lut_entry;
static lut_entry g_lut_cache[LUT_CACHE];

static struct {
    ub_framebuffer *fb;
    u32 *zbuf;
    int zw, zh;
    vec3 cam_pos, right, up, fwd;
    f32 focal, cx, cy, far_dist;
    f32 clip[5][4];        /* polygon clip planes in view space: a,b,c,d */
    f32 cull[5][4];        /* same planes normalised, for sphere tests */
    ub_render_env env;
    ub_render_stats st;
    u64 t0;
    grads g;
    const shading *s;      /* current face */
    bool ready;
} R;

/* -------------------------------------------------------------- setup */

bool sw_init(void)
{
    memset(g_lut_cache, 0, sizeof g_lut_cache);
    ub_palette_init();
    ub_textures_init();
    ub_meshes_init();
    R.ready = true;
    return true;
}

void sw_shutdown(void)
{
    plat_free(R.zbuf);
    R.zbuf = NULL;
    R.zw = R.zh = 0;
    R.ready = false;
}


static u32 lerp_rgb(u32 a, u32 b, f32 t)
{
    i32 k = (i32)(ub_clampf(t, 0.0f, 1.0f) * 256.0f);
    u32 r = (u32)((i32)((a >> 16) & 255) + ((((i32)((b >> 16) & 255) - (i32)((a >> 16) & 255)) * k) >> 8));
    u32 g = (u32)((i32)((a >> 8) & 255) + ((((i32)((b >> 8) & 255) - (i32)((a >> 8) & 255)) * k) >> 8));
    u32 bl = (u32)((i32)(a & 255) + ((((i32)(b & 255) - (i32)(a & 255)) * k) >> 8));
    return (r << 16) | (g << 8) | bl;
}

static void draw_sky(void)
{
    ub_framebuffer *fb = R.fb;
    for (int y = 0; y < fb->height; ++y) {
        /* elevation of the ray through the centre of this row */
        f32 dy = (R.cy - ((f32)y + 0.5f)) / R.focal;
        f32 wy = R.up.y * dy + R.fwd.y;
        f32 elev = wy / ub_sqrtf(dy * dy + 1.0f);
        u32 c = elev >= 0 ? lerp_rgb(R.env.sky_horizon, R.env.sky_top, elev * 1.6f)
                          : lerp_rgb(R.env.sky_horizon, R.env.ground_color, -elev * 6.0f);
        if (fb->format == UB_PIXEL_XRGB32) {
            u32 *row = fb->pixels + y * fb->pitch;
            for (int x = 0; x < fb->width; ++x) row[x] = c;
        } else {
            int r = (int)(c >> 16) & 255, g = (int)(c >> 8) & 255, b = (int)c & 255;
            u8 even = ub_palette_match(r, g, b, (y & 1) << 1);
            u8 odd = ub_palette_match(r, g, b, ((y & 1) << 1) | 1);
            u8 *row = fb->pixels8 + y * fb->pitch;
            for (int x = 0; x < fb->width; x += 2) {
                row[x] = even;
                if (x + 1 < fb->width) row[x + 1] = odd;
            }
        }
    }
}

static void set_plane(int i, f32 a, f32 b, f32 c, f32 d)
{
    R.clip[i][0] = a;
    R.clip[i][1] = b;
    R.clip[i][2] = c;
    R.clip[i][3] = d;
    f32 len = ub_sqrtf(a * a + b * b + c * c);
    for (int k = 0; k < 4; ++k) R.cull[i][k] = R.clip[i][k] / len;
}

void sw_begin(ub_framebuffer *fb, const ub_camera *cam, const ub_render_env *env)
{
    R.t0 = plat_time_us();
    memset(&R.st, 0, sizeof R.st);
    R.fb = fb;
    R.env = *env;

    if (!R.zbuf || R.zw != fb->width || R.zh != fb->height) {
        plat_free(R.zbuf);
        R.zbuf = (u32 *)plat_alloc((size_t)fb->width * fb->height * sizeof(u32));
        R.zw = R.zbuf ? fb->width : 0;
        R.zh = R.zbuf ? fb->height : 0;
    }
    if (R.zbuf) memset(R.zbuf, 0, (size_t)R.zw * R.zh * sizeof(u32));

    /* camera basis */
    f32 sy = ub_sinf(cam->yaw), cy = ub_cosf(cam->yaw);
    f32 sp = ub_sinf(cam->pitch), cp = ub_cosf(cam->pitch);
    R.cam_pos = cam->pos;
    R.fwd = v3(sy * cp, sp, cy * cp);
    R.right = v3(cy, 0, -sy);
    R.up = v3_cross(R.fwd, R.right);
    R.cx = (f32)fb->width * 0.5f;
    R.cy = (f32)fb->height * 0.5f;
    f32 half = ub_clampf(cam->fov_y, 0.2f, 2.8f) * 0.5f;
    R.focal = R.cy * ub_cosf(half) / ub_sinf(half);
    R.far_dist = cam->far_dist > 1.0f ? cam->far_dist : 1000.0f;

    /* inside when a*x + b*y + c*z + d >= 0 (view space) */
    f32 w = (f32)fb->width, h = (f32)fb->height;
    set_plane(0, 0, 0, 1, -NEAR_Z);              /* near */
    set_plane(1, R.focal, 0, R.cx, 0);           /* left:   sx >= 0 */
    set_plane(2, -R.focal, 0, w - R.cx, 0);      /* right:  sx <= w */
    set_plane(3, 0, -R.focal, R.cy, 0);          /* top:    sy >= 0 */
    set_plane(4, 0, R.focal, h - R.cy, 0);       /* bottom: sy <= h */

    draw_sky();
}

void sw_end(ub_render_stats *stats)
{
    R.st.render_us = (u32)(plat_time_us() - R.t0);
    if (stats) *stats = R.st;
}

/* -------------------------------------------------------------- spans */

static i32 clamp_fixed(f32 v)
{
    if (v > 2.0e9f) return 2000000000;
    if (v < -2.0e9f) return -2000000000;
    return (i32)v;
}

static void span_flat32(int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    (void)uz; (void)vz;
    u32 *row = R.fb->pixels + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    u32 c = R.s->flat32;
    u32 zf = (u32)(i64)(iz * ZSCALE);
    u32 dz = (u32)clamp_fixed(R.g.diz_dx * ZSCALE);
    for (int x = x0; x < x1; ++x) {
        if (zf > zrow[x]) {
            zrow[x] = zf;
            row[x] = c;
        }
        zf += dz;
    }
    R.st.pixels_tested += (u32)(x1 - x0);
}

static void span_flat8(int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    (void)uz; (void)vz;
    u8 *row = R.fb->pixels8 + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    u8 even = R.s->flat8[(y & 1) << 1], odd = R.s->flat8[((y & 1) << 1) | 1];
    u32 zf = (u32)(i64)(iz * ZSCALE);
    u32 dz = (u32)clamp_fixed(R.g.diz_dx * ZSCALE);
    for (int x = x0; x < x1; ++x) {
        if (zf > zrow[x]) {
            zrow[x] = zf;
            row[x] = (x & 1) ? odd : even;
        }
        zf += dz;
    }
    R.st.pixels_tested += (u32)(x1 - x0);
}

/* Shared setup for one perspective-correct segment of a textured span. */
typedef struct {
    i32 u, v, du, dv;
    u32 z, dz;
} seg;

static void segment(seg *s, int len, f32 *iz, f32 *uz, f32 *vz, f32 *u, f32 *v)
{
    /* Keep values sane for the integer conversions below even when a
     * nearly edge-on face sends u, v towards infinity. */
    *u = ub_clampf(*u, -1.0e7f, 1.0e7f);
    *v = ub_clampf(*v, -1.0e7f, 1.0e7f);
    f32 iz2 = *iz + R.g.diz_dx * (f32)len;
    f32 uz2 = *uz + R.g.duz_dx * (f32)len;
    f32 vz2 = *vz + R.g.dvz_dx * (f32)len;
    if (iz2 < 1e-6f) iz2 = 1e-6f;
    f32 z2 = 1.0f / iz2;
    f32 u2 = ub_clampf(uz2 * z2, -1.0e7f, 1.0e7f), v2 = ub_clampf(vz2 * z2, -1.0e7f, 1.0e7f);
    /* Keep fixed-point texture coordinates small: shift both ends of the
     * segment by the same whole number of texture repeats. */
    f32 bu = (f32)(ub_floorf(*u * (1.0f / UB_TEX_SIZE)) * UB_TEX_SIZE);
    f32 bv = (f32)(ub_floorf(*v * (1.0f / UB_TEX_SIZE)) * UB_TEX_SIZE);
    f32 su = ub_clampf(u2 - *u, -30000.0f, 30000.0f);
    f32 sv = ub_clampf(v2 - *v, -30000.0f, 30000.0f);
    f32 inv = 65536.0f / (f32)len;
    s->u = (i32)((*u - bu) * 65536.0f);
    s->v = (i32)((*v - bv) * 65536.0f);
    s->du = (i32)(su * inv);
    s->dv = (i32)(sv * inv);
    s->z = (u32)(i64)(*iz * ZSCALE);
    s->dz = (u32)clamp_fixed(R.g.diz_dx * ZSCALE);
    *iz = iz2;
    *uz = uz2;
    *vz = vz2;
    *u = u2;
    *v = v2;
}

#define TEXEL(tex, s) (tex)[(((s).v >> 10) & (UB_TEX_MASK << UB_TEX_SHIFT)) | (((s).u >> 16) & UB_TEX_MASK)]

static void span_tex32(int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    u32 *row = R.fb->pixels + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    const u8 *tex = R.s->tex;
    const u32 *lut = R.s->lut32;
    f32 z = 1.0f / iz, u = uz * z, v = vz * z;
    R.st.pixels_tested += (u32)(x1 - x0);
    while (x0 < x1) {
        int len = x1 - x0 > SUBDIV ? SUBDIV : x1 - x0;
        seg s;
        segment(&s, len, &iz, &uz, &vz, &u, &v);
        for (int x = x0; x < x0 + len; ++x) {
            if (s.z > zrow[x]) {
                zrow[x] = s.z;
                row[x] = lut[TEXEL(tex, s)];
            }
            s.z += s.dz;
            s.u += s.du;
            s.v += s.dv;
        }
        x0 += len;
    }
}

static void span_tex8(int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    u8 *row = R.fb->pixels8 + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    const u8 *tex = R.s->tex;
    const u8 *lut_even = R.s->lut8[(y & 1) << 1], *lut_odd = R.s->lut8[((y & 1) << 1) | 1];
    f32 z = 1.0f / iz, u = uz * z, v = vz * z;
    R.st.pixels_tested += (u32)(x1 - x0);
    while (x0 < x1) {
        int len = x1 - x0 > SUBDIV ? SUBDIV : x1 - x0;
        seg s;
        segment(&s, len, &iz, &uz, &vz, &u, &v);
        for (int x = x0; x < x0 + len; ++x) {
            if (s.z > zrow[x]) {
                zrow[x] = s.z;
                row[x] = ((x & 1) ? lut_odd : lut_even)[TEXEL(tex, s)];
            }
            s.z += s.dz;
            s.u += s.du;
            s.v += s.dv;
        }
        x0 += len;
    }
}

/* ------------------------------------------------------- rasterizer */

/* Scan-converts a convex polygon. Pixel centres are at +0.5; a pixel is
 * drawn when its centre lies inside (top-left rule), so polygons sharing
 * an edge neither overlap nor leave gaps. */
static void raster_poly(const pvert *p, int n, span_fn span)
{
    /* Attribute gradients from the largest triangle fan member, for
     * numerical stability. The polygon is planar, so any works in theory. */
    int best = 1;
    f32 best_d = 0;
    for (int i = 1; i + 1 < n; ++i) {
        f32 d = (p[i].sx - p[0].sx) * (p[i + 1].sy - p[0].sy) -
                (p[i + 1].sx - p[0].sx) * (p[i].sy - p[0].sy);
        if (ub_absf(d) > ub_absf(best_d)) {
            best_d = d;
            best = i;
        }
    }
    if (ub_absf(best_d) < 1e-3f) return; /* degenerate: edge-on or sub-pixel */
    {
        const pvert *a = &p[0], *b = &p[best], *c = &p[best + 1];
        f32 x1 = b->sx - a->sx, y1 = b->sy - a->sy, x2 = c->sx - a->sx, y2 = c->sy - a->sy;
        f32 inv = 1.0f / best_d;
        R.g.diz_dx = ((b->iz - a->iz) * y2 - (c->iz - a->iz) * y1) * inv;
        R.g.diz_dy = ((c->iz - a->iz) * x1 - (b->iz - a->iz) * x2) * inv;
        R.g.duz_dx = ((b->uz - a->uz) * y2 - (c->uz - a->uz) * y1) * inv;
        R.g.duz_dy = ((c->uz - a->uz) * x1 - (b->uz - a->uz) * x2) * inv;
        R.g.dvz_dx = ((b->vz - a->vz) * y2 - (c->vz - a->vz) * y1) * inv;
        R.g.dvz_dy = ((c->vz - a->vz) * x1 - (b->vz - a->vz) * x2) * inv;
        R.g.x0 = a->sx;
        R.g.y0 = a->sy;
        R.g.iz0 = a->iz;
        R.g.uz0 = a->uz;
        R.g.vz0 = a->vz;
    }

    int top = 0;
    f32 ymin = p[0].sy, ymax = p[0].sy;
    for (int i = 1; i < n; ++i) {
        if (p[i].sy < ymin) { ymin = p[i].sy; top = i; }
        if (p[i].sy > ymax) ymax = p[i].sy;
    }
    int y = ub_ceilf(ymin - 0.5f), yend = ub_ceilf(ymax - 0.5f);
    if (y < 0) y = 0;
    if (yend > R.fb->height) yend = R.fb->height;
    if (y >= yend) return;

    /* Walk both chains down from the top vertex; per scanline the left
     * and right ends are just the min and max of the two. */
    int ai = top, an = (top + 1) % n;          /* chain A: increasing index */
    int bi = top, bn = (top + n - 1) % n;      /* chain B: decreasing index */
    f32 aslope = 0, bslope = 0;
    bool a_set = false, b_set = false;
    const int w = R.fb->width;

    for (; y < yend; ++y) {
        f32 fy = (f32)y + 0.5f;
        for (int guard = n; (p[an].sy <= fy || !a_set) && guard > 0; --guard) {
            if (p[an].sy <= fy) { ai = an; an = (an + 1) % n; }
            f32 dy = p[an].sy - p[ai].sy;
            aslope = dy > 1e-6f ? (p[an].sx - p[ai].sx) / dy : 0.0f;
            a_set = true;
        }
        for (int guard = n; (p[bn].sy <= fy || !b_set) && guard > 0; --guard) {
            if (p[bn].sy <= fy) { bi = bn; bn = (bn + n - 1) % n; }
            f32 dy = p[bn].sy - p[bi].sy;
            bslope = dy > 1e-6f ? (p[bn].sx - p[bi].sx) / dy : 0.0f;
            b_set = true;
        }
        f32 xa = p[ai].sx + (fy - p[ai].sy) * aslope;
        f32 xb = p[bi].sx + (fy - p[bi].sy) * bslope;
        f32 xl = xa < xb ? xa : xb, xr = xa < xb ? xb : xa;
        int x0 = ub_ceilf(ub_clampf(xl, -1.0f, (f32)w + 1.0f) - 0.5f);
        int x1 = ub_ceilf(ub_clampf(xr, -1.0f, (f32)w + 1.0f) - 0.5f);
        if (x0 < 0) x0 = 0;
        if (x1 > w) x1 = w;
        if (x0 >= x1) continue;
        f32 dx = (f32)x0 + 0.5f - R.g.x0, dyy = fy - R.g.y0;
        f32 iz = R.g.iz0 + R.g.diz_dx * dx + R.g.diz_dy * dyy;
        f32 uz = R.g.uz0 + R.g.duz_dx * dx + R.g.duz_dy * dyy;
        f32 vz = R.g.vz0 + R.g.dvz_dx * dx + R.g.dvz_dy * dyy;
        if (iz < 1e-6f) iz = 1e-6f;
        span(y, x0, x1, iz, uz, vz);
    }
}

/* ----------------------------------------------------------- clipping */

static int clip_poly(const cvert *in, int n, cvert *out, const f32 *pl)
{
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const cvert *a = &in[i], *b = &in[(i + 1) % n];
        f32 da = pl[0] * a->x + pl[1] * a->y + pl[2] * a->z + pl[3];
        f32 db = pl[0] * b->x + pl[1] * b->y + pl[2] * b->z + pl[3];
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            f32 t = da / (da - db);
            cvert *o = &out[m++];
            o->x = a->x + (b->x - a->x) * t;
            o->y = a->y + (b->y - a->y) * t;
            o->z = a->z + (b->z - a->z) * t;
            o->u = a->u + (b->u - a->u) * t;
            o->v = a->v + (b->v - a->v) * t;
        }
    }
    return m;
}

/* --------------------------------------------------------------- parts */

static void fill_shading(shading *sh, u32 color, ub_material mat, i32 lq, bool idx8)
{
    i32 r = (i32)((color >> 16) & 255), g = (i32)((color >> 8) & 255), b = (i32)(color & 255);
    sh->tex = ub_texture(mat);
    if (!sh->tex) {
        i32 rr = (r * lq) >> 8, gg = (g * lq) >> 8, bb = (b * lq) >> 8;
        if (idx8) {
            for (int d = 0; d < 4; ++d) sh->flat8[d] = ub_palette_match(rr, gg, bb, d);
        } else {
            sh->flat32 = ((u32)UB_MIN(rr, 255) << 16) | ((u32)UB_MIN(gg, 255) << 8) | (u32)UB_MIN(bb, 255);
        }
        return;
    }
    for (int t = 0; t < UB_TEX_LEVELS; ++t) {
        i32 k = (lq * ub_texel_gain(t)) >> 8;
        i32 rr = (r * k) >> 8, gg = (g * k) >> 8, bb = (b * k) >> 8;
        if (idx8) {
            for (int d = 0; d < 4; ++d) sh->lut8[d][t] = ub_palette_match(rr, gg, bb, d);
        } else {
            sh->lut32[t] = ((u32)UB_MIN(rr, 255) << 16) | ((u32)UB_MIN(gg, 255) << 8) | (u32)UB_MIN(bb, 255);
        }
    }
}

/* light is 0..1.3; quantised to 1/64 steps (finer is invisible). */
static void select_shading(u32 color, ub_material mat, f32 light)
{
    i32 lq = (i32)(ub_clampf(light, 0.0f, 1.3f) * 64.0f) * 4;
    u8 fmt = (u8)R.fb->format;
    u32 h = (color * 2654435761u) ^ ((u32)lq * 40503u) ^ ((u32)mat * 97u) ^ ((u32)fmt * 7919u);
    lut_entry *e = &g_lut_cache[(h ^ (h >> 15)) & (LUT_CACHE - 1)];
    if (!e->used || e->color != color || e->light_q != lq || e->material != mat || e->format != fmt) {
        fill_shading(&e->s, color, mat, lq, fmt == UB_PIXEL_INDEX8);
        e->color = color;
        e->light_q = (i16)lq;
        e->material = (u8)mat;
        e->format = fmt;
        e->used = true;
        R.st.luts_built++;
    }
    R.s = &e->s;
}

static vec3 to_view(vec3 w)
{
    return v3(v3_dot(w, R.right), v3_dot(w, R.up), v3_dot(w, R.fwd));
}

void sw_part(const ub_part_draw *p)
{
    if (!R.ready || !R.fb || !R.zbuf) return;
    R.st.parts++;

    vec3 size = v3(UB_MAX(p->size.x, 0.01f), UB_MAX(p->size.y, 0.01f), UB_MAX(p->size.z, 0.01f));
    vec3 centre = to_view(v3_sub(p->pos, R.cam_pos));
    f32 radius = 0.5f * v3_length(size);

    /* bounding sphere against the far distance and the five planes */
    if (centre.z - radius > R.far_dist) { R.st.parts_culled++; return; }
    for (int i = 0; i < 5; ++i) {
        const f32 *c = R.cull[i];
        if (c[0] * centre.x + c[1] * centre.y + c[2] * centre.z + c[3] < -radius) {
            R.st.parts_culled++;
            return;
        }
    }

    const ub_mesh *mesh = ub_mesh_get((ub_shape)p->shape, UB_DETAIL_LOW);
    ub_material mat = (ub_material)(p->material < UB_MAT_COUNT ? p->material : UB_MAT_PLASTIC);
    bool textured = ub_texture(mat) != NULL;
    f32 texels_per_unit = (f32)UB_TEX_SIZE / ub_materials[mat].world_size;

    /* part axes scaled by size, in view space */
    vec3 ax = to_view(v3_scale(p->rot.x, size.x));
    vec3 ay = to_view(v3_scale(p->rot.y, size.y));
    vec3 az = to_view(v3_scale(p->rot.z, size.z));

    vec3 view[UB_MESH_MAX_VERTS];
    for (int i = 0; i < mesh->vert_count; ++i) {
        vec3 l = mesh->verts[i];
        view[i] = v3(centre.x + ax.x * l.x + ay.x * l.y + az.x * l.z,
                     centre.y + ax.y * l.x + ay.y * l.y + az.y * l.z,
                     centre.z + ax.z * l.x + ay.z * l.y + az.z * l.z);
    }

    span_fn span = R.fb->format == UB_PIXEL_INDEX8 ? (textured ? span_tex8 : span_flat8)
                                                    : (textured ? span_tex32 : span_flat32);

    for (int fi = 0; fi < mesh->face_count; ++fi) {
        const ub_face *f = &mesh->faces[fi];
        R.st.faces++;

        /* Normal after non-uniform scaling: divide by size, renormalise. */
        vec3 ns = v3_normalize(v3(f->normal.x / size.x, f->normal.y / size.y, f->normal.z / size.z));
        vec3 nw = mat3_apply(&p->rot, ns);
        vec3 nv = to_view(nw);
        if (v3_dot(nv, view[f->v[0]]) >= 0) { R.st.faces_backfacing++; continue; }

        f32 sun = v3_dot(nw, R.env.sun_dir);
        f32 light = R.env.ambient * (0.8f + 0.2f * nw.y) + R.env.sun_strength * (sun > 0 ? sun : 0);
        select_shading(p->color, mat, light);

        /* Planar texture mapping in the part's scaled space: u along the
         * face's first edge, v perpendicular to it within the face. */
        cvert a[MAX_CLIP], b[MAX_CLIP];
        vec3 s0 = v3_mul(mesh->verts[f->v[0]], size);
        vec3 tu = v3(0, 0, 0), tv = v3(0, 0, 0);
        if (textured) {
            tu = v3_normalize(v3_sub(v3_mul(mesh->verts[f->v[1]], size), s0));
            tv = v3_cross(ns, tu);
        }
        for (int i = 0; i < f->count; ++i) {
            vec3 v = view[f->v[i]];
            a[i].x = v.x;
            a[i].y = v.y;
            a[i].z = v.z;
            if (textured) {
                vec3 d = v3_sub(v3_mul(mesh->verts[f->v[i]], size), s0);
                a[i].u = v3_dot(d, tu) * texels_per_unit;
                a[i].v = v3_dot(d, tv) * texels_per_unit;
            } else {
                a[i].u = a[i].v = 0;
            }
        }

        /* clip: only against planes the face actually crosses */
        int n = f->count;
        cvert *in = a, *out = b;
        for (int k = 0; k < 5 && n >= 3; ++k) {
            const f32 *pl = R.clip[k];
            int inside = 0;
            for (int i = 0; i < n; ++i)
                inside += pl[0] * in[i].x + pl[1] * in[i].y + pl[2] * in[i].z + pl[3] >= 0;
            if (inside == n) continue;
            if (inside == 0) { n = 0; break; }
            n = clip_poly(in, n, out, pl);
            cvert *t = in; in = out; out = t;
        }
        if (n < 3) continue;

        pvert pv[MAX_CLIP];
        bool sane = true;
        for (int i = 0; i < n; ++i) {
            f32 iz = 1.0f / (in[i].z > NEAR_Z * 0.5f ? in[i].z : NEAR_Z * 0.5f);
            pv[i].sx = R.cx + R.focal * in[i].x * iz;
            pv[i].sy = R.cy - R.focal * in[i].y * iz;
            pv[i].iz = iz;
            pv[i].uz = in[i].u * iz;
            pv[i].vz = in[i].v * iz;
            /* comparisons are false for NaN, so this also rejects NaN */
            if (!(pv[i].sx > -1.0e5f && pv[i].sx < 1.0e5f && pv[i].sy > -1.0e5f && pv[i].sy < 1.0e5f))
                sane = false;
        }
        if (!sane) continue;
        R.st.polys_drawn++;
        raster_poly(pv, n, span);
    }
}
