/* Uniblox - the 2022 value types not in ub_sapi_types.c: Region3,
 * Vector3int16, Vector2int16, Region3int16, OverlapParams,
 * PhysicalProperties, Axes and Faces. All in Roblox's coordinates. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "script/ub_sapi.h"

static const char *const LOCKED = "The metatable is locked";

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static f32 mn(f32 a, f32 b) { return a < b ? a : b; }
static f32 mx(f32 a, f32 b) { return a > b ? a : b; }

static void *test_udata(lua_State *L, int i, const char *name)
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
    void *p = test_udata(L, i, name);
    if (!p) luaL_typerror(L, i, name);
    return p;
}

static void *new_udata(lua_State *L, size_t size, const char *name)
{
    void *p = lua_newuserdata(L, size);
    luaL_getmetatable(L, name);
    lua_setmetatable(L, -2);
    return p;
}

static int not_member(lua_State *L, const char *k, const char *type)
{
    return sapi_error(L, "%s is not a valid member of %s", k, type);
}

/* ------------------------------------------------------------- Region3 */

typedef struct { vec3 lo, hi; } sregion;

static int r3_new(lua_State *L)
{
    vec3 a = lua_isnoneornil(L, 1) ? v3(0, 0, 0) : check_v3(L, 1);
    vec3 b = lua_isnoneornil(L, 2) ? v3(0, 0, 0) : check_v3(L, 2);
    sregion *r = (sregion *)new_udata(L, sizeof(sregion), "Region3");
    r->lo = v3(mn(a.x, b.x), mn(a.y, b.y), mn(a.z, b.z));
    r->hi = v3(mx(a.x, b.x), mx(a.y, b.y), mx(a.z, b.z));
    return 1;
}

static int r3_expand(lua_State *L)
{
    sregion *r = (sregion *)check_udata(L, 1, "Region3");
    f64 g = luaL_checknumber(L, 2);
    if (!(g > 0)) return luaL_argerror(L, 2, "resolution must be positive");
    sregion *o = (sregion *)new_udata(L, sizeof(sregion), "Region3");
    o->lo = v3((f32)(floor(r->lo.x / g) * g), (f32)(floor(r->lo.y / g) * g), (f32)(floor(r->lo.z / g) * g));
    o->hi = v3((f32)(ceil(r->hi.x / g) * g), (f32)(ceil(r->hi.y / g) * g), (f32)(ceil(r->hi.z / g) * g));
    return 1;
}

static int r3_index(lua_State *L)
{
    sregion *r = (sregion *)check_udata(L, 1, "Region3");
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "CFrame")) {
        mat3 id = mat3_identity();
        push_cf(L, v3_scale(v3_add(r->lo, r->hi), 0.5f), &id);
    } else if (streq(k, "Size")) push_v3(L, v3_sub(r->hi, r->lo));
    else if (streq(k, "ExpandToGrid")) lua_pushcfunction(L, r3_expand);
    else return not_member(L, k, "Region3");
    return 1;
}

static int r3_tostring(lua_State *L)
{
    sregion *r = (sregion *)check_udata(L, 1, "Region3");
    vec3 c = v3_scale(v3_add(r->lo, r->hi), 0.5f), s = v3_sub(r->hi, r->lo);
    lua_pushfstring(L, "%f, %f, %f, 1, 0, 0, 0, 1, 0, 0, 0, 1; %f, %f, %f", (f64)c.x, (f64)c.y, (f64)c.z, (f64)s.x,
                    (f64)s.y, (f64)s.z);
    return 1;
}

bool get_region3(lua_State *L, int i, vec3 *lo, vec3 *hi)
{
    sregion *r = (sregion *)test_udata(L, i, "Region3");
    if (!r) return false;
    *lo = r->lo;
    *hi = r->hi;
    return true;
}

/* ------------------------------------------- Vector3int16, Vector2int16 */

typedef struct { i16 v[3]; } sint16;

static i16 to16(f64 d)
{
    i32 v = (i32)d; /* truncated, then wrapped as Roblox does */
    return (i16)(u16)(u32)v;
}

static int i16_new(lua_State *L)
{
    int n = lua_tointeger(L, lua_upvalueindex(1));
    i16 v[3];
    for (int i = 0; i < 3; ++i) v[i] = i < n ? to16(luaL_optnumber(L, i + 1, 0)) : 0; /* before the push */
    sint16 *p = (sint16 *)new_udata(L, sizeof(sint16), n == 3 ? "Vector3int16" : "Vector2int16");
    memcpy(p->v, v, sizeof v);
    return 1;
}

