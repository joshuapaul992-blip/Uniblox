#include "world/ub_world.h"
#include "core/ub_math.h"
#include "core/ub_string.h"
#include "core/ub_vec.h"
#include "physics/ub_pmath.h"
#include "render/ub_mesh.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

static const char *const k_class_names[UB_CLASS_COUNT] = {
    "Instance", "Workspace", "Lighting", "Folder", "Model", "Part", "SpawnLocation"
};

const char *ub_class_name(ub_class c)
{
    return (unsigned)c < UB_CLASS_COUNT ? k_class_names[c] : "Instance";
}

ub_lighting ub_lighting_default(void)
{
    ub_lighting l;
    l.clock_time = 14.0f;
    l.latitude = 41.7f;
    l.ambient = 0x464646;
    l.outdoor_ambient = 0x808080;
    l.brightness = 2.0f;
    l.fog_end = 0.0f;
    l.fog_color = 0xC0C0C0;
    return l;
}

void ub_world_init(ub_world *w)
{
    memset(w, 0, sizeof *w);
    w->workspace = -1;
    w->lighting = ub_lighting_default();
    w->gravity = UB_GRAVITY_DEFAULT;
    w->fallen_height = UB_FALLEN_HEIGHT_DEFAULT;
    ub_fmt(w->name, sizeof w->name, "Untitled");
}

void ub_world_free_props(ub_world *w); /* ub_props.c */

static void free_meshes(ub_world *w)
{
    for (i32 i = 0; i < w->mesh_count; ++i) {
        plat_free(w->meshes[i].v); /* one block: v, tri, pieces */
        plat_free(w->meshes[i].render_cache);
        plat_free(w->meshes[i].phys_cache);
    }
    plat_free(w->meshes);
    w->meshes = NULL;
    w->mesh_count = w->mesh_cap = 0;
}

void ub_world_free(ub_world *w)
{
    free_meshes(w);
    ub_world_free_props(w);
    plat_free(w->inst);
    plat_free(w->parts);
    plat_free(w->strings);
    plat_free(w->welds);
    plat_free(w->part_inst);
    plat_free(w->str_cache);
    plat_free(w->free_inst);
    plat_free(w->free_part);
    ub_world_init(w);
}

/* Grow an array to hold at least `need` elements. */
static bool grow(void **p, i32 *cap, i32 need, size_t elem)
{
    if (need <= *cap) return true;
    i32 nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    void *np = plat_alloc((size_t)nc * elem);
    if (!np) return false;
    if (*p) {
        memcpy(np, *p, (size_t)*cap * elem);
        plat_free(*p);
    }
    *p = np;
    *cap = nc;
    return true;
}

#define STR_CACHE 4096 /* entries; strings are never changed, so equal ones can share */

static u32 add_string(ub_world *w, const char *s)
{
    u32 n = (u32)ub_strlen(s) + 1;
    u32 h = 2166136261u;
    for (u32 i = 0; i + 1 < n; ++i) h = (h ^ (u8)s[i]) * 16777619u;
    h &= STR_CACHE - 1;
    if (!w->str_cache) {
        w->str_cache = (u32 *)plat_alloc(sizeof(u32) * STR_CACHE);
        if (w->str_cache)
            for (int i = 0; i < STR_CACHE; ++i) w->str_cache[i] = 0xFFFFFFFFu;
    }
    if (w->str_cache && w->str_cache[h] != 0xFFFFFFFFu) {
        const char *c = w->strings + w->str_cache[h];
        u32 i = 0;
        while (i < n && c[i] == s[i]) ++i;
        if (i == n) return w->str_cache[h];
    }
    if (w->str_len + n > w->str_cap) {
        u32 nc = w->str_cap ? w->str_cap * 2 : 1024;
        while (nc < w->str_len + n) nc *= 2;
        char *np = (char *)plat_alloc(nc);
        if (!np) return 0xFFFFFFFFu;
        if (w->strings) {
            memcpy(np, w->strings, w->str_len);
            plat_free(w->strings);
        }
        w->strings = np;
        w->str_cap = nc;
    }
    u32 off = w->str_len;
    memcpy(w->strings + off, s, n);
    w->str_len += n;
    if (w->str_cache) w->str_cache[h] = off;
    return off;
}

u32 ub_world_add_string_internal(ub_world *w, const char *s) { return add_string(w, s); }

const char *ub_world_str(const ub_world *w, u32 offset)
{
    return offset < w->str_len ? w->strings + offset : "";
}

static void link_child(ub_world *w, i32 parent, i32 child)
{
    ub_instance *c = &w->inst[child];
    c->parent = parent;
    c->next_sibling = -1;
    if (parent < 0) return;
    ub_instance *p = &w->inst[parent];
    if (p->last_child >= 0) w->inst[p->last_child].next_sibling = child;
    else p->first_child = child;
    p->last_child = child;
}

i32 ub_world_add(ub_world *w, i32 parent, ub_class cls, const char *class_name, const char *name)
{
    if (parent >= w->count) return -1;
    if (!grow((void **)&w->inst, &w->cap, w->count + 1, sizeof(ub_instance))) return -1;
    u32 n = add_string(w, name ? name : "");
    u32 cn = add_string(w, class_name ? class_name : ub_class_name(cls));
    if (n == 0xFFFFFFFFu || cn == 0xFFFFFFFFu) return -1;
    i32 idx = w->free_inst_count > 0 ? w->free_inst[--w->free_inst_count] : w->count++;
    ub_instance *in = &w->inst[idx];
    memset(in, 0, sizeof *in);
    in->first_child = in->last_child = in->next_sibling = -1;
    in->cls = (u16)cls;
    in->name = n;
    in->class_name = cn;
    in->part = -1;
    in->source = UB_NO_STRING;
    if (idx < w->props_cap) ub_world_clear_props(w, idx); /* a reused slot starts empty */
    link_child(w, parent, idx);
    w->epoch++;
    if (cls == UB_CLASS_WORKSPACE && w->workspace < 0) w->workspace = idx;
    return idx;
}

i32 ub_world_add_part(ub_world *w, i32 parent, ub_class cls, const char *class_name, const char *name,
                      const ub_part *part)
{
    if (!grow((void **)&w->parts, &w->part_cap, w->part_count + 1, sizeof(ub_part))) return -1;
    if (!grow((void **)&w->part_inst, &w->part_inst_cap, w->part_count + 1, sizeof(i32))) return -1;
    i32 idx = ub_world_add(w, parent, cls, class_name, name);
    if (idx < 0) return -1;
    i32 pi = w->free_part_count > 0 ? w->free_part[--w->free_part_count] : w->part_count++;
    w->parts[pi] = *part;
    if ((i32)part->mesh > w->mesh_count) w->parts[pi].mesh = 0;
    if ((i32)part->look > w->mesh_count) w->parts[pi].look = 0;
    w->part_inst[pi] = idx;
    w->inst[idx].part = pi;
    return idx;
}

bool ub_world_add_weld(ub_world *w, i32 a, i32 b)
{
    if (a < 0 || b < 0 || a >= w->count || b >= w->count || a == b) return false;
    if (w->inst[a].part < 0 || w->inst[b].part < 0) return false;
    if (!grow((void **)&w->welds, &w->weld_cap, w->weld_count + 1, sizeof(ub_weld))) return false;
    w->welds[w->weld_count].a = a;
    w->welds[w->weld_count].b = b;
    w->weld_count++;
    return true;
}

