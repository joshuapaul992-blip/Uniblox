/* Uniblox - a running place: the world, its physics, the player's
 * character and the place's scripts, stepped together.
 *
 * The app owns one, feeds it input each frame and draws what it holds.
 * Nothing here touches the screen or the OS, so the unit tests run whole
 * games headless.
 *
 * The player has a character in the Workspace, made when the game starts
 * and never saved: a Model named after the player with a
 * HumanoidRootPart (an invisible part that follows the character) and a
 * Humanoid (health, walk speed, jump height). Scripts see it as Roblox
 * scripts expect. */
#ifndef UB_GAME_H
#define UB_GAME_H

#include "physics/ub_character.h"
#include "physics/ub_physics.h"
#include "world/ub_world.h"

#define UB_GAME_MAX_STEPS 12          /* per frame: below 10 fps the game slows down */
#define UB_GAME_RESPAWN_TIME 3.0f     /* seconds from dying to coming back */

/* What a script changed on a part. */
enum {
    UB_CHANGE_LOOK = 1,   /* colour, material, transparency, shape: redraw */
    UB_CHANGE_PHYS = 2,   /* position, size, anchored, solid: physics */
};

typedef struct ub_game {
    ub_world world;
    ub_physics phys;
    bool phys_ok;
    ub_character chr;
    bool scripts;               /* the place's scripts are running */

    f32 accum;                  /* simulated time owed */
    f32 time;                   /* seconds since the game started (scripts' time()) */
    f32 alpha;                  /* blend between the last two steps, for drawing */
    vec3 spawn;                 /* where the player comes back */
    vec3 spawn_default;         /* the place's own spawn point */
    i32 respawn_location;       /* Player.RespawnLocation (a part instance), -1 = none */
    f32 kill_height;

    /* the player and their character (instance indices) */
    i32 players, player, char_model, char_root, humanoid;
    f32 health, max_health;
    f32 walk_speed, jump_height;
    bool dead;
    f32 respawn_timer;
    i32 last_ground_part;
    vec3 feet;                  /* where to draw the character this frame */

    /* parts drawn every frame instead of in the static set: moving ones,
     * and any a script has changed */
    u8 *live_flag;
    i32 live_flag_cap;
    i32 *live;
    i32 live_count, live_cap;
    bool statics_dirty;         /* the static set needs rebuilding */

    /* for the app: sounds and counters (the app clears the flags) */
    u32 jumps;
    bool jumped;
    f32 landed_speed;
    bool respawned;

    u32 phys_us, script_us;     /* CPU time last frame */
    bool rest_logged;
} ub_game;

/* Starts a game in `w`, which the game takes over (*w is left empty).
 * run_scripts: start the place's scripts. */
bool ub_game_start(ub_game *g, ub_world *w, bool run_scripts);
void ub_game_stop(ub_game *g);

/* One frame: scripts, then fixed physics steps for dt, then scripts again.
 * move: direction on the ground (length up to 1); jump: held. */
void ub_game_update(ub_game *g, f32 dt, vec3 move, bool jump);

/* Is this part drawn at all (in the Workspace, alive, not invisible)? */
bool ub_game_part_visible(const ub_game *g, i32 part);
/* Where a live part is this frame (blended if it moves). */
void ub_game_part_pose(const ub_game *g, i32 part, vec3 *pos, mat3 *rot);
bool ub_game_part_live(const ub_game *g, i32 part);

/* Scripts report what they change. */
void ub_game_part_changed(ub_game *g, i32 part, u32 what);
/* An instance was reparented, destroyed or made: re-check its parts. */
void ub_game_subtree_changed(ub_game *g, i32 inst);
void ub_game_set_gravity(ub_game *g, f32 gravity);

/* The character. */
void ub_game_set_health(ub_game *g, f32 health);
void ub_game_teleport(ub_game *g, vec3 feet);
void ub_game_set_walk_speed(ub_game *g, f32 speed);
void ub_game_set_jump_height(ub_game *g, f32 height);
vec3 ub_game_root_position(const ub_game *g); /* HumanoidRootPart's centre */
void ub_game_set_velocity(ub_game *g, vec3 v);
/* Player.RespawnLocation: come back on top of this part (-1: the place's spawn). */
void ub_game_set_respawn_location(ub_game *g, i32 part_inst);

#endif
