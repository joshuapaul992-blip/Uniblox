/* Uniblox - renderer front end: queues parts and dispatches to the
 * software or OpenGL backend. */
#include "render/ub_render_internal.h"
#include "render/ub_palette.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "crt/ub_crt.h"

#define MAX_QUEUED_PARTS 4096

static struct {
    bool ready;
    ub_backend backend;
    ub_quality quality;
    ub_frame *frame;
    ub_camera cam;
    ub_render_env env;
    ub_part_draw *parts;
    int count;
    ub_render_stats stats;
    u64 t0;
    bool scene_done;
    bool fixed_function;
    ub_part_draw *statics;     /* software backend draws these every frame */
    int static_count;
    int text_px;               /* 0 = follows the window */
    bool text_in_frame;        /* software: draw text into the frame */
    int clip_x0, clip_y0, clip_x1, clip_y1; /* window pixels; x1 <= x0 = off */
} F;

/* Fonts for measuring and for drawing text into a software frame. The same
 * face as the OS draws and the OpenGL atlas, so widths agree. */
typedef struct {
    ub_font_atlas a;
    int px;
    bool tried, ok;
} font_slot;
static font_slot g_font_measure, g_font_draw;

static const ub_font_atlas *get_font(font_slot *f, int px)
{
    if (f->tried && f->px == px) return f->ok ? &f->a : NULL;
    if (f->ok) plat_free(f->a.pixels);
    memset(f, 0, sizeof *f);
    f->tried = true;
    f->px = px;
    f->ok = plat_font_atlas(px, &f->a);
    return f->ok ? &f->a : NULL;
}

static int text_px_now(void) { return ub_render_line_height() - 2; }

void ub_render_set_static(const ub_part_draw *parts, int count)
{
    plat_free(F.statics);
    F.statics = NULL;
    F.static_count = 0;
    if (count > 0) {
        F.statics = (ub_part_draw *)plat_alloc(sizeof(ub_part_draw) * (size_t)count);
        if (F.statics) {
            memcpy(F.statics, parts, sizeof(ub_part_draw) * (size_t)count);
            F.static_count = count;
        }
    }
    if (F.ready && F.backend == UB_BACKEND_OPENGL) gl_set_static(F.statics, F.static_count);
}

void ub_render_force_fixed_function(bool on)
{
    F.fixed_function = on;
}

bool ub_render_init(const ub_frame *frame, char *desc, int desc_len)
{
    F.backend = frame->backend;
    F.quality = UB_QUALITY_HIGH;
    F.ready = false;
    F.parts = (ub_part_draw *)plat_alloc(sizeof(ub_part_draw) * MAX_QUEUED_PARTS);
    if (!F.parts) return false;
    ub_palette_init();
    bool ok;
    if (F.backend == UB_BACKEND_OPENGL) {
        ok = gl_init(desc, desc_len, F.fixed_function);
        if (ok) gl_set_quality(F.quality);
    } else {
        ok = sw_init();
        ub_fmt(desc, desc_len, "software renderer");
    }
    F.ready = ok;
    return ok;
}

void ub_render_shutdown(void)
{
    font_slot *fs[2] = { &g_font_measure, &g_font_draw };
    for (int i = 0; i < 2; ++i) {
        if (fs[i]->ok) plat_free(fs[i]->a.pixels);
        memset(fs[i], 0, sizeof *fs[i]);
    }
    if (F.backend == UB_BACKEND_OPENGL) gl_shutdown();
    else sw_shutdown();
    plat_free(F.statics);
    F.statics = NULL;
    F.static_count = 0;
    plat_free(F.parts);
    F.parts = NULL;
    F.ready = false;
}

ub_backend ub_render_backend(void)
{
    return F.backend;
}

void ub_render_set_quality(ub_quality q)
{
    F.quality = (ub_quality)UB_CLAMP((int)q, 0, UB_QUALITY_COUNT - 1);
    if (F.ready && F.backend == UB_BACKEND_OPENGL) gl_set_quality(F.quality);
}

