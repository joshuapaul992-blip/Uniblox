/* Uniblox - Milestone 2 demo app: a textured, lit room of parts.
 *
 *   - WASD/arrows move, Space jumps, hold the right mouse button to orbit
 *     the camera (left/right and up/down), wheel zooms
 *   - F5 runs a 10-second benchmark flythrough and logs the result
 *   - F6 switches the window between 1x and 2x size (1x skips stretching)
 *   - F8 switches between 32-bit colour and 8-bit palette rendering
 *   - F3 hides the text overlay; M mutes; Enter or '/' opens a chat line;
 *     left click beeps (panned/pitched by position); Esc quits
 *   - settings (camera, mute, colour mode) are saved to uniblox.cfg
 *
 * There is no collision yet (that is milestone 4): you can walk through
 * walls, and the floor is always at height 0. */
#include "app/app.h"
#include "audio/ub_mixer.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "core/ub_vec.h"
#include "render/ub_render.h"

#define WORLD_LIMIT 140.0f
#define MOVE_SPEED 16.0f
#define TURN_SPEED 1.8f        /* radians/s for arrow keys */
#define LOOK_SENSITIVITY 0.006f
#define PITCH_MIN (-1.35f)
#define PITCH_MAX 0.35f
#define ZOOM_MIN 6.0f
#define ZOOM_MAX 60.0f
#define GRAVITY 60.0f
#define JUMP_SPEED 22.0f
#define CONFIG_PATH "uniblox.cfg"
#define TYPED_MAX 40
#define BENCH_SECONDS 10
#define MAX_PARTS 128

static ub_system_info g_sys;
static ub_part_draw g_parts[MAX_PARTS];
static int g_part_count;
static int g_spinner = -1;            /* index of the animated part */

static f32 g_px, g_pz = 4.0f, g_py, g_vy;   /* player feet position */
static f32 g_facing;                  /* player yaw */
static f32 g_yaw, g_pitch = -0.35f;   /* camera angles around the player */
static f32 g_zoom = 22.0f;            /* camera distance */
static f32 g_time;
static bool g_muted;
static int g_colors_cfg;              /* 0 = automatic, 8 or 32 = chosen with F8 */
static int g_window_scale = 2;
static bool g_hide_overlay;
static char g_typed[TYPED_MAX + 1];
static bool g_typing;
static char g_last_message[TYPED_MAX + 1];
static u32 g_fps_frames, g_fps_accum_us, g_fps_x10;
static u32 g_jumps, g_clicks;
static ub_sound g_snd_jump, g_snd_land, g_snd_click;
static ub_render_stats g_stats;

/* benchmark */
static bool g_bench;
static f32 g_bench_t;
static u32 g_bench_frames;
static u64 g_bench_render_us, g_bench_present_us, g_bench_polys, g_bench_pixels;
static char g_bench_result[UB_OVERLAY_LINE_LEN];

/* --------------------------------------------------------------- scene */

static ub_part_draw *add_part(ub_shape shape, f32 x, f32 y, f32 z, f32 sx, f32 sy, f32 sz,
                              u32 color, ub_material mat)
{
    if (g_part_count >= MAX_PARTS) return &g_parts[MAX_PARTS - 1];
    ub_part_draw *p = &g_parts[g_part_count++];
    p->pos = v3(x, y, z);
    p->size = v3(sx, sy, sz);
    p->rot = mat3_identity();
    p->color = color;
    p->shape = (u8)shape;
    p->material = (u8)mat;
    return p;
}

static void add_tree(f32 x, f32 z, f32 scale)
{
    add_part(UB_SHAPE_CYLINDER, x, 3.0f * scale, z, 1.2f * scale, 6.0f * scale, 1.2f * scale,
             0x6B4423, UB_MAT_WOOD);
    add_part(UB_SHAPE_BALL, x, 7.5f * scale, z, 6.0f * scale, 5.0f * scale, 6.0f * scale,
             0x3A9A3A, UB_MAT_GRASS);
}

