/* Uniblox - the client UI (milestone 9A): what a 2021-era game's player
 * had around the view, in Uniblox's own look: a top bar (menu and chat
 * buttons), the chat window and chat bubbles, the player list with the
 * game's leaderstats, a health bar, notifications from scripts, and the
 * game menu (people, settings, help; reset character, leave, resume).
 *
 * It is made of GUI objects under the game's CoreGui root, so the GUI
 * engine draws it in both renderers, rounded and see-through. The app
 * hit-tests it itself (the layout is its own, in screen pixels), so it
 * works with scripts off too. Scripts turn parts off with
 * SetCoreGuiEnabled and feed it with SetCore and Chat:Chat
 * (game/ub_game.h, ub_hud_state). */
#include "app/app_hud.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "world/ub_classes.h"
#include "crt/ub_crt.h"

u32 brickcolor_number_rgb(u32 number); /* script/ub_sapi_types.c */

#define CHAT_LINES 8
#define LINE_H 20
#define MAX_STATS 4
#define MAX_ROWS 8
#define MAX_BUBBLES 8

static struct {
    ub_game *g;
    i32 root;                      /* the CoreGui root these live in */
    i32 top, menu_btn, chat_btn;
    i32 chat, chat_bg, chat_line[CHAT_LINES], chat_input, chat_input_text;
    i32 list, list_bg, list_head[1 + MAX_STATS], list_cell[MAX_ROWS][1 + MAX_STATS];
    i32 health, health_fill;
    i32 notes, note_frame[4], note_title[4], note_text[4];
    i32 bubble[MAX_BUBBLES], bubble_text[MAX_BUBBLES];
    i32 menu, menu_tabs[3], menu_body, menu_lines[10], menu_btns[3], menu_btn_text[3];
    u32 cache[512];                /* hash of the last text set on each label slot (avoids relayout) */
    bool cache_set[512];
    u32 chat_seen;                 /* chat_count when the chat was last looked at */
    f32 chat_active_until;
    int sw, sh;
    int tab;                       /* the menu's tab: 0 people, 1 settings, 2 help */
    int confirm;                   /* 0 none, 1 reset, 2 leave */
    bool menu_open;
    bool list_shown;
    bool chat_hidden;              /* the chat button hides the window */
    int below_chat;                /* the first free y under the chat (for the F3 text) */
    int nstats, nrows;
    ub_hud_rect rect[HUD_HIT_COUNT];
} H;

/* ------------------------------------------------------------ building */

static ub_world *W(void) { return &H.g->world; }

static ub_value vudim2(f32 sx, i32 ox, f32 sy, i32 oy)
{
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_UDIM2;
    v.v.ud2.sx = sx;
    v.v.ud2.ox = ox;
    v.v.ud2.sy = sy;
    v.v.ud2.oy = oy;
    return v;
}

static ub_value vcolor(u32 rgb)
{
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_COLOR3;
    v.v.c3.r = (f32)((rgb >> 16) & 255) / 255.0f;
    v.v.c3.g = (f32)((rgb >> 8) & 255) / 255.0f;
    v.v.c3.b = (f32)(rgb & 255) / 255.0f;
    return v;
}

static void setv(i32 i, const char *k, ub_value v)
{
    if (i < 0) return;
    const ub_value *old = ub_world_prop_named(W(), i, k);
    if (old && ub_value_equal(old, &v)) return; /* unchanged: no relayout */
    ub_world_set_prop_named(W(), i, k, &v);
}

static i32 mk(const char *cls, i32 parent, const char *name)
{
    i32 i = ub_world_add(W(), parent, UB_CLASS_OTHER, cls, name);
    if (i < 0) return -1;
    W()->inst[i].flags |= UB_INST_RUNTIME;
    ub_cdb_fill_defaults(W(), i, cls);
    return i;
}

static i32 screen(const char *name, int order)
{
    i32 s = mk("ScreenGui", H.root, name);
    setv(s, "IgnoreGuiInset", ub_vbool(true));
    setv(s, "DisplayOrder", ub_vint(order));
    setv(s, "ResetOnSpawn", ub_vbool(false));
    return s;
}

