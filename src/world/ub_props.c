/* Uniblox - typed properties of instances (see ub_world.h).
 *
 * Each instance has a small array of (key, value) pairs; lookups scan it
 * (instances have a few dozen properties at most). Key ids name property
 * names and are shared by the whole world. */
#include "world/ub_world.h"
#include "core/ub_string.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

static u32 hash_str(const char *s)
{
    u32 h = 2166136261u;
    for (; *s; ++s) h = (h ^ (u8)*s) * 16777619u;
    return h;
}

static bool streq(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* --------------------------------------------------------------- keys */

/* Key names live in the world's string pool (ub_world.c). */
u32 ub_world_add_string_internal(ub_world *w, const char *s);
u32 ub_world_key_at(ub_world *w, u32 off);

static bool rehash(ub_world *w, i32 cap)
{
    i32 *nh = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap);
    if (!nh) return false;
    for (i32 i = 0; i < cap; ++i) nh[i] = -1;
    for (i32 k = 0; k < w->key_count; ++k) {
        u32 h = hash_str(ub_world_str(w, w->keys[k])) & (u32)(cap - 1);
        while (nh[h] >= 0) h = (h + 1) & (u32)(cap - 1);
        nh[h] = k;
    }
    plat_free(w->key_hash);
    w->key_hash = nh;
    w->key_hash_cap = cap;
    return true;
}

u32 ub_world_find_key(const ub_world *w, const char *name)
{
    if (!w->key_hash_cap) return UB_NO_STRING;
    u32 h = hash_str(name) & (u32)(w->key_hash_cap - 1);
    for (i32 probes = 0; probes < w->key_hash_cap; ++probes) {
        i32 k = w->key_hash[h];
        if (k < 0) return UB_NO_STRING;
        if (streq(ub_world_str(w, w->keys[k]), name)) return (u32)k;
        h = (h + 1) & (u32)(w->key_hash_cap - 1);
    }
    return UB_NO_STRING;
}

u32 ub_world_key(ub_world *w, const char *name)
{
    u32 k = ub_world_find_key(w, name);
    if (k != UB_NO_STRING) return k;
    u32 off = ub_world_add_string_internal(w, name);
    return off == UB_NO_STRING ? UB_NO_STRING : ub_world_key_at(w, off);
}

/* A key whose name is already in the string pool at `off` (loading). */
u32 ub_world_key_at(ub_world *w, u32 off)
{
    if (off >= w->str_len || (off > 0 && w->strings[off - 1] != 0)) return UB_NO_STRING;
    const char *name = w->strings + off;
    u32 k = ub_world_find_key(w, name);
    if (k != UB_NO_STRING) return k;
    if ((w->key_count + 1) * 2 > w->key_hash_cap && !rehash(w, w->key_hash_cap ? w->key_hash_cap * 2 : 256))
        return UB_NO_STRING;
    if (w->key_count == w->key_cap) {
        i32 nc = w->key_cap ? w->key_cap * 2 : 128;
        u32 *nk = (u32 *)plat_alloc(sizeof(u32) * (size_t)nc);
        if (!nk) return UB_NO_STRING;
        if (w->keys) {
            memcpy(nk, w->keys, sizeof(u32) * (size_t)w->key_count);
            plat_free(w->keys);
        }
        w->keys = nk;
        w->key_cap = nc;
    }
    i32 id = w->key_count++;
    w->keys[id] = off;
    u32 h = hash_str(name) & (u32)(w->key_hash_cap - 1);
    while (w->key_hash[h] >= 0) h = (h + 1) & (u32)(w->key_hash_cap - 1);
    w->key_hash[h] = id;
    return (u32)id;
}

const char *ub_world_key_name(const ub_world *w, u32 key)
{
    return key < (u32)w->key_count ? ub_world_str(w, w->keys[key]) : "";
}

/* -------------------------------------------------------------- values */

void ub_value_free(ub_value *v)
{
    if (v->type == UB_PV_STRING) plat_free(v->v.str.s);
    else if (v->type == UB_PV_NUMSEQ || v->type == UB_PV_COLORSEQ) plat_free(v->v.seq.k);
    else if (v->type == UB_PV_FONT) plat_free(v->v.font.family);
    else if (v->type == UB_PV_CFRAME) plat_free(v->v.cf.m);
    v->type = UB_PV_NONE;
}

static int seq_stride(u8 type) { return type == UB_PV_NUMSEQ ? 3 : 4; }

