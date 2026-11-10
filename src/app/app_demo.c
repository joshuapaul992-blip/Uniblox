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
 *     left click beeps (panned/pitched by position); Esc opens the menu (Q quits from it)
 *   - F7 saves the place as places/<name>.ubx
 *   - settings are saved to uniblox.cfg
 *
 * The place runs in a game (game/ub_game.h): physics at a fixed 120 steps
 * a second, the player's character, and the place's scripts. Moving things
 * are drawn blended between the last two steps, so motion is smooth at any
 * frame rate. The demo place's door, spinner, lava and ball button are
 * scripts. */
#include "app/app.h"
#include "app/app_play.h"
#include "app/app_hud.h"
#include "gui/ub_gui.h"
#include "app/app_settings.h"
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
#include "ui/ub_ui.h"
#include "world/ub_rbxl.h"
#include "world/ub_world.h"
#include "crt/ub_crt.h"

#define WORLD_LIMIT 100000.0f
#define PITCH_MIN PLAY_PITCH_MIN
#define PITCH_MAX PLAY_PITCH_MAX
#define ZOOM_MIN PLAY_ZOOM_MIN
#define ZOOM_MAX PLAY_ZOOM_MAX
#define CONFIG_PATH "uniblox.cfg"
#define TYPED_MAX 40
#define BENCH_SECONDS 10
#define PLACE_MAX_BYTES (512u << 20)

static ub_system_info g_sys;
static ub_world g_world;              /* a place being built or loaded; then the game takes it */
static ub_game g_game;                /* the running place */
static bool g_game_on;
#define GW (&g_game.world)
static ub_settings g_set;             /* everything in uniblox.cfg (the menu changes it) */
static ub_menu g_menu;
static bool g_menu_open;
#define g_run_scripts g_set.scripts
static ub_play_view g_view;           /* camera, controls, the character's figure, the static set */
static vec3 g_bench_center;
static bool g_demo_world;
static ub_import_stats g_import;
static bool g_imported;
static char g_place_arg[260];         /* place from the command line or config */
static bool g_auto_bench;             /* "uniblox.exe -benchmark": run the benchmark, then quit */
#define g_cfg_place g_set.place
static char g_status[160];            /* last load/save message for the overlay */

#define g_yaw g_view.yaw
#define g_pitch g_view.pitch
#define g_zoom g_view.zoom
static f32 g_time;
static ub_camera g_last_cam;      /* last frame's camera (the GUI places billboards with it) */
static bool g_have_cam;
#define g_muted g_set.muted
#define g_colors_cfg g_set.colors
#define g_prefs g_set.prefs
#define g_quality g_set.quality
static int g_win_w, g_win_h;          /* last windowed size, saved on exit */
static char g_renderer_desc[160];
#define g_gl_shaders g_set.gl_shaders
static u32 g_fps_log_us;
#define g_hide_overlay g_set.hide_overlay
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
static u64 g_bench_gui_us, g_bench_render_us, g_bench_scene_us, g_bench_present_us, g_bench_polys, g_bench_pixels;
static u32 g_bench_saved_cap;
static char g_bench_result[160];

/* --------------------------------------------------------------- scene */


static void build_demo_world(void)
{
    ub_world_free(&g_world);
    ub_demo_place(&g_world);
    g_demo_world = true;
    g_imported = false;
}


