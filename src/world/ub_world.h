/* Uniblox - the world model: a tree of instances, some of which are parts.
 *
 * Every instance has a class, a name, a parent and an ordered list of
 * children. Parts carry their geometry and look. The tree also holds the
 * lighting settings. This one structure is what place files save and load,
 * what the renderer draws and (later) what scripts and the network see.
 *
 * Coordinates are Uniblox's: left-handed, +X right, +Y up, +Z forward,
 * in studs (1 stud = 1 world unit). */
#ifndef UB_WORLD_H
#define UB_WORLD_H

#include "core/ub_vec.h"

typedef enum {
    UB_CLASS_OTHER,       /* anything we keep but do not interpret (scripts...) */
    UB_CLASS_WORKSPACE,
    UB_CLASS_LIGHTING,
    UB_CLASS_FOLDER,
    UB_CLASS_MODEL,
    UB_CLASS_PART,
    UB_CLASS_SPAWN,       /* a part where players appear */
    UB_CLASS_COUNT
} ub_class;

enum {
    UB_PART_ANCHORED    = 1 << 0,
    UB_PART_CAN_COLLIDE = 1 << 1,
    UB_PART_PLACEHOLDER = 1 << 2,  /* stands in for a mesh we cannot load */
    UB_PART_GLOW        = 1 << 3,  /* neon: lit at full brightness */
    UB_PART_REMOVED     = 1 << 4,  /* its instance was destroyed (only while running) */
};

/* Shapes and materials use the renderer's numbering (ub_shape, ub_material). */
typedef struct {
    vec3 pos;
    mat3 rot;
    vec3 size;
    u32 color;            /* 0x00RRGGBB */
    u8 shape;
    u8 material;
    u8 transparency;      /* 0 opaque .. 255 invisible */
    u8 flags;             /* UB_PART_* */
} ub_part;

/* Instance flags. The low byte is saved; the rest only exists while the
 * game runs. */
enum {
    UB_INST_DISABLED  = 1 << 0,   /* a script that does not run */
    UB_INST_CLIENT    = 1 << 1,   /* a script for the player's own machine (LocalScript-like) */
    UB_INST_DESTROYED = 1 << 8,   /* Destroy()ed: gone for good */
    UB_INST_RUNTIME   = 1 << 9,   /* made by the game (the player's character): never saved */
    UB_INST_DETACHED  = 1 << 10,  /* not parented (Parent = nil): not saved */
    UB_INST_FREE      = 1 << 11,  /* an empty slot (reclaimed), reused by the next add */
};
#define UB_INST_SAVED_FLAGS 0x00FF
#define UB_NO_STRING 0xFFFFFFFFu

typedef struct {
    i32 parent;           /* -1 for a top-level instance */
    i32 first_child, last_child, next_sibling;
    u16 cls;              /* ub_class */
    u16 flags;            /* UB_INST_* */
    u32 name;             /* offset into the string pool */
    u32 class_name;       /* original class name, e.g. "MeshPart" or "Script" */
    i32 part;             /* index into parts[], or -1 */
    u32 source;           /* scripts: their code (string pool), or UB_NO_STRING */
} ub_instance;

typedef struct {
    f32 clock_time;       /* hours, 0..24 (14 = 2 pm) */
    f32 latitude;         /* degrees north */
    u32 ambient;          /* 0x00RRGGBB, indoor ambient */
    u32 outdoor_ambient;
    f32 brightness;       /* sun strength, 1 = normal */
    f32 fog_end;          /* studs; 0 = no fog */
    u32 fog_color;
} ub_lighting;

/* Two parts held rigidly together (Roblox Weld, Snap, Motor6D,
 * WeldConstraint...): they keep the offset they have when physics starts.
 * Parts joined by welds move as one rigid body; if any of them is
 * anchored, all of them stay put. */
typedef struct {
    i32 a, b;             /* instance indices of two parts */
} ub_weld;

#define UB_GRAVITY_DEFAULT 196.2f        /* studs/s^2, Roblox's default */
#define UB_FALLEN_HEIGHT_DEFAULT (-500.0f)

typedef struct {
    ub_instance *inst;
    i32 count, cap;
    ub_part *parts;
    i32 part_count, part_cap;
    i32 *part_inst;       /* part index -> its instance */
    i32 part_inst_cap;
    char *strings;        /* NUL-terminated strings, back to back */
    u32 str_len, str_cap;
    u32 *str_cache;       /* recent strings by hash, so repeated names share one copy */
    i32 *free_inst, *free_part;  /* reclaimed slots, reused first */
    i32 free_inst_count, free_inst_cap, free_part_count, free_part_cap;
    ub_lighting lighting;
    ub_weld *welds;
    i32 weld_count, weld_cap;
    f32 gravity;          /* studs/s^2, downwards */
    f32 fallen_height;    /* loose parts that fall below this are removed */
    char name[64];        /* place name, UTF-8 */
    i32 workspace;        /* index of the Workspace instance, -1 if none */
} ub_world;

void ub_world_init(ub_world *w);
void ub_world_free(ub_world *w);
ub_lighting ub_lighting_default(void);

/* Adds an instance as the last child of `parent` (-1 = top level).
 * class_name may be NULL (then the class's own name is used). Returns its
 * index, or -1 if out of memory. */
i32 ub_world_add(ub_world *w, i32 parent, ub_class cls, const char *class_name, const char *name);
/* Adds a part instance; returns its instance index or -1. */
i32 ub_world_add_part(ub_world *w, i32 parent, ub_class cls, const char *class_name, const char *name,
                      const ub_part *part);

