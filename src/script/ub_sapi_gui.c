/* Uniblox - scripts and the GUI: Roblox's 2D value types (UDim, UDim2,
 * Vector2, Rect, NumberRange, NumberSequence, ColorSequence, Font,
 * InputObject), the properties of classes in the class table (GUI objects,
 * value objects, remotes), the GUI's computed properties (AbsoluteSize,
 * TextBounds...), its methods and events, remotes and bindables in a
 * single-player game, UserInputService, the Mouse, the Camera, GuiService
 * and TextService. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "script/ub_sapi.h"
#include "core/ub_log.h"
#include "core/ub_math.h"
#include "gui/ub_gui.h"
#include "gui/ub_image.h"
#include "gui/ub_text.h"
#include "world/ub_classes.h"

#define W (&S.g->world)
#define GUI (S.g->gui)

static const char *const LOCKED = "The metatable is locked";

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static const char *cls_of(i32 i) { return i >= 0 ? ub_world_str(W, W->inst[i].class_name) : ""; }

static void *udata(lua_State *L, int i, const char *name)
{
    void *p = lua_touserdata(L, i);
    if (!p || !lua_getmetatable(L, i)) return NULL;
    luaL_getmetatable(L, name);
    bool same = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return same ? p : NULL;
}

static void *check_udata(lua_State *L, int i, const char *name)
{
    void *p = udata(L, i, name);
    if (!p) luaL_typerror(L, i, name);
    return p;
}

static int readonly(lua_State *L) { return sapi_error(L, "%s cannot be assigned to", luaL_optstring(L, 2, "it")); }

static void newmeta(lua_State *L, const char *name, lua_CFunction index, lua_CFunction tostr)
{
    luaL_newmetatable(L, name);
    lua_pushcfunction(L, index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, readonly);
    lua_setfield(L, -2, "__newindex");
    if (tostr) {
        lua_pushcfunction(L, tostr);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pushstring(L, LOCKED);
    lua_setfield(L, -2, "__metatable");
}

static void op(lua_State *L, const char *name, lua_CFunction f)
{
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, name);
}

/* A read-only library table in the scripts' base. */
static void lib(lua_State *L, int base, const char *name, const luaL_Reg *fns)
{
    lua_newtable(L);
    for (int i = 0; fns[i].name; ++i) {
        lua_pushcfunction(L, fns[i].func);
        lua_setfield(L, -2, fns[i].name);
    }
    lua_newtable(L);
    lua_newtable(L);
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, readonly);
    lua_setfield(L, -2, "__newindex");
    lua_pushstring(L, LOCKED);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_setfield(L, base, name);
    lua_pop(L, 1);
}

static f32 numf(lua_State *L, int i) { return (f32)luaL_checknumber(L, i); }
static f32 optf(lua_State *L, int i, f32 d) { return (f32)luaL_optnumber(L, i, d); }

/* Formats a number as Roblox prints it in tostring. */
static void fmtn(char *buf, size_t cap, f64 v) { snprintf(buf, cap, "%.14g", v); }

/* ------------------------------------------------------ enum shortcuts */

static int E_UIT, E_UIS, E_KC, E_FONT, E_FONTWEIGHT, E_FONTSTYLE, E_TWEENSTATUS, E_CAMERATYPE, E_MOUSEBEHAVIOR,
    E_FONTSIZE, E_PLATFORM;

static int ev_of(int type, const char *item)
{
    if (type < EN_COUNT) return -1;
    return ub_enum_value_of(type - EN_COUNT, item);
}

/* ---------------------------------------------------------------- UDim */

typedef struct { f32 s, o; } sudim;
typedef struct { f32 sx, ox, sy, oy; } sudim2;
typedef struct { f32 x, y; } sv2;
typedef struct { f32 x0, y0, x1, y1; } srect;
typedef struct { f32 lo, hi; } srange;
typedef struct { f32 t, v, e; } snkey;
typedef struct { f32 t, r, g, b; } sckey;
typedef struct { u32 n; snkey k[UB_SEQ_MAX_KEYS]; } snseq;
typedef struct { u32 n; sckey k[UB_SEQ_MAX_KEYS]; } scseq;
typedef struct { char family[200]; u16 weight; u8 style; } sfont;
typedef struct {
    int type, state, key;
    f32 px, py, pz, dx, dy, dz;
} sinput;

/* UDim offsets are whole numbers (int32) */
static f32 whole(f64 v) { return (f32)(v < 0 ? ceil(v - 0.5) : floor(v + 0.5)); }

void push_udim(lua_State *L, f32 s, f32 o)
{
    sudim *u = (sudim *)lua_newuserdata(L, sizeof(sudim));
    u->s = s;
    u->o = whole(o);
    luaL_getmetatable(L, "UDim");
    lua_setmetatable(L, -2);
}

bool is_udim(lua_State *L, int i) { return udata(L, i, "UDim") != NULL; }

void get_udim(lua_State *L, int i, f32 *out)
{
    sudim *u = (sudim *)check_udata(L, i, "UDim");
    out[0] = u->s;
    out[1] = u->o;
}

static int udim_new(lua_State *L)
{
    push_udim(L, optf(L, 1, 0), optf(L, 2, 0));
    return 1;
}

static int udim_index(lua_State *L)
{
    sudim *u = (sudim *)check_udata(L, 1, "UDim");
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "Scale")) lua_pushnumber(L, u->s);
    else if (streq(k, "Offset")) lua_pushinteger(L, (lua_Integer)u->o);
    else return sapi_error(L, "%s is not a valid member of UDim", k);
    return 1;
}

static int udim_tostring(lua_State *L)
{
    sudim *u = (sudim *)check_udata(L, 1, "UDim");
    char a[32];
    fmtn(a, sizeof a, u->s);
    lua_pushfstring(L, "%s, %d", a, (int)u->o);
    return 1;
}

static int udim_add(lua_State *L)
{
    sudim *a = (sudim *)check_udata(L, 1, "UDim"), *b = (sudim *)check_udata(L, 2, "UDim");
    push_udim(L, a->s + b->s, a->o + b->o);
    return 1;
}

static int udim_sub(lua_State *L)
{
    sudim *a = (sudim *)check_udata(L, 1, "UDim"), *b = (sudim *)check_udata(L, 2, "UDim");
    push_udim(L, a->s - b->s, a->o - b->o);
    return 1;
}

static int udim_unm(lua_State *L)
{
    sudim *a = (sudim *)check_udata(L, 1, "UDim");
    push_udim(L, -a->s, -a->o);
    return 1;
}

static int udim_eq(lua_State *L)
{
    sudim *a = (sudim *)udata(L, 1, "UDim"), *b = (sudim *)udata(L, 2, "UDim");
    lua_pushboolean(L, a && b && a->s == b->s && a->o == b->o);
    return 1;
}

/* --------------------------------------------------------------- UDim2 */

void push_udim2(lua_State *L, f32 sx, f32 ox, f32 sy, f32 oy)
{
    sudim2 *u = (sudim2 *)lua_newuserdata(L, sizeof(sudim2));
    u->sx = sx;
    u->ox = whole(ox);
    u->sy = sy;
    u->oy = whole(oy);
    luaL_getmetatable(L, "UDim2");
    lua_setmetatable(L, -2);
}

bool is_udim2(lua_State *L, int i) { return udata(L, i, "UDim2") != NULL; }

void get_udim2(lua_State *L, int i, f32 *out)
{
    sudim2 *u = (sudim2 *)check_udata(L, i, "UDim2");
    out[0] = u->sx;
    out[1] = u->ox;
    out[2] = u->sy;
    out[3] = u->oy;
}

static int udim2_new(lua_State *L)
{
    if (is_udim(L, 1)) { /* UDim2.new(UDim x, UDim y) */
        sudim *x = (sudim *)check_udata(L, 1, "UDim"), *y = (sudim *)check_udata(L, 2, "UDim");
        push_udim2(L, x->s, x->o, y->s, y->o);
        return 1;
    }
    push_udim2(L, optf(L, 1, 0), optf(L, 2, 0), optf(L, 3, 0), optf(L, 4, 0));
    return 1;
}

static int udim2_fromscale(lua_State *L)
{
    push_udim2(L, optf(L, 1, 0), 0, optf(L, 2, 0), 0);
    return 1;
}

static int udim2_fromoffset(lua_State *L)
{
    push_udim2(L, 0, optf(L, 1, 0), 0, optf(L, 2, 0));
    return 1;
}

static int udim2_lerp(lua_State *L)
{
    sudim2 *a = (sudim2 *)check_udata(L, 1, "UDim2"), *b = (sudim2 *)check_udata(L, 2, "UDim2");
    f32 t = numf(L, 3);
    push_udim2(L, a->sx + (b->sx - a->sx) * t, a->ox + (b->ox - a->ox) * t, a->sy + (b->sy - a->sy) * t,
               a->oy + (b->oy - a->oy) * t);
    return 1;
}

static int udim2_index(lua_State *L)
{
    sudim2 *u = (sudim2 *)check_udata(L, 1, "UDim2");
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "X") || streq(k, "Width")) push_udim(L, u->sx, u->ox);
    else if (streq(k, "Y") || streq(k, "Height")) push_udim(L, u->sy, u->oy);
    else if (streq(k, "Lerp")) lua_pushcfunction(L, udim2_lerp);
    else return sapi_error(L, "%s is not a valid member of UDim2", k);
    return 1;
}

static int udim2_tostring(lua_State *L)
{
    sudim2 *u = (sudim2 *)check_udata(L, 1, "UDim2");
    char a[32], b[32];
    fmtn(a, sizeof a, u->sx);
    fmtn(b, sizeof b, u->sy);
    lua_pushfstring(L, "{%s, %d}, {%s, %d}", a, (int)u->ox, b, (int)u->oy);
    return 1;
}

static int udim2_add(lua_State *L)
{
    sudim2 *a = (sudim2 *)check_udata(L, 1, "UDim2"), *b = (sudim2 *)check_udata(L, 2, "UDim2");
    push_udim2(L, a->sx + b->sx, a->ox + b->ox, a->sy + b->sy, a->oy + b->oy);
    return 1;
}

static int udim2_sub(lua_State *L)
{
    sudim2 *a = (sudim2 *)check_udata(L, 1, "UDim2"), *b = (sudim2 *)check_udata(L, 2, "UDim2");
    push_udim2(L, a->sx - b->sx, a->ox - b->ox, a->sy - b->sy, a->oy - b->oy);
    return 1;
}

static int udim2_unm(lua_State *L)
{
    sudim2 *a = (sudim2 *)check_udata(L, 1, "UDim2");
    push_udim2(L, -a->sx, -a->ox, -a->sy, -a->oy);
    return 1;
}

static int udim2_eq(lua_State *L)
{
    sudim2 *a = (sudim2 *)udata(L, 1, "UDim2"), *b = (sudim2 *)udata(L, 2, "UDim2");
    lua_pushboolean(L, a && b && a->sx == b->sx && a->ox == b->ox && a->sy == b->sy && a->oy == b->oy);
    return 1;
}

/* ------------------------------------------------------------- Vector2 */

void push_vector2(lua_State *L, f32 x, f32 y)
{
    sv2 *v = (sv2 *)lua_newuserdata(L, sizeof(sv2));
    v->x = x;
    v->y = y;
    luaL_getmetatable(L, "Vector2");
    lua_setmetatable(L, -2);
}

bool is_vector2(lua_State *L, int i) { return udata(L, i, "Vector2") != NULL; }

void get_vector2(lua_State *L, int i, f32 *out)
{
    sv2 *v = (sv2 *)check_udata(L, i, "Vector2");
    out[0] = v->x;
    out[1] = v->y;
}

static sv2 *cv2(lua_State *L, int i) { return (sv2 *)check_udata(L, i, "Vector2"); }

static int v2_new(lua_State *L)
{
    push_vector2(L, optf(L, 1, 0), optf(L, 2, 0));
    return 1;
}

static int v2_dot(lua_State *L)
{
    sv2 *a = cv2(L, 1), *b = cv2(L, 2);
    lua_pushnumber(L, a->x * b->x + a->y * b->y);
    return 1;
}

static int v2_cross(lua_State *L)
{
    sv2 *a = cv2(L, 1), *b = cv2(L, 2);
    lua_pushnumber(L, a->x * b->y - a->y * b->x);
    return 1;
}

static int v2_lerp(lua_State *L)
{
    sv2 *a = cv2(L, 1), *b = cv2(L, 2);
    f32 t = numf(L, 3);
    push_vector2(L, a->x + (b->x - a->x) * t, a->y + (b->y - a->y) * t);
    return 1;
}

static int v2_min(lua_State *L)
{
    sv2 *a = cv2(L, 1);
    f32 x = a->x, y = a->y;
    for (int i = 2; i <= lua_gettop(L); ++i) {
        sv2 *b = cv2(L, i);
        if (b->x < x) x = b->x;
        if (b->y < y) y = b->y;
    }
    push_vector2(L, x, y);
    return 1;
}

