/* Uniblox - internals shared by the scripting files (not for the rest of
 * the game). */
#ifndef UB_SAPI_H
#define UB_SAPI_H

#include "game/ub_game.h"
#include "script/lua/lauxlib.h"
#include "script/lua/lua.h"
#include "script/ub_script.h"

/* ------------------------------------------------------------ the state */

typedef struct {
    lua_State *L;
    ub_game *g;
    size_t mem_used;
    i32 cur_script;          /* the script whose code is running, -1 none */
    u64 resume_start;        /* when the current resume began (time limit) */
    bool timed_out;
    bool out_of_memory;      /* the running script used up the memory (stopped like a timeout) */
    i32 base_live;           /* instances when scripts started (scripts may add UB_SCRIPT_MAX_INSTANCES) */
    bool need_gc;            /* a full collection is owed (after running out of memory) */
    u32 next_conn_id;
    u8 *touch_watch;         /* per instance: Touched connections */
    i32 touch_watch_cap;
    u8 *started;             /* per instance: script already started */
    i32 started_cap;
    ub_script_stats stats;
} ub_sstate;

extern ub_sstate S;

/* registry keys (light userdata addresses) */
extern char REG_INSTANCES, REG_SIGNALS, REG_WAITERS, REG_PROPS, REG_SCRIPTS, REG_THREAD_SCRIPT, REG_MODULES,
    REG_BASE, REG_TWEENS;

/* --------------------------------------------- Roblox <-> Uniblox space
 *
 * Scripts use Roblox's coordinates (right-handed, +Z towards the viewer);
 * the world uses Uniblox's (z flipped). Rotations convert as S R S with
 * S = diag(1, 1, -1). Cylinders and wedges are also turned, as on import
 * (world/ub_rbxl.c), so get(set(x)) == x. */

#define rbx_vec ub_rbx_vec
#define rbx_rot ub_rbx_rot
#define part_get_cframe ub_part_get_rbx
#define part_set_cframe ub_part_set_rbx
#define part_get_size ub_part_get_rbx_size
#define part_set_size ub_part_set_rbx_size

/* ------------------------------------------------------- types (Lua) */

typedef struct { f32 x, y, z; } sv3;
typedef struct { f32 r, g, b; } sc3;
typedef struct { vec3 p; mat3 r; } scf;                     /* Roblox space; columns = right, up, back */

void push_v3(lua_State *L, vec3 v);
vec3 check_v3(lua_State *L, int i);
bool is_v3(lua_State *L, int i);
void push_c3(lua_State *L, f32 r, f32 g, f32 b);
void push_c3_rgb(lua_State *L, u32 rgb);
u32 check_c3_rgb(lua_State *L, int i);
bool is_c3(lua_State *L, int i);
void push_cf(lua_State *L, vec3 p, const mat3 *r);
scf *check_cf(lua_State *L, int i);
bool is_cf(lua_State *L, int i);
void push_brickcolor(lua_State *L, u32 rgb);
bool is_brickcolor(lua_State *L, int i);
u32 brickcolor_rgb(lua_State *L, int i);

/* Enums: Enum.Material, Enum.PartType, Enum.EasingStyle, ... */
enum {
    EN_MATERIAL, EN_PARTTYPE, EN_EASINGSTYLE, EN_EASINGDIRECTION, EN_PLAYBACKSTATE, EN_RAYCASTFILTER,
    EN_HUMANOIDSTATE, EN_KEYCODE, EN_SURFACETYPE, EN_COUNT
};
void push_enum(lua_State *L, int type, int value);
int check_enum(lua_State *L, int i, int type);              /* also accepts the name or number */
const char *enum_item_name(int type, int value);
u8 material_to_ub(int rbx_value);
int material_from_ub(u8 m);

typedef struct {
    f32 time, delay;
    int style, dir;
    int repeat;
    bool reverses;
} stweeninfo;
stweeninfo *check_tweeninfo(lua_State *L, int i);
f32 ease(int style, int dir, f32 t);