/* A walled room (open roof) with a doorway facing the spawn point, and
 * some things outside. Room interior: x -18..18, z 22..58. */
static void build_scene(void)
{
    g_part_count = 0;
    add_part(UB_SHAPE_BOX, 0, -0.5f, 20, 300, 1, 300, 0x4C8C3C, UB_MAT_GRASS);       /* ground */

    /* room */
    add_part(UB_SHAPE_BOX, 0, 0.05f, 40, 36, 0.1f, 36, 0xA0703C, UB_MAT_WOOD);       /* floor */
    add_part(UB_SHAPE_BOX, 0, 6, 58.5f, 38, 12, 1, 0xB04A30, UB_MAT_BRICK);          /* north */
    add_part(UB_SHAPE_BOX, -18.5f, 6, 40, 1, 12, 36, 0xB04A30, UB_MAT_BRICK);        /* west */
    add_part(UB_SHAPE_BOX, 18.5f, 6, 40, 1, 12, 36, 0xB04A30, UB_MAT_BRICK);         /* east */
    add_part(UB_SHAPE_BOX, -11.5f, 6, 21.5f, 15, 12, 1, 0xB04A30, UB_MAT_BRICK);     /* south, left */
    add_part(UB_SHAPE_BOX, 11.5f, 6, 21.5f, 15, 12, 1, 0xB04A30, UB_MAT_BRICK);      /* south, right */
    add_part(UB_SHAPE_BOX, 0, 10, 21.5f, 8, 4, 1, 0xB04A30, UB_MAT_BRICK);           /* lintel */
    for (int i = 0; i < 4; ++i)                                                       /* pillars */
        add_part(UB_SHAPE_CYLINDER, (i & 1) ? 14.0f : -14.0f, 6, (i & 2) ? 54.0f : 26.0f,
                 2.5f, 12, 2.5f, 0x9A9A9A, UB_MAT_STONE);
    for (int i = 0; i < 6; ++i)                                                       /* stairs */
        add_part(UB_SHAPE_BOX, 15, (f32)(i + 1) * 0.5f, 31.0f + (f32)i * 3.0f, 5, (f32)(i + 1), 3,
                 0x8B5A2B, UB_MAT_WOOD);
    add_part(UB_SHAPE_WEDGE, -12, 1.5f, 46, 6, 3, 8, 0x8090A0, UB_MAT_METAL);         /* ramp */
    add_part(UB_SHAPE_BOX, -12, 1.5f, 53, 6, 3, 6, 0x8090A0, UB_MAT_METAL);           /* landing */
    add_part(UB_SHAPE_CYLINDER, 0, 1.5f, 40, 4, 3, 4, 0x7A8590, UB_MAT_METAL);        /* pedestal */
    g_spinner = g_part_count;
    add_part(UB_SHAPE_BOX, 0, 6, 40, 3, 3, 3, 0x2E8BEF, UB_MAT_PLASTIC);              /* spinner */
    add_part(UB_SHAPE_BALL, -6, 1.5f, 30, 3, 3, 3, 0xF2C230, UB_MAT_PLASTIC);
    add_part(UB_SHAPE_BALL, 6, 1.0f, 33, 2, 2, 2, 0x30C060, UB_MAT_PLASTIC);
    add_part(UB_SHAPE_BALL, -4, 1.0f, 50, 2, 2, 2, 0xE04080, UB_MAT_PLASTIC);
    add_part(UB_SHAPE_BOX, 6, 3.0f, 50, 6, 0.4f, 4, 0x9C6B3A, UB_MAT_WOOD);           /* table */
    for (int i = 0; i < 4; ++i)
        add_part(UB_SHAPE_BOX, (i & 1) ? 8.6f : 3.4f, 1.4f, (i & 2) ? 51.6f : 48.4f,
                 0.4f, 2.8f, 0.4f, 0x6E4A26, UB_MAT_WOOD);

    /* outside */
    for (int i = 0; i < 4; ++i)                                                       /* path */
        add_part(UB_SHAPE_BOX, 0, 0.05f, 2.5f + (f32)i * 5.0f, 4, 0.1f, 4, 0x8A8A8A, UB_MAT_STONE);
    add_tree(-30, 8, 1.0f);
    add_tree(32, -6, 1.2f);
    add_tree(-34, 68, 0.9f);
    add_tree(38, 62, 1.1f);
    add_tree(-8, -30, 1.0f);
    static const u32 tower[4] = { 0xD94040, 0xF2B233, 0x3FAE4A, 0x3F6FD9 };
    for (int i = 0; i < 4; ++i) {                                                     /* block tower */
        ub_part_draw *p = add_part(UB_SHAPE_BOX, 28, 1.5f + (f32)i * 3.0f, 20, 3, 3, 3, tower[i],
                                   UB_MAT_PLASTIC);
        p->rot = mat3_from_euler((f32)i * 0.3f, 0, 0);
    }
    add_part(UB_SHAPE_BOX, -40, 0.5f, -20, 20, 1, 20, 0xA3A2A5, UB_MAT_GRID);         /* grid pad */
    add_part(UB_SHAPE_WEDGE, -40, 1.5f, -7, 8, 2, 6, 0xA3A2A5, UB_MAT_GRID);
    ub_info("scene: %d parts", g_part_count);
}

