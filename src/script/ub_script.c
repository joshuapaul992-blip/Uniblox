/* Uniblox - the script engine: one sandboxed Lua state, a scheduler for
 * waiting threads, signals, and script start-up. The API objects live in
 * ub_sapi_types.c (Vector3, CFrame...) and ub_sapi_inst.c (instances). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "script/ub_sapi.h"
#include "script/lua/lualib.h"
#include "core/ub_log.h"
#include "core/ub_string.h"
#include "platform/platform.h"

ub_sstate S;
char REG_INSTANCES, REG_SIGNALS, REG_WAITERS, REG_PROPS, REG_SCRIPTS, REG_THREAD_SCRIPT, REG_MODULES, REG_BASE,
    REG_TWEENS;
static char REG_SHARED, REG_STOPPED, REG_PCALL;

#define HOOK_EVERY 10000 /* instructions between time-limit checks */

/* --------------------------------------------------------------- memory */

static void *l_alloc(void *ud, void *p, size_t osize, size_t nsize)
{
    (void)ud;
    if (nsize == 0) {
        if (p) {
            plat_free(p);
            S.mem_used -= osize;
        }
        return NULL;
    }
    if (nsize > osize && S.mem_used + (nsize - osize) > UB_SCRIPT_MEMORY) { /* the scripts' budget */
        /* the script that ran out is stopped (see hook), so it cannot keep
         * all the memory to itself by catching the error */
        if (S.cur_script >= 0 && !S.timed_out) {
            S.timed_out = true;
            S.out_of_memory = true;
        }
        return NULL;
    }
    void *np = plat_alloc(nsize);
    if (!np) return NULL;
    if (p) {
        memcpy(np, p, osize < nsize ? osize : nsize);
        plat_free(p);
    }
    S.mem_used += nsize - osize;
    return np;
}

static int l_panic(lua_State *L)
{
    ub_error("scripts: unrecoverable Lua error: %s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    return 0;
}

/* --------------------------------------------------------------- output */

static struct {
    char text[UB_SCRIPT_OUTPUT_LINES][160];
    bool err[UB_SCRIPT_OUTPUT_LINES];
    int count, next;
} g_out;

void script_output(const char *text, bool is_error)
{
    if (is_error) ub_warn("script: %s", text);
    else ub_info("script: %s", text);
    /* one overlay line per text line */
    const char *p = text;
    do {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        if (n > 159) n = 159;
        memcpy(g_out.text[g_out.next], p, (size_t)n);
        g_out.text[g_out.next][n] = 0;
        g_out.err[g_out.next] = is_error;
        g_out.next = (g_out.next + 1) % UB_SCRIPT_OUTPUT_LINES;
        if (g_out.count < UB_SCRIPT_OUTPUT_LINES) g_out.count++;
        p = e ? e + 1 : NULL;
    } while (p && *p);
}

int ub_script_output(const char **lines, bool *is_error, int max)
{
    int n = g_out.count < max ? g_out.count : max;
    for (int i = 0; i < n; ++i) {
        int k = (g_out.next - n + i + UB_SCRIPT_OUTPUT_LINES) % UB_SCRIPT_OUTPUT_LINES;
        lines[i] = g_out.text[k];
        is_error[i] = g_out.err[k];
    }
    return n;
}

static void lua_stdout(const char *line, int is_error) { script_output(line, is_error != 0); }

int sapi_error(lua_State *L, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    luaL_where(L, 1);
    lua_pushstring(L, buf);
    lua_concat(L, 2);
    return lua_error(L);
}

/* ----------------------------------------------------------- time limit */

static void hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (!S.timed_out && plat_time_us() - S.resume_start <= UB_SCRIPT_TIME_LIMIT_US) return;
    if (!S.timed_out) {
        S.timed_out = true;
        /* from now on every instruction of this thread errors, so pcall
         * cannot catch the error and carry on looping */
        lua_sethook(L, hook, LUA_MASKCOUNT, 1);
    }
    if (S.out_of_memory) luaL_error(L, "not enough memory: the scripts' memory is used up");
    luaL_error(L, "script timeout: it ran for too long without waiting");
}

/* --------------------------------------------------------------- scripts */

static const char *script_name(lua_State *L, i32 script, char *buf, int cap)
{
    if (script < 0 || script >= S.g->world.count) return "?";
    /* Workspace.Model.Script */
    i32 chain[32];
    int n = 0;
    for (i32 i = script; i >= 0 && n < 32; i = S.g->world.inst[i].parent) chain[n++] = i;
    int len = 0;
    buf[0] = 0;
    for (int k = n - 1; k >= 0 && len < cap - 2; --k) {
        const char *nm = ub_world_str(&S.g->world, S.g->world.inst[chain[k]].name);
        len += snprintf(buf + len, (size_t)(cap - len), "%s%s", k == n - 1 ? "" : ".", nm);
    }
    (void)L;
    return buf;
}

static bool script_stopped(lua_State *L, i32 script)
{
    if (script < 0) return false;
    lua_pushlightuserdata(L, &REG_STOPPED);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, -1, script);
    bool s = lua_toboolean(L, -1);
    lua_pop(L, 2);
    return s;
}

/* ------------------------------------------------------------ scheduler */

typedef struct {
    int ref;          /* the thread, in the registry */
    f64 wake;         /* game time to resume at */
    f64 since;        /* when it started waiting (wait() returns the time waited) */
    i32 script;
    int nargs;        /* first run: arguments already on its stack */
    u8 first;         /* 1: first run (function + args on its stack); 0: resume from wait() */
} sched;

static sched *g_q;
static int g_qn, g_qcap;
static int g_depth;   /* nested resumes (a script spawning another thread) */

f64 script_time(void) { return (f64)S.g->time; }

static bool q_add(int ref, f64 wake, f64 since, i32 script, int nargs, u8 first)
{
    if (g_qn == g_qcap) {
        int nc = g_qcap ? g_qcap * 2 : 64;
        sched *nq = (sched *)plat_alloc(sizeof(sched) * (size_t)nc);
        if (!nq) return false;
        if (g_q) {
            memcpy(nq, g_q, sizeof(sched) * (size_t)g_qn);
            plat_free(g_q);
        }
        g_q = nq;
        g_qcap = nc;
    }
    sched *e = &g_q[g_qn++];
    e->ref = ref;
    e->wake = wake;
    e->since = since;
    e->script = script;
    e->nargs = nargs;
    e->first = first;
    return true;
}