static sint16 *check_i16(lua_State *L, int i, int *n)
{
    sint16 *p = (sint16 *)test_udata(L, i, "Vector3int16");
    *n = 3;
    if (!p) {
        p = (sint16 *)test_udata(L, i, "Vector2int16");
        *n = 2;
    }
    if (!p) luaL_typerror(L, i, "Vector3int16");
    return p;
}

static int i16_index(lua_State *L)
{
    int n;
    sint16 *p = check_i16(L, 1, &n);
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "X")) lua_pushinteger(L, p->v[0]);
    else if (streq(k, "Y")) lua_pushinteger(L, p->v[1]);
    else if (streq(k, "Z") && n == 3) lua_pushinteger(L, p->v[2]);
    else return not_member(L, k, n == 3 ? "Vector3int16" : "Vector2int16");
    return 1;
}

static int i16_arith(lua_State *L)
{
    int op = lua_tointeger(L, lua_upvalueindex(1)), n, m;
    sint16 *a = check_i16(L, 1, &n);
    sint16 *b = check_i16(L, 2, &m);
    sint16 *r = (sint16 *)new_udata(L, sizeof(sint16), n == 3 ? "Vector3int16" : "Vector2int16");
    for (int i = 0; i < 3; ++i) {
        i32 x = a->v[i], y = b->v[i];
        i32 v = op == 0 ? x + y : op == 1 ? x - y : op == 2 ? x * y : (y ? x / y : 0);
        r->v[i] = (i16)(u16)(u32)v;
    }
    return 1;
}

static int i16_eq(lua_State *L)
{
    int n, m;
    sint16 *a = check_i16(L, 1, &n), *b = check_i16(L, 2, &m);
    lua_pushboolean(L, n == m && memcmp(a, b, sizeof *a) == 0);
    return 1;
}

static int i16_tostring(lua_State *L)
{
    int n;
    sint16 *p = check_i16(L, 1, &n);
    if (n == 3) lua_pushfstring(L, "%d, %d, %d", p->v[0], p->v[1], p->v[2]);
    else lua_pushfstring(L, "%d, %d", p->v[0], p->v[1]);
    return 1;
}

/* ---------------------------------------------------------- Region3int16 */

typedef struct { sint16 lo, hi; } sregion16;

static int r16_new(lua_State *L)
{
    int n;
    sregion16 v;
    memset(&v, 0, sizeof v);
    if (!lua_isnoneornil(L, 1)) v.lo = *check_i16(L, 1, &n);
    if (!lua_isnoneornil(L, 2)) v.hi = *check_i16(L, 2, &n);
    *(sregion16 *)new_udata(L, sizeof(sregion16), "Region3int16") = v;
    return 1;
}

static int r16_index(lua_State *L)
{
    sregion16 *r = (sregion16 *)check_udata(L, 1, "Region3int16");
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "Min") || streq(k, "Max")) {
        sint16 *p = (sint16 *)new_udata(L, sizeof(sint16), "Vector3int16");
        *p = streq(k, "Min") ? r->lo : r->hi;
    } else return not_member(L, k, "Region3int16");
    return 1;
}

/* ---------------------------------------------------- PhysicalProperties */

typedef struct { f32 density, friction, elasticity, fweight, eweight; } sphys;

/* density, friction, elasticity, friction weight, elasticity weight of
 * Roblox's materials (its documented material table) */
static const struct { int material; sphys p; } k_mat_phys[] = {
    { 256, { 0.7f, 0.3f, 0.5f, 1, 1 } },     /* Plastic */
    { 272, { 0.7f, 0.2f, 0.5f, 1, 1 } },     /* SmoothPlastic */
    { 288, { 0.7f, 0.3f, 0.2f, 1, 1 } },     /* Neon */
    { 512, { 0.35f, 0.48f, 0.2f, 1, 1 } },   /* Wood */
    { 528, { 0.35f, 0.48f, 0.2f, 1, 1 } },   /* WoodPlanks */
    { 784, { 2.56f, 0.2f, 0.17f, 1, 1 } },   /* Marble */
    { 800, { 2.69f, 0.4f, 0.2f, 1, 1 } },    /* Slate */
    { 816, { 2.4f, 0.7f, 0.2f, 1, 1 } },     /* Concrete */
    { 832, { 2.69f, 0.4f, 0.2f, 1, 1 } },    /* Granite */
    { 848, { 1.92f, 0.8f, 0.15f, 1, 1 } },   /* Brick */
    { 864, { 2.4f, 0.4f, 0.17f, 1, 1 } },    /* Pebble */
    { 880, { 2.69f, 0.5f, 0.17f, 1, 1 } },   /* Cobblestone */
    { 1040, { 7.85f, 0.7f, 0.2f, 1, 1 } },   /* CorrodedMetal */
    { 1056, { 7.85f, 0.35f, 0.25f, 1, 1 } }, /* DiamondPlate */
    { 1072, { 2.7f, 0.4f, 0.25f, 1, 1 } },   /* Foil */
    { 1088, { 7.85f, 0.4f, 0.25f, 1, 1 } },  /* Metal */
    { 1280, { 0.9f, 0.4f, 0.1f, 1, 1 } },    /* Grass */
    { 1296, { 1.6f, 0.5f, 0.05f, 1, 1 } },   /* Sand */
    { 1312, { 0.7f, 0.35f, 0.05f, 1, 1 } },  /* Fabric */
    { 1328, { 0.9f, 0.3f, 0.03f, 1, 1 } },   /* Snow */
    { 1536, { 0.919f, 0.02f, 0.15f, 1, 1 } },/* Ice */
    { 1568, { 2.4f, 0.25f, 0.2f, 1, 1 } },   /* Glass */
};