/* ------------------------------------------------------------ sounds */

#define SYNTH_RATE 22050u

static bool synth_alloc(ub_sound *s, f32 seconds)
{
    s->frames = (u32)(seconds * SYNTH_RATE);
    s->rate = SYNTH_RATE;
    s->channels = 1;
    s->samples = (i16 *)plat_alloc(s->frames * sizeof(i16));
    return s->samples != NULL;
}

/* Square wave sweeping upward with a linear fade. */
static void synth_jump(ub_sound *s)
{
    if (!synth_alloc(s, 0.18f)) return;
    f32 phase = 0;
    for (u32 i = 0; i < s->frames; ++i) {
        f32 t = (f32)i / (f32)s->frames;
        f32 freq = 220.0f + 440.0f * t;
        phase += freq / SYNTH_RATE;
        if (phase >= 1.0f) phase -= 1.0f;
        f32 env = 1.0f - t;
        s->samples[i] = (i16)((phase < 0.5f ? 7000.0f : -7000.0f) * env);
    }
}

/* Low-passed noise burst. */
static void synth_land(ub_sound *s)
{
    if (!synth_alloc(s, 0.14f)) return;
    u32 seed = 12345;
    f32 lp = 0;
    for (u32 i = 0; i < s->frames; ++i) {
        seed = seed * 1103515245u + 12345u;
        f32 noise = (f32)(i32)(seed >> 16 & 0x7FFF) - 16384.0f;
        lp += (noise - lp) * 0.15f;
        f32 env = 1.0f - (f32)i / (f32)s->frames;
        s->samples[i] = (i16)ub_clampf(lp * 2.0f * env * env, -32000.0f, 32000.0f);
    }
}

/* Short sine blip (pitch is varied at play time). */
static void synth_click(ub_sound *s)
{
    if (!synth_alloc(s, 0.09f)) return;
    for (u32 i = 0; i < s->frames; ++i) {
        f32 t = (f32)i / SYNTH_RATE;
        f32 env = 1.0f - (f32)i / (f32)s->frames;
        s->samples[i] = (i16)(ub_sinf(UB_TWO_PI * 660.0f * t) * 9000.0f * env);
    }
}