/* Resumes a thread; reports errors; stops the script if it ran too long. */
static void run_thread(lua_State *L, lua_State *co, int nargs, i32 script)
{
    i32 saved_script = S.cur_script;
    bool saved_timeout = S.timed_out;
    if (g_depth == 0) {
        S.resume_start = plat_time_us();
        S.timed_out = false;
        S.out_of_memory = false;
    }
    lua_sethook(co, hook, LUA_MASKCOUNT, HOOK_EVERY); /* a stopped thread's hook may be set to every instruction */
    S.cur_script = script;
    g_depth++;
    int r = lua_resume(co, nargs);
    g_depth--;
    bool timed_out = S.timed_out;
    if (timed_out && g_depth == 0) {
        /* the error may have been caught by a pcall: the script is stopped all the same */
        char name[192], line[300];
        if (r == 0 || r == LUA_YIELD) S.stats.errors++;
        snprintf(line, sizeof line, "%s was stopped: %s", script_name(L, script, name, sizeof name),
                 S.out_of_memory ? "it used up the scripts' memory" : "it ran for too long without waiting");
        if (r != 0 && r != LUA_YIELD) {
            const char *msg = lua_tostring(co, -1);
            script_output(msg ? msg : "(error object is not a string)", true);
        }
        S.stats.stopped++;
        script_output(line, true);
        stop_script(script, NULL);
        S.need_gc = true;
    } else if (r != 0 && r != LUA_YIELD) {
        char name[192];
        const char *msg = lua_tostring(co, -1);
        char line[400];
        snprintf(line, sizeof line, "%s", msg ? msg : "(error object is not a string)");
        S.stats.errors++;
        script_output(line, true);
        (void)name;
    }
    S.cur_script = saved_script;
    if (g_depth == 0) S.timed_out = S.out_of_memory = false;
    else S.timed_out = saved_timeout || timed_out;
    /* Lua 5.1 has no emergency collection: after running out, collect now
     * (the failed thread's garbage), or every script would keep failing */
    if (r == LUA_ERRMEM || (r != 0 && r != LUA_YIELD && S.mem_used > UB_SCRIPT_MEMORY / 2)) S.need_gc = true;
    (void)saved_timeout;
}

/* A full collection when one is owed, at a point where no thread runs. */
static void collect_if_owed(lua_State *L)
{
    if (!S.need_gc || g_depth > 0) return;
    S.need_gc = false;
    lua_gc(L, LUA_GCCOLLECT, 0);
}

static void run_ref(lua_State *L, int ref, int nargs_on_stack, i32 script)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref); /* stays on the stack: a running thread is not a GC root */
    lua_State *co = lua_tothread(L, -1);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    if (co && !script_stopped(L, script)) run_thread(L, co, nargs_on_stack, script);
    lua_pop(L, 1);
}

void spawn_now(lua_State *L, int nargs)
{
    lua_State *co = lua_newthread(L);
    lua_insert(L, -(nargs + 2));
    lua_xmove(L, co, nargs + 1);
    /* the thread belongs to the current script */
    lua_pushlightuserdata(L, &REG_THREAD_SCRIPT);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, -2);
    lua_pushinteger(L, S.cur_script);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_pushvalue(L, -1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX); /* pops the copy; the thread stays on top */
    run_ref(L, ref, nargs, S.cur_script);
}

void spawn_later(lua_State *L, int nargs, f64 delay)
{
    lua_State *co = lua_newthread(L);
    lua_insert(L, -(nargs + 2));
    lua_xmove(L, co, nargs + 1);
    lua_pushvalue(L, -1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX); /* the thread stays on top */
    f64 now = script_time();
    q_add(ref, now + (delay > 0 ? delay : 0), now, S.cur_script, nargs, 1);
}

/* Pushes the thread the scheduler should wake for L: L itself, or, when L
 * is the coroutine of a pcall (see k_pcall), the thread that called it.
 * Yields inside pcall pass up through it, so waking that thread resumes
 * them. */
