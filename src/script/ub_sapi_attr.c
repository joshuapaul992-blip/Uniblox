/* Uniblox - attributes and CollectionService tags (milestone 8B).
 *
 * Both live in the world as typed properties, so they come from place
 * files, are saved in .ubx and are copied by Clone: attribute "Name" is
 * the property "@Name"; an instance's tags are the property "Tags", the
 * names separated by NULs (as Roblox files store them). */
#include <stdio.h>
#include <string.h>

#include "script/ub_sapi.h"

#define W (&S.g->world)

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* ----------------------------------------------------------- attributes */

/* Roblox's rule: letters, digits and underscores, at most 100, and not
 * "RBX..." (reserved). */
static void check_attr_name(lua_State *L, const char *n)
{
    size_t len = strlen(n);
    bool ok = len > 0 && len <= 100 && strncmp(n, "RBX", 3) != 0;
    for (size_t i = 0; ok && i < len; ++i) {
        char c = n[i];
        ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }
    if (!ok)
        sapi_error(L, "Attribute name '%s' is not valid: use at most 100 letters, digits and underscores, "
                      "not starting with RBX", n);
}

static u32 attr_key(lua_State *L, const char *n, bool make)
{
    char k[104];
    snprintf(k, sizeof k, "@%s", n);
    u32 key = make ? ub_world_key(W, k) : ub_world_find_key(W, k);
    if (make && key == UB_NO_STRING) luaL_error(L, "out of memory");
    return key;
}

static i32 attr_target(lua_State *L)
{
    i32 i = check_inst(L, 1);
    if (i < 0) sapi_error(L, "attributes are not available on %s", ub_typeof(L, 1));
    return i;
}

int attr_get(lua_State *L)
{
    i32 i = attr_target(L);
    u32 key = attr_key(L, luaL_checkstring(L, 2), false);
    const ub_value *v = key == UB_NO_STRING ? NULL : ub_world_prop(W, i, key);
    if (v) push_pvalue(L, v, -1);
    else lua_pushnil(L);
    return 1;
}

/* the type an attribute of this Lua value has; UB_PV_NONE if none can */
static u8 attr_type_of(lua_State *L, int i)
{
    switch (lua_type(L, i)) {
    case LUA_TBOOLEAN: return UB_PV_BOOL;
    case LUA_TNUMBER: return UB_PV_DOUBLE;
    case LUA_TSTRING: return UB_PV_STRING;
    case LUA_TUSERDATA: {
        static const struct { const char *t; u8 pv; } k[] = {
            { "UDim", UB_PV_UDIM }, { "UDim2", UB_PV_UDIM2 }, { "BrickColor", UB_PV_BRICKCOLOR },
            { "Color3", UB_PV_COLOR3 }, { "Vector2", UB_PV_VECTOR2 }, { "Vector3", UB_PV_VECTOR3 },
            { "NumberSequence", UB_PV_NUMSEQ }, { "ColorSequence", UB_PV_COLORSEQ },
            { "NumberRange", UB_PV_NUMRANGE }, { "Rect", UB_PV_RECT }, { "Font", UB_PV_FONT },
        };
        const char *t = ub_typeof(L, i);
        for (size_t j = 0; j < sizeof k / sizeof k[0]; ++j)
            if (streq(k[j].t, t)) return k[j].pv;
        return UB_PV_NONE;
    }
    default: return UB_PV_NONE;
    }
}

static void attr_changed(lua_State *L, i32 i, const char *n)
{
    char k[104];
    snprintf(k, sizeof k, "@%s", n);
    lua_pushstring(L, n);
    fire_signal(L, i, EV_ATTRCHANGED, NULL, 1);
    fire_signal(L, i, EV_PROPCHANGED, k, 0);
}

int attr_set(lua_State *L)
{
    i32 i = attr_target(L);
    const char *n = luaL_checkstring(L, 2);
    check_attr_name(L, n);
    lua_settop(L, 3);
    if (lua_isnil(L, 3)) {
        u32 key = attr_key(L, n, false);
        if (key == UB_NO_STRING || !ub_world_prop(W, i, key)) return 0;
        ub_world_remove_prop(W, i, key);
        attr_changed(L, i, n);
        return 0;
    }
    u8 t = attr_type_of(L, 3);
    if (t == UB_PV_NONE) return sapi_error(L, "%s is not a supported attribute type", ub_typeof(L, 3));
    ub_value v;
    f32 seq[UB_SEQ_MAX_KEYS * 4];
    const char *err;
    if (!to_pvalue(L, 3, t, -1, &v, seq, &err)) return sapi_error(L, "%s", err ? err : "bad attribute value");
    u32 key = attr_key(L, n, true);
    const ub_value *old = ub_world_prop(W, i, key);
    if (old && ub_value_equal(old, &v)) return 0;
    if (!ub_world_set_prop(W, i, key, &v)) return luaL_error(L, "out of memory");
    attr_changed(L, i, n);
    return 0;
}

