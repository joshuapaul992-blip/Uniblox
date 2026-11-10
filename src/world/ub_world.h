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
    UB_PART_CARRIED     = 1 << 5,  /* moved by the game (character rig, welded at run time): not simulated */
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
    u16 mesh;             /* 0: its shape; else meshes[mesh - 1], for collision and looks (unions, MeshParts) */
    u16 look;             /* 0: as `mesh`; else meshes[look - 1], for looks only (mesh files, SpecialMesh) */
} ub_part;

/* A part's own geometry (milestone 8C): triangles in the part's unit cube
 * (-0.5..0.5, scaled by its size), in Uniblox's axes, made of convex
 * pieces (vertices and triangles of each piece follow one another) that
 * physics uses as collision hulls. Parts that look the same share one. */
typedef struct {
    f32 *v;               /* 3 floats per vertex */
    u32 *tri;             /* 3 vertex indices per triangle */
    u32 *piece_v, *piece_t; /* per piece: its end vertex and end triangle */
    u32 nv, nt, npieces;
    u32 flags;            /* UB_WMESH_* */
    u32 hash;
    void *render_cache;   /* the renderer's form of it (one allocation, freed with the world) */
    void *phys_cache;     /* physics' collision hulls (one allocation, freed with the world) */
} ub_wmesh;

enum {
    UB_WMESH_WOUND = 1,   /* triangles face the way they are wound (counter-clockwise = outside);
                             otherwise each faces out of its convex piece */
};

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

/* ------------------------------------------------- typed properties
 *
 * Any instance can carry named, typed properties: the GUI's (Size,
 * BackgroundColor3, Text...), value objects' Value and whatever the importer
 * or a script stores. They are saved in .ubx files. Strings and sequences
 * own their memory. Keys are small ids for property names
 * (ub_world_key). */
typedef enum {
    UB_PV_NONE, UB_PV_BOOL, UB_PV_INT, UB_PV_FLOAT, UB_PV_DOUBLE, UB_PV_STRING, UB_PV_ENUM,
    UB_PV_COLOR3, UB_PV_VECTOR2, UB_PV_VECTOR3, UB_PV_UDIM, UB_PV_UDIM2, UB_PV_RECT, UB_PV_NUMRANGE,
    UB_PV_NUMSEQ,         /* keypoints of (time, value, envelope) */
    UB_PV_COLORSEQ,       /* keypoints of (time, r, g, b) */
    UB_PV_REF,            /* an instance, -1 = nil */
    UB_PV_FONT,           /* family (a content id), weight 100..900, style 0 normal / 1 italic */
    UB_PV_BRICKCOLOR,     /* a BrickColor number */
    UB_PV_COUNT
} ub_ptype;

typedef struct {
    u8 type;              /* ub_ptype */
    union {
        bool b;
        i32 i;
        f32 f;
        f64 d;
        u32 e;            /* enum value, BrickColor number */
        i32 ref;
        struct { char *s; u32 len; } str;
        struct { f32 r, g, b; } c3;
        struct { f32 x, y; } v2;
        struct { f32 x, y, z; } v3;
        struct { f32 s; i32 o; } ud;
        struct { f32 sx; i32 ox; f32 sy; i32 oy; } ud2;
        struct { f32 x0, y0, x1, y1; } rect;
        struct { f32 lo, hi; } range;
        struct { f32 *k; u32 n; } seq;            /* n keypoints, 3 (NUMSEQ) or 4 (COLORSEQ) floats each */
        struct { char *family; u16 weight; u8 style; } font;
    } v;
} ub_value;

typedef struct {
    u32 key;
    ub_value val;
} ub_prop;

typedef struct {
    ub_prop *p;
    u16 n, cap;
} ub_proplist;

#define UB_SEQ_MAX_KEYS 20   /* Roblox allows 20 keypoints */

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
    /* typed properties: per instance, and the names of their keys */
    ub_proplist *props;
    i32 props_cap;
    u32 *keys;            /* key id -> name (string pool offset) */
    i32 key_count, key_cap;
    i32 *key_hash;        /* open addressing: key ids, -1 empty */
    i32 key_hash_cap;
    u32 epoch;            /* bumped by every change to the tree, names and properties */
    ub_wmesh *meshes;
    i32 mesh_count, mesh_cap;
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

