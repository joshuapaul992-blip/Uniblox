/* Uniblox - immediate-mode UI (see ub_ui.h). */
#include "ui/ub_ui.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "render/ub_render.h"
#include "crt/ub_crt.h"

#define MAX_REC 1024
#define MAX_SCROLL 32
#define EDIT_MAX 512
#define PAD 6
#define TAB_SPACES 4

enum { F_HOVER = 1, F_PRESS = 2, F_CLICK = 4, F_HELD = 8 };
enum { LAYER_BASE, LAYER_DIALOG, LAYER_POPUP };
enum { POP_NONE, POP_CHOICE, POP_COLOR };

typedef struct {
    u32 id;
    char label[64];   /* shown text */
    char full[64];    /* label as given */
    ui_rect r;
} rec;

static struct {
    const ub_input *in;
    int w, h;
    f32 dt, time;
    int lh, wh;               /* text line height, widget height */

    u32 active, active_seen;  /* the widget being pressed */
    bool active_was_seen;
    u32 focus;                /* the text box with the keyboard */
    bool focus_seen;
    char edit[EDIT_MAX];
    int edit_cur;
    bool edit_start_select;   /* first frame of an edit */
    int press_x, press_y;
    f32 press_value;
    bool dragging;
    u32 last_click_id;
    f32 last_click_time;

    /* layout */
    bool in_panel;
    ui_rect panel, clip;
    u32 panel_id;
    int x, y, inner_w, indent;
    int cols, col_i, col_w, row_y, row_h;
    int *scroll, content_top;

    /* layers */
    int layer, top_prev, top_cur;
    bool over_cur, over_prev;
    ui_rect pop_rects[8];     /* this frame's top-layer rectangles */
    int pop_n;

    /* records */
    rec recs[2][MAX_REC];
    int nrec[2], cur;

    struct {
        u32 id;
        int y;
    } scrolls[MAX_SCROLL];

    /* menus */
    ui_rect bar;
    int bar_x;
    u32 open_menu;
    bool menu_open_now;
    int menu_x, menu_y, menu_w, menu_h;
    int menubar_layer_saved;

    /* pop-up lists */
    int pop_kind;
    u32 pop_id;
    ui_rect pop_anchor;
    const char *const *pop_items;
    const u32 *pop_palette;
    int pop_count, pop_sel;
    u32 result_id;
    int result_val;

    /* dialog */
    bool in_dialog;
    ui_rect dlg;
} U;

/* ---------------------------------------------------------- helpers */

static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (u8)*a - (u8)*b;
}

static u32 hash_str(const char *s, u32 h)
{
    for (; *s; ++s) h = (h ^ (u8)*s) * 16777619u;
    return h;
}

static u32 make_id(const char *label) { return hash_str(label, 2166136261u ^ (U.panel_id * 31u)) | 1u; }

/* the shown part of a label (before "##") */
static void shown(const char *label, char *out, int cap)
{
    int n = 0;
    while (label[n] && !(label[n] == '#' && label[n + 1] == '#') && n < cap - 1) {
        out[n] = label[n];
        ++n;
    }
    out[n] = 0;
}

static void record(u32 id, const char *label, ui_rect r)
{
    int c = U.cur;
    if (U.nrec[c] >= MAX_REC) return;
    rec *e = &U.recs[c][U.nrec[c]++];
    e->id = id;
    shown(label, e->label, sizeof e->label);
    ub_fmt(e->full, sizeof e->full, "%s", label);
    e->r = r;
}

bool ui_find(const char *label, ui_rect *out)
{
    int p = U.cur; /* the last finished frame (called between frames) */
    for (int i = 0; i < U.nrec[p]; ++i) {
        const rec *e = &U.recs[p][i];
        if (!str_cmp(e->label, label) || !str_cmp(e->full, label)) {
            *out = e->r;
            return true;
        }
    }
    return false;
}

static ui_rect intersect(ui_rect a, ui_rect b)
{
    int x0 = UB_MAX(a.x, b.x), y0 = UB_MAX(a.y, b.y);
    int x1 = UB_MIN(a.x + a.w, b.x + b.w), y1 = UB_MIN(a.y + a.h, b.y + b.h);
    return ui_r(x0, y0, UB_MAX(0, x1 - x0), UB_MAX(0, y1 - y0));
}

static void set_clip(ui_rect r)
{
    U.clip = r;
    ub_render_clip(r.x, r.y, r.w > 0 ? r.w : 1, r.h > 0 ? r.h : 1);
}

static void no_clip(void)
{
    U.clip = ui_r(0, 0, U.w, U.h);
    ub_render_clip(0, 0, 0, 0);
}

static void fill(ui_rect r, u32 c) { ub_render_rect(r.x, r.y, r.w, r.h, c); }

static void frame_rect(ui_rect r, u32 fillc, u32 border)
{
    fill(r, border);
    fill(ui_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2), fillc);
}

/* bytes of `s` that fit in `w` pixels (whole characters) */
static int fit(const char *s, int w)
{
    int n = 0;
    const char *p = s;
    while (*p) {
        const char *q = p;
        ub_utf8_decode(&q);
        if (ub_render_text_width_n(s, (int)(q - s)) > w) break;
        p = q;
        n = (int)(p - s);
    }
    return n;
}

/* text, cut with "..." to fit; vertically centred in r */
static void text_in(ui_rect r, int xoff, const char *s, u32 color)
{
    int w = r.w - xoff - 2;
    int ty = r.y + (r.h - U.lh) / 2 + 1;
    if (ub_render_text_width(s) <= w) {
        ub_render_text(r.x + xoff, ty, s, color | UB_TEXT_FLAT);
        return;
    }
    char buf[256];
    int n = fit(s, w - ub_render_text_width("..."));
    if (n > 250) n = 250;
    memcpy(buf, s, (size_t)n);
    memcpy(buf + n, "...", 4);
    ub_render_text(r.x + xoff, ty, buf, color | UB_TEXT_FLAT);
}