static void push_outer_thread(lua_State *L)
{
    lua_pushthread(L);
    lua_pushlightuserdata(L, &REG_PCALL);
    lua_rawget(L, LUA_REGISTRYINDEX);
    for (int guard = 0; guard < 256; ++guard) {
        lua_pushvalue(L, -2);
        lua_rawget(L, -2);
        if (!lua_isthread(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        lua_replace(L, -3);
    }
    lua_pop(L, 1);
}

int yield_for(lua_State *L, f64 delay)
{
    if (L == S.L) return luaL_error(L, "wait can only be called from a script");
    f64 now = script_time();
    push_outer_thread(L);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    /* wait() and wait(0) still give way for a frame, as in Roblox */
    q_add(ref, now + (delay > 0 ? delay : 0), now, S.cur_script, 0, 0);
    return lua_yield(L, 0);
}

int yield_parked(lua_State *L, int *ref_out)
{
    if (L == S.L) return luaL_error(L, "this can only be called from a script");
    push_outer_thread(L);
    lua_pushlightuserdata(L, &REG_THREAD_SCRIPT);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, -2);
    lua_pushinteger(L, S.cur_script);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    *ref_out = luaL_ref(L, LUA_REGISTRYINDEX);
    return lua_yield(L, 0);
}

/* Resumes a parked thread with the nargs values on top of L. */
void resume_ref(lua_State *L, int ref, int nargs)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_State *co = lua_tothread(L, -1);
    i32 script = -1;
    if (co) {
        lua_pushlightuserdata(L, &REG_THREAD_SCRIPT);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushvalue(L, -2);
        lua_rawget(L, -2);
        script = lua_isnumber(L, -1) ? (i32)lua_tointeger(L, -1) : -1;
        lua_pop(L, 2);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    if (!co || lua_status(co) != LUA_YIELD || script_stopped(L, script)) {
        lua_pop(L, nargs + 1);
        return;
    }
    lua_insert(L, -(nargs + 1)); /* keep the thread below its arguments while it runs */
    lua_xmove(L, co, nargs);
    run_thread(L, co, nargs, script);
    lua_pop(L, 1);
}

static void run_due(lua_State *L)
{
    f64 now = script_time();
    int n = g_qn;
    /* take the due entries out first: resuming may add more */
    int due = 0;
    for (int i = 0; i < n; ++i) due += g_q[i].wake <= now;
    if (!due) return;
    sched *list = (sched *)plat_alloc(sizeof(sched) * (size_t)due);
    if (!list) return;
    int k = 0, keep = 0;
    for (int i = 0; i < g_qn; ++i) {
        if (i < n && g_q[i].wake <= now) list[k++] = g_q[i];
        else g_q[keep++] = g_q[i];
    }
    g_qn = keep;
    for (int i = 0; i < k; ++i) {
        sched *e = &list[i];
        if (e->first) {
            run_ref(L, e->ref, e->nargs, e->script);
        } else {
            lua_rawgeti(L, LUA_REGISTRYINDEX, e->ref);
            lua_State *co = lua_tothread(L, -1);
            lua_pop(L, 1);
            if (co && lua_status(co) == LUA_YIELD) {
                lua_pushnumber(co, now - e->since); /* wait() returns the time waited */
                lua_pushnumber(co, now);
                run_ref(L, e->ref, 2, e->script);
            } else {
                luaL_unref(L, LUA_REGISTRYINDEX, e->ref);
            }
        }
    }
    plat_free(list);
}

void stop_script(i32 script, const char *why)
{
    lua_State *L = S.L;
    if (script < 0) return;
    if (why) script_output(why, true);
    lua_pushlightuserdata(L, &REG_STOPPED);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushboolean(L, 1);
    lua_rawseti(L, -2, script);
    lua_pop(L, 1);
    /* its waiting threads never resume */
    int keep = 0;
    for (int i = 0; i < g_qn; ++i) {
        if (g_q[i].script == script) luaL_unref(L, LUA_REGISTRYINDEX, g_q[i].ref);
        else g_q[keep++] = g_q[i];
    }
    g_qn = keep;
    /* and its connections go: every signal list drops entries of this script */
    lua_pushlightuserdata(L, &REG_SIGNALS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        /* value: array of connection records {fn, id, script, once} */
        int len = (int)lua_objlen(L, -1), w = 1;
        for (int i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            lua_rawgeti(L, -1, 3);
            i32 s = (i32)lua_tointeger(L, -1);
            lua_pop(L, 1);
            if (s != script) {
                lua_rawseti(L, -2, w++);
            } else {
                lua_pop(L, 1);
            }
        }
        for (int i = w; i <= len; ++i) {
            lua_pushnil(L);
            lua_rawseti(L, -2, i);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/* ------------------------------------------------------------- signals */

typedef struct {
    i32 inst;
    i32 ev;
    char prop[32];
} ssignal;

typedef struct {
    i32 inst;
    i32 ev;
    char prop[32];
    u32 id;
} sconn;

/* the signal's key in REG_SIGNALS / REG_WAITERS */
static void push_key(lua_State *L, i32 inst, int ev, const char *prop)
{
    if (prop && prop[0]) lua_pushfstring(L, "%d:%s", (int)inst, prop);
    else lua_pushnumber(L, ((f64)inst + 1000000.0) * 64.0 + ev);
}

void push_signal(lua_State *L, i32 inst, int ev, const char *prop)
{
    ssignal *s = (ssignal *)lua_newuserdata(L, sizeof(ssignal));
    s->inst = inst;
    s->ev = ev;
    snprintf(s->prop, sizeof s->prop, "%s", prop ? prop : "");
    luaL_getmetatable(L, "RBXScriptSignal");
    lua_setmetatable(L, -2);
}

static void touch_watch(i32 inst, int delta)
{
    if (inst < 0 || inst >= S.g->world.count) return;
    if (inst >= S.touch_watch_cap) {
        i32 nc = S.g->world.count + 256;
        u8 *n = (u8 *)plat_alloc((size_t)nc);
        if (!n) return;
        if (S.touch_watch) {
            memcpy(n, S.touch_watch, (size_t)S.touch_watch_cap);
            plat_free(S.touch_watch);
        }
        S.touch_watch = n;
        S.touch_watch_cap = nc;
    }
    int v = S.touch_watch[inst] + delta;
    S.touch_watch[inst] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
    if (S.g->world.inst[inst].part >= 0) ub_game_part_changed(S.g, S.g->world.inst[inst].part, UB_CHANGE_PHYS);
}

/* Forgets everything kept about these instances (marked[i] != 0): their
 * connections and waiting threads, extra properties, script state. For
 * Destroy (which disconnects, as in Roblox) and for reused slots. */
void forget_instances(lua_State *L, const u8 *marked, i32 n)
{
    char *const tables[] = { &REG_SIGNALS, &REG_WAITERS };
    for (int t = 0; t < 2; ++t) {
        lua_pushlightuserdata(L, tables[t]);
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            lua_pop(L, 1);
            i32 inst = -1;
            if (lua_type(L, -1) == LUA_TNUMBER) {
                inst = (i32)floor(lua_tonumber(L, -1) / 64.0) - 1000000;
            } else if (lua_type(L, -1) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -1);
                int v = 0, sign = 1;
                if (*k == '-') { sign = -1; ++k; }
                while (*k >= '0' && *k <= '9') v = v * 10 + (*k++ - '0');
                inst = v * sign;
            }
            if (inst >= 0 && inst < n && marked[inst]) { /* clearing a field while traversing is allowed */
                lua_pushvalue(L, -1);
                lua_pushnil(L);
                lua_rawset(L, -4);
            }
        }
        lua_pop(L, 1);
    }
    char *const by_index[] = { &REG_PROPS, &REG_STOPPED };
    for (int t = 0; t < 2; ++t) {
        lua_pushlightuserdata(L, by_index[t]);
        lua_rawget(L, LUA_REGISTRYINDEX);
        for (i32 i = 0; i < n; ++i)
            if (marked[i]) {
                lua_pushnil(L);
                lua_rawseti(L, -2, i);
            }
        lua_pop(L, 1);
    }
    for (i32 i = 0; i < n; ++i) {
        if (!marked[i]) continue;
        if (i < S.touch_watch_cap) S.touch_watch[i] = 0;
        if (i < S.started_cap) S.started[i] = 0;
    }
}

bool ub_script_watches_touch(i32 inst)
{
    return inst >= 0 && inst < S.touch_watch_cap && S.touch_watch[inst] > 0;
}

static int sig_connect_common(lua_State *L, bool once)
{
    ssignal *s = (ssignal *)luaL_checkudata(L, 1, "RBXScriptSignal");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushlightuserdata(L, &REG_SIGNALS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    push_key(L, s->inst, s->ev, s->prop);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        push_key(L, s->inst, s->ev, s->prop);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    u32 id = ++S.next_conn_id;
    lua_createtable(L, 4, 0); /* {fn, id, script, once} */
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, id);
    lua_rawseti(L, -2, 2);
    lua_pushinteger(L, S.cur_script);
    lua_rawseti(L, -2, 3);
    lua_pushboolean(L, once);
    lua_rawseti(L, -2, 4);
    lua_rawseti(L, -2, (int)lua_objlen(L, -2) + 1);
    if (s->ev == EV_TOUCHED) touch_watch(s->inst, 1);
    sconn *c = (sconn *)lua_newuserdata(L, sizeof(sconn));
    c->inst = s->inst;
    c->ev = s->ev;
    memcpy(c->prop, s->prop, sizeof c->prop);
    c->id = id;
    luaL_getmetatable(L, "RBXScriptConnection");
    lua_setmetatable(L, -2);
    return 1;
}

static int sig_connect(lua_State *L) { return sig_connect_common(L, false); }
static int sig_once(lua_State *L) { return sig_connect_common(L, true); }

/* Removes connection `id` from its list; returns whether it was there. */
static bool disconnect(lua_State *L, i32 inst, int ev, const char *prop, u32 id)
{
    bool found = false;
    lua_pushlightuserdata(L, &REG_SIGNALS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    push_key(L, inst, ev, prop);
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        int len = (int)lua_objlen(L, -1), w = 1;
        for (int i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            lua_rawgeti(L, -1, 2);
            u32 cid = (u32)lua_tonumber(L, -1);
            lua_pop(L, 1);
            if (cid == id) {
                found = true;
                lua_pop(L, 1);
            } else {
                lua_rawseti(L, -2, w++);
            }
        }
        for (int i = w; i <= len; ++i) {
            lua_pushnil(L);
            lua_rawseti(L, -2, i);
        }
    }
    lua_pop(L, 2);
    if (found && ev == EV_TOUCHED) touch_watch(inst, -1);
    return found;
}

static int conn_disconnect(lua_State *L)
{
    sconn *c = (sconn *)luaL_checkudata(L, 1, "RBXScriptConnection");
    if (c->id) disconnect(L, c->inst, c->ev, c->prop, c->id);
    c->id = 0;
    return 0;
}

static bool conn_alive(lua_State *L, const sconn *c)
{
    bool found = false;
    lua_pushlightuserdata(L, &REG_SIGNALS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    push_key(L, c->inst, c->ev, c->prop);
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        int len = (int)lua_objlen(L, -1);
        for (int i = 1; i <= len && !found; ++i) {
            lua_rawgeti(L, -1, i);
            lua_rawgeti(L, -1, 2);
            found = (u32)lua_tonumber(L, -1) == c->id;
            lua_pop(L, 2);
        }
    }
    lua_pop(L, 2);
    return found;
}

static int conn_index(lua_State *L)
{
    sconn *c = (sconn *)luaL_checkudata(L, 1, "RBXScriptConnection");
    const char *k = luaL_checkstring(L, 2);
    if (strcmp(k, "Disconnect") == 0 || strcmp(k, "disconnect") == 0) {
        lua_pushcfunction(L, conn_disconnect);
        return 1;
    }
    if (strcmp(k, "Connected") == 0) {
        lua_pushboolean(L, c->id && conn_alive(L, c));
        return 1;
    }
    return sapi_error(L, "%s is not a valid member of RBXScriptConnection", k);
}

static int sig_wait(lua_State *L)
{
    ssignal *s = (ssignal *)luaL_checkudata(L, 1, "RBXScriptSignal");
    int ref;
    lua_pushlightuserdata(L, &REG_WAITERS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    push_key(L, s->inst, s->ev, s->prop);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        push_key(L, s->inst, s->ev, s->prop);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    int list = lua_gettop(L);
    /* park this thread; its reference goes in the waiters list */
    if (L == S.L) return luaL_error(L, "Wait can only be called from a script");
    lua_pushthread(L);
    lua_pushlightuserdata(L, &REG_THREAD_SCRIPT);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, -2);
    lua_pushinteger(L, S.cur_script);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushinteger(L, ref);
    lua_rawseti(L, list, (int)lua_objlen(L, list) + 1);
    lua_settop(L, 0);
    return lua_yield(L, 0);
}

static int sig_index(lua_State *L)
{
    luaL_checkudata(L, 1, "RBXScriptSignal");
    const char *k = luaL_checkstring(L, 2);
    if (strcmp(k, "Connect") == 0 || strcmp(k, "connect") == 0 || strcmp(k, "ConnectParallel") == 0)
        lua_pushcfunction(L, sig_connect);
    else if (strcmp(k, "Once") == 0)
        lua_pushcfunction(L, sig_once);
    else if (strcmp(k, "Wait") == 0 || strcmp(k, "wait") == 0)
        lua_pushcfunction(L, sig_wait);
    else
        return sapi_error(L, "%s is not a valid member of RBXScriptSignal", k);
    return 1;
}

static int sig_tostring(lua_State *L)
{
    lua_pushstring(L, "Signal");
    return 1;
}

bool signal_has_handlers(lua_State *L, i32 inst, int ev)
{
    bool any = false;
    lua_pushlightuserdata(L, &REG_SIGNALS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    push_key(L, inst, ev, NULL);
    lua_rawget(L, -2);
    any = lua_istable(L, -1) && lua_objlen(L, -1) > 0;
    lua_pop(L, 2);
    if (any) return true;
    lua_pushlightuserdata(L, &REG_WAITERS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    push_key(L, inst, ev, NULL);
    lua_rawget(L, -2);
    any = lua_istable(L, -1) && lua_objlen(L, -1) > 0;
    lua_pop(L, 2);
    return any;
}

void fire_signal(lua_State *L, i32 inst, int ev, const char *prop, int nargs)
{
    int args = lua_gettop(L) - nargs + 1;
    /* handlers: work from a copy of the list (a handler may connect or
     * disconnect while we go) */
    lua_pushlightuserdata(L, &REG_SIGNALS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    push_key(L, inst, ev, prop);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    int n = lua_istable(L, -1) ? (int)lua_objlen(L, -1) : 0;
    lua_createtable(L, n, 0);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -2, i);
        lua_rawseti(L, -2, i);
    }
    lua_remove(L, -2);
    int copy = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, copy, i);
        int rec = lua_gettop(L);
        lua_rawgeti(L, rec, 3);
        i32 script = (i32)lua_tointeger(L, -1);
        lua_rawgeti(L, rec, 4);
        bool once = lua_toboolean(L, -1);
        lua_pop(L, 2);
        if (script_stopped(L, script)) {
            lua_pop(L, 1);
            continue;
        }
        if (once) {
            lua_rawgeti(L, rec, 2);
            disconnect(L, inst, ev, prop, (u32)lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        lua_rawgeti(L, rec, 1);
        lua_remove(L, rec);
        for (int a = 0; a < nargs; ++a) lua_pushvalue(L, args + a);
        i32 saved = S.cur_script;
        S.cur_script = script;
        spawn_now(L, nargs);
        S.cur_script = saved;
        lua_settop(L, copy);
    }
    lua_settop(L, copy - 1);
    /* threads parked in :Wait() */
    lua_pushlightuserdata(L, &REG_WAITERS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    int waiters = lua_gettop(L);
    push_key(L, inst, ev, prop);
    lua_rawget(L, waiters);
    int len = lua_istable(L, -1) ? (int)lua_objlen(L, -1) : 0;
    int *refs = len ? (int *)plat_alloc(sizeof(int) * (size_t)len) : NULL;
    for (int i = 1; i <= len && refs; ++i) {
        lua_rawgeti(L, -1, i);
        refs[i - 1] = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (len) { /* clear the list before resuming: they may wait again */
        push_key(L, inst, ev, prop);
        lua_pushnil(L);
        lua_rawset(L, waiters);
    }
    lua_pop(L, 1);
    for (int i = 0; i < len && refs; ++i) {
        for (int a = 0; a < nargs; ++a) lua_pushvalue(L, args + a);
        resume_ref(L, refs[i], nargs);
    }
    plat_free(refs);
    lua_settop(L, args - 1); /* pop the arguments */
}

void open_signals(lua_State *L)
{
    luaL_newmetatable(L, "RBXScriptSignal");
    lua_pushcfunction(L, sig_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, sig_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    luaL_newmetatable(L, "RBXScriptConnection");
    lua_pushcfunction(L, conn_index);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
}

/* ------------------------------------------------------------- globals */

/* print's formatting: the arguments through tostring, space-separated. */
static const char *join_args(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_Buffer b;
    lua_getglobal(L, "tostring");
    int ts = lua_gettop(L);
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        lua_pushvalue(L, ts);
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        if (!lua_isstring(L, -1)) return luaL_error(L, "'tostring' must return a string to 'print'"), "";
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    return lua_tostring(L, -1);
}

static int g_print(lua_State *L)
{
    script_output(join_args(L), false);
    return 0;
}

static int g_warn(lua_State *L)
{
    script_output(join_args(L), true);
    return 0;
}

static int g_wait(lua_State *L)
{
    f64 t = luaL_optnumber(L, 1, 0.03);
    if (!(t >= 0)) t = 0;
    return yield_for(L, t);
}

static int g_spawn(lua_State *L) /* spawn(fn): runs it on the next frame */
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_settop(L, 1);
    spawn_later(L, 0, 0);
    return 0;
}

static int g_delay(lua_State *L)
{
    f64 t = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_settop(L, 2);
    spawn_later(L, 0, t);
    return 0;
}

static int t_spawn(lua_State *L) /* task.spawn(fn, ...): runs it now */
{
    int n = lua_gettop(L);
    if (lua_type(L, 1) == LUA_TTHREAD) { /* resume an existing thread */
        lua_State *co = lua_tothread(L, 1);
        lua_xmove(L, co, n - 1);
        lua_pushvalue(L, 1);
        run_thread(L, co, n - 1, S.cur_script);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TFUNCTION);
    spawn_now(L, n - 1);
    return 0;
}

static int t_defer(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    spawn_later(L, lua_gettop(L) - 1, 0);
    return 0;
}

static int t_delay(lua_State *L)
{
    f64 t = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_remove(L, 1);
    spawn_later(L, lua_gettop(L) - 1, t);
    return 1; /* the thread, for task.cancel */
}

static int t_wait(lua_State *L)
{
    f64 t = luaL_optnumber(L, 1, 0);
    if (!(t >= 0)) t = 0;
    return yield_for(L, t);
}

static int t_cancel(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTHREAD);
    lua_State *co = lua_tothread(L, 1);
    /* drop any scheduled resume of it */
    int keep = 0;
    for (int i = 0; i < g_qn; ++i) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_q[i].ref);
        bool same = lua_tothread(L, -1) == co;
        lua_pop(L, 1);
        if (same) luaL_unref(L, LUA_REGISTRYINDEX, g_q[i].ref);
        else g_q[keep++] = g_q[i];
    }
    g_qn = keep;
    return 0;
}

static int g_tick(lua_State *L)
{
    lua_pushnumber(L, (f64)plat_time_us() * 1e-6);
    return 1;
}

static int g_time(lua_State *L)
{
    lua_pushnumber(L, script_time());
    return 1;
}

static int g_typeof(lua_State *L)
{
    luaL_checkany(L, 1);
    lua_pushstring(L, ub_typeof(L, 1));
    return 1;
}

static int g_collectgarbage(lua_State *L)
{
    const char *opt = luaL_optstring(L, 1, "count");
    if (strcmp(opt, "count") != 0) return luaL_error(L, "collectgarbage can only count");
    lua_pushnumber(L, (f64)S.mem_used / 1024.0);
    return 1;
}

static int os_time(lua_State *L)
{
    ub_datetime t;
    plat_local_time(&t);
    /* days since 1970 (civil from days, Howard Hinnant) */
    int y = t.year, m = t.month, d = t.day;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    f64 days = (f64)era * 146097 + doe - 719468;
    lua_pushnumber(L, days * 86400 + t.hour * 3600 + t.minute * 60 + t.second);
    return 1;
}

static int os_clock(lua_State *L)
{
    lua_pushnumber(L, (f64)plat_time_us() * 1e-6);
    return 1;
}

static int os_difftime(lua_State *L)
{
    lua_pushnumber(L, luaL_checknumber(L, 1) - luaL_optnumber(L, 2, 0));
    return 1;
}

/* require(ModuleScript): runs it once, in its own globals, and returns
 * what it returned. */
static void make_env(lua_State *L, i32 script);

static int g_require(lua_State *L)
{
    i32 m = check_inst(L, 1);
    if (m < 0 || strcmp(ub_world_str(&S.g->world, S.g->world.inst[m].class_name), "ModuleScript") != 0)
        return sapi_error(L, "require: expected a ModuleScript (loading by asset id is not available)");
    lua_pushlightuserdata(L, &REG_MODULES);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, -1, m);
    if (lua_istable(L, -1) && lua_toboolean(L, -1)) { /* cached: {value} or {loading = true} */
        lua_getfield(L, -1, "loading");
        if (lua_toboolean(L, -1)) return sapi_error(L, "require: a ModuleScript requires itself (loop)");
        lua_pop(L, 1);
        lua_rawgeti(L, -1, 1);
        return 1;
    }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "loading");
    lua_rawseti(L, -2, m);
    const char *src = ub_world_source(&S.g->world, m);
    char name[192];
    char chunk[200];
    snprintf(chunk, sizeof chunk, "=%s", script_name(L, m, name, sizeof name));
    if (src[0] == 27) return sapi_error(L, "require: precompiled code is not allowed");
    if (luaL_loadbuffer(L, src, strlen(src), chunk) != 0) return lua_error(L);
    make_env(L, m);
    lua_setfenv(L, -2);
    if (lua_pcall(L, 0, 1, 0) != 0) {
        const char *msg = lua_tostring(L, -1);
        return sapi_error(L, "Requested module experienced an error while loading: %s", msg ? msg : "?");
    }
    lua_newtable(L);
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, 1);
    lua_rawseti(L, -3, m);
    return 1;
}

