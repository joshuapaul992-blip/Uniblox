/* Uniblox - Milestone 1 demo app.
 *
 * Exercises the platform layer on every target OS:
 *   - keyboard: WASD/arrows move, Space jumps, M mutes, Esc quits;
 *     Enter or '/' opens a chat-style text box (tests WM_CHAR)
 *   - mouse: hold the right button to orbit the camera (mouse-look),
 *     wheel zooms (NT 4.0+), left click plays a tone panned and pitched by
 *     where you click
 *   - audio: synthesized jump/land/click sounds, or sounds/jump.wav,
 *     sounds/land.wav and sounds/click.wav if they exist next to the .exe
 *   - files: camera zoom/angle and mute are saved to uniblox.cfg on exit
 *     and loaded on start; everything is logged to uniblox.log
 *
 * The scene is a "mode 7" style floor plus screen-aligned blocks: just
 * enough 3D to check input and timing. The real renderer is milestone 2. */
#include "app/app.h"
#include "audio/ub_mixer.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"

#define WORLD_LIMIT 1000.0f
#define FOCAL 160.0f
#define MOVE_SPEED 30.0f
#define TURN_SPEED 1.8f       /* radians/s for arrow keys */
#define LOOK_SENSITIVITY 0.008f
#define ZOOM_MIN 14.0f
#define ZOOM_MAX 90.0f
#define GRAVITY 100.0f
#define JUMP_SPEED 40.0f
#define CONFIG_PATH "uniblox.cfg"
#define TYPED_MAX 40

typedef struct {
    f32 x, z, size, height;
    u32 color;
} part;

static const part k_parts[] = {
    { 30, 60, 10, 0, 0x3070E0 },
    { -40, 90, 14, 0, 0xE0C030 },
    { 60, -30, 8, 0, 0xB050D0 },
    { -70, -50, 12, 0, 0x30C0C0 },
    { 0, 140, 20, 0, 0xF09030 },
    { -20, -110, 10, 0, 0xE0E0E0 },
};

static ub_system_info g_sys;
static f32 g_px, g_pz, g_py, g_vy;   /* player block */
static f32 g_yaw;                    /* camera angle around the player */
static f32 g_zoom = 34.0f;           /* camera distance */
static bool g_muted;
static char g_typed[TYPED_MAX + 1];
static bool g_typing;                /* chat-style text entry is open */
static char g_last_message[TYPED_MAX + 1];
static u32 g_fps_frames, g_fps_accum_us, g_fps_x10;
static u32 g_jumps, g_clicks;
static ub_sound g_snd_jump, g_snd_land, g_snd_click;

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
        else if (key_is(line, "yaw_deg", &v)) g_yaw = (f32)parse_int(v) * (UB_PI / 180.0f);
        else if (key_is(line, "muted", &v)) g_muted = parse_int(v) != 0;
        *end = saved;
        line = end;
        while (*line == '\n' || *line == '\r') ++line;
    }
    plat_free(text);
    ub_info("loaded %s: zoom %d, yaw %d deg, muted %d", CONFIG_PATH, (i32)g_zoom,
            ub_roundf(g_yaw * (180.0f / UB_PI)), g_muted);
}

static void save_config(void)
{
    char text[256];
    int n = ub_fmt(text, sizeof text,
                   "# Uniblox settings (written on exit)\r\nzoom=%d\r\nyaw_deg=%d\r\nmuted=%d\r\n",
                   (i32)g_zoom, ub_roundf(ub_wrap_angle(g_yaw) * (180.0f / UB_PI)), g_muted);
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
    load_config();
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
    ub_info("demo: %u jumps, %u clicks", g_jumps, g_clicks);
}

static inline u32 rgb(u32 r, u32 g, u32 b)
{
    return (r << 16) | (g << 8) | b;
}

