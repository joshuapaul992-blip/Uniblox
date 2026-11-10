/* Uniblox - the software renderers: they draw into a ub_framebuffer.
 *
 * The plain one draws each part as it comes. The fast one (milestone 9A,
 * for NT 3.51 and 4.0, where there is no OpenGL worth having) waits for
 * the whole scene and then:
 *   - sorts the parts nearest first, so hidden pixels fail the depth test
 *     before any texture work;
 *   - keeps a coarse depth per 16x16 tile (the farthest depth in it) and
 *     skips the polygons, and the stretches of their rows, that lie behind
 *     everything already drawn there;
 *   - steps the perspective divide further on polygons whose depth hardly
 *     changes (most of a scene at a distance);
 *   - splits the frame into rows of tiles shared out over every processor
 *     the machine has (NT 3.51 and 4.0 run on several), each drawing its
 *     own rows, sky and depth clear included.
 * Both share the spans below, so they look the same. */
#include "render/ub_render_internal.h"
#include "render/ub_palette.h"
#include "crt/ub_crt.h"

#define NEAR_Z   0.1f
#define ZSCALE   268435456.0f   /* 2^28: 1/z (at most 1/NEAR_Z = 10) -> z-buffer units */
#define SUBDIV   16             /* pixels between perspective divides */
#define MAX_CLIP (UB_FACE_MAX_VERTS + 8) /* face verts + one per clip plane */
#define TILE_SHIFT 4
#define TILE       (1 << TILE_SHIFT)  /* fast: tiles and the rows of them a thread draws */
#define MAX_TILES_X 512               /* fast: no coarse depth on frames wider than 8192 */
#define MAX_THREADS 16
#define SMAP 512               /* fast, high: the sun's shadow map, texels a side */
#define SMAP_TEXEL 0.25f       /* studs a texel: the map covers 128 studs round the camera's focus */
#define SDEPTH_SCALE 262144.0f /* shadow map depth units a stud */
#define SDEPTH_OFFSET 4096.0f
/* how far behind the map's depth a point must be to be in shadow, by its
 * face's shadow level (faces the sun lights at a slant need a little more) */
static const f32 k_shadow_bias[8] = { 0, 0.3f * SDEPTH_SCALE, 0.2f * SDEPTH_SCALE, 0.12f * SDEPTH_SCALE,
                                      0.1f * SDEPTH_SCALE, 0.1f * SDEPTH_SCALE, 0.1f * SDEPTH_SCALE, 0.1f * SDEPTH_SCALE };
#define FOG_BITS 4096          /* fog table: by the exponent and top 7 bits of a depth as a float, */
#define FOG_BASE (127 << 7)    /* from 1 (exponent 127) to 2^32 (159) */

/* View-space vertex with texture coordinates in texels. */
typedef struct {
    f32 x, y, z, u, v, l;   /* l: smooth faces' light (a ramp index) */
} cvert;

/* Projected vertex: screen position plus attributes that are linear in
 * screen space (1/z, u/z, v/z). */
typedef struct {
    f32 sx, sy, iz, uz, vz, l;
} pvert;

/* Screen-space gradients of the attributes over the current polygon. */
typedef struct {
    f32 diz_dx, diz_dy, duz_dx, duz_dy, dvz_dx, dvz_dy, dl_dx, dl_dy;
    f32 x0, y0, iz0, uz0, vz0, l0;
} grads;

/* How the current face turns texels into pixels. */
#define RAMP 64            /* smooth faces: light levels */
#define RAMP_MAX 2.0f      /* the light at the top of the ramp (above 1: highlights going white) */

typedef struct {
    const u8 *tex;         /* NULL: flat colour */
    u32 lut32[UB_TEX_LEVELS];
    u8 lut8[4][UB_TEX_LEVELS];
    u32 flat32;
    u8 flat8[4];
    const u8 *mip[UB_TEX_MIPS]; /* the texture averaged for far away (mip[0] = tex) */
    u32 ramp32[RAMP + 2];  /* smooth faces (fast renderer): the colour at each light level */
    u8 ramp8[4][RAMP + 2];
} shading;

/* What one thread needs to draw polygons: everything else is shared and
 * only read while drawing. */
typedef struct rctx {
    grads g;
    const shading *s;
    int see;               /* see-through level of the current part (0 = solid) */
    int subdiv;            /* pixels between perspective divides */
    int y0, y1;            /* the rows it may draw */
    int w;                 /* and how wide they are */
    u32 zmask, lvl;        /* fast, high: depths keep the face's shadow level in their low 3 bits */
    u32 pixels;            /* pixels tested (stats) */
    bool mips;             /* fast: textures averaged for far away */
    const u8 *occ;         /* fast: per tile column of the current row of tiles, 1 = the polygon is hidden there */
    u8 *dirty;             /* fast: per tile column, drawn into since its depth was taken */
} rctx;

typedef void (*span_fn)(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz);

/* Colour lookup tables are cached: in a mostly static scene the same
 * (colour, material, light) combinations recur every frame, and building
 * an 8-bit table costs 128 palette searches. Direct-mapped, keyed exactly.
 * The fast renderer draws after the whole scene is set up, so an entry in
 * use this frame is never replaced: a clash takes a table from a pool. */
#define LUT_CACHE 2048
typedef struct {
    u32 color;
    i16 q[3];              /* the light (or a ramp's tint) per channel, 1/256 steps */
    u8 material, format;
    bool ramp;
    bool used;
    u32 frame;
    shading s;
} lut_entry;
static lut_entry g_lut_cache[LUT_CACHE];

#define POOL_BLOCK 128
typedef struct pool_block {
    struct pool_block *next;
    shading s[POOL_BLOCK];
} pool_block;

/* fast: a part waiting to be drawn, and a polygon ready to be */
typedef struct {
    f32 key;               /* view depth of its nearest point (bounding sphere) */
    const ub_part_draw *p;
} queued;

typedef struct {
    grads g;
    const shading *s;
    span_fn span;
    u32 first;             /* its vertices in the pool */
    u32 znear;             /* depth (z-buffer units) of its nearest point */
    i16 x0, x1, y0, y1;    /* pixels it can touch */
    u8 n, see, subdiv, lvl;
} fpoly;

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
    rctx main;             /* the plain renderer's, and GUI pictures' */
    bool ready;
    u32 frame;
    /* fast */
    bool fast;
    int threads_setting;   /* 0 = one per processor */
    int threads;           /* this frame */
    queued *queue, *queue_tmp;
    int nqueue, queue_cap, queue_tmp_cap;
    fpoly *polys;
    int npolys, polys_cap;
    pvert *verts;
    int nverts, verts_cap;
    pool_block *pool;      /* tables for clashing cache slots */
    pool_block *pool_cur;
    int pool_used;
    bool overflow;         /* out of memory this frame: drawn with what there was */
    bool flushed;          /* the scene is drawn (this frame) */
    int strips, tiles_x;
    u32 t_pixels[MAX_THREADS], t_hidden[MAX_THREADS];
    int quality;           /* fast: 0 low (plain lighting), 1 medium (colour, smooth faces, fog), 2 high (and shadows) */
    f32 sky_tint[3], ground_tint[3], sun_tint[3]; /* the lights' colours, averaging 1 */
    vec3 sun_view;         /* the sun's direction in view space */
    /* fast, high: the sun's shadows */
    bool shadow_on;
    u32 *smap;             /* SMAP x SMAP depths seen from the sun (larger = nearer it), 0 = nothing */
    vec3 lx, ly, lz;       /* the sun's view: across, up, towards the sun */
    f32 lcx, lcy;          /* the middle of the map, along lx and ly */
    const ub_part_draw **casters;
    int ncasters, casters_cap;
    fpoly *spolys;
    int nspolys, spolys_cap;
    pvert *sverts;
    int nsverts, sverts_cap;
    /* fast, medium and up: distance fog */
    bool fog_on;
    u32 fog_color;
    u8 fog_lut[FOG_BITS];  /* fog amount (0..255) by a depth's float bits (fog_index) */
    u8 remap[4][8][8][256];/* 256 colours: index -> shadowed (8 levels) and fogged (8 levels), per dither cell */
    u32 remap_key;         /* what the remap tables were made for */
    bool remap_ok;
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
    plat_free(R.queue);
    plat_free(R.queue_tmp);
    plat_free(R.polys);
    plat_free(R.verts);
    R.queue = R.queue_tmp = NULL;
    R.polys = NULL;
    R.verts = NULL;
    R.queue_cap = R.queue_tmp_cap = R.polys_cap = R.verts_cap = 0;
    plat_free(R.smap);
    plat_free(R.casters);
    plat_free(R.spolys);
    plat_free(R.sverts);
    R.smap = NULL;
    R.casters = NULL;
    R.spolys = NULL;
    R.sverts = NULL;
    R.casters_cap = R.spolys_cap = R.sverts_cap = 0;
    R.remap_ok = false;
    while (R.pool) {
        pool_block *n = R.pool->next;
        plat_free(R.pool);
        R.pool = n;
    }
    R.ready = false;
}

void sw_set_fast(bool on, int threads)
{
    R.fast = on;
    R.threads_setting = threads;
}

bool sw_fast(void) { return R.fast; }

void sw_set_quality(int q) { R.quality = UB_CLAMP(q, 0, 2); }

int sw_threads(void)
{
    int n = R.threads_setting > 0 ? R.threads_setting : plat_cpu_count();
    return UB_CLAMP(n, 1, MAX_THREADS);
}

static u32 lerp_rgb(u32 a, u32 b, f32 t)
{
    i32 k = (i32)(ub_clampf(t, 0.0f, 1.0f) * 256.0f);
    u32 r = (u32)((i32)((a >> 16) & 255) + ((((i32)((b >> 16) & 255) - (i32)((a >> 16) & 255)) * k) >> 8));
    u32 g = (u32)((i32)((a >> 8) & 255) + ((((i32)((b >> 8) & 255) - (i32)((a >> 8) & 255)) * k) >> 8));
    u32 bl = (u32)((i32)(a & 255) + ((((i32)(b & 255) - (i32)(a & 255)) * k) >> 8));
    return (r << 16) | (g << 8) | bl;
}