/* ------------------------------------------------------ tree changes */

static void unlink_child(ub_world *w, i32 child)
{
    ub_instance *c = &w->inst[child];
    if (c->parent < 0) return;
    ub_instance *p = &w->inst[c->parent];
    i32 prev = -1;
    for (i32 k = p->first_child; k >= 0 && k != child; k = w->inst[k].next_sibling) prev = k;
    if (prev < 0) p->first_child = c->next_sibling;
    else w->inst[prev].next_sibling = c->next_sibling;
    if (p->last_child == child) p->last_child = prev;
    c->parent = -1;
    c->next_sibling = -1;
}

bool ub_world_set_parent(ub_world *w, i32 inst, i32 parent)
{
    if (!ub_world_alive(w, inst) || parent >= w->count) return false;
    if (parent >= 0 && (!ub_world_alive(w, parent) || ub_world_is_descendant(w, parent, inst))) return false;
    if (w->inst[inst].parent == parent) return true;
    unlink_child(w, inst);
    link_child(w, parent, inst);
    w->epoch++;
    if (parent >= 0) w->inst[inst].flags &= (u16)~UB_INST_DETACHED;
    else w->inst[inst].flags |= UB_INST_DETACHED;
    return true;
}

void ub_world_destroy(ub_world *w, i32 inst)
{
    if (!ub_world_alive(w, inst)) return;
    ub_world_set_parent(w, inst, -1);
    w->epoch++;
    /* flag the whole subtree (children stay linked, so walk it) */
    i32 stack_small[64];
    i32 *stack = stack_small, cap = 64, sp = 0;
    stack[sp++] = inst;
    while (sp > 0) {
        i32 i = stack[--sp];
        w->inst[i].flags |= UB_INST_DESTROYED;
        if (w->inst[i].part >= 0) w->parts[w->inst[i].part].flags |= UB_PART_REMOVED;
        for (i32 c = w->inst[i].first_child; c >= 0; c = w->inst[c].next_sibling) {
            if (sp == cap) {
                i32 *ns = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap * 2);
                if (!ns) break;
                memcpy(ns, stack, sizeof(i32) * (size_t)cap);
                if (stack != stack_small) plat_free(stack);
                stack = ns;
                cap *= 2;
            }
            stack[sp++] = c;
        }
    }
    if (stack != stack_small) plat_free(stack);
}

static bool push_free(i32 **list, i32 *count, i32 *cap, i32 v)
{
    if (!grow((void **)list, cap, *count + 1, sizeof(i32))) return false;
    (*list)[(*count)++] = v;
    return true;
}

void ub_world_reclaim(ub_world *w, i32 inst)
{
    if (inst < 0 || inst >= w->count) return;
    ub_instance *in = &w->inst[inst];
    if (!(in->flags & UB_INST_DESTROYED) || (in->flags & UB_INST_FREE)) return;
    if (!grow((void **)&w->free_inst, &w->free_inst_cap, w->free_inst_count + 1, sizeof(i32))) return;
    if (in->part >= 0 && !push_free(&w->free_part, &w->free_part_count, &w->free_part_cap, in->part)) return;
    unlink_child(w, inst);
    for (i32 c = in->first_child; c >= 0;) {
        i32 next = w->inst[c].next_sibling;
        w->inst[c].parent = -1;
        w->inst[c].next_sibling = -1;
        c = next;
    }
    if (in->part >= 0) {
        w->part_inst[in->part] = -1;
        w->parts[in->part].flags = UB_PART_REMOVED;
        w->parts[in->part].transparency = 255;
    }
    for (i32 k = 0; k < w->weld_count;) { /* welds to it go (they would join whatever reuses the slot) */
        if (w->welds[k].a == inst || w->welds[k].b == inst) w->welds[k] = w->welds[--w->weld_count];
        else ++k;
    }
    in->first_child = in->last_child = in->next_sibling = in->parent = -1;
    in->part = -1;
    in->source = UB_NO_STRING;
    in->flags = UB_INST_DESTROYED | UB_INST_FREE;
    ub_world_clear_props(w, inst);
    w->free_inst[w->free_inst_count++] = inst;
}

i32 ub_world_clone(ub_world *w, i32 inst)
{
    if (!ub_world_alive(w, inst)) return -1;
    i32 old_count = w->count;
    i32 *map = (i32 *)plat_alloc(sizeof(i32) * (size_t)(old_count + 1));
    i32 *stack = (i32 *)plat_alloc(sizeof(i32) * 2 * (size_t)(old_count + 1));
    if (!map || !stack) {
        plat_free(map);
        plat_free(stack);
        return -1;
    }
    for (i32 i = 0; i < old_count; ++i) map[i] = -1;
    i32 root = -1, sp = 0;
    stack[sp++] = inst;
    stack[sp++] = -1; /* (instance, new parent) pairs */
    while (sp > 0) {
        i32 np = stack[--sp], i = stack[--sp];
        ub_instance src = w->inst[i]; /* copy: adding may move the array */
        i32 ni;
        if (src.part >= 0) {
            ub_part p = w->parts[src.part];
            p.flags &= (u8)~UB_PART_REMOVED;
            ni = ub_world_add_part(w, np, (ub_class)src.cls, NULL, NULL, &p);
        } else {
            ni = ub_world_add(w, np, (ub_class)src.cls, NULL, NULL);
        }
        if (ni < 0) break;
        ub_instance *d = &w->inst[ni];
        d->name = src.name; /* the pool never changes, so share the strings */
        d->class_name = src.class_name;
        d->source = src.source;
        d->flags = (u16)(src.flags & UB_INST_SAVED_FLAGS);
        if (!ub_world_copy_props(w, ni, i)) break;
        map[i] = ni;
        if (root < 0) {
            root = ni;
            d->flags |= UB_INST_DETACHED;
        }
        /* children in reverse so they keep their order */
        i32 kids = 0;
        for (i32 c = src.first_child; c >= 0; c = w->inst[c].next_sibling) kids++;
        i32 base = sp, k = 0;
        sp += 2 * kids;
        for (i32 c = src.first_child; c >= 0; c = w->inst[c].next_sibling, ++k) {
            stack[base + 2 * (kids - 1 - k)] = c;
            stack[base + 2 * (kids - 1 - k) + 1] = ni;
        }
    }
    /* references inside the copy point at the copies (as Roblox clones) */
    for (i32 i = 0; i < old_count; ++i) {
        if (map[i] < 0) continue;
        int np = ub_world_prop_count(w, map[i]);
        for (int k = 0; k < np; ++k) {
            ub_prop *p = (ub_prop *)ub_world_prop_at(w, map[i], k);
            if (p->val.type == UB_PV_REF && p->val.v.ref >= 0 && p->val.v.ref < old_count && map[p->val.v.ref] >= 0)
                p->val.v.ref = map[p->val.v.ref];
        }
    }
    /* welds whose two parts were both copied */
    i32 nw = w->weld_count;
    for (i32 k = 0; k < nw; ++k) {
        i32 a = w->welds[k].a, b = w->welds[k].b;
        if (a < old_count && b < old_count && map[a] >= 0 && map[b] >= 0) ub_world_add_weld(w, map[a], map[b]);
    }
    plat_free(map);
    plat_free(stack);
    return root;
}

