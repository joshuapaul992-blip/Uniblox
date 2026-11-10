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
    bool clients_started;    /* LocalScripts start on the first frame, once the screen is known */
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
/* The type id of an enum by name (Enum.<name>), -1 if unknown. Types from
 * EN_COUNT on are the class table's (world/ub_classes.h). */
int enum_type_named(const char *name);
bool is_enum_of(lua_State *L, int i, int type);
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
/* Region3, Vector3int16, OverlapParams, PhysicalProperties... (ub_sapi_types2.c) */
void open_types2(lua_State *L, int base);
bool get_region3(lua_State *L, int i, vec3 *lo, vec3 *hi);

/* ---------------------------------------------------------- instances */

/* Virtual services (not world instances): negative indices. */
enum {
    VIRT_GAME = -2, VIRT_RUNSERVICE = -3, VIRT_DEBRIS = -4, VIRT_TWEENSERVICE = -5, VIRT_HTTP = -6,
    VIRT_USERINPUT = -7, VIRT_MARKETPLACE = -8, VIRT_DATASTORE = -9, VIRT_CHAT = -10, VIRT_TELEPORT = -11,
    VIRT_SOUNDSERVICE = -12, VIRT_PHYSICSSERVICE = -13, VIRT_COLLECTION = -14, VIRT_TEXTSERVICE = -15,
    VIRT_GUISERVICE = -16, VIRT_MOUSE = -17, VIRT_CAMERA = -18, VIRT_CONTENTPROVIDER = -19,
    VIRT_PROMPTSERVICE = -20, VIRT_POLICY = -21, VIRT_BADGE = -22, VIRT_LOCALIZATION = -23, VIRT_PATHFINDING = -24,
    VIRT_GROUP = -25, VIRT_SOCIAL = -26, VIRT_ANALYTICS = -27,
    VIRT_LAST = -27
};
void push_inst(lua_State *L, i32 idx);                      /* nil for -1 */
i32 check_inst(lua_State *L, int i);
i32 opt_inst(lua_State *L, int i);                          /* -1 for nil */
bool is_inst(lua_State *L, int i);
void open_instances(lua_State *L);
/* Changed and GetPropertyChangedSignal(prop) of an instance. */
void prop_changed(lua_State *L, i32 idx, const char *prop);
/* Sets a property as a script would (instance at idx, value at stack index v). */
int set_prop(lua_State *L, i32 idx, const char *k, int v);

/* ------------------------------------------- GUI, input (ub_sapi_gui.c) */
void open_gui(lua_State *L);
/* World property values <-> Lua. enum_type: enum_type_named(...) or -1. */
void push_pvalue(lua_State *L, const ub_value *v, int enum_type);
/* Converts the Lua value at i to a property of the given type; false (and
 * a message in err) if it is the wrong kind. Sequences use `seq`
 * (UB_SEQ_MAX_KEYS * 4 floats); strings point into Lua. */
bool to_pvalue(lua_State *L, int i, u8 type, int enum_type, ub_value *out, f32 *seq, const char **err);
/* Properties of classes in the class table (GUI, value objects, remotes):
 * pushed and true, or false if not one. */
bool gui_get(lua_State *L, i32 idx, const char *k);
/* Sets one; false if the class table does not have it. Raises on a value
 * of the wrong type or a read-only property. */
bool gui_set(lua_State *L, i32 idx, const char *k, int v);
lua_CFunction gui_method(i32 idx, const char *k);
int gui_event(i32 idx, const char *k);
/* Virtual services and objects: GuiService, Mouse, Camera, UserInputService... */
bool gui_virtual_index(lua_State *L, i32 idx, const char *k);
bool gui_virtual_newindex(lua_State *L, i32 idx, const char *k, int v);
/* Instance.new for a class of the class table; -1 if not one. */
i32 gui_instance_new(const char *cls);
/* Each frame, before scripts run: GUI events, UserInputService, Mouse. */
void gui_pump(lua_State *L);
/* After a respawn: GUIs that reset on spawn are copied again. */
void gui_respawned(lua_State *L);
/* Tweens of UDim2 / Vector2 / UDim values: their floats (4, 2, 2). */
bool is_udim2(lua_State *L, int i);
bool is_vector2(lua_State *L, int i);
bool is_udim(lua_State *L, int i);
void get_udim2(lua_State *L, int i, f32 *out);
void push_udim2(lua_State *L, f32 sx, f32 ox, f32 sy, f32 oy);
void get_vector2(lua_State *L, int i, f32 *out);
void push_vector2(lua_State *L, f32 x, f32 y);
void get_udim(lua_State *L, int i, f32 *out);
void push_udim(lua_State *L, f32 s, f32 o);
u32 brickcolor_number_rgb(u32 number);
void push_ray(lua_State *L, vec3 origin, vec3 dir);  /* Roblox axes */
u32 brickcolor_rgb_number(u32 rgb);

/* ------------------------------------------------------------ signals */