/* small helpers written in Lua: Roblox's additions to the standard tables */
static const char k_prelude[] =
    "local string, table, math, select, type, pairs = string, table, math, select, type, pairs\n"
    "function table.find(t, v, init) for i = init or 1, #t do if t[i] == v then return i end end return nil end\n"
    "function table.clear(t) for k in pairs(t) do t[k] = nil end end\n"
    "function table.pack(...) return { n = select('#', ...), ... } end\n"
    "table.unpack = unpack\n"
    "function table.create(n, v) local t = {} if v ~= nil then for i = 1, n do t[i] = v end end return t end\n"
    "function table.freeze(t) return t end\n"
    "function table.isfrozen() return false end\n"
    "function table.clone(t) local c = {} for k, v in pairs(t) do c[k] = v end return c end\n"
    "function string.split(s, sep)\n"
    "  sep = sep or ','\n"
    "  local out, i = {}, 1\n"
    "  if sep == '' then for c in s:gmatch('.') do out[#out + 1] = c end return out end\n"
    "  while true do\n"
    "    local a, b = s:find(sep, i, true)\n"
    "    if not a then out[#out + 1] = s:sub(i) break end\n"
    "    out[#out + 1] = s:sub(i, a - 1)\n"
    "    i = b + 1\n"
    "  end\n"
    "  return out\n"
    "end\n"
    "function math.clamp(x, lo, hi) if x < lo then return lo elseif x > hi then return hi end return x end\n"
    "function math.sign(x) if x > 0 then return 1 elseif x < 0 then return -1 end return 0 end\n"
    "function math.round(x) if x >= 0 then return math.floor(x + 0.5) end return math.ceil(x - 0.5) end\n"
    "function math.noise(x, y, z)\n"
    "  local n = math.sin((x or 0) * 12.9898 + (y or 0) * 78.233 + (z or 0) * 37.719) * 43758.5453\n"
    "  return (n - math.floor(n)) - 0.5\n"
    "end\n";

