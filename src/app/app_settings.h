/* Uniblox - the game's settings: uniblox.cfg and the in-game menu (Esc).
 *
 * The menu changes what it can at once (frame rate cap, fullscreen and its
 * display mode, render resolution, colours, quality, presenting, volume,
 * mouse, field of view). The renderer, the timer and scripts take effect
 * the next time the game starts (or the place is loaded). */
#ifndef UB_APP_SETTINGS_H
#define UB_APP_SETTINGS_H

#include "platform/platform.h"

typedef struct {
    ub_display_prefs prefs;   /* renderer, window, fullscreen + its mode, fps cap, vsync, sw_height, timer */
    int quality;              /* ub_quality (OpenGL) */
    bool gl_shaders;
    int colors;               /* software: 0 = automatic, 8 or 32 */
    bool changed_only;        /* software: send only what changed */
    bool hide_overlay;
    int volume;               /* 0..100 */
    bool muted;
    int mouse_pct;            /* mouse-look speed, 100 = normal */
    bool invert_y;
    bool shift_lock;          /* Shift toggles shift lock */
    int fov;                  /* degrees, vertical */
    bool scripts;
    int net_sim;              /* the loopback link acts like: 0 nothing (perfect), 1 broadband, 2 a 56k modem */
    int render_threads;       /* fast software renderer: 0 = one per processor */
    int zoom, yaw_deg, pitch_deg;
    char place[260];
} ub_settings;

#define UB_NET_SIM_COUNT 3
const char *settings_net_sim_name(int v);

void settings_defaults(ub_settings *s);
void settings_parse(ub_settings *s, const char *text);
int settings_write(const ub_settings *s, char *buf, int cap);

/* What the menu did this frame. */
enum {
    SET_FPS = 1 << 0, SET_FULLSCREEN = 1 << 1, SET_FSMODE = 1 << 2, SET_QUALITY = 1 << 3, SET_SWRES = 1 << 4,
    SET_COLORS = 1 << 5, SET_PRESENT = 1 << 6, SET_SOUND = 1 << 7, SET_CONTROLS = 1 << 8,
    SET_RESTART_NEEDED = 1 << 9, SET_OVERLAY = 1 << 10, SET_NET = 1 << 11,
    MENU_RESUME = 1 << 16, MENU_QUIT = 1 << 17, MENU_RELOAD = 1 << 18,
};

typedef struct {
    int page;                 /* 0 = paused, 1 = display, 2 = sound and controls, 3 = game */
    ub_display_mode modes[48];
    int mode_count;
    bool modes_listed;
    bool opengl;              /* which renderer is running */
    bool fast;                /* the fast software renderer (it has quality levels too) */
} ub_menu;

/* Declares the menu (between ui_begin and ui_end); changes `s` and
 * returns SET_ and MENU_ flags. */
int settings_menu(ub_menu *m, ub_settings *s, const ub_input *in);

#endif