static int v2_max(lua_State *L)
{
    sv2 *a = cv2(L, 1);
    f32 x = a->x, y = a->y;
    for (int i = 2; i <= lua_gettop(L); ++i) {
        sv2 *b = cv2(L, i);
        if (b->x > x) x = b->x;
        if (b->y > y) y = b->y;
    }
    push_vector2(L, x, y);
    return 1;
}

static int v2_fuzzy(lua_State *L)
{
    sv2 *a = cv2(L, 1), *b = cv2(L, 2);
    f32 eps = optf(L, 3, 1e-5f), dx = a->x - b->x, dy = a->y - b->y;
    lua_pushboolean(L, dx * dx + dy * dy <= eps * eps);
    return 1;
}

static int v2_index(lua_State *L)
{
    sv2 *v = cv2(L, 1);
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "X") || streq(k, "x")) lua_pushnumber(L, v->x);
    else if (streq(k, "Y") || streq(k, "y")) lua_pushnumber(L, v->y);
    else if (streq(k, "Magnitude") || streq(k, "magnitude")) lua_pushnumber(L, sqrt((f64)v->x * v->x + (f64)v->y * v->y));
    else if (streq(k, "Unit") || streq(k, "unit")) {
        f64 m = sqrt((f64)v->x * v->x + (f64)v->y * v->y);
        if (m > 0) push_vector2(L, (f32)(v->x / m), (f32)(v->y / m));
        else push_vector2(L, __builtin_nanf(""), __builtin_nanf(""));
    } else if (streq(k, "Dot")) lua_pushcfunction(L, v2_dot);
    else if (streq(k, "Cross")) lua_pushcfunction(L, v2_cross);
    else if (streq(k, "Lerp") || streq(k, "lerp")) lua_pushcfunction(L, v2_lerp);
    else if (streq(k, "Min")) lua_pushcfunction(L, v2_min);
    else if (streq(k, "Max")) lua_pushcfunction(L, v2_max);
    else if (streq(k, "FuzzyEq")) lua_pushcfunction(L, v2_fuzzy);
    else return sapi_error(L, "%s is not a valid member of Vector2", k);
    return 1;
}

static int v2_tostring(lua_State *L)
{
    sv2 *v = cv2(L, 1);
    char a[32], b[32];
    fmtn(a, sizeof a, v->x);
    fmtn(b, sizeof b, v->y);
    lua_pushfstring(L, "%s, %s", a, b);
    return 1;
}

static int v2_add(lua_State *L)
{
    sv2 *a = cv2(L, 1), *b = cv2(L, 2);
    push_vector2(L, a->x + b->x, a->y + b->y);
    return 1;
}

static int v2_sub(lua_State *L)
{
    sv2 *a = cv2(L, 1), *b = cv2(L, 2);
    push_vector2(L, a->x - b->x, a->y - b->y);
    return 1;
}

static int v2_mul(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TNUMBER) {
        sv2 *b = cv2(L, 2);
        f32 k = numf(L, 1);
        push_vector2(L, b->x * k, b->y * k);
    } else if (lua_type(L, 2) == LUA_TNUMBER) {
        sv2 *a = cv2(L, 1);
        f32 k = numf(L, 2);
        push_vector2(L, a->x * k, a->y * k);
    } else {
        sv2 *a = cv2(L, 1), *b = cv2(L, 2);
        push_vector2(L, a->x * b->x, a->y * b->y);
    }
    return 1;
}

static int v2_div(lua_State *L)
{
    sv2 *a = cv2(L, 1);
    if (lua_type(L, 2) == LUA_TNUMBER) {
        f32 k = numf(L, 2);
        push_vector2(L, a->x / k, a->y / k);
    } else {
        sv2 *b = cv2(L, 2);
        push_vector2(L, a->x / b->x, a->y / b->y);
    }
    return 1;
}

static int v2_unm(lua_State *L)
{
    sv2 *a = cv2(L, 1);
    push_vector2(L, -a->x, -a->y);
    return 1;
}

static int v2_eq(lua_State *L)
{
    sv2 *a = (sv2 *)udata(L, 1, "Vector2"), *b = (sv2 *)udata(L, 2, "Vector2");
    lua_pushboolean(L, a && b && a->x == b->x && a->y == b->y);
    return 1;
}

/* ---------------------------------------------------------------- Rect */

static void push_rect(lua_State *L, f32 x0, f32 y0, f32 x1, f32 y1)
{
    srect *r = (srect *)lua_newuserdata(L, sizeof(srect));
    r->x0 = x0;
    r->y0 = y0;
    r->x1 = x1;
    r->y1 = y1;
    luaL_getmetatable(L, "Rect");
    lua_setmetatable(L, -2);
}

static int rect_new(lua_State *L)
{
    if (is_vector2(L, 1)) {
        sv2 *a = cv2(L, 1), *b = cv2(L, 2);
        push_rect(L, a->x, a->y, b->x, b->y);
    } else {
        push_rect(L, optf(L, 1, 0), optf(L, 2, 0), optf(L, 3, 0), optf(L, 4, 0));
    }
    return 1;
}

static int rect_index(lua_State *L)
{
    srect *r = (srect *)check_udata(L, 1, "Rect");
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "Min")) push_vector2(L, r->x0, r->y0);
    else if (streq(k, "Max")) push_vector2(L, r->x1, r->y1);
    else if (streq(k, "Width")) lua_pushnumber(L, r->x1 - r->x0);
    else if (streq(k, "Height")) lua_pushnumber(L, r->y1 - r->y0);
    else return sapi_error(L, "%s is not a valid member of Rect", k);
    return 1;
}

static int rect_tostring(lua_State *L)
{
    srect *r = (srect *)check_udata(L, 1, "Rect");
    char a[32], b[32], c[32], d[32];
    fmtn(a, sizeof a, r->x0);
    fmtn(b, sizeof b, r->y0);
    fmtn(c, sizeof c, r->x1);
    fmtn(d, sizeof d, r->y1);
    lua_pushfstring(L, "%s, %s, %s, %s", a, b, c, d);
    return 1;
}

static int rect_eq(lua_State *L)
{
    srect *a = (srect *)udata(L, 1, "Rect"), *b = (srect *)udata(L, 2, "Rect");
    lua_pushboolean(L, a && b && !memcmp(a, b, sizeof *a));
    return 1;
}

/* --------------------------------------------------------- NumberRange */

static void push_range(lua_State *L, f32 lo, f32 hi)
{
    srange *r = (srange *)lua_newuserdata(L, sizeof(srange));
    r->lo = lo;
    r->hi = hi;
    luaL_getmetatable(L, "NumberRange");
    lua_setmetatable(L, -2);
}

static int range_new(lua_State *L)
{
    f32 lo = numf(L, 1), hi = optf(L, 2, lo);
    if (hi < lo) return sapi_error(L, "NumberRange: invalid range");
    push_range(L, lo, hi);
    return 1;
}

static int range_index(lua_State *L)
{
    srange *r = (srange *)check_udata(L, 1, "NumberRange");
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "Min")) lua_pushnumber(L, r->lo);
    else if (streq(k, "Max")) lua_pushnumber(L, r->hi);
    else return sapi_error(L, "%s is not a valid member of NumberRange", k);
    return 1;
}

static int range_tostring(lua_State *L)
{
    srange *r = (srange *)check_udata(L, 1, "NumberRange");
    char a[32], b[32];
    fmtn(a, sizeof a, r->lo);
    fmtn(b, sizeof b, r->hi);
    lua_pushfstring(L, "%s %s ", a, b);
    return 1;
}

static int range_eq(lua_State *L)
{
    srange *a = (srange *)udata(L, 1, "NumberRange"), *b = (srange *)udata(L, 2, "NumberRange");
    lua_pushboolean(L, a && b && a->lo == b->lo && a->hi == b->hi);
    return 1;
}

/* ------------------------------------------------ the sequence types */

static int nkey_new(lua_State *L)
{
    f32 t = numf(L, 1), v = numf(L, 2), e = optf(L, 3, 0); /* before pushing anything */
    snkey *k = (snkey *)lua_newuserdata(L, sizeof(snkey));
    k->t = t;
    k->v = v;
    k->e = e;
    luaL_getmetatable(L, "NumberSequenceKeypoint");
    lua_setmetatable(L, -2);
    return 1;
}

static int nkey_index(lua_State *L)
{
    snkey *k = (snkey *)check_udata(L, 1, "NumberSequenceKeypoint");
    const char *f = luaL_checkstring(L, 2);
    if (streq(f, "Time")) lua_pushnumber(L, k->t);
    else if (streq(f, "Value")) lua_pushnumber(L, k->v);
    else if (streq(f, "Envelope")) lua_pushnumber(L, k->e);
    else return sapi_error(L, "%s is not a valid member of NumberSequenceKeypoint", f);
    return 1;
}

static int ckey_new(lua_State *L)
{
    sckey *k = (sckey *)lua_newuserdata(L, sizeof(sckey));
    k->t = numf(L, 1);
    u32 c = check_c3_rgb(L, 2);
    lua_getfield(L, 2, "R");
    lua_getfield(L, 2, "G");
    lua_getfield(L, 2, "B");
    k->r = (f32)lua_tonumber(L, -3);
    k->g = (f32)lua_tonumber(L, -2);
    k->b = (f32)lua_tonumber(L, -1);
    lua_pop(L, 3);
    (void)c;
    luaL_getmetatable(L, "ColorSequenceKeypoint");
    lua_setmetatable(L, -2);
    return 1;
}

static int ckey_index(lua_State *L)
{
    sckey *k = (sckey *)check_udata(L, 1, "ColorSequenceKeypoint");
    const char *f = luaL_checkstring(L, 2);
    if (streq(f, "Time")) lua_pushnumber(L, k->t);
    else if (streq(f, "Value")) push_c3(L, k->r, k->g, k->b);
    else return sapi_error(L, "%s is not a valid member of ColorSequenceKeypoint", f);
    return 1;
}

static void push_nseq(lua_State *L, const f32 *keys, u32 n)
{
    snseq *s = (snseq *)lua_newuserdata(L, sizeof(snseq));
    s->n = n > UB_SEQ_MAX_KEYS ? UB_SEQ_MAX_KEYS : n;
    for (u32 i = 0; i < s->n; ++i) {
        s->k[i].t = keys[i * 3];
        s->k[i].v = keys[i * 3 + 1];
        s->k[i].e = keys[i * 3 + 2];
    }
    luaL_getmetatable(L, "NumberSequence");
    lua_setmetatable(L, -2);
}

static void push_cseq(lua_State *L, const f32 *keys, u32 n)
{
    scseq *s = (scseq *)lua_newuserdata(L, sizeof(scseq));
    s->n = n > UB_SEQ_MAX_KEYS ? UB_SEQ_MAX_KEYS : n;
    for (u32 i = 0; i < s->n; ++i) {
        s->k[i].t = keys[i * 4];
        s->k[i].r = keys[i * 4 + 1];
        s->k[i].g = keys[i * 4 + 2];
        s->k[i].b = keys[i * 4 + 3];
    }
    luaL_getmetatable(L, "ColorSequence");
    lua_setmetatable(L, -2);
}

/* keypoints: times must start at 0, end at 1 and not go back */
static bool keys_ok(const f32 *k, u32 n, u32 stride)
{
    if (n < 2 || n > UB_SEQ_MAX_KEYS || k[0] != 0 || k[(n - 1) * stride] != 1) return false;
    for (u32 i = 1; i < n; ++i)
        if (k[i * stride] < k[(i - 1) * stride]) return false;
    return true;
}

