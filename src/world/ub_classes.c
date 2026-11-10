/* Uniblox - the class table (see ub_classes.h). */
#include "world/ub_classes.h"
#include "core/ub_string.h"
#include "crt/ub_crt.h"

typedef struct {
    const char *name;
    i16 super;
    i16 first_prop, prop_count;
    i16 first_default, default_count;
} cdb_class;

typedef struct {
    i16 prop;
    f32 n[4];
    const char *s;
} cdb_default;

typedef struct {
    const char *name;
    i16 first, count;
} cdb_enum;

typedef struct {
    const char *name;
    i32 value;
} cdb_item;

#define UB_CDB_INF __builtin_inff()

#include "world/ub_classdb.inc"

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static bool streq(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

int ub_cdb_class(const char *name)
{
    for (int i = 0; i < COUNT(k_cdb_classes); ++i)
        if (streq(k_cdb_classes[i].name, name)) return i;
    return -1;
}

const char *ub_cdb_class_name(int cls) { return cls >= 0 && cls < COUNT(k_cdb_classes) ? k_cdb_classes[cls].name : ""; }

int ub_cdb_superclass(int cls) { return cls >= 0 && cls < COUNT(k_cdb_classes) ? k_cdb_classes[cls].super : -1; }

bool ub_cdb_is_a(const char *cls, const char *base)
{
    for (int c = ub_cdb_class(cls); c >= 0; c = k_cdb_classes[c].super)
        if (streq(k_cdb_classes[c].name, base)) return true;
    return false;
}

const ub_cdb_prop *ub_cdb_find_prop(int cls, const char *name)
{
    for (int c = cls; c >= 0 && c < COUNT(k_cdb_classes); c = k_cdb_classes[c].super) {
        const cdb_class *k = &k_cdb_classes[c];
        for (int i = 0; i < k->prop_count; ++i)
            if (streq(k_cdb_props[k->first_prop + i].name, name)) return &k_cdb_props[k->first_prop + i];
    }
    return NULL;
}

bool ub_cdb_creatable(int cls) { return cls >= 0 && cls < COUNT(k_cdb_classes) && k_cdb_classes[cls].default_count > 0; }

/* A value from a table default (strings and font families point at the
 * table's text; sequences at `seq`). */
static void make_value(const cdb_default *d, ub_value *v, f32 *seq)
{
    const ub_cdb_prop *p = &k_cdb_props[d->prop];
    memset(v, 0, sizeof *v);
    v->type = p->type;
    switch (p->type) {
    case UB_PV_BOOL: v->v.b = d->n[0] != 0; break;
    case UB_PV_INT: v->v.i = (i32)d->n[0]; break;
    case UB_PV_FLOAT: v->v.f = d->n[0]; break;
    case UB_PV_DOUBLE: v->v.d = d->n[0]; break;
    case UB_PV_ENUM: case UB_PV_BRICKCOLOR: v->v.e = (u32)d->n[0]; break;
    case UB_PV_REF: v->v.ref = -1; break;
    case UB_PV_STRING:
        v->v.str.s = (char *)(d->s ? d->s : "");
        v->v.str.len = (u32)ub_strlen(v->v.str.s);
        break;
    case UB_PV_COLOR3: v->v.c3.r = d->n[0]; v->v.c3.g = d->n[1]; v->v.c3.b = d->n[2]; break;
    case UB_PV_VECTOR3: v->v.v3.x = d->n[0]; v->v.v3.y = d->n[1]; v->v.v3.z = d->n[2]; break;
    case UB_PV_VECTOR2: v->v.v2.x = d->n[0]; v->v.v2.y = d->n[1]; break;
    case UB_PV_UDIM: v->v.ud.s = d->n[0]; v->v.ud.o = (i32)d->n[1]; break;
    case UB_PV_UDIM2:
        v->v.ud2.sx = d->n[0]; v->v.ud2.ox = (i32)d->n[1]; v->v.ud2.sy = d->n[2]; v->v.ud2.oy = (i32)d->n[3];
        break;
    case UB_PV_RECT: v->v.rect.x0 = d->n[0]; v->v.rect.y0 = d->n[1]; v->v.rect.x1 = d->n[2]; v->v.rect.y1 = d->n[3]; break;
    case UB_PV_NUMRANGE: v->v.range.lo = d->n[0]; v->v.range.hi = d->n[1]; break;
    case UB_PV_NUMSEQ:
        seq[0] = 0; seq[1] = d->n[0]; seq[2] = 0;
        seq[3] = 1; seq[4] = d->n[0]; seq[5] = 0;
        v->v.seq.k = seq;
        v->v.seq.n = 2;
        break;
    case UB_PV_COLORSEQ:
        seq[0] = 0; seq[1] = d->n[0]; seq[2] = d->n[1]; seq[3] = d->n[2];
        seq[4] = 1; seq[5] = d->n[0]; seq[6] = d->n[1]; seq[7] = d->n[2];
        v->v.seq.k = seq;
        v->v.seq.n = 2;
        break;
    case UB_PV_CFRAME: /* defaults: a position, no rotation */
        for (int k = 0; k < 12; ++k) seq[k] = 0;
        seq[0] = d->n[0]; seq[1] = d->n[1]; seq[2] = d->n[2];
        seq[3] = seq[7] = seq[11] = 1;
        v->v.cf.m = seq;
        break;
    case UB_PV_FONT:
        v->v.font.family = (char *)(d->s ? d->s : "");
        v->v.font.weight = (u16)d->n[0];
        v->v.font.style = (u8)d->n[1];
        break;
    default: v->type = UB_PV_NONE; break;
    }
}

bool ub_cdb_fill_defaults(ub_world *w, i32 inst, const char *cls)
{
    int c = ub_cdb_class(cls);
    if (c < 0) return true;
    const cdb_class *k = &k_cdb_classes[c];
    for (int i = 0; i < k->default_count; ++i) {
        const cdb_default *d = &k_cdb_defaults[k->first_default + i];
        u32 key = ub_world_key(w, k_cdb_props[d->prop].name);
        if (key == UB_NO_STRING) return false;
        if (ub_world_prop(w, inst, key)) continue;
        f32 seq[12];
        ub_value v;
        make_value(d, &v, seq);
        if (v.type != UB_PV_NONE && !ub_world_set_prop(w, inst, key, &v)) return false;
    }
    return true;
}

bool ub_cdb_default(int cls, const char *prop, ub_value *out)
{
    static f32 seq[12]; /* sequences, CFrames: valid until the next call */
    if (cls < 0 || cls >= COUNT(k_cdb_classes)) return false;
    const cdb_class *k = &k_cdb_classes[cls];
    for (int i = 0; i < k->default_count; ++i) {
        const cdb_default *d = &k_cdb_defaults[k->first_default + i];
        if (streq(k_cdb_props[d->prop].name, prop)) {
            make_value(d, out, seq);
            return out->type != UB_PV_NONE;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- enums */

int ub_enum_count(void) { return COUNT(k_cdb_enums); }
const char *ub_enum_name(int e) { return e >= 0 && e < COUNT(k_cdb_enums) ? k_cdb_enums[e].name : ""; }

int ub_enum_find(const char *name)
{
    for (int i = 0; i < COUNT(k_cdb_enums); ++i)
        if (streq(k_cdb_enums[i].name, name)) return i;
    return -1;
}

int ub_enum_item_count(int e) { return e >= 0 && e < COUNT(k_cdb_enums) ? k_cdb_enums[e].count : 0; }

const char *ub_enum_item_name(int e, int i)
{
    return i >= 0 && i < ub_enum_item_count(e) ? k_cdb_items[k_cdb_enums[e].first + i].name : "";
}

int ub_enum_item_value(int e, int i)
{
    return i >= 0 && i < ub_enum_item_count(e) ? k_cdb_items[k_cdb_enums[e].first + i].value : -1;
}

int ub_enum_value_of(int e, const char *item)
{
    for (int i = 0; i < ub_enum_item_count(e); ++i)
        if (streq(k_cdb_items[k_cdb_enums[e].first + i].name, item)) return k_cdb_items[k_cdb_enums[e].first + i].value;
    return -1;
}

const char *ub_enum_name_of_value(int e, int value)
{
    for (int i = 0; i < ub_enum_item_count(e); ++i)
        if (k_cdb_items[k_cdb_enums[e].first + i].value == value) return k_cdb_items[k_cdb_enums[e].first + i].name;
    return NULL;
}