/* pcall and xpcall that let the function wait, as Roblox's do (Lua 5.1's
 * cannot yield across them): the function runs as a coroutine, and its
 * yields are passed on to whoever resumes this thread. */
static const char k_pcall[] =
    "local create, resume, status, yield, mark, unpack, select, type = ...\n"
    "local function pack(...) return { n = select('#', ...), ... } end\n"
    "local function drive(co, handler, ...)\n"
    "  local r = pack(resume(co, ...))\n"
    "  while true do\n"
    "    if not r[1] then\n"
    "      if handler then return false, handler(r[2]) end\n"
    "      return false, r[2]\n"
    "    end\n"
    "    if status(co) == 'dead' then return unpack(r, 1, r.n) end\n"
    "    r = pack(resume(co, yield(unpack(r, 2, r.n))))\n"
    "  end\n"
    "end\n"
    "local function start(f)\n"
    "  if type(f) ~= 'function' then local g = f f = function(...) return g(...) end end\n"
    "  local co = create(f)\n"
    "  mark(co)\n"
    "  return co\n"
    "end\n"
    "local function pcall(f, ...) return drive(start(f), nil, ...) end\n"
    "local function xpcall(f, handler, ...) return drive(start(f), handler, ...) end\n"
    "return pcall, xpcall\n";

