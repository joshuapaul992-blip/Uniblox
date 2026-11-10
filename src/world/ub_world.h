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

typedef struct {
    i32 parent;           /* -1 for a top-level instance */
    i32 first_child, last_child, next_sibling;
    u16 cls;              /* ub_class */
    u16 flags;            /* reserved, saved as-is */
    u32 name;             /* offset into the string pool */
    u32 class_name;       /* original class name, e.g. "MeshPart" or "Script" */
    i32 part;             /* index into parts[], or -1 */
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

typedef struct {
    ub_instance *inst;
    i32 count, cap;
    ub_part *parts;
    i32 part_count, part_cap;
    char *strings;        /* NUL-terminated strings, back to back */
    u32 str_len, str_cap;
    ub_lighting lighting;
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

const char *ub_world_str(const ub_world *w, u32 offset);
const char *ub_class_name(ub_class c);
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
 * PART, LITE (lighting), and "END ". Unknown chunks are skipped, so later
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

/* Serialise to a plat_alloc'd buffer (free with plat_free). */
bool ub_ubx_save(const ub_world *w, u8 **data, u32 *size);
/* Parse into `w`, which must be freshly initialised. Untrusted input. */
ub_load_result ub_ubx_load(ub_world *w, const u8 *data, u32 size);

#endif