/* After building or loading g_world: run it (the game takes it over). */
static void world_changed(void)
{
    ub_render_set_static(NULL, 0);
    if (g_game_on) ub_game_stop(&g_game);
    g_game_on = ub_game_start(&g_game, &g_world, g_run_scripts);
    play_view_reset(&g_view, &g_game);
    hud_begin_game(g_game_on ? &g_game : NULL);
    g_bench_center = g_demo_world ? v3(0, 3, 40) : v3_add(g_game.spawn, v3(0, 3, 0));
    char title[128];
    ub_fmt(title, sizeof title, "Uniblox - %s", GW->name);
    plat_set_title(title);
    ub_script_stats st;
    ub_script_get_stats(&st);
    ub_info("world: \"%s\", %d instances, %d parts drawn (%d live), spawn at %d,%d,%d; %u script(s) running%s",
            GW->name, GW->count, g_view.static_count, g_game.live_count, ub_roundf(g_game.spawn.x),
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
static char g_loaded_path[260];   /* the place last loaded ("" = the demo) */
static bool g_loaded_user;

static bool load_place(const char *path, bool user_path);

/* The menu's "Load the place again": a fresh start of the same place. */
static void reload_place(void)
{
    ub_info("loading the place again");
    char path[260];
    ub_fmt(path, sizeof path, "%s", g_loaded_path);
    if (!path[0] || !load_place(path, g_loaded_user)) {
        build_demo_world();
        world_changed();
    }
}

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
    ub_fmt(g_loaded_path, sizeof g_loaded_path, "%s", path);
    g_loaded_user = user_path;
    g_imported = roblox;
    u32 ms = (u32)((plat_time_us() - t0) / 1000u);
    if (roblox) {
        ub_info("imported Roblox place \"%s\" in %u ms: %u instances, %u classes, %u parts, %u unions/meshes "
                "with their shape, %u with a mesh file or SpecialMesh look, %u mesh/union placeholders, %u invisible, "
                "%u terrain (not imported), %u scripts, compression %s",
                path, ms, g_import.instances, g_import.classes, g_import.parts, g_import.meshes, g_import.looks,
                g_import.placeholders, g_import.invisible, g_import.terrain, g_import.scripts, g_import.compression);
        ub_fmt(g_status, sizeof g_status, "Imported %s: %u parts, %u shaped meshes, %u mesh boxes%s", w.name,
               g_import.parts, g_import.meshes, g_import.placeholders, g_import.terrain ? ", terrain skipped" : "");
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



#define DEG(r) ub_roundf((r) * (180.0f / UB_PI))
#define RAD(d) ((f32)(d) * (UB_PI / 180.0f))

static const int k_sw_heights[] = { 240, 360, 480, 720, 0 };   /* 0 = window height */
static const u32 k_fps_caps[] = { 60, 120, 144, 240, 0 };      /* 0 = unlimited */


static void apply_volume(void) { ub_mixer_set_master_volume(g_muted ? 0.0f : (f32)g_set.volume / 100.0f); }

static void apply_controls(void)
{
    g_view.look_scale = (f32)g_set.mouse_pct / 100.0f;
    g_view.invert_y = g_set.invert_y;
    g_view.shift_lock_allowed = g_set.shift_lock;
    g_view.fov_deg = (f32)g_set.fov;
}

static void load_config(void)
{
    char *text;
    u32 size;
    if (!plat_file_read_all(CONFIG_PATH, 16384, (void **)&text, &size)) return;
    settings_parse(&g_set, text);
    plat_free(text);
    g_zoom = ub_clampf((f32)g_set.zoom, ZOOM_MIN, ZOOM_MAX);
    g_yaw = RAD(g_set.yaw_deg);
    g_pitch = ub_clampf(RAD(g_set.pitch_deg), PITCH_MIN, PITCH_MAX);
    if (!g_place_arg[0] && g_set.place[0]) ub_fmt(g_place_arg, sizeof g_place_arg, "@%s", g_set.place);
    ub_info("loaded %s", CONFIG_PATH);
}

static void save_config(void)
{
    g_set.zoom = ub_roundf(g_zoom);
    g_set.yaw_deg = DEG(ub_wrap_angle(g_yaw));
    g_set.pitch_deg = DEG(g_pitch);
    g_set.prefs.window_w = g_win_w;
    g_set.prefs.window_h = g_win_h;
    char text[3072];
    int n = settings_write(&g_set, text, sizeof text);
    if (plat_file_write_all(CONFIG_PATH, text, (u32)n)) ub_info("saved %s", CONFIG_PATH);
    else ub_warn("could not save %s", CONFIG_PATH);
}

/* --------------------------------------------------------- character */



/* ------------------------------------------------------------ app API */

const ub_app_info *app_info(void)
{
    static const ub_app_info info = { "Uniblox", "uniblox.log", "uniblox.old.log" };
    return &info;
}

void app_preinit(ub_display_prefs *prefs)
{
    play_view_init(&g_view);
    settings_defaults(&g_set);
    g_prefs = *prefs; /* platform defaults: 120 fps cap, software height 480 */
    g_prefs.fs_width = g_prefs.fs_height = 0;
    plat_command_line_arg(g_place_arg, sizeof g_place_arg); /* "uniblox.exe castle.rbxl" */
    if (!ub_strcmp(g_place_arg, "-benchmark") || !ub_strcmp(g_place_arg, "/benchmark")) {
        g_auto_bench = true;
        g_place_arg[0] = 0;
    }
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
    /* without OpenGL, the fast software renderer unless the plain one is asked for */
    ub_render_set_fast(frame->backend == UB_BACKEND_SOFTWARE && g_prefs.backend != UB_BACKEND_PREF_SOFTWARE,
                       g_set.render_threads);
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
    apply_volume();
    apply_controls();
    plat_set_present_changed_only(g_set.changed_only);
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
    play_view_free(&g_view);
    u32 jumps = g_game.jumps;
    if (g_game_on) ub_game_stop(&g_game);
    g_game_on = false;
    ub_world_free(&g_world);
    ub_render_shutdown();
    ub_info("demo: %u jumps, %u clicks", jumps, g_clicks);
}

/* Chat-style text entry: Enter or '/' opens it, Enter sends, Escape
 * cancels. While it is open, keys type text instead of moving. Works on
 * the ordered character stream so fast typing is never reordered.
 * Returns whether it was open at any point this frame: '/', text and Esc
 * can all arrive in one frame, and that Esc only cancels the text. */
static bool handle_typing(const ub_input *in)
{
    bool used = g_typing;
    for (const char *p = in->text; *p;) {
        const char *start = p;
        u32 c = ub_utf8_decode(&p);
        int len = (int)ub_strlen(g_typed);
        if (!g_typing) {
            if (c == '\r' || c == '/') {
                g_typing = used = true;
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
                if (g_game_on) ub_game_player_chat(&g_game, g_typed); /* the chat, a bubble, Player.Chatted */
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
    return used;
}

static const char *mode_name(const ub_frame *frame)
{
    if (frame->backend == UB_BACKEND_OPENGL) return ub_render_quality_name(ub_render_quality());
    bool idx8 = frame->fb && frame->fb->format == UB_PIXEL_INDEX8;
    if (ub_render_fast()) {
        static const char *const k[2][UB_QUALITY_COUNT] = { { "32-bit low", "32-bit medium", "32-bit high" },
                                                            { "8-bit low", "8-bit medium", "8-bit high" } };
        return k[idx8][UB_CLAMP((int)ub_render_quality(), 0, UB_QUALITY_COUNT - 1)];
    }
    return idx8 ? "8-bit" : "32-bit";
}

/* what the menu changed, applied now */
static void apply_menu(int f, ub_frame *frame)
{
    if (f & SET_FPS) plat_set_fps_cap(g_prefs.fps_cap);
    if (f & SET_FULLSCREEN) plat_set_fullscreen(g_prefs.fullscreen);
    if (f & SET_FSMODE) plat_set_fullscreen_mode(g_prefs.fs_width, g_prefs.fs_height);
    if (f & SET_QUALITY) ub_render_set_quality((ub_quality)g_quality);
    if (f & SET_SWRES) plat_set_sw_resolution(g_prefs.sw_height);
    if (f & SET_COLORS)
        plat_set_pixel_format(g_colors_cfg == 8 || (g_colors_cfg == 0 && g_sys.palette_display) ? UB_PIXEL_INDEX8 : UB_PIXEL_XRGB32);
    if (f & SET_PRESENT) plat_set_present_changed_only(g_set.changed_only);
    if (f & SET_SOUND) apply_volume();
    if (f & SET_CONTROLS) apply_controls();
    if (f & SET_RESTART_NEEDED) ub_fmt(g_status, sizeof g_status, "Some changes take effect when Uniblox starts again");
    if (f & 0xFFFF) save_config(); /* changes are kept at once */
    if (f & MENU_RESUME) {
        g_menu_open = false;
        ub_info("menu closed");
    }
    if (f & MENU_RELOAD) {
        g_menu_open = false;
        reload_place();
    }
    if (f & MENU_QUIT) {
        ub_info("quitting from the menu");
        plat_request_quit();
    }
    (void)frame;
}

static void start_benchmark(ub_frame *frame)
{
    g_bench = true;
    g_bench_t = 0;
    g_bench_frames = 0;
    g_bench_gui_us = g_bench_render_us = g_bench_scene_us = g_bench_present_us = g_bench_polys = g_bench_pixels = 0;
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
    u32 sms10 = (u32)(g_bench_scene_us / g_bench_frames / 100u);
    u32 gms10 = (u32)(g_bench_gui_us / g_bench_frames / 100u);
    const char *backend = frame->backend == UB_BACKEND_OPENGL ? "OpenGL" : ub_render_fast() ? "fast software" : "software";
    int rw = frame->fb ? frame->fb->width : frame->width, rh = frame->fb ? frame->fb->height : frame->height;
    ub_fmt(g_bench_result, sizeof g_bench_result,
           "Benchmark: %u.%u fps at %dx%d (%s %s; CPU %u.%u ms, present %u.%u ms)",
           fps10 / 10, fps10 % 10, frame->width, frame->height, backend, mode_name(frame),
           ms10 / 10, ms10 % 10, pms10 / 10, pms10 % 10);
    ub_info("benchmark: %u frames in %u ms = %u.%u fps; render %u.%u ms/frame (3D scene %u.%u; GUI %u.%u), present %u.%u ms/frame; "
            "%u polys, %u pixels per frame; window %dx%d, render %dx%d; %s %s; %s; %s; %s%s%s",
            g_bench_frames, (u32)(g_bench_t * 1000.0f), fps10 / 10, fps10 % 10, ms10 / 10, ms10 % 10, sms10 / 10, sms10 % 10, gms10 / 10, gms10 % 10,
            pms10 / 10, pms10 % 10,
            (u32)(g_bench_polys / g_bench_frames), (u32)(g_bench_pixels / g_bench_frames),
            frame->width, frame->height, rw, rh, backend, mode_name(frame), g_renderer_desc,
            g_sys.cpu_desc, g_sys.os_name, frame->present_detail[0] ? "; present: " : "",
            frame->present_detail);
    if (g_auto_bench) {
        ub_info("benchmark: done (-benchmark); quitting");
        plat_request_quit();
    }
}

/* The client UI's keys and clicks (app_hud.c): the game menu (Esc), its
 * tabs and buttons (R reset, L leave, S settings, Enter yes), the player
 * list (Tab), the chat button and the chat box. Returns true if it used
 * this frame's input, so the game does not see it. */
static bool hud_input(const ub_frame *frame, const ub_input *in, bool keys_free)
{
    int sw, sh, act = HUD_DO_NONE;
    play_gui_screen_size(frame, &sw, &sh);
    bool menu = hud_menu_open(), used = menu;
    if (menu) {
        int c = hud_confirming();
        if (in->key_pressed[UB_KEY_ESCAPE]) act = c ? HUD_DO_CANCEL : HUD_DO_RESUME;
        else if (c && in->key_pressed[UB_KEY_ENTER]) act = c == 1 ? HUD_DO_RESET : HUD_DO_LEAVE;
        else if (!c && in->key_pressed['R']) act = HUD_ASK_RESET;
        else if (!c && in->key_pressed['L']) act = HUD_ASK_LEAVE;
        else if (!c && in->key_pressed['S']) act = HUD_DO_SETTINGS;
        else if (in->key_pressed[UB_KEY_TAB]) hud_set_tab(-1); /* the next tab */
    } else if (keys_free && in->key_pressed[UB_KEY_ESCAPE]) {
        hud_set_menu(true);
        ub_info("game menu opened");
        return true;
    } else if (keys_free && in->key_pressed[UB_KEY_TAB]) {
        hud_toggle_list();
    }
    if (in->mouse_pressed[UB_MOUSE_LEFT] && !in->mouse_captured && frame->width > 0 && frame->height > 0) {
        int x = in->mouse_x * sw / frame->width, y = in->mouse_y * sh / frame->height;
        int hit = hud_hit(x, y);
        if (hit >= 0) used = true;
        if (hit == HUD_HIT_MENU) {
            if (menu) act = HUD_DO_RESUME;
            else {
                hud_set_menu(true);
                ub_info("game menu opened");
            }
        } else if (hit == HUD_HIT_CHAT) hud_toggle_chat();
        else if (hit == HUD_HIT_CHAT_INPUT && !g_typing) {
            g_typing = true;
            g_typed[0] = 0;
        } else if (hit == HUD_HIT_LIST) hud_toggle_list();
        else if (hit >= HUD_HIT_TAB_PEOPLE && hit <= HUD_HIT_TAB_HELP) hud_set_tab(hit - HUD_HIT_TAB_PEOPLE);
        else if (hit >= HUD_HIT_BTN0 && hit <= HUD_HIT_BTN2) act = hud_button_meaning(hit - HUD_HIT_BTN0);
    }
    switch (act) {
    case HUD_DO_RESUME:
        hud_set_menu(false);
        ub_info("game menu closed");
        break;
    case HUD_ASK_RESET:
        if (!g_game.hud.reset_off) hud_confirm(1);
        break;
    case HUD_ASK_LEAVE: hud_confirm(2); break;
    case HUD_DO_CANCEL: hud_confirm(0); break;
    case HUD_DO_RESET:
        hud_set_menu(false);
        ub_info("reset character from the menu");
        ub_game_reset_character(&g_game);
        break;
    case HUD_DO_LEAVE:
        ub_info("leaving from the game menu");
        plat_request_quit();
        break;
    case HUD_DO_SETTINGS:
        hud_set_menu(false);
        g_menu_open = true; /* the settings menu (it reads Esc itself from the next frame) */
        g_menu.page = 1; /* straight to the settings: the game menu is the paused page */
        g_menu.opengl = frame->backend == UB_BACKEND_OPENGL;
        g_menu.fast = ub_render_fast();
        ub_info("menu opened");
        break;
    default: break;
    }
    return used || act != HUD_DO_NONE;
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

    /* --- the place's GUI first: it may take the mouse, the wheel or the
     * keyboard (a TextBox), and then the game does not get them --- */
    if (g_game_on) {
        int sw, sh;
        play_gui_screen_size(frame, &sw, &sh);
        hud_frame(sw, sh, g_typing ? g_typed : NULL, dt);
    }
    bool gui_keys = g_game.gui && ub_gui_keyboard_taken(g_game.gui);
    if (g_game.gui) {
        if (!g_menu_open && !g_bench && !g_typing && !hud_menu_open()) {
            play_gui_input(&g_game, frame, in, g_have_cam ? &g_last_cam : NULL, in->mouse_captured, g_time);
        } else {
            int sw, sh;
            play_gui_screen_size(frame, &sw, &sh);
            ub_gui_update(g_game.gui, GW, sw, sh, g_have_cam ? &g_last_cam : NULL, NULL, g_time);
        }
        gui_keys = gui_keys || ub_gui_keyboard_taken(g_game.gui);
    }
    bool gui_mouse = g_game.gui && ub_gui_mouse_taken(g_game.gui);
    ub_input pin = *in; /* what the camera and movement see */
    if (g_game.gui && ub_gui_wheel_taken(g_game.gui)) pin.wheel = 0;

    /* --- text entry: while it is open, keys do not move us --- */
    bool typed = !g_bench && !gui_keys && !hud_menu_open() && handle_typing(in);
    bool menu_was_open = g_menu_open;
    bool play_keys = !typed && !g_typing && !gui_keys && !g_bench && !g_menu_open;
    bool hud_used = !g_bench && !g_menu_open && g_game_on && hud_input(frame, in, play_keys);
    if (hud_used || hud_menu_open()) play_keys = false;
    if (hud_used) gui_mouse = true; /* (the click was the UI's) */

    /* --- function keys --- */
    if (play_keys && in->key_pressed[UB_KEY_F3]) g_hide_overlay = !g_hide_overlay;
    if ((in->key_pressed[UB_KEY_F11] && !g_typing) ||
        (in->key_down[UB_KEY_ALT] && in->key_pressed[UB_KEY_ENTER])) {
        g_prefs.fullscreen = !frame->fullscreen;
        plat_set_fullscreen(g_prefs.fullscreen);
    }
    if ((play_keys && in->key_pressed[UB_KEY_F5]) || (g_auto_bench && !g_bench && g_time > 1.5f && !g_bench_result[0]))
        start_benchmark(frame);
    else if (g_bench && (in->key_pressed[UB_KEY_ESCAPE] || in->key_pressed[UB_KEY_F5])) {
        finish_benchmark(frame, true);
        play_keys = false; /* this Escape only cancels */
    }
    if (play_keys && in->key_pressed[UB_KEY_F6]) {
        if (frame->backend == UB_BACKEND_OPENGL || ub_render_fast()) {
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

    /* --- camera controls and movement --- */
    vec3 move;
    bool jump;
    play_view_input(&g_view, &g_game, &pin, dt, play_keys, !g_bench && !g_menu_open && !hud_menu_open(), &move, &jump);

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
    play_view_update(&g_view, &g_game, dt);

    /* --- other input --- */
    if (play_keys && in->key_pressed['M']) {
        g_muted = !g_muted;
        apply_volume();
        ub_debug("mute %s", g_muted ? "on" : "off");
    }
    if (!g_bench && in->mouse_pressed[UB_MOUSE_LEFT] && !gui_mouse) {
        ub_play_params p = ub_play_default();
        p.pan = (f32)(in->mouse_x * 2 - frame->width) / (f32)frame->width;
        p.pitch = 2.0f - 1.5f * (f32)in->mouse_y / (f32)frame->height;
        ub_mixer_play(&g_snd_click, p);
        g_clicks++;
        ub_debug("click %u at %d,%d", g_clicks, in->mouse_x, in->mouse_y);
    }
    if (play_keys && in->key_pressed[UB_KEY_ESCAPE] && !g_menu_open && !g_game_on) {
        g_menu_open = true; /* no game: straight to the settings (it reads Esc itself from the next frame) */
        g_menu.page = 0;
        g_menu.opengl = frame->backend == UB_BACKEND_OPENGL;
        g_menu.fast = ub_render_fast();
        ub_info("menu opened");
        play_keys = false;
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
        cam.fov_y = 70.0f * UB_PI / 180.0f;
        cam.far_dist = 600.0f;
    } else {
        cam = play_view_camera(&g_view, &g_game);
    }

    /* --- render --- */
    ub_render_env env = play_env(GW);
    ub_render_begin(frame, &cam, &env);
    play_view_draw(&g_view, &g_game, !g_bench);
    play_gui_draw_world(&g_game, &cam);
    ub_render_end_scene();
    play_gui_draw_screen(&g_game);
    g_last_cam = cam;
    g_have_cam = true;

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
    if ((!g_hide_overlay || g_bench) && !g_menu_open) {
        int lh = ub_render_line_height(), x = lh / 2, y = g_game_on && !g_bench ? hud_below_chat() : lh / 3;
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
            if (g_game.gui) {
                ub_gui_stats gs;
                ub_gui_get_stats(g_game.gui, &gs);
                if (gs.layers)
                    LINE("GUI: %u layer(s), %u objects; layout %u.%u ms, drawing %u.%u ms", gs.layers, gs.objects,
                         gs.layout_us / 1000u, gs.layout_us / 100u % 10u, gs.drawn_us / 1000u, gs.drawn_us / 100u % 10u);
            }
            if (g_game.scripts) {
                ub_script_stats st;
                ub_script_get_stats(&st);
                u32 s10 = g_game.script_us / 100u;
                LINE("Scripts: %u running, %u waiting, %u KB, %u.%u ms%s", st.scripts, st.threads, st.memory_kb,
                     s10 / 10, s10 % 10, st.errors ? "  - errors below" : "");
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
            if (g_demo_world && !g_last_message[0]) LINE("Chat (/): fast, slow, night, day");
            LINE("F5 benchmark, F6 %s, F7 save place, F8 colours, F9 fps cap, F3 hide this text",
                 frame->backend == UB_BACKEND_OPENGL || ub_render_fast() ? "quality" : "resolution");
        }
#undef LINE
    }
    if (g_menu_open) {
        ub_input mi = *in;
        if (!menu_was_open) mi.key_pressed[UB_KEY_ESCAPE] = false; /* (this Esc opened it) */
        ui_begin(&mi, frame->width, frame->height, dt);
        int f = settings_menu(&g_menu, &g_set, &mi);
        ui_end();
        apply_menu(f, frame);
        if (g_menu_open && g_game_on && g_menu.page == 0) {
            /* Back (or Esc) from the settings: to the game menu, which replaces the paused page */
            g_menu_open = false;
            hud_set_menu(true);
            hud_set_tab(1);
            ub_info("menu closed");
        }
    }
    ub_render_end(&g_stats);

    if (g_bench) {
        g_bench_frames++;
        g_bench_render_us += g_stats.render_us;
        g_bench_scene_us += g_stats.scene_us;
        if (g_game.gui) {
            ub_gui_stats gs;
            ub_gui_get_stats(g_game.gui, &gs);
            g_bench_gui_us += gs.layout_us + gs.drawn_us;
        }
        g_bench_present_us += frame->present_us;
        g_bench_polys += g_stats.polys_drawn;
        g_bench_pixels += g_stats.pixels_tested;
        if (g_bench_t >= (f32)BENCH_SECONDS) finish_benchmark(frame, false);
    }
}