static bool mouse_in(ui_rect r)
{
    int mx = U.in->mouse_x, my = U.in->mouse_y;
    if (!ui_inside(r, mx, my)) return false;
    if (!ui_inside(U.clip, mx, my)) return false;
    return U.layer >= U.top_prev;
}

static int interact(u32 id, ui_rect r)
{
    int f = 0;
    bool over = mouse_in(r);
    if (over) f |= F_HOVER;
    if (over && U.in->mouse_pressed[UB_MOUSE_LEFT] && !U.active) {
        U.active = id;
        U.press_x = U.in->mouse_x;
        U.press_y = U.in->mouse_y;
        U.dragging = false;
        f |= F_PRESS;
    }
    if (U.active == id) {
        U.active_was_seen = true;
        f |= F_HELD;
        if (U.in->mouse_released[UB_MOUSE_LEFT] || !U.in->mouse_down[UB_MOUSE_LEFT]) {
            if (over) f |= F_CLICK;
            U.active = 0;
        }
    }
    return f;
}

static bool double_click(u32 id)
{
    bool dbl = U.last_click_id == id && U.time - U.last_click_time < 0.45f;
    U.last_click_id = dbl ? 0 : id;
    U.last_click_time = U.time;
    return dbl;
}

/* ------------------------------------------------------------- frame */

void ui_begin(const ub_input *in, int width, int height, f32 dt)
{
    U.in = in;
    U.w = width;
    U.h = height;
    U.dt = dt;
    U.time += dt;
    U.lh = ub_render_line_height();
    U.wh = U.lh + 5;
    U.cur ^= 1;
    U.nrec[U.cur] = 0;
    U.top_prev = U.top_cur;
    U.top_cur = (U.open_menu || U.pop_kind) ? LAYER_POPUP : LAYER_BASE;
    U.over_prev = U.over_cur;
    U.over_cur = false;
    U.pop_n = 0;
    U.layer = LAYER_BASE;
    U.active_was_seen = false;
    U.focus_seen = false;
    U.in_panel = false;
    U.menu_open_now = false;
    U.clip = ui_r(0, 0, width, height);
    if (U.active && !in->mouse_down[UB_MOUSE_LEFT] && !in->mouse_released[UB_MOUSE_LEFT]) U.active = 0;
}

static void draw_popup(void);

void ui_end(void)
{
    no_clip();
    U.layer = LAYER_POPUP;
    if (U.pop_kind) draw_popup();
    /* a click outside the open menus and pop-ups closes them */
    if (U.top_prev == LAYER_POPUP && U.in->mouse_pressed[UB_MOUSE_LEFT]) {
        bool inside = false;
        for (int i = 0; i < U.pop_n; ++i) inside |= ui_inside(U.pop_rects[i], U.in->mouse_x, U.in->mouse_y);
        if (!inside) {
            U.open_menu = 0;
            U.pop_kind = POP_NONE;
        }
    }
    if ((U.open_menu || U.pop_kind) && U.in->key_pressed[UB_KEY_ESCAPE]) {
        U.open_menu = 0;
        U.pop_kind = POP_NONE;
    }
    if (!U.active_was_seen && U.active && !U.in->mouse_down[UB_MOUSE_LEFT]) U.active = 0;
    if (!U.focus_seen) U.focus = 0;
    U.layer = LAYER_BASE;
}

bool ui_wants_mouse(void)
{
    /* (called before the frame's widgets: over_cur is still last frame's) */
    return U.over_cur || U.top_prev > LAYER_BASE || U.top_cur > LAYER_BASE || U.active != 0;
}

bool ui_wants_keyboard(void) { return U.focus != 0; }
int ui_line_height(void) { return U.wh; }
f32 ui_time(void) { return U.time; }

void ui_claim(ui_rect r)
{
    if (ui_inside(r, U.in->mouse_x, U.in->mouse_y)) U.over_cur = true;
}

static void add_pop_rect(ui_rect r)
{
    if (U.pop_n < 8) U.pop_rects[U.pop_n++] = r;
    ui_claim(r);
}

/* ------------------------------------------------------------ layout */

static int *scroll_slot(u32 id)
{
    for (int i = 0; i < MAX_SCROLL; ++i)
        if (U.scrolls[i].id == id) return &U.scrolls[i].y;
    for (int i = 0; i < MAX_SCROLL; ++i)
        if (!U.scrolls[i].id) {
            U.scrolls[i].id = id;
            U.scrolls[i].y = 0;
            return &U.scrolls[i].y;
        }
    return NULL;
}

void ui_panel_begin(const char *id, ui_rect r, bool scroll)
{
    U.panel_id = hash_str(id, 2166136261u);
    U.panel = r;
    U.in_panel = true;
    fill(r, U.layer == LAYER_DIALOG ? UI_PANEL : UI_PANEL);
    ui_claim(r);
    U.scroll = scroll ? scroll_slot(U.panel_id) : NULL;
    int sy = U.scroll ? *U.scroll : 0;
    if (U.scroll && mouse_in(r) && U.in->wheel)
        sy -= U.in->wheel / UB_WHEEL_NOTCH * U.wh * 3;
    if (sy < 0) sy = 0;
    if (U.scroll) *U.scroll = sy;
    U.x = r.x + PAD;
    U.content_top = r.y + PAD;
    U.y = U.content_top - sy;
    U.inner_w = r.w - 2 * PAD;
    U.indent = 0;
    U.cols = 0;
    set_clip(intersect(r, U.in_dialog ? U.dlg : ui_r(0, 0, U.w, U.h)));
}