/* Use sounds/<name>.wav if present, otherwise synthesize. */
static void load_sound(ub_sound *s, const char *path, void (*synth)(ub_sound *))
{
    if (plat_file_exists(path)) {
        void *data;
        u32 size;
        if (plat_file_read_all(path, 16u << 20, &data, &size)) {
            ub_wav_result r = ub_wav_parse(data, size, s);
            plat_free(data);
            if (r == UB_WAV_OK) {
                ub_info("loaded %s: %u frames, %u Hz, %u ch", path, s->frames, s->rate, s->channels);
                return;
            }
            ub_warn("%s: %s; using built-in sound", path, ub_wav_result_name(r));
        } else {
            ub_warn("could not read %s; using built-in sound", path);
        }
    }
    synth(s);
}

/* ------------------------------------------------------------ config */

static i32 parse_int(const char *s)
{
    i32 sign = 1, v = 0;
    if (*s == '-') { sign = -1; ++s; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * sign;
}

static bool key_is(const char *line, const char *key, const char **value)
{
    while (*key && *line == *key) { ++line; ++key; }
    if (*key || *line != '=') return false;
    *value = line + 1;
    return true;
}

#define DEG(r) ub_roundf((r) * (180.0f / UB_PI))
#define RAD(d) ((f32)(d) * (UB_PI / 180.0f))

static void load_config(void)
{
    char *text;
    u32 size;
    if (!plat_file_read_all(CONFIG_PATH, 4096, (void **)&text, &size)) return;
    char *line = text;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n' && *end != '\r') ++end;
        char saved = *end;
        *end = 0;
        const char *v;
        if (key_is(line, "zoom", &v)) g_zoom = ub_clampf((f32)parse_int(v), ZOOM_MIN, ZOOM_MAX);
        else if (key_is(line, "yaw_deg", &v)) g_yaw = RAD(parse_int(v));
        else if (key_is(line, "pitch_deg", &v)) g_pitch = ub_clampf(RAD(parse_int(v)), PITCH_MIN, PITCH_MAX);
        else if (key_is(line, "muted", &v)) g_muted = parse_int(v) != 0;
        else if (key_is(line, "window_scale", &v)) g_window_scale = UB_CLAMP(parse_int(v), 1, 2);
        else if (key_is(line, "colors", &v)) {
            i32 c = parse_int(v);
            g_colors_cfg = (c == 8 || c == 32) ? c : 0;
        }
        *end = saved;
        line = end;
        while (*line == '\n' || *line == '\r') ++line;
    }
    plat_free(text);
    ub_info("loaded %s: zoom %d, yaw %d deg, pitch %d deg, muted %d, colors %d", CONFIG_PATH,
            (i32)g_zoom, DEG(g_yaw), DEG(g_pitch), g_muted, g_colors_cfg);
}

static void save_config(void)
{
    char text[320];
    int n = ub_fmt(text, sizeof text,
                   "# Uniblox settings (written on exit)\r\n"
                   "zoom=%d\r\nyaw_deg=%d\r\npitch_deg=%d\r\nmuted=%d\r\n"
                   "# colors: 0 = automatic, 8 = 256-colour palette, 32 = full colour\r\n"
                   "colors=%d\r\n"
                   "# window_scale: 1 = 320x240 (fastest), 2 = 640x480\r\n"
                   "window_scale=%d\r\n",
                   (i32)g_zoom, DEG(ub_wrap_angle(g_yaw)), DEG(g_pitch), g_muted, g_colors_cfg,
                   g_window_scale);
    if (plat_file_write_all(CONFIG_PATH, text, (u32)n)) ub_info("saved %s", CONFIG_PATH);
    else ub_warn("could not save %s", CONFIG_PATH);
}

/* ------------------------------------------------------------ app API */