static int nseq_new(lua_State *L)
{
    f32 k[UB_SEQ_MAX_KEYS * 3];
    u32 n = 0;
    if (lua_type(L, 1) == LUA_TNUMBER) {
        f32 a = numf(L, 1), b = optf(L, 2, a);
        f32 kk[6] = { 0, a, 0, 1, b, 0 };
        push_nseq(L, kk, 2);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    int len = (int)lua_objlen(L, 1);
    for (int i = 1; i <= len && n < UB_SEQ_MAX_KEYS; ++i) {
        lua_rawgeti(L, 1, i);
        snkey *kp = (snkey *)check_udata(L, -1, "NumberSequenceKeypoint");
        k[n * 3] = kp->t;
        k[n * 3 + 1] = kp->v;
        k[n * 3 + 2] = kp->e;
        n++;
        lua_pop(L, 1);
    }
    if (!keys_ok(k, n, 3)) return sapi_error(L, "NumberSequence: keypoints must run from time 0 to time 1");
    push_nseq(L, k, n);
    return 1;
}

static int cseq_new(lua_State *L)
{
    f32 k[UB_SEQ_MAX_KEYS * 4];
    u32 n = 0;
    if (is_c3(L, 1)) {
        for (int arg = 1; arg <= 2; ++arg) {
            int a = lua_isnoneornil(L, 2) ? 1 : arg;
            lua_getfield(L, a, "R");
            lua_getfield(L, a, "G");
            lua_getfield(L, a, "B");
            k[n * 4] = (f32)(arg - 1);
            k[n * 4 + 1] = (f32)lua_tonumber(L, -3);
            k[n * 4 + 2] = (f32)lua_tonumber(L, -2);
            k[n * 4 + 3] = (f32)lua_tonumber(L, -1);
            lua_pop(L, 3);
            n++;
        }
        push_cseq(L, k, n);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    int len = (int)lua_objlen(L, 1);
    for (int i = 1; i <= len && n < UB_SEQ_MAX_KEYS; ++i) {
        lua_rawgeti(L, 1, i);
        sckey *kp = (sckey *)check_udata(L, -1, "ColorSequenceKeypoint");
        k[n * 4] = kp->t;
        k[n * 4 + 1] = kp->r;
        k[n * 4 + 2] = kp->g;
        k[n * 4 + 3] = kp->b;
        n++;
        lua_pop(L, 1);
    }
    if (!keys_ok(k, n, 4)) return sapi_error(L, "ColorSequence: keypoints must run from time 0 to time 1");
    push_cseq(L, k, n);
    return 1;
}

static int nseq_index(lua_State *L)
{
    snseq *s = (snseq *)check_udata(L, 1, "NumberSequence");
    const char *f = luaL_checkstring(L, 2);
    if (!streq(f, "Keypoints")) return sapi_error(L, "%s is not a valid member of NumberSequence", f);
    lua_createtable(L, (int)s->n, 0);
    for (u32 i = 0; i < s->n; ++i) {
        snkey *k = (snkey *)lua_newuserdata(L, sizeof(snkey));
        *k = s->k[i];
        luaL_getmetatable(L, "NumberSequenceKeypoint");
        lua_setmetatable(L, -2);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

static int cseq_index(lua_State *L)
{
    scseq *s = (scseq *)check_udata(L, 1, "ColorSequence");
    const char *f = luaL_checkstring(L, 2);
    if (!streq(f, "Keypoints")) return sapi_error(L, "%s is not a valid member of ColorSequence", f);
    lua_createtable(L, (int)s->n, 0);
    for (u32 i = 0; i < s->n; ++i) {
        sckey *k = (sckey *)lua_newuserdata(L, sizeof(sckey));
        *k = s->k[i];
        luaL_getmetatable(L, "ColorSequenceKeypoint");
        lua_setmetatable(L, -2);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

static int nseq_tostring(lua_State *L)
{
    snseq *s = (snseq *)check_udata(L, 1, "NumberSequence");
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (u32 i = 0; i < s->n; ++i) {
        char t[96];
        snprintf(t, sizeof t, "%.14g %.14g %.14g ", (f64)s->k[i].t, (f64)s->k[i].v, (f64)s->k[i].e);
        luaL_addstring(&b, t);
    }
    luaL_pushresult(&b);
    return 1;
}

static int cseq_tostring(lua_State *L)
{
    scseq *s = (scseq *)check_udata(L, 1, "ColorSequence");
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (u32 i = 0; i < s->n; ++i) {
        char t[128];
        snprintf(t, sizeof t, "%.14g %.14g %.14g %.14g 0 ", (f64)s->k[i].t, (f64)s->k[i].r, (f64)s->k[i].g,
                 (f64)s->k[i].b);
        luaL_addstring(&b, t);
    }
    luaL_pushresult(&b);
    return 1;
}

static int seq_eq(lua_State *L)
{
    void *a = lua_touserdata(L, 1), *b = lua_touserdata(L, 2);
    bool same = false;
    if (udata(L, 1, "NumberSequence") && udata(L, 2, "NumberSequence"))
        same = ((snseq *)a)->n == ((snseq *)b)->n && !memcmp(a, b, sizeof(u32) + sizeof(snkey) * ((snseq *)a)->n);
    else if (udata(L, 1, "ColorSequence") && udata(L, 2, "ColorSequence"))
        same = ((scseq *)a)->n == ((scseq *)b)->n && !memcmp(a, b, sizeof(u32) + sizeof(sckey) * ((scseq *)a)->n);
    lua_pushboolean(L, same);
    return 1;
}

/* ---------------------------------------------------------------- Font */

static const char *const k_font_family[] = {
    "LegacyArial", "Arial", "Arial", "SourceSansPro", "SourceSansPro", "SourceSansPro", "SourceSansPro",
    "AccanthisADFStd", "Guru", "ComicNeueAngular", "Inconsolata", "HighwayGothic", "Zekton", "PressStart2P",
    "Balthazar", "RomanAntique", "SourceSansPro", "GothamSSm", "GothamSSm", "GothamSSm", "GothamSSm", "AmaticSC",
    "Bangers", "Creepster", "DenkOne", "Fondamento", "FredokaOne", "GrenzeGotisch", "IndieFlower", "JosefinSans",
    "Jura", "Kalam", "LuckiestGuy", "Merriweather", "Michroma", "Nunito", "Oswald", "PatrickHand",
    "PermanentMarker", "Roboto", "RobotoCondensed", "RobotoMono", "Sarpanch", "SpecialElite", "TitilliumWeb",
    "Ubuntu",
};
static const u16 k_font_weight[] = { 400, 400, 700, 400, 700, 300, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400,
                                     600, 400, 500, 700, 900, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400,
                                     400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400 };

static void font_of_enum(int e, sfont *f)
{
    int n = (int)(sizeof k_font_family / sizeof k_font_family[0]);
    if (e < 0 || e >= n) e = 3;
    snprintf(f->family, sizeof f->family, "rbxasset://fonts/families/%s.json", k_font_family[e]);
    f->weight = k_font_weight[e];
    f->style = e == 6 ? 1 : 0;
}

/* The Enum.Font a FontFace stands for, Unknown (100) if none */
static int enum_of_font(const char *family, int weight, int style)
{
    int n = (int)(sizeof k_font_family / sizeof k_font_family[0]);
    for (int e = 0; e < n; ++e) {
        sfont f;
        font_of_enum(e, &f);
        if (streq(f.family, family) && f.weight == weight && f.style == style) return e;
    }
    return 100;
}

static void push_font(lua_State *L, const char *family, int weight, int style)
{
    sfont *f = (sfont *)lua_newuserdata(L, sizeof(sfont));
    snprintf(f->family, sizeof f->family, "%s", family ? family : "");
    f->weight = (u16)UB_CLAMP(weight, 100, 900);
    f->style = (u8)(style ? 1 : 0);
    luaL_getmetatable(L, "Font");
    lua_setmetatable(L, -2);
}

static int opt_weight(lua_State *L, int i)
{
    if (lua_isnoneornil(L, i)) return 400;
    return check_enum(L, i, E_FONTWEIGHT);
}

static int opt_style(lua_State *L, int i)
{
    if (lua_isnoneornil(L, i)) return 0;
    return check_enum(L, i, E_FONTSTYLE);
}

static int font_new(lua_State *L)
{
    push_font(L, luaL_checkstring(L, 1), opt_weight(L, 2), opt_style(L, 3));
    return 1;
}

static int font_fromname(lua_State *L)
{
    char fam[200];
    snprintf(fam, sizeof fam, "rbxasset://fonts/families/%s.json", luaL_checkstring(L, 1));
    push_font(L, fam, opt_weight(L, 2), opt_style(L, 3));
    return 1;
}

static int font_fromid(lua_State *L)
{
    char fam[64];
    snprintf(fam, sizeof fam, "rbxassetid://%.0f", luaL_checknumber(L, 1));
    push_font(L, fam, opt_weight(L, 2), opt_style(L, 3));
    return 1;
}

static int font_fromenum(lua_State *L)
{
    sfont f;
    font_of_enum(check_enum(L, 1, E_FONT), &f);
    push_font(L, f.family, f.weight, f.style);
    return 1;
}

static int font_index(lua_State *L)
{
    sfont *f = (sfont *)check_udata(L, 1, "Font");
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "Family")) lua_pushstring(L, f->family);
    else if (streq(k, "Weight")) push_enum(L, E_FONTWEIGHT, f->weight);
    else if (streq(k, "Style")) push_enum(L, E_FONTSTYLE, f->style);
    else if (streq(k, "Bold")) lua_pushboolean(L, f->weight >= 600);
    else return sapi_error(L, "%s is not a valid member of Font", k);
    return 1;
}

static int font_tostring(lua_State *L)
{
    sfont *f = (sfont *)check_udata(L, 1, "Font");
    lua_pushfstring(L, "Font { Family = %s, Weight = %s, Style = %s }", f->family,
                    enum_item_name(E_FONTWEIGHT, f->weight), enum_item_name(E_FONTSTYLE, f->style));
    return 1;
}

static int font_eq(lua_State *L)
{
    sfont *a = (sfont *)udata(L, 1, "Font"), *b = (sfont *)udata(L, 2, "Font");
    lua_pushboolean(L, a && b && streq(a->family, b->family) && a->weight == b->weight && a->style == b->style);
    return 1;
}

/* ---------------------------------------------------------- InputObject */

static void push_input(lua_State *L, const char *type, const char *state, int key, f32 x, f32 y, f32 z, f32 dx, f32 dy)
{
    sinput *in = (sinput *)lua_newuserdata(L, sizeof(sinput));
    memset(in, 0, sizeof *in);
    in->type = ev_of(E_UIT, type);
    in->state = ev_of(E_UIS, state);
    in->key = key;
    in->px = x;
    in->py = y;
    in->pz = z;
    in->dx = dx;
    in->dy = dy;
    luaL_getmetatable(L, "InputObject");
    lua_setmetatable(L, -2);
}

static int input_ismod(lua_State *L)
{
    lua_pushboolean(L, 0);
    return 1;
}

static int input_index(lua_State *L)
{
    sinput *in = (sinput *)check_udata(L, 1, "InputObject");
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "UserInputType")) push_enum(L, E_UIT, in->type);
    else if (streq(k, "UserInputState")) push_enum(L, E_UIS, in->state);
    else if (streq(k, "KeyCode")) push_enum(L, E_KC, in->key);
    else if (streq(k, "Position")) push_v3(L, v3(in->px, in->py, in->pz));
    else if (streq(k, "Delta")) push_v3(L, v3(in->dx, in->dy, in->dz));
    else if (streq(k, "IsModifierKeyDown")) lua_pushcfunction(L, input_ismod);
    else if (streq(k, "Changed")) push_signal(L, VIRT_USERINPUT, EV_NEVER, NULL);
    else if (streq(k, "ClassName") || streq(k, "Name")) lua_pushliteral(L, "InputObject");
    else return sapi_error(L, "%s is not a valid member of InputObject", k);
    return 1;
}

static int input_tostring(lua_State *L)
{
    lua_pushliteral(L, "InputObject");
    return 1;
}

/* Windows virtual keys -> Enum.KeyCode values */
static int keycode_of(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return vk - 'A' + 97;
    if (vk >= '0' && vk <= '9') return vk;
    if (vk >= UB_KEY_F1 && vk <= UB_KEY_F12) return 282 + (vk - UB_KEY_F1);
    switch (vk) {
    case UB_KEY_BACKSPACE: return 8;
    case UB_KEY_TAB: return 9;
    case UB_KEY_ENTER: return 13;
    case UB_KEY_ESCAPE: return 27;
    case UB_KEY_SPACE: return 32;
    case UB_KEY_SHIFT: return 304;   /* LeftShift */
    case UB_KEY_CONTROL: return 306; /* LeftControl */
    case UB_KEY_ALT: return 308;     /* LeftAlt */
    case UB_KEY_UP: return 273;
    case UB_KEY_DOWN: return 274;
    case UB_KEY_RIGHT: return 275;
    case UB_KEY_LEFT: return 276;
    case UB_KEY_INSERT: return 277;
    case UB_KEY_HOME: return 278;
    case UB_KEY_END: return 279;
    case UB_KEY_PAGEUP: return 280;
    case UB_KEY_PAGEDOWN: return 281;
    case UB_KEY_DELETE: return 127;
    case 0xBA: return 59;  /* ; */
    case 0xBB: return 61;  /* = */
    case 0xBC: return 44;  /* , */
    case 0xBD: return 45;  /* - */
    case 0xBE: return 46;  /* . */
    case 0xBF: return 47;  /* / */
    case 0xC0: return 96;  /* ` */
    case 0xDB: return 91;  /* [ */
    case 0xDC: return 92;  /* \ */
    case 0xDD: return 93;  /* ] */
    case 0xDE: return 39;  /* ' */
    default: return 0;
    }
}

static int vk_of(int keycode)
{
    for (int vk = 1; vk < 256; ++vk)
        if (keycode_of(vk) == keycode) return vk;
    if (keycode == 303) return UB_KEY_SHIFT; /* RightShift */
    if (keycode == 305) return UB_KEY_CONTROL;
    if (keycode == 307) return UB_KEY_ALT;
    return -1;
}

/* ------------------------------------------------- property values */

void push_pvalue(lua_State *L, const ub_value *v, int enum_type)
{
    switch (v->type) {
    case UB_PV_BOOL: lua_pushboolean(L, v->v.b); break;
    case UB_PV_INT: lua_pushinteger(L, v->v.i); break;
    case UB_PV_FLOAT: lua_pushnumber(L, v->v.f); break;
    case UB_PV_DOUBLE: lua_pushnumber(L, v->v.d); break;
    case UB_PV_STRING: lua_pushlstring(L, v->v.str.s, v->v.str.len); break;
    case UB_PV_ENUM:
        if (enum_type >= 0) push_enum(L, enum_type, (int)v->v.e);
        else lua_pushinteger(L, (lua_Integer)v->v.e);
        break;
    case UB_PV_COLOR3: push_c3(L, v->v.c3.r, v->v.c3.g, v->v.c3.b); break;
    case UB_PV_VECTOR2: push_vector2(L, v->v.v2.x, v->v.v2.y); break;
    case UB_PV_VECTOR3: push_v3(L, v3(v->v.v3.x, v->v.v3.y, v->v.v3.z)); break;
    case UB_PV_UDIM: push_udim(L, v->v.ud.s, (f32)v->v.ud.o); break;
    case UB_PV_UDIM2: push_udim2(L, v->v.ud2.sx, (f32)v->v.ud2.ox, v->v.ud2.sy, (f32)v->v.ud2.oy); break;
    case UB_PV_RECT: push_rect(L, v->v.rect.x0, v->v.rect.y0, v->v.rect.x1, v->v.rect.y1); break;
    case UB_PV_NUMRANGE: push_range(L, v->v.range.lo, v->v.range.hi); break;
    case UB_PV_NUMSEQ: push_nseq(L, v->v.seq.k, v->v.seq.n); break;
    case UB_PV_COLORSEQ: push_cseq(L, v->v.seq.k, v->v.seq.n); break;
    case UB_PV_REF: push_inst(L, ub_world_alive(W, v->v.ref) ? v->v.ref : -1); break;
    case UB_PV_FONT: push_font(L, v->v.font.family, v->v.font.weight, v->v.font.style); break;
    case UB_PV_CFRAME: {
        const f32 *m = v->v.cf.m;
        mat3 r;
        r.x = v3(m[3], m[6], m[9]); /* columns from the rows */
        r.y = v3(m[4], m[7], m[10]);
        r.z = v3(m[5], m[8], m[11]);
        push_cf(L, v3(m[0], m[1], m[2]), &r);
        break;
    }
    case UB_PV_BRICKCOLOR: push_brickcolor(L, brickcolor_number_rgb(v->v.e)); break;
    default: lua_pushnil(L); break;
    }
}

static const char *ptype_name(u8 t)
{
    static const char *const n[UB_PV_COUNT] = { "nil", "bool", "int", "float", "double", "string", "EnumItem",
                                                "Color3", "Vector2", "Vector3", "UDim", "UDim2", "Rect", "NumberRange",
                                                "NumberSequence", "ColorSequence", "Object", "Font", "BrickColor", "CFrame" };
    return t < UB_PV_COUNT ? n[t] : "?";
}

bool to_pvalue(lua_State *L, int i, u8 type, int enum_type, ub_value *out, f32 *seq, const char **err)
{
    memset(out, 0, sizeof *out);
    out->type = type;
    *err = NULL;
    int lt = lua_type(L, i);
    switch (type) {
    case UB_PV_BOOL:
        out->v.b = lua_toboolean(L, i) != 0;
        return true;
    case UB_PV_INT: case UB_PV_FLOAT: case UB_PV_DOUBLE: {
        if (lt != LUA_TNUMBER && !(lt == LUA_TSTRING && lua_tonumber(L, i) != 0)) break;
        f64 d = lua_tonumber(L, i);
        if (type == UB_PV_INT) out->v.i = (i32)(d > 2147483647.0 ? 2147483647.0 : d < -2147483648.0 ? -2147483648.0 : d);
        else if (type == UB_PV_FLOAT) out->v.f = (f32)d;
        else out->v.d = d;
        return true;
    }
    case UB_PV_STRING: {
        if (lt != LUA_TSTRING && lt != LUA_TNUMBER) break;
        size_t n;
        const char *s = lua_tolstring(L, i, &n);
        out->v.str.s = (char *)s;
        out->v.str.len = (u32)n;
        return true;
    }
    case UB_PV_ENUM:
        if (enum_type >= 0 && (is_enum_of(L, i, enum_type) || lt == LUA_TNUMBER || lt == LUA_TSTRING)) {
            if (lt == LUA_TSTRING) { /* must name an item */
                const char *nm = lua_tostring(L, i);
                bool found = false;
                if (enum_type >= EN_COUNT) found = ub_enum_value_of(enum_type - EN_COUNT, nm) >= 0;
                else found = true;
                if (!found) break;
            }
            out->v.e = (u32)check_enum(L, i, enum_type);
            return true;
        }
        if (enum_type < 0 && lt == LUA_TNUMBER) {
            out->v.e = (u32)lua_tointeger(L, i);
            return true;
        }
        break;
    case UB_PV_COLOR3:
        if (!is_c3(L, i)) break;
        lua_getfield(L, i, "R");
        lua_getfield(L, i, "G");
        lua_getfield(L, i, "B");
        out->v.c3.r = (f32)lua_tonumber(L, -3);
        out->v.c3.g = (f32)lua_tonumber(L, -2);
        out->v.c3.b = (f32)lua_tonumber(L, -1);
        lua_pop(L, 3);
        return true;
    case UB_PV_VECTOR2:
        if (!is_vector2(L, i)) break;
        out->v.v2.x = cv2(L, i)->x;
        out->v.v2.y = cv2(L, i)->y;
        return true;
    case UB_PV_VECTOR3: {
        if (!is_v3(L, i)) break;
        vec3 v = check_v3(L, i);
        out->v.v3.x = v.x;
        out->v.v3.y = v.y;
        out->v.v3.z = v.z;
        return true;
    }
    case UB_PV_UDIM: {
        if (!is_udim(L, i)) break;
        sudim *u = (sudim *)lua_touserdata(L, i);
        out->v.ud.s = u->s;
        out->v.ud.o = (i32)u->o;
        return true;
    }
    case UB_PV_UDIM2: {
        if (!is_udim2(L, i)) break;
        sudim2 *u = (sudim2 *)lua_touserdata(L, i);
        out->v.ud2.sx = u->sx;
        out->v.ud2.ox = (i32)u->ox;
        out->v.ud2.sy = u->sy;
        out->v.ud2.oy = (i32)u->oy;
        return true;
    }
    case UB_PV_RECT: {
        srect *r = (srect *)udata(L, i, "Rect");
        if (!r) break;
        out->v.rect.x0 = r->x0;
        out->v.rect.y0 = r->y0;
        out->v.rect.x1 = r->x1;
        out->v.rect.y1 = r->y1;
        return true;
    }
    case UB_PV_NUMRANGE: {
        srange *r = (srange *)udata(L, i, "NumberRange");
        if (!r) break;
        out->v.range.lo = r->lo;
        out->v.range.hi = r->hi;
        return true;
    }
    case UB_PV_NUMSEQ: {
        snseq *s = (snseq *)udata(L, i, "NumberSequence");
        if (!s) break;
        for (u32 k = 0; k < s->n; ++k) {
            seq[k * 3] = s->k[k].t;
            seq[k * 3 + 1] = s->k[k].v;
            seq[k * 3 + 2] = s->k[k].e;
        }
        out->v.seq.k = seq;
        out->v.seq.n = s->n;
        return true;
    }
    case UB_PV_COLORSEQ: {
        scseq *s = (scseq *)udata(L, i, "ColorSequence");
        if (!s) break;
        for (u32 k = 0; k < s->n; ++k) {
            seq[k * 4] = s->k[k].t;
            seq[k * 4 + 1] = s->k[k].r;
            seq[k * 4 + 2] = s->k[k].g;
            seq[k * 4 + 3] = s->k[k].b;
        }
        out->v.seq.k = seq;
        out->v.seq.n = s->n;
        return true;
    }
    case UB_PV_REF:
        if (lt == LUA_TNIL) {
            out->v.ref = -1;
            return true;
        }
        if (!is_inst(L, i)) break;
        out->v.ref = check_inst(L, i);
        if (out->v.ref < 0) break;
        return true;
    case UB_PV_FONT: {
        sfont *f = (sfont *)udata(L, i, "Font");
        if (!f) break;
        out->v.font.family = f->family;
        out->v.font.weight = f->weight;
        out->v.font.style = f->style;
        return true;
    }
    case UB_PV_CFRAME: {
        if (!is_cf(L, i)) break;
        scf *c = check_cf(L, i);
        f32 m[12] = { c->p.x, c->p.y, c->p.z, c->r.x.x, c->r.y.x, c->r.z.x, c->r.x.y, c->r.y.y, c->r.z.y,
                      c->r.x.z, c->r.y.z, c->r.z.z };
        memcpy(seq, m, sizeof m);
        out->v.cf.m = seq;
        return true;
    }
    case UB_PV_BRICKCOLOR:
        if (!is_brickcolor(L, i)) break;
        out->v.e = brickcolor_rgb_number(brickcolor_rgb(L, i));
        return true;
    default: break;
    }
    *err = ptype_name(type);
    return false;
}

/* ------------------------------------------------ class-table properties */

static int enum_type_of(const ub_cdb_prop *pd)
{
    return pd && pd->enum_id >= 0 ? enum_type_named(ub_enum_name(pd->enum_id)) : -1;
}

/* Enum.FontSize items -> pixels */
static const u8 k_fontsize[] = { 8, 9, 10, 11, 12, 14, 18, 24, 28, 32, 36, 42, 48, 60, 96 };

static void strip_tags(lua_State *L, const char *s, size_t n)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '<') {
            size_t j = i + 1;
            while (j < n && s[j] != '>' && s[j] != '<') ++j;
            if (j < n && s[j] == '>') {
                if (j > i + 1 && (s[i + 1] == 'b' || s[i + 1] == 'B') && (s[i + 2] == 'r' || s[i + 2] == 'R')) luaL_addchar(&b, '\n');
                i = j;
                continue;
            }
        }
        if (s[i] == '&') {
            static const struct { const char *e; char c; } ents[] = {
                { "&lt;", '<' }, { "&gt;", '>' }, { "&amp;", '&' }, { "&quot;", '"' }, { "&apos;", '\'' },
            };
            bool hit = false;
            for (int k = 0; k < 5 && !hit; ++k) {
                size_t el = strlen(ents[k].e);
                if (n - i >= el && !memcmp(s + i, ents[k].e, el)) {
                    luaL_addchar(&b, ents[k].c);
                    i += el - 1;
                    hit = true;
                }
            }
            if (hit) continue;
        }
        luaL_addchar(&b, s[i]);
    }
    luaL_pushresult(&b);
}