const char *ub_typeof(lua_State *L, int i);
void open_types(lua_State *L);

/* ---------------------------------------------------------- instances */

/* Virtual services (not world instances): negative indices. */
enum {
    VIRT_GAME = -2, VIRT_RUNSERVICE = -3, VIRT_DEBRIS = -4, VIRT_TWEENSERVICE = -5, VIRT_HTTP = -6,
    VIRT_USERINPUT = -7, VIRT_MARKETPLACE = -8, VIRT_DATASTORE = -9, VIRT_CHAT = -10, VIRT_TELEPORT = -11,
    VIRT_SOUNDSERVICE = -12, VIRT_PHYSICSSERVICE = -13, VIRT_COLLECTION = -14, VIRT_TEXTSERVICE = -15,
    VIRT_LAST = -15
};
void push_inst(lua_State *L, i32 idx);                      /* nil for -1 */
i32 check_inst(lua_State *L, int i);
i32 opt_inst(lua_State *L, int i);                          /* -1 for nil */
bool is_inst(lua_State *L, int i);
void open_instances(lua_State *L);

/* ------------------------------------------------------------ signals */

enum {
    EV_TOUCHED, EV_TOUCHENDED, EV_CHANGED, EV_CHILDADDED, EV_CHILDREMOVED, EV_DESCENDANTADDED,
    EV_DESCENDANTREMOVING, EV_DESTROYING, EV_ANCESTRYCHANGED, EV_DIED, EV_HEALTHCHANGED, EV_PLAYERADDED,
    EV_PLAYERREMOVING, EV_CHARACTERADDED, EV_CHARACTERREMOVING, EV_CHATTED, EV_HEARTBEAT, EV_STEPPED,
    EV_RENDERSTEPPED, EV_COMPLETED, EV_PROPCHANGED, EV_COUNT
};
/* A signal of an instance (or a tween: inst = -1000 - tween id). For
 * GetPropertyChangedSignal, `prop` names the property (else NULL). */
void push_signal(lua_State *L, i32 inst, int ev, const char *prop);
/* Fires: handlers get the `nargs` values on top of the stack (popped). */
void fire_signal(lua_State *L, i32 inst, int ev, const char *prop, int nargs);
bool signal_has_handlers(lua_State *L, i32 inst, int ev);
void open_signals(lua_State *L);

/* ---------------------------------------------------------- scheduling */

/* Runs fn (at stack top - nargs - 1) with the args on a new thread now,
 * for the current script. */
void spawn_now(lua_State *L, int nargs);
/* Same, after `delay` seconds. */
void spawn_later(lua_State *L, int nargs, f64 delay);
/* Yields the running thread until `delay` seconds pass; wait()'s return
 * values are pushed when it resumes. Use as: return yield_for(L, t); */
int yield_for(lua_State *L, f64 delay);
/* Yields until resumed by resume_ref (signal waiters). */
int yield_parked(lua_State *L, int *ref_out);
void resume_ref(lua_State *L, int ref, int nargs);
f64 script_time(void);
void script_output(const char *text, bool is_error);
void start_scripts_in(i32 inst);                            /* run Scripts that just entered the Workspace */
void stop_script(i32 script_inst, const char *why);
void forget_instances(lua_State *L, const u8 *marked, i32 n);
void open_scheduler(lua_State *L);

/* Tweens and Debris (ub_sapi_inst.c) */
void update_tweens(lua_State *L, f64 now);
void update_debris(lua_State *L, f64 now);
/* Reuses the slots of instances no script can reach any more (destroyed,
 * or never parented and forgotten). At safe points only. */
void reclaim_instances(lua_State *L);
void free_instances(void);

/* Errors as Roblox words them. */
int sapi_error(lua_State *L, const char *fmt, ...);

#endif