static i32 panel(i32 parent, const char *name, u32 color, f32 transparency, i32 corner)
{
    i32 f = mk("Frame", parent, name);
    setv(f, "BackgroundColor3", vcolor(color));
    setv(f, "BackgroundTransparency", ub_vfloat(transparency));
    setv(f, "BorderSizePixel", ub_vint(0));
    if (corner > 0) {
        i32 c = mk("UICorner", f, "Corner");
        ub_value u;
        memset(&u, 0, sizeof u);
        u.type = UB_PV_UDIM;
        u.v.ud.o = corner;
        setv(c, "CornerRadius", u);
    }
    return f;
}

static i32 label(i32 parent, const char *name, f32 size, bool bold, u32 color, int xalign)
{
    i32 t = mk("TextLabel", parent, name);
    setv(t, "BackgroundTransparency", ub_vfloat(1));
    setv(t, "TextColor3", vcolor(color));
    setv(t, "TextSize", ub_vfloat(size));
    setv(t, "TextXAlignment", ub_venum((u32)xalign)); /* Left 0, Right 1, Center 2 */
    ub_value f;
    memset(&f, 0, sizeof f);
    f.type = UB_PV_FONT;
    f.v.font.family = (char *)"rbxasset://fonts/families/SourceSansPro.json";
    f.v.font.weight = bold ? 700 : 400;
    setv(t, "FontFace", f);
    setv(t, "Font", ub_venum(bold ? 4u : 3u));
    return t;
}

/* sets a label's text if it changed (slot: a stable number per label) */
static void set_text(i32 t, int slot, const char *s)
{
    if (t < 0 || slot < 0 || slot >= 512) return;
    u32 h = 2166136261u;
    for (const char *p = s; *p; ++p) h = (h ^ (u8)*p) * 16777619u;
    if (H.cache_set[slot] && H.cache[slot] == h) return;
    H.cache[slot] = h;
    H.cache_set[slot] = true;
    ub_value v = ub_vstr(s, (u32)ub_strlen(s));
    ub_world_set_prop_named(W(), t, "Text", &v);
}

static void place(i32 i, int x, int y, int w, int h)
{
    setv(i, "Position", vudim2(0, x, 0, y));
    setv(i, "Size", vudim2(0, w, 0, h));
}

static void show(i32 i, bool on)
{
    if (i >= 0) setv(i, "Visible", ub_vbool(on));
}

static void enable(i32 screen_gui, bool on) /* (ScreenGuis have Enabled, not Visible) */
{
    if (screen_gui >= 0) setv(screen_gui, "Enabled", ub_vbool(on));
}