static bool gui_lua_method(lua_State *L, i32 idx, const char *k);

bool gui_get(lua_State *L, i32 idx, const char *k)
{
    if (idx < 0) return false;
    const char *cls = cls_of(idx);
    int c = ub_cdb_class(cls);
    if (c < 0) return false;
    bool gb = ub_cdb_is_a(cls, "GuiBase2d");
    /* worked out by the layout */
    if (gb || ub_cdb_is_a(cls, "UIGridStyleLayout")) {
        bool want = streq(k, "AbsolutePosition") || streq(k, "AbsoluteSize") || streq(k, "AbsoluteRotation") ||
                    streq(k, "TextBounds") || streq(k, "TextFits") || streq(k, "AbsoluteContentSize") ||
                    streq(k, "AbsoluteCanvasSize") || streq(k, "AbsoluteWindowSize") || streq(k, "AbsoluteCellSize") ||
                    streq(k, "AbsoluteCellCount") || streq(k, "CurrentPage");
        if (want) {
            ub_gui_info in;
            if (!GUI || !ub_gui_info_of(GUI, W, idx, &in)) {
                memset(&in, 0, sizeof in);
                in.page = -1;
            }
            if (streq(k, "AbsolutePosition")) push_vector2(L, in.x, in.y);
            else if (streq(k, "AbsoluteSize")) push_vector2(L, in.w, in.h);
            else if (streq(k, "AbsoluteRotation")) lua_pushnumber(L, in.rotation);
            else if (streq(k, "TextBounds")) push_vector2(L, in.text_w, in.text_h);
            else if (streq(k, "TextFits")) lua_pushboolean(L, in.text_fits);
            else if (streq(k, "AbsoluteContentSize")) push_vector2(L, in.content_w, in.content_h);
            else if (streq(k, "AbsoluteCanvasSize")) push_vector2(L, in.canvas_w, in.canvas_h);
            else if (streq(k, "AbsoluteWindowSize")) push_vector2(L, in.window_w, in.window_h);
            else if (streq(k, "AbsoluteCellSize")) push_vector2(L, in.cell_w, in.cell_h);
            else if (streq(k, "AbsoluteCellCount")) push_vector2(L, (f32)in.cells_x, (f32)in.cells_y);
            else push_inst(L, in.page);
            return true;
        }
    }
    if (gb && (streq(k, "ContentText") || streq(k, "LocalizedText"))) {
        const ub_value *t = ub_world_prop_named(W, idx, "Text"), *rich = ub_world_prop_named(W, idx, "RichText");
        if (!t || t->type != UB_PV_STRING) return false;
        if (streq(k, "ContentText") && rich && rich->type == UB_PV_BOOL && rich->v.b) strip_tags(L, t->v.str.s, t->v.str.len);
        else lua_pushlstring(L, t->v.str.s, t->v.str.len);
        return true;
    }
    if (gb && streq(k, "IsLoaded")) {
        const ub_value *im = ub_world_prop_named(W, idx, "Image");
        lua_pushboolean(L, im && im->type == UB_PV_STRING && ub_image_get(im->v.str.s) != NULL);
        return true;
    }
    /* deprecated names for stored properties */
    if (gb && streq(k, "Transparency")) k = "BackgroundTransparency";
    else if (gb && streq(k, "TextWrap")) k = "TextWrapped";
    else if (gb && streq(k, "FontSize")) {
        f32 sz = 14;
        const ub_value *v = ub_world_prop_named(W, idx, "TextSize");
        if (v && v->type == UB_PV_FLOAT) sz = v->v.f;
        int best = 0;
        for (int i = 0; i < (int)sizeof k_fontsize; ++i)
            if (k_fontsize[i] <= sz + 0.5f) best = i;
        push_enum(L, E_FONTSIZE, best);
        return true;
    }
    if (gui_lua_method(L, idx, k)) return true;
    const ub_value *v = ub_world_prop_named(W, idx, k);
    const ub_cdb_prop *pd = ub_cdb_find_prop(c, k);
    if (v) {
        push_pvalue(L, v, enum_type_of(pd));
        return true;
    }
    ub_value d;
    if (pd && ub_cdb_default(c, k, &d)) {
        push_pvalue(L, &d, enum_type_of(pd));
        return true;
    }
    return false;
}