/* The sky's colour on a row (and its two dithered indexes in 8-bit). */
static u32 sky_row(int y, u8 *even, u8 *odd)
{
    f32 dy = (R.cy - ((f32)y + 0.5f)) / R.focal; /* the ray through the centre of this row */
    f32 wy = R.up.y * dy + R.fwd.y;
    f32 elev = wy / ub_sqrtf(dy * dy + 1.0f);
    u32 c = elev >= 0 ? lerp_rgb(R.env.sky_horizon, R.env.sky_top, elev * 1.6f)
                      : lerp_rgb(R.env.sky_horizon, R.env.ground_color, -elev * 6.0f);
    if (R.fb->format == UB_PIXEL_INDEX8) {
        int r = (int)(c >> 16) & 255, g = (int)(c >> 8) & 255, b = (int)c & 255;
        *even = ub_palette_match(r, g, b, (y & 1) << 1);
        *odd = ub_palette_match(r, g, b, ((y & 1) << 1) | 1);
    }
    return c;
}

/* fast: the sky where nothing was drawn (depth still clear) */
static void sky_behind(int y0, int y1)
{
    ub_framebuffer *fb = R.fb;
    for (int y = y0; y < y1; ++y) {
        const u32 *z = R.zbuf + y * R.zw;
        u8 even = 0, odd = 0;
        u32 c = sky_row(y, &even, &odd);
        if (fb->format == UB_PIXEL_XRGB32) {
            u32 *row = fb->pixels + y * fb->pitch;
            for (int x = 0; x < fb->width; ++x)
                if (!z[x]) row[x] = c;
        } else {
            u8 *row = fb->pixels8 + y * fb->pitch;
            for (int x = 0; x < fb->width; ++x)
                if (!z[x]) row[x] = (x & 1) ? odd : even;
        }
    }
}