bool app_init(const ub_system_info *sys)
{
    g_sys = *sys;
    u32 ram_mb = (u32)(sys->total_phys_bytes >> 20);
    if (ram_mb + 8 < UB_RAM_MIN_MB)
        ub_warn("only %u MB RAM; Uniblox needs %u MB (%u MB recommended)", ram_mb,
                UB_RAM_MIN_MB, UB_RAM_REC_MB);
    if (!ub_render_init()) return false;
    load_config();
    if (g_colors_cfg == 8) plat_set_pixel_format(UB_PIXEL_INDEX8);
    if (g_colors_cfg == 32) plat_set_pixel_format(UB_PIXEL_XRGB32);
    if (g_window_scale != 2) plat_set_window_scale(g_window_scale);
    build_scene();
    load_sound(&g_snd_jump, "sounds/jump.wav", synth_jump);
    load_sound(&g_snd_land, "sounds/land.wav", synth_land);
    load_sound(&g_snd_click, "sounds/click.wav", synth_click);
    ub_mixer_set_master_volume(g_muted ? 0.0f : 0.8f);
    ub_info("demo ready");
    return true;
}

void app_shutdown(void)
{
    save_config();
    ub_sound_free(&g_snd_jump);
    ub_sound_free(&g_snd_land);
    ub_sound_free(&g_snd_click);
    ub_render_shutdown();
    ub_info("demo: %u jumps, %u clicks", g_jumps, g_clicks);
}

/* Chat-style text entry: Enter or '/' opens it, Enter sends, Escape
 * cancels. While it is open, keys type text instead of moving. Works on
 * the ordered character stream so fast typing is never reordered. */
static void handle_typing(const ub_input *in)
{
    for (const char *p = in->text; *p;) {
        const char *start = p;
        u32 c = ub_utf8_decode(&p);
        int len = (int)ub_strlen(g_typed);
        if (!g_typing) {
            if (c == '\r' || c == '/') {
                g_typing = true;
                g_typed[0] = 0; /* the '/' that opened it is not part of the text */
            }
            continue; /* anything else is a gameplay key */
        }
        if (c == 0x1B) {
            g_typing = false;
        } else if (c == '\b') {
            if (len > 0) {
                do { --len; } while (len > 0 && ((u8)g_typed[len] & 0xC0) == 0x80);
                g_typed[len] = 0;
            }
        } else if (c == '\r') {
            if (len > 0) {
                ub_info("typed: %s", g_typed);
                for (int i = 0; i <= len; ++i) g_last_message[i] = g_typed[i];
            }
            g_typed[0] = 0;
            g_typing = false;
        } else if (c >= 32) {
            int n = (int)(p - start);
            if (len + n <= TYPED_MAX) {
                for (int i = 0; i < n; ++i) g_typed[len + i] = start[i];
                g_typed[len + n] = 0;
            }
        }
    }
}

static void start_benchmark(void)
{
    g_bench = true;
    g_bench_t = 0;
    g_bench_frames = 0;
    g_bench_render_us = g_bench_present_us = g_bench_polys = g_bench_pixels = 0;
    ub_info("benchmark: started (%d s flythrough)", BENCH_SECONDS);
}

static void finish_benchmark(ub_framebuffer *fb, bool cancelled)
{
    g_bench = false;
    if (cancelled || g_bench_frames == 0) {
        ub_info("benchmark: cancelled");
        ub_fmt(g_bench_result, sizeof g_bench_result, "Benchmark cancelled");
        return;
    }
    u32 fps10 = (u32)((f32)g_bench_frames * 10.0f / g_bench_t);
    u32 ms10 = (u32)(g_bench_render_us / g_bench_frames / 100u);
    u32 pms10 = (u32)(g_bench_present_us / g_bench_frames / 100u);
    const char *mode = fb->format == UB_PIXEL_INDEX8 ? "8-bit" : "32-bit";
    ub_fmt(g_bench_result, sizeof g_bench_result,
           "Benchmark: %u.%u fps (render %u.%u ms, display %u.%u ms, %s, %dx)",
           fps10 / 10, fps10 % 10, ms10 / 10, ms10 % 10, pms10 / 10, pms10 % 10, mode,
           fb->window_scale);
    ub_info("benchmark: %u frames in %u ms = %u.%u fps; render %u.%u ms/frame, display %u.%u ms/frame; "
            "%u polys, %u pixels per frame; %dx%d %s, window %dx; %s; %s; %d bpp desktop",
            g_bench_frames, (u32)(g_bench_t * 1000.0f), fps10 / 10, fps10 % 10, ms10 / 10, ms10 % 10,
            pms10 / 10, pms10 % 10,
            (u32)(g_bench_polys / g_bench_frames), (u32)(g_bench_pixels / g_bench_frames),
            fb->width, fb->height, mode, fb->window_scale, g_sys.cpu_desc, g_sys.os_name,
            g_sys.display_bpp);
}