ub_quality ub_render_quality(void)
{
    return F.quality;
}

const char *ub_render_quality_name(ub_quality q)
{
    static const char *const names[UB_QUALITY_COUNT] = { "low", "medium", "high" };
    return names[UB_CLAMP((int)q, 0, UB_QUALITY_COUNT - 1)];
}

const u32 *ub_render_palette(void)
{
    return ub_palette;
}

void ub_render_begin(ub_frame *frame, const ub_camera *cam, const ub_render_env *env)
{
    F.t0 = plat_time_us();
    memset(&F.stats, 0, sizeof F.stats);
    F.frame = frame;
    F.cam = *cam;
    F.env = *env;
    F.count = 0;
    F.scene_done = false;
    F.clip_x1 = F.clip_x0 = 0;
    frame->text_count = 0;
    frame->text_px = ub_render_line_height() - 2;
}

const u8 ub_see_through_pattern[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

void ub_render_part(const ub_part_draw *p)
{
    if (F.count < MAX_QUEUED_PARTS) F.parts[F.count++] = *p;
}

void ub_render_end_scene(void)
{
    if (!F.ready || F.scene_done) return;
    F.scene_done = true;
    if (F.backend == UB_BACKEND_OPENGL) {
        gl_draw_scene(F.frame->width, F.frame->height, &F.cam, &F.env, F.parts, F.count, &F.stats);
        gl_begin_2d(F.frame->width, F.frame->height);
        if (F.clip_x1 > F.clip_x0) gl_clip(F.clip_x0, F.clip_y0, F.clip_x1 - F.clip_x0, F.clip_y1 - F.clip_y0);
    } else if (F.frame->fb) {
        sw_begin(F.frame->fb, &F.cam, &F.env);
        for (int i = 0; i < F.static_count; ++i) sw_part(&F.statics[i]);
        for (int i = 0; i < F.count; ++i) sw_part(&F.parts[i]);
        sw_end(&F.stats);
    }
}

int ub_render_line_height(void)
{
    if (F.text_px > 0) return F.text_px + 4;
    if (F.ready && F.backend == UB_BACKEND_OPENGL) return gl_line_height();
    /* software: the OS draws the text; scale it with the window */
    int h = F.frame ? F.frame->height : 480;
    return UB_CLAMP(h / 34, 15, 40);
}

/* ------------------------------------------------------------------ 2D */

void ub_render_set_text_px(int px)
{
    F.text_px = px > 0 ? UB_CLAMP(px, 8, 48) : 0;
    gl_set_text_px(F.text_px);
}

void ub_render_set_text_in_frame(bool on) { F.text_in_frame = on; }

int ub_render_text_width_n(const char *utf8, int n)
{
    int px = text_px_now();
    const ub_font_atlas *fa = get_font(&g_font_measure, px);
    int w = 0;
    const char *end = utf8 + n;
    for (const char *p = utf8; *p && p < end;) {
        u32 c = ub_utf8_decode(&p);
        if (c < 32 || c > 127) c = '?';
        w += fa ? fa->advance[c - 32] : (px * 11 + 10) / 20;
    }
    return w;
}

int ub_render_text_width(const char *utf8) { return ub_render_text_width_n(utf8, 0x7FFFFFFF); }

/* window pixels -> frame pixels (the software frame may be smaller) */
static int fb_x(int x) { return (int)((i64)x * F.frame->fb->width / (F.frame->width > 0 ? F.frame->width : 1)); }
static int fb_y(int y) { return (int)((i64)y * F.frame->fb->height / (F.frame->height > 0 ? F.frame->height : 1)); }

/* the clip rectangle in frame pixels */
static void fb_clip(int *x0, int *y0, int *x1, int *y1)
{
    ub_framebuffer *fb = F.frame->fb;
    *x0 = 0;
    *y0 = 0;
    *x1 = fb->width;
    *y1 = fb->height;
    if (F.clip_x1 > F.clip_x0) {
        *x0 = UB_MAX(*x0, fb_x(F.clip_x0));
        *y0 = UB_MAX(*y0, fb_y(F.clip_y0));
        *x1 = UB_MIN(*x1, fb_x(F.clip_x1));
        *y1 = UB_MIN(*y1, fb_y(F.clip_y1));
    }
}

void ub_render_rect(int x, int y, int w, int h, u32 color)
{
    if (!F.ready || !F.frame || w <= 0 || h <= 0) return;
    ub_render_end_scene();
    if (F.backend == UB_BACKEND_OPENGL) {
        gl_rect(x, y, w, h, color);
        return;
    }
    ub_framebuffer *fb = F.frame->fb;
    if (!fb) return;
    int cx0, cy0, cx1, cy1;
    fb_clip(&cx0, &cy0, &cx1, &cy1);
    int x0 = UB_MAX(fb_x(x), cx0), y0 = UB_MAX(fb_y(y), cy0);
    int x1 = UB_MIN(fb_x(x + w), cx1), y1 = UB_MIN(fb_y(y + h), cy1);
    if (x1 <= x0 || y1 <= y0) return;
    if (fb->format == UB_PIXEL_INDEX8) {
        int r = (int)(color >> 16 & 255), g = (int)(color >> 8 & 255), b = (int)(color & 255);
        u8 d[4];
        for (int k = 0; k < 4; ++k) d[k] = ub_palette_match(r, g, b, k);
        for (int yy = y0; yy < y1; ++yy) {
            u8 *row = fb->pixels8 + yy * fb->pitch;
            for (int xx = x0; xx < x1; ++xx) row[xx] = d[((yy & 1) << 1) | (xx & 1)];
        }
    } else {
        u32 c = color & 0xFFFFFF;
        for (int yy = y0; yy < y1; ++yy) {
            u32 *row = fb->pixels + yy * fb->pitch;
            for (int xx = x0; xx < x1; ++xx) row[xx] = c;
        }
    }
}

void ub_render_clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        F.clip_x0 = F.clip_x1 = 0;
    } else {
        F.clip_x0 = x;
        F.clip_y0 = y;
        F.clip_x1 = x + w;
        F.clip_y1 = y + h;
    }
    if (F.ready && F.backend == UB_BACKEND_OPENGL && F.scene_done) gl_clip(x, y, w, h);
}

