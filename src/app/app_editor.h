/* Uniblox Editor - a look inside, for the tests (which drive the editor
 * through its UI with made-up input). */
#ifndef UB_APP_EDITOR_H
#define UB_APP_EDITOR_H

#include "editor/ub_editor.h"
#include "game/ub_game.h"
#include "render/ub_render.h"

ub_editor *app_editor_model(void);
const ub_camera *app_editor_camera(void);
bool app_editor_playing(void);
ub_game *app_editor_game(void);   /* the play test's client game */
ub_game *app_editor_server(void); /* and its server's (NULL when not playing) */
const char *app_editor_status(void);

#endif
