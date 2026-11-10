/* Uniblox - Roblox's GUI objects, laid out, drawn and clicked.
 *
 * Reads the GUI from the world each frame it changed (ScreenGuis under the
 * player's PlayerGui; BillboardGuis and SurfaceGuis on parts) and works out
 * every object's AbsolutePosition, AbsoluteSize and AbsoluteRotation, as
 * Roblox does: Size, Position, AnchorPoint, SizeConstraint, UIScale,
 * UIPadding, UISizeConstraint, UIAspectRatioConstraint, UIListLayout,
 * UIGridLayout, UIPageLayout, AutomaticSize, ScrollingFrames, rotation,
 * clipping, ZIndex in both ZIndexBehavior modes and the GUI inset at the top
 * of the screen. Then it draws them with ub_canvas (UICorner, UIStroke,
 * UIGradient, borders, images, rich text, TextScaled) and turns the mouse
 * and keyboard into the events scripts expect (MouseEnter,
 * MouseButton1Click, Activated, InputBegan, Focused, FocusLost...).
 *
 * OS-independent: the app gives it input and a camera, and composites the
 * pictures it makes. */
#ifndef UB_GUI_H
#define UB_GUI_H

#include "gui/ub_canvas.h"
#include "render/ub_render.h"
#include "world/ub_world.h"

#define UB_GUI_INSET 36          /* Roblox's top bar in 2022: ScreenGuis start below it unless IgnoreGuiInset */

typedef struct ub_gui ub_gui;

typedef struct {
    f32 mouse_x, mouse_y;        /* in screen (frame) pixels */
    bool down[3], pressed[3], released[3]; /* left, right, middle */
    int wheel;                   /* notches, + away from the user */
    const char *text;            /* characters typed this frame (UTF-8; \r Enter, \b Backspace, \x1b Esc) */
    bool key_pressed[256];       /* UB_KEY_* this frame (arrows, Home, End, Delete...) */
    bool mouse_free;             /* false while the mouse turns the camera: no hovering */
} ub_gui_input;

enum {
    UB_GEV_MOUSE_ENTER, UB_GEV_MOUSE_LEAVE, UB_GEV_MOUSE_MOVED, UB_GEV_WHEEL_FORWARD, UB_GEV_WHEEL_BACKWARD,
    UB_GEV_B1_DOWN, UB_GEV_B1_UP, UB_GEV_B1_CLICK, UB_GEV_B2_DOWN, UB_GEV_B2_UP, UB_GEV_B2_CLICK,
    UB_GEV_ACTIVATED,
    UB_GEV_INPUT_BEGAN, UB_GEV_INPUT_ENDED, UB_GEV_INPUT_CHANGED,  /* button: 0..2, 3 = movement, 4 = wheel */
    UB_GEV_FOCUSED, UB_GEV_FOCUS_LOST,                             /* flag: Enter pressed */
    UB_GEV_PROP_CHANGED,                                           /* key: the property (AbsoluteSize, Text...) */
    UB_GEV_COUNT
};

typedef struct {
    i32 inst;
    u8 type;
    u8 button;
    u8 flag;
    i32 x, y;
    u32 key;
} ub_gui_event;

/* A BillboardGui or SurfaceGui: a picture to draw in the world. */
typedef struct {
    i32 inst;
    ub_render_image image;
    vec3 corners[4];             /* world (Uniblox) positions of the top-left, top-right, bottom-right, bottom-left */
    bool on_top;                 /* AlwaysOnTop */
    f32 brightness;
} ub_gui_quad;

ub_gui *ub_gui_new(void);
void ub_gui_free(ub_gui *g);

/* The instance whose LayerCollectors are the screen's (the player's
 * PlayerGui), or -1. */
void ub_gui_set_root(ub_gui *g, i32 player_gui);
/* A second root drawn like the PlayerGui (the engine's own GUIs: prompts). */
void ub_gui_set_core_root(ub_gui *g, i32 core_gui);

/* Each frame, before scripts run: lays out what changed, handles the
 * mouse and keyboard and queues events. `cam` (may be NULL) places
 * BillboardGuis and SurfaceGuis. */
void ub_gui_update(ub_gui *g, ub_world *w, int screen_w, int screen_h, const ub_camera *cam, const ub_gui_input *in,
                   f64 time);
/* After scripts ran: lays out again if they changed things and redraws
 * the pictures that changed. */
void ub_gui_render(ub_gui *g, ub_world *w, const ub_camera *cam);

/* Events queued since the last clear. */
int ub_gui_events(const ub_gui *g, const ub_gui_event **out);
void ub_gui_clear_events(ub_gui *g);

/* The screen's GUI: a picture the size of the screen, and the rectangles
 * of it that have anything in them. NULL if there is no screen GUI. */
const ub_render_image *ub_gui_screen(const ub_gui *g, const int **rects, int *nrects);
/* BillboardGuis and SurfaceGuis to draw this frame. */
int ub_gui_quads(const ub_gui *g, const ub_gui_quad **out);

/* Is the mouse over GUI that takes it (an Active object: buttons, text
 * boxes, frames with Active)? Then the world does not get the click. */
bool ub_gui_mouse_taken(const ub_gui *g);
/* Did the GUI use the wheel (a ScrollingFrame scrolled)? */
bool ub_gui_wheel_taken(const ub_gui *g);
/* A TextBox has the keyboard: game keys must not move the character. */
bool ub_gui_keyboard_taken(const ub_gui *g);
i32 ub_gui_focused_box(const ub_gui *g);
/* TextBox:CaptureFocus() / ReleaseFocus() */
void ub_gui_focus(ub_gui *g, ub_world *w, i32 textbox);
void ub_gui_release_focus(ub_gui *g, ub_world *w, bool enter);

/* What scripts read (laid out first if the world changed). False if the
 * instance is not a laid-out GUI object. */
typedef struct {
    f32 x, y, w, h;              /* AbsolutePosition, AbsoluteSize */
    f32 rotation;                /* AbsoluteRotation */
    f32 text_w, text_h;          /* TextBounds */
    bool text_fits;              /* TextFits */
    f32 content_w, content_h;    /* layouts: AbsoluteContentSize */
    f32 cell_w, cell_h;          /* UIGridLayout: AbsoluteCellSize */
    i32 cells_x, cells_y;        /* UIGridLayout: AbsoluteCellCount */
    f32 canvas_w, canvas_h;      /* ScrollingFrame: AbsoluteCanvasSize */
    f32 window_w, window_h;      /* ScrollingFrame: AbsoluteWindowSize */
    i32 page;                    /* UIPageLayout: the current page (a child instance), -1 none */
} ub_gui_info;
bool ub_gui_info_of(ub_gui *g, ub_world *w, i32 inst, ub_gui_info *out);
/* The screen size the GUI was last laid out for. */
void ub_gui_screen_size(const ub_gui *g, int *w, int *h);
/* UIPageLayout:JumpTo / Next / Previous. */
void ub_gui_page_jump(ub_gui *g, ub_world *w, i32 layout, i32 page_child);
void ub_gui_page_step(ub_gui *g, ub_world *w, i32 layout, int step);
/* GuiObjects at a point, topmost first (PlayerGui:GetGuiObjectsAtPosition). */
int ub_gui_objects_at(ub_gui *g, ub_world *w, f32 x, f32 y, i32 *out, int max);

/* Counts for the overlay. */
typedef struct {
    u32 layers, objects, drawn_us, layout_us;
} ub_gui_stats;
void ub_gui_get_stats(const ub_gui *g, ub_gui_stats *out);

#endif
