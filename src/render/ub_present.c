/* Uniblox - putting only what changed on the screen (see ub_present.h). */
#include "render/ub_present.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

void ub_damage_free(ub_damage *d)
{
    plat_free(d->prev);
    plat_free(d->dirty);
    memset(d, 0, sizeof *d);
}

bool ub_damage_resize(ub_damage *d, int fw, int fh, int ww, int wh)
{
    if (d->prev && d->fw == fw && d->fh == fh && d->ww == ww && d->wh == wh) return false;
    ub_damage_free(d);
    if (fw <= 0 || fh <= 0 || ww <= 0 || wh <= 0) return true;
    d->fw = fw;
    d->fh = fh;
    d->ww = ww;
    d->wh = wh;
    d->tx = (fw + UB_DAMAGE_TILE - 1) / UB_DAMAGE_TILE;
    d->ty = (fh + UB_DAMAGE_TILE - 1) / UB_DAMAGE_TILE;
    d->prev = (u32 *)plat_alloc((size_t)fw * fh * 4);
    d->dirty = (u8 *)plat_alloc((size_t)d->tx * d->ty);
    if (!d->prev || !d->dirty) {
        ub_damage_free(d);
        return true;
    }
    d->all = true;
    return true;
}

void ub_damage_invalidate(ub_damage *d) { d->all = true; }

/* first window pixel showing frame pixel s: ceil(s * w / f) */
int ub_present_win_x(const ub_damage *d, int s) { return (int)(((i64)s * d->ww + d->fw - 1) / d->fw); }
int ub_present_win_y(const ub_damage *d, int s) { return (int)(((i64)s * d->wh + d->fh - 1) / d->fh); }

int ub_damage_update(ub_damage *d, const u32 *pixels, int pitch)
{
    if (!d->prev) return 0;
    const int T = UB_DAMAGE_TILE;
    int n = 0;
    for (int ty = 0; ty < d->ty; ++ty) {
        int y0 = ty * T, y1 = y0 + T < d->fh ? y0 + T : d->fh;
        for (int tx = 0; tx < d->tx; ++tx) {
            int x0 = tx * T, x1 = x0 + T < d->fw ? x0 + T : d->fw;
            size_t bytes = (size_t)(x1 - x0) * 4;
            bool changed = d->all;
            for (int y = y0; y < y1 && !changed; ++y)
                changed = memcmp(pixels + (size_t)y * pitch + x0, d->prev + (size_t)y * d->fw + x0, bytes) != 0;
            if (changed)
                for (int y = y0; y < y1; ++y)
                    memcpy(d->prev + (size_t)y * d->fw + x0, pixels + (size_t)y * pitch + x0, bytes);
            d->dirty[ty * d->tx + tx] = changed;
            n += changed;
        }
    }
    d->all = false;
    d->dirty_count = n;
    return n;
}

void ub_damage_mark_window(ub_damage *d, int x, int y, int w, int h)
{
    if (!d->dirty || w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > d->ww) x1 = d->ww;
    if (y1 > d->wh) y1 = d->wh;
    if (x1 <= x || y1 <= y) return;
    /* window pixels [x, x1) show frame pixels [x*fw/ww, (x1-1)*fw/ww] */
    int fx0 = (int)((i64)x * d->fw / d->ww), fx1 = (int)((i64)(x1 - 1) * d->fw / d->ww);
    int fy0 = (int)((i64)y * d->fh / d->wh), fy1 = (int)((i64)(y1 - 1) * d->fh / d->wh);
    for (int ty = fy0 / UB_DAMAGE_TILE; ty <= fy1 / UB_DAMAGE_TILE && ty < d->ty; ++ty)
        for (int tx = fx0 / UB_DAMAGE_TILE; tx <= fx1 / UB_DAMAGE_TILE && tx < d->tx; ++tx) {
            u8 *t = &d->dirty[ty * d->tx + tx];
            if (!*t) {
                *t = 1;
                d->dirty_count++;
            }
        }
}

int ub_damage_rects(const ub_damage *d, ub_prect *out, int max)
{
    if (!d->dirty) return 0;
    const int T = UB_DAMAGE_TILE;
    int n = 0;
    for (int ty = 0; ty < d->ty; ++ty) {
        int wy0 = ub_present_win_y(d, ty * T);
        int wy1 = ub_present_win_y(d, (ty + 1) * T < d->fh ? (ty + 1) * T : d->fh);
        if (wy1 <= wy0) continue;
        for (int tx = 0; tx < d->tx;) {
            if (!d->dirty[ty * d->tx + tx]) {
                ++tx;
                continue;
            }
            int run = tx;
            while (run < d->tx && d->dirty[ty * d->tx + run]) ++run;
            int wx0 = ub_present_win_x(d, tx * T);
            int wx1 = ub_present_win_x(d, run * T < d->fw ? run * T : d->fw);
            tx = run;
            if (wx1 <= wx0) continue;
            /* the same run in the row above: grow that rectangle down */
            bool merged = false;
            for (int k = n - 1; k >= 0 && out[k].y + out[k].h >= wy0; --k)
                if (out[k].x == wx0 && out[k].w == wx1 - wx0 && out[k].y + out[k].h == wy0) {
                    out[k].h += wy1 - wy0;
                    merged = true;
                    break;
                }
            if (merged) continue;
            if (n == max) return n;
            out[n].x = wx0;
            out[n].y = wy0;
            out[n].w = wx1 - wx0;
            out[n].h = wy1 - wy0;
            ++n;
        }
    }
    return n;
}

/* frame column for each window column, worked out once per size */
static int *g_xmap;
static int g_xmap_fw, g_xmap_ww;

static const int *xmap(int fw, int ww)
{
    if (g_xmap && g_xmap_fw == fw && g_xmap_ww == ww) return g_xmap;
    plat_free(g_xmap);
    g_xmap = (int *)plat_alloc(sizeof(int) * (size_t)ww);
    if (!g_xmap) return NULL;
    for (int x = 0; x < ww; ++x) g_xmap[x] = (int)((i64)x * fw / ww);
    g_xmap_fw = fw;
    g_xmap_ww = ww;
    return g_xmap;
}

void ub_present_stretch(const u32 *src, int fw, int fh, int spitch, u32 *dst, int ww, int wh, int dst_pitch,
                        ub_prect r)
{
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > ww) r.w = ww - r.x;
    if (r.y + r.h > wh) r.h = wh - r.y;
    if (r.w <= 0 || r.h <= 0) return;
    const int *map = fw == ww ? NULL : xmap(fw, ww);
    if (fw != ww && !map) return;
    int last_sy = -1;
    const u32 *last_row = NULL;
    for (int y = r.y; y < r.y + r.h; ++y) {
        int sy = (int)((i64)y * fh / wh);
        u32 *drow = dst + (i64)y * dst_pitch + r.x;
        if (sy == last_sy) { /* the same frame row as the line above: copy it */
            memcpy(drow, last_row, (size_t)r.w * 4);
            continue;
        }
        const u32 *srow = src + (size_t)sy * spitch;
        if (!map) {
            memcpy(drow, srow + r.x, (size_t)r.w * 4);
        } else {
            const int *m = map + r.x;
            for (int x = 0; x < r.w; ++x) drow[x] = srow[m[x]];
        }
        last_sy = sy;
        last_row = drow;
    }
}