static void set_font_enum(i32 idx, int e)
{
    sfont f;
    font_of_enum(e, &f);
    ub_value v;
    memset(&v, 0, sizeof v);
    v.type = UB_PV_FONT;
    v.v.font.family = f.family;
    v.v.font.weight = f.weight;
    v.v.font.style = f.style;
    ub_world_set_prop_named(W, idx, "FontFace", &v);
}

bool gui_set(lua_State *L, i32 idx, const char *k, int vi)
{
    if (idx < 0) return false;
    const char *cls = cls_of(idx);
    int c = ub_cdb_class(cls);
    if (c < 0) return false;
    bool gb = ub_cdb_is_a(cls, "GuiBase2d");
    if (gb && streq(k, "Transparency")) k = "BackgroundTransparency";
    else if (gb && streq(k, "TextWrap")) k = "TextWrapped";
    else if (gb && streq(k, "FontSize")) {
        int e = check_enum(L, vi, E_FONTSIZE);
        ub_value v = ub_vfloat((f32)k_fontsize[e >= 0 && e < (int)sizeof k_fontsize ? e : 5]);
        ub_world_set_prop_named(W, idx, "TextSize", &v);
        prop_changed(L, idx, "TextSize");
        return true;
    }
    const ub_cdb_prop *pd = ub_cdb_find_prop(c, k);
    if (!pd) return false;
    if (pd->flags & UB_CDB_READONLY) return sapi_error(L, "Unable to assign property %s. Property is read only", k), true;
    f32 seq[UB_SEQ_MAX_KEYS * 4];
    ub_value v;
    const char *err;
    int et = enum_type_of(pd);
    if (!to_pvalue(L, vi, pd->type, et, &v, seq, &err))
        return sapi_error(L, "Unable to assign property %s. %s expected, got %s", k, err, ub_typeof(L, vi)), true;
    /* as Roblox: IntValue rounds; numbers of these properties stay sane */
    if (streq(cls, "IntValue") && streq(k, "Value") && v.type == UB_PV_DOUBLE)
        v.v.d = v.v.d < 0 ? ceil(v.v.d - 0.5) : floor(v.v.d + 0.5);
    if (v.type == UB_PV_FLOAT && !(v.v.f == v.v.f)) v.v.f = 0;
    if (!ub_world_set_prop_named(W, idx, k, &v)) return luaL_error(L, "out of memory"), true;
    /* Font and FontFace say the same thing */
    if (streq(k, "Font") && v.type == UB_PV_ENUM) set_font_enum(idx, (int)v.v.e);
    if (streq(k, "FontFace") && v.type == UB_PV_FONT) {
        ub_value e = ub_venum((u32)enum_of_font(v.v.font.family, v.v.font.weight, v.v.font.style));
        ub_world_set_prop_named(W, idx, "Font", &e);
    }
    return true;
}

i32 gui_instance_new(const char *cls)
{
    int c = ub_cdb_class(cls);
    if (c < 0 || !ub_cdb_creatable(c)) return -1;
    i32 idx = ub_world_add(W, -1, streq(cls, "Folder") ? UB_CLASS_FOLDER : UB_CLASS_OTHER, cls, cls);
    if (idx >= 0) ub_cdb_fill_defaults(W, idx, cls);
    return idx;
}

/* ------------------------------------------------------------- methods */

static int m_fireserver(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    int n = lua_gettop(L) - 1;
    push_inst(L, S.g->player);
    lua_insert(L, 2);
    fire_signal(L, idx, EV_SERVEREVENT, NULL, n + 1);
    return 0;
}

static int m_fireclient(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    check_inst(L, 2); /* the player: there is one */
    lua_remove(L, 2);
    fire_signal(L, idx, EV_CLIENTEVENT, NULL, lua_gettop(L) - 1);
    return 0;
}

static int m_fireall(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    fire_signal(L, idx, EV_CLIENTEVENT, NULL, lua_gettop(L) - 1);
    return 0;
}

static int m_fire(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    fire_signal(L, idx, EV_BINDABLE, NULL, lua_gettop(L) - 1);
    return 0;
}

static int m_capturefocus(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    if (GUI) ub_gui_focus(GUI, W, idx);
    return 0;
}

static int m_releasefocus(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    if (GUI && ub_gui_focused_box(GUI) == idx) ub_gui_release_focus(GUI, W, lua_toboolean(L, 2) != 0);
    return 0;
}

static int m_isfocused(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    lua_pushboolean(L, GUI && ub_gui_focused_box(GUI) == idx);
    return 1;
}

static int m_jumpto(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    i32 page = check_inst(L, 2);
    if (GUI) ub_gui_page_jump(GUI, W, idx, page);
    return 0;
}

static int m_jumptoindex(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    int want = (int)luaL_checkinteger(L, 2);
    ub_gui_info in;
    if (GUI && ub_gui_info_of(GUI, W, idx, &in)) {
        ub_gui_page_jump(GUI, W, idx, in.page);
        ub_gui_page_step(GUI, W, idx, want);
    }
    return 0;
}

static int m_next(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    if (GUI) ub_gui_page_step(GUI, W, idx, 1);
    return 0;
}

static int m_previous(lua_State *L)
{
    i32 idx = check_inst(L, 1);
    if (GUI) ub_gui_page_step(GUI, W, idx, -1);
    return 0;
}