int attr_getall(lua_State *L)
{
    i32 i = attr_target(L);
    lua_newtable(L);
    for (int k = 0, n = ub_world_prop_count(W, i); k < n; ++k) {
        const ub_prop *p = ub_world_prop_at(W, i, k);
        const char *name = ub_world_key_name(W, p->key);
        if (!name || name[0] != '@') continue;
        lua_pushstring(L, name + 1);
        push_pvalue(L, &p->val, -1);
        lua_rawset(L, -3);
    }
    return 1;
}

int attr_signal(lua_State *L)
{
    i32 i = attr_target(L);
    char k[104];
    snprintf(k, sizeof k, "@%s", luaL_checkstring(L, 2));
    push_signal(L, i, EV_PROPCHANGED, k);
    return 1;
}

/* ----------------------------------------------------------------- tags */

static int g_tag_signals; /* InstanceAdded/Removed signals made: only then are moves watched */

static const ub_value *tags_of(i32 i)
{
    if (i < 0 || !ub_world_alive(W, i)) return NULL;
    const ub_value *v = ub_world_prop_named(W, i, "Tags");
    return v && v->type == UB_PV_STRING && v->v.str.len ? v : NULL;
}

/* calls fn(tag, len, ud) for each tag; stops when it returns false */
static void each_tag(const ub_value *v, bool (*fn)(const char *, u32, void *), void *ud)
{
    if (!v) return;
    const char *s = v->v.str.s, *e = s + v->v.str.len;
    while (s < e) {
        const char *z = s;
        while (z < e && *z) z++;
        if (z > s && !fn(s, (u32)(z - s), ud)) return;
        s = z + 1;
    }
}

typedef struct { const char *tag; u32 len; bool found; } tag_find;

static bool find_fn(const char *t, u32 n, void *ud)
{
    tag_find *f = (tag_find *)ud;
    if (n == f->len && memcmp(t, f->tag, n) == 0) f->found = true;
    return !f->found;
}

static bool has_tag(i32 i, const char *tag)
{
    tag_find f = { tag, (u32)strlen(tag), false };
    each_tag(tags_of(i), find_fn, &f);
    return f.found;
}

bool sapi_in_game(i32 i)
{
    if (i < 0) return i == VIRT_GAME;
    if (!ub_world_alive(W, i)) return false;
    while (W->inst[i].parent >= 0) i = W->inst[i].parent;
    return !(W->inst[i].flags & UB_INST_DETACHED);
}

static void fire_tag(lua_State *L, i32 i, const char *tag, int ev)
{
    if (!g_tag_signals || !sapi_in_game(i)) return;
    push_inst(L, i);
    fire_signal(L, VIRT_COLLECTION, ev, tag, 1);
}

static void add_tag(lua_State *L, i32 i, const char *tag)
{
    if (i < 0) sapi_error(L, "tags are not available on this object");
    if (has_tag(i, tag)) return;
    const ub_value *old = tags_of(i);
    u32 ol = old ? old->v.str.len : 0, tl = (u32)strlen(tag);
    char stack[256], *b = ol + tl + 1 <= sizeof stack ? stack : (char *)lua_newuserdata(L, ol + tl + 1);
    if (ol) {
        memcpy(b, old->v.str.s, ol);
        b[ol++] = 0;
    }
    memcpy(b + ol, tag, tl);
    ub_value v = ub_vstr(b, ol + tl);
    if (!ub_world_set_prop_named(W, i, "Tags", &v)) luaL_error(L, "out of memory");
    fire_tag(L, i, tag, EV_TAGADDED);
}

typedef struct { char *out; u32 n; const char *skip; u32 skip_len; } tag_copy;

static bool copy_fn(const char *t, u32 n, void *ud)
{
    tag_copy *c = (tag_copy *)ud;
    if (n == c->skip_len && memcmp(t, c->skip, n) == 0) return true;
    if (c->n) c->out[c->n++] = 0;
    memcpy(c->out + c->n, t, n);
    c->n += n;
    return true;
}

static void remove_tag(lua_State *L, i32 i, const char *tag)
{
    if (i < 0 || !has_tag(i, tag)) return;
    const ub_value *old = tags_of(i);
    tag_copy c = { (char *)lua_newuserdata(L, old->v.str.len + 1), 0, tag, (u32)strlen(tag) };
    each_tag(old, copy_fn, &c);
    if (c.n == 0) ub_world_remove_prop(W, i, ub_world_find_key(W, "Tags"));
    else {
        ub_value v = ub_vstr(c.out, c.n);
        if (!ub_world_set_prop_named(W, i, "Tags", &v)) luaL_error(L, "out of memory");
    }
    fire_tag(L, i, tag, EV_TAGREMOVED);
}

typedef struct { lua_State *L; int n; } tag_list;

static bool list_fn(const char *t, u32 n, void *ud)
{
    tag_list *l = (tag_list *)ud;
    lua_pushlstring(l->L, t, n);
    lua_rawseti(l->L, -2, ++l->n);
    return true;
}

static int push_tags(lua_State *L, i32 i)
{
    lua_newtable(L);
    tag_list l = { L, 0 };
    each_tag(tags_of(i), list_fn, &l);
    return 1;
}

