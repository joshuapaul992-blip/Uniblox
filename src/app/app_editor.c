/* Uniblox Editor (ubedit.exe) - build places, then play-test them.
 *
 *   Menus along the top; the Explorer (every object in the place) on the
 *   left; Properties of what is selected on the right; the 3D view in the
 *   middle.
 *
 *   3D view: hold the right mouse button to look around, WASD to fly (Q/E
 *   down/up, Shift faster), wheel to move forwards and back, F to look at
 *   the selection. Click to select (Shift or Ctrl adds; Alt picks a part
 *   inside a model). Drag a selected part to slide it along the ground.
 *   Tools: 1 Select, 2 Move (arrows along the axes), 3 Scale (pull a
 *   face), 4 Rotate. R turns the selection 90 degrees, T tips it over.
 *
 *   Ctrl+Z/Y undo/redo, Ctrl+C/X/V copy/cut/paste, Ctrl+D duplicate,
 *   Delete, Ctrl+G group, Ctrl+U ungroup, Ctrl+A select all, Ctrl+N/O/S
 *   new/open/save, F5 play test (F5 or Esc stops).
 *
 * Places are saved as places/<name>.ubx, which uniblox.exe plays. */
#include "app/app.h"
#include "app/app_editor.h"
#include "app/app_play.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "editor/ub_editor.h"
#include "game/ub_game.h"
#include "render/ub_mesh.h"
#include "render/ub_render.h"
#include "render/ub_texture.h"
#include "script/ub_script.h"
#include "ui/ub_ui.h"
#include "world/ub_rbxl.h"
#include "crt/ub_crt.h"

#define CONFIG_PATH "ubedit.cfg"
#define PLACE_MAX_BYTES (512u << 20)
#define SCRIPT_MAX (64u << 10)
#define MAX_FILES 96
#define MAX_OUTLINED 256

enum { TOOL_SELECT, TOOL_MOVE, TOOL_SCALE, TOOL_ROTATE };
enum { DLG_NONE, DLG_OPEN, DLG_SAVE_AS, DLG_CONFIRM, DLG_SCRIPT, DLG_HELP };
enum { AFTER_NOTHING, AFTER_NEW, AFTER_OPEN };
enum { DRAG_NONE, DRAG_SLIDE, DRAG_AXIS, DRAG_FACE };

static ub_display_prefs g_prefs;
static ub_system_info g_sys;
static char g_renderer_desc[160];
static bool g_gl_shaders = true;
static int g_quality = UB_QUALITY_HIGH;
static int g_win_w, g_win_h;

static ub_editor E;
static char g_path[260];                 /* the open place file (game path), "" if new */
static char g_status[200];
static f32 g_status_until;
static int g_tool = TOOL_SELECT;
static u8 *g_expand;                     /* explorer: expanded instances */
static i32 g_expand_cap;
static u32 g_expand_scene = 0xFFFFFFFFu;

/* the editor's camera */
static vec3 g_cam_pos = { 0, 18, -32 };
static f32 g_cam_yaw = 0, g_cam_pitch = -0.45f; /* looking along +z, like the game: lit faces towards us */

/* drawing */
static ub_part_draw *g_statics;
static int g_static_count, g_static_cap;
static u32 g_static_scene = 0xFFFFFFFFu, g_static_sel = 0xFFFFFFFFu;
static ub_camera g_camera;
static int g_w, g_h;
static ui_rect g_view_rect;

/* dragging in the 3D view */
static struct {
    int mode;
    vec3 axis, anchor;        /* DRAG_AXIS / DRAG_FACE: the line dragged along */
    f32 t0, applied;
    f32 plane_y;              /* DRAG_SLIDE: the plane grabbed */
    vec3 start;
    vec3 applied_v;
    i32 part;
    int face;
    bool moved;
} D;
static int g_hover_handle = -1;
static const ub_input *g_in;

/* play test */
static bool g_playing;
static ub_game g_game;
static ub_play_view g_view;

/* dialogs */
static int g_dlg, g_after;
static char g_save_name[64];
static char g_files[MAX_FILES][100];
static int g_nfiles;
static i32 g_script = -1;
static char *g_script_buf;
static ui_text_state g_script_st;

