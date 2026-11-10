/* Uniblox - presenting only what changed: partial updates must give
 * exactly the picture a full stretch gives. */
#include <stdlib.h>

#include "render/ub_present.h"
#include "test.h"

static u32 rng = 12345;
static u32 rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

static void case_sizes(int fw, int fh, int ww, int wh, bool bottom_up)
{
    u32 *frame = (u32 *)calloc((size_t)fw * fh, 4);
    u32 *screen = (u32 *)calloc((size_t)ww * wh, 4);
    u32 *want = (u32 *)calloc((size_t)ww * wh, 4);
    ub_damage d;
    memset(&d, 0, sizeof d);
    CHECK(ub_damage_resize(&d, fw, fh, ww, wh));
    CHECK(!ub_damage_resize(&d, fw, fh, ww, wh));
    ub_prect rects[512];
    int bad = 0, sent_px = 0, total_px = 0;
    for (int f = 0; f < 40; ++f) {
        /* change a few small areas (sometimes nothing, sometimes everything) */
        int edits = f == 0 ? 0 : (int)(rnd() % 5);
        if (f % 13 == 7) edits = 1000;
        for (int k = 0; k < edits; ++k) {
            int x0 = (int)(rnd() % (u32)fw), y0 = (int)(rnd() % (u32)fh);
            int w = 1 + (int)(rnd() % 20), h = 1 + (int)(rnd() % 20);
            u32 c = rnd();
            for (int y = y0; y < y0 + h && y < fh; ++y)
                for (int x = x0; x < x0 + w && x < fw; ++x) frame[y * fw + x] = c;
        }
        int dirty = ub_damage_update(&d, frame, fw);
        CHECK(f > 0 || dirty == d.tx * d.ty);
        /* a "text" rectangle in window space must be redrawn too */
        ub_prect text = { (int)(rnd() % (u32)ww), (int)(rnd() % (u32)wh), 37, 15 };
        if (f % 3 == 0) ub_damage_mark_window(&d, text.x, text.y, text.w, text.h);
        int n = ub_damage_rects(&d, rects, 512);
        u32 *dst = bottom_up ? screen + (size_t)(wh - 1) * ww : screen;
        int pitch = bottom_up ? -ww : ww;
        for (int i = 0; i < n; ++i) {
            ub_present_stretch(frame, fw, fh, fw, dst, ww, wh, pitch, rects[i]);
            sent_px += rects[i].w * rects[i].h;
        }
        total_px += ww * wh;
        /* the text rectangle is covered */
        if (f % 3 == 0) {
            for (int y = text.y; y < text.y + text.h && y < wh; ++y)
                for (int x = text.x; x < text.x + text.w && x < ww; ++x) {
                    bool in = false;
                    for (int i = 0; i < n && !in; ++i)
                        in = x >= rects[i].x && x < rects[i].x + rects[i].w && y >= rects[i].y && y < rects[i].y + rects[i].h;
                    if (!in) bad++;
                }
        }
        /* rectangles never overlap and stay inside the window */
        for (int i = 0; i < n; ++i) {
            CHECK(rects[i].x >= 0 && rects[i].y >= 0 && rects[i].x + rects[i].w <= ww && rects[i].y + rects[i].h <= wh);
            for (int j = i + 1; j < n; ++j)
                if (rects[i].x < rects[j].x + rects[j].w && rects[j].x < rects[i].x + rects[i].w &&
                    rects[i].y < rects[j].y + rects[j].h && rects[j].y < rects[i].y + rects[i].h)
                    bad++;
        }
        /* the result is exactly a full stretch */
        ub_prect all = { 0, 0, ww, wh };
        ub_present_stretch(frame, fw, fh, fw, want, ww, wh, ww, all);
        for (int y = 0; y < wh; ++y)
            for (int x = 0; x < ww; ++x) {
                u32 got = bottom_up ? screen[(size_t)(wh - 1 - y) * ww + x] : screen[(size_t)y * ww + x];
                if (got != want[(size_t)y * ww + x]) bad++;
            }
    }
    printf("  %dx%d -> %dx%d%s: sent %d%% of the pixels a full redraw would\n", fw, fh, ww, wh,
           bottom_up ? " (bottom-up)" : "", (int)((i64)sent_px * 100 / total_px));
    CHECK(bad == 0);
    if (bad) printf("    %d wrong pixels or overlaps\n", bad);
    ub_damage_free(&d);
    free(frame);
    free(screen);
    free(want);
}

void test_present(void)
{
    case_sizes(160, 90, 160, 90, false);   /* one to one */
    case_sizes(160, 90, 480, 270, true);   /* 3x, bottom-up like a DIB */
    case_sizes(171, 96, 640, 359, false);  /* uneven scale */
    case_sizes(97, 61, 1000, 600, true);
    /* a still frame sends nothing; an exposure sends everything */
    ub_damage d;
    memset(&d, 0, sizeof d);
    u32 *frame = (u32 *)calloc(64 * 64, 4);
    ub_damage_resize(&d, 64, 64, 128, 128);
    ub_damage_update(&d, frame, 64);
    CHECK(ub_damage_update(&d, frame, 64) == 0);
    ub_prect r[64];
    CHECK(ub_damage_rects(&d, r, 64) == 0);
    ub_damage_invalidate(&d);
    CHECK(ub_damage_update(&d, frame, 64) == 16);
    CHECK(ub_damage_rects(&d, r, 64) == 1 && r[0].w == 128 && r[0].h == 128); /* merged into one */
    ub_damage_free(&d);
    free(frame);
}
