/* Uniblox - demo app: a textured, lit room of parts.
 *
 *   - WASD/arrows move, Space jumps, hold the right mouse button to orbit
 *     the camera (left/right and up/down), wheel zooms
 *   - F11 or Alt+Enter toggles fullscreen
 *   - F5 runs a 10-second benchmark flythrough (uncapped) and logs it
 *   - F6 cycles graphics quality (OpenGL) or render resolution (software)
 *   - F8 switches 32-bit / 8-bit palette rendering (software renderer)
 *   - F9 cycles the frame rate cap: 60, 120, 144, 240, unlimited
 *   - F3 hides the text overlay; M mutes; Enter or '/' opens a chat line;
 *     left click beeps (panned/pitched by position); Esc quits
 *   - F7 saves the place as places/<name>.ubx
 *   - settings are saved to uniblox.cfg
 *
 * The place runs in a game (game/ub_game.h): physics at a fixed 120 steps
 * a second, the player's character, and the place's scripts. Moving things
 * are drawn blended between the last two steps, so motion is smooth at any
 * frame rate. The demo place's door, spinner, lava and ball button are
 * scripts. */
#include "app/app.h"
#include "audio/ub_mixer.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "core/ub_vec.h"
#include "game/ub_demo_place.h"
#include "game/ub_game.h"
#include "render/ub_render.h"
#include "script/ub_script.h"
#include "world/ub_rbxl.h"
#include "world/ub_world.h"
#include "crt/ub_crt.h"

#define WORLD_LIMIT 100000.0f
#define TURN_SPEED 1.8f        /* radians/s for arrow keys */
#define LOOK_SENSITIVITY 0.006f
#define PITCH_MIN (-1.35f)
#define PITCH_MAX 0.35f
#define ZOOM_MIN 6.0f
#define ZOOM_MAX 60.0f
#define CONFIG_PATH "uniblox.cfg"
#define TYPED_MAX 40
#define BENCH_SECONDS 10
#define PLACE_MAX_BYTES (512u << 20)

static ub_system_info g_sys;
static ub_world g_world;              /* a place being built or loaded; then the game takes it */
static ub_game g_game;                /* the running place */
static bool g_game_on;
#define GW (&g_game.world)
static bool g_run_scripts = true;     /* scripts= in the config */
static ub_part_draw *g_statics;       /* visible parts nothing has moved or changed, handed to the renderer */
static int g_static_count, g_static_cap;
static f32 g_death_t;                 /* how far the character has fallen over */
static vec3 g_bench_center;
static bool g_demo_world;
static ub_import_stats g_import;
static bool g_imported;
static char g_place_arg[260];         /* place from the command line or config */
static char g_cfg_place[260];         /* place= from the config, written back on exit */
static char g_status[160];            /* last load/save message for the overlay */

static f32 g_facing;                  /* player yaw */
static f32 g_walk_phase, g_walk_amount;  /* limb swing animation */
static f32 g_yaw, g_pitch = -0.35f;   /* camera angles around the player */
static f32 g_zoom = 22.0f;            /* camera distance */
static f32 g_time;
static bool g_muted;
static int g_colors_cfg;              /* 0 = automatic, 8 or 32 = chosen with F8 */
static ub_display_prefs g_prefs;      /* renderer, window, fps cap (from the config) */
static int g_quality = UB_QUALITY_HIGH;
static int g_win_w, g_win_h;          /* last windowed size, saved on exit */
static char g_renderer_desc[160];
static bool g_gl_shaders = true;
static u32 g_fps_log_us;
static bool g_hide_overlay;
static char g_typed[TYPED_MAX + 1];
static bool g_typing;
static char g_last_message[TYPED_MAX + 1];
static u32 g_fps_frames, g_fps_accum_us, g_fps_x10;
static u32 g_clicks;
static ub_sound g_snd_jump, g_snd_land, g_snd_click;
static ub_render_stats g_stats;

/* benchmark */
static bool g_bench;
static f32 g_bench_t;
static u32 g_bench_frames;
static u64 g_bench_render_us, g_bench_present_us, g_bench_polys, g_bench_pixels;
static u32 g_bench_saved_cap;
static char g_bench_result[160];

/* --------------------------------------------------------------- scene */

static ub_part_draw part_draw(const ub_part *p)
{
    ub_part_draw d;
    d.pos = p->pos;
    d.rot = p->rot;
    d.size = p->size;
    d.color = p->color;
    d.shape = p->shape;
    d.material = p->material;
    d.flags = (u8)(((p->flags & UB_PART_GLOW) ? UB_DRAW_GLOW : 0) | ub_draw_see_through(p->transparency));
    return d;
}

static void build_demo_world(void)
{
    ub_world_free(&g_world);
    ub_demo_place(&g_world);
    g_demo_world = true;
    g_imported = false;
}

/* The static set: visible parts nothing has moved or changed. Rebuilt when
 * a part leaves it (a script changes it, physics moves it). */