/* instance methods: inst:AddTag(tag) ... */
int tag_has(lua_State *L) { lua_pushboolean(L, has_tag(check_inst(L, 1), luaL_checkstring(L, 2))); return 1; }
int tag_add(lua_State *L) { add_tag(L, check_inst(L, 1), luaL_checkstring(L, 2)); return 0; }
int tag_remove(lua_State *L) { remove_tag(L, check_inst(L, 1), luaL_checkstring(L, 2)); return 0; }
int tag_list_of(lua_State *L) { return push_tags(L, check_inst(L, 1)); }

/* CollectionService methods (self at 1) */
static int cs_has(lua_State *L) { lua_pushboolean(L, has_tag(check_inst(L, 2), luaL_checkstring(L, 3))); return 1; }
static int cs_add(lua_State *L) { add_tag(L, check_inst(L, 2), luaL_checkstring(L, 3)); return 0; }
static int cs_remove(lua_State *L) { remove_tag(L, check_inst(L, 2), luaL_checkstring(L, 3)); return 0; }
static int cs_gettags(lua_State *L) { return push_tags(L, check_inst(L, 2)); }

static int cs_gettagged(lua_State *L)
{
    const char *tag = luaL_checkstring(L, 2);
    lua_newtable(L);
    int n = 0;
    for (i32 i = 0; i < W->count; ++i)
        if (has_tag(i, tag) && sapi_in_game(i)) {
            push_inst(L, i);
            lua_rawseti(L, -2, ++n);
        }
    return 1;
}

typedef struct { lua_State *L; int set, n; } tag_all;

static bool all_fn(const char *t, u32 n, void *ud)
{
    tag_all *a = (tag_all *)ud;
    lua_pushlstring(a->L, t, n);
    lua_rawget(a->L, a->set);
    bool seen = lua_toboolean(a->L, -1);
    lua_pop(a->L, 1);
    if (!seen) {
        lua_pushlstring(a->L, t, n);
        lua_pushboolean(a->L, 1);
        lua_rawset(a->L, a->set);
        lua_pushlstring(a->L, t, n);
        lua_rawseti(a->L, a->set + 1, ++a->n);
    }
    return true;
}

static int cs_getalltags(lua_State *L)
{
    lua_newtable(L); /* seen */
    lua_newtable(L); /* the list */
    tag_all a = { L, lua_gettop(L) - 1, 0 };
    for (i32 i = 0; i < W->count; ++i)
        if (sapi_in_game(i)) each_tag(tags_of(i), all_fn, &a);
    return 1;
}

static int cs_signal(lua_State *L)
{
    const char *tag = luaL_checkstring(L, 2);
    g_tag_signals++;
    push_signal(L, VIRT_COLLECTION, lua_toboolean(L, lua_upvalueindex(1)) ? EV_TAGADDED : EV_TAGREMOVED, tag);
    return 1;
}

bool collection_index(lua_State *L, const char *k)
{
    static const luaL_Reg m[] = { { "HasTag", cs_has }, { "AddTag", cs_add }, { "RemoveTag", cs_remove },
                                  { "GetTags", cs_gettags }, { "GetTagged", cs_gettagged },
                                  { "GetAllTags", cs_getalltags }, { NULL, NULL } };
    for (int i = 0; m[i].name; ++i)
        if (streq(m[i].name, k)) {
            lua_pushcfunction(L, m[i].func);
            return true;
        }
    if (streq(k, "GetInstanceAddedSignal") || streq(k, "GetInstanceRemovedSignal")) {
        lua_pushboolean(L, streq(k, "GetInstanceAddedSignal"));
        lua_pushcclosure(L, cs_signal, 1);
        return true;
    }
    return false;
}

/* An instance moved in or out of the game (or is about to be destroyed):
 * its subtree's tagged instances are announced. */
void tags_moved(lua_State *L, i32 root, bool was_in, bool now_in)
{
    if (!g_tag_signals || was_in == now_in || root < 0) return;
    i32 n;
    i32 *sub = sapi_subtree(root, &n);
    if (!sub) return;
    for (i32 j = 0; j < n; ++j) {
        const ub_value *v = tags_of(sub[j]);
        if (!v) continue;
        /* copy: handlers may change the tags */
        char *copy = (char *)lua_newuserdata(L, v->v.str.len + 1);
        int slot = lua_gettop(L);
        u32 len = v->v.str.len;
        memcpy(copy, v->v.str.s, len);
        copy[len] = 0;
        for (u32 s = 0; s < len;) {
            u32 e = s;
            while (e < len && copy[e]) e++;
            if (e > s) {
                lua_pushlstring(L, copy + s, e - s);
                push_inst(L, sub[j]);
                fire_signal(L, VIRT_COLLECTION, now_in ? EV_TAGADDED : EV_TAGREMOVED, lua_tostring(L, -2), 1);
                lua_pop(L, 1);
            }
            s = e + 1;
        }
        lua_remove(L, slot);
    }
    plat_free(sub);
}
