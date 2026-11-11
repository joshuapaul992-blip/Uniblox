/* Uniblox - scripts in a networked game (milestone 9D): remotes over the
 * connection, the client's clicks, prompts and tools reaching the server,
 * the server's sounds reaching the client, and the client's scripts
 * hearing of what replication changed.
 *
 * Values cross as Roblox's do: nil, booleans, numbers, strings, Vector3,
 * Vector2, CFrame, Color3, UDim, UDim2, BrickColor, EnumItems, instances
 * the other side has (others arrive as nil) and tables of those (no
 * functions; nested at most PACK_DEPTH deep). */
#include <string.h>

#include "script/ub_sapi.h"
#include "script/lua/lualib.h"
#include "game/ub_game.h"
#include "net/ub_netbuf.h"
#include "platform/platform.h"

#define PACK_DEPTH 24
#define PACK_MAX_ITEMS 4096        /* table entries in one message */
#define MAX_ARGS 200               /* values in one call */
#define W (&S.g->world)

enum {
    SM_EVENT = 1,    /* remote net, values: FireServer / FireClient(s) */
    SM_INVOKE,       /* id, remote net, values: InvokeServer / InvokeClient */
    SM_RESULT,       /* id, ok, values */
    SM_PLAYER,       /* inst net, event: a click or a prompt (client -> server) */
    SM_TOOL,         /* action, tool net (client -> server) */
    SM_SOUND,        /* sound net, action, from (server -> client) */
};

enum { V_NIL, V_FALSE, V_TRUE, V_NUM, V_STR, V_V3, V_CF, V_C3, V_INST, V_TABLE, V_V2, V_UDIM2, V_ENUM, V_BRICK, V_UDIM };

bool sapi_networked(void) { return S.g->link && S.g->role != UB_ROLE_SOLO; }

static u32 to_net(i32 inst)
{
    if (inst < 0 || !sapi_networked()) return UB_NET_NONE;
    return S.g->link->to_net(S.g->link->ctx, inst);
}

static i32 to_local(u32 net)
{
    if (net == UB_NET_NONE || !sapi_networked()) return -1;
    i32 i = S.g->link->to_local(S.g->link->ctx, net);
    return i >= 0 && ub_world_alive(W, i) ? i : -1;
}

static void begin(ub_wbuf *b, u8 sub)
{
    ub_wbuf_init(b, NULL, 0);
    ub_w_u8(b, UB_MSG_SCRIPT);
    ub_w_u8(b, sub);
}

static void send(ub_wbuf *b)
{
    if (!b->err && sapi_networked()) S.g->link->send(S.g->link->ctx, true, b->p, b->len);
    ub_wbuf_free(b);
}

/* ------------------------------------------------------------- values */