bool ub_world_set_name(ub_world *w, i32 inst, const char *name)
{
    if (inst < 0 || inst >= w->count) return false;
    u32 n = add_string(w, name ? name : "");
    if (n == 0xFFFFFFFFu) return false;
    w->inst[inst].name = n;
    w->epoch++;
    return true;
}

bool ub_world_set_source(ub_world *w, i32 inst, const char *source)
{
    if (inst < 0 || inst >= w->count) return false;
    u32 n = add_string(w, source ? source : "");
    if (n == 0xFFFFFFFFu) return false;
    w->inst[inst].source = n;
    return true;
}

const char *ub_world_source(const ub_world *w, i32 inst)
{
    if (inst < 0 || inst >= w->count || w->inst[inst].source == UB_NO_STRING) return "";
    return ub_world_str(w, w->inst[inst].source);
}

i32 ub_world_part_inst(const ub_world *w, i32 part)
{
    return part >= 0 && part < w->part_count && w->part_inst ? w->part_inst[part] : -1;
}

i32 ub_world_find_child(const ub_world *w, i32 parent, const char *name)
{
    i32 c = parent >= 0 ? w->inst[parent].first_child : -1;
    for (; c >= 0; c = w->inst[c].next_sibling) {
        const char *a = ub_world_str(w, w->inst[c].name), *b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == *b) return c;
    }
    if (parent < 0) { /* top level: scan for parentless instances */
        for (i32 i = 0; i < w->count; ++i) {
            if (w->inst[i].parent >= 0) continue;
            const char *a = ub_world_str(w, w->inst[i].name), *b = name;
            while (*a && *a == *b) { ++a; ++b; }
            if (*a == *b) return i;
        }
    }
    return -1;
}

i32 ub_world_workspace(ub_world *w)
{
    if (w->workspace < 0) w->workspace = ub_world_add(w, -1, UB_CLASS_WORKSPACE, NULL, "Workspace");
    return w->workspace;
}

bool ub_world_is_descendant(const ub_world *w, i32 index, i32 ancestor)
{
    for (int guard = 0; index >= 0 && guard <= w->count; ++guard) {
        if (index == ancestor) return true;
        index = w->inst[index].parent;
    }
    return false;
}

const char *ub_load_result_name(ub_load_result r)
{
    switch (r) {
    case UB_LOAD_OK: return "ok";
    case UB_LOAD_NOT_FOUND: return "file not found";
    case UB_LOAD_BAD_FORMAT: return "not a place file this version understands";
    case UB_LOAD_CORRUPT: return "file is damaged";
    case UB_LOAD_UNSUPPORTED: return "made by a newer version or uses an unsupported feature";
    case UB_LOAD_NO_MEMORY: return "out of memory";
    }
    return "?";
}

/* ---------------------------------------------------------------- writer */

typedef struct {
    u8 *p;
    u32 len, cap;
    bool failed;
} buf;