static int pp_new(lua_State *L)
{
    sphys p = { 0.7f, 0.3f, 0.5f, 1, 1 };
    if (lua_gettop(L) == 1) { /* a material */
        int m = lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : -1;
        if (m < 0) {
            lua_getfield(L, 1, "Value");
            m = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        for (size_t i = 0; i < sizeof k_mat_phys / sizeof k_mat_phys[0]; ++i)
            if (k_mat_phys[i].material == m) p = k_mat_phys[i].p;
    } else {
        p.density = (f32)luaL_checknumber(L, 1);
        p.friction = (f32)luaL_checknumber(L, 2);
        p.elasticity = (f32)luaL_checknumber(L, 3);
        p.fweight = (f32)luaL_optnumber(L, 4, 1);
        p.eweight = (f32)luaL_optnumber(L, 5, 1);
        if (p.density < 0.01f) p.density = 0.01f;
        if (p.density > 100) p.density = 100;
        p.friction = p.friction < 0 ? 0 : p.friction > 2 ? 2 : p.friction;
        p.elasticity = p.elasticity < 0 ? 0 : p.elasticity > 1 ? 1 : p.elasticity;
    }
    *(sphys *)new_udata(L, sizeof(sphys), "PhysicalProperties") = p;
    return 1;
}

static int pp_index(lua_State *L)
{
    sphys *p = (sphys *)check_udata(L, 1, "PhysicalProperties");
    const char *k = luaL_checkstring(L, 2);
    if (streq(k, "Density")) lua_pushnumber(L, p->density);
    else if (streq(k, "Friction")) lua_pushnumber(L, p->friction);
    else if (streq(k, "Elasticity")) lua_pushnumber(L, p->elasticity);
    else if (streq(k, "FrictionWeight")) lua_pushnumber(L, p->fweight);
    else if (streq(k, "ElasticityWeight")) lua_pushnumber(L, p->eweight);
    else return not_member(L, k, "PhysicalProperties");
    return 1;
}

static int pp_tostring(lua_State *L)
{
    sphys *p = (sphys *)check_udata(L, 1, "PhysicalProperties");
    char b[128];
    snprintf(b, sizeof b, "%g, %g, %g, %g, %g", (f64)p->density, (f64)p->friction, (f64)p->elasticity,
             (f64)p->fweight, (f64)p->eweight);
    lua_pushstring(L, b);
    return 1;
}

/* --------------------------------------------------------- Axes, Faces */

/* bits: Right 1, Top 2, Back 4, Left 8, Bottom 16, Front 32 (NormalId
 * order); Axes keep X/Y/Z as Right|Left, Top|Bottom, Back|Front */
typedef struct { u8 bits; } sfaces;

static int faces_new(lua_State *L)
{
    bool axes = lua_tointeger(L, lua_upvalueindex(1)) != 0; /* (0 is true in Lua) */
    u8 bits = 0;
    for (int i = 1, n = lua_gettop(L); i <= n; ++i) {
        int axis_t = enum_type_named("Axis"), normal_t = enum_type_named("NormalId");
        bool is_axis = axis_t >= 0 && is_enum_of(L, i, axis_t);
        if (!is_axis && !(normal_t >= 0 && is_enum_of(L, i, normal_t)))
            return luaL_argerror(L, i, "Enum.NormalId or Enum.Axis expected");
        lua_getfield(L, i, "Value");
        int v = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        if (is_axis && v >= 0 && v <= 2) bits |= (u8)((1u << v) | (1u << (v + 3)));
        else if (!is_axis && v >= 0 && v <= 5) bits |= (u8)(axes ? (1u << (v % 3)) | (1u << (v % 3 + 3)) : 1u << v);
    }
    ((sfaces *)new_udata(L, sizeof(sfaces), axes ? "Axes" : "Faces"))->bits = bits;
    return 1;
}

static int faces_index(lua_State *L)
{
    sfaces *f = (sfaces *)test_udata(L, 1, "Faces");
    bool axes = false;
    if (!f) {
        f = (sfaces *)check_udata(L, 1, "Axes");
        axes = true;
    }
    const char *k = luaL_checkstring(L, 2);
    static const char *const faces[] = { "Right", "Top", "Back", "Left", "Bottom", "Front" };
    static const char *const ax[] = { "X", "Y", "Z" };
    for (int i = 0; i < 6; ++i)
        if (streq(k, faces[i])) {
            lua_pushboolean(L, (f->bits >> i) & 1);
            return 1;
        }
    for (int i = 0; axes && i < 3; ++i)
        if (streq(k, ax[i])) {
            lua_pushboolean(L, (f->bits >> i) & 1);
            return 1;
        }
    return not_member(L, k, axes ? "Axes" : "Faces");
}

/* ------------------------------------------------------- OverlapParams */

static int op_new(lua_State *L)
{
    lua_newtable(L);
    lua_newtable(L);
    lua_setfield(L, -2, "FilterDescendantsInstances");
    push_enum(L, EN_RAYCASTFILTER, 0);
    lua_setfield(L, -2, "FilterType");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "MaxParts");
    lua_pushliteral(L, "Default");
    lua_setfield(L, -2, "CollisionGroup");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "RespectCanCollide");
    luaL_getmetatable(L, "OverlapParams");
    lua_setmetatable(L, -2);
    return 1;
}

