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
#include "net/ub_repl.h"

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

/* The client UI's state (milestone 9A): what scripts asked for and what
 * the app shows (app/app_hud.c draws it). */
enum { UB_CORE_PLAYERLIST, UB_CORE_HEALTH, UB_CORE_BACKPACK, UB_CORE_CHAT, UB_CORE_COUNT }; /* CoreGuiType values */
#define UB_HUD_CHAT_LINES 64
#define UB_HUD_TEXT 200
typedef struct {
    char who[48];              /* "" for a system message */
    char text[UB_HUD_TEXT];
    u32 color;                 /* the name's (or a system message's) colour, 0x00RRGGBB */
    f32 time;                  /* game time it arrived */
} ub_chat_line;

typedef struct {
    char title[64], text[UB_HUD_TEXT];
    f32 time, duration;
} ub_notice;

typedef struct {
    i32 adornee;               /* the part it floats over */
    char text[UB_HUD_TEXT];
    f32 time;
} ub_bubble;

typedef struct {
    bool core_off[UB_CORE_COUNT];
    bool reset_off;            /* SetCore("ResetButtonCallback", false) */
    ub_chat_line chat[UB_HUD_CHAT_LINES];
    u32 chat_count;            /* total ever: the newest is chat[(chat_count - 1) % LINES] */
    ub_notice notices[8];
    u32 notice_count;
    ub_bubble bubbles[16];
    u32 bubble_count;
    bool reset_requested;      /* the menu's Reset Character (or a script) */
} ub_hud_state;

/* The camera (milestone 9B): the rules the place and its scripts set, and
 * what the app's camera did this frame (scripts and the client UI read it). */
enum { UB_CAMTYPE_FIXED, UB_CAMTYPE_ATTACH, UB_CAMTYPE_WATCH, UB_CAMTYPE_TRACK, UB_CAMTYPE_FOLLOW,
       UB_CAMTYPE_CUSTOM, UB_CAMTYPE_SCRIPTABLE, UB_CAMTYPE_ORBITAL };      /* Enum.CameraType */
enum { UB_MOUSE_DEFAULT, UB_MOUSE_LOCK_CENTER, UB_MOUSE_LOCK_CURRENT };     /* Enum.MouseBehavior */
typedef struct {
    int camera_mode;            /* Player.CameraMode: 0 Classic, 1 LockFirstPerson */
    f32 min_zoom, max_zoom;     /* Player.CameraMinZoomDistance / CameraMaxZoomDistance, studs */
    bool mouse_lock_ok;         /* Player.DevEnableMouseLock (StarterPlayer.EnableMouseLockOption) */
    int occlusion;              /* Player.DevCameraOcclusionMode: 0 Zoom (the camera comes closer), 1 Invisicam */
    int mouse_behavior;         /* UserInputService.MouseBehavior set by a script */
    bool icon_enabled;          /* UserInputService.MouseIconEnabled */
    int camera_type;            /* Camera.CameraType (UB_CAMTYPE_*) */
    i32 subject;                /* Camera.CameraSubject, -1 = the humanoid */
    vec3 script_pos, script_fwd;/* Camera.CFrame as a script set it (our axes; fwd = where it looks) */
    bool script_cf;             /* a script set Camera.CFrame */
    /* the app's camera this frame */
    bool first_person, shift_lock, mouse_locked;
    f32 distance;               /* from the camera to its subject */
} ub_cam_state;

/* --- networked games (milestone 9D) ---
 *
 * A networked game is two games: the server's (the world as it is, its
 * Scripts, the physics of everything but the player's character) and the
 * client's (a copy of what the client may see, its LocalScripts, and the
 * player's character, which the client moves and reports). Changes the
 * client's scripts make stay on the client, except to its character.
 * game/ub_session.h runs both over a connection; a solo game (the editor's,
 * the tests' reference) is both in one. */
enum { UB_ROLE_SOLO, UB_ROLE_SERVER, UB_ROLE_CLIENT };

/* messages between the games (after replication's, see net/ub_repl.h) */
enum {
    UB_MSG_HELLO = UB_REPL_LAST + 1, /* server: who the player is (8 net ids) */
    UB_MSG_STATE,     /* server: health, speeds, dead, spawn count */
    UB_MSG_MOVE,      /* server: a teleport or a launch of the character */
    UB_MSG_REPORT,    /* client (unreliable): where the character is and how it looks */
    UB_MSG_CHAT,      /* client: the player said something */
    UB_MSG_RESET,     /* client: reset the character */
    UB_MSG_SCRIPT,    /* both: the scripts' (remotes, clicks, prompts, tools, sounds) */
};
#define UB_NET_NONE 0xFFFFFFFFu
#define UB_REPORT_HZ 20