static void status(const char *msg)
{
    ub_fmt(g_status, sizeof g_status, "%s", msg);
    g_status_until = ui_time() + 6.0f;
    ub_info("editor: %s", msg);
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

static bool str_is(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

static bool ends_with(const char *s, const char *tail)
{
    int n = (int)ub_strlen(s), m = (int)ub_strlen(tail);
    if (m > n) return false;
    for (int i = 0; i < m; ++i) {
        char a = s[n - m + i], b = tail[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
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
        if (key_is(line, "renderer", &v))
            g_prefs.backend = str_is(v, "opengl") ? UB_BACKEND_PREF_OPENGL
                            : str_is(v, "software") ? UB_BACKEND_PREF_SOFTWARE : UB_BACKEND_PREF_AUTO;
        else if (key_is(line, "fullscreen", &v)) g_prefs.fullscreen = parse_int(v) != 0;
        else if (key_is(line, "window_w", &v)) g_prefs.window_w = parse_int(v);
        else if (key_is(line, "window_h", &v)) g_prefs.window_h = parse_int(v);
        else if (key_is(line, "fps_cap", &v)) g_prefs.fps_cap = (u32)UB_CLAMP(parse_int(v), 0, 1000);
        else if (key_is(line, "sw_height", &v)) g_prefs.sw_height = UB_CLAMP(parse_int(v), 0, 4320);
        else if (key_is(line, "quality", &v)) g_quality = UB_CLAMP(parse_int(v), 0, UB_QUALITY_COUNT - 1);
        else if (key_is(line, "gl_shaders", &v)) g_gl_shaders = parse_int(v) != 0;
        else if (key_is(line, "grid_x100", &v)) E.grid = (f32)UB_CLAMP(parse_int(v), 0, 10000) / 100.0f;
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
    char text[1024];
    int n = ub_fmt(text, sizeof text,
                   "# Uniblox Editor settings (written on exit)\r\n"
                   "# renderer: auto, opengl or software\r\n"
                   "renderer=%s\r\nquality=%d\r\ngl_shaders=%d\r\nfps_cap=%u\r\n"
                   "fullscreen=%d\r\nwindow_w=%d\r\nwindow_h=%d\r\n"
                   "# sw_height (software renderer): 0 = the window's size (sharpest text)\r\n"
                   "sw_height=%d\r\n"
                   "# grid_x100: grid size in hundredths of a stud (100 = 1 stud, 0 = off)\r\n"
                   "grid_x100=%d\r\n",
                   renderer[g_prefs.backend], g_quality, g_gl_shaders, g_prefs.fps_cap, g_prefs.fullscreen,
                   g_win_w, g_win_h, g_prefs.sw_height, ub_roundf(E.grid * 100.0f));
    if (plat_file_write_all(CONFIG_PATH, text, (u32)n)) ub_info("saved %s", CONFIG_PATH);
}

/* ------------------------------------------------------------ files */

static void set_title(void)
{
    char t[128];
    ub_fmt(t, sizeof t, "Uniblox Editor - %s%s", E.world.name, E.dirty ? " *" : "");
    plat_set_title(t);
}

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

static void look_at_place(void);

static bool open_place(const char *path, bool user_path)
{
    void *data;
    u32 size;
    bool ok = user_path ? plat_file_read_user(path, PLACE_MAX_BYTES, &data, &size)
                        : plat_file_read_all(path, PLACE_MAX_BYTES, &data, &size);
    char msg[300];
    if (!ok) {
        ub_fmt(msg, sizeof msg, "Could not open %s", path);
        status(msg);
        return false;
    }
    bool roblox = ub_rbxl_detect((const u8 *)data, size);
    ub_load_result r = ed_load(&E, (const u8 *)data, size, roblox);
    plat_free(data);
    if (r != UB_LOAD_OK) {
        ub_fmt(msg, sizeof msg, "Could not load %s: %s", path, ub_load_result_name(r));
        status(msg);
        return false;
    }
    if (roblox) {
        base_name(path, E.world.name, sizeof E.world.name);
        g_path[0] = 0; /* save it as .ubx */
    } else {
        ub_fmt(g_path, sizeof g_path, "%s", user_path ? "" : path);
    }
    ub_fmt(msg, sizeof msg, "Opened %s (%d parts)%s", path, E.world.part_count, roblox ? " - imported from Roblox" : "");
    status(msg);
    look_at_place();
    set_title();
    return true;
}

static void sanitize(const char *in, char *out, int cap)
{
    int n = 0;
    for (const char *p = in; *p && n < cap - 1 && n < 40; ++p) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out[n++] = ok ? c : '_';
    }
    if (!n) out[n++] = 'p';
    out[n] = 0;
}

static bool save_to(const char *path)
{
    u8 *data;
    u32 size;
    char msg[300];
    if (!plat_make_dir("places") || !ed_save(&E, &data, &size)) {
        ub_fmt(msg, sizeof msg, "Could not save %s", path);
        status(msg);
        return false;
    }
    bool ok = plat_file_write_all(path, data, size);
    plat_free(data);
    ub_fmt(msg, sizeof msg, "%s %s (%u bytes)", ok ? "Saved" : "Could not save", path, size);
    status(msg);
    if (ok) ub_fmt(g_path, sizeof g_path, "%s", path);
    else E.dirty = true;
    set_title();
    return ok;
}

static void save(void)
{
    if (g_path[0]) {
        save_to(g_path);
        return;
    }
    ub_fmt(g_save_name, sizeof g_save_name, "%s", E.world.name);
    g_dlg = DLG_SAVE_AS;
}

static void list_cb(void *user, const char *name, bool is_dir, u32 size)
{
    const char *dir = (const char *)user;
    (void)size;
    if (is_dir || g_nfiles >= MAX_FILES) return;
    if (!ends_with(name, ".ubx") && !ends_with(name, ".rbxl") && !ends_with(name, ".rbxm")) return;
    ub_fmt(g_files[g_nfiles++], sizeof g_files[0], "%s%s%s", dir, dir[0] ? "/" : "", name);
}

static void open_dialog(void)
{
    g_nfiles = 0;
    plat_list_files("places", list_cb, (void *)"places");
    plat_list_files("", list_cb, (void *)"");
    g_dlg = DLG_OPEN;
}

/* New and Open ask first when there are unsaved changes */
static void request(int what)
{
    if (E.dirty) {
        g_after = what;
        g_dlg = DLG_CONFIRM;
        return;
    }
    if (what == AFTER_NEW) {
        ed_new_place(&E);
        g_path[0] = 0;
        look_at_place();
        status("New place");
        set_title();
    } else if (what == AFTER_OPEN) {
        open_dialog();
    }
}

/* ----------------------------------------------------------- camera */

static vec3 cam_forward(void)
{
    f32 cp = ub_cosf(g_cam_pitch);
    return v3(ub_sinf(g_cam_yaw) * cp, ub_sinf(g_cam_pitch), ub_cosf(g_cam_yaw) * cp);
}

static void look_at_box(vec3 lo, vec3 hi)
{
    vec3 c = v3_scale(v3_add(lo, hi), 0.5f);
    f32 r = v3_length(v3_sub(hi, lo)) * 0.5f;
    if (r < 2) r = 2;
    if (r > 300) r = 300;
    vec3 f = cam_forward();
    g_cam_pos = v3_sub(c, v3_scale(f, r * 2.2f + 4));
}

static void look_at_place(void)
{
    /* the spawn (or the middle), from behind and above */
    i32 spawn = -1;
    for (i32 i = 0; i < E.world.count && spawn < 0; ++i)
        if (E.world.inst[i].cls == UB_CLASS_SPAWN && ub_world_alive(&E.world, i)) spawn = i;
    vec3 c = spawn >= 0 ? E.world.parts[E.world.inst[spawn].part].pos : v3(0, 0, 0);
    g_cam_yaw = 0;
    g_cam_pitch = -0.45f;
    g_cam_pos = v3_sub(c, v3_scale(cam_forward(), 36));
}

static void focus_selection(void)
{
    vec3 lo, hi;
    if (ed_selection_bounds(&E, &lo, &hi)) look_at_box(lo, hi);
}

/* --------------------------------------------------------- the view */

/* what is selected or inside the selection, and its parts: worked out
 * once per change of selection (not per frame: places can be big) */
static u8 *g_insel;
static i32 g_insel_cap, *g_selparts, g_nselparts, g_selparts_cap;
static u32 g_insel_sel = 0xFFFFFFFFu, g_insel_scene = 0xFFFFFFFFu;

static void update_insel(void)
{
    const ub_world *w = &E.world;
    if (g_insel_sel == E.sel_version && g_insel_scene == E.scene_version && g_insel_cap > w->count) return;
    if (g_insel_cap <= w->count) {
        plat_free(g_insel);
        plat_free(g_selparts);
        g_insel_cap = g_selparts_cap = w->count + 1024;
        g_insel = (u8 *)plat_alloc((size_t)g_insel_cap);
        g_selparts = (i32 *)plat_alloc(sizeof(i32) * (size_t)g_selparts_cap);
        if (!g_insel || !g_selparts) {
            plat_free(g_insel);
            plat_free(g_selparts);
            g_insel = NULL;
            g_selparts = NULL;
            g_insel_cap = g_selparts_cap = 0;
            return;
        }
    }
    g_nselparts = 0;
    for (i32 i = 0; i < w->count; ++i) {
        u8 in = 0;
        if (E.sel_count && ub_world_alive(w, i))
            for (i32 a = i; a >= 0 && !in; a = w->inst[a].parent) in = ed_selected(&E, a);
        g_insel[i] = in;
        if (in && w->inst[i].part >= 0) g_selparts[g_nselparts++] = i;
    }
    g_insel_sel = E.sel_version;
    g_insel_scene = E.scene_version;
}

static void rebuild_statics(void)
{
    const ub_world *w = &E.world;
    update_insel();
    if (g_static_cap < w->part_count + 1) {
        plat_free(g_statics);
        g_static_cap = w->part_count + 1024;
        g_statics = (ub_part_draw *)plat_alloc(sizeof(ub_part_draw) * (size_t)g_static_cap);
        if (!g_statics) g_static_cap = 0;
    }
    /* parts that are selected (or inside a selection) are drawn live */
    const u8 *live = g_insel;
    g_static_count = 0;
    i32 ws = w->workspace;
    for (i32 i = 0; i < w->count && g_statics; ++i) {
        const ub_instance *in = &w->inst[i];
        if (in->part < 0 || !ub_world_alive(w, i) || (live && live[i])) continue;
        if (w->parts[in->part].transparency >= 250 || !ub_world_is_descendant(w, i, ws)) continue;
        g_statics[g_static_count++] = play_part_draw(&w->parts[in->part]);
    }
    ub_render_set_static(g_statics, g_static_count);
    g_static_scene = E.scene_version;
    g_static_sel = E.sel_version;
}

static void draw_box(vec3 pos, const mat3 *rot, vec3 size, u32 color)
{
    ub_part_draw d;
    d.pos = pos;
    d.rot = *rot;
    d.size = size;
    d.color = color;
    d.shape = UB_SHAPE_BOX;
    d.material = UB_MAT_PLASTIC;
    d.flags = UB_DRAW_GLOW;
    ub_render_part(&d);
}

static f32 dist_to_cam(vec3 p) { return v3_length(v3_sub(p, g_camera.pos)); }

/* the 12 edges of a part's box */
static void outline(const ub_part *p, u32 color)
{
    f32 t = ub_clampf(dist_to_cam(p->pos) * 0.004f, 0.03f, 0.6f);
    vec3 h = v3_scale(p->size, 0.5f);
    const vec3 ax[3] = { p->rot.x, p->rot.y, p->rot.z };
    const f32 hs[3] = { h.x, h.y, h.z };
    for (int a = 0; a < 3; ++a) {
        int b = (a + 1) % 3, c = (a + 2) % 3;
        for (int k = 0; k < 4; ++k) {
            f32 sb = (k & 1) ? 1.0f : -1.0f, sc = (k & 2) ? 1.0f : -1.0f;
            vec3 pos = v3_add(p->pos, v3_add(v3_scale(ax[b], sb * hs[b]), v3_scale(ax[c], sc * hs[c])));
            f32 sz[3];
            sz[a] = hs[a] * 2 + t;
            sz[b] = t;
            sz[c] = t;
            draw_box(pos, &p->rot, v3(sz[0], sz[1], sz[2]), color);
        }
    }
}

/* Handles: six arrows (Move: along the world axes from the selection's
 * box; Scale: out of the faces of the first selected part). */
typedef struct {
    vec3 base, dir;
    f32 len;
    int face;     /* 0..5 = +x +y +z -x -y -z */
} handle;

static int handles(handle *out)
{
    if (g_tool != TOOL_MOVE && g_tool != TOOL_SCALE) return 0;
    if (g_tool == TOOL_SCALE) {
        i32 pi = ed_first_part(&E);
        if (pi < 0) return 0;
        const ub_part *p = &E.world.parts[E.world.inst[pi].part];
        f32 len = ub_clampf(dist_to_cam(p->pos) * 0.07f, 0.8f, 8.0f);
        const vec3 ax[3] = { p->rot.x, p->rot.y, p->rot.z };
        const f32 hs[3] = { p->size.x * 0.5f, p->size.y * 0.5f, p->size.z * 0.5f };
        for (int f = 0; f < 6; ++f) {
            vec3 d = f < 3 ? ax[f % 3] : v3_neg(ax[f % 3]);
            out[f].base = v3_madd(p->pos, d, hs[f % 3] + 0.2f);
            out[f].dir = d;
            out[f].len = len;
            out[f].face = f;
        }
        return 6;
    }
    vec3 lo, hi;
    if (!ed_selection_bounds(&E, &lo, &hi)) return 0;
    vec3 c = v3_scale(v3_add(lo, hi), 0.5f), h = v3_scale(v3_sub(hi, lo), 0.5f);
    f32 len = ub_clampf(dist_to_cam(c) * 0.07f, 0.8f, 8.0f);
    const f32 hs[3] = { h.x, h.y, h.z };
    for (int f = 0; f < 6; ++f) {
        vec3 d = v3(0, 0, 0);
        (&d.x)[f % 3] = f < 3 ? 1.0f : -1.0f;
        out[f].base = v3_madd(c, d, hs[f % 3] + 0.3f);
        out[f].dir = d;
        out[f].len = len;
        out[f].face = f;
    }
    return 6;
}

static f32 seg_dist2(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by)
{
    f32 dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    f32 t = l2 > 0 ? ub_clampf(((px - ax) * dx + (py - ay) * dy) / l2, 0, 1) : 0;
    f32 x = ax + dx * t - px, y = ay + dy * t - py;
    return x * x + y * y;
}

static int handle_under(const handle *hs, int n, int mx, int my)
{
    int best = -1;
    f32 bestd = 9 * 9;
    for (int i = 0; i < n; ++i) {
        f32 ax, ay, bx, by;
        vec3 tip = v3_madd(hs[i].base, hs[i].dir, hs[i].len);
        if (!ub_render_project(&g_camera, g_w, g_h, hs[i].base, &ax, &ay)) continue;
        if (!ub_render_project(&g_camera, g_w, g_h, tip, &bx, &by)) continue;
        f32 d = seg_dist2((f32)mx, (f32)my, ax, ay, bx, by);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

static void draw_handles(const handle *hs, int n)
{
    static const u32 colors[3] = { 0xF04040, 0x40D040, 0x4080FF };
    for (int i = 0; i < n; ++i) {
        f32 t = hs[i].len * 0.12f;
        vec3 mid = v3_madd(hs[i].base, hs[i].dir, hs[i].len * 0.5f);
        /* a box whose long side is the handle's direction */
        mat3 r = mat3_identity();
        vec3 d = hs[i].dir, up = ub_absf(d.y) < 0.9f ? v3(0, 1, 0) : v3(1, 0, 0);
        r.z = d;
        r.x = v3_normalize(v3_cross(up, d));
        r.y = v3_cross(d, r.x);
        u32 col = i == g_hover_handle || (D.mode && D.face == hs[i].face) ? 0xFFE040 : colors[hs[i].face % 3];
        draw_box(mid, &r, v3(t, t, hs[i].len), col);
        draw_box(v3_madd(hs[i].base, d, hs[i].len), &r, v3(t * 2.5f, t * 2.5f, t * 2.5f), col);
    }
}

/* closest point on the line (p, a) to the ray (o, d): the line's parameter */
static f32 line_param(vec3 p, vec3 a, vec3 o, vec3 d)
{
    vec3 w0 = v3_sub(p, o);
    f32 b = v3_dot(a, d), dd = v3_dot(a, w0), e = v3_dot(d, w0);
    f32 denom = 1.0f - b * b;
    if (denom < 1e-5f) return 0;
    return (b * e - dd) / denom;
}

static void mouse_ray(const ub_input *in, vec3 *o, vec3 *d)
{
    ub_render_screen_ray(&g_camera, g_w, g_h, (f32)in->mouse_x + 0.5f, (f32)in->mouse_y + 0.5f, o, d);
}

/* where to put a new part: what is in the middle of the view, else the ground */
static vec3 insert_point(void)
{
    vec3 o, d, hit;
    ub_render_screen_ray(&g_camera, g_w, g_h, (f32)(g_view_rect.x + g_view_rect.w / 2),
                         (f32)(g_view_rect.y + g_view_rect.h / 2), &o, &d);
    if (ed_pick(&E, o, d, false, &hit) >= 0 && v3_length(v3_sub(hit, o)) < 300) return hit;
    if (d.y < -0.01f) {
        f32 t = -o.y / d.y;
        if (t < 300) return v3_madd(o, d, t);
    }
    return v3_madd(o, d, 30);
}

static void do_insert(ed_insert_kind k)
{
    i32 i = ed_insert(&E, k, insert_point());
    char msg[80];
    ub_fmt(msg, sizeof msg, i >= 0 ? "Inserted %s" : "Could not insert %s", ed_insert_name(k));
    status(msg);
}

static void do_behaviour(ed_behaviour b)
{
    int n = ed_add_behaviour(&E, b);
    char msg[120];
    if (n) ub_fmt(msg, sizeof msg, "%s added to %d part(s)", ed_behaviour_name(b), n);
    else ub_fmt(msg, sizeof msg, "%s: select a part first", ed_behaviour_name(b));
    status(msg);
}

static void open_script(i32 inst)
{
    if (!g_script_buf) g_script_buf = (char *)plat_alloc(SCRIPT_MAX);
    if (!g_script_buf || !ub_world_alive(&E.world, inst)) return;
    ub_fmt(g_script_buf, (int)SCRIPT_MAX, "%s", ub_world_source(&E.world, inst));
    memset(&g_script_st, 0, sizeof g_script_st);
    g_script_st.focused = true;
    g_script = inst;
    g_dlg = DLG_SCRIPT;
}

/* --------------------------------------------------------- play test */

static void play_start(void)
{
    ub_world w;
    if (!ed_play_world(&E, &w)) {
        status("Could not start the play test (out of memory)");
        return;
    }
    if (!ub_game_start(&g_game, &w, true)) {
        ub_world_free(&w);
        return;
    }
    g_playing = true;
    play_view_init(&g_view);
    g_view.yaw = g_cam_yaw;
    play_view_reset(&g_view, &g_game);
    ui_close_menus();
    status("Play test: F5 or Esc stops");
}

static void play_stop(void)
{
    ub_game_stop(&g_game);
    play_view_free(&g_view);
    g_playing = false;
    plat_set_mouse_capture(false);
    g_static_scene = 0xFFFFFFFFu; /* redraw the editor's own place */
    status("Play test stopped");
}

/* ------------------------------------------------------------- input */

/* A modifier counts if it is down or was let go this frame: on a slow
 * machine Ctrl+S can be pressed and released within one frame. */
static bool ctrl(const ub_input *in) { return in->key_down[UB_KEY_CONTROL] || in->key_released[UB_KEY_CONTROL]; }
static bool shift(const ub_input *in) { return in->key_down[UB_KEY_SHIFT] || in->key_released[UB_KEY_SHIFT]; }

static void shortcuts(const ub_input *in)
{
    if (ui_wants_keyboard() || g_dlg) return;
    bool c = ctrl(in), sh = shift(in);
    if (c && in->key_pressed['Z']) { if (sh) ed_redo(&E); else ed_undo(&E); }
    if (c && in->key_pressed['Y']) ed_redo(&E);
    if (c && in->key_pressed['C']) ed_copy(&E);
    if (c && in->key_pressed['X']) ed_cut(&E);
    if (c && in->key_pressed['V']) ed_paste(&E);
    if (c && in->key_pressed['D']) ed_duplicate(&E);
    if (c && in->key_pressed['G']) ed_group(&E);
    if (c && in->key_pressed['U']) ed_ungroup(&E);
    if (c && in->key_pressed['A']) ed_select_all(&E);
    if (c && in->key_pressed['S']) save();
    if (c && in->key_pressed['O']) request(AFTER_OPEN);
    if (c && in->key_pressed['N']) request(AFTER_NEW);
    if (in->key_pressed[UB_KEY_DELETE]) ed_delete(&E);
    if (in->key_pressed[UB_KEY_F5]) play_start();
    if (in->key_pressed[UB_KEY_ESCAPE] && !ui_menu_open()) ed_select_none(&E);
    if (!c) {
        if (in->key_pressed['1']) g_tool = TOOL_SELECT;
        if (in->key_pressed['2']) g_tool = TOOL_MOVE;
        if (in->key_pressed['3']) g_tool = TOOL_SCALE;
        if (in->key_pressed['4']) g_tool = TOOL_ROTATE;
        if (in->key_pressed['R']) ed_rotate(&E, 1, E.rot_step);
        if (in->key_pressed['T']) ed_rotate(&E, 0, E.rot_step);
        if (in->key_pressed['F']) focus_selection();
    }
}

static void camera_input(const ub_input *in, f32 dt, bool in_view)
{
    bool look = in->mouse_down[UB_MOUSE_RIGHT] && in->has_focus && (in_view || in->mouse_captured) && !g_dlg;
    plat_set_mouse_capture(look);
    if (look) {
        g_cam_yaw = ub_wrap_angle(g_cam_yaw + (f32)in->mouse_dx * 0.006f);
        g_cam_pitch = ub_clampf(g_cam_pitch - (f32)in->mouse_dy * 0.006f, -1.5f, 1.5f);
    }
    if (ui_wants_keyboard() || g_dlg || ctrl(in)) return;
    vec3 f = cam_forward(), r = v3(ub_cosf(g_cam_yaw), 0, -ub_sinf(g_cam_yaw));
    f32 speed = (in->key_down[UB_KEY_SHIFT] ? 90.0f : 30.0f) * dt;
    vec3 m = v3(0, 0, 0);
    if (in->key_down['W']) m = v3_add(m, f);
    if (in->key_down['S']) m = v3_sub(m, f);
    if (in->key_down['D']) m = v3_add(m, r);
    if (in->key_down['A']) m = v3_sub(m, r);
    if (in->key_down['E']) m.y += 1;
    if (in->key_down['Q']) m.y -= 1;
    g_cam_pos = v3_madd(g_cam_pos, m, speed);
    if (in_view && in->wheel) g_cam_pos = v3_madd(g_cam_pos, f, (f32)in->wheel / UB_WHEEL_NOTCH * 4.0f);
}

static void view_input(const ub_input *in)
{
    bool in_view = ui_inside(g_view_rect, in->mouse_x, in->mouse_y) && !ui_wants_mouse() && !g_dlg;
    handle hs[6];
    int nh = handles(hs);
    g_hover_handle = in_view && !D.mode ? handle_under(hs, nh, in->mouse_x, in->mouse_y) : -1;
    vec3 o, d;
    mouse_ray(in, &o, &d);

    if (D.mode && !in->mouse_down[UB_MOUSE_LEFT]) {
        D.mode = DRAG_NONE;
        g_static_scene = 0xFFFFFFFFu;
        return;
    }
    if (D.mode == DRAG_AXIS || D.mode == DRAG_FACE) {
        f32 t = line_param(D.anchor, D.axis, o, d) - D.t0;
        f32 want = ed_snap(&E, t);
        if (want != D.applied) {
            if (D.mode == DRAG_AXIS) ed_translate(&E, v3_scale(D.axis, want - D.applied));
            else ed_resize(&E, D.part, D.face, want - D.applied);
            D.applied = want;
        }
        return;
    }
    if (D.mode == DRAG_SLIDE) {
        if (ub_absf(d.y) > 1e-4f) {
            f32 t = (D.plane_y - o.y) / d.y;
            if (t > 0 && t < 2000) {
                vec3 p = v3_madd(o, d, t);
                vec3 want = v3(ed_snap(&E, p.x - D.start.x), 0, ed_snap(&E, p.z - D.start.z));
                vec3 step = v3_sub(want, D.applied_v);
                if (step.x != 0 || step.z != 0) {
                    ed_translate(&E, step);
                    D.applied_v = want;
                }
            }
        }
        return;
    }
    if (!in_view || !in->mouse_pressed[UB_MOUSE_LEFT]) return;
    /* a handle? */
    if (g_hover_handle >= 0) {
        handle *h = &hs[g_hover_handle];
        D.mode = g_tool == TOOL_SCALE ? DRAG_FACE : DRAG_AXIS;
        D.axis = h->dir;
        D.anchor = h->base;
        D.t0 = line_param(D.anchor, D.axis, o, d);
        D.applied = 0;
        D.face = h->face;
        D.part = ed_first_part(&E);
        ed_checkpoint(&E, D.mode == DRAG_FACE ? "Resize" : "Move");
        return;
    }
    vec3 hit;
    bool alt = in->key_down[UB_KEY_ALT];
    i32 picked = ed_pick(&E, o, d, !alt, &hit);
    bool add = shift(in) || ctrl(in);
    if (picked < 0) {
        if (!add) ed_select_none(&E);
        return;
    }
    if (!ed_selected(&E, picked) || add) ed_select(&E, picked, add);
    /* grabbing a selected part slides the selection along the ground */
    if (ed_selected(&E, picked) && g_tool != TOOL_ROTATE && !add) {
        D.mode = DRAG_SLIDE;
        D.plane_y = hit.y;
        D.start = hit;
        D.applied_v = v3(0, 0, 0);
        D.face = -1;
        ed_checkpoint(&E, "Move");
    }
}

/* -------------------------------------------------------------- panels */

static const char *const k_materials[UB_MAT_COUNT] = { "Plastic", "Grid", "Brick", "Wood", "Grass", "Metal", "Stone" };
static const char *const k_shapes[] = { "Block", "Wedge", "Cylinder", "Ball", "Corner wedge" };
static const char *const k_grids[] = { "Off", "0.25 studs", "0.5 studs", "1 stud", "2 studs", "4 studs" };
static const f32 k_grid_vals[] = { 0, 0.25f, 0.5f, 1, 2, 4 };
static const u32 k_palette[] = {
    0xF2F3F3, 0xA3A2A5, 0x635F62, 0x1B2A35, 0x111111, 0xC4281C, 0xFF0000, 0xDA8541,
    0xE29B40, 0xF5CD30, 0xFFFF00, 0xA4BD47, 0x4B974B, 0x287F47, 0x00FF00, 0x9FF3E9,
    0x80BBDB, 0x0D69AC, 0x2154B9, 0x0000FF, 0x6B327C, 0xAA00AA, 0xFF66CC, 0xCC8E69,
    0x7C5C46, 0x694028, 0xD7C59A, 0x5A4C42, 0xFFC9C9, 0xB1A7FF, 0x8C5B9F, 0x3C8C3C,
};

static u32 icon_color(i32 i)
{
    const ub_instance *in = &E.world.inst[i];
    if (in->part >= 0) return E.world.parts[in->part].color;
    if (in->source != UB_NO_STRING) return 0x5EA1FF;
    if (in->cls == UB_CLASS_MODEL) return 0xF0A040;
    if (in->cls == UB_CLASS_FOLDER) return 0xE0C060;
    if (in->cls == UB_CLASS_WORKSPACE) return 0x60C0F0;
    return 0xC0C0C0;
}

/* explorer rows: the tree, only what is expanded, drawing only what shows */
typedef struct {
    i32 inst;
    int depth;
} row;

static row *g_rows;
static i32 g_rows_cap, g_nrows;

static void add_rows(i32 i, int depth)
{
    for (; i >= 0; i = E.world.inst[i].next_sibling) {
        if (!ub_world_alive(&E.world, i)) continue;
        if (g_nrows == g_rows_cap) {
            i32 nc = g_rows_cap ? g_rows_cap * 2 : 256;
            row *n = (row *)plat_alloc(sizeof(row) * (size_t)nc);
            if (!n) return;
            if (g_rows) {
                memcpy(n, g_rows, sizeof(row) * (size_t)g_nrows);
                plat_free(g_rows);
            }
            g_rows = n;
            g_rows_cap = nc;
        }
        g_rows[g_nrows].inst = i;
        g_rows[g_nrows++].depth = depth;
        if (i < g_expand_cap && g_expand[i] && E.world.inst[i].first_child >= 0) add_rows(E.world.inst[i].first_child, depth + 1);
    }
}

static void explorer(ui_rect r)
{
    ub_world *w = &E.world;
    if (g_expand_cap < w->count + 1) {
        i32 nc = w->count + 1024;
        u8 *n = (u8 *)plat_alloc((size_t)nc);
        if (n && g_expand) memcpy(n, g_expand, (size_t)g_expand_cap);
        plat_free(g_expand);
        g_expand = n;
        g_expand_cap = n ? nc : 0;
    }
    if (g_expand && g_expand_scene != E.scene_version && w->workspace >= 0) {
        g_expand[w->workspace] = 1; /* the Workspace starts open */
        g_expand_scene = E.scene_version;
    }
    /* selected things show: open their parents */
    for (i32 s = 0; s < E.sel_count && g_expand; ++s)
        for (i32 a = w->inst[E.sel[s]].parent; a >= 0; a = w->inst[a].parent) g_expand[a] = 1;
    g_nrows = 0;
    for (i32 i = 0; i < w->count; ++i)
        if (w->inst[i].parent < 0 && ub_world_alive(w, i) && !(w->inst[i].flags & UB_INST_DETACHED)) add_rows(i, 0);

    ui_panel_begin("explorer", r, true);
    ui_heading("Explorer");
    int rh = ui_tree_row_height();
    int top = ui_panel_scroll() - rh * 2, first = top > 0 ? top / rh : 0;
    int shown = r.h / rh + 2;
    ui_space(first * rh);
    for (int k = first; k < g_nrows && k < first + shown; ++k) {
        i32 i = g_rows[k].inst;
        char label[96];
        ub_fmt(label, sizeof label, "%s##%d", ub_world_str(w, w->inst[i].name), i);
        bool kids = w->inst[i].first_child >= 0;
        int f = ui_tree_row(label, g_rows[k].depth, kids, i < g_expand_cap && g_expand[i], ed_selected(&E, i), icon_color(i));
        if ((f & UI_ROW_TOGGLE) && i < g_expand_cap) g_expand[i] ^= 1;
        if (f & UI_ROW_CLICK) ed_select(&E, i, g_in && (ctrl(g_in) || shift(g_in)));
        if (f & UI_ROW_DOUBLE) {
            if (w->inst[i].source != UB_NO_STRING) open_script(i);
            else focus_selection();
        }
    }
    if (g_nrows > first + shown) ui_space((g_nrows - first - shown) * rh);
    ui_panel_end();
}

static void place_properties(void)
{
    ui_heading("Place");
    ub_world *w = &E.world;
    char name[64];
    ub_fmt(name, sizeof name, "%s", w->name);
    if (ui_text_field("Name", name, sizeof name) && name[0]) {
        ed_checkpoint(&E, "Rename place");
        ub_fmt(w->name, sizeof w->name, "%s", name);
        set_title();
    }
    f32 clock = w->lighting.clock_time;
    if (ui_slider("Time of day", &clock, 0, 24)) {
        ed_checkpoint(&E, "Time of day");
        w->lighting.clock_time = clock;
    }
    f32 grav = w->gravity;
    if (ui_number("Gravity", &grav, 5, 1)) {
        ed_checkpoint(&E, "Gravity");
        w->gravity = ub_clampf(grav, 0, 2000);
    }
    f32 fall = w->fallen_height;
    if (ui_number("Fall limit", &fall, 10, 0)) {
        ed_checkpoint(&E, "Fall limit");
        w->fallen_height = fall;
    }
    ui_space(6);
    ui_heading("Building");
    int gi = 3;
    for (int i = 0; i < 6; ++i)
        if (ub_absf(k_grid_vals[i] - E.grid) < 1e-3f) gi = i;
    if (ui_choice("Grid", &gi, k_grids, 6)) E.grid = k_grid_vals[gi];
    ui_space(6);
    ui_label_color("Click a part to select it.", UI_DIM);
    ui_label_color("Right mouse + WASD: fly. Wheel: zoom.", UI_DIM);
    ui_label_color("Insert parts from the toolbar, then add", UI_DIM);
    ui_label_color("behaviours to make them do things.", UI_DIM);
    ui_label_color("F5 plays the place.", UI_DIM);
}

static void properties(ui_rect r)
{
    ui_panel_begin("properties", r, true);
    ub_world *w = &E.world;
    i32 first = ed_first(&E);
    if (first < 0 || !ub_world_alive(w, first)) {
        place_properties();
        ui_panel_end();
        return;
    }
    char title[96];
    if (E.sel_count > 1) ub_fmt(title, sizeof title, "%d selected", E.sel_count);
    else ub_fmt(title, sizeof title, "%s", ub_world_str(w, w->inst[first].class_name));
    ui_heading(title);
    if (E.sel_count == 1) {
        char name[64];
        ub_fmt(name, sizeof name, "%s", ub_world_str(w, w->inst[first].name));
        if (ui_text_field("Name", name, sizeof name)) ed_set_name(&E, first, name);
    }
    i32 pi = ed_first_part(&E);
    if (pi >= 0) {
        const ub_part *p = &w->parts[w->inst[pi].part];
        if (E.sel_count == 1) {
            vec3 pos, ori, size;
            ed_get_rbx(&E, pi, &pos, &ori, &size);
            vec3 np = pos, no = ori, ns = size;
            ui_label_color("Position", UI_DIM);
            ui_columns(3);
            ui_number("##px", &np.x, E.grid > 0 ? E.grid : 0.1f, 2);
            ui_number("##py", &np.y, E.grid > 0 ? E.grid : 0.1f, 2);
            ui_number("##pz", &np.z, E.grid > 0 ? E.grid : 0.1f, 2);
            ui_label_color("Size", UI_DIM);
            ui_columns(3);
            ui_number("##sx", &ns.x, E.grid > 0 ? E.grid : 0.1f, 2);
            ui_number("##sy", &ns.y, E.grid > 0 ? E.grid : 0.1f, 2);
            ui_number("##sz", &ns.z, E.grid > 0 ? E.grid : 0.1f, 2);
            ui_label_color("Orientation", UI_DIM);
            ui_columns(3);
            ui_number("##ox", &no.x, 15, 1);
            ui_number("##oy", &no.y, 15, 1);
            ui_number("##oz", &no.z, 15, 1);
            if (np.x != pos.x || np.y != pos.y || np.z != pos.z || ns.x != size.x || ns.y != size.y ||
                ns.z != size.z || no.x != ori.x || no.y != ori.y || no.z != ori.z)
                ed_set_rbx(&E, pi, np, no, ns);
        }
        u32 color = p->color;
        if (ui_color("Color", &color, k_palette, (int)(sizeof k_palette / sizeof k_palette[0]))) ed_set_color(&E, color);
        int mat = p->material < UB_MAT_COUNT ? p->material : 0;
        if (ui_choice("Material", &mat, k_materials, UB_MAT_COUNT)) ed_set_material(&E, (u8)mat);
        int shape = p->shape < 5 ? p->shape : 0;
        if (ui_choice("Shape", &shape, k_shapes, 5)) ed_set_shape(&E, (u8)shape);
        f32 tr = p->transparency / 255.0f;
        if (ui_slider("Transparency", &tr, 0, 1)) ed_set_transparency(&E, tr);
        bool anchored = (p->flags & UB_PART_ANCHORED) != 0, coll = (p->flags & UB_PART_CAN_COLLIDE) != 0;
        bool glow = (p->flags & UB_PART_GLOW) != 0;
        if (ui_checkbox("Anchored", &anchored)) ed_set_anchored(&E, anchored);
        if (ui_checkbox("CanCollide", &coll)) ed_set_collide(&E, coll);
        if (ui_checkbox("Neon", &glow)) ed_set_glow(&E, glow);
        ui_columns(2);
        if (ui_button("Turn (R)")) ed_rotate(&E, 1, E.rot_step);
        if (ui_button("Tip over (T)")) ed_rotate(&E, 0, E.rot_step);
        ui_space(6);
        ui_heading("Behaviours");
        /* the ones it has */
        for (i32 c = w->inst[pi].first_child; c >= 0; c = w->inst[c].next_sibling) {
            if (w->inst[c].source == UB_NO_STRING || !ub_world_alive(w, c)) continue;
            char lab[96];
            ub_fmt(lab, sizeof lab, "Edit %s##%d", ub_world_str(w, w->inst[c].name), c);
            if (ui_button(lab)) open_script(c);
        }
        static const char *beh_names[ED_BEH_COUNT];
        for (int b = 0; b < ED_BEH_COUNT; ++b) beh_names[b] = ed_behaviour_name((ed_behaviour)b);
        int pick = -1;
        if (ui_choice("Add behaviour", &pick, beh_names, ED_BEH_COUNT) && pick >= 0) do_behaviour((ed_behaviour)pick);
    }
    if (E.sel_count == 1 && w->inst[first].source != UB_NO_STRING) {
        if (ui_button("Edit script")) open_script(first);
        bool off = (w->inst[first].flags & UB_INST_DISABLED) != 0;
        if (ui_checkbox("Disabled", &off)) {
            ed_checkpoint(&E, "Disabled");
            if (off) w->inst[first].flags |= UB_INST_DISABLED;
            else w->inst[first].flags &= (u16)~UB_INST_DISABLED;
        }
    }
    if (w->inst[first].cls == UB_CLASS_MODEL || w->inst[first].cls == UB_CLASS_FOLDER) {
        if (ui_button("Ungroup (Ctrl+U)")) ed_ungroup(&E);
    }
    ui_space(6);
    ui_columns(2);
    if (ui_button("Duplicate")) ed_duplicate(&E);
    if (ui_button("Delete")) ed_delete(&E);
    ui_panel_end();
}

static void menus(void)
{
    ui_menubar_begin(ui_r(0, 0, g_w, ui_line_height() + 2));
    if (ui_menu_begin("File")) {
        if (ui_menu_item("New place", "Ctrl+N", true)) request(AFTER_NEW);
        if (ui_menu_item("Open...", "Ctrl+O", true)) request(AFTER_OPEN);
        if (ui_menu_item("Save", "Ctrl+S", true)) save();
        if (ui_menu_item("Save as...", NULL, true)) {
            ub_fmt(g_save_name, sizeof g_save_name, "%s", E.world.name);
            g_dlg = DLG_SAVE_AS;
        }
        ui_menu_separator();
        if (ui_menu_item("Exit", NULL, true)) plat_request_quit();
        ui_menu_end();
    }
    if (ui_menu_begin("Edit")) {
        char u[64], r[64];
        ub_fmt(u, sizeof u, "Undo %s", ed_undo_name(&E) ? ed_undo_name(&E) : "");
        ub_fmt(r, sizeof r, "Redo %s", ed_redo_name(&E) ? ed_redo_name(&E) : "");
        if (ui_menu_item(u, "Ctrl+Z", ed_undo_name(&E) != NULL)) ed_undo(&E);
        if (ui_menu_item(r, "Ctrl+Y", ed_redo_name(&E) != NULL)) ed_redo(&E);
        ui_menu_separator();
        bool sel = E.sel_count > 0;
        if (ui_menu_item("Cut", "Ctrl+X", sel)) ed_cut(&E);
        if (ui_menu_item("Copy", "Ctrl+C", sel)) ed_copy(&E);
        if (ui_menu_item("Paste", "Ctrl+V", E.has_clip)) ed_paste(&E);
        if (ui_menu_item("Duplicate", "Ctrl+D", sel)) ed_duplicate(&E);
        if (ui_menu_item("Delete", "Del", sel)) ed_delete(&E);
        ui_menu_separator();
        if (ui_menu_item("Select all", "Ctrl+A", true)) ed_select_all(&E);
        if (ui_menu_item("Group", "Ctrl+G", sel)) ed_group(&E);
        if (ui_menu_item("Ungroup", "Ctrl+U", sel)) ed_ungroup(&E);
        ui_menu_end();
    }
    if (ui_menu_begin("Insert")) {
        for (int k = 0; k < ED_INS_COUNT; ++k) {
            if (k == ED_INS_MODEL) ui_menu_separator();
            if (ui_menu_item(ed_insert_name((ed_insert_kind)k), NULL, true)) do_insert((ed_insert_kind)k);
        }
        ui_menu_end();
    }
    if (ui_menu_begin("Behaviours")) {
        for (int b = 0; b < ED_BEH_COUNT; ++b)
            if (ui_menu_item(ed_behaviour_name((ed_behaviour)b), NULL, E.sel_count > 0)) do_behaviour((ed_behaviour)b);
        ui_menu_end();
    }
    if (ui_menu_begin("Test")) {
        if (ui_menu_item("Play", "F5", true)) play_start();
        ui_menu_end();
    }
    if (ui_menu_begin("Help")) {
        if (ui_menu_item("Controls...", "F1", true)) g_dlg = DLG_HELP;
        ui_menu_end();
    }
    ui_menubar_end();
}

static void toolbar(ui_rect r)
{
    ui_panel_begin("toolbar", r, false);
    ui_columns(13);
    if (ui_toggle("Select", g_tool == TOOL_SELECT)) g_tool = TOOL_SELECT;
    if (ui_toggle("Move", g_tool == TOOL_MOVE)) g_tool = TOOL_MOVE;
    if (ui_toggle("Scale", g_tool == TOOL_SCALE)) g_tool = TOOL_SCALE;
    if (ui_toggle("Rotate", g_tool == TOOL_ROTATE)) g_tool = TOOL_ROTATE;
    if (ui_button("+ Block")) do_insert(ED_INS_BLOCK);
    if (ui_button("+ Wedge")) do_insert(ED_INS_WEDGE);
    if (ui_button("+ Cylinder")) do_insert(ED_INS_CYLINDER);
    if (ui_button("+ Ball")) do_insert(ED_INS_BALL);
    if (ui_button("+ Spawn")) do_insert(ED_INS_SPAWN);
    if (ui_button_enabled("Undo", ed_undo_name(&E) != NULL)) ed_undo(&E);
    if (ui_button_enabled("Redo", ed_redo_name(&E) != NULL)) ed_redo(&E);
    if (ui_button("Save")) save();
    if (ui_toggle("Play (F5)", false)) play_start();
    ui_panel_end();
}

static void status_bar(ui_rect r)
{
    ui_claim(r);
    ub_render_rect(r.x, r.y, r.w, r.h, UI_BG);
    char line[256];
    if (g_status[0] && ui_time() < g_status_until) ub_fmt(line, sizeof line, "%s", g_status);
    else {
        static const char *const tools[] = { "Select: click parts, drag to slide them",
                                             "Move: drag the arrows", "Scale: drag the arrows to pull faces",
                                             "Rotate: R turns, T tips over" };
        char grid[16];
        ub_fmt_num(grid, sizeof grid, E.grid, 2);
        ub_fmt(line, sizeof line, "%s  |  %d selected  |  grid %s  |  %d parts", tools[g_tool], E.sel_count,
               E.grid > 0 ? grid : "off", E.world.part_count);
    }
    ub_render_text(r.x + 6, r.y + (r.h - ub_render_line_height()) / 2 + 1, line, UI_TEXT | UB_TEXT_FLAT);
}

static void dialogs(void)
{
    if (g_dlg == DLG_OPEN) {
        ui_dialog_begin("Open a place", 460, 420);
        if (!g_nfiles) ui_label_color("No places yet: save one, or put .rbxl files in places/", UI_DIM);
        for (int i = 0; i < g_nfiles; ++i) {
            char lab[120];
            ub_fmt(lab, sizeof lab, "%s##f%d", g_files[i], i);
            if (ui_button(lab)) {
                g_dlg = DLG_NONE;
                open_place(g_files[i], false);
            }
        }
        ui_space(8);
        if (ui_button("Cancel")) g_dlg = DLG_NONE;
        ui_dialog_end();
    } else if (g_dlg == DLG_SAVE_AS) {
        ui_dialog_begin("Save as", 420, 170);
        ui_text_field("File name", g_save_name, sizeof g_save_name);
        char clean[64], path[96];
        sanitize(g_save_name, clean, sizeof clean);
        ub_fmt(path, sizeof path, "places/%s.ubx", clean);
        ui_label_color(path, UI_DIM);
        ui_columns(2);
        if (ui_button("Save##as")) {
            ub_fmt(E.world.name, sizeof E.world.name, "%s", g_save_name[0] ? g_save_name : clean);
            g_dlg = DLG_NONE;
            save_to(path);
        }
        if (ui_button("Cancel")) g_dlg = DLG_NONE;
        ui_dialog_end();
    } else if (g_dlg == DLG_CONFIRM) {
        ui_dialog_begin("Unsaved changes", 380, 140);
        ui_label("Save the changes to this place first?");
        ui_columns(3);
        int after = g_after;
        if (ui_button("Save##c")) {
            g_dlg = DLG_NONE;
            save();
            if (!E.dirty) { request(after); }
        }
        if (ui_button("Don't save")) {
            g_dlg = DLG_NONE;
            E.dirty = false;
            request(after);
        }
        if (ui_button("Cancel")) g_dlg = DLG_NONE;
        ui_dialog_end();
    } else if (g_dlg == DLG_SCRIPT) {
        if (!ub_world_alive(&E.world, g_script)) {
            g_dlg = DLG_NONE;
            return;
        }
        char title[100];
        ub_fmt(title, sizeof title, "Script: %s", ub_world_str(&E.world, E.world.inst[g_script].name));
        ui_dialog_begin(title, g_w - 60, g_h - 60);
        ui_rect ta = ui_next_rect((g_h - 60) - ui_line_height() * 3 - 30);
        ui_text_area("script text", g_script_buf, (int)SCRIPT_MAX, ta, &g_script_st);
        ui_columns(3);
        if (ui_button("Save and close")) {
            if (!str_is(g_script_buf, ub_world_source(&E.world, g_script))) ed_set_source(&E, g_script, g_script_buf);
            g_dlg = DLG_NONE;
        }
        if (ui_button("Revert")) {
            ub_fmt(g_script_buf, (int)SCRIPT_MAX, "%s", ub_world_source(&E.world, g_script));
            g_script_st.cursor = 0;
        }
        if (ui_button("Cancel")) g_dlg = DLG_NONE;
        ui_dialog_end();
    } else if (g_dlg == DLG_HELP) {
        ui_dialog_begin("Controls", 520, 430);
        static const char *const lines[] = {
            "Right mouse button + move: look around",
            "W A S D: fly, Q / E: down / up, Shift: faster, wheel: zoom",
            "Click: select (Shift or Ctrl adds, Alt picks inside models)",
            "Drag a selected part: slide it along the ground",
            "1 Select, 2 Move, 3 Scale, 4 Rotate; R turn, T tip over",
            "F: look at the selection",
            "Ctrl+Z / Ctrl+Y: undo / redo",
            "Ctrl+C / X / V: copy / cut / paste, Ctrl+D: duplicate",
            "Delete: delete, Ctrl+G / Ctrl+U: group / ungroup",
            "Ctrl+N / O / S: new / open / save",
            "F5: play the place (F5 or Esc stops)",
            "Behaviours make parts do things without code:",
            "select parts, then pick one in the Behaviours menu.",
        };
        for (int i = 0; i < (int)(sizeof lines / sizeof lines[0]); ++i) ui_label(lines[i]);
        ui_space(6);
        if (ui_button("Close")) g_dlg = DLG_NONE;
        ui_dialog_end();
    }
}

/* ------------------------------------------------------------ app API */

const ub_app_info *app_info(void)
{
    static const ub_app_info info = { "Uniblox Editor", "ubedit.log", "ubedit.old.log" };
    return &info;
}

void app_preinit(ub_display_prefs *prefs)
{
    g_prefs = *prefs;
    g_prefs.sw_height = 0; /* the window's own size: sharp panels */
    ed_init(&E);
    load_config();
    *prefs = g_prefs;
    g_win_w = prefs->window_w;
    g_win_h = prefs->window_h;
}

bool app_init(const ub_system_info *sys, const ub_frame *frame)
{
    g_sys = *sys;
    ub_render_force_fixed_function(!g_gl_shaders);
    if (!ub_render_init(frame, g_renderer_desc, sizeof g_renderer_desc)) {
        ub_error("renderer failed to start: %s", g_renderer_desc);
        return false;
    }
    ub_info("renderer: %s", g_renderer_desc);
    ub_render_set_quality((ub_quality)g_quality);
    ub_render_set_text_px(13);
    ub_render_set_text_in_frame(true);
    if (!frame->fullscreen) {
        g_win_w = frame->width;
        g_win_h = frame->height;
    }
    char arg[260];
    if (!plat_command_line_arg(arg, sizeof arg) || !open_place(arg, true)) look_at_place();
    set_title();
    ub_info("editor ready");
    return true;
}

void app_shutdown(void)
{
    if (g_playing) play_stop();
    if (E.dirty) { /* nothing is lost: keep a copy */
        u8 *data;
        u32 size;
        if (plat_make_dir("places") && ub_ubx_save(&E.world, &data, &size)) {
            if (plat_file_write_all("places/autosave.ubx", data, size))
                ub_info("editor: unsaved changes kept in places/autosave.ubx");
            plat_free(data);
        }
    }
    save_config();
    ub_render_set_static(NULL, 0);
    plat_free(g_statics);
    plat_free(g_expand);
    plat_free(g_rows);
    plat_free(g_script_buf);
    plat_free(g_insel);
    plat_free(g_selparts);
    g_insel = NULL;
    g_selparts = NULL;
    g_insel_cap = g_selparts_cap = 0;
    g_statics = NULL;
    g_expand = NULL;
    g_rows = NULL;
    g_script_buf = NULL;
    ed_free(&E);
    ub_render_shutdown();
}

static void play_frame(ub_frame *frame, const ub_input *in, f32 dt)
{
    if (in->key_pressed[UB_KEY_F5] || in->key_pressed[UB_KEY_ESCAPE]) {
        play_stop();
        return;
    }
    vec3 move;
    bool jump;
    play_view_input(&g_view, in, dt, true, true, &move, &jump);
    ub_game_update(&g_game, dt, move, jump);
    g_game.jumped = false;
    g_game.landed_speed = 0;
    play_view_update(&g_view, &g_game, dt);
    ub_camera cam = play_view_camera(&g_view, &g_game);
    ub_render_env env = play_env(&g_game.world);
    ub_render_begin(frame, &cam, &env);
    play_view_draw(&g_view, &g_game, true);
    ui_begin(in, g_w, g_h, dt);
    int lh = ui_line_height();
    ui_panel_begin("playbar", ui_r(0, 0, g_w, lh + 12), false);
    ui_columns(4);
    if (ui_toggle("Stop (F5)", true)) {
        ui_end();
        ub_render_end(NULL);
        play_stop();
        return;
    }
    char hp[48];
    ub_fmt(hp, sizeof hp, g_game.dead ? "Knocked out - back soon" : "Health %d", ub_roundf(g_game.health));
    ui_label_color(hp, g_game.dead ? UI_ERROR : UI_TEXT);
    ui_label_color("Playing - WASD move, Space jump", UI_DIM);
    ui_label_color(" ", UI_DIM);
    ui_panel_end();
    /* what scripts printed */
    const char *out[UB_SCRIPT_OUTPUT_LINES];
    bool err[UB_SCRIPT_OUTPUT_LINES];
    int n = ub_script_output(out, err, 6);
    int y = g_h - (n + 1) * (lh - 3);
    for (int i = 0; i < n; ++i) ub_render_text(10, y + i * (lh - 3), out[i], err[i] ? UI_ERROR : 0xC8F0C8);
    ui_end();
    ub_render_end(NULL);
}

void app_frame(ub_frame *frame, const ub_input *in, u32 dt_us)
{
    if (frame->fb && !frame->fb->palette) {
        frame->fb->palette = ub_render_palette();
        frame->fb->palette_serial++;
    }
    if (dt_us > 100000) dt_us = 100000;
    const f32 dt = (f32)dt_us * 1e-6f;
    g_w = frame->width;
    g_h = frame->height;
    if (!frame->fullscreen) {
        g_win_w = frame->width;
        g_win_h = frame->height;
    }
    E.now = ui_time();
    g_in = in;
    if ((in->key_pressed[UB_KEY_F11]) || (in->key_down[UB_KEY_ALT] && in->key_pressed[UB_KEY_ENTER])) {
        g_prefs.fullscreen = !frame->fullscreen;
        plat_set_fullscreen(g_prefs.fullscreen);
    }
    if (g_playing) {
        play_frame(frame, in, dt);
        return;
    }

    /* layout */
    int lh = ui_line_height();
    int mb = lh + 2, tb = lh + 12, sb = lh + 2;
    int exw = UB_CLAMP(g_w / 5, 150, 260), pw = UB_CLAMP(g_w / 4, 200, 300);
    if (exw + pw > g_w - 160) {
        exw = (g_w - 160) * 2 / 5;
        pw = (g_w - 160) * 3 / 5;
    }
    ui_rect r_tool = ui_r(0, mb, g_w, tb);
    ui_rect r_ex = ui_r(0, mb + tb, exw, g_h - mb - tb - sb);
    ui_rect r_props = ui_r(g_w - pw, mb + tb, pw, g_h - mb - tb - sb);
    ui_rect r_status = ui_r(0, g_h - sb, g_w, sb);
    g_view_rect = ui_r(exw, mb + tb, g_w - exw - pw, g_h - mb - tb - sb);

    /* camera (projection over the whole window, as the renderer draws) */
    bool in_view = ui_inside(g_view_rect, in->mouse_x, in->mouse_y) && !ui_wants_mouse() && !g_dlg;
    camera_input(in, dt, in_view);
    g_camera.pos = g_cam_pos;
    g_camera.yaw = g_cam_yaw;
    g_camera.pitch = g_cam_pitch;
    g_camera.focus = v3_madd(g_cam_pos, cam_forward(), 10);
    g_camera.fov_y = 70.0f * UB_PI / 180.0f;
    g_camera.far_dist = 1000.0f;
    shortcuts(in);
    if (in->key_pressed[UB_KEY_F1] && !ui_wants_keyboard()) g_dlg = DLG_HELP;
    view_input(in);
    if (!g_playing && (g_static_scene != E.scene_version || g_static_sel != E.sel_version)) rebuild_statics();
    static bool was_dirty;
    if (E.dirty != was_dirty) {
        was_dirty = E.dirty;
        set_title();
    }

    /* 3D: the place, the selection drawn live with outlines, handles */
    ub_render_env env = play_env(&E.world);
    ub_render_begin(frame, &g_camera, &env);
    update_insel();
    for (i32 k = 0; k < g_nselparts; ++k) {
        i32 i = g_selparts[k];
        if (i >= E.world.count || !ub_world_alive(&E.world, i) || E.world.inst[i].part < 0) continue;
        const ub_part *p = &E.world.parts[E.world.inst[i].part];
        if (p->transparency < 250) {
            ub_part_draw d = play_part_draw(p);
            ub_render_part(&d);
        }
        if (k < MAX_OUTLINED) outline(p, 0x3DA5FF);
    }
    handle hs[6];
    int nh = handles(hs);
    draw_handles(hs, nh);

    /* 2D */
    ui_begin(in, g_w, g_h, dt);
    explorer(r_ex);
    properties(r_props);
    toolbar(r_tool);
    status_bar(r_status);
    menus();
    dialogs();
    ui_end();
    ub_render_end(NULL);
}

/* ------------------------------------------------------------- tests */

ub_editor *app_editor_model(void) { return &E; }
const ub_camera *app_editor_camera(void) { return &g_camera; }
bool app_editor_playing(void) { return g_playing; }
ub_game *app_editor_game(void) { return &g_game; }
const char *app_editor_status(void) { return g_status; }