/* ---------------------------------------------------------------- setup */

static void meta(lua_State *L, const char *name, lua_CFunction index, lua_CFunction tostr)
{
    luaL_newmetatable(L, name);
    if (index) {
        lua_pushcfunction(L, index);
        lua_setfield(L, -2, "__index");
    }
    if (tostr) {
        lua_pushcfunction(L, tostr);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pushstring(L, name);
    lua_setfield(L, -2, "__type");
    lua_pushstring(L, LOCKED);
    lua_setfield(L, -2, "__metatable");
}

static void op_up(lua_State *L, const char *name, lua_CFunction f, int up)
{
    lua_pushinteger(L, up);
    lua_pushcclosure(L, f, 1);
    lua_setfield(L, -2, name);
}

static void ctor(lua_State *L, int base, const char *name, lua_CFunction f, int up)
{
    lua_newtable(L);
    lua_pushinteger(L, up);
    lua_pushcclosure(L, f, 1);
    lua_setfield(L, -2, "new");
    lua_newtable(L); /* read-only proxy */
    lua_newtable(L);
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_pushstring(L, LOCKED);
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, base, name);
    lua_pop(L, 1);
}

void open_types2(lua_State *L, int base)
{
    meta(L, "Region3", r3_index, r3_tostring);
    lua_pop(L, 1);
    meta(L, "Region3int16", r16_index, NULL);
    lua_pop(L, 1);
    static const char *const int16_types[] = { "Vector3int16", "Vector2int16" };
    for (int i = 0; i < 2; ++i) {
        meta(L, int16_types[i], i16_index, i16_tostring);
        op_up(L, "__add", i16_arith, 0);
        op_up(L, "__sub", i16_arith, 1);
        op_up(L, "__mul", i16_arith, 2);
        op_up(L, "__div", i16_arith, 3);
        lua_pushcfunction(L, i16_eq);
        lua_setfield(L, -2, "__eq");
        lua_pop(L, 1);
    }
    meta(L, "PhysicalProperties", pp_index, pp_tostring);
    lua_pop(L, 1);
    meta(L, "Faces", faces_index, NULL);
    lua_pop(L, 1);
    meta(L, "Axes", faces_index, NULL);
    lua_pop(L, 1);
    luaL_newmetatable(L, "OverlapParams");
    lua_pushliteral(L, "OverlapParams");
    lua_setfield(L, -2, "__type");
    lua_pop(L, 1);

    ctor(L, base, "Region3", r3_new, 0);
    ctor(L, base, "Region3int16", r16_new, 0);
    ctor(L, base, "Vector3int16", i16_new, 3);
    ctor(L, base, "Vector2int16", i16_new, 2);
    ctor(L, base, "PhysicalProperties", pp_new, 0);
    ctor(L, base, "Faces", faces_new, 0);
    ctor(L, base, "Axes", faces_new, 1);
    ctor(L, base, "OverlapParams", op_new, 0);
}