/* software: one line of text drawn into the frame from the font atlas */
static void sw_text(int x, int y, const char *utf8, u32 color)
{
    ub_framebuffer *fb = F.frame->fb;
    int px = UB_MAX(6, fb_y(text_px_now()));
    const ub_font_atlas *fa = get_font(&g_font_draw, px);
    if (!fa || !fb) return;
    int cx0, cy0, cx1, cy1;
    fb_clip(&cx0, &cy0, &cx1, &cy1);
    int r = (int)(color >> 16 & 255), g = (int)(color >> 8 & 255), b = (int)(color & 255);
    u8 idx = ub_palette_match(r, g, b, 0);
    int ox = fb_x(x), oy = fb_y(y);
    for (const char *p = utf8; *p;) {
        u32 c = ub_utf8_decode(&p);
        if (c < 32 || c > 127) c = '?';
        int i = (int)c - 32;
        int gx = (i % 16) * fa->cell_w + 1, gy = (i / 16) * fa->cell_h + 1;
        int gw = UB_MIN(fa->advance[i] + 1, fa->cell_w - 1);
        for (int yy = 0; yy < fa->glyph_h; ++yy) {
            int ty = oy + yy;
            if (ty < cy0 || ty >= cy1) continue;
            const u8 *src = fa->pixels + (gy + yy) * fa->width + gx;
            for (int xx = 0; xx < gw; ++xx) {
                int tx = ox + xx;
                u32 a = src[xx];
                if (a < 24 || tx < cx0 || tx >= cx1) continue;
                if (fb->format == UB_PIXEL_INDEX8) {
                    if (a > 110) fb->pixels8[ty * fb->pitch + tx] = idx;
                } else {
                    u32 *d = &fb->pixels[ty * fb->pitch + tx];
                    u32 dr = *d >> 16 & 255, dg = *d >> 8 & 255, db = *d & 255;
                    dr += (u32)(((i32)r - (i32)dr) * (i32)a / 255);
                    dg += (u32)(((i32)g - (i32)dg) * (i32)a / 255);
                    db += (u32)(((i32)b - (i32)db) * (i32)a / 255);
                    *d = (dr << 16) | (dg << 8) | db;
                }
            }
        }
        ox += fa->advance[i];
    }
}