static void pack(lua_State *L, int i, ub_wbuf *b, int depth, int *items)
{
    i = i < 0 ? lua_gettop(L) + i + 1 : i;
    int t = lua_type(L, i);
    if (++*items > PACK_MAX_ITEMS) t = LUA_TNIL;
    if (t == LUA_TBOOLEAN) {
        ub_w_u8(b, lua_toboolean(L, i) ? V_TRUE : V_FALSE);
    } else if (t == LUA_TNUMBER) {
        ub_w_u8(b, V_NUM);
        ub_w_f64(b, lua_tonumber(L, i));
    } else if (t == LUA_TSTRING) {
        size_t n;
        const char *s = lua_tolstring(L, i, &n);
        ub_w_u8(b, V_STR);
        ub_w_str(b, s, (u32)n);
    } else if (is_v3(L, i)) {
        ub_w_u8(b, V_V3);
        ub_w_vec3(b, check_v3(L, i));
    } else if (is_cf(L, i)) {
        scf *c = check_cf(L, i);
        ub_w_u8(b, V_CF);
        ub_w_vec3(b, c->p);
        ub_w_vec3(b, c->r.x);
        ub_w_vec3(b, c->r.y);
        ub_w_vec3(b, c->r.z);
    } else if (is_c3(L, i)) {
        sc3 *c = (sc3 *)luaL_checkudata(L, i, "Color3");
        ub_w_u8(b, V_C3);
        ub_w_vec3(b, v3(c->r, c->g, c->b));
    } else if (is_vector2(L, i)) {
        f32 v[2];
        get_vector2(L, i, v);
        ub_w_u8(b, V_V2);
        ub_w_f32(b, v[0]);
        ub_w_f32(b, v[1]);
    } else if (is_udim2(L, i)) {
        f32 v[4];
        get_udim2(L, i, v);
        ub_w_u8(b, V_UDIM2);
        for (int k = 0; k < 4; ++k) ub_w_f32(b, v[k]);
    } else if (is_udim(L, i)) {
        f32 v[2];
        get_udim(L, i, v);
        ub_w_u8(b, V_UDIM);
        ub_w_f32(b, v[0]);
        ub_w_f32(b, v[1]);
    } else if (is_brickcolor(L, i)) {
        ub_w_u8(b, V_BRICK);
        ub_w_u32(b, brickcolor_rgb(L, i));
    } else if (is_inst(L, i)) {
        u32 net = to_net(check_inst(L, i));
        if (net == UB_NET_NONE) {
            ub_w_u8(b, V_NIL);
        } else {
            ub_w_u8(b, V_INST);
            ub_w_varu(b, net);
        }
    } else if (t == LUA_TUSERDATA) {
        int et, ev;
        if (get_enum(L, i, &et, &ev)) {
            ub_w_u8(b, V_ENUM);
            ub_w_varu(b, (u32)et);
            ub_w_varu(b, (u32)ev);
        } else {
            ub_w_u8(b, V_NIL);
        }
    } else if (t == LUA_TTABLE && depth < PACK_DEPTH && lua_checkstack(L, 4)) {
        u32 n = 0;
        lua_pushnil(L);
        while (lua_next(L, i)) {
            lua_pop(L, 1);
            n++;
        }
        ub_w_u8(b, V_TABLE);
        ub_w_varu(b, n);
        lua_pushnil(L);
        while (lua_next(L, i)) {
            pack(L, -2, b, depth + 1, items);
            pack(L, -1, b, depth + 1, items);
            lua_pop(L, 1);
        }
    } else {
        ub_w_u8(b, V_NIL); /* functions, threads... */
    }
}

static void pack_args(lua_State *L, int first, int n, ub_wbuf *b)
{
    int items = 0;
    ub_w_varu(b, (u32)n);
    for (int k = 0; k < n; ++k) pack(L, first + k, b, 0, &items);
}

static bool unpack(lua_State *L, ub_rbuf *r, int depth)
{
    if (!lua_checkstack(L, 4)) return false;
    u8 t = ub_r_u8(r);
    switch (t) {
    case V_NIL: lua_pushnil(L); break;
    case V_FALSE: case V_TRUE: lua_pushboolean(L, t == V_TRUE); break;
    case V_NUM: lua_pushnumber(L, ub_r_f64(r)); break;
    case V_STR: {
        u32 n;
        const char *s = ub_r_str(r, &n);
        if (!s) return false;
        lua_pushlstring(L, s, n);
        break;
    }
    case V_V3: push_v3(L, ub_r_vec3(r)); break;
    case V_CF: {
        vec3 p = ub_r_vec3(r);
        mat3 m;
        m.x = ub_r_vec3(r);
        m.y = ub_r_vec3(r);
        m.z = ub_r_vec3(r);
        push_cf(L, p, &m);
        break;
    }
    case V_C3: {
        vec3 c = ub_r_vec3(r);
        push_c3(L, c.x, c.y, c.z);
        break;
    }
    case V_V2: {
        f32 x = ub_r_f32(r), y = ub_r_f32(r);
        push_vector2(L, x, y);
        break;
    }
    case V_UDIM2: {
        f32 v[4];
        for (int k = 0; k < 4; ++k) v[k] = ub_r_f32(r);
        push_udim2(L, v[0], v[1], v[2], v[3]);
        break;
    }
    case V_UDIM: {
        f32 s = ub_r_f32(r), o = ub_r_f32(r);
        push_udim(L, s, o);
        break;
    }
    case V_BRICK: push_brickcolor(L, ub_r_u32(r)); break;
    case V_INST: push_inst(L, to_local(ub_r_varu(r))); break;
    case V_ENUM: {
        u32 et = ub_r_varu(r), ev = ub_r_varu(r);
        if (r->err || et > 4096 || ev > 65535) return false;
        push_enum(L, (int)et, (int)ev);
        break;
    }
    case V_TABLE: {
        u32 n = ub_r_varu(r);
        if (r->err || depth >= PACK_DEPTH || n > PACK_MAX_ITEMS) return false;
        lua_createtable(L, 0, (int)n);
        for (u32 k = 0; k < n; ++k) {
            if (!unpack(L, r, depth + 1) || !unpack(L, r, depth + 1)) return false;
            if (lua_isnil(L, -2)) lua_pop(L, 2); /* (a key the other side lost) */
            else lua_rawset(L, -3);
        }
        break;
    }
    default: return false;
    }
    return !r->err;
}