void ui_panel_end(void)
{
    if (U.cols) { /* an unfinished row */
        U.y = U.row_y + U.row_h + 3;
        U.cols = 0;
    }
    if (U.scroll) { /* keep the scroll inside the contents */
        int content = U.y + *U.scroll - U.content_top + PAD;
        int max = content - U.panel.h;
        if (*U.scroll > max) *U.scroll = max > 0 ? max : 0;
        if (max > 0) { /* a scroll bar */
            int bh = U.panel.h * U.panel.h / (content > 0 ? content : 1);
            int by = U.panel.y + (U.panel.h - bh) * *U.scroll / max;
            fill(ui_r(U.panel.x + U.panel.w - 4, by, 3, bh), UI_HOVER);
        }
    }
    U.in_panel = false;
    U.panel_id = 0;
    if (U.in_dialog) set_clip(U.dlg);
    else no_clip();
}

int ui_panel_scroll(void) { return U.scroll ? *U.scroll : 0; }
int ui_tree_row_height(void) { return U.lh + 2 + 3; }

/* a row of columns left unfinished ends here */
static void finish_row(void)
{
    if (U.cols) {
        U.y = U.row_y + U.row_h + 3;
        U.cols = 0;
    }
}

void ui_columns(int n)
{
    finish_row();
    U.cols = n > 1 ? n : 0;
    U.col_i = 0;
    U.col_w = n > 1 ? (U.inner_w - U.indent - (n - 1) * 4) / n : 0;
    U.row_y = U.y;
    U.row_h = 0;
}

void ui_space(int px)
{
    finish_row();
    U.y += px;
}
void ui_indent(int px) { U.indent = UB_MAX(0, U.indent + px); }

ui_rect ui_next_rect(int h)
{
    ui_rect r;
    if (U.cols) {
        r = ui_r(U.x + U.indent + U.col_i * (U.col_w + 4), U.row_y, U.col_w, h);
        U.row_h = UB_MAX(U.row_h, h);
        if (++U.col_i == U.cols) {
            U.y = U.row_y + U.row_h + 3;
            U.cols = 0;
        }
        return r;
    }
    r = ui_r(U.x + U.indent, U.y, U.inner_w - U.indent, h);
    U.y += h + 3;
    return r;
}

/* the value half of a labelled widget (label drawn on the left) */
static ui_rect value_rect(const char *label, ui_rect r, u32 id)
{
    char txt[64];
    shown(label, txt, sizeof txt);
    if (!txt[0]) return r;
    int lw = r.w * 2 / 5;
    text_in(ui_r(r.x, r.y, lw, r.h), 0, txt, UI_DIM);
    (void)id;
    return ui_r(r.x + lw, r.y, r.w - lw, r.h);
}

/* ------------------------------------------------------------ widgets */

void ui_label_color(const char *text, u32 color)
{
    ui_rect r = ui_next_rect(U.lh);
    text_in(r, 0, text, color);
    record(make_id(text), text, r);
}

void ui_label(const char *text) { ui_label_color(text, UI_TEXT); }

void ui_heading(const char *text)
{
    ui_rect r = ui_next_rect(U.lh + 2);
    text_in(r, 0, text, UI_ACCENT | 0x202020);
    fill(ui_r(r.x, r.y + r.h - 1, r.w, 1), UI_BORDER);
}

void ui_separator(void)
{
    ui_rect r = ui_next_rect(5);
    fill(ui_r(r.x, r.y + 2, r.w, 1), UI_BORDER);
}

static bool button_impl(const char *label, bool enabled, bool lit)
{
    u32 id = make_id(label);
    ui_rect r = ui_next_rect(U.wh);
    char txt[96];
    shown(label, txt, sizeof txt);
    int f = enabled ? interact(id, r) : 0;
    u32 bg = !enabled ? UI_PANEL : (f & F_HELD) && (f & F_HOVER) ? UI_PRESS : lit ? UI_ACCENT : (f & F_HOVER) ? UI_HOVER : UI_WIDGET;
    frame_rect(r, bg, UI_BORDER);
    int tw = ub_render_text_width(txt);
    text_in(r, tw < r.w - 8 ? (r.w - tw) / 2 : 4, txt, enabled ? UI_TEXT : UI_DIM);
    record(id, label, r);
    return enabled && (f & F_CLICK);
}

bool ui_button(const char *label) { return button_impl(label, true, false); }
bool ui_button_enabled(const char *label, bool enabled) { return button_impl(label, enabled, false); }
bool ui_toggle(const char *label, bool on) { return button_impl(label, true, on); }

bool ui_checkbox(const char *label, bool *v)
{
    u32 id = make_id(label);
    ui_rect r = ui_next_rect(U.wh);
    ui_rect vr = value_rect(label, r, id);
    int s = U.wh - 6;
    ui_rect box = ui_r(vr.x + 2, vr.y + 3, s, s);
    int f = interact(id, r);
    frame_rect(box, (f & F_HOVER) ? UI_HOVER : UI_FIELD, UI_BORDER);
    if (*v) fill(ui_r(box.x + 3, box.y + 3, s - 6, s - 6), UI_ACCENT);
    record(id, label, r);
    if (f & F_CLICK) {
        *v = !*v;
        return true;
    }
    return false;
}

/* ---- single-line editing ---- */

static int prev_char(const char *s, int i)
{
    if (i <= 0) return 0;
    --i;
    while (i > 0 && ((u8)s[i] & 0xC0) == 0x80) --i;
    return i;
}

static int next_char(const char *s, int i)
{
    if (!s[i]) return i;
    ++i;
    while (s[i] && ((u8)s[i] & 0xC0) == 0x80) ++i;
    return i;
}

static void start_edit(u32 id, const char *text)
{
    U.focus = id;
    U.focus_seen = true;
    ub_fmt(U.edit, sizeof U.edit, "%s", text);
    U.edit_cur = (int)ub_strlen(U.edit);
    U.edit_start_select = true;
}