bool ub_value_copy(ub_value *dst, const ub_value *src)
{
    *dst = *src;
    if (src->type >= UB_PV_COUNT) {
        dst->type = UB_PV_NONE;
        return false;
    }
    if (src->type == UB_PV_STRING) {
        u32 n = src->v.str.len;
        if (n > (16u << 20)) n = 16u << 20; /* as Roblox: text props are bounded */
        dst->v.str.s = (char *)plat_alloc(n + 1);
        if (!dst->v.str.s) {
            dst->type = UB_PV_NONE;
            return false;
        }
        if (n) memcpy(dst->v.str.s, src->v.str.s, n);
        dst->v.str.s[n] = 0;
        dst->v.str.len = n;
    } else if (src->type == UB_PV_NUMSEQ || src->type == UB_PV_COLORSEQ) {
        u32 n = src->v.seq.n;
        if (n < 2 || n > UB_SEQ_MAX_KEYS) {
            dst->type = UB_PV_NONE;
            return false;
        }
        size_t bytes = sizeof(f32) * (size_t)n * (size_t)seq_stride(src->type);
        dst->v.seq.k = (f32 *)plat_alloc(bytes);
        if (!dst->v.seq.k) {
            dst->type = UB_PV_NONE;
            return false;
        }
        memcpy(dst->v.seq.k, src->v.seq.k, bytes);
    } else if (src->type == UB_PV_CFRAME) {
        dst->v.cf.m = (f32 *)plat_alloc(sizeof(f32) * 12);
        if (!dst->v.cf.m) {
            dst->type = UB_PV_NONE;
            return false;
        }
        for (int k = 0; k < 12; ++k) { /* never NaN in the world */
            f32 x = src->v.cf.m ? src->v.cf.m[k] : (k == 3 || k == 7 || k == 11 ? 1.0f : 0.0f);
            dst->v.cf.m[k] = x == x ? x : 0;
        }
    } else if (src->type == UB_PV_FONT) {
        const char *f = src->v.font.family ? src->v.font.family : "";
        u32 n = (u32)ub_strlen(f);
        if (n > 1024) n = 1024;
        dst->v.font.family = (char *)plat_alloc(n + 1);
        if (!dst->v.font.family) {
            dst->type = UB_PV_NONE;
            return false;
        }
        memcpy(dst->v.font.family, f, n);
        dst->v.font.family[n] = 0;
    }
    return true;
}

bool ub_value_equal(const ub_value *a, const ub_value *b)
{
    if (a->type != b->type) return false;
    switch (a->type) {
    case UB_PV_NONE: return true;
    case UB_PV_BOOL: return a->v.b == b->v.b;
    case UB_PV_INT: case UB_PV_REF: return a->v.i == b->v.i;
    case UB_PV_FLOAT: return a->v.f == b->v.f;
    case UB_PV_DOUBLE: return a->v.d == b->v.d;
    case UB_PV_ENUM: case UB_PV_BRICKCOLOR: return a->v.e == b->v.e;
    case UB_PV_STRING:
        return a->v.str.len == b->v.str.len && memcmp(a->v.str.s, b->v.str.s, a->v.str.len) == 0;
    case UB_PV_COLOR3: return a->v.c3.r == b->v.c3.r && a->v.c3.g == b->v.c3.g && a->v.c3.b == b->v.c3.b;
    case UB_PV_VECTOR2: return a->v.v2.x == b->v.v2.x && a->v.v2.y == b->v.v2.y;
    case UB_PV_VECTOR3: return a->v.v3.x == b->v.v3.x && a->v.v3.y == b->v.v3.y && a->v.v3.z == b->v.v3.z;
    case UB_PV_UDIM: return a->v.ud.s == b->v.ud.s && a->v.ud.o == b->v.ud.o;
    case UB_PV_UDIM2:
        return a->v.ud2.sx == b->v.ud2.sx && a->v.ud2.ox == b->v.ud2.ox && a->v.ud2.sy == b->v.ud2.sy &&
               a->v.ud2.oy == b->v.ud2.oy;
    case UB_PV_RECT:
        return a->v.rect.x0 == b->v.rect.x0 && a->v.rect.y0 == b->v.rect.y0 && a->v.rect.x1 == b->v.rect.x1 &&
               a->v.rect.y1 == b->v.rect.y1;
    case UB_PV_NUMRANGE: return a->v.range.lo == b->v.range.lo && a->v.range.hi == b->v.range.hi;
    case UB_PV_NUMSEQ: case UB_PV_COLORSEQ:
        return a->v.seq.n == b->v.seq.n &&
               memcmp(a->v.seq.k, b->v.seq.k, sizeof(f32) * a->v.seq.n * (u32)seq_stride(a->type)) == 0;
    case UB_PV_CFRAME: return memcmp(a->v.cf.m, b->v.cf.m, sizeof(f32) * 12) == 0;
    case UB_PV_FONT:
        return a->v.font.weight == b->v.font.weight && a->v.font.style == b->v.font.style &&
               streq(a->v.font.family ? a->v.font.family : "", b->v.font.family ? b->v.font.family : "");
    default: return false;
    }
}

/* ------------------------------------------------------- per instance */

static ub_proplist *list_of(const ub_world *w, i32 inst)
{
    if (inst < 0 || inst >= w->props_cap || !w->props) return NULL;
    return &w->props[inst];
}