void ub_render_screen_ray(const ub_camera *cam, int width, int height, f32 x, f32 y, vec3 *origin, vec3 *dir)
{
    f32 sy = ub_sinf(cam->yaw), cy = ub_cosf(cam->yaw);
    f32 sp = ub_sinf(cam->pitch), cp = ub_cosf(cam->pitch);
    vec3 fwd = v3(sy * cp, sp, cy * cp), right = v3(cy, 0, -sy), up = v3_cross(fwd, right);
    f32 h = (f32)(height > 0 ? height : 1);
    f32 t = ub_sinf(cam->fov_y * 0.5f) / ub_cosf(cam->fov_y * 0.5f);
    f32 nx = (x - (f32)width * 0.5f) / (h * 0.5f) * t, ny = ((f32)height * 0.5f - y) / (h * 0.5f) * t;
    *origin = cam->pos;
    *dir = v3_normalize(v3_add(fwd, v3_add(v3_scale(right, nx), v3_scale(up, ny))));
}

bool ub_render_project(const ub_camera *cam, int width, int height, vec3 p, f32 *sx, f32 *sy_out)
{
    f32 sy = ub_sinf(cam->yaw), cy = ub_cosf(cam->yaw);
    f32 sp = ub_sinf(cam->pitch), cp = ub_cosf(cam->pitch);
    vec3 fwd = v3(sy * cp, sp, cy * cp), right = v3(cy, 0, -sy), up = v3_cross(fwd, right);
    vec3 d = v3_sub(p, cam->pos);
    f32 z = v3_dot(d, fwd);
    if (z < 0.05f) return false;
    f32 h = (f32)(height > 0 ? height : 1);
    f32 t = ub_sinf(cam->fov_y * 0.5f) / ub_cosf(cam->fov_y * 0.5f);
    *sx = (f32)width * 0.5f + v3_dot(d, right) / (z * t) * h * 0.5f;
    *sy_out = (f32)height * 0.5f - v3_dot(d, up) / (z * t) * h * 0.5f;
    return true;
}

void ub_render_text(int x, int y, const char *utf8, u32 color)
{
    if (!F.ready || !F.frame) return;
    ub_render_end_scene();
    if (F.backend == UB_BACKEND_OPENGL) {
        gl_text(x, y, utf8, color);
        return;
    }
    if (F.text_in_frame && F.frame->fb) {
        if (!(color & UB_TEXT_FLAT)) sw_text(x + 1, y + 1, utf8, 0);
        sw_text(x, y, utf8, color);
        return;
    }
    ub_frame *f = F.frame;
    if (f->text_count >= UB_TEXT_QUEUE_MAX) return;
    ub_text_item *t = &f->text[f->text_count++];
    t->x = x;
    t->y = y;
    t->color = color;
    ub_fmt(t->text, sizeof t->text, "%s", utf8);
}

void ub_render_end(ub_render_stats *stats)
{
    ub_render_end_scene();
    u32 backend_us = F.stats.render_us;
    F.stats.render_us = (u32)(plat_time_us() - F.t0);
    (void)backend_us;
    if (stats) *stats = F.stats;
}