static void rebuild_statics(void)
{
    if (g_static_cap < GW->part_count + 1) {
        plat_free(g_statics);
        g_static_cap = GW->part_count + 1024;
        g_statics = (ub_part_draw *)plat_alloc(sizeof(ub_part_draw) * (size_t)g_static_cap);
        if (!g_statics) g_static_cap = 0;
    }
    g_static_count = 0;
    for (i32 i = 0; i < GW->part_count && g_statics; ++i)
        if (ub_game_part_visible(&g_game, i) && !ub_game_part_live(&g_game, i))
            g_statics[g_static_count++] = part_draw(&GW->parts[i]);
    ub_render_set_static(g_statics, g_static_count);
    g_game.statics_dirty = false;
}

/* After building or loading g_world: run it (the game takes it over). */
static void world_changed(void)
{
    ub_render_set_static(NULL, 0);
    if (g_game_on) ub_game_stop(&g_game);
    g_game_on = ub_game_start(&g_game, &g_world, g_run_scripts);
    rebuild_statics();
    g_bench_center = g_demo_world ? v3(0, 3, 40) : v3_add(g_game.spawn, v3(0, 3, 0));
    g_death_t = 0;
    char title[128];
    ub_fmt(title, sizeof title, "Uniblox - %s", GW->name);
    plat_set_title(title);
    ub_script_stats st;
    ub_script_get_stats(&st);
    ub_info("world: \"%s\", %d instances, %d parts drawn (%d live), spawn at %d,%d,%d; %u script(s) running%s",
            GW->name, GW->count, g_static_count, g_game.live_count, ub_roundf(g_game.spawn.x),
            ub_roundf(g_game.spawn.y), ub_roundf(g_game.spawn.z), g_game.scripts ? st.scripts : 0u,
            g_run_scripts ? "" : " (scripts are off: scripts=0 in uniblox.cfg)");
}

/* The last path component without its extension, for naming imports. */
static void base_name(const char *path, char *out, int cap)
{
    const char *b = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    int n = 0;
    while (b[n] && b[n] != '.' && n < cap - 1) {
        out[n] = b[n];
        ++n;
    }
    out[n] = 0;
}

/* Load a .ubx or Roblox .rbxl/.rbxm place. user_path: chosen by the user
 * (command line), so any path is allowed; otherwise a checked game path. */
static bool load_place(const char *path, bool user_path)
{
    void *data;
    u32 size;
    bool read = user_path ? plat_file_read_user(path, PLACE_MAX_BYTES, &data, &size)
                          : plat_file_read_all(path, PLACE_MAX_BYTES, &data, &size);
    if (!read) {
        ub_warn("could not read place \"%s\"", path);
        ub_fmt(g_status, sizeof g_status, "Could not open %s", path);
        return false;
    }
    u64 t0 = plat_time_us();
    ub_world w;
    ub_world_init(&w);
    ub_load_result r;
    bool roblox = ub_rbxl_detect((const u8 *)data, size);
    if (roblox) r = ub_rbxl_load(&w, (const u8 *)data, size, &g_import);
    else r = ub_ubx_load(&w, (const u8 *)data, size);
    plat_free(data);
    if (r != UB_LOAD_OK) {
        ub_warn("could not load place \"%s\": %s", path, ub_load_result_name(r));
        ub_fmt(g_status, sizeof g_status, "Could not load %s: %s", path, ub_load_result_name(r));
        ub_world_free(&w);
        return false;
    }
    if (roblox) base_name(path, w.name, sizeof w.name);
    ub_world_free(&g_world); /* (empty: the game took the last one) */
    g_world = w;
    g_demo_world = false;
    g_imported = roblox;
    u32 ms = (u32)((plat_time_us() - t0) / 1000u);
    if (roblox) {
        ub_info("imported Roblox place \"%s\" in %u ms: %u instances, %u classes, %u parts, %u mesh/union "
                "placeholders, %u invisible, %u terrain (not imported), %u scripts, compression %s",
                path, ms, g_import.instances, g_import.classes, g_import.parts, g_import.placeholders,
                g_import.invisible, g_import.terrain, g_import.scripts, g_import.compression);
        ub_fmt(g_status, sizeof g_status, "Imported %s: %u parts, %u mesh boxes%s", w.name, g_import.parts,
               g_import.placeholders, g_import.terrain ? ", terrain skipped" : "");
    } else {
        ub_info("loaded place \"%s\" in %u ms", path, ms);
        ub_fmt(g_status, sizeof g_status, "Loaded %s", w.name);
    }
    world_changed();
    return true;
}