void app_frame(ub_framebuffer *fb, const ub_input *in, u32 dt_us, ub_overlay *ov)
{
    if (!fb->palette) {
        fb->palette = ub_render_palette();
        fb->palette_serial++;
    }
    u32 real_dt_us = dt_us;
    if (dt_us > 100000) dt_us = 100000; /* clamp after stalls/debugger */
    const f32 dt = (f32)dt_us * 1e-6f;
    g_time += dt;

    /* --- text entry first: while it is open, keys do not move us --- */
    bool was_typing = g_typing;
    if (!g_bench) handle_typing(in);
    bool play_keys = !was_typing && !g_typing && !g_bench;

    /* --- function keys --- */
    if (play_keys && in->key_pressed[UB_KEY_F3]) g_hide_overlay = !g_hide_overlay;
    if (play_keys && in->key_pressed[UB_KEY_F5]) start_benchmark();
    else if (g_bench && (in->key_pressed[UB_KEY_ESCAPE] || in->key_pressed[UB_KEY_F5])) {
        finish_benchmark(fb, true);
        play_keys = false; /* this Escape only cancels */
    }
    if (play_keys && in->key_pressed[UB_KEY_F6]) {
        g_window_scale = fb->window_scale == 1 ? 2 : 1;
        plat_set_window_scale(g_window_scale);
    }
    if (play_keys && in->key_pressed[UB_KEY_F8]) {
        ub_pixel_format next = fb->format == UB_PIXEL_INDEX8 ? UB_PIXEL_XRGB32 : UB_PIXEL_INDEX8;
        g_colors_cfg = next == UB_PIXEL_INDEX8 ? 8 : 32;
        plat_set_pixel_format(next);
    }

    /* --- camera controls --- */
    bool want_look = in->mouse_down[UB_MOUSE_RIGHT] && in->has_focus && !g_bench;
    if (want_look != in->mouse_captured) ub_debug("mouse look %s", want_look ? "on" : "off");
    plat_set_mouse_capture(want_look);
    g_yaw += (f32)in->mouse_dx * LOOK_SENSITIVITY;
    g_pitch = ub_clampf(g_pitch - (f32)in->mouse_dy * LOOK_SENSITIVITY, PITCH_MIN, PITCH_MAX);
    if (play_keys && in->key_down[UB_KEY_LEFT]) g_yaw -= TURN_SPEED * dt;
    if (play_keys && in->key_down[UB_KEY_RIGHT]) g_yaw += TURN_SPEED * dt;
    g_yaw = ub_wrap_angle(g_yaw);
    for (int n = in->wheel; n >= UB_WHEEL_NOTCH; n -= UB_WHEEL_NOTCH) g_zoom *= 0.88f;
    for (int n = in->wheel; n <= -UB_WHEEL_NOTCH; n += UB_WHEEL_NOTCH) g_zoom /= 0.88f;
    g_zoom = ub_clampf(g_zoom, ZOOM_MIN, ZOOM_MAX);

    f32 fx = ub_sinf(g_yaw), fz = ub_cosf(g_yaw);
    f32 rx = fz, rz = -fx;

    /* --- movement, relative to the camera --- */
    f32 mf = 0, mr = 0;
    if (play_keys) {
        if (in->key_down['W'] || in->key_down[UB_KEY_UP]) mf += 1;
        if (in->key_down['S'] || in->key_down[UB_KEY_DOWN]) mf -= 1;
        if (in->key_down['D']) mr += 1;
        if (in->key_down['A']) mr -= 1;
    }
    if (mf != 0 && mr != 0) { mf *= 0.7071f; mr *= 0.7071f; }
    f32 mx = fx * mf + rx * mr, mz = fz * mf + rz * mr;
    if (mf != 0 || mr != 0) {
        /* turn the character smoothly towards where it is going
         * (yaw of direction (x, z) is atan2(x, z): yaw 0 faces +Z) */
        f32 want = ub_atan2f(mx, mz);
        f32 diff = ub_wrap_angle(want - g_facing);
        g_facing = ub_wrap_angle(g_facing + diff * ub_clampf(dt * 12.0f, 0, 1));
    }
    g_px = ub_clampf(g_px + mx * MOVE_SPEED * dt, -WORLD_LIMIT, WORLD_LIMIT);
    g_pz = ub_clampf(g_pz + mz * MOVE_SPEED * dt, -WORLD_LIMIT, WORLD_LIMIT);

    bool on_ground = g_py <= 0.0f;
    if (play_keys && in->key_down[UB_KEY_SPACE] && on_ground) {
        g_vy = JUMP_SPEED;
        g_jumps++;
        ub_debug("jump %u", g_jumps);
        ub_mixer_play(&g_snd_jump, ub_play_default());
    }
    g_vy -= GRAVITY * dt;
    g_py += g_vy * dt;
    if (g_py < 0.0f) {
        if (!on_ground) {
            ub_play_params p = ub_play_default();
            p.volume = ub_clampf(-g_vy / JUMP_SPEED, 0.2f, 1.0f);
            ub_mixer_play(&g_snd_land, p);
        }
        g_py = 0.0f;
        g_vy = 0.0f;
    }

    /* --- other input --- */
    if (play_keys && in->key_pressed['M']) {
        g_muted = !g_muted;
        ub_mixer_set_master_volume(g_muted ? 0.0f : 0.8f);
        ub_debug("mute %s", g_muted ? "on" : "off");
    }
    if (!g_bench && in->mouse_pressed[UB_MOUSE_LEFT]) {
        ub_play_params p = ub_play_default();
        p.pan = (f32)(in->mouse_x * 2 - fb->width) / (f32)fb->width;
        p.pitch = 2.0f - 1.5f * (f32)in->mouse_y / (f32)fb->height;
        ub_mixer_play(&g_snd_click, p);
        g_clicks++;
        ub_debug("click %u at %d,%d", g_clicks, in->mouse_x, in->mouse_y);
    }
    if (play_keys && in->key_pressed[UB_KEY_ESCAPE]) {
        ub_info("escape pressed, quitting");
        plat_request_quit();
    }

    /* --- animate --- */
    if (g_spinner >= 0)
        g_parts[g_spinner].rot = mat3_from_euler(g_time * 0.9f, g_time * 0.5f, g_time * 0.3f);

    /* --- camera --- */
    ub_camera cam;
    vec3 target;
    if (g_bench) {
        /* fixed flythrough: orbit the room, then swoop through the door */
        g_bench_t += (f32)real_dt_us * 1e-6f;
        f32 t = g_bench_t / (f32)BENCH_SECONDS;
        f32 a = t * UB_TWO_PI;
        target = v3(0, 3, 40);
        cam.yaw = a + UB_PI;
        cam.pitch = -0.3f - 0.2f * ub_sinf(a * 2.0f);
        f32 dist = 30.0f - 12.0f * ub_sinf(a);
        cam.pos = v3(target.x + ub_sinf(a) * dist, 8.0f + 4.0f * ub_sinf(a * 2.0f),
                     target.z + ub_cosf(a) * dist);
        cam.yaw = ub_wrap_angle(cam.yaw);
    } else {
        target = v3(g_px, g_py + 2.5f, g_pz);
        cam.yaw = g_yaw;
        cam.pitch = g_pitch;
        f32 cp = ub_cosf(g_pitch);
        vec3 fwd = v3(fx * cp, ub_sinf(g_pitch), fz * cp);
        cam.pos = v3_sub(target, v3_scale(fwd, g_zoom));
        if (cam.pos.y < 0.5f) cam.pos.y = 0.5f; /* stay above the ground */
    }
    cam.fov_y = 70.0f * UB_PI / 180.0f;
    cam.far_dist = 400.0f;

    /* --- render --- */
    ub_render_env env = ub_render_default_env();
    ub_render_begin(fb, &cam, &env);
    for (int i = 0; i < g_part_count; ++i) ub_render_part(&g_parts[i]);
    if (!g_bench) {
        ub_part_draw me;
        me.pos = v3(g_px, g_py + 1.5f, g_pz);
        me.rot = mat3_from_euler(g_facing, 0, 0);
        me.size = v3(2, 3, 1.2f);
        me.color = 0xE04A3C;
        me.shape = UB_SHAPE_BOX;
        me.material = UB_MAT_PLASTIC;
        ub_render_part(&me);
        ub_part_draw head = me;
        head.pos = v3(g_px, g_py + 3.6f, g_pz);
        head.size = v3(1.2f, 1.2f, 1.2f);
        head.color = 0xF2C9A0;
        head.shape = UB_SHAPE_BALL;
        ub_render_part(&head);
    }
    ub_render_end(&g_stats);

    if (g_bench) {
        g_bench_frames++;
        g_bench_render_us += g_stats.render_us;
        g_bench_present_us += fb->present_us;
        g_bench_polys += g_stats.polys_drawn;
        g_bench_pixels += g_stats.pixels_tested;
        if (g_bench_t >= (f32)BENCH_SECONDS) finish_benchmark(fb, false);
    }

    /* --- overlay --- */
    g_fps_frames++;
    g_fps_accum_us += real_dt_us;
    if (g_fps_accum_us >= 500000) {
        g_fps_x10 = (u32)((u64)g_fps_frames * 10000000u / g_fps_accum_us);
        g_fps_frames = 0;
        g_fps_accum_us = 0;
    }
    ov->count = 0;
    if (g_hide_overlay && !g_bench) return;
#define LINE(...) ub_fmt(ov->lines[ov->count++], UB_OVERLAY_LINE_LEN, __VA_ARGS__)
    u32 rms10 = g_stats.render_us / 100u, pms10 = fb->present_us / 100u;
    LINE("Uniblox milestone 2 - %s", g_sys.os_name);
    LINE("%s", g_sys.cpu_desc);
    LINE("%u.%u fps  render %u.%u ms  display %u.%u ms  %u polys  %s %dx", g_fps_x10 / 10,
         g_fps_x10 % 10, rms10 / 10, rms10 % 10, pms10 / 10, pms10 % 10, g_stats.polys_drawn,
         fb->format == UB_PIXEL_INDEX8 ? "8-bit" : "32-bit", fb->window_scale);
    if (g_bench) {
        LINE("BENCHMARK running: %u s left (Esc cancels)",
             (u32)((f32)BENCH_SECONDS - g_bench_t) + 1u);
    } else {
        LINE("Audio: %s%s   display %d bpp", g_sys.audio_desc, g_muted ? " (muted)" : "",
             g_sys.display_bpp);
        if (g_bench_result[0]) LINE("%s", g_bench_result);
        if (g_typing) LINE("Say: %s_   (Enter send, Esc cancel)", g_typed);
        else if (g_last_message[0]) LINE("Last message: %s", g_last_message);
        LINE("WASD move, Space jump, right mouse look, wheel zoom");
        LINE("F5 benchmark, F6 size, F8 colours, F3 hide text, Esc quit");
    }
#undef LINE
}