static void build(void)
{
    H.root = H.g->core_gui;
    if (H.root < 0) return;
    /* the top bar: menu and chat buttons */
    H.top = screen("TopBar", 5);
    H.menu_btn = panel(H.top, "Menu", 0x15171C, 0.35f, 9);
    for (int k = 0; k < 3; ++k) {
        i32 bar = panel(H.menu_btn, "Bar", 0xFFFFFF, 0, 1);
        place(bar, 9, 10 + k * 6, 14, 2);
    }
    H.chat_btn = panel(H.top, "Chat", 0x15171C, 0.35f, 9);
    i32 bub = panel(H.chat_btn, "Bubble", 0xFFFFFF, 0, 4);
    place(bub, 8, 9, 16, 11);
    i32 tail = panel(H.chat_btn, "Tail", 0xFFFFFF, 0, 0);
    place(tail, 10, 18, 5, 5);
    setv(tail, "Rotation", ub_vfloat(45));
    /* the chat */
    H.chat = screen("Chat", 3);
    H.chat_bg = panel(H.chat, "Window", 0x15171C, 1, 10);
    for (int i = 0; i < CHAT_LINES; ++i) {
        H.chat_line[i] = label(H.chat_bg, "Line", 17, false, 0xF2F3F5, 0);
        setv(H.chat_line[i], "RichText", ub_vbool(true));
        setv(H.chat_line[i], "TextStrokeTransparency", ub_vfloat(0.6f));
        setv(H.chat_line[i], "TextTruncate", ub_venum(1)); /* AtEnd */
    }
    H.chat_input = panel(H.chat, "Input", 0x15171C, 0.3f, 8);
    H.chat_input_text = label(H.chat_input, "Text", 17, false, 0xF2F3F5, 0);
    place(H.chat_input_text, 10, 0, 400, 28);
    /* the player list */
    H.list = screen("PlayerList", 4);
    H.list_bg = panel(H.list, "List", 0x15171C, 0.35f, 10);
    for (int c = 0; c <= MAX_STATS; ++c) H.list_head[c] = label(H.list_bg, "Head", 15, true, 0xC8CDD6, c ? 1 : 0);
    for (int r = 0; r < MAX_ROWS; ++r)
        for (int c = 0; c <= MAX_STATS; ++c) H.list_cell[r][c] = label(H.list_bg, "Cell", 17, c == 0, 0xFFFFFF, c ? 1 : 0);
    /* health */
    H.health = panel(H.list, "Health", 0x15171C, 0.35f, 4);
    H.health_fill = panel(H.health, "Fill", 0x3FD07A, 0, 4);
    /* notifications */
    H.notes = screen("Notifications", 6);
    for (int i = 0; i < 4; ++i) {
        H.note_frame[i] = panel(H.notes, "Notification", 0x15171C, 0.2f, 10);
        H.note_title[i] = label(H.note_frame[i], "Title", 18, true, 0xFFFFFF, 0);
        H.note_text[i] = label(H.note_frame[i], "Text", 16, false, 0xC8CDD6, 0);
        setv(H.note_text[i], "TextWrapped", ub_vbool(true));
        setv(H.note_text[i], "TextYAlignment", ub_venum(0)); /* Top */
        place(H.note_title[i], 14, 8, 272, 22);
        place(H.note_text[i], 14, 30, 272, 36);
    }
    /* chat bubbles: billboards made on demand */
    for (int i = 0; i < MAX_BUBBLES; ++i) H.bubble[i] = H.bubble_text[i] = -1;
    /* the menu */
    H.menu = screen("GameMenu", 10);
    i32 dim = panel(H.menu, "Dim", 0x000000, 0.45f, 0);
    setv(dim, "Size", vudim2(1, 0, 1, 0));
    setv(dim, "Active", ub_vbool(true));
    H.menu_body = panel(H.menu, "Panel", 0x1B1E25, 0.05f, 14);
    static const char *const tabs[3] = { "People", "Settings", "Help" };
    for (int t = 0; t < 3; ++t) {
        H.menu_tabs[t] = panel(H.menu_body, "Tab", 0x2A2F3A, 0, 8);
        i32 tl = label(H.menu_tabs[t], "Text", 18, true, 0xFFFFFF, 2);
        setv(tl, "Size", vudim2(1, 0, 1, 0));
        ub_value v = ub_vstr(tabs[t], (u32)ub_strlen(tabs[t]));
        ub_world_set_prop_named(W(), tl, "Text", &v);
    }
    for (int i = 0; i < 10; ++i) {
        H.menu_lines[i] = label(H.menu_body, "Line", 17, false, 0xE6E9EF, 0);
        setv(H.menu_lines[i], "RichText", ub_vbool(true));
    }
    for (int b = 0; b < 3; ++b) {
        H.menu_btns[b] = panel(H.menu_body, "Button", b == 2 ? 0x2D6FD8 : 0x2A2F3A, 0, 9);
        H.menu_btn_text[b] = label(H.menu_btns[b], "Text", 18, true, 0xFFFFFF, 2);
        setv(H.menu_btn_text[b], "Size", vudim2(1, 0, 1, 0));
    }
    enable(H.menu, false);
}

/* ------------------------------------------------------------ helpers */

static void escape(const char *s, char *out, int cap) /* for RichText */
{
    int n = 0;
    for (; *s && n < cap - 6; ++s) {
        const char *rep = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : NULL;
        if (rep)
            while (*rep && n < cap - 1) out[n++] = *rep++;
        else out[n++] = *s;
    }
    out[n] = 0;
}

static i32 find_child(i32 parent, const char *name)
{
    for (i32 c = parent >= 0 ? W()->inst[parent].first_child : -1; c >= 0; c = W()->inst[c].next_sibling)
        if (ub_world_alive(W(), c) && !ub_strcmp(ub_world_str(W(), W()->inst[c].name), name)) return c;
    return -1;
}