/* Keys for the focused one-line box. 1 = commit, -1 = cancel, 0 = still editing. */
static int edit_keys(void)
{
    const ub_input *in = U.in;
    int len = (int)ub_strlen(U.edit);
    for (const char *p = in->text; *p;) {
        const char *start = p;
        u32 c = ub_utf8_decode(&p);
        if (c == '\r') return 1;
        if (c == 0x1B) return -1;
        if (c == '\b') {
            if (U.edit_start_select) { /* typing over the whole text */
                U.edit[0] = 0;
                U.edit_cur = len = 0;
            } else if (U.edit_cur > 0) {
                int pc = prev_char(U.edit, U.edit_cur);
                memmove(U.edit + pc, U.edit + U.edit_cur, (size_t)(len - U.edit_cur + 1));
                len -= U.edit_cur - pc;
                U.edit_cur = pc;
            }
        } else if (c >= 32) {
            if (U.edit_start_select) {
                U.edit[0] = 0;
                U.edit_cur = len = 0;
            }
            int n = (int)(p - start);
            if (len + n < EDIT_MAX - 1) {
                memmove(U.edit + U.edit_cur + n, U.edit + U.edit_cur, (size_t)(len - U.edit_cur + 1));
                memcpy(U.edit + U.edit_cur, start, (size_t)n);
                U.edit_cur += n;
                len += n;
            }
        }
        U.edit_start_select = false;
    }
    if (in->key_repeat[UB_KEY_LEFT]) U.edit_cur = prev_char(U.edit, U.edit_cur);
    if (in->key_repeat[UB_KEY_RIGHT]) U.edit_cur = next_char(U.edit, U.edit_cur);
    if (in->key_repeat[UB_KEY_HOME]) U.edit_cur = 0;
    if (in->key_repeat[UB_KEY_END]) U.edit_cur = len;
    if (in->key_repeat[UB_KEY_DELETE] && U.edit_cur < len) {
        int nc = next_char(U.edit, U.edit_cur);
        memmove(U.edit + U.edit_cur, U.edit + nc, (size_t)(len - nc + 1));
    }
    if (in->key_repeat[UB_KEY_LEFT] || in->key_repeat[UB_KEY_RIGHT] || in->key_repeat[UB_KEY_HOME] ||
        in->key_repeat[UB_KEY_END] || in->key_repeat[UB_KEY_DELETE])
        U.edit_start_select = false;
    if (in->key_pressed[UB_KEY_TAB]) return 1;
    return 0;
}

static void draw_edit(ui_rect vr)
{
    frame_rect(vr, UI_FIELD, UI_ACCENT);
    ui_rect saved = U.clip;
    set_clip(intersect(saved, ui_r(vr.x + 2, vr.y, vr.w - 4, vr.h)));
    int cx = ub_render_text_width_n(U.edit, U.edit_cur);
    int shift = cx > vr.w - 12 ? cx - (vr.w - 12) : 0;
    if (U.edit_start_select && U.edit[0])
        fill(ui_r(vr.x + 4, vr.y + 3, UB_MIN(ub_render_text_width(U.edit), vr.w - 8), vr.h - 6), UI_PRESS);
    ub_render_text(vr.x + 4 - shift, vr.y + (vr.h - U.lh) / 2 + 1, U.edit, UI_TEXT | UB_TEXT_FLAT);
    if (((int)(U.time * 2.0f) & 1) == 0 || U.edit_start_select)
        fill(ui_r(vr.x + 4 - shift + cx, vr.y + 3, 1, vr.h - 6), UI_TEXT);
    set_clip(saved);
}

/* One-line box logic shared by text and number fields. Returns 1 when an
 * edit is committed (result in U.edit). */
static int field_logic(u32 id, ui_rect vr, const char *current, bool drag_number, f32 *num, f32 step)
{
    int f = interact(id, vr);
    if (U.focus == id) {
        U.focus_seen = true;
        int k = edit_keys();
        bool away = U.in->mouse_pressed[UB_MOUSE_LEFT] && !ui_inside(vr, U.in->mouse_x, U.in->mouse_y);
        if (k == 1 || away) {
            U.focus = 0;
            return 1;
        }
        if (k == -1) U.focus = 0;
        return 0;
    }
    if (drag_number && (f & F_HELD)) {
        if (f & F_PRESS) U.press_value = *num;
        int dx = U.in->mouse_x - U.press_x;
        if (!U.dragging && (dx > 3 || dx < -3)) U.dragging = true;
        if (U.dragging) {
            *num = U.press_value + (f32)(dx / 4) * step;
            return 2;
        }
    }
    if ((f & F_CLICK) && !U.dragging) start_edit(id, current);
    if (f & F_CLICK) U.dragging = false;
    return 0;
}

bool ui_text_field(const char *label, char *buf, int cap)
{
    u32 id = make_id(label);
    ui_rect r = ui_next_rect(U.wh);
    ui_rect vr = value_rect(label, r, id);
    record(id, label, r);
    int res = field_logic(id, vr, buf, false, NULL, 0);
    if (U.focus == id) {
        draw_edit(vr);
        return false;
    }
    frame_rect(vr, mouse_in(vr) ? UI_WIDGET : UI_FIELD, UI_BORDER);
    text_in(vr, 4, res == 1 ? U.edit : buf, UI_TEXT);
    if (res == 1 && str_cmp(U.edit, buf) != 0) {
        ub_fmt(buf, cap, "%s", U.edit);
        return true;
    }
    return false;
}

bool ui_number(const char *label, f32 *v, f32 step, int decimals)
{
    u32 id = make_id(label);
    ui_rect r = ui_next_rect(U.wh);
    ui_rect vr = value_rect(label, r, id);
    record(id, label, r);
    char cur[48];
    ub_fmt_num(cur, sizeof cur, *v, decimals);
    f32 before = *v;
    int res = field_logic(id, vr, cur, true, v, step);
    if (U.focus == id) {
        draw_edit(vr);
        return false;
    }
    bool changed = false;
    if (res == 1) {
        f32 nv;
        if (ub_parse_num(U.edit, &nv) && nv != *v) {
            *v = nv;
            changed = true;
        }
    } else if (res == 2) {
        changed = *v != before;
    }
    ub_fmt_num(cur, sizeof cur, *v, decimals);
    frame_rect(vr, mouse_in(vr) || U.active == id ? UI_WIDGET : UI_FIELD, UI_BORDER);
    text_in(vr, 4, cur, UI_TEXT);
    return changed;
}