/* pushes the values; returns how many, -1 if malformed (nothing left pushed) */
static int unpack_args(lua_State *L, ub_rbuf *r)
{
    u32 n = ub_r_varu(r);
    if (r->err || n > MAX_ARGS || !lua_checkstack(L, (int)n + 8)) return -1;
    int top = lua_gettop(L);
    for (u32 k = 0; k < n; ++k)
        if (!unpack(L, r, 0)) {
            lua_settop(L, top);
            return -1;
        }
    return (int)n;
}

/* ------------------------------------------------------------ remotes */

void net_fire_remote(lua_State *L, i32 remote, int first, int n)
{
    u32 net = to_net(remote);
    if (net == UB_NET_NONE) return; /* (the other side cannot see it) */
    ub_wbuf b;
    begin(&b, SM_EVENT);
    ub_w_varu(&b, net);
    pack_args(L, first, n, &b);
    send(&b);
}

static void push_invokes(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "ub.invokes");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "ub.invokes");
    }
}

/* netinvoke(remote, ...): sends the call and waits; returns ok, values... */
int net_invoke(lua_State *L)
{
    i32 remote = check_inst(L, 1);
    u32 net = to_net(remote);
    if (net == UB_NET_NONE) return luaL_error(L, "the other side has no %s", ub_world_str(W, W->inst[remote].name));
    u32 id = ++S.next_invoke;
    ub_wbuf b;
    begin(&b, SM_INVOKE);
    ub_w_varu(&b, id);
    ub_w_varu(&b, net);
    pack_args(L, 2, lua_gettop(L) - 1, &b);
    send(&b);
    int ref;
    int r = yield_parked(L, &ref);
    push_invokes(L);
    lua_pushinteger(L, ref);
    lua_rawseti(L, -2, (int)id);
    lua_pop(L, 1);
    return r;
}

/* netreply(id, ok, ...): the answer to an invoke */
static int net_reply(lua_State *L)
{
    u32 id = (u32)luaL_checkinteger(L, 1);
    ub_wbuf b;
    begin(&b, SM_RESULT);
    ub_w_varu(&b, id);
    bool ok = lua_toboolean(L, 2);
    ub_w_u8(&b, ok ? 1 : 0);
    if (ok) {
        pack_args(L, 3, lua_gettop(L) - 2, &b);
    } else { /* the error, as text */
        const char *m = lua_tostring(L, 3);
        lua_pushstring(L, m ? m : "error in the invoked function");
        pack_args(L, lua_gettop(L), 1, &b);
    }
    send(&b);
    return 0;
}

static int l_networked(lua_State *L)
{
    lua_pushboolean(L, sapi_networked());
    return 1;
}

/* the Lua side's helpers (see k_gui_lua in ub_sapi_gui.c) */
void net_open(lua_State *L, int table)
{
    lua_pushcfunction(L, net_invoke);
    lua_setfield(L, table, "netinvoke");
    lua_pushcfunction(L, net_reply);
    lua_setfield(L, table, "netreply");
    lua_pushcfunction(L, l_networked);
    lua_setfield(L, table, "networked");
}

/* ------------------------------------------ clicks, prompts, tools, sounds */

void net_player_event(i32 inst, int ev)
{
    u32 net = to_net(inst);
    if (net == UB_NET_NONE) return;
    ub_wbuf b;
    begin(&b, SM_PLAYER);
    ub_w_varu(&b, net);
    ub_w_u8(&b, (u8)ev);
    send(&b);
}

void net_tool(int action, i32 tool)
{
    u32 net = tool >= 0 ? to_net(tool) : UB_NET_NONE;
    if (tool >= 0 && net == UB_NET_NONE) return;
    ub_wbuf b;
    begin(&b, SM_TOOL);
    ub_w_u8(&b, (u8)action);
    ub_w_varu(&b, net == UB_NET_NONE ? 0 : net + 1);
    send(&b);
}