static void draw_sky(int y0, int y1)
{
    ub_framebuffer *fb = R.fb;
    for (int y = y0; y < y1; ++y) {
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

static vec3 to_view(vec3 w);
static vec3 to_view_dir(vec3 w) { return to_view(w); }

void sw_begin(ub_framebuffer *fb, const ub_camera *cam, const ub_render_env *env)
{
    R.t0 = plat_time_us();
    memset(&R.st, 0, sizeof R.st);
    R.fb = fb;
    R.env = *env;
    R.frame++;
    R.nqueue = R.npolys = R.nverts = 0;
    R.pool_cur = R.pool;
    R.pool_used = 0;
    R.overflow = false;
    R.flushed = false;

    if (!R.zbuf || R.zw != fb->width || R.zh != fb->height) {
        plat_free(R.zbuf);
        R.zbuf = (u32 *)plat_alloc((size_t)fb->width * fb->height * sizeof(u32));
        R.zw = R.zbuf ? fb->width : 0;
        R.zh = R.zbuf ? fb->height : 0;
    }

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

    /* the lights' colours, as tints that average 1 (the fast renderer's) */
    u32 lc[3] = { env->sky_light, env->ground_light, env->sun_color };
    f32 *tint[3] = { R.sky_tint, R.ground_tint, R.sun_tint };
    for (int i = 0; i < 3; ++i) {
        f32 r = (f32)((lc[i] >> 16) & 255), g = (f32)((lc[i] >> 8) & 255), b = (f32)(lc[i] & 255);
        f32 avg = (r + g + b) / 3.0f;
        if (avg < 1) r = g = b = avg = 1;
        tint[i][0] = r / avg;
        tint[i][1] = g / avg;
        tint[i][2] = b / avg;
    }
    R.sun_view = to_view_dir(env->sun_dir);

    /* fast, high: the sun's shadow map round the camera's focus, its middle
     * on the map's grid so the shadows' edges keep still as it moves */
    R.shadow_on = R.fast && R.quality >= 2 && env->sun_strength > 0.05f && env->sun_dir.y > 0.05f;
    R.ncasters = R.nspolys = R.nsverts = 0;
    if (R.shadow_on && !R.smap) {
        R.smap = (u32 *)plat_alloc(sizeof(u32) * SMAP * SMAP);
        if (!R.smap) R.shadow_on = false;
    }
    if (R.shadow_on) {
        R.lz = v3_normalize(env->sun_dir);
        vec3 a = ub_absf(R.lz.y) < 0.9f ? v3(0, 1, 0) : v3(1, 0, 0);
        R.lx = v3_normalize(v3_cross(a, R.lz));
        R.ly = v3_cross(R.lz, R.lx);
        R.lcx = (f32)ub_rint(v3_dot(cam->focus, R.lx) / SMAP_TEXEL) * SMAP_TEXEL;
        R.lcy = (f32)ub_rint(v3_dot(cam->focus, R.ly) / SMAP_TEXEL) * SMAP_TEXEL;
    }
    /* fast, medium and up: fog towards the horizon's colour, looked up by
     * depth (the z-buffer's 1/z as a float's bits: no divide per pixel) */
    /* (not in 256 colours: light fog snaps to a palette colour too far off) */
    R.fog_on = R.fast && R.quality >= 1 && env->fog_density > 0.0001f && fb->format == UB_PIXEL_XRGB32;
    if (R.fog_on) {
        static f32 last_density = -1;
        if (env->fog_density != last_density) {
            for (int i = 0; i < FOG_BITS; ++i) {
                union { u32 b; f32 f; } u;
                u.b = ((u32)(i + FOG_BASE) << 16) | 0x8000u; /* the middle of the bucket */
                f32 zv = u.f; /* a z-buffer value (2^28 / z) */
                f32 d = zv > 1 ? ZSCALE / zv * env->fog_density : 1e9f;
                /* 1 - e^-d, without a library exp: (1 + d/64)^-64 */
                f32 e = d < 1e6f ? 1.0f / (1.0f + d / 64.0f) : 0;
                e = e * e; e = e * e; e = e * e; e = e * e; e = e * e; e = e * e;
                R.fog_lut[i] = (u8)ub_clampf((1.0f - e) * 255.0f + 0.5f, 0, 255);
            }
            last_density = env->fog_density;
        }
        R.fog_color = env->sky_horizon;
    }

    memset(&R.main, 0, sizeof R.main);
    R.main.subdiv = SUBDIV;
    R.main.y0 = 0;
    R.main.y1 = fb->height;
    R.main.w = fb->width;
    R.main.zmask = ~0u;
    if (!R.fast) {
        /* (the fast renderer clears and draws the sky row by row, in its threads) */
        if (R.zbuf) memset(R.zbuf, 0, (size_t)R.zw * R.zh * sizeof(u32));
        draw_sky(0, fb->height);
    }
}

void sw_end(ub_render_stats *stats)
{
    if (R.fast && !R.flushed) sw_flush(); /* (parts given straight to the backend) */
    R.st.pixels_tested += R.main.pixels;
    R.main.pixels = 0;
    R.st.render_us = (u32)(plat_time_us() - R.t0);
    if (stats) *stats = R.st;
}

/* -------------------------------------------------------------- spans */

static i32 clamp_fixed(f32 v)
{
    if (v > 2.0e9f) return 2000000000;
    if (v < -2.0e9f) return -2000000000;
    return ub_rint(v);
}

static void span_flat32(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    (void)uz; (void)vz;
    u32 *row = R.fb->pixels + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    u32 col = c->s->flat32;
    u32 zf = (ub_rintu(iz * ZSCALE) & c->zmask) | c->lvl;
    u32 dz = (u32)clamp_fixed(c->g.diz_dx * ZSCALE) & c->zmask;
    for (int x = x0; x < x1; ++x) {
        if (zf > zrow[x]) {
            zrow[x] = zf;
            row[x] = col;
        }
        zf += dz;
    }
    c->pixels += (u32)(x1 - x0);
}

static void span_flat8(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    (void)uz; (void)vz;
    u8 *row = R.fb->pixels8 + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    u8 even = c->s->flat8[(y & 1) << 1], odd = c->s->flat8[((y & 1) << 1) | 1];
    u32 zf = (ub_rintu(iz * ZSCALE) & c->zmask) | c->lvl;
    u32 dz = (u32)clamp_fixed(c->g.diz_dx * ZSCALE) & c->zmask;
    for (int x = x0; x < x1; ++x) {
        if (zf > zrow[x]) {
            zrow[x] = zf;
            row[x] = (x & 1) ? odd : even;
        }
        zf += dz;
    }
    c->pixels += (u32)(x1 - x0);
}

/* Shared setup for one perspective-correct segment of a textured span. */
typedef struct {
    i32 u, v, du, dv;
    u32 z, dz;
} seg;

static void segment(const rctx *c, seg *s, int len, f32 *iz, f32 *uz, f32 *vz, f32 *u, f32 *v)
{
    /* Keep values sane for the integer conversions below even when a
     * nearly edge-on face sends u, v towards infinity. */
    *u = ub_clampf(*u, -1.0e7f, 1.0e7f);
    *v = ub_clampf(*v, -1.0e7f, 1.0e7f);
    f32 iz2 = *iz + c->g.diz_dx * (f32)len;
    f32 uz2 = *uz + c->g.duz_dx * (f32)len;
    f32 vz2 = *vz + c->g.dvz_dx * (f32)len;
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
    s->u = ub_rint((*u - bu) * 65536.0f);
    s->v = ub_rint((*v - bv) * 65536.0f);
    s->du = ub_rint(su * inv);
    s->dv = ub_rint(sv * inv);
    s->z = (ub_rintu(*iz * ZSCALE) & c->zmask) | c->lvl;
    s->dz = (u32)clamp_fixed(c->g.diz_dx * ZSCALE) & c->zmask;
    *iz = iz2;
    *uz = uz2;
    *vz = vz2;
    *u = u2;
    *v = v2;
}

#define TEXEL(tex, s) (tex)[(((s).v >> 10) & (UB_TEX_MASK << UB_TEX_SHIFT)) | (((s).u >> 16) & UB_TEX_MASK)]

static void span_tex32(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    u32 *row = R.fb->pixels + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    const u8 *tex = c->s->tex;
    const u32 *lut = c->s->lut32;
    f32 z = 1.0f / iz, u = uz * z, v = vz * z;
    c->pixels += (u32)(x1 - x0);
    while (x0 < x1) {
        int len = x1 - x0 > c->subdiv ? c->subdiv : x1 - x0;
        seg s;
        segment(c, &s, len, &iz, &uz, &vz, &u, &v);
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

static void span_tex8(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    u8 *row = R.fb->pixels8 + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    const u8 *tex = c->s->tex;
    const u8 *lut_even = c->s->lut8[(y & 1) << 1], *lut_odd = c->s->lut8[((y & 1) << 1) | 1];
    f32 z = 1.0f / iz, u = uz * z, v = vz * z;
    c->pixels += (u32)(x1 - x0);
    while (x0 < x1) {
        int len = x1 - x0 > c->subdiv ? c->subdiv : x1 - x0;
        seg s;
        segment(c, &s, len, &iz, &uz, &vz, &u, &v);
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

/* See-through parts: the same spans, leaving out the pixels the dither
 * pattern picks (neither colour nor depth is written there). */
static void span_see32(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    u32 *row = R.fb->pixels + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    const u8 *pat = ub_see_through_pattern[y & 3];
    const int level = c->see;
    c->pixels += (u32)(x1 - x0);
    if (!c->s->tex) {
        u32 col = c->s->flat32;
        u32 zf = (ub_rintu(iz * ZSCALE) & c->zmask) | c->lvl;
        u32 dz = (u32)clamp_fixed(c->g.diz_dx * ZSCALE) & c->zmask;
        for (int x = x0; x < x1; ++x, zf += dz)
            if (pat[x & 3] >= level && zf > zrow[x]) {
                zrow[x] = zf;
                row[x] = col;
            }
        return;
    }
    const u8 *tex = c->s->tex;
    const u32 *lut = c->s->lut32;
    f32 z = 1.0f / iz, u = uz * z, v = vz * z;
    while (x0 < x1) {
        int len = x1 - x0 > c->subdiv ? c->subdiv : x1 - x0;
        seg s;
        segment(c, &s, len, &iz, &uz, &vz, &u, &v);
        for (int x = x0; x < x0 + len; ++x) {
            if (pat[x & 3] >= level && s.z > zrow[x]) {
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

static void span_see8(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    u8 *row = R.fb->pixels8 + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    const u8 *pat = ub_see_through_pattern[y & 3];
    const int level = c->see;
    c->pixels += (u32)(x1 - x0);
    if (!c->s->tex) {
        u8 even = c->s->flat8[(y & 1) << 1], odd = c->s->flat8[((y & 1) << 1) | 1];
        u32 zf = (ub_rintu(iz * ZSCALE) & c->zmask) | c->lvl;
        u32 dz = (u32)clamp_fixed(c->g.diz_dx * ZSCALE) & c->zmask;
        for (int x = x0; x < x1; ++x, zf += dz)
            if (pat[x & 3] >= level && zf > zrow[x]) {
                zrow[x] = zf;
                row[x] = (x & 1) ? odd : even;
            }
        return;
    }
    const u8 *tex = c->s->tex;
    const u8 *lut_even = c->s->lut8[(y & 1) << 1], *lut_odd = c->s->lut8[((y & 1) << 1) | 1];
    f32 z = 1.0f / iz, u = uz * z, v = vz * z;
    while (x0 < x1) {
        int len = x1 - x0 > c->subdiv ? c->subdiv : x1 - x0;
        seg s;
        segment(c, &s, len, &iz, &uz, &vz, &u, &v);
        for (int x = x0; x < x0 + len; ++x) {
            if (pat[x & 3] >= level && s.z > zrow[x]) {
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

/* The fast renderer's textured spans: u and v in one register (6.10 fixed
 * point each, v in the top half; a step that carries out of u leaves v a
 * 1/1024 texel off once per texture repeat), and loops simple enough for
 * the Pentium's seven registers. */
static inline u32 pack_uv(const seg *s) { return ((u32)(s->v >> 6) << 16) | ((u32)(s->u >> 6) & 0xFFFFu); }
static inline u32 pack_duv(const seg *s) { return ((u32)(s->dv >> 6) << 16) + (u32)(s->du >> 6); }
#define PTEXEL(tex, uv) (tex)[(((uv) >> 20) & (UB_TEX_MASK << UB_TEX_SHIFT)) | (((uv) >> 10) & UB_TEX_MASK)]

/* Which of the texture's averaged versions suits a segment starting at
 * (u, v) (texels) with 1/z = iz on row y: how many texels a pixel covers,
 * across and down the screen (mipmapping, in the fast renderer). */
static const u8 *mip_for(const rctx *c, f32 iz, f32 u, f32 v)
{
    if (!c->mips) return c->s->tex;
    f32 z = 1.0f / iz;
    f32 udx = (c->g.duz_dx - u * c->g.diz_dx) * z, vdx = (c->g.dvz_dx - v * c->g.diz_dx) * z;
    f32 udy = (c->g.duz_dy - u * c->g.diz_dy) * z, vdy = (c->g.dvz_dy - v * c->g.diz_dy) * z;
    f32 a = udx * udx + vdx * vdx, b = udy * udy + vdy * vdy;
    f32 r2 = a > b ? a : b;
    int k = r2 < 1.96f ? 0 : r2 < 7.84f ? 1 : r2 < 31.4f ? 2 : r2 < 125.0f ? 3 : r2 < 502.0f ? 4 : 5;
    return c->s->mip[k];
}

static void span_ftex32(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    u32 *row = R.fb->pixels + y * R.fb->pitch + x0;
    u32 *zp = R.zbuf + y * R.zw + x0;
    const u32 *lut = c->s->lut32;
    f32 z = 1.0f / iz, u = uz * z, v = vz * z;
    c->pixels += (u32)(x1 - x0);
    while (x0 < x1) {
        int len = x1 - x0 > c->subdiv ? c->subdiv : x1 - x0;
        seg s;
        const u8 *tex = mip_for(c, iz, u, v);
        segment(c, &s, len, &iz, &uz, &vz, &u, &v);
        u32 uv = pack_uv(&s), zf = s.z;
        const u32 duv = pack_duv(&s), dz = s.dz;
        x0 += len;
        do {
            if (zf > *zp) {
                *zp = zf;
                *row = lut[PTEXEL(tex, uv)];
            }
            zp++;
            row++;
            zf += dz;
            uv += duv;
        } while (--len);
    }
}

/* 8-bit: two pixels a step, the even and the odd column's dither tables */
static void span_ftex8(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    u8 *row = R.fb->pixels8 + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    const u8 *lut[2] = { c->s->lut8[(y & 1) << 1], c->s->lut8[((y & 1) << 1) | 1] };
    f32 z = 1.0f / iz, u = uz * z, v = vz * z;
    c->pixels += (u32)(x1 - x0);
    while (x0 < x1) {
        int len = x1 - x0 > c->subdiv ? c->subdiv : x1 - x0;
        seg s;
        const u8 *tex = mip_for(c, iz, u, v);
        segment(c, &s, len, &iz, &uz, &vz, &u, &v);
        u32 uv = pack_uv(&s), zf = s.z;
        const u32 duv = pack_duv(&s), dz = s.dz;
        int x = x0, end = x0 + len;
        x0 = end;
        if (x & 1) { /* to an even column */
            if (zf > zrow[x]) {
                zrow[x] = zf;
                row[x] = lut[1][PTEXEL(tex, uv)];
            }
            x++;
            zf += dz;
            uv += duv;
        }
        const u8 *le = lut[0], *lo = lut[1];
        for (; x + 1 < end; x += 2) {
            if (zf > zrow[x]) {
                zrow[x] = zf;
                row[x] = le[PTEXEL(tex, uv)];
            }
            zf += dz;
            uv += duv;
            if (zf > zrow[x + 1]) {
                zrow[x + 1] = zf;
                row[x + 1] = lo[PTEXEL(tex, uv)];
            }
            zf += dz;
            uv += duv;
        }
        if (x < end && zf > zrow[x]) {
            zrow[x] = zf;
            row[x] = le[PTEXEL(tex, uv)];
        }
    }
}

/* Smooth faces (fast renderer): flat colour, the light stepped across the
 * span through the face's ramp (16.16 fixed point). */
static void smooth_start(const rctx *c, int y, int x0, int x1, i32 *l, i32 *dl)
{
    f32 fl = c->g.l0 + c->g.dl_dx * ((f32)x0 + 0.5f - c->g.x0) + c->g.dl_dy * ((f32)y + 0.5f - c->g.y0);
    f32 fe = fl + c->g.dl_dx * (f32)(x1 - x0 - 1);
    fl = ub_clampf(fl, 0, (f32)RAMP - 0.01f);
    fe = ub_clampf(fe, 0, (f32)RAMP - 0.01f);
    *l = ub_rint(fl * 65536.0f);
    *dl = x1 - x0 > 1 ? ub_rint((fe - fl) * 65536.0f / (f32)(x1 - x0 - 1)) : 0;
}

static void span_smooth32(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    (void)uz; (void)vz;
    u32 *row = R.fb->pixels + y * R.fb->pitch + x0;
    u32 *zp = R.zbuf + y * R.zw + x0;
    const u32 *ramp = c->s->ramp32;
    u32 zf = (ub_rintu(iz * ZSCALE) & c->zmask) | c->lvl, dz = (u32)clamp_fixed(c->g.diz_dx * ZSCALE) & c->zmask;
    i32 l, dl;
    smooth_start(c, y, x0, x1, &l, &dl);
    c->pixels += (u32)(x1 - x0);
    for (int n = x1 - x0; n > 0; --n) {
        if (zf > *zp) {
            *zp = zf;
            *row = ramp[l >> 16];
        }
        zp++;
        row++;
        zf += dz;
        l += dl;
    }
}

static void span_smooth8(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    (void)uz; (void)vz;
    u8 *row = R.fb->pixels8 + y * R.fb->pitch;
    u32 *zrow = R.zbuf + y * R.zw;
    const u8 *re = c->s->ramp8[(y & 1) << 1], *ro = c->s->ramp8[((y & 1) << 1) | 1];
    u32 zf = (ub_rintu(iz * ZSCALE) & c->zmask) | c->lvl, dz = (u32)clamp_fixed(c->g.diz_dx * ZSCALE) & c->zmask;
    i32 l, dl;
    smooth_start(c, y, x0, x1, &l, &dl);
    c->pixels += (u32)(x1 - x0);
    for (int x = x0; x < x1; ++x) {
        if (zf > zrow[x]) {
            zrow[x] = zf;
            row[x] = ((x & 1) ? ro : re)[l >> 16];
        }
        zf += dz;
        l += dl;
    }
}

/* ------------------------------------------------------- rasterizer */

/* Attribute gradients from the largest triangle fan member, for numerical
 * stability (the polygon is planar, so any works in theory). False if it
 * is degenerate: edge-on or sub-pixel. */
static bool poly_grads(const pvert *p, int n, grads *g)
{
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
    if (ub_absf(best_d) < 1e-3f) return false;
    const pvert *a = &p[0], *b = &p[best], *c = &p[best + 1];
    f32 x1 = b->sx - a->sx, y1 = b->sy - a->sy, x2 = c->sx - a->sx, y2 = c->sy - a->sy;
    f32 inv = 1.0f / best_d;
    g->diz_dx = ((b->iz - a->iz) * y2 - (c->iz - a->iz) * y1) * inv;
    g->diz_dy = ((c->iz - a->iz) * x1 - (b->iz - a->iz) * x2) * inv;
    g->duz_dx = ((b->uz - a->uz) * y2 - (c->uz - a->uz) * y1) * inv;
    g->duz_dy = ((c->uz - a->uz) * x1 - (b->uz - a->uz) * x2) * inv;
    g->dvz_dx = ((b->vz - a->vz) * y2 - (c->vz - a->vz) * y1) * inv;
    g->dvz_dy = ((c->vz - a->vz) * x1 - (b->vz - a->vz) * x2) * inv;
    g->dl_dx = ((b->l - a->l) * y2 - (c->l - a->l) * y1) * inv;
    g->dl_dy = ((c->l - a->l) * x1 - (b->l - a->l) * x2) * inv;
    g->l0 = a->l;
    g->x0 = a->sx;
    g->y0 = a->sy;
    g->iz0 = a->iz;
    g->uz0 = a->uz;
    g->vz0 = a->vz;
    return true;
}

/* One run of a row: attributes at its first pixel, then the span. */
static void emit_run(rctx *c, span_fn span, int y, int x0, int x1, f32 fy)
{
    f32 dx = (f32)x0 + 0.5f - c->g.x0, dyy = fy - c->g.y0;
    f32 iz = c->g.iz0 + c->g.diz_dx * dx + c->g.diz_dy * dyy;
    f32 uz = c->g.uz0 + c->g.duz_dx * dx + c->g.duz_dy * dyy;
    f32 vz = c->g.vz0 + c->g.dvz_dx * dx + c->g.dvz_dy * dyy;
    if (iz < 1e-6f) iz = 1e-6f;
    span(c, y, x0, x1, iz, uz, vz);
    if (c->dirty)
        for (int tx = x0 >> TILE_SHIFT, last = (x1 - 1) >> TILE_SHIFT; tx <= last; ++tx) c->dirty[tx] = 1;
}

/* Scan-converts a convex polygon (gradients in c->g) over the rows c may
 * draw. Pixel centres are at +0.5; a pixel is drawn when its centre lies
 * inside (top-left rule), so polygons sharing an edge neither overlap nor
 * leave gaps. With c->occ, the tiles where it is hidden are left out. */
static void raster_scan(rctx *c, const pvert *p, int n, span_fn span)
{
    int top = 0;
    f32 ymin = p[0].sy, ymax = p[0].sy;
    for (int i = 1; i < n; ++i) {
        if (p[i].sy < ymin) { ymin = p[i].sy; top = i; }
        if (p[i].sy > ymax) ymax = p[i].sy;
    }
    int y = ub_ceilf(ymin - 0.5f), yend = ub_ceilf(ymax - 0.5f);
    if (y < c->y0) y = c->y0;
    if (yend > c->y1) yend = c->y1;
    if (y >= yend) return;

    /* Walk both chains down from the top vertex; per scanline the left
     * and right ends are just the min and max of the two. */
    int ai = top, an = (top + 1) % n;          /* chain A: increasing index */
    int bi = top, bn = (top + n - 1) % n;      /* chain B: decreasing index */
    f32 aslope = 0, bslope = 0;
    bool a_set = false, b_set = false;
    const int w = c->w;

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
        if (!c->occ) {
            emit_run(c, span, y, x0, x1, fy);
            continue;
        }
        int run = x0;
        for (int tx = x0 >> TILE_SHIFT, last = (x1 - 1) >> TILE_SHIFT; tx <= last; ++tx) {
            if (!c->occ[tx]) continue;
            int a = tx << TILE_SHIFT, b = a + TILE;
            if (a < x0) a = x0;
            if (b > x1) b = x1;
            if (run < a) emit_run(c, span, y, run, a, fy);
            run = b;
        }
        if (run < x1) emit_run(c, span, y, run, x1, fy);
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
            o->l = a->l + (b->l - a->l) * t;
        }
    }
    return m;
}

/* --------------------------------------------------------------- parts */

static u32 pack_rgb(i32 r, i32 g, i32 b)
{
    return ((u32)UB_CLAMP(r, 0, 255) << 16) | ((u32)UB_CLAMP(g, 0, 255) << 8) | (u32)UB_CLAMP(b, 0, 255);
}

/* The colour table for a face: its colour in light q (per channel, 256 =
 * full), through the material's texture levels. */
static void fill_shading(shading *sh, u32 color, ub_material mat, const i16 *q, bool idx8)
{
    i32 r = (i32)((color >> 16) & 255), g = (i32)((color >> 8) & 255), b = (i32)(color & 255);
    sh->tex = ub_texture(mat);
    for (int k = 0; k < UB_TEX_MIPS; ++k) sh->mip[k] = ub_texture_mip(mat, k);
    if (!sh->tex) {
        i32 rr = (r * q[0]) >> 8, gg = (g * q[1]) >> 8, bb = (b * q[2]) >> 8;
        if (idx8) {
            for (int d = 0; d < 4; ++d) sh->flat8[d] = ub_palette_match(rr, gg, bb, d);
        } else {
            sh->flat32 = pack_rgb(rr, gg, bb);
        }
        return;
    }
    for (int t = 0; t < UB_TEX_LEVELS; ++t) {
        i32 k = ub_texel_gain(t);
        i32 rr = (r * ((q[0] * k) >> 8)) >> 8, gg = (g * ((q[1] * k) >> 8)) >> 8, bb = (b * ((q[2] * k) >> 8)) >> 8;
        if (idx8) {
            for (int d = 0; d < 4; ++d) sh->lut8[d][t] = ub_palette_match(rr, gg, bb, d);
        } else {
            sh->lut32[t] = pack_rgb(rr, gg, bb);
        }
    }
}

/* A smooth face's ramp: its colour, tinted by q (per channel, 256 = 1),
 * from no light to RAMP_MAX; past 1 the highlight goes towards white. */
static void fill_ramp(shading *sh, u32 color, const i16 *q, bool idx8)
{
    f32 c[3] = { (f32)((color >> 16) & 255), (f32)((color >> 8) & 255), (f32)(color & 255) };
    sh->tex = NULL;
    for (int i = 0; i < RAMP + 2; ++i) {
        f32 lv = (f32)UB_MIN(i, RAMP - 1) * RAMP_MAX / (f32)(RAMP - 1);
        f32 o[3];
        for (int k = 0; k < 3; ++k) {
            f32 d = c[k] * (f32)q[k] / 256.0f * (lv < 1 ? lv : 1);
            if (lv > 1) d += (255.0f - d) * ub_clampf((lv - 1) * 0.8f, 0, 1);
            o[k] = d;
        }
        i32 rr = ub_rint(o[0]), gg = ub_rint(o[1]), bb = ub_rint(o[2]);
        if (idx8) {
            for (int d = 0; d < 4; ++d) sh->ramp8[d][i] = ub_palette_match(rr, gg, bb, d);
        } else {
            sh->ramp32[i] = pack_rgb(rr, gg, bb);
        }
    }
}

/* A table from the pool (fast renderer, cache slot in use this frame). */
static shading *pool_take(void)
{
    if (R.pool_cur && R.pool_used == POOL_BLOCK) {
        if (!R.pool_cur->next) {
            pool_block *b = (pool_block *)plat_alloc(sizeof(pool_block));
            if (!b) return NULL;
            R.pool_cur->next = b;
        }
        R.pool_cur = R.pool_cur->next;
        R.pool_used = 0;
    }
    if (!R.pool_cur) {
        if (!R.pool) {
            R.pool = (pool_block *)plat_alloc(sizeof(pool_block));
            if (!R.pool) return NULL;
        }
        R.pool_cur = R.pool;
        R.pool_used = 0;
    }
    return &R.pool_cur->s[R.pool_used++];
}

/* The table for (colour, material, light per channel 0..1.6), cached;
 * ramp: a smooth face's ramp, the light being its tint. Light is
 * quantised to 1/64 steps (finer is invisible). */
static const shading *select_table(u32 color, ub_material mat, const f32 *light, bool ramp)
{
    i16 q[3];
    for (int k = 0; k < 3; ++k) q[k] = (i16)((i32)(ub_clampf(light[k], 0.0f, 1.6f) * 64.0f) * 4);
    u8 fmt = (u8)R.fb->format;
    if (ramp) mat = UB_MAT_PLASTIC;
    u32 h = (color * 2654435761u) ^ ((u32)q[0] * 40503u) ^ ((u32)q[1] * 9973u) ^ ((u32)q[2] * 70001u) ^
            ((u32)mat * 97u) ^ ((u32)fmt * 7919u) ^ (ramp ? 0x5bd1e995u : 0);
    lut_entry *e = &g_lut_cache[(h ^ (h >> 15)) & (LUT_CACHE - 1)];
    if (!e->used || e->color != color || e->q[0] != q[0] || e->q[1] != q[1] || e->q[2] != q[2] || e->material != mat ||
        e->format != fmt || e->ramp != ramp) {
        shading *s = &e->s;
        bool cached = true;
        if (R.fast && e->used && e->frame == R.frame) {
            /* the slot holds another table a waiting polygon uses */
            shading *p = pool_take();
            if (p) {
                s = p;
                cached = false;
            }
        }
        if (ramp) fill_ramp(s, color, q, fmt == UB_PIXEL_INDEX8);
        else fill_shading(s, color, mat, q, fmt == UB_PIXEL_INDEX8);
        R.st.luts_built++;
        if (!cached) return s;
        e->color = color;
        for (int k = 0; k < 3; ++k) e->q[k] = q[k];
        e->material = (u8)mat;
        e->format = fmt;
        e->ramp = ramp;
        e->used = true;
    }
    e->frame = R.frame;
    return &e->s;
}

static const shading *select_shading(u32 color, ub_material mat, f32 light)
{
    f32 l[3] = { light, light, light };
    return select_table(color, mat, l, false);
}

/* The fast renderer's light on a face turned to n (world space): the sky's
 * colour from above, the ground's from below, the sun's. */
static void light_rgb(vec3 n, f32 *out)
{
    f32 ndl = v3_dot(n, R.env.sun_dir);
    if (ndl < 0) ndl = 0;
    f32 a = R.env.ambient * (0.8f + 0.2f * n.y), up = n.y * 0.5f + 0.5f;
    for (int k = 0; k < 3; ++k)
        out[k] = a * (R.ground_tint[k] + (R.sky_tint[k] - R.ground_tint[k]) * up) + R.env.sun_strength * ndl * R.sun_tint[k];
}

/* x^n for a whole n (highlights) */
static f32 powi(f32 x, int n)
{
    f32 r = 1;
    while (n > 0) {
        if (n & 1) r *= x;
        x *= x;
        n >>= 1;
    }
    return r;
}

static vec3 to_view(vec3 w)
{
    return v3(v3_dot(w, R.right), v3_dot(w, R.up), v3_dot(w, R.fwd));
}

static bool grow(void **p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return true;
    int nc = *cap ? *cap : 256;
    while (nc < need) nc *= 2;
    void *n = plat_alloc((size_t)nc * elem);
    if (!n) return false;
    if (*p) memcpy(n, *p, (size_t)*cap * elem);
    plat_free(*p);
    *p = n;
    *cap = nc;
    return true;
}

/* A polygon ready to draw: now (plain renderer) or after the scene (fast). */
static void out_poly(const pvert *pv, int n, span_fn span, const shading *sh, int see, int lvl)
{
    grads g;
    if (!poly_grads(pv, n, &g)) return;
    R.st.polys_drawn++;
    if (!R.fast) {
        R.main.g = g;
        R.main.s = sh;
        R.main.see = see;
        raster_scan(&R.main, pv, n, span);
        return;
    }
    if (!grow((void **)&R.polys, &R.polys_cap, R.npolys + 1, sizeof(fpoly)) ||
        !grow((void **)&R.verts, &R.verts_cap, R.nverts + n, sizeof(pvert))) {
        R.overflow = true;
        return;
    }
    fpoly *fp = &R.polys[R.npolys++];
    fp->g = g;
    fp->s = sh;
    fp->span = span;
    fp->first = (u32)R.nverts;
    fp->n = (u8)n;
    fp->see = (u8)see;
    fp->lvl = (u8)lvl;
    f32 x0 = pv[0].sx, x1 = pv[0].sx, y0 = pv[0].sy, y1 = pv[0].sy, izmin = pv[0].iz, izmax = pv[0].iz;
    for (int i = 0; i < n; ++i) {
        R.verts[R.nverts++] = pv[i];
        x0 = UB_MIN(x0, pv[i].sx);
        x1 = UB_MAX(x1, pv[i].sx);
        y0 = UB_MIN(y0, pv[i].sy);
        y1 = UB_MAX(y1, pv[i].sy);
        izmin = UB_MIN(izmin, pv[i].iz);
        izmax = UB_MAX(izmax, pv[i].iz);
    }
    int w = R.fb->width, h = R.fb->height;
    fp->x0 = (i16)UB_CLAMP((int)ub_floorf(x0), 0, w);
    fp->x1 = (i16)UB_CLAMP((int)ub_floorf(x1) + 2, 0, w);
    fp->y0 = (i16)UB_CLAMP((int)ub_floorf(y0), 0, h);
    fp->y1 = (i16)UB_CLAMP((int)ub_floorf(y1) + 2, 0, h);
    f64 zn = (f64)izmax * ZSCALE;
    fp->znear = zn >= 4294967295.0 ? 0xFFFFFFFFu : (u32)zn;
    /* Little change in depth across it: step the perspective divide further
     * (at a 3% change, 64 pixels stray under a tenth of a texel per texel) */
    f32 ratio = izmax / (izmin > 1e-6f ? izmin : 1e-6f);
    fp->subdiv = (u8)(ratio < 1.03f ? 64 : ratio < 1.2f ? 32 : SUBDIV);
}

/* Sets up (and draws, or keeps) every face of a part. */
static void part_faces(const ub_part_draw *p, vec3 size, vec3 centre)
{
    ub_detail detail = UB_DETAIL_LOW;
    if (R.fast && R.quality >= 1 && !p->mesh) {
        /* big on the screen: the round shapes' finer mesh */
        f32 radius = 0.5f * v3_length(size), z = centre.z > NEAR_Z ? centre.z : NEAR_Z;
        if (radius * R.focal / z > 48.0f) detail = UB_DETAIL_HIGH;
    }
    const ub_mesh *mesh = p->mesh ? p->mesh : ub_mesh_get((ub_shape)p->shape, detail);
    ub_material mat = (ub_material)(p->material < UB_MAT_COUNT ? p->material : UB_MAT_PLASTIC);
    bool textured = ub_texture(mat) != NULL;
    f32 texels_per_unit = (f32)UB_TEX_SIZE / ub_materials[mat].world_size;

    /* part axes scaled by size, in view space */
    vec3 ax = to_view(v3_scale(p->rot.x, size.x));
    vec3 ay = to_view(v3_scale(p->rot.y, size.y));
    vec3 az = to_view(v3_scale(p->rot.z, size.z));

    static vec3 view_fixed[UB_MESH_MAX_VERTS];
    static vec3 *view_big;
    static int view_big_cap;
    vec3 *view = view_fixed;
    if (mesh->vert_count > UB_MESH_MAX_VERTS) { /* a part's own mesh */
        if (mesh->vert_count > view_big_cap) {
            vec3 *nb = (vec3 *)plat_alloc(sizeof(vec3) * (size_t)mesh->vert_count);
            if (!nb) return;
            plat_free(view_big);
            view_big = nb;
            view_big_cap = mesh->vert_count;
        }
        view = view_big;
    }
    for (int i = 0; i < mesh->vert_count; ++i) {
        vec3 l = mesh->verts[i];
        view[i] = v3(centre.x + ax.x * l.x + ay.x * l.y + az.x * l.z,
                     centre.y + ax.y * l.x + ay.y * l.y + az.y * l.z,
                     centre.z + ax.z * l.x + ay.z * l.y + az.z * l.z);
    }

    span_fn span = R.fb->format == UB_PIXEL_INDEX8 ? (textured ? (R.fast ? span_ftex8 : span_tex8) : span_flat8)
                                                    : (textured ? (R.fast ? span_ftex32 : span_tex32) : span_flat32);
    int see = ub_draw_see_level(p->flags);
    if (see) span = R.fb->format == UB_PIXEL_INDEX8 ? span_see8 : span_see32;

    for (int fi = 0; fi < mesh->face_count; ++fi) {
        const ub_face *f = &mesh->faces[fi];
        R.st.faces++;

        /* Normal after non-uniform scaling: divide by size, renormalise. */
        vec3 ns = v3_normalize(v3(f->normal.x / size.x, f->normal.y / size.y, f->normal.z / size.z));
        vec3 nw = mat3_apply(&p->rot, ns);
        vec3 nv = to_view(nw);
        if (v3_dot(nv, view[f->v[0]]) >= 0) { R.st.faces_backfacing++; continue; }

        const shading *sh;
        span_fn fspan = span;
        bool smooth = false;
        int lvl = 0; /* how much of its light is the sun's (where shadows darken it), 0 = none */
        if (R.shadow_on && !(p->flags & UB_DRAW_GLOW)) {
            f32 ndl = v3_dot(nw, R.env.sun_dir);
            if (ndl > 0.03f) {
                f32 sun = R.env.sun_strength * ndl, amb = R.env.ambient * (0.8f + 0.2f * nw.y);
                lvl = UB_CLAMP(ub_rint(sun / (sun + amb) * 0.85f * 8.0f), 0, 7);
            }
        }
        f32 vl[UB_FACE_MAX_VERTS];
        if (R.fast && R.quality >= 1 && !(p->flags & UB_DRAW_GLOW)) {
            /* in colour; curved faces lit at each corner, with a highlight */
            f32 rgb[3];
            light_rgb(nw, rgb);
            smooth = f->smooth && mesh->vnormals && !textured && !see;
            if (smooth) {
                f32 lum = (rgb[0] + rgb[1] + rgb[2]) / 3.0f, tint[3];
                /* (coarse steps: one ramp serves a ball's faces) */
                for (int k = 0; k < 3; ++k) tint[k] = (f32)ub_rint((lum > 0.02f ? rgb[k] / lum : 1.0f) * 16.0f) / 16.0f;
                sh = select_table(p->color, mat, tint, true);
                f32 spec = ub_materials[mat].specular;
                int shine = (int)ub_materials[mat].shininess;
                for (int i = 0; i < f->count; ++i) {
                    vec3 vn = mesh->vnormals[f->v[i]];
                    vec3 nws = v3_normalize(mat3_apply(&p->rot, v3_normalize(v3(vn.x / size.x, vn.y / size.y, vn.z / size.z))));
                    f32 c3[3];
                    light_rgb(nws, c3);
                    f32 l = (c3[0] + c3[1] + c3[2]) / 3.0f;
                    vec3 nvv = to_view(nws);
                    f32 ndl = v3_dot(nvv, R.sun_view);
                    if (ndl > 0 && spec > 0) {
                        vec3 h = v3_normalize(v3_sub(R.sun_view, v3_normalize(view[f->v[i]])));
                        f32 ndh = v3_dot(nvv, h);
                        if (ndh > 0) l += spec * powi(ndh, shine) * ndl * R.env.sun_strength * 2.0f;
                    }
                    vl[i] = ub_clampf(l, 0, RAMP_MAX) * (f32)(RAMP - 1) / RAMP_MAX;
                }
                fspan = R.fb->format == UB_PIXEL_INDEX8 ? span_smooth8 : span_smooth32;
            } else {
                sh = select_table(p->color, mat, rgb, false);
            }
        } else {
            f32 sun = v3_dot(nw, R.env.sun_dir);
            f32 light = R.env.ambient * (0.8f + 0.2f * nw.y) + R.env.sun_strength * (sun > 0 ? sun : 0);
            if (p->flags & UB_DRAW_GLOW) light = 1.25f;
            sh = select_shading(p->color, mat, light);
        }

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
            a[i].l = smooth ? vl[i] : 0;
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
            pv[i].l = in[i].l;
            /* comparisons are false for NaN, so this also rejects NaN */
            if (!(pv[i].sx > -1.0e5f && pv[i].sx < 1.0e5f && pv[i].sy > -1.0e5f && pv[i].sy < 1.0e5f))
                sane = false;
        }
        if (!sane) continue;
        out_poly(pv, n, fspan, sh, see, lvl);
    }
}

void sw_part(const ub_part_draw *p)
{
    if (!R.ready || !R.fb || !R.zbuf) return;
    R.st.parts++;

    vec3 size = v3(UB_MAX(p->size.x, 0.01f), UB_MAX(p->size.y, 0.01f), UB_MAX(p->size.z, 0.01f));
    vec3 centre = to_view(v3_sub(p->pos, R.cam_pos));
    f32 radius = 0.5f * v3_length(size);

    /* shadows come from parts near the camera's focus, seen or not */
    if (R.shadow_on && ub_draw_see_level(p->flags) < 8) {
        f32 px = v3_dot(p->pos, R.lx) - R.lcx, py = v3_dot(p->pos, R.ly) - R.lcy;
        f32 half = SMAP_TEXEL * SMAP * 0.5f + radius;
        if (px > -half && px < half && py > -half && py < half &&
            grow((void **)&R.casters, &R.casters_cap, R.ncasters + 1, sizeof(*R.casters)))
            R.casters[R.ncasters++] = p;
    }
    /* bounding sphere against the far distance and the five planes */
    if (centre.z - radius > R.far_dist) { R.st.parts_culled++; return; }
    for (int i = 0; i < 5; ++i) {
        const f32 *c = R.cull[i];
        if (c[0] * centre.x + c[1] * centre.y + c[2] * centre.z + c[3] < -radius) {
            R.st.parts_culled++;
            return;
        }
    }
    if (R.fast) {
        if (!grow((void **)&R.queue, &R.queue_cap, R.nqueue + 1, sizeof(queued))) {
            R.overflow = true;
            return;
        }
        R.queue[R.nqueue].key = centre.z;
        R.queue[R.nqueue].p = p;
        R.nqueue++;
        return;
    }
    part_faces(p, size, centre);
}

/* ------------------------------------------------- the fast renderer */

/* Stable merge sort, nearest first. */
static void sort_queue(void)
{
    int n = R.nqueue;
    if (n < 2) return;
    if (R.queue_tmp_cap < n) {
        plat_free(R.queue_tmp);
        R.queue_tmp = (queued *)plat_alloc(sizeof(queued) * (size_t)R.queue_cap);
        R.queue_tmp_cap = R.queue_tmp ? R.queue_cap : 0;
        if (!R.queue_tmp) return; /* unsorted: still right, only slower */
    }
    queued *src = R.queue, *dst = R.queue_tmp;
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = UB_MIN(lo + width, n), hi = UB_MIN(lo + 2 * width, n);
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) dst[k++] = src[j].key < src[i].key ? src[j++] : src[i++];
            while (i < mid) dst[k++] = src[i++];
            while (j < hi) dst[k++] = src[j++];
        }
        queued *t = src;
        src = dst;
        dst = t;
    }
    if (src != R.queue) memcpy(R.queue, src, sizeof(queued) * (size_t)n);
}

/* The farthest depth in a tile of the current row of tiles, if every pixel
 * is at least `thr` (else false: the tile stays as it was). */
static bool tile_depth(int tx, int y0, int y1, u32 thr, u32 *far)
{
    int x0 = tx << TILE_SHIFT, x1 = UB_MIN(x0 + TILE, R.zw);
    u32 m = 0xFFFFFFFFu;
    for (int y = y0; y < y1; ++y) {
        const u32 *z = R.zbuf + y * R.zw;
        for (int x = x0; x < x1; ++x) {
            if (z[x] < thr) return false;
            if (z[x] < m) m = z[x];
        }
    }
    *far = m;
    return true;
}

/* ------------------------------------------- shadows, fog, the sun
 * (the fast renderer, medium and high) */

/* The shadow map's depth spans: nearest the sun wins. */
static void span_shadow(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    (void)uz; (void)vz;
    u32 *row = R.smap + y * SMAP;
    u32 zf = ub_rintu(iz), dz = (u32)ub_rint(c->g.diz_dx);
    for (int x = x0; x < x1; ++x, zf += dz)
        if (zf > row[x]) row[x] = zf;
}

/* A point in the shadow map: x, y in texels; depth in its units. */
static void to_smap(vec3 w, f32 *x, f32 *y, f32 *d)
{
    *x = (v3_dot(w, R.lx) - R.lcx) / SMAP_TEXEL + SMAP * 0.5f;
    *y = (R.lcy - v3_dot(w, R.ly)) / SMAP_TEXEL + SMAP * 0.5f;
    *d = (v3_dot(w, R.lz) + SDEPTH_OFFSET) * SDEPTH_SCALE;
}

/* The casters' faces turned from the sun, as polygons on the map. */
static void shadow_polys(void)
{
    for (int ci = 0; ci < R.ncasters; ++ci) {
        const ub_part_draw *p = R.casters[ci];
        vec3 size = v3(UB_MAX(p->size.x, 0.01f), UB_MAX(p->size.y, 0.01f), UB_MAX(p->size.z, 0.01f));
        const ub_mesh *mesh = p->mesh ? p->mesh : ub_mesh_get((ub_shape)p->shape, UB_DETAIL_LOW);
        for (int fi = 0; fi < mesh->face_count; ++fi) {
            const ub_face *f = &mesh->faces[fi];
            vec3 ns = v3_normalize(v3(f->normal.x / size.x, f->normal.y / size.y, f->normal.z / size.z));
            /* the faces turned away from the sun: a lit face is then always
             * nearer the sun than its own part's far side, so it never
             * shadows itself */
            if (v3_dot(mat3_apply(&p->rot, ns), R.lz) >= 0) continue;
            if (!grow((void **)&R.sverts, &R.sverts_cap, R.nsverts + f->count, sizeof(pvert)) ||
                !grow((void **)&R.spolys, &R.spolys_cap, R.nspolys + 1, sizeof(fpoly)))
                return;
            pvert *pv = R.sverts + R.nsverts;
            f32 x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            bool sane = true;
            for (int i = 0; i < f->count; ++i) {
                vec3 l = v3_mul(mesh->verts[f->v[i]], size);
                vec3 w = v3_add(p->pos, mat3_apply(&p->rot, l));
                memset(&pv[i], 0, sizeof pv[i]);
                to_smap(w, &pv[i].sx, &pv[i].sy, &pv[i].iz);
                if (!(pv[i].sx > -1.0e5f && pv[i].sx < 1.0e5f && pv[i].sy > -1.0e5f && pv[i].sy < 1.0e5f)) sane = false;
                x0 = UB_MIN(x0, pv[i].sx);
                x1 = UB_MAX(x1, pv[i].sx);
                y0 = UB_MIN(y0, pv[i].sy);
                y1 = UB_MAX(y1, pv[i].sy);
            }
            if (!sane || x1 < 0 || y1 < 0 || x0 >= SMAP || y0 >= SMAP) continue;
            fpoly *fp = &R.spolys[R.nspolys];
            memset(fp, 0, sizeof *fp);
            if (!poly_grads(pv, f->count, &fp->g)) continue;
            fp->first = (u32)R.nsverts;
            fp->n = (u8)f->count;
            fp->y0 = (i16)UB_CLAMP((int)ub_floorf(y0), 0, SMAP);
            fp->y1 = (i16)UB_CLAMP((int)ub_floorf(y1) + 2, 0, SMAP);
            R.nsverts += f->count;
            R.nspolys++;
        }
    }
}

/* Thread t draws rows t, t + threads... of tiles of the shadow map. */
static void shadow_job(void *unused, int t)
{
    (void)unused;
    rctx c;
    memset(&c, 0, sizeof c);
    c.w = SMAP;
    for (int s = t; s < SMAP / TILE; s += R.threads) {
        int y0 = s * TILE, y1 = y0 + TILE;
        memset(R.smap + y0 * SMAP, 0, sizeof(u32) * TILE * SMAP);
        c.y0 = y0;
        c.y1 = y1;
        for (int i = 0; i < R.nspolys; ++i) {
            const fpoly *fp = &R.spolys[i];
            if (fp->y1 <= y0 || fp->y0 >= y1) continue;
            c.g = fp->g;
            raster_scan(&c, R.sverts + fp->first, fp->n, span_shadow);
        }
    }
}

/* 256 colours: each index shadowed (8 levels) and fogged (8 levels), made
 * again when the palette's use or the fog's colour changes */
static void make_remap(void)
{
    u32 key = R.fog_color ^ 0x80000000u;
    if (R.remap_ok && R.remap_key == key) return;
    const u32 *pal = ub_palette;
    i32 fr = (i32)((R.fog_color >> 16) & 255), fg = (i32)((R.fog_color >> 8) & 255), fb = (i32)(R.fog_color & 255);
    for (int sh = 0; sh < 8; ++sh)
        for (int fo = 0; fo < 8; ++fo)
            for (int i = 0; i < 256; ++i) {
                i32 m = 256 - sh * 32, f = fo * 32;
                i32 r = (i32)((pal[i] >> 16) & 255) * m >> 8, g = (i32)((pal[i] >> 8) & 255) * m >> 8,
                    b = (i32)(pal[i] & 255) * m >> 8;
                r += (fr - r) * f >> 8;
                g += (fg - g) * f >> 8;
                b += (fb - b) * f >> 8;
                for (int d = 0; d < 4; ++d) R.remap[d][sh][fo][i] = (sh || fo) ? ub_palette_match(r, g, b, d) : (u8)i;
            }
    R.remap_key = key;
    R.remap_ok = true;
}

static inline u32 darken_fog32(u32 c, u32 m, u32 f, u32 fogc)
{
    u32 rb = c & 0xFF00FFu, g = c & 0x00FF00u;
    if (m < 256) {
        rb = ((rb * m) >> 8) & 0xFF00FFu;
        g = ((g * m) >> 8) & 0x00FF00u;
    }
    if (f) {
        u32 frb = fogc & 0xFF00FFu, fg = fogc & 0x00FF00u, k = 256 - f;
        rb = ((rb * k + frb * f) >> 8) & 0xFF00FFu;
        g = ((g * k + fg * f) >> 8) & 0x00FF00u;
    }
    return rb | g;
}

/* How lit a pixel is (256 = all, less in shadow): where its depth puts it
 * on the shadow map, the four texels round that point weighted by nearness
 * (soft edges), deeper than the map by the face level's bias. */
static u32 shadow_at(int x, f32 zv, u32 lvl, f32 sx_base, f32 sy_base, f32 sd_base, f32 sx_row, f32 sy_row,
                     f32 sd_row, f32 sx_step, f32 sy_step, f32 sd_step)
{
    f32 zz = ZSCALE / zv, fx = (f32)x;
    f32 px = sx_base + zz * (sx_row + sx_step * fx) - 0.5f, py = sy_base + zz * (sy_row + sy_step * fx) - 0.5f;
    i32 ix = ub_rint(px - 0.5f), iy = ub_rint(py - 0.5f);
    if ((u32)ix >= SMAP - 1 || (u32)iy >= SMAP - 1) return 256;
    const u32 *t = R.smap + iy * SMAP + ix;
    u32 d = (u32)(sd_base + zz * (sd_row + sd_step * fx) + k_shadow_bias[lvl]);
    u32 a = t[0] > d, b = t[1] > d, c = t[SMAP] > d, e = t[SMAP + 1] > d;
    if (!(a | b | c | e)) return 256;
    i32 wx = ub_rint((px - (f32)ix) * 256.0f), wy = ub_rint((py - (f32)iy) * 256.0f);
    wx = UB_CLAMP(wx, 0, 256);
    wy = UB_CLAMP(wy, 0, 256);
    u32 top = a * (u32)(256 - wx) + b * (u32)wx, bot = c * (u32)(256 - wx) + e * (u32)wx;
    u32 w = (top * (u32)(256 - wy) + bot * (u32)wy) >> 8; /* 0..256 */
    return 256 - ((lvl * 32) * w >> 8);
}

/* After a row of tiles is drawn: the sky (and the sun) where nothing was;
 * elsewhere each pixel's place in the world from its depth, its shadow
 * (four samples of the map round it, so edges are soft) and its fog. */
static void finish_rows(int y0, int y1)
{
    ub_framebuffer *fb = R.fb;
    bool idx8 = fb->format == UB_PIXEL_INDEX8;
    const f32 inv_f = 1.0f / R.focal;
    /* the sun on the screen */
    f32 sunx = 0, suny = 0, sunr = 0;
    bool sun = R.quality >= 1 && R.sun_view.z > 0.05f && R.env.sun_strength > 0.05f;
    if (sun) {
        sunx = R.cx + R.focal * R.sun_view.x / R.sun_view.z;
        suny = R.cy - R.focal * R.sun_view.y / R.sun_view.z;
        sunr = R.focal * 0.035f;
    }
    u32 sunc = R.env.sun_color | 0x101010u;
    /* shadow map coordinates of the point at depth z through pixel (x, y):
     * base + z * (row + x * step), for each of x, y and depth */
    vec3 dx = v3_scale(R.right, inv_f);
    f32 sx_step = v3_dot(dx, R.lx) / SMAP_TEXEL, sy_step = -v3_dot(dx, R.ly) / SMAP_TEXEL;
    f32 sd_step = v3_dot(dx, R.lz) * SDEPTH_SCALE;
    f32 sx_base = 0, sy_base = 0, sd_base = 0;
    if (R.shadow_on) to_smap(R.cam_pos, &sx_base, &sy_base, &sd_base);
    if (idx8 && (R.shadow_on || R.fog_on)) make_remap();
    for (int y = y0; y < y1; ++y) {
        const u32 *z = R.zbuf + y * R.zw;
        u8 even = 0, odd = 0;
        u32 sky = sky_row(y, &even, &odd);
        u32 *row32 = fb->pixels + y * fb->pitch;
        u8 *row8 = fb->pixels8 + y * fb->pitch;
        f32 yn = (R.cy - ((f32)y + 0.5f)) * inv_f;
        /* the ray through (x = 0.5) of this row, in the map's terms */
        vec3 r0 = v3_add(R.fwd, v3_add(v3_scale(R.up, yn), v3_scale(R.right, (0.5f - R.cx) * inv_f)));
        f32 sx_row = v3_dot(r0, R.lx) / SMAP_TEXEL, sy_row = -v3_dot(r0, R.ly) / SMAP_TEXEL;
        f32 sd_row = v3_dot(r0, R.lz) * SDEPTH_SCALE;
        bool sun_row = sun && (f32)y > suny - sunr * 5 && (f32)y < suny + sunr * 5;
        /* a pixel's shadow serves its right neighbour on the same surface
         * (the same face level and nearly the same depth) */
        int pair_x = -2;
        u32 pair_lvl = 0, pair_z = 0, pair_tol = 0, pair_m = 256;
        for (int x = 0; x < fb->width; ++x) {
            u32 zr = z[x];
            if (!zr) { /* the sky */
                if (sun_row && (f32)x > sunx - sunr * 5 && (f32)x < sunx + sunr * 5) {
                    f32 ddx = (f32)x + 0.5f - sunx, ddy = (f32)y + 0.5f - suny;
                    f32 d = ub_sqrtf(ddx * ddx + ddy * ddy) / sunr;
                    f32 k = d < 1 ? 1.0f : d < 5 ? (1 - (d - 1) / 4) * (1 - (d - 1) / 4) * 0.45f : 0;
                    u32 f = (u32)(k * 256);
                    u32 c = f ? darken_fog32(sky, 256, f > 256 ? 256 : f, sunc) : sky;
                    if (idx8) row8[x] = ub_palette_match((int)(c >> 16) & 255, (int)(c >> 8) & 255, (int)c & 255, (x & 1) | ((y & 1) << 1));
                    else row32[x] = c;
                } else if (idx8) row8[x] = (x & 1) ? odd : even;
                else row32[x] = sky;
                continue;
            }
            u32 m = 256, f = 0, lvl = zr & 7;
            /* (as a float: halved to stay signed, a cheap conversion on the x87) */
            union { f32 f; u32 b; } fz;
            fz.f = (f32)(i32)(zr >> 1) * 2.0f;
            if (lvl && R.shadow_on) {
                if (x == pair_x + 1 && lvl == pair_lvl && zr - pair_z + pair_tol <= 2 * pair_tol) {
                    m = pair_m; /* the left neighbour's, on the same surface */
                } else {
                    m = shadow_at(x, fz.f, lvl, sx_base, sy_base, sd_base, sx_row, sy_row, sd_row, sx_step, sy_step, sd_step);
                    pair_x = x;
                    pair_lvl = lvl;
                    pair_z = zr;
                    pair_tol = zr >> 7;
                    pair_m = m;
                }
            }
            if (R.fog_on) {
                u32 fi = (fz.b >> 16) - FOG_BASE;
                f = R.fog_lut[fi < FOG_BITS ? fi : FOG_BITS - 1];
                if (f < 6) f = 0; /* (too little to see) */
            }
            if (m == 256 && !f) continue;
            if (idx8) {
                /* between the tables' levels by an ordered dither, so gradients stay smooth */
                static const u8 k_dith[4] = { 0, 16, 24, 8 };
                int cell = (x & 1) | ((y & 1) << 1);
                u32 sl = (256 - m + k_dith[cell]) >> 5, fl = (f + k_dith[cell]) >> 5;
                row8[x] = R.remap[cell][sl > 7 ? 7 : sl][fl > 7 ? 7 : fl][row8[x]];
            }
            else row32[x] = darken_fog32(row32[x], m, f, R.fog_color);
        }
    }
}

/* Thread t draws rows of tiles t, t + threads, t + 2 * threads... */
static void strip_job(void *unused, int t)
{
    (void)unused;
    rctx c;
    memset(&c, 0, sizeof c);
    u32 far[MAX_TILES_X];
    u8 dirty[MAX_TILES_X], occ[MAX_TILES_X];
    bool hiz = R.tiles_x <= MAX_TILES_X;
    u32 hidden = 0;
    for (int s = t; s < R.strips; s += R.threads) {
        int y0 = s * TILE, y1 = UB_MIN(y0 + TILE, R.fb->height);
        memset(R.zbuf + y0 * R.zw, 0, (size_t)(y1 - y0) * R.zw * sizeof(u32));
        if (hiz) {
            memset(far, 0, sizeof(u32) * (size_t)R.tiles_x);
            memset(dirty, 0, (size_t)R.tiles_x);
        }
        c.y0 = y0;
        c.y1 = y1;
        c.dirty = hiz ? dirty : NULL;
        c.mips = R.quality >= 1;
        c.w = R.fb->width;
        c.zmask = R.shadow_on ? ~7u : ~0u;
        for (int i = 0; i < R.npolys; ++i) {
            const fpoly *fp = &R.polys[i];
            if (fp->y1 <= y0 || fp->y0 >= y1 || fp->x0 >= fp->x1) continue;
            c.occ = NULL;
            if (hiz) {
                /* hidden where everything drawn so far is nearer than its nearest point */
                u32 thr = fp->znear + (fp->znear >> 10) + 64;
                if (thr < fp->znear) thr = 0xFFFFFFFFu;
                int tx0 = fp->x0 >> TILE_SHIFT, tx1 = (fp->x1 - 1) >> TILE_SHIFT, nh = 0;
                int rows = UB_MIN(fp->y1, y1) - UB_MAX(fp->y0, y0);
                for (int tx = tx0; tx <= tx1; ++tx) {
                    if (dirty[tx] && far[tx] < thr) {
                        /* worth a look only if it covers a good part of the tile */
                        int cols = UB_MIN(fp->x1, (tx + 1) << TILE_SHIFT) - UB_MAX(fp->x0, tx << TILE_SHIFT);
                        if (cols * rows >= 96 && tile_depth(tx, y0, y1, thr, &far[tx])) dirty[tx] = 0;
                    }
                    occ[tx] = far[tx] >= thr;
                    nh += occ[tx];
                }
                if (nh == tx1 - tx0 + 1) {
                    hidden++;
                    continue;
                }
                if (nh) c.occ = occ;
            }
            c.g = fp->g;
            c.s = fp->s;
            c.see = fp->see;
            c.subdiv = fp->subdiv;
            c.lvl = fp->lvl;
            raster_scan(&c, R.verts + fp->first, fp->n, fp->span);
        }
        if (R.shadow_on || R.fog_on || R.quality >= 1) finish_rows(y0, y1);
        else sky_behind(y0, y1);
    }
    R.t_pixels[t] = c.pixels;
    R.t_hidden[t] = hidden;
}

void sw_flush(void)
{
    if (!R.ready || !R.fb || !R.zbuf || !R.fast || R.flushed) return;
    R.flushed = true;
    sort_queue();
    for (int i = 0; i < R.nqueue; ++i) {
        const ub_part_draw *p = R.queue[i].p;
        vec3 size = v3(UB_MAX(p->size.x, 0.01f), UB_MAX(p->size.y, 0.01f), UB_MAX(p->size.z, 0.01f));
        part_faces(p, size, to_view(v3_sub(p->pos, R.cam_pos)));
    }
    R.nqueue = 0;
    R.strips = (R.fb->height + TILE - 1) / TILE;
    R.tiles_x = (R.fb->width + TILE - 1) / TILE;
    R.threads = UB_MIN(sw_threads(), R.strips);
    if (R.threads < 1) R.threads = 1;
    if (R.shadow_on) {
        shadow_polys();
        plat_parallel(shadow_job, NULL, R.threads);
    }
    plat_parallel(strip_job, NULL, R.threads);
    for (int t = 0; t < R.threads; ++t) {
        R.st.pixels_tested += R.t_pixels[t];
        R.st.polys_hidden += R.t_hidden[t];
    }
    R.npolys = R.nverts = 0;
}

/* ------------------------------------------------------- GUI pictures */

/* the picture the current span draws (BillboardGui, SurfaceGui) */
static const ub_image_quad *g_q;
static u32 g_q_bright;   /* brightness, 8.8 fixed point */
static bool g_q_ztest;

static inline u32 over32(u32 dst, u32 src, u32 bright)
{
    u32 a = src >> 24;
    if (!a) return dst;
    u32 r = (src >> 16) & 255, g = (src >> 8) & 255, b = src & 255;
    if (bright != 256) {
        r = UB_MIN(r * bright >> 8, 255u);
        g = UB_MIN(g * bright >> 8, 255u);
        b = UB_MIN(b * bright >> 8, 255u);
    }
    if (a == 255) return (r << 16) | (g << 8) | b;
    u32 ia = 255 - a;
    u32 dr = (dst >> 16) & 255, dg = (dst >> 8) & 255, db = dst & 255;
    dr = UB_MIN(r + (dr * ia + 127) / 255, 255u);
    dg = UB_MIN(g + (dg * ia + 127) / 255, 255u);
    db = UB_MIN(b + (db * ia + 127) / 255, 255u);
    return (dr << 16) | (dg << 8) | db;
}

/* 8-bit frames: blend in RGB through the palette, then back to an index */
static inline u8 over8(const u32 *pal, u8 dst, u32 src, u32 bright, int x, int y)
{
    u32 a = src >> 24;
    if (!a) return dst;
    u32 c = over32(pal ? pal[dst] : 0, src, bright);
    return ub_palette_match((int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255), (x & 1) | ((y & 1) << 1));
}

static void span_img(rctx *c, int y, int x0, int x1, f32 iz, f32 uz, f32 vz)
{
    const ub_render_image *im = &g_q->img;
    u32 *zrow = R.zbuf + y * R.zw;
    u32 zf = (ub_rintu(iz * ZSCALE) & c->zmask) | c->lvl;
    u32 dz = (u32)clamp_fixed(c->g.diz_dx * ZSCALE) & c->zmask;
    bool idx8 = R.fb->format == UB_PIXEL_INDEX8;
    for (int x = x0; x < x1; ++x, iz += c->g.diz_dx, uz += c->g.duz_dx, vz += c->g.dvz_dx, zf += dz) {
        if (g_q_ztest && zf <= zrow[x]) continue; /* behind a part */
        f32 z = 1.0f / (iz > 1e-6f ? iz : 1e-6f);
        int tx = ub_floorf(uz * z), ty = ub_floorf(vz * z);
        if (tx < 0 || ty < 0 || tx >= im->w || ty >= im->h) continue;
        u32 s = im->px[(size_t)ty * im->pitch + tx];
        if (!(s >> 24)) continue;
        if (idx8) {
            u8 *p = &R.fb->pixels8[y * R.fb->pitch + x];
            *p = over8(R.fb->palette, *p, s, g_q_bright, x, y);
        } else {
            u32 *p = &R.fb->pixels[y * R.fb->pitch + x];
            *p = over32(*p, s, g_q_bright);
        }
    }
    c->pixels += (u32)(x1 - x0);
}

void sw_image_quad(const ub_image_quad *q)
{
    if (!R.ready || !R.fb || !R.zbuf || !q->img.px) return;
    cvert a[MAX_CLIP], b[MAX_CLIP];
    const f32 uv[4][2] = { { 0, 0 }, { (f32)q->img.w, 0 }, { (f32)q->img.w, (f32)q->img.h }, { 0, (f32)q->img.h } };
    for (int k = 0; k < 4; ++k) {
        vec3 v = to_view(v3_sub(q->c[k], R.cam_pos));
        a[k].x = v.x;
        a[k].y = v.y;
        a[k].z = v.z;
        a[k].u = uv[k][0];
        a[k].v = uv[k][1];
        a[k].l = 0;
    }
    int n = 4;
    cvert *in = a, *out = b;
    for (int k = 0; k < 5 && n >= 3; ++k) {
        n = clip_poly(in, n, out, R.clip[k]);
        cvert *t = in;
        in = out;
        out = t;
    }
    if (n < 3) return;
    pvert pv[MAX_CLIP];
    for (int i = 0; i < n; ++i) {
        f32 iz = 1.0f / (in[i].z > NEAR_Z * 0.5f ? in[i].z : NEAR_Z * 0.5f);
        pv[i].sx = R.cx + R.focal * in[i].x * iz;
        pv[i].sy = R.cy - R.focal * in[i].y * iz;
        pv[i].iz = iz;
        pv[i].uz = in[i].u * iz;
        pv[i].vz = in[i].v * iz;
        pv[i].l = 0;
        if (!(pv[i].sx > -1.0e5f && pv[i].sx < 1.0e5f && pv[i].sy > -1.0e5f && pv[i].sy < 1.0e5f)) return;
    }
    g_q = q;
    g_q_bright = (u32)(ub_clampf(q->brightness, 0, 4) * 256.0f);
    g_q_ztest = !q->on_top;
    /* either winding: the polygon walker does not care */
    rctx *c = &R.main;
    c->occ = NULL;
    c->dirty = NULL;
    c->y0 = 0;
    c->y1 = R.fb->height;
    c->w = R.fb->width;
    if (poly_grads(pv, n, &c->g)) raster_scan(c, pv, n, span_img);
    g_q = NULL;
}

void sw_overlay(ub_framebuffer *fb, const ub_render_image *img, const int *rects, int nrects)
{
    int W = UB_MIN(fb->width, img->w), H = UB_MIN(fb->height, img->h);
    bool idx8 = fb->format == UB_PIXEL_INDEX8;
    for (int r = 0; r < nrects; ++r) {
        int x0 = UB_MAX(rects[r * 4], 0), y0 = UB_MAX(rects[r * 4 + 1], 0);
        int x1 = UB_MIN(rects[r * 4 + 2], W), y1 = UB_MIN(rects[r * 4 + 3], H);
        for (int y = y0; y < y1; ++y) {
            const u32 *src = img->px + (size_t)y * img->pitch;
            if (idx8) {
                u8 *dst = fb->pixels8 + (size_t)y * fb->pitch;
                for (int x = x0; x < x1; ++x)
                    if (src[x] >> 24) dst[x] = over8(fb->palette, dst[x], src[x], 256, x, y);
            } else {
                u32 *dst = fb->pixels + (size_t)y * fb->pitch;
                for (int x = x0; x < x1; ++x)
                    if (src[x] >> 24) dst[x] = over32(dst[x], src[x], 256);
            }
        }
    }
}