bool ui_slider(const char *label, f32 *v, f32 lo, f32 hi)
{
    u32 id = make_id(label);
    ui_rect r = ui_next_rect(U.wh);
    ui_rect vr = value_rect(label, r, id);
    record(id, label, r);
    int f = interact(id, vr);
    f32 before = *v;
    if ((f & F_HELD) && hi > lo) {
        f32 t = (f32)(U.in->mouse_x - vr.x - 4) / (f32)UB_MAX(1, vr.w - 8);
        *v = lo + ub_clampf(t, 0, 1) * (hi - lo);
    }
    frame_rect(vr, UI_FIELD, UI_BORDER);
    f32 t = hi > lo ? ub_clampf((*v - lo) / (hi - lo), 0, 1) : 0;
    int fw = (int)((f32)(vr.w - 4) * t);
    fill(ui_r(vr.x + 2, vr.y + 2, fw, vr.h - 4), (f & F_HOVER) || (f & F_HELD) ? UI_ACCENT : UI_PRESS);
    char txt[32];
    ub_fmt_num(txt, sizeof txt, *v, 2);
    text_in(vr, 6, txt, UI_TEXT);
    return *v != before;
}

int ui_stepper(const char *label, const char *value)
{
    u32 id = make_id(label);
    ui_rect r = ui_next_rect(U.wh);
    ui_rect vr = value_rect(label, r, id);
    record(id, label, r);
    int bw = U.wh + 4;
    ui_rect lb = ui_r(vr.x, vr.y, bw, vr.h), rb = ui_r(vr.x + vr.w - bw, vr.y, bw, vr.h);
    ui_rect mid = ui_r(vr.x + bw + 2, vr.y, vr.w - 2 * bw - 4, vr.h);
    int fl = interact(id ^ 0x2545F491u, lb), fr = interact(id ^ 0x9E3779B9u, rb);
    frame_rect(lb, (fl & F_HELD) ? UI_PRESS : (fl & F_HOVER) ? UI_HOVER : UI_WIDGET, UI_BORDER);
    frame_rect(rb, (fr & F_HELD) ? UI_PRESS : (fr & F_HOVER) ? UI_HOVER : UI_WIDGET, UI_BORDER);
    int cw = ub_render_text_width("<");
    ub_render_text(lb.x + (lb.w - cw) / 2, lb.y + (lb.h - U.lh) / 2 + 1, "<", UI_TEXT | UB_TEXT_FLAT);
    ub_render_text(rb.x + (rb.w - cw) / 2, rb.y + (rb.h - U.lh) / 2 + 1, ">", UI_TEXT | UB_TEXT_FLAT);
    fill(mid, UI_FIELD);
    int tw = ub_render_text_width(value);
    text_in(mid, tw < mid.w - 8 ? (mid.w - tw) / 2 : 4, value, UI_TEXT);
    char sub[80];
    ub_fmt(sub, sizeof sub, "%s <", label);
    record(id ^ 1, sub, lb);
    ub_fmt(sub, sizeof sub, "%s >", label);
    record(id ^ 2, sub, rb);
    return (fr & F_CLICK) ? 1 : (fl & F_CLICK) ? -1 : 0;
}

/* ---- pop-up lists ---- */

static bool take_result(u32 id, int *out)
{
    if (U.result_id != id) return false;
    U.result_id = 0;
    *out = U.result_val;
    return true;
}

static void open_popup(u32 id, int kind, ui_rect anchor, const char *const *items, const u32 *pal, int n, int sel)
{
    U.pop_kind = kind;
    U.pop_id = id;
    U.pop_anchor = anchor;
    U.pop_items = items;
    U.pop_palette = pal;
    U.pop_count = n;
    U.pop_sel = sel;
    U.top_cur = LAYER_POPUP;
}

bool ui_choice(const char *label, int *sel, const char *const *items, int count)
{
    u32 id = make_id(label);
    ui_rect r = ui_next_rect(U.wh);
    ui_rect vr = value_rect(label, r, id);
    record(id, label, r);
    int f = interact(id, vr);
    frame_rect(vr, (f & F_HOVER) ? UI_HOVER : UI_WIDGET, UI_BORDER);
    const char *cur = *sel >= 0 && *sel < count ? items[*sel] : "Choose...";
    text_in(ui_r(vr.x, vr.y, vr.w - 14, vr.h), 4, cur, UI_TEXT);
    ub_render_text(vr.x + vr.w - 12, vr.y + (vr.h - U.lh) / 2 + 1, "v", UI_DIM | UB_TEXT_FLAT);
    if (f & F_CLICK) {
        if (U.pop_kind && U.pop_id == id) U.pop_kind = POP_NONE;
        else open_popup(id, POP_CHOICE, vr, items, NULL, count, *sel);
    }
    int v;
    if (take_result(id, &v) && v != *sel) {
        *sel = v;
        return true;
    }
    return false;
}

bool ui_color(const char *label, u32 *rgb, const u32 *palette, int count)
{
    u32 id = make_id(label);
    ui_rect r = ui_next_rect(U.wh);
    ui_rect vr = value_rect(label, r, id);
    record(id, label, r);
    int f = interact(id, vr);
    frame_rect(vr, (f & F_HOVER) ? UI_HOVER : UI_WIDGET, UI_BORDER);
    fill(ui_r(vr.x + 3, vr.y + 3, U.wh, vr.h - 6), *rgb & 0xFFFFFF);
    char hex[16];
    ub_fmt(hex, sizeof hex, "%02X%02X%02X", (*rgb >> 16) & 255, (*rgb >> 8) & 255, *rgb & 255);
    text_in(vr, U.wh + 8, hex, UI_DIM);
    if (f & F_CLICK) {
        if (U.pop_kind && U.pop_id == id) U.pop_kind = POP_NONE;
        else open_popup(id, POP_COLOR, vr, NULL, palette, count, -1);
    }
    int v;
    if (take_result(id, &v) && v >= 0 && v < count && palette[v] != *rgb) {
        *rgb = palette[v];
        return true;
    }
    return false;
}