void net_sound(i32 inst, int action, f64 from)
{
    u32 net = to_net(inst);
    if (net == UB_NET_NONE) return;
    ub_wbuf b;
    begin(&b, SM_SOUND);
    ub_w_varu(&b, net);
    ub_w_u8(&b, (u8)action);
    ub_w_f64(&b, from);
    send(&b);
}

/* ---------------------------------------------------------- receiving */

typedef struct {
    const u8 *msg;
    u32 len;
} smsg;

static int e_message(lua_State *L)
{
    smsg *m = (smsg *)lua_touserdata(L, 1);
    lua_settop(L, 0);
    ub_rbuf r;
    ub_rbuf_init(&r, m->msg, m->len);
    ub_r_u8(&r); /* UB_MSG_SCRIPT */
    u8 sub = ub_r_u8(&r);
    bool server = S.g->role == UB_ROLE_SERVER;
    switch (sub) {
    case SM_EVENT: {
        i32 remote = to_local(ub_r_varu(&r));
        if (r.err || remote < 0) break;
        if (server) push_inst(L, S.g->player);
        int n = unpack_args(L, &r);
        if (n < 0) break;
        fire_signal(L, remote, server ? EV_SERVEREVENT : EV_CLIENTEVENT, NULL, n + (server ? 1 : 0));
        break;
    }
    case SM_INVOKE: {
        u32 id = ub_r_varu(&r);
        i32 remote = to_local(ub_r_varu(&r));
        if (r.err) break;
        lua_getfield(L, LUA_REGISTRYINDEX, "ub.GuiLua");
        if (!lua_istable(L, -1)) break;
        lua_getfield(L, -1, "serve");
        lua_remove(L, -2);
        lua_pushinteger(L, (lua_Integer)id);
        push_inst(L, remote);
        lua_pushstring(L, server ? "OnServerInvoke" : "OnClientInvoke");
        if (server) push_inst(L, S.g->player);
        int n = unpack_args(L, &r);
        if (n < 0) break;
        spawn_now(L, 3 + (server ? 1 : 0) + n);
        break;
    }
    case SM_RESULT: {
        u32 id = ub_r_varu(&r);
        bool ok = ub_r_u8(&r) != 0;
        if (r.err) break;
        push_invokes(L);
        lua_rawgeti(L, -1, (int)id);
        int ref = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : LUA_NOREF;
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_rawseti(L, -2, (int)id);
        lua_pop(L, 1);
        if (ref == LUA_NOREF) break;
        lua_pushboolean(L, ok);
        int n = unpack_args(L, &r);
        if (n < 0) {
            lua_pop(L, 1);
            lua_pushboolean(L, 0);
            lua_pushstring(L, "a malformed answer");
            n = 1;
        }
        resume_ref(L, ref, n + 1);
        break;
    }
    case SM_PLAYER: {
        i32 inst = to_local(ub_r_varu(&r));
        int ev = ub_r_u8(&r);
        if (!r.err && inst >= 0 && server) interact_remote_event(L, inst, ev);
        break;
    }
    case SM_TOOL: {
        int action = ub_r_u8(&r);
        u32 n = ub_r_varu(&r);
        if (r.err || !server) break;
        interact_remote_tool(L, action, n ? to_local(n - 1) : -1);
        break;
    }
    case SM_SOUND: {
        i32 inst = to_local(ub_r_varu(&r));
        int action = ub_r_u8(&r);
        f64 from = ub_r_f64(&r);
        if (!r.err && inst >= 0 && !server) sound_remote(L, inst, action, from);
        break;
    }
    default: break;
    }
    return 0;
}

void net_message(lua_State *L, const u8 *msg, u32 len)
{
    smsg m = { msg, len };
    lua_pushcfunction(L, e_message);
    lua_pushlightuserdata(L, &m);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        const char *e = lua_tostring(L, -1);
        script_output(e ? e : "error handling a message", true);
        lua_pop(L, 1);
    }
}

/* ------------------------------------------------ what replication changed */