static void put(buf *b, const void *data, u32 n)
{
    if (b->failed) return;
    if (b->len + n > b->cap) {
        u32 nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n) nc *= 2;
        u8 *np = (u8 *)plat_alloc(nc);
        if (!np) {
            b->failed = true;
            return;
        }
        if (b->p) {
            memcpy(np, b->p, b->len);
            plat_free(b->p);
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void put_u8(buf *b, u32 v) { u8 x = (u8)v; put(b, &x, 1); }
static void put_u16(buf *b, u32 v) { u8 x[2] = { (u8)v, (u8)(v >> 8) }; put(b, x, 2); }
static void put_u32(buf *b, u32 v)
{
    u8 x[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    put(b, x, 4);
}
static void put_f32(buf *b, f32 f)
{
    u32 v;
    memcpy(&v, &f, 4);
    put_u32(b, v);
}

/* Starts a chunk; returns the offset of its length field. */
static u32 chunk_begin(buf *b, const char *tag)
{
    put(b, tag, 4);
    u32 at = b->len;
    put_u32(b, 0);
    return at;
}

static void chunk_end(buf *b, u32 at)
{
    if (b->failed) return;
    u32 n = b->len - at - 4;
    b->p[at] = (u8)n;
    b->p[at + 1] = (u8)(n >> 8);
    b->p[at + 2] = (u8)(n >> 16);
    b->p[at + 3] = (u8)(n >> 24);
}

/* The order instances are saved in (tree pre-order, skipping what is not
 * part of the place); remap[i] = position in the file or -1. */
static i32 save_order(const ub_world *w, i32 *order, i32 *remap, i32 *stack)
{
    /* made at run time, destroyed or unparented: not part of the place */
    const u16 skip = UB_INST_DESTROYED | UB_INST_RUNTIME | UB_INST_DETACHED;
    i32 n = 0;
    for (i32 root = 0; root < w->count; ++root) {
        if (w->inst[root].parent >= 0 || (w->inst[root].flags & skip)) continue;
        i32 sp = 0;
        stack[sp++] = root;
        while (sp > 0) {
            i32 i = stack[--sp];
            order[n++] = i;
            /* push children in reverse so they pop in order */
            i32 kids = 0;
            for (i32 c = w->inst[i].first_child; c >= 0; c = w->inst[c].next_sibling)
                kids += !(w->inst[c].flags & skip);
            i32 base = sp;
            sp += kids;
            i32 k = 0;
            for (i32 c = w->inst[i].first_child; c >= 0; c = w->inst[c].next_sibling)
                if (!(w->inst[c].flags & skip)) stack[base + kids - 1 - k++] = c;
        }
    }
    for (i32 i = 0; i < w->count; ++i) remap[i] = -1;
    for (i32 i = 0; i < n; ++i) remap[order[i]] = i;
    return n;
}

/* PROP: the names of the keys used (in order of first use), then
 * (instance, key, type, value) for every property of a saved instance. */
static void save_props(const ub_world *w, buf *b, const i32 *order, i32 n, const i32 *remap)
{
    i32 *local = (i32 *)plat_alloc(sizeof(i32) * (size_t)(w->key_count + 1));
    if (!local) {
        b->failed = true;
        return;
    }
    for (i32 k = 0; k < w->key_count; ++k) local[k] = -1;
    u32 nkeys = 0, entries = 0;
    for (i32 k = 0; k < n; ++k)
        for (int i = 0; i < ub_world_prop_count(w, order[k]); ++i) {
            const ub_prop *p = ub_world_prop_at(w, order[k], i);
            if (local[p->key] < 0) local[p->key] = (i32)nkeys++;
            entries++;
        }
    if (!entries) {
        plat_free(local);
        return;
    }
    u32 c = chunk_begin(b, "PROP");
    put_u32(b, nkeys);
    for (u32 j = 0; j < nkeys; ++j) /* key names: offsets into STRS */
        for (i32 k = 0; k < w->key_count; ++k)
            if (local[k] == (i32)j) put_u32(b, w->keys[k]);
    put_u32(b, entries);
    for (i32 k = 0; k < n; ++k)
        for (int i = 0; i < ub_world_prop_count(w, order[k]); ++i) {
            const ub_prop *p = ub_world_prop_at(w, order[k], i);
            const ub_value *v = &p->val;
            put_u32(b, (u32)k);
            put_u32(b, (u32)local[p->key]);
            put_u8(b, v->type);
            switch (v->type) {
            case UB_PV_BOOL: put_u8(b, v->v.b ? 1 : 0); break;
            case UB_PV_INT: case UB_PV_ENUM: case UB_PV_BRICKCOLOR: put_u32(b, (u32)v->v.i); break;
            case UB_PV_REF:
                put_u32(b, (u32)(v->v.ref >= 0 && v->v.ref < w->count ? remap[v->v.ref] : -1));
                break;
            case UB_PV_FLOAT: put_f32(b, v->v.f); break;
            case UB_PV_DOUBLE: put(b, &v->v.d, 8); break;
            case UB_PV_STRING: put_u32(b, v->v.str.len); put(b, v->v.str.s, v->v.str.len); break;
            case UB_PV_COLOR3: put_f32(b, v->v.c3.r); put_f32(b, v->v.c3.g); put_f32(b, v->v.c3.b); break;
            case UB_PV_VECTOR3: put_f32(b, v->v.v3.x); put_f32(b, v->v.v3.y); put_f32(b, v->v.v3.z); break;
            case UB_PV_VECTOR2: put_f32(b, v->v.v2.x); put_f32(b, v->v.v2.y); break;
            case UB_PV_UDIM: put_f32(b, v->v.ud.s); put_u32(b, (u32)v->v.ud.o); break;
            case UB_PV_UDIM2:
                put_f32(b, v->v.ud2.sx); put_u32(b, (u32)v->v.ud2.ox);
                put_f32(b, v->v.ud2.sy); put_u32(b, (u32)v->v.ud2.oy);
                break;
            case UB_PV_RECT:
                put_f32(b, v->v.rect.x0); put_f32(b, v->v.rect.y0); put_f32(b, v->v.rect.x1); put_f32(b, v->v.rect.y1);
                break;
            case UB_PV_NUMRANGE: put_f32(b, v->v.range.lo); put_f32(b, v->v.range.hi); break;
            case UB_PV_NUMSEQ: case UB_PV_COLORSEQ: {
                u32 stride = v->type == UB_PV_NUMSEQ ? 3 : 4;
                put_u32(b, v->v.seq.n);
                for (u32 j = 0; j < v->v.seq.n * stride; ++j) put_f32(b, v->v.seq.k[j]);
                break;
            }
            case UB_PV_CFRAME:
                for (int j = 0; j < 12; ++j) put_f32(b, v->v.cf.m[j]);
                break;
            case UB_PV_FONT: {
                const char *f = v->v.font.family ? v->v.font.family : "";
                u32 len = (u32)ub_strlen(f);
                put_u16(b, v->v.font.weight);
                put_u8(b, v->v.font.style);
                put_u32(b, len);
                put(b, f, len);
                break;
            }
            default: break;
            }
        }
    chunk_end(b, c);
    plat_free(local);
}

bool ub_world_save_remap(const ub_world *w, i32 *remap)
{
    i32 *order = (i32 *)plat_alloc(sizeof(i32) * (size_t)(w->count + 1));
    i32 *stack = (i32 *)plat_alloc(sizeof(i32) * (size_t)(w->count + 1));
    bool ok = order && stack;
    if (ok) save_order(w, order, remap, stack);
    plat_free(order);
    plat_free(stack);
    return ok;
}

bool ub_ubx_save(const ub_world *w, u8 **data, u32 *size)
{
    *data = NULL;
    *size = 0;
    /* tree pre-order, so every parent is written before its children */
    i32 *order = (i32 *)plat_alloc(sizeof(i32) * (size_t)(w->count + 1));
    i32 *remap = (i32 *)plat_alloc(sizeof(i32) * (size_t)(w->count + 1));
    i32 *stack = (i32 *)plat_alloc(sizeof(i32) * (size_t)(w->count + 1));
    if (!order || !remap || !stack) {
        plat_free(order);
        plat_free(remap);
        plat_free(stack);
        return false;
    }
    i32 n = save_order(w, order, remap, stack);

    buf b = { 0 };
    put(&b, "UBXPLACE", 8);
    put_u32(&b, UB_UBX_VERSION);

    u32 c = chunk_begin(&b, "INFO");
    u32 nl = (u32)ub_strlen(w->name);
    put_u16(&b, nl);
    put(&b, w->name, nl);
    chunk_end(&b, c);

    c = chunk_begin(&b, "STRS");
    put_u32(&b, w->str_len);
    put(&b, w->strings, w->str_len);
    chunk_end(&b, c);

    c = chunk_begin(&b, "INST");
    put_u32(&b, (u32)n);
    i32 parts_written = 0;
    for (i32 k = 0; k < n; ++k) {
        const ub_instance *in = &w->inst[order[k]];
        put_u32(&b, (u32)(in->parent >= 0 ? remap[in->parent] : -1));
        put_u16(&b, in->cls);
        put_u16(&b, in->flags & UB_INST_SAVED_FLAGS);
        put_u32(&b, in->name);
        put_u32(&b, in->class_name);
        put_u32(&b, (u32)(in->part >= 0 ? parts_written++ : -1)); /* parts renumbered in tree order */
    }
    chunk_end(&b, c);

    c = chunk_begin(&b, "PART");
    put_u32(&b, (u32)parts_written);
    for (i32 k = 0; k < n; ++k) {
        const ub_instance *in = &w->inst[order[k]];
        if (in->part < 0) continue;
        const ub_part *p = &w->parts[in->part];
        put_f32(&b, p->pos.x); put_f32(&b, p->pos.y); put_f32(&b, p->pos.z);
        const vec3 *cols[3] = { &p->rot.x, &p->rot.y, &p->rot.z };
        for (int j = 0; j < 3; ++j) { put_f32(&b, cols[j]->x); put_f32(&b, cols[j]->y); put_f32(&b, cols[j]->z); }
        put_f32(&b, p->size.x); put_f32(&b, p->size.y); put_f32(&b, p->size.z);
        put_u32(&b, p->color);
        put_u8(&b, p->shape);
        put_u8(&b, p->material);
        put_u8(&b, p->transparency);
        put_u8(&b, p->flags);
    }
    chunk_end(&b, c);

    c = chunk_begin(&b, "LITE");
    put_f32(&b, w->lighting.clock_time);
    put_f32(&b, w->lighting.latitude);
    put_u32(&b, w->lighting.ambient);
    put_u32(&b, w->lighting.outdoor_ambient);
    put_f32(&b, w->lighting.brightness);
    put_f32(&b, w->lighting.fog_end);
    put_u32(&b, w->lighting.fog_color);
    chunk_end(&b, c);

    c = chunk_begin(&b, "WSPC");
    put_f32(&b, w->gravity);
    put_f32(&b, w->fallen_height);
    chunk_end(&b, c);

    c = chunk_begin(&b, "WELD");
    u32 saved_welds = 0;
    for (i32 k = 0; k < w->weld_count; ++k) saved_welds += remap[w->welds[k].a] >= 0 && remap[w->welds[k].b] >= 0;
    put_u32(&b, saved_welds);
    for (i32 k = 0; k < w->weld_count; ++k) {
        if (remap[w->welds[k].a] < 0 || remap[w->welds[k].b] < 0) continue;
        put_u32(&b, (u32)remap[w->welds[k].a]);
        put_u32(&b, (u32)remap[w->welds[k].b]);
    }
    chunk_end(&b, c);

    c = chunk_begin(&b, "SRCS");
    u32 nsrc = 0;
    for (i32 k = 0; k < n; ++k) nsrc += w->inst[order[k]].source != UB_NO_STRING;
    put_u32(&b, nsrc);
    for (i32 k = 0; k < n; ++k) { /* (instance, string offset): the code is in STRS */
        if (w->inst[order[k]].source == UB_NO_STRING) continue;
        put_u32(&b, (u32)k);
        put_u32(&b, w->inst[order[k]].source);
    }
    chunk_end(&b, c);

    save_props(w, &b, order, n, remap);

    /* meshes, and which saved part uses which (only if any does) */
    bool any_mesh = false;
    for (i32 k = 0; k < n && !any_mesh; ++k) {
        const ub_instance *in = &w->inst[order[k]];
        any_mesh = in->part >= 0 && (w->parts[in->part].mesh != 0 || w->parts[in->part].look != 0);
    }
    if (any_mesh) {
        c = chunk_begin(&b, "MESH");
        put_u32(&b, (u32)w->mesh_count);
        for (i32 i = 0; i < w->mesh_count; ++i) {
            const ub_wmesh *m = &w->meshes[i];
            put_u32(&b, m->nv);
            put_u32(&b, m->nt);
            put_u32(&b, m->npieces);
            put_u32(&b, m->flags);
            for (u32 j = 0; j < m->nv * 3; ++j) put_f32(&b, m->v[j]);
            for (u32 j = 0; j < m->nt * 3; ++j) put_u32(&b, m->tri[j]);
            for (u32 j = 0; j < m->npieces; ++j) put_u32(&b, m->piece_v[j]);
            for (u32 j = 0; j < m->npieces; ++j) put_u32(&b, m->piece_t[j]);
        }
        chunk_end(&b, c);
        c = chunk_begin(&b, "PMSH");
        put_u32(&b, (u32)parts_written);
        for (i32 k = 0; k < n; ++k) {
            const ub_instance *in = &w->inst[order[k]];
            if (in->part >= 0) {
                put_u16(&b, w->parts[in->part].mesh);
                put_u16(&b, w->parts[in->part].look);
            }
        }
        chunk_end(&b, c);
    }

    c = chunk_begin(&b, "END ");
    chunk_end(&b, c);

    plat_free(order);
    plat_free(remap);
    plat_free(stack);
    if (b.failed) {
        plat_free(b.p);
        return false;
    }
    *data = b.p;
    *size = b.len;
    return true;
}

/* ---------------------------------------------------------------- reader */

typedef struct {
    const u8 *p;
    u32 len, pos;
    bool bad;
} rd;

static u32 get_u8(rd *r) { if (r->pos + 1 > r->len) { r->bad = true; return 0; } return r->p[r->pos++]; }
static u32 get_u16(rd *r)
{
    if (r->pos + 2 > r->len) { r->bad = true; return 0; }
    u32 v = r->p[r->pos] | ((u32)r->p[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}
static u32 get_u32(rd *r)
{
    if (r->pos + 4 > r->len) { r->bad = true; return 0; }
    const u8 *q = r->p + r->pos;
    r->pos += 4;
    return q[0] | ((u32)q[1] << 8) | ((u32)q[2] << 16) | ((u32)q[3] << 24);
}
static f32 get_f32(rd *r)
{
    u32 v = get_u32(r);
    f32 f;
    memcpy(&f, &v, 4);
    return f;
}

/* Untrusted numbers: NaN/infinite/huge values become harmless ones. */
static f32 sane(f32 v, f32 lo, f32 hi, f32 fallback)
{
    return (v >= lo && v <= hi) ? v : fallback; /* false for NaN */
}

void ub_part_sanitize(ub_part *p)
{
    const f32 big = 1.0e6f;
    p->pos.x = sane(p->pos.x, -big, big, 0);
    p->pos.y = sane(p->pos.y, -big, big, 0);
    p->pos.z = sane(p->pos.z, -big, big, 0);
    p->size.x = sane(p->size.x, 0.0f, 1.0e5f, 1);
    p->size.y = sane(p->size.y, 0.0f, 1.0e5f, 1);
    p->size.z = sane(p->size.z, 0.0f, 1.0e5f, 1);
    const f32 *m = &p->rot.x.x; /* mat3 is nine consecutive floats */
    for (int j = 0; j < 9; ++j)
        if (!(m[j] >= -2.0f && m[j] <= 2.0f)) { /* also catches NaN */
            p->rot = mat3_identity();
            break;
        }
}

/* ------------------------------------------------------------- meshes */

u16 ub_world_add_mesh(ub_world *w, const f32 *v, u32 nv, const u32 *tri, u32 nt, const u32 *piece_v,
                      const u32 *piece_t, u32 npieces, u32 flags)
{
    flags &= UB_WMESH_WOUND;
    if (!nv || !nt || !npieces || nv > 1000000 || nt > 2000000 || npieces > 100000 || w->mesh_count >= 65535)
        return 0;
    /* valid: indices in range, pieces in order and ending at the totals */
    for (u32 i = 0; i < nt * 3; ++i)
        if (tri[i] >= nv) return 0;
    for (u32 i = 0; i < npieces; ++i)
        if ((i > 0 && (piece_v[i] < piece_v[i - 1] || piece_t[i] < piece_t[i - 1])) || piece_v[i] > nv || piece_t[i] > nt)
            return 0;
    if (piece_v[npieces - 1] != nv || piece_t[npieces - 1] != nt) return 0;
    u32 h = 2166136261u ^ flags;
    const u8 *bytes[4] = { (const u8 *)v, (const u8 *)tri, (const u8 *)piece_v, (const u8 *)piece_t };
    const u32 lens[4] = { nv * 12, nt * 12, npieces * 4, npieces * 4 };
    for (int k = 0; k < 4; ++k)
        for (u32 i = 0; i < lens[k]; ++i) h = (h ^ bytes[k][i]) * 16777619u;
    for (i32 i = 0; i < w->mesh_count; ++i) {
        const ub_wmesh *m = &w->meshes[i];
        if (m->hash == h && m->flags == flags && m->nv == nv && m->nt == nt && m->npieces == npieces && !memcmp(m->v, v, lens[0]) &&
            !memcmp(m->tri, tri, lens[1]) && !memcmp(m->piece_v, piece_v, lens[2]) && !memcmp(m->piece_t, piece_t, lens[3]))
            return (u16)(i + 1);
    }
    if (!grow((void **)&w->meshes, &w->mesh_cap, w->mesh_count + 1, sizeof(ub_wmesh))) return 0;
    u8 *block = (u8 *)plat_alloc((size_t)lens[0] + lens[1] + lens[2] + lens[3]);
    if (!block) return 0;
    ub_wmesh *m = &w->meshes[w->mesh_count];
    memset(m, 0, sizeof *m);
    m->v = (f32 *)block;
    m->tri = (u32 *)(block + lens[0]);
    m->piece_v = (u32 *)(block + lens[0] + lens[1]);
    m->piece_t = (u32 *)(block + lens[0] + lens[1] + lens[2]);
    memcpy(m->v, v, lens[0]);
    memcpy(m->tri, tri, lens[1]);
    memcpy(m->piece_v, piece_v, lens[2]);
    memcpy(m->piece_t, piece_t, lens[3]);
    for (u32 i = 0; i < nv * 3; ++i) /* untrusted: inside a sane box, never NaN */
        m->v[i] = sane(m->v[i], -1000.0f, 1000.0f, 0);
    m->nv = nv;
    m->nt = nt;
    m->npieces = npieces;
    m->flags = flags;
    m->hash = h;
    w->mesh_count++;
    return (u16)w->mesh_count;
}

const struct ub_mesh_s *ub_world_render_mesh(const ub_world *w, u16 id)
{
    if (id == 0 || (i32)id > w->mesh_count) return NULL;
    ub_wmesh *m = &((ub_world *)w)->meshes[id - 1]; /* the cache is not part of the world's value */
    if (!m->render_cache)
        m->render_cache = ub_mesh_from_triangles(m->v, m->nv, m->tri, m->nt, m->piece_v, m->piece_t, m->npieces,
                                                 (m->flags & UB_WMESH_WOUND) != 0);
    return (const ub_mesh *)m->render_cache;
}

static f32 get_fin(rd *r) /* not NaN (infinity is a real value: BillboardGui.MaxDistance) */
{
    f32 v = get_f32(r);
    return v == v ? v : 0;
}

u32 ub_world_key_at(ub_world *w, u32 off); /* ub_props.c */

static ub_load_result load_props(ub_world *w, const u8 *p, u32 len, u32 inst_count)
{
    rd r = { p, len, 0, false };
    u32 nkeys = get_u32(&r);
    if (r.bad || nkeys > len / 4) return UB_LOAD_CORRUPT;
    u32 *keys = (u32 *)plat_alloc(sizeof(u32) * (nkeys + 1));
    if (!keys) return UB_LOAD_NO_MEMORY;
    ub_load_result res = UB_LOAD_OK;
    char name[256];
    for (u32 i = 0; i < nkeys && res == UB_LOAD_OK; ++i) {
        u32 off = get_u32(&r);
        keys[i] = r.bad ? UB_NO_STRING : ub_world_key_at(w, off);
        if (keys[i] == UB_NO_STRING) res = UB_LOAD_CORRUPT;
    }
    u32 entries = res == UB_LOAD_OK ? get_u32(&r) : 0;
    if (r.bad) res = UB_LOAD_CORRUPT;
    f32 seq[UB_SEQ_MAX_KEYS * 4];
    for (u32 e = 0; e < entries && res == UB_LOAD_OK; ++e) {
        u32 at = get_u32(&r), key = get_u32(&r), type = get_u8(&r);
        if (r.bad || at >= inst_count || key >= nkeys) {
            res = UB_LOAD_CORRUPT;
            break;
        }
        ub_value v;
        memset(&v, 0, sizeof v);
        v.type = (u8)type;
        switch (type) {
        case UB_PV_BOOL: v.v.b = get_u8(&r) != 0; break;
        case UB_PV_INT: case UB_PV_ENUM: case UB_PV_BRICKCOLOR: v.v.i = (i32)get_u32(&r); break;
        case UB_PV_REF: {
            i32 ref = (i32)get_u32(&r);
            v.v.ref = ref >= 0 && (u32)ref < inst_count ? ref : -1;
            break;
        }
        case UB_PV_FLOAT: v.v.f = get_fin(&r); break;
        case UB_PV_DOUBLE:
            if (r.len - r.pos < 8) r.bad = true;
            else {
                memcpy(&v.v.d, r.p + r.pos, 8);
                r.pos += 8;
                /* only attributes ("@Name") may be infinite or NaN, as scripts can make them */
                const char *kn = ub_world_key_name(w, keys[key]);
                if (!(v.v.d >= -1.0e300 && v.v.d <= 1.0e300) && !(kn && kn[0] == '@')) v.v.d = 0;
            }
            break;
        case UB_PV_STRING: {
            u32 n = get_u32(&r);
            if (r.bad || n > r.len - r.pos) r.bad = true;
            else {
                v.v.str.s = (char *)(r.p + r.pos); /* copied by set_prop */
                v.v.str.len = n;
                r.pos += n;
            }
            break;
        }
        case UB_PV_COLOR3: v.v.c3.r = get_fin(&r); v.v.c3.g = get_fin(&r); v.v.c3.b = get_fin(&r); break;
        case UB_PV_VECTOR3: v.v.v3.x = get_fin(&r); v.v.v3.y = get_fin(&r); v.v.v3.z = get_fin(&r); break;
        case UB_PV_VECTOR2: v.v.v2.x = get_fin(&r); v.v.v2.y = get_fin(&r); break;
        case UB_PV_UDIM: v.v.ud.s = get_fin(&r); v.v.ud.o = (i32)get_u32(&r); break;
        case UB_PV_UDIM2:
            v.v.ud2.sx = get_fin(&r); v.v.ud2.ox = (i32)get_u32(&r);
            v.v.ud2.sy = get_fin(&r); v.v.ud2.oy = (i32)get_u32(&r);
            break;
        case UB_PV_RECT:
            v.v.rect.x0 = get_fin(&r); v.v.rect.y0 = get_fin(&r); v.v.rect.x1 = get_fin(&r); v.v.rect.y1 = get_fin(&r);
            break;
        case UB_PV_NUMRANGE: v.v.range.lo = get_fin(&r); v.v.range.hi = get_fin(&r); break;
        case UB_PV_NUMSEQ: case UB_PV_COLORSEQ: {
            u32 n = get_u32(&r), stride = type == UB_PV_NUMSEQ ? 3 : 4;
            if (r.bad || n < 2 || n > UB_SEQ_MAX_KEYS) r.bad = true;
            else {
                for (u32 j = 0; j < n * stride; ++j) seq[j] = get_fin(&r);
                v.v.seq.k = seq;
                v.v.seq.n = n;
            }
            break;
        }
        case UB_PV_CFRAME:
            for (int j = 0; j < 12; ++j) seq[j] = get_fin(&r);
            v.v.cf.m = seq;
            break;
        case UB_PV_FONT: {
            v.v.font.weight = (u16)get_u16(&r);
            v.v.font.style = (u8)get_u8(&r);
            u32 n = get_u32(&r);
            if (r.bad || n >= sizeof name || n > r.len - r.pos) r.bad = true;
            else {
                memcpy(name, r.p + r.pos, n);
                name[n] = 0;
                r.pos += n;
                v.v.font.family = name;
            }
            break;
        }
        default: r.bad = true; break;
        }
        if (r.bad) res = UB_LOAD_CORRUPT;
        else if (!ub_world_set_prop(w, (i32)at, keys[key], &v)) res = UB_LOAD_NO_MEMORY;
    }
    if (res == UB_LOAD_OK && r.pos != r.len) res = UB_LOAD_CORRUPT;
    plat_free(keys);
    return res;
}

ub_load_result ub_ubx_load(ub_world *w, const u8 *data, u32 size)
{
    if (size < 12 || memcmp(data, "UBXPLACE", 8) != 0) return UB_LOAD_BAD_FORMAT;
    rd top = { data, size, 8, false };
    u32 version = get_u32(&top);
    if (version > UB_UBX_VERSION) return UB_LOAD_UNSUPPORTED;

    bool have_inst = false, have_parts = false, have_strs = false, ended = false;
    u32 inst_count = 0, part_count = 0;
    const u8 *inst_p = NULL;
    u32 inst_len = 0;
    const u8 *weld_p = NULL, *src_p = NULL, *prop_p = NULL, *pmsh_p = NULL;
    u32 weld_count = 0, src_len = 0, prop_len = 0, pmsh_len = 0;
    while (!ended) {
        if (top.len - top.pos < 8) return UB_LOAD_CORRUPT;
        const u8 *tag = top.p + top.pos;
        top.pos += 4;
        u32 len = get_u32(&top);
        if (len > top.len - top.pos) return UB_LOAD_CORRUPT;
        rd r = { top.p + top.pos, len, 0, false };
        top.pos += len;
        if (memcmp(tag, "INFO", 4) == 0) {
            u32 n = get_u16(&r);
            if (n > len - 2 || n >= sizeof w->name) return UB_LOAD_CORRUPT;
            memcpy(w->name, r.p + 2, n);
            w->name[n] = 0;
        } else if (memcmp(tag, "STRS", 4) == 0) {
            u32 n = get_u32(&r);
            if (r.bad || n != len - 4 || (n > 0 && r.p[len - 1] != 0)) return UB_LOAD_CORRUPT;
            w->strings = (char *)plat_alloc(n ? n : 1);
            if (!w->strings) return UB_LOAD_NO_MEMORY;
            memcpy(w->strings, r.p + 4, n);
            w->str_len = w->str_cap = n;
            have_strs = true;
        } else if (memcmp(tag, "INST", 4) == 0) {
            inst_count = get_u32(&r);
            if (r.bad || inst_count > (len - 4) / 20 || (len - 4) != inst_count * 20) return UB_LOAD_CORRUPT;
            inst_p = r.p + 4;
            inst_len = len - 4;
            have_inst = true;
        } else if (memcmp(tag, "PART", 4) == 0) {
            part_count = get_u32(&r);
            if (r.bad || (len - 4) / 68 < part_count || (len - 4) != part_count * 68) return UB_LOAD_CORRUPT;
            if (part_count) {
                w->parts = (ub_part *)plat_alloc(sizeof(ub_part) * part_count);
                if (!w->parts) return UB_LOAD_NO_MEMORY;
            }
            for (u32 i = 0; i < part_count; ++i) {
                ub_part *p = &w->parts[i];
                p->pos.x = get_f32(&r); p->pos.y = get_f32(&r); p->pos.z = get_f32(&r);
                vec3 *cols[3] = { &p->rot.x, &p->rot.y, &p->rot.z };
                for (int j = 0; j < 3; ++j) { cols[j]->x = get_f32(&r); cols[j]->y = get_f32(&r); cols[j]->z = get_f32(&r); }
                p->size.x = get_f32(&r); p->size.y = get_f32(&r); p->size.z = get_f32(&r);
                p->color = get_u32(&r) & 0xFFFFFF;
                p->shape = (u8)get_u8(&r);
                p->material = (u8)get_u8(&r);
                p->transparency = (u8)get_u8(&r);
                p->flags = (u8)get_u8(&r);
                ub_part_sanitize(p);
            }
            w->part_count = w->part_cap = (i32)part_count;
            have_parts = true;
        } else if (memcmp(tag, "LITE", 4) == 0) {
            w->lighting.clock_time = get_f32(&r);
            w->lighting.latitude = get_f32(&r);
            w->lighting.ambient = get_u32(&r) & 0xFFFFFF;
            w->lighting.outdoor_ambient = get_u32(&r) & 0xFFFFFF;
            w->lighting.brightness = get_f32(&r);
            w->lighting.fog_end = get_f32(&r);
            w->lighting.fog_color = get_u32(&r) & 0xFFFFFF;
            if (r.bad) return UB_LOAD_CORRUPT;
        } else if (memcmp(tag, "WSPC", 4) == 0) {
            f32 g = get_f32(&r), fh = get_f32(&r);
            if (r.bad) return UB_LOAD_CORRUPT;
            w->gravity = sane(g, -1.0e4f, 1.0e4f, UB_GRAVITY_DEFAULT);
            w->fallen_height = sane(fh, -1.0e6f, 1.0e6f, UB_FALLEN_HEIGHT_DEFAULT);
        } else if (memcmp(tag, "WELD", 4) == 0) {
            weld_count = get_u32(&r);
            if (r.bad || weld_count > (len - 4) / 8 || (len - 4) != weld_count * 8) return UB_LOAD_CORRUPT;
            weld_p = r.p + 4;
        } else if (memcmp(tag, "SRCS", 4) == 0) {
            src_p = r.p;
            src_len = len;
        } else if (memcmp(tag, "PROP", 4) == 0) {
            prop_p = r.p;
            prop_len = len;
        } else if (memcmp(tag, "MESH", 4) == 0) {
            u32 count = get_u32(&r);
            if (r.bad || count > 65535) return UB_LOAD_CORRUPT;
            for (u32 i = 0; i < count; ++i) {
                u32 nv = get_u32(&r), nt = get_u32(&r), np = get_u32(&r), mflags = get_u32(&r);
                u64 need = (u64)nv * 12 + (u64)nt * 12 + (u64)np * 8;
                if (r.bad || need > r.len - r.pos) return UB_LOAD_CORRUPT;
                const u8 *q = r.p + r.pos;
                r.pos += (u32)need;
                f32 *v = (f32 *)plat_alloc((size_t)need + 4);
                if (!v) return UB_LOAD_NO_MEMORY;
                memcpy(v, q, (size_t)need); /* little-endian on every machine Uniblox runs on */
                u32 *tri = (u32 *)(v + nv * 3), *pv = tri + nt * 3, *pt = pv + np;
                u16 id = ub_world_add_mesh(w, v, nv, tri, nt, pv, pt, np, mflags);
                plat_free(v);
                if (id != i + 1) return UB_LOAD_CORRUPT; /* invalid, or a duplicate a saver never writes */
            }
        } else if (memcmp(tag, "PMSH", 4) == 0) {
            pmsh_p = r.p;
            pmsh_len = len;
        } else if (memcmp(tag, "END ", 4) == 0) {
            ended = true;
        }
        /* unknown chunks are skipped */
    }
    (void)have_parts; /* optional: without it no instance may reference a part */
    if (pmsh_p) {
        rd pr = { pmsh_p, pmsh_len, 0, false };
        u32 n = get_u32(&pr);
        if (pr.bad || n != part_count || pmsh_len != 4 + n * 4) return UB_LOAD_CORRUPT;
        for (u32 i = 0; i < n; ++i) {
            u32 id = get_u16(&pr), look = get_u16(&pr);
            if ((i32)id > w->mesh_count || (i32)look > w->mesh_count) return UB_LOAD_CORRUPT;
            w->parts[i].mesh = (u16)id;
            w->parts[i].look = (u16)look;
        }
    }
    if (!have_inst || (inst_count > 0 && !have_strs)) return UB_LOAD_CORRUPT;

    if (inst_count) {
        w->inst = (ub_instance *)plat_alloc(sizeof(ub_instance) * inst_count);
        if (!w->inst) return UB_LOAD_NO_MEMORY;
    }
    w->cap = (i32)inst_count;
    rd r = { inst_p, inst_len, 0, false };
    bool *part_used = part_count ? (bool *)plat_alloc(part_count) : NULL;
    if (part_count && !part_used) return UB_LOAD_NO_MEMORY;
    for (u32 i = 0; i < inst_count; ++i) {
        i32 parent = (i32)get_u32(&r);
        u32 cls = get_u16(&r);
        u32 flags = get_u16(&r);
        u32 name = get_u32(&r);
        u32 cname = get_u32(&r);
        i32 part = (i32)get_u32(&r);
        /* parents come first (no cycles); offsets point at string starts */
        bool ok = parent >= -1 && parent < (i32)i && cls < UB_CLASS_COUNT && name < w->str_len &&
                  cname < w->str_len && (name == 0 || w->strings[name - 1] == 0) &&
                  (cname == 0 || w->strings[cname - 1] == 0) && part >= -1 && part < (i32)part_count;
        if (ok && part >= 0) {
            ok = !part_used[part];
            part_used[part] = true;
        }
        if (!ok) {
            plat_free(part_used);
            return UB_LOAD_CORRUPT;
        }
        ub_instance *in = &w->inst[i];
        in->first_child = in->last_child = in->next_sibling = -1;
        in->cls = (u16)cls;
        in->flags = (u16)(flags & UB_INST_SAVED_FLAGS);
        in->name = name;
        in->class_name = cname;
        in->part = part;
        in->source = UB_NO_STRING;
        w->count = (i32)i + 1;
        link_child(w, parent, (i32)i);
        if (cls == UB_CLASS_WORKSPACE && w->workspace < 0) w->workspace = (i32)i;
    }
    plat_free(part_used);
    if (part_count) {
        w->part_inst = (i32 *)plat_alloc(sizeof(i32) * part_count);
        if (!w->part_inst) return UB_LOAD_NO_MEMORY;
        w->part_inst_cap = (i32)part_count;
        for (u32 i = 0; i < part_count; ++i) w->part_inst[i] = -1;
        for (i32 i = 0; i < w->count; ++i)
            if (w->inst[i].part >= 0) w->part_inst[w->inst[i].part] = i;
    }

    /* scripts' code */
    if (src_p) {
        rd sr = { src_p, src_len, 0, false };
        u32 nsrc = get_u32(&sr);
        if (sr.bad || nsrc > (src_len - 4) / 8) return UB_LOAD_CORRUPT;
        for (u32 i = 0; i < nsrc; ++i) {
            u32 at = get_u32(&sr), off = get_u32(&sr);
            if (at >= inst_count || off >= w->str_len || (off > 0 && w->strings[off - 1] != 0)) return UB_LOAD_CORRUPT;
            w->inst[at].source = off;
        }
    }

    if (prop_p) {
        ub_load_result pr = load_props(w, prop_p, prop_len, inst_count);
        if (pr != UB_LOAD_OK) return pr;
    }

    /* welds last: they refer to instances by index */
    rd wr = { weld_p, weld_count * 8, 0, false };
    for (u32 i = 0; i < weld_count; ++i) {
        u32 a = get_u32(&wr), b = get_u32(&wr);
        if (a >= inst_count || b >= inst_count || a == b || w->inst[a].part < 0 || w->inst[b].part < 0)
            return UB_LOAD_CORRUPT;
        if (!ub_world_add_weld(w, (i32)a, (i32)b)) return UB_LOAD_NO_MEMORY;
    }
    return UB_LOAD_OK;
}

/* ------------------------------------------------ parts in Roblox space */

void ub_part_get_rbx(const ub_part *p, vec3 *pos, mat3 *rot)
{
    mat3 c = p->rot;
    if (p->shape == UB_SHAPE_CYLINDER) { /* ours runs along Y, Roblox's along X */
        mat3 t;
        t.x = c.y;
        t.y = v3_neg(c.x);
        t.z = c.z;
        c = t;
    } else if (p->shape == UB_SHAPE_WEDGE || p->shape == UB_SHAPE_CORNER_WEDGE) {
        c.x = v3_neg(c.x);
        c.z = v3_neg(c.z);
    }
    *rot = ub_rbx_rot(&c);
    *pos = ub_rbx_vec(p->pos);
}

void ub_part_set_rbx(ub_part *p, vec3 pos, const mat3 *rot)
{
    /* clean up drift: keep it a rotation */
    quat q = quat_from_mat3(rot);
    mat3 r = quat_to_mat3(q);
    mat3 c = ub_rbx_rot(&r);
    if (p->shape == UB_SHAPE_CYLINDER) {
        mat3 t;
        t.x = v3_neg(c.y);
        t.y = c.x;
        t.z = c.z;
        c = t;
    } else if (p->shape == UB_SHAPE_WEDGE || p->shape == UB_SHAPE_CORNER_WEDGE) {
        c.x = v3_neg(c.x);
        c.z = v3_neg(c.z);
    }
    p->rot = c;
    p->pos = ub_rbx_vec(pos);
}

vec3 ub_part_get_rbx_size(const ub_part *p)
{
    if (p->shape == UB_SHAPE_CYLINDER) return v3(p->size.y, p->size.x, p->size.z);
    return p->size;
}

void ub_part_set_rbx_size(ub_part *p, vec3 s)
{
    s = v3(s.x < 0.001f ? 0.001f : s.x, s.y < 0.001f ? 0.001f : s.y, s.z < 0.001f ? 0.001f : s.z);
    if (s.x > 2048) s.x = 2048;
    if (s.y > 2048) s.y = 2048;
    if (s.z > 2048) s.z = 2048;
    if (p->shape == UB_SHAPE_CYLINDER) {
        f32 d = s.y < s.z ? s.y : s.z;
        p->size = v3(d, s.x, d);
    } else if (p->shape == UB_SHAPE_BALL) {
        f32 d = s.x < s.y ? s.x : s.y;
        d = d < s.z ? d : s.z;
        p->size = v3(d, d, d);
    } else {
        p->size = s;
    }
}

vec3 ub_rbx_orientation(const mat3 *r)
{
    f32 s = ub_clampf(-r->z.y, -1, 1);
    f32 x = ub_atan2f(s, ub_sqrtf(1 - s * s)), y, z;
    if (ub_absf(r->z.y) < 0.9999999f) {
        y = ub_atan2f(r->z.x, r->z.z);
        z = ub_atan2f(r->x.y, r->y.y);
    } else {
        y = ub_atan2f(-r->x.z, r->x.x);
        z = 0;
    }
    const f32 d = 180.0f / UB_PI;
    return v3(x * d, y * d, z * d);
}

mat3 ub_rbx_from_orientation(vec3 o)
{
    const f32 k = UB_PI / 180.0f;
    f32 cx = ub_cosf(o.x * k), sx = ub_sinf(o.x * k), cy = ub_cosf(o.y * k), sy = ub_sinf(o.y * k);
    f32 cz = ub_cosf(o.z * k), sz = ub_sinf(o.z * k);
    mat3 m; /* Ry * Rx * Rz, as columns */
    m.x = v3(cy * cz + sy * sx * sz, cx * sz, -sy * cz + cy * sx * sz);
    m.y = v3(-cy * sz + sy * sx * cz, cx * cz, sy * sz + cy * sx * cz);
    m.z = v3(sy * cx, -sx, cy * cx);
    return m;
}