static int m_objects_at(lua_State *L)
{
    check_inst(L, 1);
    i32 hits[128];
    int n = GUI ? ub_gui_objects_at(GUI, W, numf(L, 2), numf(L, 3), hits, 128) : 0;
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        push_inst(L, hits[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int m_noop(lua_State *L) { return 0; }


static int m_nil(lua_State *L)
{
    lua_pushnil(L);
    return 1;
}

static int m_half(lua_State *L)
{
    lua_pushnumber(L, 0.5);
    return 1;
}

static int m_setcore(lua_State *L)
{
    const char *what = luaL_optstring(L, 2, "");
    if (streq(what, "SendNotification") && lua_istable(L, 3)) {
        lua_getfield(L, 3, "Title");
        lua_getfield(L, 3, "Text");
        ub_info("script: notification: %s - %s", luaL_optstring(L, -2, ""), luaL_optstring(L, -1, ""));
        lua_pop(L, 2);
    }
    return 0;
}

/* StarterGui:SetCoreGuiEnabled(type, on): the Backpack (2) and All (5) hide
 * the hotbar; the other core GUIs Uniblox does not draw. */
extern bool g_backpack_hidden;
static int m_setcoregui(lua_State *L)
{
    int t = lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : -1;
    if (t < 0) {
        lua_getfield(L, 2, "Value");
        t = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    if (t == 2 || t == 5) g_backpack_hidden = !lua_toboolean(L, 3);
    return 0;
}

static int m_getcoregui(lua_State *L)
{
    int t = lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : -1;
    if (t < 0) {
        lua_getfield(L, 2, "Value");
        t = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    lua_pushboolean(L, (t == 2 || t == 5) ? !g_backpack_hidden : 1);
    return 1;
}

lua_CFunction gui_method(i32 idx, const char *k)
{
    if (idx < 0) return NULL;
    const char *cls = cls_of(idx);
    if (streq(cls, "RemoteEvent")) {
        if (streq(k, "FireServer")) return m_fireserver;
        if (streq(k, "FireClient")) return m_fireclient;
        if (streq(k, "FireAllClients")) return m_fireall;
    }
    if (streq(cls, "BindableEvent") && streq(k, "Fire")) return m_fire;
    if (streq(cls, "TextBox")) {
        if (streq(k, "CaptureFocus")) return m_capturefocus;
        if (streq(k, "ReleaseFocus")) return m_releasefocus;
        if (streq(k, "IsFocused")) return m_isfocused;
    }
    if (streq(cls, "UIPageLayout")) {
        if (streq(k, "JumpTo")) return m_jumpto;
        if (streq(k, "JumpToIndex")) return m_jumptoindex;
        if (streq(k, "Next")) return m_next;
        if (streq(k, "Previous")) return m_previous;
    }
    if (ub_cdb_is_a(cls, "UIGridStyleLayout") && streq(k, "ApplyLayout")) return m_noop;
    if (streq(cls, "PlayerGui") || ub_cdb_is_a(cls, "LayerCollector")) {
        if (streq(k, "GetGuiObjectsAtPosition")) return m_objects_at;
        if (streq(k, "SetTopbarTransparency")) return m_noop;
        if (streq(k, "GetTopbarTransparency")) return m_half;
    }
    if (streq(cls, "StarterGui")) {
        if (streq(k, "SetCoreGuiEnabled")) return m_setcoregui;
        if (streq(k, "RegisterSetCore") || streq(k, "RegisterGetCore")) return m_noop;
        if (streq(k, "GetCoreGuiEnabled")) return m_getcoregui;
        if (streq(k, "SetCore")) return m_setcore;
        if (streq(k, "GetCore")) return m_nil;
    }
    return NULL;
}

/* Methods written in Lua (they wait): InvokeServer, Invoke, TweenPosition... */
static bool gui_lua_method(lua_State *L, i32 idx, const char *k)
{
    const char *cls = cls_of(idx);
    bool want = (streq(cls, "RemoteFunction") && (streq(k, "InvokeServer") || streq(k, "InvokeClient"))) ||
                (streq(cls, "BindableFunction") && streq(k, "Invoke")) ||
                (ub_cdb_is_a(cls, "GuiObject") &&
                 (streq(k, "TweenPosition") || streq(k, "TweenSize") || streq(k, "TweenSizeAndPosition")));
    if (!want) return false;
    lua_getfield(L, LUA_REGISTRYINDEX, "ub.GuiLua");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_getfield(L, -1, k);
    lua_remove(L, -2);
    return true;
}

/* -------------------------------------------------------------- events */

int gui_event(i32 idx, const char *k)
{
    if (idx == VIRT_USERINPUT) {
        if (streq(k, "InputBegan")) return EV_INPUTBEGAN;
        if (streq(k, "InputEnded")) return EV_INPUTENDED;
        if (streq(k, "InputChanged")) return EV_INPUTCHANGED;
        if (streq(k, "TextBoxFocused")) return EV_TEXTBOXFOCUSED;
        if (streq(k, "TextBoxFocusReleased")) return EV_TEXTBOXRELEASED;
        if (streq(k, "JumpRequest")) return EV_JUMPREQUEST;
        if (streq(k, "WindowFocused") || streq(k, "WindowFocusReleased") || streq(k, "LastInputTypeChanged") ||
            streq(k, "TouchStarted") || streq(k, "TouchEnded") || streq(k, "TouchMoved") || streq(k, "TouchTap") ||
            streq(k, "GamepadConnected") || streq(k, "GamepadDisconnected") || streq(k, "DeviceAccelerationChanged"))
            return EV_NEVER;
    }
    if (idx == VIRT_MOUSE) {
        if (streq(k, "Button1Down")) return EV_B1DOWN;
        if (streq(k, "Button1Up")) return EV_B1UP;
        if (streq(k, "Button2Down")) return EV_B2DOWN;
        if (streq(k, "Button2Up")) return EV_B2UP;
        if (streq(k, "Move")) return EV_MOVE;
        if (streq(k, "WheelForward")) return EV_WHEELFORWARD;
        if (streq(k, "WheelBackward")) return EV_WHEELBACKWARD;
        if (streq(k, "KeyDown")) return EV_KEYDOWN;
        if (streq(k, "KeyUp")) return EV_KEYUP;
        if (streq(k, "Idle")) return EV_NEVER;
    }
    if (idx == VIRT_GUISERVICE && (streq(k, "MenuOpened") || streq(k, "MenuClosed"))) return EV_NEVER;
    if (idx < 0) return -1;
    const char *cls = cls_of(idx);
    if (streq(cls, "RemoteEvent")) {
        if (streq(k, "OnServerEvent")) return EV_SERVEREVENT;
        if (streq(k, "OnClientEvent")) return EV_CLIENTEVENT;
    }
    if (streq(cls, "BindableEvent") && streq(k, "Event")) return EV_BINDABLE;
    if (ub_cdb_is_a(cls, "GuiObject")) {
        if (streq(k, "MouseEnter")) return EV_MOUSEENTER;
        if (streq(k, "MouseLeave")) return EV_MOUSELEAVE;
        if (streq(k, "MouseMoved")) return EV_MOUSEMOVED;
        if (streq(k, "MouseWheelForward")) return EV_WHEELFORWARD;
        if (streq(k, "MouseWheelBackward")) return EV_WHEELBACKWARD;
        if (streq(k, "InputBegan")) return EV_INPUTBEGAN;
        if (streq(k, "InputEnded")) return EV_INPUTENDED;
        if (streq(k, "InputChanged")) return EV_INPUTCHANGED;
        if (streq(k, "TouchTap") || streq(k, "TouchLongPress") || streq(k, "TouchPan") || streq(k, "TouchPinch") ||
            streq(k, "TouchRotate") || streq(k, "TouchSwipe") || streq(k, "SelectionGained") ||
            streq(k, "SelectionLost") || streq(k, "DragBegin") || streq(k, "DragStopped"))
            return EV_NEVER;
    }
    if (ub_cdb_is_a(cls, "GuiButton")) {
        if (streq(k, "Activated")) return EV_ACTIVATED;
        if (streq(k, "MouseButton1Click")) return EV_B1CLICK;
        if (streq(k, "MouseButton1Down")) return EV_B1DOWN;
        if (streq(k, "MouseButton1Up")) return EV_B1UP;
        if (streq(k, "MouseButton2Click")) return EV_B2CLICK;
        if (streq(k, "MouseButton2Down")) return EV_B2DOWN;
        if (streq(k, "MouseButton2Up")) return EV_B2UP;
    }
    if (streq(cls, "TextBox")) {
        if (streq(k, "Focused")) return EV_FOCUSED;
        if (streq(k, "FocusLost")) return EV_FOCUSLOST;
        if (streq(k, "ReturnPressedFromOnScreenKeyboard")) return EV_NEVER;
    }
    if (streq(cls, "UIPageLayout") && (streq(k, "PageEnter") || streq(k, "PageLeave") || streq(k, "Stopped")))
        return EV_NEVER;
    return -1;
}

/* ------------------------------------------------------- virtual objects */

static const ub_game_input *IN(void) { return &S.g->input; }

static f32 gui_y(f32 screen_y) { return screen_y - UB_GUI_INSET; }

/* The camera's frame in Roblox's axes: columns right, up, back. */
static void camera_frame(vec3 *pos, mat3 *rot)
{
    const ub_camera *c = &IN()->cam;
    f32 sy = ub_sinf(c->yaw), cy = ub_cosf(c->yaw), sp = ub_sinf(c->pitch), cp = ub_cosf(c->pitch);
    vec3 fwd = v3(sy * cp, sp, cy * cp), right = v3(cy, 0, -sy), up = v3_cross(fwd, right);
    *pos = rbx_vec(c->pos);
    rot->x = rbx_vec(right);
    rot->y = rbx_vec(up);
    rot->z = v3_scale(rbx_vec(fwd), -1);
}

/* The ray through a screen point, Roblox axes. */
static void screen_ray(f32 x, f32 y, vec3 *o, vec3 *d)
{
    const ub_game_input *in = IN();
    vec3 uo, ud;
    ub_render_screen_ray(&in->cam, in->screen_w, in->screen_h, x, y, &uo, &ud);
    *o = rbx_vec(uo);
    *d = rbx_vec(ud);
}

/* What the mouse points at: the part and the point (Roblox axes). */
static i32 mouse_target(vec3 *hit)
{
    const ub_game_input *in = IN();
    vec3 uo, ud;
    ub_render_screen_ray(&in->cam, in->screen_w, in->screen_h, in->mouse_x, in->mouse_y, &uo, &ud);
    ub_phys_hit h;
    i32 part = -1;
    vec3 p = v3_add(uo, v3_scale(ud, 1000));
    if (S.g->phys_ok && ub_phys_raycast(&S.g->phys, uo, ud, 5000, S.g->chr.body, &h)) {
        p = h.point;
        part = h.part >= 0 ? ub_world_part_inst(W, h.part) : -1;
    }
    *hit = rbx_vec(p);
    return part;
}

i32 gui_mouse_target(vec3 *hit) { return mouse_target(hit); }
int gui_vk_of(int keycode) { return vk_of(keycode); }

static mat3 look_rot(vec3 dir)
{
    vec3 back = v3_scale(v3_normalize(dir), -1);
    vec3 up = ub_absf(back.y) > 0.99f ? v3(0, 0, 1) : v3(0, 1, 0);
    vec3 right = v3_normalize(v3_cross(up, back));
    mat3 m;
    m.x = right;
    m.y = v3_cross(back, right);
    m.z = back;
    return m;
}

static int m_worldtoviewport(lua_State *L)
{
    vec3 p = rbx_vec(check_v3(L, 2));
    const ub_game_input *in = IN();
    const ub_camera *c = &in->cam;
    f32 sy = ub_sinf(c->yaw), cy = ub_cosf(c->yaw), sp = ub_sinf(c->pitch), cp = ub_cosf(c->pitch);
    vec3 fwd = v3(sy * cp, sp, cy * cp), right = v3(cy, 0, -sy), up = v3_cross(fwd, right);
    vec3 d = v3_sub(p, c->pos);
    f32 z = v3_dot(d, fwd), t = ub_sinf(c->fov_y * 0.5f) / ub_cosf(c->fov_y * 0.5f), h = (f32)(in->screen_h > 0 ? in->screen_h : 1);
    f32 zz = ub_absf(z) > 1e-4f ? z : 1e-4f;
    f32 sx = (f32)in->screen_w * 0.5f + v3_dot(d, right) / (zz * t) * h * 0.5f;
    f32 syy = (f32)in->screen_h * 0.5f - v3_dot(d, up) / (zz * t) * h * 0.5f;
    bool screen = lua_toboolean(L, lua_upvalueindex(1));
    if (screen) syy -= UB_GUI_INSET;
    push_v3(L, v3(sx, syy, z));
    lua_pushboolean(L, z > 0 && sx >= 0 && sx <= (f32)in->screen_w && syy >= (screen ? -UB_GUI_INSET : 0) &&
                           syy <= (f32)in->screen_h);
    return 2;
}

static int m_pointtoray(lua_State *L)
{
    f32 x = numf(L, 2), y = numf(L, 3), depth = optf(L, 4, 0);
    if (lua_toboolean(L, lua_upvalueindex(1))) y += UB_GUI_INSET; /* ScreenPointToRay */
    vec3 o, d;
    screen_ray(x, y, &o, &d);
    push_ray(L, v3_add(o, v3_scale(d, depth)), d);
    return 1;
}

static int m_emptylist(lua_State *L)
{
    lua_newtable(L);
    return 1;
}

static int m_getguiinset(lua_State *L)
{
    push_vector2(L, 0, UB_GUI_INSET);
    push_vector2(L, 0, 0);
    return 2;
}

static int m_false(lua_State *L)
{
    lua_pushboolean(L, 0);
    return 1;
}

static int m_iskeydown(lua_State *L)
{
    int vk = vk_of(check_enum(L, 2, E_KC));
    lua_pushboolean(L, vk > 0 && IN()->key_down[vk]);
    return 1;
}

static int m_ismousedown(lua_State *L)
{
    int t = check_enum(L, 2, E_UIT);
    int b = t == ev_of(E_UIT, "MouseButton1") ? 0 : t == ev_of(E_UIT, "MouseButton2") ? 1 : t == ev_of(E_UIT, "MouseButton3") ? 2 : -1;
    lua_pushboolean(L, b >= 0 && IN()->down[b]);
    return 1;
}

static int m_mouselocation(lua_State *L)
{
    push_vector2(L, IN()->mouse_x, IN()->mouse_y);
    return 1;
}

static int m_mousedelta(lua_State *L)
{
    push_vector2(L, IN()->mouse_dx, IN()->mouse_dy);
    return 1;
}

static int m_keyspressed(lua_State *L)
{
    lua_newtable(L);
    int n = 0;
    for (int vk = 1; vk < 256; ++vk)
        if (IN()->key_down[vk] && keycode_of(vk)) {
            push_input(L, "Keyboard", "Begin", keycode_of(vk), 0, 0, 0, 0, 0);
            lua_rawseti(L, -2, ++n);
        }
    return 1;
}

static int m_focusedbox(lua_State *L)
{
    push_inst(L, GUI ? ub_gui_focused_box(GUI) : -1);
    return 1;
}

static int m_lastinput(lua_State *L)
{
    push_enum(L, E_UIT, ev_of(E_UIT, "Keyboard"));
    return 1;
}

static int m_platform(lua_State *L)
{
    push_enum(L, E_PLATFORM, E_PLATFORM >= 0 ? ev_of(E_PLATFORM, "Windows") : 0);
    return 1;
}

static int m_gettextsize(lua_State *L)
{
    size_t n;
    const char *s = luaL_checklstring(L, 2, &n);
    f32 size = numf(L, 3);
    int font = check_enum(L, 4, E_FONT);
    f32 fw = 0;
    if (is_vector2(L, 5)) fw = cv2(L, 5)->x;
    f32 w, h;
    ub_text_measure(s, (u32)n, ub_text_face_enum(font), size, fw, &w, &h);
    push_vector2(L, w, h);
    return 1;
}

static int filtered_text(lua_State *L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

static int filtered_index(lua_State *L)
{
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushcclosure(L, filtered_text, 1);
    return 1;
}

static int m_filterstring(lua_State *L)
{
    /* offline: nobody else sees the text, so it is returned as it is */
    luaL_checkstring(L, 2);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, filtered_index, 1);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    return 1;
}

static int m_preload(lua_State *L) { return 0; }

static int m_fetchstatus(lua_State *L)
{
    lua_pushnil(L);
    return 1;
}

/* the properties scripts set on virtual objects, kept here */
static struct {
    char mouse_icon[200];
    i32 target_filter;
    int mouse_behavior;
    bool icon_enabled;
    int camera_type;
    i32 camera_subject;
    i32 selected;
} V = { "", -1, 0, true, 0, -1, -1 };

bool gui_virtual_index(lua_State *L, i32 idx, const char *k)
{
    const ub_game_input *in = IN();
    if (idx == VIRT_USERINPUT) {
        if (streq(k, "IsKeyDown")) lua_pushcfunction(L, m_iskeydown);
        else if (streq(k, "IsMouseButtonPressed")) lua_pushcfunction(L, m_ismousedown);
        else if (streq(k, "GetMouseLocation")) lua_pushcfunction(L, m_mouselocation);
        else if (streq(k, "GetMouseDelta")) lua_pushcfunction(L, m_mousedelta);
        else if (streq(k, "GetKeysPressed")) lua_pushcfunction(L, m_keyspressed);
        else if (streq(k, "GetMouseButtonsPressed")) lua_pushcfunction(L, m_emptylist);
        else if (streq(k, "GetFocusedTextBox")) lua_pushcfunction(L, m_focusedbox);
        else if (streq(k, "GetLastInputType")) lua_pushcfunction(L, m_lastinput);
        else if (streq(k, "GetPlatform")) lua_pushcfunction(L, m_platform);
        else if (streq(k, "GetConnectedGamepads") || streq(k, "GetNavigationGamepads") || streq(k, "GetSupportedGamepadKeyCodes"))
            lua_pushcfunction(L, m_emptylist);
        else if (streq(k, "GetGamepadConnected") || streq(k, "IsGamepadButtonDown") || streq(k, "IsNavigationGamepad"))
            lua_pushcfunction(L, m_false);
        else if (streq(k, "MouseEnabled") || streq(k, "KeyboardEnabled")) lua_pushboolean(L, 1);
        else if (streq(k, "TouchEnabled") || streq(k, "GamepadEnabled") || streq(k, "AccelerometerEnabled") ||
                 streq(k, "GyroscopeEnabled") || streq(k, "VREnabled") || streq(k, "OnScreenKeyboardVisible") ||
                 streq(k, "ModalEnabled"))
            lua_pushboolean(L, 0);
        else if (streq(k, "MouseBehavior")) push_enum(L, E_MOUSEBEHAVIOR, V.mouse_behavior);
        else if (streq(k, "MouseIconEnabled")) lua_pushboolean(L, V.icon_enabled);
        else if (streq(k, "MouseDeltaSensitivity")) lua_pushnumber(L, 1);
        else return false;
        return true;
    }
    if (idx == VIRT_MOUSE) {
        if (streq(k, "X")) lua_pushinteger(L, (lua_Integer)in->mouse_x);
        else if (streq(k, "Y")) lua_pushinteger(L, (lua_Integer)gui_y(in->mouse_y));
        else if (streq(k, "ViewSizeX")) lua_pushinteger(L, in->screen_w);
        else if (streq(k, "ViewSizeY")) lua_pushinteger(L, in->screen_h - UB_GUI_INSET);
        else if (streq(k, "Hit")) {
            vec3 p;
            mouse_target(&p);
            vec3 o, d;
            screen_ray(in->mouse_x, in->mouse_y, &o, &d);
            mat3 r = look_rot(d);
            push_cf(L, p, &r);
        } else if (streq(k, "Target")) {
            vec3 p;
            push_inst(L, mouse_target(&p));
        } else if (streq(k, "Origin")) {
            vec3 o, d;
            screen_ray(in->mouse_x, in->mouse_y, &o, &d);
            mat3 r = look_rot(d);
            push_cf(L, o, &r);
        } else if (streq(k, "UnitRay")) {
            vec3 o, d;
            screen_ray(in->mouse_x, in->mouse_y, &o, &d);
            push_ray(L, o, d);
        } else if (streq(k, "Icon")) lua_pushstring(L, V.mouse_icon);
        else if (streq(k, "TargetFilter")) push_inst(L, ub_world_alive(W, V.target_filter) ? V.target_filter : -1);
        else if (streq(k, "TargetSurface")) push_enum(L, enum_type_named("NormalId"), 1);
        else return false;
        return true;
    }
    if (idx == VIRT_CAMERA) {
        if (streq(k, "CFrame") || streq(k, "CoordinateFrame")) {
            vec3 p;
            mat3 r;
            camera_frame(&p, &r);
            push_cf(L, p, &r);
        } else if (streq(k, "Focus")) {
            mat3 id = mat3_identity();
            push_cf(L, rbx_vec(in->cam.focus), &id);
        } else if (streq(k, "FieldOfView")) lua_pushnumber(L, in->cam.fov_y * 180.0 / UB_PI);
        else if (streq(k, "ViewportSize")) push_vector2(L, (f32)in->screen_w, (f32)in->screen_h);
        else if (streq(k, "CameraType")) push_enum(L, E_CAMERATYPE, V.camera_type);
        else if (streq(k, "CameraSubject")) push_inst(L, ub_world_alive(W, V.camera_subject) ? V.camera_subject : S.g->humanoid);
        else if (streq(k, "NearPlaneZ")) lua_pushnumber(L, -0.1);
        else if (streq(k, "HeadScale")) lua_pushnumber(L, 1);
        else if (streq(k, "WorldToViewportPoint") || streq(k, "WorldToScreenPoint")) {
            lua_pushboolean(L, k[5] == 'S');
            lua_pushcclosure(L, m_worldtoviewport, 1);
        } else if (streq(k, "ViewportPointToRay") || streq(k, "ScreenPointToRay")) {
            lua_pushboolean(L, k[0] == 'S');
            lua_pushcclosure(L, m_pointtoray, 1);
        } else if (streq(k, "GetPartsObscuringTarget")) lua_pushcfunction(L, m_emptylist);
        else return false;
        return true;
    }
    if (idx == VIRT_GUISERVICE) {
        if (streq(k, "GetGuiInset")) lua_pushcfunction(L, m_getguiinset);
        else if (streq(k, "IsTenFootInterface")) lua_pushcfunction(L, m_false);
        else if (streq(k, "MenuIsOpen")) lua_pushboolean(L, 0);
        else if (streq(k, "SelectedObject")) push_inst(L, ub_world_alive(W, V.selected) ? V.selected : -1);
        else if (streq(k, "AutoSelectGuiEnabled") || streq(k, "GuiNavigationEnabled") || streq(k, "TouchControlsEnabled"))
            lua_pushboolean(L, 1);
        else return false;
        return true;
    }
    if (idx == VIRT_TEXTSERVICE) {
        if (streq(k, "GetTextSize")) lua_pushcfunction(L, m_gettextsize);
        else if (streq(k, "FilterStringAsync")) lua_pushcfunction(L, m_filterstring);
        else return false;
        return true;
    }
    if (idx == VIRT_CONTENTPROVIDER) {
        if (streq(k, "PreloadAsync") || streq(k, "Preload")) lua_pushcfunction(L, m_preload);
        else if (streq(k, "RequestQueueSize")) lua_pushinteger(L, 0);
        else if (streq(k, "GetAssetFetchStatus")) lua_pushcfunction(L, m_fetchstatus);
        else return false;
        return true;
    }
    return false;
}

bool gui_virtual_newindex(lua_State *L, i32 idx, const char *k, int v)
{
    if (idx == VIRT_MOUSE && streq(k, "Icon")) {
        snprintf(V.mouse_icon, sizeof V.mouse_icon, "%s", luaL_checkstring(L, v));
        return true;
    }
    if (idx == VIRT_MOUSE && streq(k, "TargetFilter")) {
        V.target_filter = opt_inst(L, v);
        return true;
    }
    if (idx == VIRT_USERINPUT && streq(k, "MouseBehavior")) {
        V.mouse_behavior = check_enum(L, v, E_MOUSEBEHAVIOR);
        return true;
    }
    if (idx == VIRT_USERINPUT && streq(k, "MouseIconEnabled")) {
        V.icon_enabled = lua_toboolean(L, v) != 0;
        return true;
    }
    if (idx == VIRT_USERINPUT && (streq(k, "ModalEnabled") || streq(k, "MouseDeltaSensitivity"))) return true;
    if (idx == VIRT_CAMERA) {
        if (streq(k, "FieldOfView")) {
            S.g->script_fov = (f32)ub_clampf(numf(L, v), 1, 120);
            prop_changed(L, idx, k);
            return true;
        }
        if (streq(k, "CameraType")) {
            V.camera_type = check_enum(L, v, E_CAMERATYPE);
            return true;
        }
        if (streq(k, "CameraSubject")) {
            V.camera_subject = opt_inst(L, v);
            return true;
        }
        if (streq(k, "CFrame") || streq(k, "Focus") || streq(k, "CoordinateFrame")) {
            check_cf(L, v); /* a scripted camera comes with milestone 8B; the player's camera stays */
            return true;
        }
    }
    if (idx == VIRT_GUISERVICE && streq(k, "SelectedObject")) {
        V.selected = opt_inst(L, v);
        return true;
    }
    if (idx == VIRT_GUISERVICE && (streq(k, "AutoSelectGuiEnabled") || streq(k, "GuiNavigationEnabled"))) return true;
    return false;
}

/* --------------------------------------------------------------- pump */

static int g_last_sw, g_last_sh;
static f32 g_last_mx = -1, g_last_my = -1;

void gui_pump(lua_State *L)
{
    const ub_game_input *in = IN();
    /* GUI events */
    if (GUI) {
        const ub_gui_event *ev;
        int n = ub_gui_events(GUI, &ev);
        /* copied: handlers may make more */
        ub_gui_event *copy = n ? (ub_gui_event *)plat_alloc(sizeof(ub_gui_event) * (size_t)n) : NULL;
        if (copy) memcpy(copy, ev, sizeof(ub_gui_event) * (size_t)n);
        ub_gui_clear_events(GUI);
        for (int i = 0; copy && i < n; ++i) {
            const ub_gui_event *e = &copy[i];
            if (!ub_world_alive(W, e->inst)) continue;
            static const char *const btn_type[5] = { "MouseButton1", "MouseButton2", "MouseButton3", "MouseMovement",
                                                     "MouseWheel" };
            const char *bt = btn_type[e->button < 5 ? e->button : 3];
            switch (e->type) {
            case UB_GEV_MOUSE_ENTER: case UB_GEV_MOUSE_LEAVE: case UB_GEV_MOUSE_MOVED:
            case UB_GEV_WHEEL_FORWARD: case UB_GEV_WHEEL_BACKWARD:
            case UB_GEV_B1_DOWN: case UB_GEV_B1_UP: case UB_GEV_B2_DOWN: case UB_GEV_B2_UP: {
                static const int map[] = { EV_MOUSEENTER, EV_MOUSELEAVE, EV_MOUSEMOVED, EV_WHEELFORWARD, EV_WHEELBACKWARD,
                                           EV_B1DOWN, EV_B1UP, -1, EV_B2DOWN, EV_B2UP };
                int sig = map[e->type];
                if (sig < 0) break;
                lua_pushinteger(L, e->x);
                lua_pushinteger(L, e->y);
                fire_signal(L, e->inst, sig, NULL, 2);
                break;
            }
            case UB_GEV_B1_CLICK: fire_signal(L, e->inst, EV_B1CLICK, NULL, 0); break;
            case UB_GEV_B2_CLICK: fire_signal(L, e->inst, EV_B2CLICK, NULL, 0); break;
            case UB_GEV_ACTIVATED:
                push_input(L, "MouseButton1", "End", 0, (f32)e->x, (f32)e->y, 0, 0, 0);
                lua_pushinteger(L, e->flag ? e->flag : 1);
                fire_signal(L, e->inst, EV_ACTIVATED, NULL, 2);
                break;
            case UB_GEV_INPUT_BEGAN: case UB_GEV_INPUT_ENDED: case UB_GEV_INPUT_CHANGED: {
                const char *st = e->type == UB_GEV_INPUT_BEGAN ? "Begin" : e->type == UB_GEV_INPUT_ENDED ? "End" : "Change";
                push_input(L, bt, st, 0, (f32)e->x, (f32)e->y, 0, 0, 0);
                fire_signal(L, e->inst, e->type == UB_GEV_INPUT_BEGAN ? EV_INPUTBEGAN : e->type == UB_GEV_INPUT_ENDED
                                                                              ? EV_INPUTENDED : EV_INPUTCHANGED,
                            NULL, 1);
                break;
            }
            case UB_GEV_FOCUSED:
                fire_signal(L, e->inst, EV_FOCUSED, NULL, 0);
                push_inst(L, e->inst);
                fire_signal(L, VIRT_USERINPUT, EV_TEXTBOXFOCUSED, NULL, 1);
                break;
            case UB_GEV_FOCUS_LOST:
                lua_pushboolean(L, e->flag);
                push_input(L, e->flag ? "Keyboard" : "MouseButton1", "Begin", e->flag ? 13 : 0, 0, 0, 0, 0, 0);
                fire_signal(L, e->inst, EV_FOCUSLOST, NULL, 2);
                push_inst(L, e->inst);
                fire_signal(L, VIRT_USERINPUT, EV_TEXTBOXRELEASED, NULL, 1);
                break;
            case UB_GEV_PROP_CHANGED:
                prop_changed(L, e->inst, ub_world_key_name(W, e->key));
                break;
            default: break;
            }
        }
        plat_free(copy);
    }
    /* the camera's ViewportSize */
    if (in->screen_w != g_last_sw || in->screen_h != g_last_sh) {
        bool first = g_last_sw == 0;
        g_last_sw = in->screen_w;
        g_last_sh = in->screen_h;
        if (!first) prop_changed(L, VIRT_CAMERA, "ViewportSize");
    }
    /* UserInputService and the Mouse */
    f32 mx = in->mouse_x, my = gui_y(in->mouse_y);
    for (int vk = 1; vk < 256; ++vk) {
        int kc = keycode_of(vk);
        if (!kc) continue;
        if (in->key_pressed[vk]) {
            push_input(L, "Keyboard", "Begin", kc, 0, 0, 0, 0, 0);
            lua_pushboolean(L, in->gui_keys);
            fire_signal(L, VIRT_USERINPUT, EV_INPUTBEGAN, NULL, 2);
            if (!in->gui_keys) {
                char key[8] = { 0 };
                if (kc >= 32 && kc < 127) key[0] = (char)kc;
                lua_pushstring(L, key);
                fire_signal(L, VIRT_MOUSE, EV_KEYDOWN, NULL, 1);
                if (vk == UB_KEY_SPACE) fire_signal(L, VIRT_USERINPUT, EV_JUMPREQUEST, NULL, 0);
            }
        }
        if (in->key_released[vk]) {
            push_input(L, "Keyboard", "End", kc, 0, 0, 0, 0, 0);
            lua_pushboolean(L, in->gui_keys);
            fire_signal(L, VIRT_USERINPUT, EV_INPUTENDED, NULL, 2);
            if (!in->gui_keys) {
                char key[8] = { 0 };
                if (kc >= 32 && kc < 127) key[0] = (char)kc;
                lua_pushstring(L, key);
                fire_signal(L, VIRT_MOUSE, EV_KEYUP, NULL, 1);
            }
        }
    }
    static const char *const bnames[3] = { "MouseButton1", "MouseButton2", "MouseButton3" };
    for (int b = 0; b < 3; ++b) {
        if (in->pressed[b]) {
            push_input(L, bnames[b], "Begin", 0, mx, my, 0, 0, 0);
            lua_pushboolean(L, in->gui_mouse);
            fire_signal(L, VIRT_USERINPUT, EV_INPUTBEGAN, NULL, 2);
            if (b < 2 && !in->gui_mouse) fire_signal(L, VIRT_MOUSE, b ? EV_B2DOWN : EV_B1DOWN, NULL, 0);
        }
        if (in->released[b]) {
            push_input(L, bnames[b], "End", 0, mx, my, 0, 0, 0);
            lua_pushboolean(L, in->gui_mouse);
            fire_signal(L, VIRT_USERINPUT, EV_INPUTENDED, NULL, 2);
            if (b < 2) fire_signal(L, VIRT_MOUSE, b ? EV_B2UP : EV_B1UP, NULL, 0);
        }
    }
    bool moved = mx != g_last_mx || my != g_last_my || in->mouse_dx || in->mouse_dy;
    if (moved && g_last_mx >= 0) {
        push_input(L, "MouseMovement", "Change", 0, mx, my, 0, in->mouse_dx, in->mouse_dy);
        lua_pushboolean(L, in->gui_mouse);
        fire_signal(L, VIRT_USERINPUT, EV_INPUTCHANGED, NULL, 2);
        fire_signal(L, VIRT_MOUSE, EV_MOVE, NULL, 0);
    }
    g_last_mx = mx;
    g_last_my = my;
    if (in->wheel) {
        push_input(L, "MouseWheel", "Change", 0, mx, my, (f32)in->wheel, 0, 0);
        lua_pushboolean(L, in->gui_mouse);
        fire_signal(L, VIRT_USERINPUT, EV_INPUTCHANGED, NULL, 2);
        fire_signal(L, VIRT_MOUSE, in->wheel > 0 ? EV_WHEELFORWARD : EV_WHEELBACKWARD, NULL, 0);
    }
    /* click detectors, proximity prompts, the humanoid's state */
    static f64 last = -1;
    f64 now = script_time();
    interact_pump(L, last < 0 || now < last ? 0 : (f32)(now - last));
    last = now;
}

/* ------------------------------------------------------------- respawn */

void gui_respawned(lua_State *L)
{
    ub_world *w = W;
    i32 pg = S.g->player_gui;
    if (pg < 0 || !ub_world_alive(w, pg)) return;
    /* what resets: everything but LayerCollectors with ResetOnSpawn off */
    i32 drop[256];
    int nd = 0;
    for (i32 c = w->inst[pg].first_child; c >= 0 && nd < 256; c = w->inst[c].next_sibling) {
        if (!ub_world_alive(w, c)) continue;
        const ub_value *r = ub_world_prop_named(w, c, "ResetOnSpawn");
        bool lc = ub_cdb_is_a(ub_world_str(w, w->inst[c].class_name), "LayerCollector");
        if (!lc || !r || r->type != UB_PV_BOOL || r->v.b) drop[nd++] = c;
    }
    for (int i = 0; i < nd; ++i) {
        push_inst(L, drop[i]);
        lua_getfield(L, -1, "Destroy");
        lua_insert(L, -2);
        lua_call(L, 1, 0);
    }
    i32 added[64];
    int na = ub_game_fill_player_gui(S.g, false, added, 64);
    for (int i = 0; i < na; ++i) {
        ub_game_subtree_changed(S.g, added[i]);
        push_inst(L, added[i]);
        fire_signal(L, pg, EV_CHILDADDED, NULL, 1);
        start_scripts_in(added[i]);
    }
}

/* ---------------------------------------------------------------- setup */

static const char k_gui_lua[] =
    "local base, getcb = ...\n"
    "local M = {}\n"
    "local function call(obj, name, ...)\n"
    "  local f = getcb(obj, name)\n"
    "  while f == nil do base.task.wait() f = getcb(obj, name) end\n"
    "  return f(...)\n"
    "end\n"
    "function M.InvokeServer(self, ...) return call(self, 'OnServerInvoke', base.game.Players.LocalPlayer, ...) end\n"
    "function M.InvokeClient(self, plr, ...) return call(self, 'OnClientInvoke', ...) end\n"
    "function M.Invoke(self, ...) return call(self, 'OnInvoke', ...) end\n"
    "local running = setmetatable({}, { __mode = 'k' })\n"
    "local function tween(self, goals, dir, style, time, override, cb)\n"
    "  local Enum = base.Enum\n"
    "  local cur = running[self]\n"
    "  if cur and cur.PlaybackState == Enum.PlaybackState.Playing then\n"
    "    if not override then return false end\n"
    "    cur:Cancel()\n"
    "  end\n"
    "  local info = base.TweenInfo.new(time or 1, style or Enum.EasingStyle.Quad, dir or Enum.EasingDirection.Out)\n"
    "  local t = base.game:GetService('TweenService'):Create(self, info, goals)\n"
    "  running[self] = t\n"
    "  if cb then\n"
    "    t.Completed:Connect(function(s)\n"
    "      cb(s == Enum.PlaybackState.Completed and Enum.TweenStatus.Completed or Enum.TweenStatus.Canceled)\n"
    "    end)\n"
    "  end\n"
    "  t:Play()\n"
    "  return true\n"
    "end\n"
    "function M.TweenPosition(self, pos, dir, style, time, override, cb)\n"
    "  return tween(self, { Position = pos }, dir, style, time, override, cb)\n"
    "end\n"
    "function M.TweenSize(self, size, dir, style, time, override, cb)\n"
    "  return tween(self, { Size = size }, dir, style, time, override, cb)\n"
    "end\n"
    "function M.TweenSizeAndPosition(self, size, pos, dir, style, time, override, cb)\n"
    "  return tween(self, { Size = size, Position = pos }, dir, style, time, override, cb)\n"
    "end\n"
    "return M\n";

void get_extra_value(lua_State *L, i32 idx, const char *key); /* ub_sapi_inst.c */

static int l_getcb(lua_State *L)
{
    get_extra_value(L, check_inst(L, 1), luaL_checkstring(L, 2));
    return 1;
}

void open_gui(lua_State *L)
{
    extern char REG_BASE;
    interact_reset();
    E_UIT = enum_type_named("UserInputType");
    E_UIS = enum_type_named("UserInputState");
    E_KC = enum_type_named("KeyCode");
    E_FONT = enum_type_named("Font");
    E_FONTWEIGHT = enum_type_named("FontWeight");
    E_FONTSTYLE = enum_type_named("FontStyle");
    E_TWEENSTATUS = enum_type_named("TweenStatus");
    E_CAMERATYPE = enum_type_named("CameraType");
    E_MOUSEBEHAVIOR = enum_type_named("MouseBehavior");
    E_FONTSIZE = enum_type_named("FontSize");
    E_PLATFORM = enum_type_named("Platform");
    memset(&V, 0, sizeof V);
    V.target_filter = V.camera_subject = V.selected = -1;
    V.icon_enabled = true;
    g_last_sw = g_last_sh = 0;
    g_last_mx = g_last_my = -1;

    newmeta(L, "UDim", udim_index, udim_tostring);
    op(L, "__add", udim_add);
    op(L, "__sub", udim_sub);
    op(L, "__unm", udim_unm);
    op(L, "__eq", udim_eq);
    lua_pop(L, 1);
    newmeta(L, "UDim2", udim2_index, udim2_tostring);
    op(L, "__add", udim2_add);
    op(L, "__sub", udim2_sub);
    op(L, "__unm", udim2_unm);
    op(L, "__eq", udim2_eq);
    lua_pop(L, 1);
    newmeta(L, "Vector2", v2_index, v2_tostring);
    op(L, "__add", v2_add);
    op(L, "__sub", v2_sub);
    op(L, "__mul", v2_mul);
    op(L, "__div", v2_div);
    op(L, "__unm", v2_unm);
    op(L, "__eq", v2_eq);
    lua_pop(L, 1);
    newmeta(L, "Rect", rect_index, rect_tostring);
    op(L, "__eq", rect_eq);
    lua_pop(L, 1);
    newmeta(L, "NumberRange", range_index, range_tostring);
    op(L, "__eq", range_eq);
    lua_pop(L, 1);
    newmeta(L, "NumberSequenceKeypoint", nkey_index, NULL);
    lua_pop(L, 1);
    newmeta(L, "ColorSequenceKeypoint", ckey_index, NULL);
    lua_pop(L, 1);
    newmeta(L, "NumberSequence", nseq_index, nseq_tostring);
    op(L, "__eq", seq_eq);
    lua_pop(L, 1);
    newmeta(L, "ColorSequence", cseq_index, cseq_tostring);
    op(L, "__eq", seq_eq);
    lua_pop(L, 1);
    newmeta(L, "Font", font_index, font_tostring);
    op(L, "__eq", font_eq);
    lua_pop(L, 1);
    newmeta(L, "InputObject", input_index, input_tostring);
    lua_pop(L, 1);

    lua_pushlightuserdata(L, &REG_BASE);
    lua_rawget(L, LUA_REGISTRYINDEX);
    int base = lua_gettop(L);
    static const luaL_Reg udim[] = { { "new", udim_new }, { NULL, NULL } };
    static const luaL_Reg udim2[] = { { "new", udim2_new }, { "fromScale", udim2_fromscale },
                                      { "fromOffset", udim2_fromoffset }, { NULL, NULL } };
    static const luaL_Reg v2[] = { { "new", v2_new }, { NULL, NULL } };
    static const luaL_Reg rect[] = { { "new", rect_new }, { NULL, NULL } };
    static const luaL_Reg range[] = { { "new", range_new }, { NULL, NULL } };
    static const luaL_Reg nkey[] = { { "new", nkey_new }, { NULL, NULL } };
    static const luaL_Reg ckey[] = { { "new", ckey_new }, { NULL, NULL } };
    static const luaL_Reg nseq[] = { { "new", nseq_new }, { NULL, NULL } };
    static const luaL_Reg cseq[] = { { "new", cseq_new }, { NULL, NULL } };
    static const luaL_Reg font[] = { { "new", font_new }, { "fromName", font_fromname }, { "fromId", font_fromid },
                                     { "fromEnum", font_fromenum }, { NULL, NULL } };
    lib(L, base, "UDim", udim);
    lib(L, base, "UDim2", udim2);
    lib(L, base, "Vector2", v2);
    lib(L, base, "Rect", rect);
    lib(L, base, "NumberRange", range);
    lib(L, base, "NumberSequenceKeypoint", nkey);
    lib(L, base, "ColorSequenceKeypoint", ckey);
    lib(L, base, "NumberSequence", nseq);
    lib(L, base, "ColorSequence", cseq);
    lib(L, base, "Font", font);
    /* Vector2.zero, .one, .xAxis, .yAxis on the real table */
    lua_getfield(L, base, "Vector2");
    lua_getmetatable(L, -1);
    lua_getfield(L, -1, "__index");
    push_vector2(L, 0, 0);
    lua_setfield(L, -2, "zero");
    push_vector2(L, 1, 1);
    lua_setfield(L, -2, "one");
    push_vector2(L, 1, 0);
    lua_setfield(L, -2, "xAxis");
    push_vector2(L, 0, 1);
    lua_setfield(L, -2, "yAxis");
    lua_pop(L, 3);

    if (luaL_loadbuffer(L, k_gui_lua, sizeof k_gui_lua - 1, "=gui") == 0) {
        lua_pushvalue(L, base);
        lua_pushcfunction(L, l_getcb);
        lua_call(L, 2, 1);
        lua_setfield(L, LUA_REGISTRYINDEX, "ub.GuiLua");
    } else {
        ub_error("scripts: gui helpers: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}