static void value_text(i32 v, char *out, int cap)
{
    const ub_value *p = ub_world_prop_named(W(), v, "Value");
    out[0] = 0;
    if (!p) return;
    if (p->type == UB_PV_STRING) ub_fmt(out, cap, "%s", p->v.str.s);
    else if (p->type == UB_PV_INT) ub_fmt(out, cap, "%d", p->v.i);
    else if (p->type == UB_PV_DOUBLE || p->type == UB_PV_FLOAT) {
        f64 d = p->type == UB_PV_DOUBLE ? p->v.d : p->v.f;
        if (d == (f64)(i64)d && d < 1e12 && d > -1e12) ub_fmt(out, cap, "%d", (int)(i64)d);
        else ub_fmt_num(out, cap, (f32)d, 2);
    } else if (p->type == UB_PV_BOOL) ub_fmt(out, cap, "%s", p->v.b ? "true" : "false");
}

static ub_hud_rect rr(int x, int y, int w, int h)
{
    ub_hud_rect r = { x, y, w, h };
    return r;
}

/* ------------------------------------------------------------ the frame */

void hud_begin_game(ub_game *g)
{
    memset(&H, 0, sizeof H);
    H.g = g;
    H.list_shown = true;
    if (g && g->core_gui >= 0) build();
}

void hud_set_menu(bool open)
{
    H.menu_open = open;
    H.confirm = 0;
    if (open) H.tab = 0;
}

void hud_toggle_list(void) { H.list_shown = !H.list_shown; }
void hud_toggle_chat(void) { H.chat_hidden = !H.chat_hidden; }
int hud_below_chat(void) { return H.below_chat; }
void hud_set_tab(int tab) { H.tab = tab < 0 ? (H.tab + 1) % 3 : tab % 3; H.confirm = 0; }
void hud_confirm(int what) { H.confirm = what; }
int hud_confirming(void) { return H.confirm; }
bool hud_menu_open(void) { return H.menu_open; }

const ub_hud_rect *hud_hit_rect(int which) { return which >= 0 && which < HUD_HIT_COUNT ? &H.rect[which] : NULL; }

int hud_hit(int x, int y)
{
    for (int i = HUD_HIT_COUNT - 1; i >= 0; --i) {
        const ub_hud_rect *r = &H.rect[i];
        if (r->w > 0 && x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h) return i;
    }
    return -1;
}