static ub_proplist *list_make(ub_world *w, i32 inst)
{
    if (inst < 0 || inst >= w->count) return NULL;
    if (inst >= w->props_cap) {
        i32 nc = w->props_cap ? w->props_cap : 64;
        while (nc <= inst) nc *= 2;
        ub_proplist *np = (ub_proplist *)plat_alloc(sizeof(ub_proplist) * (size_t)nc);
        if (!np) return NULL;
        memset(np, 0, sizeof(ub_proplist) * (size_t)nc);
        if (w->props) {
            memcpy(np, w->props, sizeof(ub_proplist) * (size_t)w->props_cap);
            plat_free(w->props);
        }
        w->props = np;
        w->props_cap = nc;
    }
    return &w->props[inst];
}

const ub_value *ub_world_prop(const ub_world *w, i32 inst, u32 key)
{
    const ub_proplist *l = list_of(w, inst);
    if (!l) return NULL;
    for (u16 i = 0; i < l->n; ++i)
        if (l->p[i].key == key) return &l->p[i].val;
    return NULL;
}

const ub_value *ub_world_prop_named(const ub_world *w, i32 inst, const char *name)
{
    u32 k = ub_world_find_key(w, name);
    return k == UB_NO_STRING ? NULL : ub_world_prop(w, inst, k);
}

bool ub_world_set_prop(ub_world *w, i32 inst, u32 key, const ub_value *v)
{
    if (key >= (u32)w->key_count || v->type == UB_PV_NONE || v->type >= UB_PV_COUNT) return false;
    ub_proplist *l = list_make(w, inst);
    if (!l) return false;
    ub_value copy;
    if (!ub_value_copy(&copy, v)) return false;
    w->epoch++;
    for (u16 i = 0; i < l->n; ++i)
        if (l->p[i].key == key) {
            ub_value_free(&l->p[i].val);
            l->p[i].val = copy;
            return true;
        }
    if (l->n == l->cap) {
        if (l->cap >= 1024) {
            ub_value_free(&copy);
            return false; /* plenty for any real class */
        }
        u16 nc = (u16)(l->cap ? l->cap * 2 : 8);
        ub_prop *np = (ub_prop *)plat_alloc(sizeof(ub_prop) * nc);
        if (!np) {
            ub_value_free(&copy);
            return false;
        }
        if (l->p) {
            memcpy(np, l->p, sizeof(ub_prop) * l->n);
            plat_free(l->p);
        }
        l->p = np;
        l->cap = nc;
    }
    l->p[l->n].key = key;
    l->p[l->n].val = copy;
    l->n++;
    return true;
}

bool ub_world_set_prop_named(ub_world *w, i32 inst, const char *name, const ub_value *v)
{
    u32 k = ub_world_key(w, name);
    return k != UB_NO_STRING && ub_world_set_prop(w, inst, k, v);
}

void ub_world_remove_prop(ub_world *w, i32 inst, u32 key)
{
    ub_proplist *l = list_of(w, inst);
    if (!l) return;
    for (u16 i = 0; i < l->n; ++i)
        if (l->p[i].key == key) {
            ub_value_free(&l->p[i].val);
            memmove(&l->p[i], &l->p[i + 1], sizeof(ub_prop) * (size_t)(l->n - i - 1));
            l->n--;
            w->epoch++;
            return;
        }
}

int ub_world_prop_count(const ub_world *w, i32 inst)
{
    const ub_proplist *l = list_of(w, inst);
    return l ? l->n : 0;
}

const ub_prop *ub_world_prop_at(const ub_world *w, i32 inst, int i)
{
    const ub_proplist *l = list_of(w, inst);
    return l && i >= 0 && i < l->n ? &l->p[i] : NULL;
}

void ub_world_clear_props(ub_world *w, i32 inst)
{
    ub_proplist *l = list_of(w, inst);
    if (!l) return;
    for (u16 i = 0; i < l->n; ++i) ub_value_free(&l->p[i].val);
    plat_free(l->p);
    l->p = NULL;
    l->n = l->cap = 0;
    w->epoch++;
}

bool ub_world_copy_props(ub_world *w, i32 to, i32 from)
{
    int n = ub_world_prop_count(w, from);
    for (int i = 0; i < n; ++i) {
        ub_prop p = *ub_world_prop_at(w, from, i); /* the struct, as set_prop may move the lists */
        if (!ub_world_set_prop(w, to, p.key, &p.val)) return false;
    }
    return true;
}

void ub_world_free_props(ub_world *w)
{
    for (i32 i = 0; i < w->props_cap; ++i) {
        ub_proplist *l = &w->props[i];
        for (u16 k = 0; k < l->n; ++k) ub_value_free(&l->p[k].val);
        plat_free(l->p);
    }
    plat_free(w->props);
    plat_free(w->keys);
    plat_free(w->key_hash);
}
