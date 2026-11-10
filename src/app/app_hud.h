/* Uniblox - the client UI (see app_hud.c). */
#ifndef APP_HUD_H
#define APP_HUD_H

#include "game/ub_game.h"

typedef struct { int x, y, w, h; } ub_hud_rect;

/* What can be clicked (in screen pixels, the top inset included); later
 * ones lie over earlier ones. */
enum {
    HUD_HIT_MENU, HUD_HIT_CHAT, HUD_HIT_CHAT_INPUT, HUD_HIT_LIST,
    HUD_HIT_MENU_DIM, HUD_HIT_MENU_PANEL, HUD_HIT_TAB_PEOPLE, HUD_HIT_TAB_SETTINGS, HUD_HIT_TAB_HELP,
    HUD_HIT_BTN0, HUD_HIT_BTN1, HUD_HIT_BTN2,
    HUD_HIT_COUNT
};

/* What a menu button does now (hud_button_meaning). */
enum { HUD_DO_NONE, HUD_DO_RESUME, HUD_ASK_RESET, HUD_ASK_LEAVE, HUD_DO_RESET, HUD_DO_LEAVE, HUD_DO_CANCEL, HUD_DO_SETTINGS };

/* A game started (or stopped: NULL): builds the UI under its CoreGui. */
void hud_begin_game(ub_game *g);
/* Every frame, before the GUI is laid out: screen size (GUI pixels), the
 * chat text being typed (NULL if not typing). */
void hud_frame(int sw, int sh, const char *typing, f32 dt);
int hud_hit(int x, int y);                     /* HUD_HIT_*, -1 none */
const ub_hud_rect *hud_hit_rect(int which);
int hud_button_meaning(int b);
void hud_set_menu(bool open);
bool hud_menu_open(void);
void hud_set_tab(int tab);
void hud_confirm(int what);                    /* 0 none, 1 reset, 2 leave */
int hud_confirming(void);
void hud_toggle_list(void);
void hud_toggle_chat(void);                    /* the chat button */
int hud_below_chat(void);                      /* GUI y under the chat window */

#endif