static void draw_popup(void)
{
    ui_rect a = U.pop_anchor;
    if (U.pop_kind == POP_CHOICE) {
        int n = U.pop_count;
        int visible = UB_MIN(n, 16);
        ui_rect pr = ui_r(a.x, a.y + a.h, UB_MAX(a.w, 140), visible * U.wh + 4);
        if (pr.y + pr.h > U.h) pr.y = a.y - pr.h;
        if (pr.x + pr.w > U.w) pr.x = U.w - pr.w;
        add_pop_rect(pr);
        frame_rect(pr, UI_PANEL, UI_BORDER);
        int first = U.pop_sel >= visible ? U.pop_sel - visible + 1 : 0;
        for (int i = 0; i < visible; ++i) {
            int k = first + i;
            ui_rect ir = ui_r(pr.x + 2, pr.y + 2 + i * U.wh, pr.w - 4, U.wh);
            bool over = ui_inside(ir, U.in->mouse_x, U.in->mouse_y);
            if (over || k == U.pop_sel) fill(ir, over ? UI_ACCENT : UI_WIDGET);
            text_in(ir, 6, U.pop_items[k], UI_TEXT);
            record(hash_str(U.pop_items[k], 77u), U.pop_items[k], ir);
            if (over && U.in->mouse_pressed[UB_MOUSE_LEFT]) {
                U.result_id = U.pop_id;
                U.result_val = k;
                U.pop_kind = POP_NONE;
            }
        }
    } else if (U.pop_kind == POP_COLOR) {
        int cols = 8, cell = U.wh + 2;
        int rows = (U.pop_count + cols - 1) / cols;
        ui_rect pr = ui_r(a.x, a.y + a.h, cols * cell + 6, rows * cell + 6);
        if (pr.y + pr.h > U.h) pr.y = a.y - pr.h;
        if (pr.x + pr.w > U.w) pr.x = U.w - pr.w;
        add_pop_rect(pr);
        frame_rect(pr, UI_PANEL, UI_BORDER);
        for (int i = 0; i < U.pop_count; ++i) {
            ui_rect cr = ui_r(pr.x + 3 + (i % cols) * cell, pr.y + 3 + (i / cols) * cell, cell - 2, cell - 2);
            bool over = ui_inside(cr, U.in->mouse_x, U.in->mouse_y);
            if (over) fill(ui_r(cr.x - 1, cr.y - 1, cr.w + 2, cr.h + 2), UI_TEXT);
            fill(cr, U.pop_palette[i] & 0xFFFFFF);
            char name[24];
            ub_fmt(name, sizeof name, "colour %d", i);
            record(hash_str(name, 99u), name, cr);
            if (over && U.in->mouse_pressed[UB_MOUSE_LEFT]) {
                U.result_id = U.pop_id;
                U.result_val = i;
                U.pop_kind = POP_NONE;
            }
        }
    }
}

int ui_tree_row(const char *label, int depth, bool has_children, bool expanded, bool selected, u32 icon_color)
{
    u32 id = make_id(label);
    ui_rect r = ui_next_rect(U.lh + 2);
    int f = interact(id, r);
    if (selected) fill(r, UI_PRESS);
    else if (f & F_HOVER) fill(r, UI_WIDGET);
    int x = r.x + depth * 12;
    ui_rect arrow = ui_r(x, r.y, 12, r.h);
    if (has_children) ub_render_text(x + 2, r.y + (r.h - U.lh) / 2 + 1, expanded ? "-" : "+", UI_DIM | UB_TEXT_FLAT);
    fill(ui_r(x + 14, r.y + r.h / 2 - 4, 8, 8), icon_color & 0xFFFFFF);
    char txt[96];
    shown(label, txt, sizeof txt);
    text_in(ui_r(x + 26, r.y, r.w - (x + 26 - r.x), r.h), 0, txt, UI_TEXT);
    record(id, label, r);
    int out = 0;
    if (f & F_PRESS) {
        bool on_arrow = has_children && ui_inside(arrow, U.in->mouse_x, U.in->mouse_y);
        out |= on_arrow ? UI_ROW_TOGGLE : UI_ROW_CLICK;
        if (!on_arrow && double_click(id)) out |= UI_ROW_DOUBLE;
    }
    return out;
}

/* -------------------------------------------------------------- menus */

void ui_menubar_begin(ui_rect bar)
{
    U.bar = bar;
    U.bar_x = bar.x + 4;
    U.menubar_layer_saved = U.layer;
    fill(bar, UI_BG);
    fill(ui_r(bar.x, bar.y + bar.h - 1, bar.w, 1), UI_BORDER);
    ui_claim(bar);
    if (U.open_menu) add_pop_rect(bar);
}

bool ui_menu_begin(const char *label)
{
    u32 id = make_id(label) ^ 0x5bd1e995u;
    char txt[64];
    shown(label, txt, sizeof txt);
    int w = ub_render_text_width(txt) + 16;
    ui_rect r = ui_r(U.bar_x, U.bar.y, w, U.bar.h - 1);
    U.bar_x += w;
    U.layer = U.open_menu ? LAYER_POPUP : U.menubar_layer_saved;
    bool over = mouse_in(r);
    if (over && U.in->mouse_pressed[UB_MOUSE_LEFT]) U.open_menu = U.open_menu == id ? 0 : id;
    else if (over && U.open_menu && U.open_menu != id) U.open_menu = id; /* slide across the bar */
    bool open = U.open_menu == id;
    if (open || over) fill(r, open ? UI_PRESS : UI_WIDGET);
    text_in(r, 8, txt, UI_TEXT);
    record(id, label, r);
    if (!open) {
        U.layer = U.menubar_layer_saved;
        return false;
    }
    U.menu_open_now = true;
    U.top_cur = LAYER_POPUP;
    U.layer = LAYER_POPUP;
    U.menu_x = r.x;
    U.menu_y = r.y + r.h;
    U.menu_w = 250;
    U.menu_h = 0;
    return true;
}

