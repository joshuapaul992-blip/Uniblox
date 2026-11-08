/* Uniblox - renderer front end: queues parts and dispatches to the
 * software or OpenGL backend. */
#include "render/ub_render_internal.h"
#include "render/ub_palette.h"
#include "core/ub_string.h"
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
} F;

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
    if (F.backend == UB_BACKEND_OPENGL) gl_shutdown();
    else sw_shutdown();
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
    frame->text_count = 0;
    frame->text_px = ub_render_line_height() - 2;
}

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
    } else if (F.frame->fb) {
        sw_begin(F.frame->fb, &F.cam, &F.env);
        for (int i = 0; i < F.count; ++i) sw_part(&F.parts[i]);
        sw_end(&F.stats);
    }
}

int ub_render_line_height(void)
{
    if (F.ready && F.backend == UB_BACKEND_OPENGL) return gl_line_height();
    /* software: the OS draws the text; scale it with the window */
    int h = F.frame ? F.frame->height : 480;
    return UB_CLAMP(h / 34, 15, 40);
}

void ub_render_text(int x, int y, const char *utf8, u32 color)
{
    if (!F.ready || !F.frame) return;
    ub_render_end_scene();
    if (F.backend == UB_BACKEND_OPENGL) {
        gl_text(x, y, utf8, color);
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
