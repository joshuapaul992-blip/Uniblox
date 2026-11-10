/* Uniblox - playing a place on screen: the camera that follows the
 * player, keys and mouse to movement, the character's figure, and drawing
 * a running game's parts. Shared by the game (app_demo.c) and the
 * editor's play test (app_editor.c). */
#ifndef UB_APP_PLAY_H
#define UB_APP_PLAY_H

#include "game/ub_game.h"
#include "render/ub_render.h"

/* The camera (milestone 9B), as Roblox's in 2021: it looks 80 degrees up
 * or down at most; the wheel (or I and O) zooms between the place's limits
 * (0.5 to 128 studs by default), smoothly, and below 2 studs goes into
 * first person; walls between it and the player pull it in; Shift turns
 * shift lock on and off (the mouse turns the camera, the character faces
 * where it looks, the view moves over the right shoulder). */
#define PLAY_PITCH_MIN (-1.39f)
#define PLAY_PITCH_MAX 1.39f
#define PLAY_ZOOM_MIN 0.5f
#define PLAY_ZOOM_MAX 128.0f
#define PLAY_FIRST_PERSON 1.0f            /* closer than this (studs) is first person */

typedef struct {
    f32 yaw, pitch, zoom;              /* camera around the player; zoom: where the wheel put it */
    f32 zoom_now;                      /* the zoom, eased towards `zoom` */
    f32 cam_len;                       /* from the head to the camera, after walls pulled it in */
    f32 shoulder;                      /* 0..1: shift lock's step over the right shoulder */
    f32 fade;                          /* 0..1: how see-through the character is drawn (close up) */
    bool shift_lock;                   /* on (Shift toggles it) */
    bool shift_lock_allowed;           /* the player's setting (Shift lock switch) */
    bool first_person, locked;         /* this frame: first person; the mouse locked to the middle */
    bool have_cam;
    ub_camera cam;                     /* the camera worked out by play_view_update */
    f32 facing;                        /* the character's yaw */
    f32 walk_phase, walk_amount;       /* limb swing */
    f32 death_t;                       /* how far it has fallen over */
    bool moving;
    f32 look_scale;                    /* mouse-look speed (1 = normal) */
    bool invert_y;
    f32 fov_deg;
    ub_part_draw *statics;             /* parts nothing has moved or changed */
    int static_count, static_cap;
} ub_play_view;

void play_view_init(ub_play_view *v);
void play_view_free(ub_play_view *v);
/* A game just started: hand its parts to the renderer. */
void play_view_reset(ub_play_view *v, ub_game *g);
/* Camera controls (right mouse, arrow keys, wheel, I/O, Shift) and
 * WASD/Space. keys: gameplay keys count (not while typing); look: the mouse
 * may turn the camera (no menu open). */
void play_view_input(ub_play_view *v, ub_game *g, const ub_input *in, f32 dt, bool keys, bool look, vec3 *move,
                     bool *jump);
/* After ub_game_update: animation, the static set when it changed, and
 * the camera (eased zoom, walls, shift lock); tells g->cam what it did. */
void play_view_update(ub_play_view *v, ub_game *g, f32 dt);
ub_camera play_view_camera(const ub_play_view *v, const ub_game *g);
/* Between ub_render_begin and ub_render_end_scene. */
void play_view_draw(ub_play_view *v, ub_game *g, bool character);

ub_part_draw play_part_draw(const ub_world *w, const ub_part *p);
/* Roblox-style lighting -> the renderer's sky, sun and fog. */
ub_render_env play_env(const ub_world *w);
void play_draw_character(f32 x, f32 y, f32 z, f32 facing, f32 phase, f32 amount, f32 fallen);

/* The game's GUI (ScreenGuis, BillboardGuis, SurfaceGuis).
 * play_gui_input: before ub_game_update, with the frame's input; `look`
 * says the mouse turns the camera (no hovering then). Afterwards the GUI
 * says what it took: ub_gui_mouse_taken, _wheel_taken, _keyboard_taken.
 * play_gui_draw_world: between ub_render_begin and the end of the scene.
 * play_gui_draw_screen: after the scene, before text. */
void play_gui_input(ub_game *g, const ub_frame *frame, const ub_input *in, const ub_camera *cam, bool look, f64 time);
void play_gui_draw_world(ub_game *g, const ub_camera *cam);
void play_gui_draw_screen(ub_game *g);
/* The GUI's screen: the frame (software: its render resolution). */
void play_gui_screen_size(const ub_frame *frame, int *w, int *h);
void play_draw_blob_shadow(ub_game *g);

#endif