/* How a networked game reaches the other side. */
typedef struct {
    void *ctx;
    void (*send)(void *ctx, bool reliable, const u8 *msg, u32 len);
    u32 (*to_net)(void *ctx, i32 inst);    /* UB_NET_NONE: the other side has no such instance */
    i32 (*to_local)(void *ctx, u32 net);   /* -1: none */
} ub_game_link;

/* The client's last report of its character (server). */
typedef struct {
    bool have;
    u16 move_seq;               /* the last of the server's moves it had applied */
    vec3 feet, vel;
    f32 facing, phase, amount, fallen;
    f32 client_time, time;      /* its game time; the server's when it came */
    u32 rejected;               /* reports that moved too far, too fast */
} ub_report;

/* A part the server moves, blended between its last two positions (client). */
typedef struct {
    i32 part;
    vec3 p0, p1;
    f32 q0[4], q1[4];
    f32 t0, t1;                 /* server time of each */
} ub_interp;

typedef struct ub_game {
    ub_world world;
    int role;                   /* UB_ROLE_* */
    const ub_game_link *link;   /* networked: the other side */
    ub_report report;           /* server: the client's character */
    u16 move_seq;               /* server: moves sent; client: the last applied */
    bool moved;                 /* server: the character was teleported (send it) */
    vec3 launch;                /* server: a velocity a script gave it (send it) */
    bool launched;
    u32 state_sig;              /* server: the state last sent */
    f32 report_timer, report_idle;  /* client */
    u8 report_last[48];         /* client: the last report sent */
    bool teleported;            /* client: a LocalScript teleported the character (the next report says so) */
    bool moving_for_server;
    u32 report_len;
    ub_interp *interp;          /* client: parts blending (by part, -1 none) */
    i32 *interp_of;
    i32 interp_count, interp_cap, interp_of_cap;
    f32 clock_off;              /* client: server time minus ours (the least seen) */
    bool clock_ok;
    ub_physics phys;
    bool phys_ok;
    ub_character chr;
    bool scripts;               /* the place's scripts are running */
    struct ub_script_rt *srt;   /* their runtime (script/ub_script.h) */

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
    ub_cam_state cam;
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
    i32 core_gui;               /* the engine's own GUI root (prompts, hotbar, the client UI) */
    ub_hud_state hud;

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
/* The server's game of a networked one: as ub_game_start, but the
 * character follows the client's reports and only Scripts run. */
bool ub_game_start_server(ub_game *g, ub_world *w, bool run_scripts, const ub_game_link *link);
/* The client's: `w` is the copy replication made, ids the HELLO's
 * (player, character, root, humanoid, PlayerGui, Backpack, PlayerScripts,
 * Players: local instances). Only client scripts run. */
#define UB_HELLO_IDS 8
bool ub_game_start_client(ub_game *g, ub_world *w, const i32 ids[UB_HELLO_IDS], bool run_scripts,
                          const ub_game_link *link);
/* A message from the other side (UB_MSG_*). */
void ub_game_message(ub_game *g, const u8 *msg, u32 len);
/* The server's HELLO for its player. */
u32 ub_game_hello(const ub_game *g, u8 *out, u32 cap);
/* Client: a part's position came from the server (replication's hook). */
void ub_game_part_from_server(ub_game *g, i32 inst, int what, u32 tick);
/* Client: replication destroys an instance (and all in it). */
void ub_game_destroy_from_server(ub_game *g, i32 inst);
/* Client: after a batch of replication, what changed (the world's journal). */
void ub_game_after_replication(ub_game *g);
#define UB_REPL_TICK_DT 0.05f   /* the server sends 20 times a second */
/* Selects the game's script runtime (before calling ub_script_* directly). */
bool ub_game_scripts(const ub_game *g);

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
/* Client: the rig the server built (its Motor6Ds, as replicated). */
void ub_rig_adopt(ub_game *g);
/* Keeps a rig joint's C0, C1 in its Motor6D's properties (after a change). */
void ub_rig_joint_saved(ub_game *g, const ub_joint *j);
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

/* --- the client UI's state (game/ub_game.c) --- */
void ub_hud_chat(ub_game *g, const char *who, const char *text, u32 color);
void ub_hud_notify(ub_game *g, const char *title, const char *text, f32 duration);
void ub_hud_bubble(ub_game *g, i32 adornee, const char *text);
/* Resets the character (its health to 0; it comes back), unless scripts turned that off. */
void ub_game_reset_character(ub_game *g);
/* Keeps the camera's zoom limits sane (0.5 <= min <= max) after a change. */
void ub_game_camera_limits(ub_game *g);
/* The player said something: the chat, a bubble over their head, Chatted. */
void ub_game_player_chat(ub_game *g, const char *text);
/* A name's colour in the chat (stable per name). */
u32 ub_hud_name_color(const char *name);

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