void hud_frame(int sw, int sh, const char *typing, f32 dt)
{
    ub_game *g = H.g;
    if (!g || H.root < 0 || !ub_world_alive(W(), H.root)) return;
    (void)dt;
    H.sw = sw;
    H.sh = sh;
    memset(H.rect, 0, sizeof H.rect);
    const ub_hud_state *hs = &g->hud;
    char buf[512];
    bool chat_on = !hs->core_off[UB_CORE_CHAT];

    /* top bar */
    place(H.menu_btn, 12, 4, 32, 32);
    H.rect[HUD_HIT_MENU] = rr(12, 4, 32, 32);
    show(H.chat_btn, chat_on);
    place(H.chat_btn, 52, 4, 32, 32);
    if (chat_on) H.rect[HUD_HIT_CHAT] = rr(52, 4, 32, 32);

    /* chat (the window hides with the chat button, but shows while typing) */
    bool window_on = chat_on && (!H.chat_hidden || typing);
    enable(H.chat, window_on);
    setv(H.chat_btn, "BackgroundTransparency", ub_vfloat(window_on ? 0.15f : 0.35f));
    H.below_chat = 44;
    if (window_on) {
        int w = sw - 24 < 460 ? sw - 24 : 460, x = 12, y = 44;
        if (typing || hs->chat_count != H.chat_seen) {
            H.chat_active_until = g->time + 8;
            H.chat_seen = hs->chat_count;
        }
        bool active = typing || g->time < H.chat_active_until;
        setv(H.chat_bg, "BackgroundTransparency", ub_vfloat(active ? 0.55f : 1.0f));
        place(H.chat_bg, x, y, w, CHAT_LINES * LINE_H + 12);
        u32 n = hs->chat_count < CHAT_LINES ? hs->chat_count : CHAT_LINES;
        for (int i = 0; i < CHAT_LINES; ++i) {
            int slot = 10 + i;
            place(H.chat_line[i], 10, 6 + i * LINE_H, w - 20, LINE_H);
            int idx = i - (CHAT_LINES - (int)n); /* the newest at the bottom */
            if (idx < 0) {
                set_text(H.chat_line[i], slot, "");
                continue;
            }
            const ub_chat_line *l = &hs->chat[(hs->chat_count - n + (u32)idx) % UB_HUD_CHAT_LINES];
            char t[UB_HUD_TEXT * 2], who[120];
            escape(l->text, t, (int)sizeof t);
            if (l->who[0]) {
                escape(l->who, who, (int)sizeof who);
                ub_fmt(buf, (int)sizeof buf, "<font color=\"#%06X\"><b>%s:</b></font> %s", l->color & 0xFFFFFF, who, t);
            } else {
                ub_fmt(buf, (int)sizeof buf, "<font color=\"#%06X\">%s</font>", l->color & 0xFFFFFF, t);
            }
            set_text(H.chat_line[i], slot, buf);
        }
        int iy = y + CHAT_LINES * LINE_H + 12 + 4;
        place(H.chat_input, x, iy, w, 28);
        place(H.chat_input_text, 10, 0, w - 20, 28);
        show(H.chat_input, true);
        setv(H.chat_input, "BackgroundTransparency", ub_vfloat(typing ? 0.25f : active ? 0.6f : 0.8f));
        if (typing) {
            char t[UB_HUD_TEXT * 2];
            ub_fmt(t, (int)sizeof t, "%s_", typing);
            set_text(H.chat_input_text, 30, t);
            setv(H.chat_input_text, "TextColor3", vcolor(0xFFFFFF));
        } else {
            set_text(H.chat_input_text, 30, "To chat click here or press /");
            setv(H.chat_input_text, "TextColor3", vcolor(0xA9AFBA));
        }
        H.rect[HUD_HIT_CHAT_INPUT] = rr(x, iy, w, 28);
        H.below_chat = iy + 28 + 8;
    }

    /* player list: the players and the game's leaderstats */
    bool list_on = !hs->core_off[UB_CORE_PLAYERLIST] && H.list_shown;
    i32 ls = g->player >= 0 ? find_child(g->player, "leaderstats") : -1;
    i32 stats[MAX_STATS];
    int ns = 0;
    for (i32 c = ls >= 0 ? W()->inst[ls].first_child : -1; c >= 0 && ns < MAX_STATS; c = W()->inst[c].next_sibling)
        if (ub_world_alive(W(), c) && ub_world_prop_named(W(), c, "Value")) stats[ns++] = c;
    int lw = 190 + 76 * ns, lx = sw - 12 - lw, ly = 4;
    int rows = g->player >= 0 ? 1 : 0;
    int lh = 30 + rows * 28 + 8;
    show(H.list_bg, list_on);
    if (list_on) {
        place(H.list_bg, lx, ly, lw, lh);
        for (int c = 0; c <= MAX_STATS; ++c) {
            bool used = c <= ns;
            show(H.list_head[c], used);
            if (!used) continue;
            int cx = c == 0 ? 12 : 190 + 76 * (c - 1), cw = c == 0 ? 170 : 66;
            place(H.list_head[c], cx, 6, cw, 22);
            set_text(H.list_head[c], 40 + c, c == 0 ? "People" : ub_world_str(W(), W()->inst[stats[c - 1]].name));
        }
        for (int r = 0; r < MAX_ROWS; ++r)
            for (int c = 0; c <= MAX_STATS; ++c) {
                bool used = r < rows && c <= ns;
                show(H.list_cell[r][c], used);
                if (!used) continue;
                int cx = c == 0 ? 12 : 190 + 76 * (c - 1), cw = c == 0 ? 170 : 66;
                place(H.list_cell[r][c], cx, 32 + r * 28, cw, 26);
                if (c == 0) {
                    const ub_value *tc = ub_world_prop_named(W(), g->player, "TeamColor");
                    const ub_value *neutral = ub_world_prop_named(W(), g->player, "Neutral");
                    bool team = tc && neutral && neutral->type == UB_PV_BOOL && !neutral->v.b;
                    setv(H.list_cell[r][c], "TextColor3", vcolor(team ? brickcolor_number_rgb((u32)tc->v.i) : 0xFFFFFF));
                    set_text(H.list_cell[r][c], 50 + r * 8 + c, ub_world_str(W(), W()->inst[g->player].name));
                } else {
                    value_text(stats[c - 1], buf, 64);
                    set_text(H.list_cell[r][c], 50 + r * 8 + c, buf);
                }
            }
        H.rect[HUD_HIT_LIST] = rr(lx, ly, lw, lh);
    }
    H.nstats = ns;
    H.nrows = rows;
    /* health: under the list, while hurt */
    bool hurt = !hs->core_off[UB_CORE_HEALTH] && g->max_health > 0 && g->health < g->max_health;
    show(H.health, hurt);
    if (hurt) {
        int hw = 180, hx = sw - 12 - hw, hy = (list_on ? ly + lh : ly) + 8;
        place(H.health, hx, hy, hw, 10);
        f32 f = ub_clampf(g->health / g->max_health, 0, 1);
        place(H.health_fill, 0, 0, (int)(f * (f32)hw), 10);
        setv(H.health_fill, "BackgroundColor3", vcolor(f > 0.5f ? 0x3FD07A : f > 0.25f ? 0xF5CD30 : 0xFD4A4A));
    }

    /* notifications: the newest at the bottom right, for their duration */
    int shown = 0;
    for (int i = 0; i < 4; ++i) {
        const ub_notice *n = NULL;
        while (shown < 8 && shown < (int)hs->notice_count) {
            const ub_notice *c = &hs->notices[(hs->notice_count - 1 - (u32)shown) % 8];
            shown++;
            if (g->time - c->time < c->duration) {
                n = c;
                break;
            }
        }
        show(H.note_frame[i], n != NULL);
        if (!n) continue;
        place(H.note_frame[i], sw - 12 - 300, sh - 12 - (i + 1) * 82, 300, 74);
        set_text(H.note_title[i], 80 + i, n->title);
        set_text(H.note_text[i], 84 + i, n->text);
    }

    /* chat bubbles: the last ones of the last 10 seconds */
    for (int i = 0; i < MAX_BUBBLES; ++i) {
        const ub_bubble *b = i < (int)hs->bubble_count ? &hs->bubbles[(hs->bubble_count - 1 - (u32)i) % 16] : NULL;
        bool live = b && g->time - b->time < 10 && b->adornee >= 0 && ub_world_alive(W(), b->adornee) &&
                    W()->inst[b->adornee].part >= 0 && chat_on;
        if (live && H.bubble[i] < 0) {
            H.bubble[i] = mk("BillboardGui", H.root, "Bubble");
            setv(H.bubble[i], "AlwaysOnTop", ub_vbool(true));
            setv(H.bubble[i], "LightInfluence", ub_vfloat(0));
            i32 bg = panel(H.bubble[i], "Body", 0xFFFFFF, 0.05f, 10);
            setv(bg, "Size", vudim2(1, 0, 1, 0));
            H.bubble_text[i] = label(bg, "Text", 16, false, 0x1B1E25, 2);
            setv(H.bubble_text[i], "Size", vudim2(1, -12, 1, 0));
            setv(H.bubble_text[i], "Position", vudim2(0, 6, 0, 0));
            setv(H.bubble_text[i], "TextWrapped", ub_vbool(true));
        }
        if (H.bubble[i] < 0) continue;
        setv(H.bubble[i], "Enabled", ub_vbool(live));
        if (!live) continue;
        ub_value ad;
        memset(&ad, 0, sizeof ad);
        ad.type = UB_PV_REF;
        ad.v.ref = b->adornee;
        setv(H.bubble[i], "Adornee", ad);
        int len = (int)ub_strlen(b->text), w = 24 + len * 8;
        w = w < 60 ? 60 : w > 260 ? 260 : w;
        int lines = (len * 8) / 236 + 1;
        setv(H.bubble[i], "Size", vudim2(0, w, 0, 14 + lines * 18));
        ub_value off;
        memset(&off, 0, sizeof off);
        off.type = UB_PV_VECTOR3;
        off.v.v3.y = 2.4f + (f32)i * 1.6f;
        setv(H.bubble[i], "StudsOffsetWorldSpace", off);
        set_text(H.bubble_text[i], 100 + i, b->text);
    }

    /* the menu */
    enable(H.menu, H.menu_open);
    if (H.menu_open) {
        int pw = sw - 40 < 560 ? sw - 40 : 560, ph = 400, px = (sw - pw) / 2, py = (sh - ph) / 2;
        place(H.menu_body, px, py, pw, ph);
        int tw = (pw - 48) / 3;
        for (int t = 0; t < 3; ++t) {
            place(H.menu_tabs[t], 16 + t * (tw + 8), 16, tw, 36);
            setv(H.menu_tabs[t], "BackgroundColor3", vcolor(t == H.tab ? 0x2D6FD8 : 0x2A2F3A));
            H.rect[HUD_HIT_TAB_PEOPLE + t] = rr(px + 16 + t * (tw + 8), py + 16, tw, 36);
        }
        static const char *const help[] = {
            "<b>W A S D</b> move   <b>Space</b> jump   <b>right mouse</b> look around",
            "<b>Mouse wheel</b> zoom   <b>1-9</b> tools   <b>Click</b> use a tool",
            "<b>/</b> or <b>Enter</b> chat   <b>Tab</b> the player list",
            "<b>E</b> use a prompt   <b>Esc</b> this menu",
            "<b>F11</b> fullscreen   <b>F3</b> the information text",
            "<b>F6</b> quality or resolution   <b>F9</b> frame rate cap",
            "",
            "Uniblox runs places made for Roblox; it is not Roblox.",
        };
        const char *lines[10];
        int nl = 0;
        static char people[2][160];
        if (H.confirm) {
            lines[nl++] = H.confirm == 1 ? "<b>Reset your character?</b>" : "<b>Leave the game?</b>";
            lines[nl++] = H.confirm == 1 ? "It falls over and comes back at a spawn point." : "Uniblox closes.";
        } else if (H.tab == 0) {
            if (g->player >= 0) {
                ub_fmt(people[0], (int)sizeof people[0], "<b>%s</b>   (you)", ub_world_str(W(), W()->inst[g->player].name));
                lines[nl++] = people[0];
            }
            lines[nl++] = "";
            lines[nl++] = "Playing alone: other players come with multiplayer.";
        } else if (H.tab == 1) {
            lines[nl++] = "Graphics, sound, the mouse and the camera:";
            lines[nl++] = "press <b>S</b> or click Settings below to open them.";
        } else {
            for (int i = 0; i < 8; ++i) lines[nl++] = help[i];
        }
        for (int i = 0; i < 10; ++i) {
            show(H.menu_lines[i], i < nl);
            if (i >= nl) continue;
            place(H.menu_lines[i], 28, 70 + i * 28, pw - 56, 26);
            set_text(H.menu_lines[i], 200 + i, lines[i]);
        }
        /* buttons: Reset / Leave / Resume, or Yes / No while asking; Settings on its tab */
        const char *bt[3];
        if (H.confirm) {
            bt[0] = H.confirm == 1 ? "Reset" : "Leave";
            bt[1] = "";
            bt[2] = "Don't";
        } else if (H.tab == 1) {
            bt[0] = "Reset character (R)";
            bt[1] = "Settings (S)";
            bt[2] = "Resume (Esc)";
        } else {
            bt[0] = H.g->hud.reset_off ? "" : "Reset character (R)";
            bt[1] = "Leave (L)";
            bt[2] = "Resume (Esc)";
        }
        if (!H.confirm && H.tab == 1 && H.g->hud.reset_off) bt[0] = "";
        int bw = (pw - 48) / 3;
        for (int b = 0; b < 3; ++b) {
            bool on = bt[b][0] != 0;
            show(H.menu_btns[b], on);
            place(H.menu_btns[b], 16 + b * (bw + 8), ph - 56, bw, 40);
            set_text(H.menu_btn_text[b], 220 + b, bt[b]);
            if (on) H.rect[HUD_HIT_BTN0 + b] = rr(px + 16 + b * (bw + 8), py + ph - 56, bw, 40);
        }
        H.rect[HUD_HIT_MENU_PANEL] = rr(px, py, pw, ph);
        H.rect[HUD_HIT_MENU_DIM] = rr(0, 0, sw, sh); /* (the panel and its buttons lie over it) */
    }
}

int hud_button_meaning(int b)
{
    /* what the menu's button b does now */
    if (H.confirm) return b == 0 ? (H.confirm == 1 ? HUD_DO_RESET : HUD_DO_LEAVE) : b == 2 ? HUD_DO_CANCEL : HUD_DO_NONE;
    if (b == 0) return HUD_ASK_RESET;
    if (b == 1) return H.tab == 1 ? HUD_DO_SETTINGS : HUD_ASK_LEAVE;
    return HUD_DO_RESUME;
}