bool ui_menu_item(const char *label, const char *shortcut, bool enabled)
{
    u32 id = make_id(label) ^ 0x1b873593u;
    ui_rect r = ui_r(U.menu_x, U.menu_y + U.menu_h, U.menu_w, U.wh);
    U.menu_h += U.wh;
    bool over = ui_inside(r, U.in->mouse_x, U.in->mouse_y);
    fill(r, over && enabled ? UI_ACCENT : UI_PANEL);
    char txt[64];
    shown(label, txt, sizeof txt);
    text_in(r, 10, txt, enabled ? UI_TEXT : UI_DIM);
    if (shortcut && shortcut[0]) {
        int sw = ub_render_text_width(shortcut);
        ub_render_text(r.x + r.w - sw - 10, r.y + (r.h - U.lh) / 2 + 1, shortcut, UI_DIM | UB_TEXT_FLAT);
    }
    record(id, label, r);
    if (over && enabled && U.in->mouse_pressed[UB_MOUSE_LEFT]) {
        U.open_menu = 0;
        U.pop_n = 0; /* (this click is used up) */
        return true;
    }
    return false;
}

void ui_menu_separator(void)
{
    ui_rect r = ui_r(U.menu_x, U.menu_y + U.menu_h, U.menu_w, 7);
    U.menu_h += 7;
    fill(r, UI_PANEL);
    fill(ui_r(r.x + 6, r.y + 3, r.w - 12, 1), UI_BORDER);
}

void ui_menu_end(void)
{
    ui_rect mr = ui_r(U.menu_x, U.menu_y, U.menu_w, U.menu_h);
    /* border */
    fill(ui_r(mr.x - 1, mr.y, 1, mr.h + 1), UI_BORDER);
    fill(ui_r(mr.x + mr.w, mr.y, 1, mr.h + 1), UI_BORDER);
    fill(ui_r(mr.x - 1, mr.y + mr.h, mr.w + 2, 1), UI_BORDER);
    if (U.open_menu) add_pop_rect(mr);
    U.layer = U.menubar_layer_saved;
}

void ui_menubar_end(void)
{
    if (U.open_menu && !U.menu_open_now) U.open_menu = 0; /* its menu was not declared */
    U.layer = U.menubar_layer_saved;
}

bool ui_menu_open(void) { return U.open_menu != 0; }
void ui_close_menus(void) { U.open_menu = 0; U.pop_kind = POP_NONE; }

/* ------------------------------------------------------------ dialogs */

void ui_dialog_begin(const char *title, int w, int h)
{
    U.in_dialog = true;
    if (U.top_cur < LAYER_DIALOG) U.top_cur = LAYER_DIALOG;
    U.layer = LAYER_DIALOG;
    w = UB_MIN(w, U.w - 8);
    h = UB_MIN(h, U.h - 8);
    ui_rect r = ui_r((U.w - w) / 2, (U.h - h) / 2, w, h);
    U.dlg = r;
    no_clip();
    frame_rect(ui_r(r.x - 2, r.y - 2, r.w + 4, r.h + 4), UI_PANEL, UI_ACCENT);
    ui_rect tb = ui_r(r.x, r.y, r.w, U.wh);
    fill(tb, UI_ACCENT);
    text_in(tb, 8, title, 0xFFFFFF);
    ui_claim(ui_r(0, 0, U.w, U.h));
    ui_panel_begin(title, ui_r(r.x, r.y + U.wh, r.w, r.h - U.wh), true);
}

void ui_dialog_end(void)
{
    ui_panel_end();
    U.in_dialog = false;
    U.layer = LAYER_BASE;
    no_clip();
}

/* ---------------------------------------------------- multi-line text */

/* width of the first n bytes of a line, tabs as TAB_SPACES spaces */
static int line_width(const char *s, int n)
{
    int w = 0, run = 0, sp = ub_render_text_width(" ");
    for (int i = 0; i < n; ++i) {
        if (s[i] == '\t') {
            w += ub_render_text_width_n(s + run, i - run) + sp * TAB_SPACES;
            run = i + 1;
        }
    }
    return w + ub_render_text_width_n(s + run, n - run);
}

static int line_start(const char *buf, int i)
{
    while (i > 0 && buf[i - 1] != '\n') --i;
    return i;
}

static int line_end(const char *buf, int i)
{
    while (buf[i] && buf[i] != '\n') ++i;
    return i;
}

/* byte in the line starting at `ls` nearest to x pixels */
static int col_at(const char *buf, int ls, int x)
{
    int le = line_end(buf, ls), best = ls, bestd = 1 << 30;
    for (int i = ls; i <= le; i = (i < le ? next_char(buf, i) : le + 1)) {
        int d = line_width(buf + ls, i - ls) - x;
        if (d < 0) d = -d;
        if (d < bestd) {
            bestd = d;
            best = i;
        }
        if (i == le) break;
    }
    return best;
}

static int line_of(const char *buf, int pos)
{
    int n = 0;
    for (int i = 0; i < pos && buf[i]; ++i) n += buf[i] == '\n';
    return n;
}

static int nth_line(const char *buf, int line)
{
    int i = 0;
    while (line > 0 && buf[i]) {
        if (buf[i] == '\n') --line;
        ++i;
    }
    return i;
}

static bool insert_text(char *buf, int cap, int *cur, const char *s, int n)
{
    int len = (int)ub_strlen(buf);
    if (len + n >= cap) return false;
    memmove(buf + *cur + n, buf + *cur, (size_t)(len - *cur + 1));
    memcpy(buf + *cur, s, (size_t)n);
    *cur += n;
    return true;
}

