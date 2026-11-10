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
#include "render/ub_render.h"
#include "world/ub_world.h"

#define UB_GAME_MAX_STEPS 12          /* per frame: below 10 fps the game slows down */
#define UB_GAME_RESPAWN_TIME 3.0f     /* seconds from dying to coming back */

/* The frame's input, as scripts see it (UserInputService, Player:GetMouse,
 * workspace.CurrentCamera). The app fills it before ub_game_update. */
typedef struct {
    f32 mouse_x, mouse_y;       /* screen (frame) pixels from the top-left, inset included */
    f32 mouse_dx, mouse_dy;     /* turning the camera */
    bool down[3], pressed[3], released[3];
    int wheel;                  /* notches */
    bool key_down[256], key_pressed[256], key_released[256];
    bool gui_mouse, gui_keys;   /* the GUI took them (gameProcessedEvent) */
    bool focused;
    int screen_w, screen_h;
    ub_camera cam;
    bool have_cam;
} ub_game_input;

/* What a script changed on a part. */
enum {
    UB_CHANGE_LOOK = 1,   /* colour, material, transparency, shape: redraw */
    UB_CHANGE_PHYS = 2,   /* position, size, anchored, solid: physics */
};

/* A joint of the character's rig (Motor6D), in Roblox's axes:
 * part1 = part0 * C0 * Transform * C1:Inverse(). */
typedef struct {
    i32 motor, part0, part1;   /* instances */
    vec3 c0p, c1p, tp;
    mat3 c0r, c1r, tr;
    u8 swing;                  /* walking moves it: 1 left leg, 2 right leg, 3 left arm, 4 right arm */
} ub_joint;

/* A part carried by another (welded at run time, or part of a custom
 * character): each frame it is put at `follow` times the offset. Carried
 * parts are not simulated themselves. */
typedef struct {
    i32 part, follow;          /* instances */
    i32 joint;                 /* the Weld/Motor6D/WeldConstraint that does it, -1 the character's own */
    vec3 p;                    /* offset in follow's space (Uniblox axes) */
    mat3 r;
} ub_carry;

#define UB_RIG_JOINTS 20

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
    i32 player_gui, backpack, player_scripts;  /* the player's PlayerGui, Backpack and PlayerScripts */
    struct ub_gui *gui;         /* the GUI engine (gui/ub_gui.h), laid out from player_gui */
    ub_game_input input;
    f32 script_fov;             /* Camera.FieldOfView set by a script (degrees), 0 = the player's setting */
    f32 health, max_health;
    f32 walk_speed, jump_height;
    bool dead;
    f32 respawn_timer;
    i32 last_ground_part;
    vec3 feet;                  /* where to draw the character this frame */

    /* the character's rig (milestone 8C): R15 or R6 parts, posed each frame */
    bool r15;
    ub_joint joints[UB_RIG_JOINTS];
    int joint_count;
    ub_carry *carry;
    i32 carry_count, carry_cap;
    f32 pose_facing, pose_phase, pose_amount, pose_fallen; /* set by the app (or from the movement) */
    bool pose_from_app;
    u32 spawn_count;            /* times the character has (re)spawned: tools refill */

    /* constraints and movers (game/ub_constraints.c) */
    bool constraints_dirty;
    i32 joint_inst[512];        /* the constraint instance of each physics joint */
    i32 pjoint_count;
    i32 *movers;
    i32 mover_count, mover_cap;

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
/* Chooses where the player appears next (RespawnLocation, team spawns). */
void ub_game_pick_spawn(ub_game *g);

/* --- the rig and carried parts (game/ub_rig.c) --- */
void ub_rig_build(ub_game *g);
/* Puts the rig's parts and carried parts where they are this frame. */
void ub_rig_pose(ub_game *g, f32 dt);
/* A rig joint by its Motor6D instance, or NULL. */
ub_joint *ub_rig_joint(ub_game *g, i32 motor);
/* Is it one of the character's parts (its root, its rig, or carried by them)? */
bool ub_rig_is_character_part(const ub_game *g, i32 inst);
/* `part` follows `follow` from now on, keeping offset (Uniblox axes);
 * `joint` is the instance that joins them (-1 none). Replaces an earlier
 * carry of the same part. */
bool ub_game_carry(ub_game *g, i32 part, i32 follow, vec3 p, const mat3 *r, i32 joint);
/* Stops carries made by `joint` (or of `part` if joint is -1). */
void ub_game_uncarry(ub_game *g, i32 part, i32 joint);
bool ub_game_is_carried(const ub_game *g, i32 part);

/* --- constraints and movers (game/ub_constraints.c) --- */
void ub_constraints_rebuild(ub_game *g);
void ub_constraints_step(ub_game *g);   /* before each physics step */
bool ub_constraint_class(const char *cls); /* changing one rebuilds the joints */
/* HingeConstraint.CurrentAngle (degrees) / PrismaticConstraint.CurrentPosition */
bool ub_constraint_reading(const ub_game *g, i32 inst, f32 *value);
/* Visible ropes, rods and springs: their ends now (Uniblox axes). */
int ub_constraints_lines(const ub_game *g, vec3 *a, vec3 *b, u32 *color, int max);
void ub_game_set_walk_speed(ub_game *g, f32 speed);
void ub_game_set_jump_height(ub_game *g, f32 height);
vec3 ub_game_root_position(const ub_game *g); /* HumanoidRootPart's centre */
void ub_game_set_velocity(ub_game *g, vec3 v);
/* Player.RespawnLocation: come back on top of this part (-1: the place's spawn). */
void ub_game_set_respawn_location(ub_game *g, i32 part_inst);
/* The player's GUI from StarterGui: all of it at the start; after a
 * respawn, what has ResetOnSpawn again. Returns the instances added. */
int ub_game_fill_player_gui(ub_game *g, bool initial, i32 *added, int max);

#endif