/* Joins two part instances; false if either is not a part, they are the
 * same, or out of memory. */
bool ub_world_add_weld(ub_world *w, i32 a, i32 b);

const char *ub_world_str(const ub_world *w, u32 offset);
const char *ub_class_name(ub_class c);
/* ---------------------------------------------------- changing the tree
 * (what scripts do). Destroying only flags an instance, so its index stays
 * valid; the slot is reused only once its owner calls ub_world_reclaim. */

/* Moves `inst` under `parent` (-1 = nowhere, i.e. Parent = nil), as its last
 * child. False if that would make a loop or the instance is destroyed. */
bool ub_world_set_parent(ub_world *w, i32 inst, i32 parent);
/* Destroy(): unparents it and flags it and everything below as destroyed. */
void ub_world_destroy(ub_world *w, i32 inst);
/* Copies an instance and everything below it (parts, scripts, welds
 * inside it); the copy has no parent. Returns its index or -1. */
i32 ub_world_clone(ub_world *w, i32 inst);
/* Frees the slot of a destroyed instance for reuse (and its part's). Only
 * when nothing refers to the index any more. Its children are left
 * without a parent. */
void ub_world_reclaim(ub_world *w, i32 inst);
/* Instances in use (not free slots). */
static inline i32 ub_world_live_count(const ub_world *w) { return w->count - w->free_inst_count; }
bool ub_world_set_name(ub_world *w, i32 inst, const char *name);
/* Scripts' code. */
bool ub_world_set_source(ub_world *w, i32 inst, const char *source);
const char *ub_world_source(const ub_world *w, i32 inst); /* "" if none */
/* The instance a part belongs to. */
i32 ub_world_part_inst(const ub_world *w, i32 part);
/* True if it is alive: not destroyed. */
static inline bool ub_world_alive(const ub_world *w, i32 inst)
{
    return inst >= 0 && inst < w->count && !(w->inst[inst].flags & UB_INST_DESTROYED);
}

/* The first child of `parent` with this name, or -1. */
i32 ub_world_find_child(const ub_world *w, i32 parent, const char *name);
/* The Workspace instance, created if missing. */
i32 ub_world_workspace(ub_world *w);
/* True if `index` is `ancestor` or below it. */
bool ub_world_is_descendant(const ub_world *w, i32 index, i32 ancestor);

/* ------------------------------------------------------- .ubx place files
 *
 * Little-endian, chunked:  "UBXPLACE" u32 version, then chunks of
 * [4-byte tag][u32 length][payload]: INFO (name), STRS (string pool),
 * INST (instances in tree pre-order, each parent before its children),
 * PART, LITE (lighting), WSPC (gravity, fallen-parts height), WELD (pairs
 * of instance indices), SRCS (scripts' code) and "END ". Instances made
 * at run time, destroyed or unparented are not saved. Unknown chunks are
 * skipped, so later
 * versions can add data. Saving is deterministic: save -> load -> save
 * gives identical bytes. */

#define UB_UBX_VERSION 1

typedef enum {
    UB_LOAD_OK,
    UB_LOAD_NOT_FOUND,
    UB_LOAD_BAD_FORMAT,   /* not this kind of file */
    UB_LOAD_CORRUPT,
    UB_LOAD_UNSUPPORTED,  /* newer version or feature we lack */
    UB_LOAD_NO_MEMORY,
} ub_load_result;

const char *ub_load_result_name(ub_load_result r);

/* Replace non-finite or absurd numbers in a part loaded from a file. */
void ub_part_sanitize(ub_part *p);

/* ------------------------------------------------- Roblox's axes
 * Scripts and the editor's numbers use Roblox's coordinates (right-handed,
 * +Z towards the viewer); the world uses Uniblox's (z flipped). Rotations
 * convert as S R S with S = diag(1, 1, -1); cylinders and wedges also turn,
 * as on import (world/ub_rbxl.c), so get(set(x)) == x. */
static inline vec3 ub_rbx_vec(vec3 v) { return v3(v.x, v.y, -v.z); } /* either way */
static inline mat3 ub_rbx_rot(const mat3 *m)                          /* either way */
{
    mat3 r;
    r.x = v3(m->x.x, m->x.y, -m->x.z);
    r.y = v3(m->y.x, m->y.y, -m->y.z);
    r.z = v3(-m->z.x, -m->z.y, m->z.z);
    return r;
}
void ub_part_get_rbx(const ub_part *p, vec3 *pos, mat3 *rot);   /* CFrame */
void ub_part_set_rbx(ub_part *p, vec3 pos, const mat3 *rot);
vec3 ub_part_get_rbx_size(const ub_part *p);
void ub_part_set_rbx_size(ub_part *p, vec3 size);               /* clamped; balls stay round */
vec3 ub_rbx_orientation(const mat3 *rbx_rot);                    /* degrees, Y then X then Z */
mat3 ub_rbx_from_orientation(vec3 degrees);

/* Serialise to a plat_alloc'd buffer (free with plat_free). */
bool ub_ubx_save(const ub_world *w, u8 **data, u32 *size);
/* Where each instance ends up in a saved file (and so in the world loaded
 * from it): remap[i] for i < count, -1 if not saved. */
bool ub_world_save_remap(const ub_world *w, i32 *remap);
/* Parse into `w`, which must be freshly initialised. Untrusted input. */
ub_load_result ub_ubx_load(ub_world *w, const u8 *data, u32 size);

#endif
