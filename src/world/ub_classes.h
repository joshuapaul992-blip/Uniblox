/* Uniblox - what Uniblox knows about Roblox's classes: the class tree,
 * the properties each class has (name, type, enum) and the values a new
 * instance starts with, plus Roblox's enums. Generated from rbx-dom's
 * reflection database (tools/gen_classdb.py); see ub_classdb.inc.
 *
 * Covers the GUI classes, value objects, remotes and bindables. Parts,
 * scripts, models and services are handled by the world and the script
 * API directly. */
#ifndef UB_CLASSES_H
#define UB_CLASSES_H

#include "world/ub_world.h"

enum {
    UB_CDB_STORED = 1,      /* kept as an instance property (and saved) */
    UB_CDB_READONLY = 2,    /* worked out by the engine (AbsoluteSize...) */
    UB_CDB_DEPRECATED = 4,
};

typedef struct {
    const char *name;
    u8 type;                /* ub_ptype */
    i16 enum_id;            /* ub_enum_* index, -1 if not an enum */
    u8 flags;               /* UB_CDB_* */
} ub_cdb_prop;

/* A class index (-1 if Uniblox's table does not have the class). */
int ub_cdb_class(const char *name);
const char *ub_cdb_class_name(int cls);
int ub_cdb_superclass(int cls);
/* `cls` is `base` or derives from it (by name; false for unknown classes). */
bool ub_cdb_is_a(const char *cls, const char *base);
/* The property of this class (declared on it or a superclass), or NULL. */
const ub_cdb_prop *ub_cdb_find_prop(int cls, const char *name);
/* Can Instance.new make it (it has defaults)? */
bool ub_cdb_creatable(int cls);
/* Sets every stored property the instance lacks to the class's default.
 * For classes not in the table it does nothing. */
bool ub_cdb_fill_defaults(ub_world *w, i32 inst, const char *cls);
/* The default of one property of a class (false if it has none). Strings
 * point at static text. */
bool ub_cdb_default(int cls, const char *prop, ub_value *out);

/* ---------------------------------------------------------------- enums */
int ub_enum_count(void);
const char *ub_enum_name(int e);
int ub_enum_find(const char *name);              /* -1 if unknown */
int ub_enum_item_count(int e);
const char *ub_enum_item_name(int e, int i);     /* i-th item, by value */
int ub_enum_item_value(int e, int i);
/* by name / by value; -1 if not found */
int ub_enum_value_of(int e, const char *item);
const char *ub_enum_name_of_value(int e, int value); /* NULL if none */

#endif