/* mark(co): co is a pcall's coroutine, called from the running thread. */
static int c_mark_pcall(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTHREAD);
    lua_pushlightuserdata(L, &REG_PCALL);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 1);
    lua_pushthread(L);
    lua_rawset(L, -3);
    return 0;
}

/* coroutine.running(): inside a pcall, the thread that called it */
static int c_running(lua_State *L)
{
    push_outer_thread(L);
    if (lua_tothread(L, -1) == S.L) lua_pushnil(L);
    return 1;
}

/* The globals every script sees (read through each script's own table). */
static void build_base(lua_State *L)
{
    /* the standard libraries go into the real globals first */
    lua_pushcfunction(L, luaopen_base);
    lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_string);
    lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_table);
    lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_math);
    lua_call(L, 0, 0);
    lua_settop(L, 0);
    /* no bytecode: string.dump goes */
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    /* lock the string metatable (else one script could change every string's methods) */
    lua_pushliteral(L, "");
    lua_getmetatable(L, -1);
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 2);
    /* Roblox's additions, written in Lua */
    if (luaL_loadbuffer(L, k_prelude, sizeof k_prelude - 1, "=prelude") == 0) lua_call(L, 0, 0);
    else ub_error("scripts: prelude: %s", lua_tostring(L, -1));
    lua_settop(L, 0);

    lua_newtable(L); /* base */
    static const char *const safe[] = { "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget",
                                        "rawset", "select", "setmetatable", "getmetatable", "tonumber", "tostring",
                                        "type", "unpack", "xpcall", "_VERSION", "string", "table", "math",
                                        "coroutine", NULL };
    for (int i = 0; safe[i]; ++i) {
        lua_getglobal(L, safe[i]);
        lua_setfield(L, -2, safe[i]);
    }
    static const luaL_Reg funcs[] = { { "print", g_print }, { "warn", g_warn }, { "wait", g_wait },
                                      { "spawn", g_spawn }, { "delay", g_delay }, { "tick", g_tick },
                                      { "time", g_time }, { "elapsedTime", g_time }, { "typeof", g_typeof },
                                      { "collectgarbage", g_collectgarbage }, { "require", g_require },
                                      { NULL, NULL } };
    for (int i = 0; funcs[i].name; ++i) {
        lua_pushcfunction(L, funcs[i].func);
        lua_setfield(L, -2, funcs[i].name);
    }
    lua_newtable(L);
    static const luaL_Reg task[] = { { "spawn", t_spawn }, { "defer", t_defer }, { "delay", t_delay },
                                     { "wait", t_wait }, { "cancel", t_cancel }, { NULL, NULL } };
    for (int i = 0; task[i].name; ++i) {
        lua_pushcfunction(L, task[i].func);
        lua_setfield(L, -2, task[i].name);
    }
    lua_setfield(L, -2, "task");
    lua_newtable(L);
    lua_pushcfunction(L, os_time);
    lua_setfield(L, -2, "time");
    lua_pushcfunction(L, os_clock);
    lua_setfield(L, -2, "clock");
    lua_pushcfunction(L, os_difftime);
    lua_setfield(L, -2, "difftime");
    lua_setfield(L, -2, "os");
    lua_pushliteral(L, "Lua 5.1 (Uniblox)");
    lua_setfield(L, -2, "_VERSION");
    /* pcall and xpcall that can wait */
    lua_newtable(L); /* REG_PCALL: pcall coroutine -> its caller (weak both ways: no ephemerons in 5.1) */
    lua_newtable(L);
    lua_pushliteral(L, "kv");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_pushlightuserdata(L, &REG_PCALL);
    lua_insert(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    if (luaL_loadbuffer(L, k_pcall, sizeof k_pcall - 1, "=pcall") == 0) {
        lua_getglobal(L, "coroutine");
        lua_getfield(L, -1, "create");
        lua_getfield(L, -2, "resume");
        lua_getfield(L, -3, "status");
        lua_getfield(L, -4, "yield");
        lua_remove(L, -5);
        lua_pushcfunction(L, c_mark_pcall);
        lua_getglobal(L, "unpack");
        lua_getglobal(L, "select");
        lua_getglobal(L, "type");
        lua_call(L, 8, 2);
        lua_setfield(L, -3, "xpcall");
        lua_setfield(L, -2, "pcall");
    } else {
        ub_error("scripts: pcall: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_getglobal(L, "coroutine");
    lua_pushcfunction(L, c_running);
    lua_setfield(L, -2, "running");
    lua_pop(L, 1);
    lua_pushlightuserdata(L, &REG_BASE);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
}

/* A script's own globals: copies of the library tables (so changing
 * string.upper affects only this script), `script`, and the shared _G;
 * everything else is read from the base table. */
static void make_env(lua_State *L, i32 script)
{
    lua_newtable(L);
    int env = lua_gettop(L);
    lua_pushlightuserdata(L, &REG_BASE);
    lua_rawget(L, LUA_REGISTRYINDEX);
    int base = lua_gettop(L);
    static const char *const libs[] = { "string", "table", "math", "coroutine", "os", "task", NULL };
    for (int i = 0; libs[i]; ++i) {
        lua_getfield(L, base, libs[i]);
        lua_newtable(L);
        lua_pushnil(L);
        while (lua_next(L, -3)) {
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, -4);
        }
        lua_setfield(L, env, libs[i]);
        lua_pop(L, 1);
    }
    push_inst(L, script);
    lua_setfield(L, env, "script");
    lua_pushlightuserdata(L, &REG_SHARED);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, -1);
    lua_setfield(L, env, "_G");
    lua_setfield(L, env, "shared");
    lua_newtable(L); /* metatable: read the base, locked */
    lua_pushvalue(L, base);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, env);
    lua_settop(L, env);
}