enum {
    EV_TOUCHED, EV_TOUCHENDED, EV_CHANGED, EV_CHILDADDED, EV_CHILDREMOVED, EV_DESCENDANTADDED,
    EV_DESCENDANTREMOVING, EV_DESTROYING, EV_ANCESTRYCHANGED, EV_DIED, EV_HEALTHCHANGED, EV_PLAYERADDED,
    EV_PLAYERREMOVING, EV_CHARACTERADDED, EV_CHARACTERREMOVING, EV_CHATTED, EV_HEARTBEAT, EV_STEPPED,
    EV_RENDERSTEPPED, EV_COMPLETED, EV_PROPCHANGED,
    /* GUI and input (ub_sapi_gui.c) */
    EV_MOUSEENTER, EV_MOUSELEAVE, EV_MOUSEMOVED, EV_WHEELFORWARD, EV_WHEELBACKWARD, EV_INPUTBEGAN, EV_INPUTENDED,
    EV_INPUTCHANGED, EV_ACTIVATED, EV_B1CLICK, EV_B1DOWN, EV_B1UP, EV_B2CLICK, EV_B2DOWN, EV_B2UP, EV_FOCUSED,
    EV_FOCUSLOST, EV_SERVEREVENT, EV_CLIENTEVENT, EV_BINDABLE, EV_TEXTBOXFOCUSED, EV_TEXTBOXRELEASED,
    EV_JUMPREQUEST, EV_MOVE, EV_KEYDOWN, EV_KEYUP, EV_NEVER,
    /* attributes and tags (ub_sapi_attr.c) */
    EV_ATTRCHANGED, EV_TAGADDED, EV_TAGREMOVED,
    /* prompts and the humanoid (ub_sapi_interact.c) */
    EV_TRIGGERED, EV_TRIGGERENDED, EV_PROMPTSHOWN, EV_PROMPTHIDDEN, EV_HOLDBEGAN, EV_HOLDENDED, EV_STATECHANGED,
    EV_RUNNING, EV_JUMPING, EV_FREEFALLING,
    /* tools (ub_sapi_interact.c) */
    EV_EQUIPPED, EV_UNEQUIPPED, EV_DEACTIVATED,
    EV_COUNT
};
typedef char ev_count_fits[EV_COUNT <= 128 ? 1 : -1]; /* signal keys pack the event in 7 bits */
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

/* Luau's library additions (ub_luaulib.c): into the base table at `base`. */
void open_luaulib(lua_State *L, int base);
/* HttpService:JSONEncode / JSONDecode / UrlEncode (self at 1). */
int json_encode(lua_State *L);
int json_decode(lua_State *L);
int url_encode(lua_State *L);

/* Attributes and tags (ub_sapi_attr.c): instance methods (self at 1). */
int attr_get(lua_State *L);
int attr_set(lua_State *L);
int attr_getall(lua_State *L);
int attr_signal(lua_State *L);
int tag_has(lua_State *L);
int tag_add(lua_State *L);
int tag_remove(lua_State *L);
int tag_list_of(lua_State *L);
/* CollectionService's members: pushed and true, or false. */
bool collection_index(lua_State *L, const char *k);
/* A subtree moved in or out of the game: InstanceAdded/Removed signals. */
void tags_moved(lua_State *L, i32 root, bool was_in, bool now_in);
bool sapi_in_game(i32 i);                                    /* in the DataModel (not detached) */
i32 *sapi_subtree(i32 root, i32 *count);                     /* root first; plat_free it */

/* ClickDetector, ProximityPrompt, Humanoid states (ub_sapi_interact.c) */
int interact_event(i32 idx, const char *k);                 /* -1 if not one */
lua_CFunction interact_method(i32 idx, const char *k);      /* NULL if not one */
void interact_pump(lua_State *L, f32 dt);                    /* each frame, after the input */
void interact_reset(void);
int humanoid_state(void);                                    /* HumanoidStateType value */
bool offline_index(lua_State *L, i32 idx, const char *k);  /* Policy/Badge/Localization/Pathfinding... */
i32 gui_mouse_target(vec3 *hit);                             /* the part under the mouse (hit in Roblox axes) */
int gui_vk_of(int keycode);                                  /* a KeyCode value's key, -1 if none */

/* Joints (ub_sapi_joint.c): Weld, Motor6D, WeldConstraint... */
bool is_joint_class(const char *cls);
bool joint_get(lua_State *L, i32 idx, const char *k);
bool joint_set(lua_State *L, i32 idx, const char *k, int v);
void joint_changed(lua_State *L, i32 idx);
/* A subtree was moved: its joints join or let go. */
void joints_moved(lua_State *L, i32 root);

/* Teams (ub_sapi_interact.c): Player.Team / TeamColor / Neutral */
bool teams_get(lua_State *L, i32 idx, const char *k);
bool teams_set(lua_State *L, i32 idx, const char *k, int v);

/* Errors as Roblox words them. */
int sapi_error(lua_State *L, const char *fmt, ...);

#endif