/* ------------------------------------------------------- properties */

/* The key for a property name (made if new); UB_NO_STRING if out of memory. */
u32 ub_world_key(ub_world *w, const char *name);
/* The key, or UB_NO_STRING if no property has ever had this name. */
u32 ub_world_find_key(const ub_world *w, const char *name);
const char *ub_world_key_name(const ub_world *w, u32 key);
/* The value, or NULL if the instance has none under that key. Valid until
 * the instance's properties change. */
const ub_value *ub_world_prop(const ub_world *w, i32 inst, u32 key);
const ub_value *ub_world_prop_named(const ub_world *w, i32 inst, const char *name);
/* Stores a copy (strings and sequences are copied). False if out of
 * memory or the value is malformed. */
bool ub_world_set_prop(ub_world *w, i32 inst, u32 key, const ub_value *v);
bool ub_world_set_prop_named(ub_world *w, i32 inst, const char *name, const ub_value *v);
void ub_world_remove_prop(ub_world *w, i32 inst, u32 key);
int ub_world_prop_count(const ub_world *w, i32 inst);
const ub_prop *ub_world_prop_at(const ub_world *w, i32 inst, int i);
/* Value helpers (no world needed). */
void ub_value_free(ub_value *v);
bool ub_value_copy(ub_value *dst, const ub_value *src);
bool ub_value_equal(const ub_value *a, const ub_value *b);
static inline ub_value ub_vbool(bool b) { ub_value v; v.type = UB_PV_BOOL; v.v.d = 0; v.v.b = b; return v; }
static inline ub_value ub_vint(i32 i) { ub_value v; v.type = UB_PV_INT; v.v.d = 0; v.v.i = i; return v; }
static inline ub_value ub_vfloat(f32 f) { ub_value v; v.type = UB_PV_FLOAT; v.v.d = 0; v.v.f = f; return v; }
static inline ub_value ub_venum(u32 e) { ub_value v; v.type = UB_PV_ENUM; v.v.d = 0; v.v.e = e; return v; }
/* a string value that points at `s` (not owned: only to pass to set_prop) */
static inline ub_value ub_vstr(const char *s, u32 len)
{
    ub_value v;
    v.type = UB_PV_STRING;
    v.v.str.s = (char *)s;
    v.v.str.len = len;
    return v;
}
/* The instance's properties are dropped (when it is reclaimed). */
void ub_world_clear_props(ub_world *w, i32 inst);
/* Copies every property of `from` onto `to`. */
bool ub_world_copy_props(ub_world *w, i32 to, i32 from);

/* ------------------------------------------------------- .ubx place files
 *
 * Little-endian, chunked:  "UBXPLACE" u32 version, then chunks of
 * [4-byte tag][u32 length][payload]: INFO (name), STRS (string pool),
 * INST (instances in tree pre-order, each parent before its children),
 * PART, LITE (lighting), WSPC (gravity, fallen-parts height), WELD (pairs
 * of instance indices), SRCS (scripts' code), PROP (typed properties) and
 * "END ". Instances made
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

/* A mesh for parts (ub_part.mesh): the id to put in a part (1-based; an
 * identical mesh already there is shared), or 0 if out of memory or
 * invalid. The data is copied. Pieces as in ub_wmesh. */
u16 ub_world_add_mesh(ub_world *w, const f32 *v, u32 nv, const u32 *tri, u32 nt, const u32 *piece_v,
                      const u32 *piece_t, u32 npieces, u32 flags);
/* The renderer's form of mesh `id` (render/ub_mesh.h), made on first use;
 * NULL if none or out of memory. */
const struct ub_mesh_s *ub_world_render_mesh(const ub_world *w, u16 id);
static inline const ub_wmesh *ub_world_mesh(const ub_world *w, u16 id)
{
    return id > 0 && (i32)id <= w->mesh_count ? &w->meshes[id - 1] : (const ub_wmesh *)0;
}

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
