/* Uniblox - playing a place on screen: the camera that follows the
 * player, keys and mouse to movement, the character's figure, and drawing
 * a running game's parts. Shared by the game (app_demo.c) and the
 * editor's play test (app_editor.c). */
#ifndef UB_APP_PLAY_H
#define UB_APP_PLAY_H

#include "game/ub_game.h"
#include "render/ub_render.h"

#define PLAY_PITCH_MIN (-1.35f)
#define PLAY_PITCH_MAX 0.35f
#define PLAY_ZOOM_MIN 6.0f
#define PLAY_ZOOM_MAX 60.0f

typedef struct {
    f32 yaw, pitch, zoom;              /* camera around the player */
    f32 facing;                        /* the character's yaw */
    f32 walk_phase, walk_amount;       /* limb swing */
    f32 death_t;                       /* how far it has fallen over */
    bool moving;
    ub_part_draw *statics;             /* parts nothing has moved or changed */
    int static_count, static_cap;
} ub_play_view;

void play_view_init(ub_play_view *v);
void play_view_free(ub_play_view *v);
/* A game just started: hand its parts to the renderer. */
void play_view_reset(ub_play_view *v, ub_game *g);
/* Camera controls (right mouse, arrow keys, wheel) and WASD/Space.
 * keys: gameplay keys count (not while typing); look: mouse-look allowed. */
void play_view_input(ub_play_view *v, const ub_input *in, f32 dt, bool keys, bool look, vec3 *move, bool *jump);
/* After ub_game_update: animation, and the static set when it changed. */
void play_view_update(ub_play_view *v, ub_game *g, f32 dt);
ub_camera play_view_camera(const ub_play_view *v, const ub_game *g);
/* Between ub_render_begin and ub_render_end_scene. */
void play_view_draw(ub_play_view *v, ub_game *g, bool character);

ub_part_draw play_part_draw(const ub_part *p);
/* Roblox-style lighting -> the renderer's sky, sun and fog. */
ub_render_env play_env(const ub_world *w);
void play_draw_character(f32 x, f32 y, f32 z, f32 facing, f32 phase, f32 amount, f32 fallen);

#endif