/* ------------------------------------------------------- running scripts */

static bool mark_started(i32 inst)
{
    if (inst >= S.started_cap) {
        i32 nc = S.g->world.count + 256;
        u8 *n = (u8 *)plat_alloc((size_t)nc);
        if (!n) return false;
        if (S.started) {
            memcpy(n, S.started, (size_t)S.started_cap);
            plat_free(S.started);
        }
        S.started = n;
        S.started_cap = nc;
    }
    if (S.started[inst]) return false;
    S.started[inst] = 1;
    return true;
}

/* Compiles and runs code as script `inst` (its first run, until it yields). */
static bool start_code(lua_State *L, i32 inst, const char *src, const char *chunk)
{
    if (src[0] == 27) { /* precompiled bytecode: never */
        script_output("precompiled code is not allowed in scripts", true);
        return false;
    }
    if (luaL_loadbuffer(L, src, strlen(src), chunk) != 0) {
        S.stats.errors++;
        script_output(lua_tostring(L, -1), true);
        lua_pop(L, 1);
        return false;
    }
    make_env(L, inst);
    lua_setfenv(L, -2);
    S.stats.scripts++;
    i32 saved = S.cur_script;
    S.cur_script = inst;
    spawn_now(L, 0);
    S.cur_script = saved;
    return true;
}

static bool runs_here(const ub_world *w, i32 i, i32 sss)
{
    const ub_instance *in = &w->inst[i];
    if (in->source == UB_NO_STRING || (in->flags & (UB_INST_DISABLED | UB_INST_DESTROYED))) return false;
    const char *cls = ub_world_str(w, in->class_name);
    if (strcmp(cls, "Script") != 0 || (in->flags & UB_INST_CLIENT)) return false;
    return ub_world_is_descendant(w, i, w->workspace) || (sss >= 0 && ub_world_is_descendant(w, i, sss));
}

static i32 find_service(const ub_world *w, const char *cls)
{
    for (i32 i = 0; i < w->count; ++i)
        if (w->inst[i].parent < 0 && strcmp(ub_world_str(w, w->inst[i].class_name), cls) == 0) return i;
    return -1;
}

static int start_in(lua_State *L)
{
    i32 root = (i32)lua_tointeger(L, 1);
    ub_world *w = &S.g->world;
    i32 sss = find_service(w, "ServerScriptService");
    for (i32 i = 0; i < w->count; ++i) {
        if (root >= 0 && !ub_world_is_descendant(w, i, root)) continue;
        if (!runs_here(w, i, sss) || !mark_started(i)) continue;
        char name[192], chunk[200];
        snprintf(chunk, sizeof chunk, "=%s", script_name(L, i, name, sizeof name));
        start_code(L, i, ub_world_source(w, i), chunk);
    }
    return 0;
}

void start_scripts_in(i32 inst)
{
    /* called from inside running code: already protected */
    lua_pushcfunction(S.L, start_in);
    lua_pushinteger(S.L, inst);
    lua_call(S.L, 1, 0);
}

/* ----------------------------------------------------------- entry points */

static void report(int r)
{
    if (r != 0) {
        const char *m = lua_tostring(S.L, -1);
        S.stats.errors++;
        script_output(m ? m : "script engine error", true);
        lua_pop(S.L, 1);
        if (r == LUA_ERRMEM) S.need_gc = true;
    }
    collect_if_owed(S.L);
}