static u32 shade(u32 c, u32 num, u32 den)
{
    u32 r = ((c >> 16) & 0xFF) * num / den, g = ((c >> 8) & 0xFF) * num / den,
        b = (c & 0xFF) * num / den;
    return rgb(UB_MIN(r, 255u), UB_MIN(g, 255u), UB_MIN(b, 255u));
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

typedef struct {
    f32 cam_x, cam_z, cam_h;
    f32 fx, fz, rx, rz; /* forward and right unit vectors on the ground */
    int horizon;
} view;

typedef struct {
    f32 depth;
    int sx, sy_ground, size, lift;
    u32 color;
    bool shadow;
} sprite;

static bool project(const view *v, ub_framebuffer *fb, f32 x, f32 z, f32 height,
                    f32 size, u32 color, sprite *out)
{
    f32 dx = x - v->cam_x, dz = z - v->cam_z;
    f32 cz = dx * v->fx + dz * v->fz;
    if (cz < 2.0f) return false;
    f32 cx = dx * v->rx + dz * v->rz;
    f32 k = FOCAL / cz;
    out->depth = cz;
    out->sx = fb->width / 2 + ub_roundf(cx * k);
    out->sy_ground = v->horizon - 1 + ub_roundf(v->cam_h * k);
    out->size = ub_roundf(size * k);
    out->lift = ub_roundf(height * k);
    out->color = color;
    out->shadow = true;
    return true;
}

static void draw_sprite(ub_framebuffer *fb, const sprite *s)
{
    int half = s->size / 2;
    if (s->shadow)
        fill_rect(fb, s->sx - half, s->sy_ground - s->size / 6, s->sx + half,
                  s->sy_ground + s->size / 6, rgb(30, 60, 30));
    int top = s->sy_ground - s->lift - s->size;
    fill_rect(fb, s->sx - half, top, s->sx + half, top + s->size, s->color);
    fill_rect(fb, s->sx - half, top, s->sx + half, top + s->size / 5,
              shade(s->color, 5, 4)); /* lit top face */
    fill_rect(fb, s->sx + half - s->size / 6, top + s->size / 5, s->sx + half,
              top + s->size, shade(s->color, 3, 4)); /* shaded side */
}

static void draw_floor(ub_framebuffer *fb, const view *v)
{
    const int w = fb->width, h = fb->height;
    for (int y = 0; y < v->horizon; ++y) {      /* sky gradient */
        u32 t = (u32)(y * 255 / v->horizon);
        u32 c = rgb(90 + t / 4, 150 + t / 5, 255);
        u32 *row = fb->pixels + y * fb->pitch;
        for (int x = 0; x < w; ++x) row[x] = c;
    }
    /* Each row of floor is a straight line in the world: compute its two
     * ends in floating point, then step across it in 16.16 fixed point. */
    for (int y = v->horizon; y < h; ++y) {
        f32 depth = v->cam_h * FOCAL / (f32)(y - v->horizon + 1);
        f32 cx = v->cam_x + v->fx * depth, cz = v->cam_z + v->fz * depth;
        f32 px = depth / FOCAL;
        f32 lx = cx - v->rx * px * (f32)(w / 2), lz = cz - v->rz * px * (f32)(w / 2);
        i32 wx = (i32)(lx * 65536.0f), wz = (i32)(lz * 65536.0f);
        i32 sx = (i32)(v->rx * px * 65536.0f), sz = (i32)(v->rz * px * 65536.0f);
        u32 fog = (u32)UB_CLAMP(255 - (i32)(depth * 0.5f), 40, 255);
        u32 light = rgb(fog * 3 / 10, fog * 7 / 10, fog * 3 / 10);
        u32 dark = rgb(fog * 2 / 10, fog * 5 / 10, fog * 2 / 10);
        u32 *row = fb->pixels + y * fb->pitch;
        for (int x = 0; x < w; ++x) {
            row[x] = (((wx >> 19) ^ (wz >> 19)) & 1) ? light : dark;
            wx += sx;
            wz += sz;
        }
    }
}

/* Chat-style text entry: Enter or '/' opens it, Enter sends, Escape
 * cancels. While it is open, keys type text instead of moving. */
static void handle_typing(const ub_input *in)
{
    if (!g_typing) {
        if (in->key_pressed[UB_KEY_ENTER] || in->text[0] == '/') {
            g_typing = true;
            g_typed[0] = 0; /* the '/' that opened it is not part of the text */
        }
        return;
    }
    int len = (int)ub_strlen(g_typed);
    if (in->key_pressed[UB_KEY_ESCAPE]) {
        g_typing = false;
        return;
    }
    if (in->key_pressed[UB_KEY_BACKSPACE] && len > 0) {
        /* remove one whole UTF-8 character */
        do { --len; } while (len > 0 && ((u8)g_typed[len] & 0xC0) == 0x80);
        g_typed[len] = 0;
    }
    for (const char *p = in->text; *p;) {
        const char *start = p;
        ub_utf8_decode(&p);
        int n = (int)(p - start);
        if (len + n > TYPED_MAX) break;
        for (int i = 0; i < n; ++i) g_typed[len++] = start[i];
        g_typed[len] = 0;
    }
    if (in->key_pressed[UB_KEY_ENTER]) {
        if (len > 0) {
            ub_info("typed: %s", g_typed);
            for (int i = 0; i <= len; ++i) g_last_message[i] = g_typed[i];
        }
        g_typed[0] = 0;
        g_typing = false;
    }
}

void app_frame(ub_framebuffer *fb, const ub_input *in, u32 dt_us, ub_overlay *ov)
{
    if (dt_us > 100000) dt_us = 100000; /* clamp after stalls/debugger */
    const f32 dt = (f32)dt_us * 1e-6f;

    /* --- camera controls --- */
    bool want_look = in->mouse_down[UB_MOUSE_RIGHT] && in->has_focus;
    if (want_look != in->mouse_captured) ub_debug("mouse look %s", want_look ? "on" : "off");
    plat_set_mouse_capture(want_look);
    g_yaw += (f32)in->mouse_dx * LOOK_SENSITIVITY;
    if (!g_typing && in->key_down[UB_KEY_LEFT]) g_yaw -= TURN_SPEED * dt;
    if (!g_typing && in->key_down[UB_KEY_RIGHT]) g_yaw += TURN_SPEED * dt;
    g_yaw = ub_wrap_angle(g_yaw);
    for (int n = in->wheel; n >= UB_WHEEL_NOTCH; n -= UB_WHEEL_NOTCH) g_zoom *= 0.88f;
    for (int n = in->wheel; n <= -UB_WHEEL_NOTCH; n += UB_WHEEL_NOTCH) g_zoom /= 0.88f;
    g_zoom = ub_clampf(g_zoom, ZOOM_MIN, ZOOM_MAX);

    f32 fx = ub_sinf(g_yaw), fz = ub_cosf(g_yaw);
    f32 rx = fz, rz = -fx;

    /* --- text entry first: while it is open, keys do not move us --- */
    bool was_typing = g_typing;
    handle_typing(in);
    bool play_keys = !was_typing && !g_typing;

    /* --- movement, relative to the camera --- */
    f32 mf = 0, mr = 0;
    if (play_keys) {
        if (in->key_down['W'] || in->key_down[UB_KEY_UP]) mf += 1;
        if (in->key_down['S'] || in->key_down[UB_KEY_DOWN]) mf -= 1;
        if (in->key_down['D']) mr += 1;
        if (in->key_down['A']) mr -= 1;
    }
    if (mf != 0 && mr != 0) { mf *= 0.7071f; mr *= 0.7071f; }
    g_px = ub_clampf(g_px + (fx * mf + rx * mr) * MOVE_SPEED * dt, -WORLD_LIMIT, WORLD_LIMIT);
    g_pz = ub_clampf(g_pz + (fz * mf + rz * mr) * MOVE_SPEED * dt, -WORLD_LIMIT, WORLD_LIMIT);

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
    if (in->mouse_pressed[UB_MOUSE_LEFT]) {
        ub_play_params p = ub_play_default();
        p.pan = (f32)(in->mouse_x * 2 - fb->width) / (f32)fb->width;
        p.pitch = 2.0f - 1.5f * (f32)in->mouse_y / (f32)fb->height; /* higher up = higher pitch */
        ub_mixer_play(&g_snd_click, p);
        g_clicks++;
        ub_debug("click %u at %d,%d", g_clicks, in->mouse_x, in->mouse_y);
    }
    if (play_keys && in->key_pressed[UB_KEY_ESCAPE]) {
        ub_info("escape pressed, quitting");
        plat_request_quit();
    }

    /* --- render --- */
    view v;
    v.fx = fx; v.fz = fz; v.rx = rx; v.rz = rz;
    v.cam_x = g_px - fx * g_zoom;
    v.cam_z = g_pz - fz * g_zoom;
    v.cam_h = 4.0f + g_zoom * 0.35f;
    v.horizon = fb->height / 3;
    draw_floor(fb, &v);

    sprite sprites[UB_ARRAY_COUNT(k_parts) + 1];
    int count = 0;
    for (int i = 0; i < UB_ARRAY_COUNT(k_parts); ++i) {
        const part *p = &k_parts[i];
        if (project(&v, fb, p->x, p->z, p->height, p->size, p->color, &sprites[count])) count++;
    }
    if (project(&v, fb, g_px, g_pz, g_py, 8.0f, rgb(230, 70, 60), &sprites[count])) count++;
    for (int i = 1; i < count; ++i) { /* far to near (painter's order) */
        sprite s = sprites[i];
        int j = i - 1;
        while (j >= 0 && sprites[j].depth < s.depth) { sprites[j + 1] = sprites[j]; --j; }
        sprites[j + 1] = s;
    }
    for (int i = 0; i < count; ++i) draw_sprite(fb, &sprites[i]);

    /* --- overlay --- */
    g_fps_frames++;
    g_fps_accum_us += dt_us;
    if (g_fps_accum_us >= 500000) {
        g_fps_x10 = (u32)((u64)g_fps_frames * 10000000u / g_fps_accum_us);
        g_fps_frames = 0;
        g_fps_accum_us = 0;
    }
    u32 ram_mb = (u32)(g_sys.total_phys_bytes >> 20);
    ov->count = 0;
#define LINE(...) ub_fmt(ov->lines[ov->count++], UB_OVERLAY_LINE_LEN, __VA_ARGS__)
    LINE("Uniblox milestone 1 - %s", g_sys.os_name);
    LINE("%u.%u fps   RAM %u MB%s   display %d bpp", g_fps_x10 / 10, g_fps_x10 % 10, ram_mb,
         ram_mb + 8 < UB_RAM_MIN_MB ? " (below 384 MB minimum!)" : "", g_sys.display_bpp);
    LINE("Audio: %s, %d voices%s", g_sys.audio_desc, ub_mixer_active_voices(),
         g_muted ? ", MUTED" : "");
    LINE("Mouse %d,%d  look %s  zoom %d  wheel %s", in->mouse_x, in->mouse_y,
         in->mouse_captured ? "ON" : "off", (i32)g_zoom,
         g_sys.has_mouse_wheel ? "yes" : "not reported");
    if (g_typing) LINE("Say: %s_   (Enter send, Esc cancel)", g_typed);
    else LINE("Last message: %s", g_last_message[0] ? g_last_message : "(press Enter or / to type)");
    LINE("WASD/arrows move, Space jump, hold right mouse to look");
    LINE("Wheel zoom, left click beeps, M mute, Esc quit");
#undef LINE
}