/* F7: save the current world as places/<name>.ubx. */
static void save_place(void)
{
    char name[48];
    int n = 0;
    for (const char *p = GW->name; *p && n < 40; ++p) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        name[n++] = ok ? c : '_';
    }
    if (!n) name[n++] = 'p';
    name[n] = 0;
    char path[80];
    ub_fmt(path, sizeof path, "places/%s.ubx", name);
    u8 *data;
    u32 size;
    if (!plat_make_dir("places") || !ub_ubx_save(GW, &data, &size)) {
        ub_fmt(g_status, sizeof g_status, "Could not save %s", path);
        return;
    }
    bool ok = plat_file_write_all(path, data, size);
    plat_free(data);
    ub_info("%s %s (%u bytes)", ok ? "saved" : "could not save", path, size);
    ub_fmt(g_status, sizeof g_status, "%s %s", ok ? "Saved" : "Could not save", path);
}

/* Roblox-style lighting -> renderer environment: sun position from the
 * clock time and latitude (equinox), fog from FogEnd. */
static ub_render_env env_from_world(void)
{
    ub_render_env e = ub_render_default_env();
    const ub_lighting *l = &GW->lighting;
    f32 h = (l->clock_time - 12.0f) * (UB_PI / 12.0f);        /* hour angle */
    f32 lat = l->latitude * (UB_PI / 180.0f);
    /* east = +X, north = +Z here (Roblox's north is -Z; Z is flipped) */
    vec3 sun = v3(-ub_sinf(h), ub_cosf(lat) * ub_cosf(h), -ub_sinf(lat) * ub_cosf(h));
    f32 day = ub_clampf((sun.y + 0.05f) / 0.25f, 0.0f, 1.0f);  /* fades out at dusk */
    if (sun.y < 0.12f) sun.y = 0.12f;                           /* keep some modelling at night */
    e.sun_dir = v3_normalize(sun);
    f32 b = ub_clampf(l->brightness / 2.0f, 0.4f, 1.5f) * (0.15f + 0.85f * day);
    e.sun_strength *= b;
    u32 sc = e.sun_color;
    u32 r = (u32)UB_MIN(255.0f, (f32)((sc >> 16) & 255) * b), g = (u32)UB_MIN(255.0f, (f32)((sc >> 8) & 255) * b),
        bl = (u32)UB_MIN(255.0f, (f32)(sc & 255) * b);
    e.sun_color = (r << 16) | (g << 8) | bl;
    if (day < 1.0f) { /* dusk/night sky */
        e.sky_top = 0x0B1630;
        e.sky_horizon = day > 0.3f ? 0xE0905A : 0x1C2A48;
    }
    e.fog_density = l->fog_end > 1.0f && l->fog_end < 20000.0f ? 3.0f / l->fog_end : 0.0012f;
    return e;
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

static const int k_sw_heights[] = { 240, 360, 480, 720, 0 };   /* 0 = window height */
static const u32 k_fps_caps[] = { 60, 120, 144, 240, 0 };      /* 0 = unlimited */

static bool str_is(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static void load_config(void)
{
    char *text;
    u32 size;
    if (!plat_file_read_all(CONFIG_PATH, 8192, (void **)&text, &size)) return;
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
        else if (key_is(line, "colors", &v)) {
            i32 c = parse_int(v);
            g_colors_cfg = (c == 8 || c == 32) ? c : 0;
        }
        else if (key_is(line, "renderer", &v))
            g_prefs.backend = str_is(v, "opengl") ? UB_BACKEND_PREF_OPENGL
                            : str_is(v, "software") ? UB_BACKEND_PREF_SOFTWARE : UB_BACKEND_PREF_AUTO;
        else if (key_is(line, "fullscreen", &v)) g_prefs.fullscreen = parse_int(v) != 0;
        else if (key_is(line, "window_w", &v)) g_prefs.window_w = parse_int(v);
        else if (key_is(line, "window_h", &v)) g_prefs.window_h = parse_int(v);
        else if (key_is(line, "fps_cap", &v)) g_prefs.fps_cap = (u32)UB_CLAMP(parse_int(v), 0, 1000);
        else if (key_is(line, "vsync", &v)) g_prefs.vsync = parse_int(v) != 0;
        else if (key_is(line, "sw_height", &v)) g_prefs.sw_height = UB_CLAMP(parse_int(v), 0, 4320);
        else if (key_is(line, "quality", &v)) g_quality = UB_CLAMP(parse_int(v), 0, UB_QUALITY_COUNT - 1);
        else if (key_is(line, "gl_shaders", &v)) g_gl_shaders = parse_int(v) != 0;
        else if (key_is(line, "scripts", &v)) g_run_scripts = parse_int(v) != 0;
        else if (key_is(line, "place", &v)) {
            ub_fmt(g_cfg_place, sizeof g_cfg_place, "%s", v);
            if (!g_place_arg[0] && v[0]) ub_fmt(g_place_arg, sizeof g_place_arg, "@%s", v);
        }
        *end = saved;
        line = end;
        while (*line == '\n' || *line == '\r') ++line;
    }
    plat_free(text);
    ub_info("loaded %s", CONFIG_PATH);
}

static void save_config(void)
{
    static const char *const renderer[] = { "auto", "opengl", "software" };
    char text[1792];
    int n = ub_fmt(text, sizeof text,
                   "# Uniblox settings (written on exit)\r\n"
                   "# renderer: auto (OpenGL if available), opengl or software\r\n"
                   "renderer=%s\r\n"
                   "# quality (OpenGL): 0 = low, 1 = medium (shadows), 2 = high (shadows + anti-aliasing)\r\n"
                   "quality=%d\r\n"
                   "# gl_shaders: 1 = full effects, 0 = basic OpenGL 1.1 (for problem drivers)\r\n"
                   "gl_shaders=%d\r\n"
                   "# fps_cap: frame rate limit, 0 = unlimited\r\n"
                   "fps_cap=%u\r\nvsync=%d\r\n"
                   "fullscreen=%d\r\nwindow_w=%d\r\nwindow_h=%d\r\n"
                   "# sw_height (software renderer): render height, 0 = window height\r\n"
                   "sw_height=%d\r\n"
                   "# colors (software renderer): 0 = automatic, 8 = 256-colour palette, 32 = full colour\r\n"
                   "colors=%d\r\n"
                   "zoom=%d\r\nyaw_deg=%d\r\npitch_deg=%d\r\nmuted=%d\r\n"
                   "# scripts: 1 = run the place's scripts, 0 = do not\r\n"
                   "scripts=%d\r\n"
                   "# place: place file to open at start, relative to this folder (.ubx, .rbxl or .rbxm),\r\n"
                   "# e.g. places/castle.ubx. Empty = the built-in demo. A file named on the command line wins\r\n"
                   "place=%s\r\n",
                   renderer[g_prefs.backend], g_quality, g_gl_shaders, g_prefs.fps_cap, g_prefs.vsync,
                   g_prefs.fullscreen, g_win_w, g_win_h, g_prefs.sw_height, g_colors_cfg,
                   (i32)g_zoom, DEG(ub_wrap_angle(g_yaw)), DEG(g_pitch), g_muted, g_run_scripts, g_cfg_place);
    if (plat_file_write_all(CONFIG_PATH, text, (u32)n)) ub_info("saved %s", CONFIG_PATH);
    else ub_warn("could not save %s", CONFIG_PATH);
}

/* --------------------------------------------------------- character */

/* A simple blocky figure: torso, round head, arms and legs that swing
 * while walking. Each limb pivots at its top (shoulder or hip). */
static void draw_limb(vec3 base, const mat3 *body, vec3 pivot, vec3 size, f32 swing, u32 color,
                      ub_material mat)
{
    mat3 r = mat3_from_euler(0, swing, 0);
    mat3 rot;   /* body rotation followed by the swing about the pivot */
    rot.x = mat3_apply(body, r.x);
    rot.y = mat3_apply(body, r.y);
    rot.z = mat3_apply(body, r.z);
    vec3 hang = mat3_apply(&rot, v3(0, -size.y * 0.5f, 0));
    ub_part_draw p;
    p.pos = v3_add(v3_add(base, mat3_apply(body, pivot)), hang);
    p.rot = rot;
    p.size = size;
    p.color = color;
    p.shape = UB_SHAPE_BOX;
    p.material = (u8)mat;
    p.flags = 0;
    ub_render_part(&p);
}

static void draw_character(f32 x, f32 y, f32 z, f32 facing, f32 phase, f32 amount, f32 fallen)
{
    const u32 shirt = 0x2F8F9D, pants = 0x3B3F4A, skin = 0xF0C8A0;
    mat3 body = mat3_from_euler(facing, -UB_HALF_PI * fallen, 0); /* dead: tipped over backwards */
    vec3 base = v3(x, y, z);
    f32 swing = ub_sinf(phase) * 0.7f * amount;

    draw_limb(base, &body, v3(-0.5f, 2.0f, 0), v3(0.9f, 2.0f, 1.0f), swing, pants, UB_MAT_PLASTIC);
    draw_limb(base, &body, v3(0.5f, 2.0f, 0), v3(0.9f, 2.0f, 1.0f), -swing, pants, UB_MAT_PLASTIC);
    ub_part_draw torso;
    torso.pos = v3_add(base, mat3_apply(&body, v3(0, 3.0f, 0)));
    torso.rot = body;
    torso.size = v3(2.0f, 2.0f, 1.0f);
    torso.color = shirt;
    torso.shape = UB_SHAPE_BOX;
    torso.material = UB_MAT_PLASTIC;
    torso.flags = 0;
    ub_render_part(&torso);
    draw_limb(base, &body, v3(-1.5f, 4.0f, 0), v3(0.9f, 2.0f, 0.9f), -swing, skin, UB_MAT_PLASTIC);
    draw_limb(base, &body, v3(1.5f, 4.0f, 0), v3(0.9f, 2.0f, 0.9f), swing, skin, UB_MAT_PLASTIC);
    ub_part_draw head = torso;
    head.pos = v3_add(base, mat3_apply(&body, v3(0, 4.75f, 0)));
    head.size = v3(1.4f, 1.4f, 1.4f);
    head.color = skin;
    head.shape = UB_SHAPE_BALL;
    ub_render_part(&head);
}

/* ------------------------------------------------------------ app API */

const ub_app_info *app_info(void)
{
    static const ub_app_info info = { "Uniblox", "uniblox.log", "uniblox.old.log" };
    return &info;
}

void app_preinit(ub_display_prefs *prefs)
{
    g_prefs = *prefs; /* platform defaults: 120 fps cap, software height 480 */
    plat_command_line_arg(g_place_arg, sizeof g_place_arg); /* "uniblox.exe castle.rbxl" */
    load_config();
    *prefs = g_prefs;
    g_win_w = prefs->window_w;
    g_win_h = prefs->window_h;
}

bool app_init(const ub_system_info *sys, const ub_frame *frame)
{
    g_sys = *sys;
    u32 ram_mb = (u32)(sys->total_phys_bytes >> 20);
    if (ram_mb + 8 < UB_RAM_MIN_MB)
        ub_warn("only %u MB RAM; Uniblox needs %u MB (%u MB recommended)", ram_mb,
                UB_RAM_MIN_MB, UB_RAM_REC_MB);
    ub_render_force_fixed_function(!g_gl_shaders);
    if (!ub_render_init(frame, g_renderer_desc, sizeof g_renderer_desc)) {
        ub_error("renderer failed to start: %s", g_renderer_desc);
        return false;
    }
    ub_info("renderer: %s", g_renderer_desc);
    ub_render_set_quality((ub_quality)g_quality);
    if (g_colors_cfg == 8) plat_set_pixel_format(UB_PIXEL_INDEX8);
    if (g_colors_cfg == 32) plat_set_pixel_format(UB_PIXEL_XRGB32);
    if (!frame->fullscreen) {
        g_win_w = frame->width;
        g_win_h = frame->height;
    }
    /* the place: command line, else place= in the config (a game path,
     * marked with '@'), else the built-in demo */
    bool loaded = false;
    if (g_place_arg[0] == '@') loaded = load_place(g_place_arg + 1, false);
    else if (g_place_arg[0]) loaded = load_place(g_place_arg, true);
    if (!loaded) {
        build_demo_world();
        world_changed();
    }
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
    ub_render_set_static(NULL, 0);
    plat_free(g_statics);
    g_statics = NULL;
    g_static_cap = 0;
    u32 jumps = g_game.jumps;
    if (g_game_on) ub_game_stop(&g_game);
    g_game_on = false;
    ub_world_free(&g_world);
    ub_render_shutdown();
    ub_info("demo: %u jumps, %u clicks", jumps, g_clicks);
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
                if (g_game.scripts) ub_script_chatted(g_typed); /* Player.Chatted */
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

static const char *mode_name(const ub_frame *frame)
{
    if (frame->backend == UB_BACKEND_OPENGL) return ub_render_quality_name(ub_render_quality());
    return frame->fb && frame->fb->format == UB_PIXEL_INDEX8 ? "8-bit" : "32-bit";
}

static void start_benchmark(ub_frame *frame)
{
    g_bench = true;
    g_bench_t = 0;
    g_bench_frames = 0;
    g_bench_render_us = g_bench_present_us = g_bench_polys = g_bench_pixels = 0;
    g_bench_saved_cap = frame->fps_cap;
    plat_set_fps_cap(0); /* measure what the machine can do */
    ub_info("benchmark: started (%d s flythrough, frame cap off)", BENCH_SECONDS);
}

static void finish_benchmark(ub_frame *frame, bool cancelled)
{
    g_bench = false;
    plat_set_fps_cap(g_bench_saved_cap);
    if (cancelled || g_bench_frames == 0) {
        ub_info("benchmark: cancelled");
        ub_fmt(g_bench_result, sizeof g_bench_result, "Benchmark cancelled");
        return;
    }
    u32 fps10 = (u32)((f32)g_bench_frames * 10.0f / g_bench_t);
    u32 ms10 = (u32)(g_bench_render_us / g_bench_frames / 100u);
    u32 pms10 = (u32)(g_bench_present_us / g_bench_frames / 100u);
    const char *backend = frame->backend == UB_BACKEND_OPENGL ? "OpenGL" : "software";
    int rw = frame->fb ? frame->fb->width : frame->width, rh = frame->fb ? frame->fb->height : frame->height;
    ub_fmt(g_bench_result, sizeof g_bench_result,
           "Benchmark: %u.%u fps at %dx%d (%s %s; CPU %u.%u ms, present %u.%u ms)",
           fps10 / 10, fps10 % 10, frame->width, frame->height, backend, mode_name(frame),
           ms10 / 10, ms10 % 10, pms10 / 10, pms10 % 10);
    ub_info("benchmark: %u frames in %u ms = %u.%u fps; render %u.%u ms/frame, present %u.%u ms/frame; "
            "%u polys, %u pixels per frame; window %dx%d, render %dx%d; %s %s; %s; %s; %s%s%s",
            g_bench_frames, (u32)(g_bench_t * 1000.0f), fps10 / 10, fps10 % 10, ms10 / 10, ms10 % 10,
            pms10 / 10, pms10 % 10,
            (u32)(g_bench_polys / g_bench_frames), (u32)(g_bench_pixels / g_bench_frames),
            frame->width, frame->height, rw, rh, backend, mode_name(frame), g_renderer_desc,
            g_sys.cpu_desc, g_sys.os_name, frame->present_detail[0] ? "; present: " : "",
            frame->present_detail);
}

void app_frame(ub_frame *frame, const ub_input *in, u32 dt_us)
{
    ub_framebuffer *fb = frame->fb;
    if (fb && !fb->palette) {
        fb->palette = ub_render_palette();
        fb->palette_serial++;
    }
    u32 real_dt_us = dt_us;
    if (dt_us > 100000) dt_us = 100000; /* clamp after stalls/debugger */
    const f32 dt = (f32)dt_us * 1e-6f;
    g_time += dt;
    if (!frame->fullscreen) {
        g_win_w = frame->width;
        g_win_h = frame->height;
    }

    /* --- text entry first: while it is open, keys do not move us --- */
    bool was_typing = g_typing;
    if (!g_bench) handle_typing(in);
    bool play_keys = !was_typing && !g_typing && !g_bench;

    /* --- function keys --- */
    if (play_keys && in->key_pressed[UB_KEY_F3]) g_hide_overlay = !g_hide_overlay;
    if ((in->key_pressed[UB_KEY_F11] && !g_typing) ||
        (in->key_down[UB_KEY_ALT] && in->key_pressed[UB_KEY_ENTER])) {
        g_prefs.fullscreen = !frame->fullscreen;
        plat_set_fullscreen(g_prefs.fullscreen);
    }
    if (play_keys && in->key_pressed[UB_KEY_F5]) start_benchmark(frame);
    else if (g_bench && (in->key_pressed[UB_KEY_ESCAPE] || in->key_pressed[UB_KEY_F5])) {
        finish_benchmark(frame, true);
        play_keys = false; /* this Escape only cancels */
    }
    if (play_keys && in->key_pressed[UB_KEY_F6]) {
        if (frame->backend == UB_BACKEND_OPENGL) {
            g_quality = (g_quality + UB_QUALITY_COUNT - 1) % UB_QUALITY_COUNT; /* high, medium, low, high... */
            ub_render_set_quality((ub_quality)g_quality);
            ub_info("quality: %s", ub_render_quality_name((ub_quality)g_quality));
        } else {
            int i = 0;
            while (i < UB_ARRAY_COUNT(k_sw_heights) - 1 && k_sw_heights[i] != g_prefs.sw_height) ++i;
            g_prefs.sw_height = k_sw_heights[(i + 1) % UB_ARRAY_COUNT(k_sw_heights)];
            plat_set_sw_resolution(g_prefs.sw_height);
            ub_info("software render height: %d%s", g_prefs.sw_height, g_prefs.sw_height ? "" : " (window)");
        }
    }
    if (play_keys && in->key_pressed[UB_KEY_F7]) save_place();
    if (play_keys && fb && in->key_pressed[UB_KEY_F8]) {
        ub_pixel_format next = fb->format == UB_PIXEL_INDEX8 ? UB_PIXEL_XRGB32 : UB_PIXEL_INDEX8;
        g_colors_cfg = next == UB_PIXEL_INDEX8 ? 8 : 32;
        plat_set_pixel_format(next);
    }
    if (play_keys && in->key_pressed[UB_KEY_F9]) {
        int i = 0;
        while (i < UB_ARRAY_COUNT(k_fps_caps) - 1 && k_fps_caps[i] != g_prefs.fps_cap) ++i;
        g_prefs.fps_cap = k_fps_caps[(i + 1) % UB_ARRAY_COUNT(k_fps_caps)];
        plat_set_fps_cap(g_prefs.fps_cap);
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
    vec3 move = v3(fx * mf + rx * mr, 0, fz * mf + rz * mr);
    bool moving = mf != 0 || mr != 0;
    if (moving) {
        /* turn the character smoothly towards where it is going
         * (yaw of direction (x, z) is atan2(x, z): yaw 0 faces +Z) */
        f32 want = ub_atan2f(move.x, move.z);
        f32 diff = ub_wrap_angle(want - g_facing);
        g_facing = ub_wrap_angle(g_facing + diff * ub_clampf(dt * 12.0f, 0, 1));
    }
    /* Roblox-style: holding Space keeps jumping each time the feet land */
    bool jump = play_keys && (in->key_down[UB_KEY_SPACE] || in->key_pressed[UB_KEY_SPACE]);

    /* --- the game: scripts, physics in fixed steps, the character --- */
    ub_game_update(&g_game, dt, move, jump);
    if (g_game.jumped) {
        g_game.jumped = false;
        ub_mixer_play(&g_snd_jump, ub_play_default());
    }
    if (g_game.landed_speed > 0) {
        if (g_game.landed_speed > 8.0f) {
            ub_play_params pp = ub_play_default();
            pp.volume = ub_clampf(g_game.landed_speed / 60.0f, 0.2f, 1.0f);
            ub_mixer_play(&g_snd_land, pp);
        }
        g_game.landed_speed = 0;
    }
    if (g_game.respawned) {
        g_game.respawned = false;
        g_death_t = 0;
    }
    g_death_t = g_game.dead ? ub_clampf(g_death_t + dt * 3.0f, 0, 1) : 0;
    if (g_game.statics_dirty) rebuild_statics();
    vec3 vel = g_game.phys_ok ? ub_char_velocity(&g_game.chr, &g_game.phys) : v3(0, 0, 0);
    f32 hspeed = ub_sqrtf(vel.x * vel.x + vel.z * vel.z);
    bool walking = moving && hspeed > 2.0f && !g_game.dead;
    g_walk_amount = ub_clampf(g_walk_amount + (walking ? dt : -dt) * 6.0f, 0.0f, 1.0f);
    if (walking) g_walk_phase = ub_wrap_angle(g_walk_phase + dt * 10.0f * hspeed / UB_CHAR_WALK_SPEED);

    /* --- other input --- */
    if (play_keys && in->key_pressed['M']) {
        g_muted = !g_muted;
        ub_mixer_set_master_volume(g_muted ? 0.0f : 0.8f);
        ub_debug("mute %s", g_muted ? "on" : "off");
    }
    if (!g_bench && in->mouse_pressed[UB_MOUSE_LEFT]) {
        ub_play_params p = ub_play_default();
        p.pan = (f32)(in->mouse_x * 2 - frame->width) / (f32)frame->width;
        p.pitch = 2.0f - 1.5f * (f32)in->mouse_y / (f32)frame->height;
        ub_mixer_play(&g_snd_click, p);
        g_clicks++;
        ub_debug("click %u at %d,%d", g_clicks, in->mouse_x, in->mouse_y);
    }
    if (play_keys && in->key_pressed[UB_KEY_ESCAPE]) {
        ub_info("escape pressed, quitting");
        plat_request_quit();
    }

    /* --- camera --- */
    ub_camera cam;
    if (g_bench) {
        /* fixed flythrough: orbit the room */
        g_bench_t += (f32)real_dt_us * 1e-6f;
        f32 t = g_bench_t / (f32)BENCH_SECONDS;
        f32 a = t * UB_TWO_PI;
        vec3 target = g_bench_center;
        cam.pitch = -0.3f - 0.2f * ub_sinf(a * 2.0f);
        f32 dist = 30.0f - 12.0f * ub_sinf(a);
        cam.pos = v3(target.x + ub_sinf(a) * dist, target.y + 5.0f + 4.0f * ub_sinf(a * 2.0f),
                     target.z + ub_cosf(a) * dist);
        cam.yaw = ub_wrap_angle(a + UB_PI);
        cam.focus = target;
    } else {
        vec3 target = v3_add(g_game.feet, v3(0, 3.5f, 0));
        cam.yaw = g_yaw;
        cam.pitch = g_pitch;
        f32 cp = ub_cosf(g_pitch);
        vec3 fwd = v3(fx * cp, ub_sinf(g_pitch), fz * cp);
        cam.pos = v3_sub(target, v3_scale(fwd, g_zoom));
        cam.focus = target;
    }
    cam.fov_y = 70.0f * UB_PI / 180.0f;
    cam.far_dist = 600.0f;

    /* --- render --- */
    ub_render_env env = env_from_world();
    ub_render_begin(frame, &cam, &env);
    /* the static set was handed over (rebuild_statics); live parts here:
     * moving ones blended between the last two physics steps, and any a
     * script changed */
    for (i32 i = 0; i < g_game.live_count; ++i) {
        i32 pi = g_game.live[i];
        if (!ub_game_part_visible(&g_game, pi)) continue;
        ub_part_draw d = part_draw(&GW->parts[pi]);
        ub_game_part_pose(&g_game, pi, &d.pos, &d.rot);
        ub_render_part(&d);
    }
    if (!g_bench)
        draw_character(g_game.feet.x, g_game.feet.y, g_game.feet.z, g_facing, g_walk_phase, g_walk_amount, g_death_t);
    ub_render_end_scene();

    /* --- overlay --- */
    g_fps_frames++;
    g_fps_accum_us += real_dt_us;
    if (g_fps_accum_us >= 500000) {
        g_fps_x10 = (u32)((u64)g_fps_frames * 10000000u / g_fps_accum_us);
        g_fps_frames = 0;
        g_fps_accum_us = 0;
    }
    g_fps_log_us += real_dt_us;
    if (g_fps_log_us >= 5000000) { /* a periodic line helps when reading logs from testers */
        g_fps_log_us = 0;
        ub_debug("fps %u.%u at %dx%d, cap %u, slept %u us last frame", g_fps_x10 / 10, g_fps_x10 % 10,
                 frame->width, frame->height, frame->fps_cap, frame->sleep_us);
    }
    if (!g_hide_overlay || g_bench) {
        int lh = ub_render_line_height(), x = lh / 2, y = lh / 3;
        char line[192];
#define LINE(...) do { ub_fmt(line, sizeof line, __VA_ARGS__); ub_render_text(x, y, line, 0xFFFFFF); y += lh; } while (0)
        u32 rms10 = g_stats.render_us / 100u, pms10 = frame->present_us / 100u;
        LINE("Uniblox - %s", g_sys.os_name);
        LINE("%s", g_renderer_desc);
        char cap[16];
        if (frame->fps_cap) ub_fmt(cap, sizeof cap, "%u", frame->fps_cap);
        else ub_fmt(cap, sizeof cap, "off");
        LINE("%u.%u fps (cap %s)  %dx%d %s  CPU %u.%u ms  present %u.%u ms  %u tris",
             g_fps_x10 / 10, g_fps_x10 % 10, cap, frame->width, frame->height, mode_name(frame),
             rms10 / 10, rms10 % 10, pms10 / 10, pms10 % 10, g_stats.polys_drawn);
        if (frame->present_detail[0]) LINE("Present: %s", frame->present_detail);
        if (g_bench) {
            LINE("BENCHMARK running: %u s left (Esc cancels)", (u32)((f32)BENCH_SECONDS - g_bench_t) + 1u);
        } else {
            LINE("Place: %s (%d parts)%s", GW->name, GW->part_count, g_imported ? " - imported from Roblox" : "");
            if (g_game.phys_ok) {
                u32 phys10 = g_game.phys_us / 100u;
                LINE("Physics: %d live parts, %d awake, %d contacts, %u.%u ms%s", g_game.live_count,
                     g_game.phys.stats.awake, g_game.phys.stats.contacts, phys10 / 10, phys10 % 10,
                     g_game.chr.grounded ? "" : "  (in the air)");
            }
            if (g_game.scripts) {
                ub_script_stats st;
                ub_script_get_stats(&st);
                u32 s10 = g_game.script_us / 100u;
                LINE("Scripts: %u running, %u waiting, %u KB, %u.%u ms%s%s", st.scripts, st.threads, st.memory_kb,
                     s10 / 10, s10 % 10, st.errors ? "  - errors below" : "",
                     st.skipped_client ? "  (LocalScripts not run yet)" : "");
                const char *out[UB_SCRIPT_OUTPUT_LINES];
                bool err[UB_SCRIPT_OUTPUT_LINES];
                int n = ub_script_output(out, err, 4);
                for (int i = 0; i < n; ++i) {
                    ub_fmt(line, sizeof line, "  %s", out[i]);
                    ub_render_text(x, y, line, err[i] ? 0xFF9080 : 0xC8F0C8);
                    y += lh;
                }
                ub_fmt(line, sizeof line, "Health %d", ub_roundf(g_game.health));
                if (g_game.dead) ub_fmt(line, sizeof line, "You died - back in %d s", (int)g_game.respawn_timer + 1);
                ub_render_text(x, y, line, g_game.dead ? 0xFF6060 : 0xFFFFFF);
                y += lh;
            } else if (!g_run_scripts) {
                LINE("Scripts are off (scripts=0 in uniblox.cfg)");
            }
            if (g_status[0]) LINE("%s", g_status);
            LINE("Audio: %s%s", g_sys.audio_desc, g_muted ? " (muted)" : "");
            if (g_bench_result[0]) LINE("%s", g_bench_result);
            if (g_typing) LINE("Say: %s_   (Enter send, Esc cancel)", g_typed);
            else if (g_last_message[0]) LINE("Last message: %s", g_last_message);
            else if (g_demo_world) LINE("Chat (Enter): fast, slow, night, day");
            LINE("WASD move, Space jump, right mouse look, wheel zoom, F11 fullscreen");
            LINE("F5 benchmark, F6 %s, F7 save place, F8 colours, F9 fps cap, F3 hide text, Esc quit",
                 frame->backend == UB_BACKEND_OPENGL ? "quality" : "resolution");
        }
#undef LINE
    }
    ub_render_end(&g_stats);

    if (g_bench) {
        g_bench_frames++;
        g_bench_render_us += g_stats.render_us;
        g_bench_present_us += frame->present_us;
        g_bench_polys += g_stats.polys_drawn;
        g_bench_pixels += g_stats.pixels_tested;
        if (g_bench_t >= (f32)BENCH_SECONDS) finish_benchmark(frame, false);
    }
}
