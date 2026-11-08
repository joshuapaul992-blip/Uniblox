/* Uniblox - Milestone 0 "hello" app.
 *
 * Purpose: prove the toolchain, PE headers, entry point, message loop,
 * timing, input and framebuffer blit all work on every target OS, from
 * Windows NT 3.51 up. It draws a sky, a scrolling perspective baseplate and
 * a block you can move (arrows) and jump (space). Esc quits.
 *
 * Integer maths only for the floor so it is cheap on a Pentium. */
#include "app/app.h"
#include "core/ub_string.h"

static ub_system_info g_sys;
static u32 g_time_ms;
static u32 g_fps_frames, g_fps_accum_us, g_fps_x10;

/* Player block, in 16.16 fixed point world units. */
static i32 g_px, g_pz;      /* ground position */
static i32 g_py, g_vy;      /* height above ground, vertical speed */

#define FX(x) ((i32)((x) * 65536))

bool app_init(const ub_system_info *sys)
{
    g_sys = *sys;
    g_px = 0;
    g_pz = FX(40);
    plat_log("Uniblox: app_init on %s, %u MB RAM, %d bpp\n",
             sys->os_name, (u32)(sys->total_phys_bytes >> 20), sys->display_bpp);
    return true;
}

void app_shutdown(void)
{
}

static inline u32 rgb(u32 r, u32 g, u32 b)
{
    return (r << 16) | (g << 8) | b;
}

static void fill_rect(ub_framebuffer *fb, int x0, int y0, int x1, int y1, u32 c)
{
    x0 = UB_CLAMP(x0, 0, fb->width);
    x1 = UB_CLAMP(x1, 0, fb->width);
    y0 = UB_CLAMP(y0, 0, fb->height);
    y1 = UB_CLAMP(y1, 0, fb->height);
    for (int y = y0; y < y1; ++y) {
        u32 *row = fb->pixels + y * fb->pitch;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

void app_frame(ub_framebuffer *fb, const ub_input *in, u32 dt_us, ub_overlay *ov)
{
    if (dt_us > 100000) dt_us = 100000; /* clamp after stalls/debugger */
    g_time_ms += dt_us / 1000;

    /* --- simulate --- */
    i32 speed = (i32)((i64)FX(30) * dt_us / 1000000); /* 30 units/s */
    if (in->key_down[UB_KEY_LEFT])  g_px -= speed;
    if (in->key_down[UB_KEY_RIGHT]) g_px += speed;
    if (in->key_down[UB_KEY_UP])    g_pz += speed;
    if (in->key_down[UB_KEY_DOWN])  g_pz -= speed;
    g_pz = UB_CLAMP(g_pz, FX(12), FX(200));
    if (in->key_down[UB_KEY_SPACE] && g_py == 0) g_vy = FX(40);
    g_vy -= (i32)((i64)FX(100) * dt_us / 1000000);  /* gravity */
    g_py += (i32)((i64)g_vy * dt_us / 1000000);
    if (g_py < 0) { g_py = 0; g_vy = 0; }

    /* --- render --- */
    const int w = fb->width, h = fb->height;
    const int horizon = h / 3;
    const int focal = 160;
    const i32 cam_x = g_px;                  /* camera follows the block */
    const i32 cam_z = g_pz - FX(30);
    const i32 cam_h = FX(12);

    for (int y = 0; y < horizon; ++y) {      /* sky gradient */
        u32 t = (u32)(y * 255 / horizon);
        u32 c = rgb(90 + t / 4, 150 + t / 5, 255);
        u32 *row = fb->pixels + y * fb->pitch;
        for (int x = 0; x < w; ++x) row[x] = c;
    }

    for (int y = horizon; y < h; ++y) {      /* perspective baseplate */
        int dy = y - horizon + 1;
        i32 depth = (i32)(((i64)cam_h * focal) / dy);        /* 16.16 */
        i32 wz = cam_z + depth;
        i32 step = depth / focal;                            /* per pixel */
        i32 wx = cam_x - step * (w / 2);
        u32 fog = (u32)UB_CLAMP(255 - (depth >> 17), 40, 255);
        u32 light = rgb(fog * 3 / 10, fog * 7 / 10, fog * 3 / 10);
        u32 dark  = rgb(fog * 2 / 10, fog * 5 / 10, fog * 2 / 10);
        u32 *row = fb->pixels + y * fb->pitch;
        for (int x = 0; x < w; ++x) {
            row[x] = (((wx >> 19) ^ (wz >> 19)) & 1) ? light : dark;
            wx += step;
        }
    }

    /* Block: project its ground point, draw a shadow then the block. */
    {
        i32 rel_z = g_pz - cam_z;
        int sx = w / 2 + (int)(((i64)(g_px - cam_x) * focal) / rel_z);
        int sy_ground = horizon - 1 + (int)(((i64)cam_h * focal) / rel_z);
        int size = (int)(((i64)FX(8) * focal) / rel_z);
        int lift = (int)(((i64)g_py * focal) / rel_z);
        fill_rect(fb, sx - size / 2, sy_ground - size / 6,
                  sx + size / 2, sy_ground + size / 6, rgb(30, 60, 30));
        int top = sy_ground - lift - size;
        fill_rect(fb, sx - size / 2, top, sx + size / 2, top + size,
                  rgb(230, 70, 60));
        fill_rect(fb, sx - size / 2, top, sx + size / 2, top + size / 5,
                  rgb(255, 120, 100)); /* lit top face */
    }

    /* --- overlay text --- */
    g_fps_frames++;
    g_fps_accum_us += dt_us;
    if (g_fps_accum_us >= 500000) {
        g_fps_x10 = (u32)((u64)g_fps_frames * 10000000u / g_fps_accum_us);
        g_fps_frames = 0;
        g_fps_accum_us = 0;
    }
    u32 ram_mb = (u32)(g_sys.total_phys_bytes >> 20);
    ov->count = 0;
    ub_fmt(ov->lines[ov->count++], UB_OVERLAY_LINE_LEN,
           "Uniblox milestone 0 - %s", g_sys.os_name);
    ub_fmt(ov->lines[ov->count++], UB_OVERLAY_LINE_LEN,
           "%u.%u fps  %dx%d  display %d bpp",
           g_fps_x10 / 10, g_fps_x10 % 10, w, h, g_sys.display_bpp);
    ub_fmt(ov->lines[ov->count++], UB_OVERLAY_LINE_LEN,
           "RAM %u MB%s", ram_mb,
           ram_mb < UB_RAM_MIN_MB - 8 ? "  (below 384 MB minimum!)" :
           ram_mb < UB_RAM_REC_MB - 8 ? "  (512 MB recommended)" : "");
    ub_fmt(ov->lines[ov->count++], UB_OVERLAY_LINE_LEN,
           "Arrows move, Space jumps, Esc quits");
}