bool ui_text_area(const char *id_label, char *buf, int cap, ui_rect r, ui_text_state *st)
{
    u32 id = make_id(id_label);
    const ub_input *in = U.in;
    bool changed = false;
    ui_claim(r);
    frame_rect(r, UI_FIELD, st->focused ? UI_ACCENT : UI_BORDER);
    int gutter = ub_render_text_width("0000") + 10;
    int lh = U.lh;
    int rows = UB_MAX(1, (r.h - 6) / lh);
    int len = (int)ub_strlen(buf);
    if (st->cursor > len) st->cursor = len;
    if (st->cursor < 0) st->cursor = 0;
    record(id, id_label, r);

    bool over = mouse_in(r);
    if (in->mouse_pressed[UB_MOUSE_LEFT]) {
        if (over) {
            st->focused = true;
            U.focus = id;
            int line = st->scroll + (in->mouse_y - r.y - 3) / lh;
            int ls = nth_line(buf, line);
            st->cursor = col_at(buf, ls, in->mouse_x - (r.x + gutter));
            st->want_col = -1;
        } else {
            st->focused = false;
        }
    }
    if (over && in->wheel) st->scroll -= in->wheel / UB_WHEEL_NOTCH * 3;
    if (st->focused) {
        U.focus = id;
        U.focus_seen = true;
        int cur = st->cursor;
        for (const char *p = in->text; *p;) {
            const char *start = p;
            u32 c = ub_utf8_decode(&p);
            if (c == '\b') {
                if (cur > 0) {
                    int pc = prev_char(buf, cur);
                    memmove(buf + pc, buf + cur, (size_t)((int)ub_strlen(buf) - cur + 1));
                    cur = pc;
                    changed = true;
                }
            } else if (c == '\r') {
                /* new line, keeping this line's indentation */
                char ind[64];
                int ls = line_start(buf, cur), n = 0;
                ind[n++] = '\n';
                while ((buf[ls] == '\t' || buf[ls] == ' ') && n < 63 && ls < cur) ind[n++] = buf[ls++];
                changed |= insert_text(buf, cap, &cur, ind, n);
            } else if (c == 0x1B) {
                st->focused = false;
            } else if (c >= 32) {
                changed |= insert_text(buf, cap, &cur, start, (int)(p - start));
            }
        }
        if (in->key_repeat[UB_KEY_TAB]) changed |= insert_text(buf, cap, &cur, "\t", 1);
        len = (int)ub_strlen(buf);
        bool ctrl = in->key_down[UB_KEY_CONTROL];
        if (in->key_repeat[UB_KEY_LEFT]) cur = prev_char(buf, cur);
        if (in->key_repeat[UB_KEY_RIGHT]) cur = next_char(buf, cur);
        if (in->key_repeat[UB_KEY_HOME]) cur = ctrl ? 0 : line_start(buf, cur);
        if (in->key_repeat[UB_KEY_END]) cur = ctrl ? len : line_end(buf, cur);
        if (in->key_repeat[UB_KEY_DELETE] && cur < len) {
            int nc = next_char(buf, cur);
            memmove(buf + cur, buf + nc, (size_t)(len - nc + 1));
            changed = true;
        }
        int vert = (in->key_repeat[UB_KEY_DOWN] ? 1 : 0) - (in->key_repeat[UB_KEY_UP] ? 1 : 0);
        vert += ((in->key_repeat[UB_KEY_PAGEDOWN] ? 1 : 0) - (in->key_repeat[UB_KEY_PAGEUP] ? 1 : 0)) * rows;
        if (vert) {
            int ls = line_start(buf, cur);
            if (st->want_col < 0) st->want_col = line_width(buf + ls, cur - ls);
            int line = line_of(buf, cur) + vert;
            if (line < 0) line = 0;
            cur = col_at(buf, nth_line(buf, line), st->want_col);
        } else if (in->key_repeat[UB_KEY_LEFT] || in->key_repeat[UB_KEY_RIGHT] || in->key_repeat[UB_KEY_HOME] ||
                   in->key_repeat[UB_KEY_END] || in->text[0]) {
            st->want_col = -1;
        }
        if (cur != st->cursor || changed) { /* keep the cursor in view */
            int cl = line_of(buf, cur);
            if (cl < st->scroll) st->scroll = cl;
            if (cl >= st->scroll + rows) st->scroll = cl - rows + 1;
        }
        st->cursor = cur;
    }
    int total = line_of(buf, (int)ub_strlen(buf)) + 1;
    if (st->scroll > total - 1) st->scroll = total - 1;
    if (st->scroll < 0) st->scroll = 0;

    /* draw */
    ui_rect saved = U.clip;
    set_clip(intersect(saved, ui_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2)));
    fill(ui_r(r.x + 1, r.y + 1, gutter - 4, r.h - 2), UI_BG);
    int pos = nth_line(buf, st->scroll);
    int cur_line = line_of(buf, st->cursor);
    for (int row = 0; row < rows && pos <= (int)ub_strlen(buf); ++row) {
        int le = line_end(buf, pos);
        int ty = r.y + 3 + row * lh;
        char num[12];
        ub_fmt(num, sizeof num, "%d", st->scroll + row + 1);
        ub_render_text(r.x + gutter - 8 - ub_render_text_width(num), ty, num, UI_DIM | UB_TEXT_FLAT);
        /* the line, with tabs expanded */
        char line[512];
        int n = 0;
        for (int i = pos; i < le && n < 500; ++i) {
            if (buf[i] == '\t') for (int k = 0; k < TAB_SPACES; ++k) line[n++] = ' ';
            else line[n++] = buf[i];
        }
        line[n] = 0;
        ub_render_text(r.x + gutter, ty, line, UI_TEXT | UB_TEXT_FLAT);
        if (st->focused && st->scroll + row == cur_line && ((int)(U.time * 2.0f) & 1) == 0)
            fill(ui_r(r.x + gutter + line_width(buf + pos, st->cursor - pos), ty, 1, lh - 2), UI_TEXT);
        if (!buf[le]) break;
        pos = le + 1;
    }
    set_clip(saved);
    return changed;
}
