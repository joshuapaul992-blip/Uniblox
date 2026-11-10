/* Uniblox - the game's settings (see app_settings.h). */
#include "app/app_settings.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "render/ub_render.h"
#include "ui/ub_ui.h"
#include "crt/ub_crt.h"

static const int k_sw_heights[] = { 240, 360, 480, 720, 0 };   /* 0 = window height */
static const u32 k_fps_caps[] = { 30, 60, 120, 144, 240, 0 };  /* 0 = unlimited */

void settings_defaults(ub_settings *s)
{
    memset(s, 0, sizeof *s);
    s->prefs.fps_cap = 120;
    s->prefs.sw_height = 480;
    s->quality = UB_QUALITY_HIGH;
    s->gl_shaders = true;
    s->changed_only = true;
    s->volume = 80;
    s->mouse_pct = 100;
    s->fov = 70;
    s->scripts = true;
    s->zoom = 22;
    s->pitch_deg = -20;
}

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

static bool str_is(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

void settings_parse(ub_settings *s, const char *text)
{
    const char *line = text;
    while (*line) {
        char buf[300];
        int n = 0;
        while (line[n] && line[n] != '\n' && line[n] != '\r' && n < 299) {
            buf[n] = line[n];
            ++n;
        }
        buf[n] = 0;
        line += n;
        while (*line && *line != '\n' && *line != '\r') ++line;
        while (*line == '\n' || *line == '\r') ++line;
        const char *v;
        ub_display_prefs *p = &s->prefs;
        if (key_is(buf, "renderer", &v))
            p->backend = str_is(v, "opengl") ? UB_BACKEND_PREF_OPENGL : str_is(v, "software") ? UB_BACKEND_PREF_SOFTWARE : UB_BACKEND_PREF_AUTO;
        else if (key_is(buf, "fullscreen", &v)) p->fullscreen = parse_int(v) != 0;
        else if (key_is(buf, "fullscreen_w", &v)) p->fs_width = UB_CLAMP(parse_int(v), 0, 16384);
        else if (key_is(buf, "fullscreen_h", &v)) p->fs_height = UB_CLAMP(parse_int(v), 0, 16384);
        else if (key_is(buf, "window_w", &v)) p->window_w = parse_int(v);
        else if (key_is(buf, "window_h", &v)) p->window_h = parse_int(v);
        else if (key_is(buf, "fps_cap", &v)) p->fps_cap = (u32)UB_CLAMP(parse_int(v), 0, 1000);
        else if (key_is(buf, "vsync", &v)) p->vsync = parse_int(v) != 0;
        else if (key_is(buf, "sw_height", &v)) p->sw_height = UB_CLAMP(parse_int(v), 0, 4320);
        else if (key_is(buf, "timer", &v)) p->timer_qpc = str_is(v, "qpc");
        else if (key_is(buf, "quality", &v)) s->quality = UB_CLAMP(parse_int(v), 0, UB_QUALITY_COUNT - 1);
        else if (key_is(buf, "gl_shaders", &v)) s->gl_shaders = parse_int(v) != 0;
        else if (key_is(buf, "colors", &v)) {
            i32 c = parse_int(v);
            s->colors = (c == 8 || c == 32) ? c : 0;
        } else if (key_is(buf, "send_changes_only", &v)) s->changed_only = parse_int(v) != 0;
        else if (key_is(buf, "overlay", &v)) s->hide_overlay = parse_int(v) == 0;
        else if (key_is(buf, "volume", &v)) s->volume = UB_CLAMP(parse_int(v), 0, 100);
        else if (key_is(buf, "muted", &v)) s->muted = parse_int(v) != 0;
        else if (key_is(buf, "mouse", &v)) s->mouse_pct = UB_CLAMP(parse_int(v), 10, 500);
        else if (key_is(buf, "invert_y", &v)) s->invert_y = parse_int(v) != 0;
        else if (key_is(buf, "fov", &v)) s->fov = UB_CLAMP(parse_int(v), 40, 110);
        else if (key_is(buf, "scripts", &v)) s->scripts = parse_int(v) != 0;
        else if (key_is(buf, "zoom", &v)) s->zoom = parse_int(v);
        else if (key_is(buf, "yaw_deg", &v)) s->yaw_deg = parse_int(v);
        else if (key_is(buf, "pitch_deg", &v)) s->pitch_deg = parse_int(v);
        else if (key_is(buf, "place", &v)) ub_fmt(s->place, sizeof s->place, "%s", v);
    }
}

int settings_write(const ub_settings *s, char *buf, int cap)
{
    static const char *const renderer[] = { "auto", "opengl", "software" };
    const ub_display_prefs *p = &s->prefs;
    return ub_fmt(buf, cap,
                  "# Uniblox settings (written on exit, or by the menu: Esc)\r\n"
                  "# renderer: auto (OpenGL if available), opengl or software\r\n"
                  "renderer=%s\r\n"
                  "# quality (OpenGL): 0 = low, 1 = medium (shadows), 2 = high (shadows + anti-aliasing)\r\n"
                  "quality=%d\r\n"
                  "# gl_shaders: 1 = full effects, 0 = basic OpenGL 1.1 (for problem drivers)\r\n"
                  "gl_shaders=%d\r\n"
                  "# fps_cap: frame rate limit, 0 = unlimited\r\n"
                  "fps_cap=%u\r\nvsync=%d\r\n"
                  "fullscreen=%d\r\nwindow_w=%d\r\nwindow_h=%d\r\n"
                  "# fullscreen_w, fullscreen_h: the screen's mode while fullscreen (NT 4.0 and later), 0 = the desktop's\r\n"
                  "fullscreen_w=%d\r\nfullscreen_h=%d\r\n"
                  "# sw_height (software renderer): render height, 0 = window height\r\n"
                  "sw_height=%d\r\n"
                  "# colors (software renderer): 0 = automatic, 8 = 256-colour palette, 32 = full colour\r\n"
                  "colors=%d\r\n"
                  "# send_changes_only (software renderer): 1 = send the screen only what changed\r\n"
                  "send_changes_only=%d\r\n"
                  "# timer: auto (the CPU's counter when it is steady) or qpc (Windows' timer)\r\n"
                  "timer=%s\r\n"
                  "overlay=%d\r\nvolume=%d\r\nmuted=%d\r\n"
                  "# mouse: mouse-look speed in percent; fov: field of view in degrees\r\n"
                  "mouse=%d\r\ninvert_y=%d\r\nfov=%d\r\n"
                  "zoom=%d\r\nyaw_deg=%d\r\npitch_deg=%d\r\n"
                  "# scripts: 1 = run the place's scripts, 0 = do not\r\n"
                  "scripts=%d\r\n"
                  "# place: place file to open at start, relative to this folder (.ubx, .rbxl or .rbxm),\r\n"
                  "# e.g. places/castle.ubx. Empty = the built-in demo. A file named on the command line wins\r\n"
                  "place=%s\r\n",
                  renderer[p->backend % 3], s->quality, s->gl_shaders, p->fps_cap, p->vsync, p->fullscreen,
                  p->window_w, p->window_h, p->fs_width, p->fs_height, p->sw_height, s->colors, s->changed_only,
                  p->timer_qpc ? "qpc" : "auto", !s->hide_overlay, s->volume, s->muted, s->mouse_pct, s->invert_y,
                  s->fov, s->zoom, s->yaw_deg, s->pitch_deg, s->scripts, s->place);
}

/* ------------------------------------------------------------ the menu */

static int step_index(int n, int cur, int dir) { return (cur + dir + n) % n; }

static bool on_off(const char *label, bool *v)
{
    if (ui_stepper(label, *v ? "On" : "Off")) {
        *v = !*v;
        return true;
    }
    return false;
}

static int display_page(ub_menu *m, ub_settings *s)
{
    int f = 0, d;
    ub_display_prefs *p = &s->prefs;
    static const char *const renderers[] = { "Automatic", "OpenGL", "Software" };
    if ((d = ui_stepper("Renderer (restart)", renderers[p->backend % 3])) != 0) {
        p->backend = (ub_backend_pref)step_index(3, (int)p->backend, d);
        f |= SET_RESTART_NEEDED;
    }
    if (on_off("Fullscreen", &p->fullscreen)) f |= SET_FULLSCREEN;
    /* the screen's mode while fullscreen: NT 4.0 and later */
    if (!m->modes_listed) {
        m->mode_count = plat_display_modes(m->modes, 48);
        m->modes_listed = true;
    }
    char txt[48];
    if (m->mode_count) {
        int cur = -1;
        for (int i = 0; i < m->mode_count; ++i)
            if (m->modes[i].w == p->fs_width && m->modes[i].h == p->fs_height) cur = i;
        if (cur >= 0) ub_fmt(txt, sizeof txt, "%d x %d", p->fs_width, p->fs_height);
        else ub_fmt(txt, sizeof txt, "The desktop's");
        if ((d = ui_stepper("Fullscreen size", txt)) != 0) {
            int i = cur + d; /* -1 = the desktop's */
            if (i < -1) i = m->mode_count - 1;
            if (i >= m->mode_count) i = -1;
            p->fs_width = i < 0 ? 0 : m->modes[i].w;
            p->fs_height = i < 0 ? 0 : m->modes[i].h;
            f |= SET_FSMODE;
        }
    } else {
        ui_stepper("Fullscreen size", "The desktop's (NT 3.51)");
    }
    if (m->opengl) {
        if ((d = ui_stepper("Graphics quality", ub_render_quality_name((ub_quality)s->quality))) != 0) {
            s->quality = step_index(UB_QUALITY_COUNT, s->quality, d);
            f |= SET_QUALITY;
        }
    } else {
        int i = 0;
        while (i < 4 && k_sw_heights[i] != p->sw_height) ++i;
        if (p->sw_height) ub_fmt(txt, sizeof txt, "%d lines", p->sw_height);
        else ub_fmt(txt, sizeof txt, "The window's");
        if ((d = ui_stepper("Render resolution", txt)) != 0) {
            p->sw_height = k_sw_heights[step_index(5, i, d)];
            f |= SET_SWRES;
        }
        if ((d = ui_stepper("Colours", s->colors == 8 ? "256 colours" : s->colors == 32 ? "Full colour" : "Automatic")) != 0) {
            static const int cyc[] = { 0, 32, 8 };
            int k = s->colors == 32 ? 1 : s->colors == 8 ? 2 : 0;
            s->colors = cyc[step_index(3, k, d)];
            f |= SET_COLORS;
        }
        if (on_off("Send only what changed", &s->changed_only)) f |= SET_PRESENT;
    }
    int i = 0;
    while (i < 5 && k_fps_caps[i] != p->fps_cap) ++i;
    if (p->fps_cap) ub_fmt(txt, sizeof txt, "%u fps", p->fps_cap);
    else ub_fmt(txt, sizeof txt, "Unlimited");
    if ((d = ui_stepper("Frame rate cap", txt)) != 0) {
        p->fps_cap = k_fps_caps[step_index(6, i, d)];
        f |= SET_FPS;
    }
    bool shown = !s->hide_overlay;
    if (on_off("Text overlay (F3)", &shown)) {
        s->hide_overlay = !shown;
        f |= SET_OVERLAY;
    }
    return f;
}

static int controls_page(ub_settings *s)
{
    int f = 0, d;
    char txt[32];
    ub_fmt(txt, sizeof txt, "%d%%", s->volume);
    if ((d = ui_stepper("Volume", txt)) != 0) {
        s->volume = UB_CLAMP(s->volume + d * 10, 0, 100);
        f |= SET_SOUND;
    }
    if (on_off("Mute (M)", &s->muted)) f |= SET_SOUND;
    ub_fmt(txt, sizeof txt, "%d%%", s->mouse_pct);
    if ((d = ui_stepper("Mouse-look speed", txt)) != 0) {
        s->mouse_pct = UB_CLAMP(s->mouse_pct + d * (s->mouse_pct >= 200 ? 50 : 10), 10, 500);
        f |= SET_CONTROLS;
    }
    if (on_off("Invert mouse up/down", &s->invert_y)) f |= SET_CONTROLS;
    ub_fmt(txt, sizeof txt, "%d degrees", s->fov);
    if ((d = ui_stepper("Field of view", txt)) != 0) {
        s->fov = UB_CLAMP(s->fov + d * 5, 40, 110);
        f |= SET_CONTROLS;
    }
    return f;
}

static int game_page(ub_settings *s)
{
    int f = 0;
    if (on_off("Scripts (next place)", &s->scripts)) f |= SET_RESTART_NEEDED;
    bool steady = !s->prefs.timer_qpc;
    if (ui_stepper("Timer (restart)", steady ? "CPU counter if steady" : "Windows' timer")) {
        s->prefs.timer_qpc = steady;
        f |= SET_RESTART_NEEDED;
    }
    ui_space(6);
    if (ui_button("Load the place again")) f |= MENU_RELOAD;
    return f;
}

int settings_menu(ub_menu *m, ub_settings *s, const ub_input *in)
{
    int f = 0;
    int lh = ui_line_height();
    if (m->page == 0) {
        ui_dialog_begin("Paused", 320, lh * 6 + 40);
        if (ui_button("Resume (Esc)")) f |= MENU_RESUME;
        if (ui_button("Settings")) m->page = 1;
        if (ui_button("Load the place again")) f |= MENU_RELOAD;
        if (ui_button("Quit (Q)")) f |= MENU_QUIT;
        ui_dialog_end();
        if (in->key_pressed['Q']) f |= MENU_QUIT;
    } else {
        ui_dialog_begin("Settings", 560, lh * 13 + 40);
        ui_columns(3);
        if (ui_toggle("Display", m->page == 1)) m->page = 1;
        if (ui_toggle("Sound and controls", m->page == 2)) m->page = 2;
        if (ui_toggle("Game", m->page == 3)) m->page = 3;
        ui_space(4);
        if (m->page == 1) f |= display_page(m, s);
        else if (m->page == 2) f |= controls_page(s);
        else f |= game_page(s);
        ui_space(8);
        if (ui_button("Back")) m->page = 0;
        ui_dialog_end();
    }
    if (in->key_pressed[UB_KEY_ESCAPE]) {
        if (m->page) m->page = 0;
        else f |= MENU_RESUME;
    }
    return f;
}