static void fire_tree(lua_State *L, i32 i)
{
    i32 np = W->inst[i].parent;
    if (np >= 0) {
        push_inst(L, i);
        fire_signal(L, np, EV_CHILDADDED, NULL, 1);
        for (i32 a = np; a >= 0 && ub_world_alive(W, i); a = W->inst[a].parent) {
            push_inst(L, i);
            fire_signal(L, a, EV_DESCENDANTADDED, NULL, 1);
        }
    }
    if (ub_world_alive(W, i)) fire_signal(L, i, EV_ANCESTRYCHANGED, NULL, 0);
}

static void key_changed(lua_State *L, i32 i, u32 key)
{
    const char *name = ub_world_str(W, key);
    if (!name || !name[0]) return;
    if (name[0] == '~') name++; /* (a value the scripts keep, see ub_sapi_inst.c) */
    prop_changed(L, i, name);
}

static int e_replicated(lua_State *L)
{
    ub_journal *j = W->journal;
    if (!j || !j->n) return 0;
    /* a copy: handlers change the world (and the journal) as they run */
    i32 n = j->n, nk = j->nk;
    i32 *list = (i32 *)plat_alloc(sizeof(i32) * (size_t)n);
    u8 *flags = (u8 *)plat_alloc((size_t)n);
    ub_jkey *keys = nk ? (ub_jkey *)plat_alloc(sizeof(ub_jkey) * (size_t)nk) : NULL;
    if (!list || !flags || (nk && !keys)) {
        plat_free(list);
        plat_free(flags);
        plat_free(keys);
        return 0;
    }
    for (i32 k = 0; k < n; ++k) {
        list[k] = j->list[k];
        flags[k] = j->flags[list[k]];
    }
    if (nk) memcpy(keys, j->keys, sizeof(ub_jkey) * (size_t)nk);
    ub_world_journal_clear(W);
    /* instances made or moved: signals, then the scripts in them start */
    for (i32 k = 0; k < n; ++k) {
        i32 i = list[k];
        if (!ub_world_alive(W, i) || !(flags[k] & (UB_JN_NEW | UB_JN_TREE))) continue;
        if (flags[k] & UB_JN_NEW) tags_moved(L, i, false, sapi_in_game(i));
        fire_tree(L, i);
    }
    for (i32 k = 0; k < n; ++k) {
        i32 i = list[k];
        if (!ub_world_alive(W, i) || !(flags[k] & (UB_JN_NEW | UB_JN_TREE))) continue;
        if ((flags[k] & UB_JN_TREE) || W->inst[i].source != UB_NO_STRING) start_scripts_in(i);
    }
    /* properties */
    for (i32 k = 0; k < n; ++k) {
        i32 i = list[k];
        if (!ub_world_alive(W, i) || (flags[k] & UB_JN_NEW)) continue;
        if (flags[k] & UB_JN_NAME) prop_changed(L, i, "Name");
        if (flags[k] & UB_JN_PROPS) {
            int np = ub_world_prop_count(W, i);
            for (int p = 0; p < np && ub_world_alive(W, i); ++p) key_changed(L, i, ub_world_prop_at(W, i, p)->key);
        }
    }
    u8 *fresh = nk ? (u8 *)plat_alloc((size_t)W->count + 1) : NULL; /* (made in this batch: no Changed) */
    if (fresh) {
        memset(fresh, 0, (size_t)W->count + 1);
        for (i32 k = 0; k < n; ++k)
            if ((flags[k] & UB_JN_NEW) && list[k] >= 0 && list[k] < W->count) fresh[list[k]] = 1;
        for (i32 k = 0; k < nk; ++k) {
            i32 i = keys[k].inst;
            if (i >= 0 && i < W->count && !fresh[i] && ub_world_alive(W, i)) key_changed(L, i, keys[k].key);
        }
        plat_free(fresh);
    }
    /* joints made or changed join their parts */
    for (i32 k = 0; k < n; ++k) {
        i32 i = list[k];
        if (ub_world_alive(W, i) && is_joint_class(ub_world_str(W, W->inst[i].class_name))) joint_changed(L, i);
    }
    plat_free(list);
    plat_free(flags);
    plat_free(keys);
    return 0;
}

void net_replicated(lua_State *L)
{
    lua_pushcfunction(L, e_replicated);
    if (lua_pcall(L, 0, 0, 0) != 0) {
        const char *e = lua_tostring(L, -1);
        script_output(e ? e : "error after replication", true);
        lua_pop(L, 1);
    }
}
