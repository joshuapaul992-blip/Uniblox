/* Uniblox - immediate-mode UI for tools (the editor's panels, menus and
 * dialogs).
 *
 * Every frame the app calls ui_begin, then declares widgets; each call
 * draws the widget and returns what the user did with it. Nothing is kept
 * between frames except which widget is being pressed or edited, scroll
 * positions and open menus/pop-ups, so the UI always shows the app's
 * current state. Drawing goes through the renderer's 2D calls, so it works
 * in both renderers and in 256 colours.
 *
 * Labels: "Name" shows "Name". "Name##2" shows "Name" with a different
 * identity, for repeated labels. Labelled value widgets (fields, checkboxes,
 * sliders, choices) draw the label on the left and the value on the right,
 * like a property sheet.
 *
 * Pop-ups (menus, choice lists, colour grids) and dialogs are drawn on top
 * and take the mouse from everything beneath them. */
#ifndef UB_UI_H
#define UB_UI_H

#include "platform/platform.h"

typedef struct {
    int x, y, w, h;
} ui_rect;

static inline ui_rect ui_r(int x, int y, int w, int h)
{
    ui_rect r = { x, y, w, h };
    return r;
}
static inline bool ui_inside(ui_rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* colours */
enum {
    UI_BG = 0x2B2B2B, UI_PANEL = 0x333333, UI_WIDGET = 0x454545, UI_HOVER = 0x555555, UI_PRESS = 0x2F5F9F,
    UI_ACCENT = 0x3D7EDB, UI_TEXT = 0xE8E8E8, UI_DIM = 0x9C9C9C, UI_BORDER = 0x1C1C1C, UI_FIELD = 0x252525,
    UI_ERROR = 0xFF8070,
};

void ui_begin(const ub_input *in, int width, int height, f32 dt);
void ui_end(void);           /* draws pop-ups; call last */
bool ui_wants_mouse(void);   /* the mouse is over the UI (the 3D view should ignore it) */
bool ui_wants_keyboard(void);/* a text box has the keyboard */
int ui_line_height(void);    /* height of one widget row */
f32 ui_time(void);

/* ------------------------------------------------------------ layout */
/* A panel: background, clipping, a vertical stack of widgets. scroll:
 * the contents scroll with the mouse wheel when taller than the panel. */
void ui_panel_begin(const char *id, ui_rect r, bool scroll);
void ui_panel_end(void);
/* The next n widgets share one row (equal widths). */
void ui_columns(int n);
void ui_space(int px);
void ui_indent(int px);      /* add to (or with a negative, take from) the left margin */
ui_rect ui_next_rect(int h); /* reserve space for something drawn by the caller */
/* Treat a rectangle the caller laid out as UI (the mouse belongs to the UI there). */
void ui_claim(ui_rect r);
int ui_panel_scroll(void);     /* how far the current panel is scrolled (pixels) */
int ui_tree_row_height(void);  /* space one ui_tree_row takes */

/* ------------------------------------------------------------ widgets */
void ui_label(const char *text);
void ui_label_color(const char *text, u32 color);
void ui_heading(const char *text);
void ui_separator(void);
bool ui_button(const char *label);
bool ui_button_enabled(const char *label, bool enabled);
bool ui_toggle(const char *label, bool on);            /* a button that stays lit when on */
bool ui_checkbox(const char *label, bool *v);
/* Number box: drag left/right to change by `step` per 4 pixels, or click
 * and type. True on a change. */
bool ui_number(const char *label, f32 *v, f32 step, int decimals);
bool ui_slider(const char *label, f32 *v, f32 lo, f32 hi);
/* Text box. True when the user finishes an edit (Enter or clicking away)
 * that changed it; Esc cancels. */
bool ui_text_field(const char *label, char *buf, int cap);
/* A choice from a list (shown as a pop-up). True on a change. `items` must
 * stay valid until the next frame. */
bool ui_choice(const char *label, int *sel, const char *const *items, int count);
/* A colour from a palette grid (pop-up). True on a change. `palette` must
 * stay valid until the next frame. */
bool ui_color(const char *label, u32 *rgb, const u32 *palette, int count);
/* A tree row (the explorer). Returns UI_ROW_* flags. */
enum { UI_ROW_CLICK = 1, UI_ROW_TOGGLE = 2, UI_ROW_DOUBLE = 4 };
int ui_tree_row(const char *label, int depth, bool has_children, bool expanded, bool selected, u32 icon_color);

/* -------------------------------------------------------------- menus */
void ui_menubar_begin(ui_rect bar);
bool ui_menu_begin(const char *label);  /* true while open: declare its items */
bool ui_menu_item(const char *label, const char *shortcut, bool enabled);
void ui_menu_separator(void);
void ui_menu_end(void);
void ui_menubar_end(void);
bool ui_menu_open(void);                /* some menu is open */
void ui_close_menus(void);

/* ------------------------------------------------------------ dialogs */
/* A modal window, centred, drawn on top of everything; the rest of the UI
 * ignores the mouse while it is up. Declare it after everything else. */
void ui_dialog_begin(const char *title, int w, int h);
void ui_dialog_end(void);

/* ---------------------------------------------------- multi-line text */
typedef struct {
    int cursor;        /* byte offset */
    int scroll;        /* first line shown */
    int want_col;      /* column kept while moving up and down (pixels) */
    bool focused;
} ui_text_state;
/* A code editor: arrows, Home/End, PgUp/PgDn, Ctrl+Home/End, Backspace,
 * Delete, Enter (keeps the indentation), Tab. True when the text changed. */
bool ui_text_area(const char *id, char *buf, int cap, ui_rect r, ui_text_state *st);

/* ------------------------------------------------------------- tests */
/* Where the widget with this label (shown text, or the full "a##b"
 * label) was drawn last frame. */
bool ui_find(const char *label, ui_rect *out);

#endif