bool ub_script_init(ub_game *g)
{
    memset(&S, 0, sizeof S);
    memset(&g_out, 0, sizeof g_out);
    g_qn = 0;
    g_depth = 0;
    S.g = g;
    S.cur_script = -1;
    ubc_set_output(lua_stdout);
    lua_State *L = lua_newstate(l_alloc, NULL);
    if (!L) return false;
    S.L = L;
    lua_atpanic(L, l_panic);
    static char *const regs[] = { &REG_INSTANCES, &REG_SIGNALS, &REG_WAITERS, &REG_PROPS, &REG_SCRIPTS,
                                  &REG_THREAD_SCRIPT, &REG_MODULES, &REG_SHARED, &REG_STOPPED, &REG_TWEENS };
    for (int i = 0; i < (int)(sizeof regs / sizeof regs[0]); ++i) {
        lua_pushlightuserdata(L, regs[i]);
        lua_newtable(L);
        if (regs[i] == &REG_INSTANCES || regs[i] == &REG_THREAD_SCRIPT) { /* weak: let unused ones go */
            lua_newtable(L);
            lua_pushstring(L, regs[i] == &REG_INSTANCES ? "v" : "k");
            lua_setfield(L, -2, "__mode");
            lua_setmetatable(L, -2);
        }
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    build_base(L);
    open_signals(L);
    open_types(L);
    open_instances(L);
    /* every thread made from now on inherits the time-limit hook */
    lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_EVERY);
    return true;
}

void ub_script_shutdown(void)
{
    if (S.L) lua_close(S.L);
    free_instances(); /* after closing: the collector queues instances */
    plat_free(g_q);
    g_q = NULL;
    g_qn = g_qcap = 0;
    plat_free(S.touch_watch);
    plat_free(S.started);
    ubc_set_output(NULL);
    memset(&S, 0, sizeof S);
}

static int e_start(lua_State *L)
{
    ub_world *w = &S.g->world;
    i32 sss = find_service(w, "ServerScriptService");
    for (i32 i = 0; i < w->count; ++i) {
        const ub_instance *in = &w->inst[i];
        if (in->source != UB_NO_STRING && (in->flags & UB_INST_CLIENT)) S.stats.skipped_client++;
        if (!runs_here(w, i, sss) || !mark_started(i)) continue;
        char name[192], chunk[200];
        snprintf(chunk, sizeof chunk, "=%s", script_name(L, i, name, sizeof name));
        start_code(L, i, ub_world_source(w, i), chunk);
    }
    if (S.stats.skipped_client)
        ub_info("scripts: %u LocalScript(s) not run (scripts for the player's side come later)", S.stats.skipped_client);
    /* the player joins */
    push_inst(L, S.g->player);
    fire_signal(L, S.g->players, EV_PLAYERADDED, NULL, 1);
    push_inst(L, S.g->char_model);
    fire_signal(L, S.g->player, EV_CHARACTERADDED, NULL, 1);
    return 0;
}

void ub_script_start(void)
{
    report(lua_cpcall(S.L, e_start, NULL));
    ub_info("scripts: %u started, %u errors", S.stats.scripts, S.stats.errors);
}

typedef struct {
    const char *name, *source;
    i32 parent;
    bool ok;
} run_args;

static int e_run(lua_State *L)
{
    run_args *a = (run_args *)lua_touserdata(L, 1);
    ub_world *w = &S.g->world;
    i32 s = ub_world_add(w, a->parent, UB_CLASS_OTHER, "Script", a->name);
    if (s < 0) return luaL_error(L, "out of memory");
    ub_world_set_source(w, s, a->source);
    mark_started(s);
    char name[192], chunk[200];
    snprintf(chunk, sizeof chunk, "=%s", script_name(L, s, name, sizeof name));
    a->ok = start_code(L, s, ub_world_source(w, s), chunk);
    return 0;
}

bool ub_script_run(const char *name, const char *source, i32 parent)
{
    run_args a = { name, source, parent, false };
    report(lua_cpcall(S.L, e_run, &a));
    return a.ok;
}

static f32 g_frame_dt;

static int e_begin(lua_State *L)
{
    run_due(L);
    lua_pushnumber(L, script_time());
    lua_pushnumber(L, g_frame_dt);
    fire_signal(L, VIRT_RUNSERVICE, EV_STEPPED, NULL, 2);
    return 0;
}

void ub_script_begin_frame(f32 dt)
{
    g_frame_dt = dt;
    report(lua_cpcall(S.L, e_begin, NULL));
}

static int e_end(lua_State *L)
{
    f64 now = script_time();
    update_tweens(L, now);
    update_debris(L, now);
    reclaim_instances(L);
    lua_pushnumber(L, g_frame_dt);
    fire_signal(L, VIRT_RUNSERVICE, EV_HEARTBEAT, NULL, 1);
    lua_pushnumber(L, g_frame_dt);
    fire_signal(L, VIRT_RUNSERVICE, EV_RENDERSTEPPED, NULL, 1);
    S.stats.threads = (u32)g_qn;
    S.stats.memory_kb = (u32)(S.mem_used / 1024);
    return 0;
}

void ub_script_end_frame(f32 dt)
{
    g_frame_dt = dt;
    report(lua_cpcall(S.L, e_end, NULL));
}

static i32 g_touch_a, g_touch_b;
static int e_touched(lua_State *L)
{
    if (signal_has_handlers(L, g_touch_a, EV_TOUCHED)) {
        push_inst(L, g_touch_b);
        fire_signal(L, g_touch_a, EV_TOUCHED, NULL, 1);
    }
    if (signal_has_handlers(L, g_touch_b, EV_TOUCHED)) {
        push_inst(L, g_touch_a);
        fire_signal(L, g_touch_b, EV_TOUCHED, NULL, 1);
    }
    return 0;
}

void ub_script_touched(i32 a, i32 b)
{
    if (!ub_script_watches_touch(a) && !ub_script_watches_touch(b)) return; /* the usual case: nobody listens */
    g_touch_a = a;
    g_touch_b = b;
    report(lua_cpcall(S.L, e_touched, NULL));
}

static int e_died(lua_State *L)
{
    lua_pushnumber(L, 0);
    fire_signal(L, S.g->humanoid, EV_HEALTHCHANGED, NULL, 1);
    fire_signal(L, S.g->humanoid, EV_DIED, NULL, 0);
    return 0;
}

void ub_script_died(void) { report(lua_cpcall(S.L, e_died, NULL)); }

static int e_respawned(lua_State *L)
{
    push_inst(L, S.g->char_model);
    fire_signal(L, S.g->player, EV_CHARACTERADDED, NULL, 1);
    return 0;
}

void ub_script_respawned(void) { report(lua_cpcall(S.L, e_respawned, NULL)); }

static int e_chatted(lua_State *L)
{
    lua_pushstring(L, (const char *)lua_touserdata(L, 1));
    fire_signal(L, S.g->player, EV_CHATTED, NULL, 1);
    return 0;
}

void ub_script_chatted(const char *message)
{
    if (S.L) report(lua_cpcall(S.L, e_chatted, (void *)message));
}

void ub_script_get_stats(ub_script_stats *out)
{
    *out = S.stats;
    out->threads = (u32)g_qn;
    out->memory_kb = (u32)(S.mem_used / 1024);
}

void open_scheduler(lua_State *L) { (void)L; }
