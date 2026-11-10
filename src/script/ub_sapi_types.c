/* Uniblox - script value types: Vector3, Color3, CFrame, BrickColor,
 * Enum, TweenInfo, Ray, RaycastParams. All in Roblox's coordinates and
 * conventions (right-handed; CFrame.Angles = Rx * Ry * Rz). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "script/ub_sapi.h"
#include "physics/ub_pmath.h"
#include "world/ub_classes.h"
#include "render/ub_texture.h"

static const char *const LOCKED = "The metatable is locked";

static void set_meta(lua_State *L, const char *name)
{
    luaL_getmetatable(L, name);
    lua_setmetatable(L, -2);
}

static void *test_udata(lua_State *L, int i, const char *name)
{
    void *p = lua_touserdata(L, i);
    if (!p || !lua_getmetatable(L, i)) return NULL;
    luaL_getmetatable(L, name);
    bool same = lua_rawequal(L, -1, -2);
    lua_pop(L, 2);
    return same ? p : NULL;
}

static f64 num_field_check(lua_State *L, int i) { return luaL_checknumber(L, i); }

/* --------------------------------------------------------------- Vector3 */

void push_v3(lua_State *L, vec3 v)
{
    sv3 *p = (sv3 *)lua_newuserdata(L, sizeof(sv3));
    p->x = v.x;
    p->y = v.y;
    p->z = v.z;
    set_meta(L, "Vector3");
}

bool is_v3(lua_State *L, int i) { return test_udata(L, i, "Vector3") != NULL; }

vec3 check_v3(lua_State *L, int i)
{
    sv3 *p = (sv3 *)test_udata(L, i, "Vector3");
    if (!p) luaL_typerror(L, i, "Vector3");
    return v3(p->x, p->y, p->z);
}

static int v3_new(lua_State *L)
{
    push_v3(L, v3((f32)luaL_optnumber(L, 1, 0), (f32)luaL_optnumber(L, 2, 0), (f32)luaL_optnumber(L, 3, 0)));
    return 1;
}

/* Vector3.fromNormalId(Enum.NormalId.Top) / fromAxis(Enum.Axis.X): enum
 * values Right 0, Top 1, Back 2, Left 3, Bottom 4, Front 5; X 0, Y 1, Z 2 */
static int v3_fromnormal(lua_State *L)
{
    static const f32 d[6][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 } };
    int v = -1;
    if (lua_isnumber(L, 1)) v = (int)lua_tointeger(L, 1);
    else {
        luaL_checkany(L, 1);
        lua_getfield(L, 1, "Value");
        v = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : -1;
        lua_pop(L, 1);
    }
    if (v < 0 || v > 5) return luaL_argerror(L, 1, "NormalId or Axis expected");
    push_v3(L, v3(d[v][0], d[v][1], d[v][2]));
    return 1;
}

static int v3_dot_l(lua_State *L)
{
    lua_pushnumber(L, v3_dot(check_v3(L, 1), check_v3(L, 2)));
    return 1;
}

static int v3_cross_l(lua_State *L)
{
    push_v3(L, v3_cross(check_v3(L, 1), check_v3(L, 2)));
    return 1;
}

static int v3_lerp_l(lua_State *L)
{
    vec3 a = check_v3(L, 1), b = check_v3(L, 2);
    f32 t = (f32)luaL_checknumber(L, 3);
    push_v3(L, v3_add(a, v3_scale(v3_sub(b, a), t)));
    return 1;
}

static int v3_fuzzy_l(lua_State *L)
{
    vec3 a = check_v3(L, 1), b = check_v3(L, 2);
    f32 eps = (f32)luaL_optnumber(L, 3, 1e-5);
    lua_pushboolean(L, v3_length(v3_sub(a, b)) <= eps);
    return 1;
}

static int v3_abs_l(lua_State *L)
{
    vec3 a = check_v3(L, 1);
    push_v3(L, v3(fabs(a.x), fabs(a.y), fabs(a.z)));
    return 1;
}

/* 2022 additions: Min, Max, Floor, Ceil, Sign, Angle */
static f32 fsign(f32 x) { return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; }

static int v3_minmax_l(lua_State *L)
{
    bool max = lua_toboolean(L, lua_upvalueindex(1));
    vec3 r = check_v3(L, 1);
    for (int i = 2, n = lua_gettop(L); i <= n; ++i) {
        vec3 b = check_v3(L, i);
        r.x = max ? (b.x > r.x ? b.x : r.x) : (b.x < r.x ? b.x : r.x);
        r.y = max ? (b.y > r.y ? b.y : r.y) : (b.y < r.y ? b.y : r.y);
        r.z = max ? (b.z > r.z ? b.z : r.z) : (b.z < r.z ? b.z : r.z);
    }
    push_v3(L, r);
    return 1;
}

static int v3_round_l(lua_State *L) /* upvalue: 0 floor, 1 ceil, 2 sign */
{
    vec3 a = check_v3(L, 1);
    int m = (int)lua_tointeger(L, lua_upvalueindex(1));
    if (m == 0) push_v3(L, v3((f32)floor(a.x), (f32)floor(a.y), (f32)floor(a.z)));
    else if (m == 1) push_v3(L, v3((f32)ceil(a.x), (f32)ceil(a.y), (f32)ceil(a.z)));
    else push_v3(L, v3(fsign(a.x), fsign(a.y), fsign(a.z)));
    return 1;
}

static int v3_angle_l(lua_State *L)
{
    vec3 a = check_v3(L, 1), b = check_v3(L, 2);
    vec3 c = v3_cross(a, b);
    f64 ang = atan2(v3_length(c), v3_dot(a, b));
    if (lua_gettop(L) >= 3 && v3_dot(c, check_v3(L, 3)) < 0) ang = -ang;
    lua_pushnumber(L, ang);
    return 1;
}

static int v3_index(lua_State *L)
{
    vec3 v = check_v3(L, 1);
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "X") || !strcmp(k, "x")) lua_pushnumber(L, v.x);
    else if (!strcmp(k, "Y") || !strcmp(k, "y")) lua_pushnumber(L, v.y);
    else if (!strcmp(k, "Z") || !strcmp(k, "z")) lua_pushnumber(L, v.z);
    else if (!strcmp(k, "Magnitude") || !strcmp(k, "magnitude")) lua_pushnumber(L, v3_length(v));
    else if (!strcmp(k, "Unit") || !strcmp(k, "unit")) push_v3(L, v3_normalize(v));
    else if (!strcmp(k, "Dot")) lua_pushcfunction(L, v3_dot_l);
    else if (!strcmp(k, "Cross")) lua_pushcfunction(L, v3_cross_l);
    else if (!strcmp(k, "Lerp") || !strcmp(k, "lerp")) lua_pushcfunction(L, v3_lerp_l);
    else if (!strcmp(k, "FuzzyEq")) lua_pushcfunction(L, v3_fuzzy_l);
    else if (!strcmp(k, "Abs")) lua_pushcfunction(L, v3_abs_l);
    else if (!strcmp(k, "Min") || !strcmp(k, "Max")) {
        lua_pushboolean(L, k[1] == 'a');
        lua_pushcclosure(L, v3_minmax_l, 1);
    } else if (!strcmp(k, "Floor") || !strcmp(k, "Ceil") || !strcmp(k, "Sign")) {
        lua_pushinteger(L, k[0] == 'F' ? 0 : k[0] == 'C' ? 1 : 2);
        lua_pushcclosure(L, v3_round_l, 1);
    } else if (!strcmp(k, "Angle")) lua_pushcfunction(L, v3_angle_l);
    else return sapi_error(L, "%s is not a valid member of Vector3", k);
    return 1;
}

static int v3_add_l(lua_State *L)
{
    push_v3(L, v3_add(check_v3(L, 1), check_v3(L, 2)));
    return 1;
}

static int v3_sub_l(lua_State *L)
{
    push_v3(L, v3_sub(check_v3(L, 1), check_v3(L, 2)));
    return 1;
}

static int v3_mul_l(lua_State *L)
{
    if (lua_isnumber(L, 1)) push_v3(L, v3_scale(check_v3(L, 2), (f32)lua_tonumber(L, 1)));
    else if (lua_isnumber(L, 2)) push_v3(L, v3_scale(check_v3(L, 1), (f32)lua_tonumber(L, 2)));
    else push_v3(L, v3_mul(check_v3(L, 1), check_v3(L, 2)));
    return 1;
}

static int v3_div_l(lua_State *L)
{
    vec3 a = check_v3(L, 1);
    if (lua_isnumber(L, 2)) {
        f32 d = (f32)lua_tonumber(L, 2);
        push_v3(L, v3(a.x / d, a.y / d, a.z / d));
    } else {
        vec3 b = check_v3(L, 2);
        push_v3(L, v3(a.x / b.x, a.y / b.y, a.z / b.z));
    }
    return 1;
}

static int v3_unm_l(lua_State *L)
{
    push_v3(L, v3_neg(check_v3(L, 1)));
    return 1;
}

static int v3_eq_l(lua_State *L)
{
    vec3 a = check_v3(L, 1), b = check_v3(L, 2);
    lua_pushboolean(L, a.x == b.x && a.y == b.y && a.z == b.z);
    return 1;
}

static int v3_tostring(lua_State *L)
{
    vec3 v = check_v3(L, 1);
    lua_pushfstring(L, "%f, %f, %f", v.x, v.y, v.z);
    /* %f in lua_pushfstring is LUA_NUMBER_FMT (%.14g): "1, 2, 3" */
    return 1;
}

static int readonly(lua_State *L)
{
    return luaL_error(L, "%s cannot be assigned to", lua_tostring(L, 2) ? lua_tostring(L, 2) : "this");
}

/* ---------------------------------------------------------------- Color3 */

void push_c3(lua_State *L, f32 r, f32 g, f32 b)
{
    sc3 *p = (sc3 *)lua_newuserdata(L, sizeof(sc3));
    p->r = r;
    p->g = g;
    p->b = b;
    set_meta(L, "Color3");
}

void push_c3_rgb(lua_State *L, u32 rgb)
{
    push_c3(L, (f32)((rgb >> 16) & 255) / 255.0f, (f32)((rgb >> 8) & 255) / 255.0f, (f32)(rgb & 255) / 255.0f);
}

bool is_c3(lua_State *L, int i) { return test_udata(L, i, "Color3") != NULL; }

static sc3 *check_c3(lua_State *L, int i)
{
    sc3 *p = (sc3 *)test_udata(L, i, "Color3");
    if (!p) luaL_typerror(L, i, "Color3");
    return p;
}

static u32 to8(f32 v)
{
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    return (u32)(v * 255.0f + 0.5f);
}

u32 check_c3_rgb(lua_State *L, int i)
{
    sc3 *c = check_c3(L, i);
    return (to8(c->r) << 16) | (to8(c->g) << 8) | to8(c->b);
}

static int c3_new(lua_State *L)
{
    push_c3(L, (f32)luaL_optnumber(L, 1, 0), (f32)luaL_optnumber(L, 2, 0), (f32)luaL_optnumber(L, 3, 0));
    return 1;
}

static int c3_rgb(lua_State *L)
{
    push_c3(L, (f32)luaL_optnumber(L, 1, 0) / 255.0f, (f32)luaL_optnumber(L, 2, 0) / 255.0f,
            (f32)luaL_optnumber(L, 3, 0) / 255.0f);
    return 1;
}

static void hsv_to_rgb(f32 h, f32 s, f32 v, f32 *r, f32 *g, f32 *b)
{
    h = h - (f32)floor(h);
    int i = (int)(h * 6);
    f32 f = h * 6 - (f32)i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (i % 6) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

static int c3_hsv(lua_State *L)
{
    f32 r, g, b;
    hsv_to_rgb((f32)luaL_checknumber(L, 1), (f32)luaL_checknumber(L, 2), (f32)luaL_checknumber(L, 3), &r, &g, &b);
    push_c3(L, r, g, b);
    return 1;
}

static int c3_hex(lua_State *L)
{
    const char *s = luaL_checkstring(L, 1);
    if (*s == '#') ++s;
    u32 v = 0;
    for (int i = 0; i < 6 && s[i]; ++i) {
        char c = s[i];
        v = v * 16 + (u32)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    push_c3_rgb(L, v & 0xFFFFFF);
    return 1;
}

static int c3_lerp_l(lua_State *L)
{
    sc3 *a = check_c3(L, 1), *b = check_c3(L, 2);
    f32 t = (f32)luaL_checknumber(L, 3);
    push_c3(L, a->r + (b->r - a->r) * t, a->g + (b->g - a->g) * t, a->b + (b->b - a->b) * t);
    return 1;
}

static int c3_tohsv_l(lua_State *L)
{
    sc3 *c = check_c3(L, 1);
    f32 mx = c->r > c->g ? (c->r > c->b ? c->r : c->b) : (c->g > c->b ? c->g : c->b);
    f32 mn = c->r < c->g ? (c->r < c->b ? c->r : c->b) : (c->g < c->b ? c->g : c->b);
    f32 d = mx - mn, h = 0;
    if (d > 0) {
        if (mx == c->r) h = (c->g - c->b) / d + (c->g < c->b ? 6 : 0);
        else if (mx == c->g) h = (c->b - c->r) / d + 2;
        else h = (c->r - c->g) / d + 4;
        h /= 6;
    }
    lua_pushnumber(L, h);
    lua_pushnumber(L, mx > 0 ? d / mx : 0);
    lua_pushnumber(L, mx);
    return 3;
}

static int c3_tohex_l(lua_State *L)
{
    char b[8];
    snprintf(b, sizeof b, "%06X", check_c3_rgb(L, 1));
    lua_pushstring(L, b);
    return 1;
}

static int c3_index(lua_State *L)
{
    sc3 *c = check_c3(L, 1);
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "R") || !strcmp(k, "r")) lua_pushnumber(L, c->r);
    else if (!strcmp(k, "G") || !strcmp(k, "g")) lua_pushnumber(L, c->g);
    else if (!strcmp(k, "B") || !strcmp(k, "b")) lua_pushnumber(L, c->b);
    else if (!strcmp(k, "Lerp") || !strcmp(k, "lerp")) lua_pushcfunction(L, c3_lerp_l);
    else if (!strcmp(k, "ToHSV")) lua_pushcfunction(L, c3_tohsv_l);
    else if (!strcmp(k, "ToHex")) lua_pushcfunction(L, c3_tohex_l);
    else return sapi_error(L, "%s is not a valid member of Color3", k);
    return 1;
}

static int c3_eq_l(lua_State *L)
{
    sc3 *a = check_c3(L, 1), *b = check_c3(L, 2);
    lua_pushboolean(L, a->r == b->r && a->g == b->g && a->b == b->b);
    return 1;
}

static int c3_tostring(lua_State *L)
{
    sc3 *c = check_c3(L, 1);
    lua_pushfstring(L, "%f, %f, %f", c->r, c->g, c->b);
    return 1;
}

/* ---------------------------------------------------------------- CFrame */

void push_cf(lua_State *L, vec3 p, const mat3 *r)
{
    scf *c = (scf *)lua_newuserdata(L, sizeof(scf));
    c->p = p;
    c->r = *r;
    set_meta(L, "CFrame");
}

bool is_cf(lua_State *L, int i) { return test_udata(L, i, "CFrame") != NULL; }

scf *check_cf(lua_State *L, int i)
{
    scf *c = (scf *)test_udata(L, i, "CFrame");
    if (!c) luaL_typerror(L, i, "CFrame");
    return c;
}

static mat3 rot_x(f64 a)
{
    f32 c = (f32)cos(a), s = (f32)sin(a);
    mat3 m = { { 1, 0, 0 }, { 0, c, s }, { 0, -s, c } };
    return m;
}

static mat3 rot_y(f64 a)
{
    f32 c = (f32)cos(a), s = (f32)sin(a);
    mat3 m = { { c, 0, -s }, { 0, 1, 0 }, { s, 0, c } };
    return m;
}

static mat3 rot_z(f64 a)
{
    f32 c = (f32)cos(a), s = (f32)sin(a);
    mat3 m = { { c, s, 0 }, { -s, c, 0 }, { 0, 0, 1 } };
    return m;
}

static mat3 euler_xyz(f64 x, f64 y, f64 z)
{
    mat3 a = rot_x(x), b = rot_y(y), c = rot_z(z);
    mat3 ab = mat3_mul(&a, &b);
    return mat3_mul(&ab, &c);
}

static mat3 euler_yxz(f64 x, f64 y, f64 z)
{
    mat3 a = rot_y(y), b = rot_x(x), c = rot_z(z);
    mat3 ab = mat3_mul(&a, &b);
    return mat3_mul(&ab, &c);
}

/* matrix element row r, column c */
static f32 el(const mat3 *m, int r, int c)
{
    const vec3 *col = c == 0 ? &m->x : c == 1 ? &m->y : &m->z;
    return r == 0 ? col->x : r == 1 ? col->y : col->z;
}

static f64 clamp1(f64 v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

static void to_euler_xyz(const mat3 *m, f64 *x, f64 *y, f64 *z)
{
    *y = asin(clamp1(el(m, 0, 2)));
    if (fabs(el(m, 0, 2)) < 0.9999999) {
        *x = atan2(-el(m, 1, 2), el(m, 2, 2));
        *z = atan2(-el(m, 0, 1), el(m, 0, 0));
    } else {
        *x = atan2(el(m, 2, 1), el(m, 1, 1));
        *z = 0;
    }
}

static void to_euler_yxz(const mat3 *m, f64 *x, f64 *y, f64 *z)
{
    *x = asin(clamp1(-el(m, 1, 2)));
    if (fabs(el(m, 1, 2)) < 0.9999999) {
        *y = atan2(el(m, 0, 2), el(m, 2, 2));
        *z = atan2(el(m, 1, 0), el(m, 1, 1));
    } else {
        *y = atan2(-el(m, 2, 0), el(m, 0, 0));
        *z = 0;
    }
}

static mat3 look_at(vec3 from, vec3 at, vec3 up)
{
    vec3 back = v3_normalize(v3_sub(from, at));
    if (v3_length2(back) < 1e-12f) return mat3_identity();
    vec3 right = v3_cross(up, back);
    if (v3_length2(right) < 1e-10f) right = v3_cross(v3(0, 0, 1), back); /* looking straight up or down */
    right = v3_normalize(right);
    mat3 m;
    m.x = right;
    m.y = v3_cross(back, right);
    m.z = back;
    return m;
}

static int cf_new(lua_State *L)
{
    int n = lua_gettop(L);
    mat3 id = mat3_identity();
    if (n == 0) {
        push_cf(L, v3(0, 0, 0), &id);
    } else if (is_v3(L, 1)) {
        vec3 p = check_v3(L, 1);
        if (n >= 2 && is_v3(L, 2)) {
            mat3 r = look_at(p, check_v3(L, 2), v3(0, 1, 0));
            push_cf(L, p, &r);
        } else {
            push_cf(L, p, &id);
        }
    } else if (n >= 12) {
        f32 v[12];
        for (int i = 0; i < 12; ++i) v[i] = (f32)num_field_check(L, i + 1);
        mat3 r;
        r.x = v3(v[3], v[6], v[9]);
        r.y = v3(v[4], v[7], v[10]);
        r.z = v3(v[5], v[8], v[11]);
        push_cf(L, v3(v[0], v[1], v[2]), &r);
    } else if (n >= 7) { /* x, y, z, qx, qy, qz, qw */
        quat q = { (f32)luaL_checknumber(L, 4), (f32)luaL_checknumber(L, 5), (f32)luaL_checknumber(L, 6),
                   (f32)luaL_checknumber(L, 7) };
        mat3 r = quat_to_mat3(quat_normalize(q));
        push_cf(L, v3((f32)luaL_checknumber(L, 1), (f32)luaL_checknumber(L, 2), (f32)luaL_checknumber(L, 3)), &r);
    } else {
        push_cf(L, v3((f32)luaL_checknumber(L, 1), (f32)luaL_optnumber(L, 2, 0), (f32)luaL_optnumber(L, 3, 0)), &id);
    }
    return 1;
}

static int cf_angles(lua_State *L)
{
    mat3 r = euler_xyz(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3));
    push_cf(L, v3(0, 0, 0), &r);
    return 1;
}

static int cf_yxz(lua_State *L)
{
    mat3 r = euler_yxz(luaL_checknumber(L, 1), luaL_checknumber(L, 2), luaL_checknumber(L, 3));
    push_cf(L, v3(0, 0, 0), &r);
    return 1;
}

static int cf_axisangle(lua_State *L)
{
    vec3 a = v3_normalize(check_v3(L, 1));
    f64 t = luaL_checknumber(L, 2);
    f32 s = (f32)sin(t / 2);
    quat q = { a.x * s, a.y * s, a.z * s, (f32)cos(t / 2) };
    mat3 r = quat_to_mat3(q);
    push_cf(L, v3(0, 0, 0), &r);
    return 1;
}

static int cf_lookat(lua_State *L)
{
    vec3 p = check_v3(L, 1), at = check_v3(L, 2);
    vec3 up = lua_gettop(L) >= 3 ? check_v3(L, 3) : v3(0, 1, 0);
    mat3 r = look_at(p, at, up);
    push_cf(L, p, &r);
    return 1;
}

static int cf_frommatrix(lua_State *L)
{
    vec3 p = check_v3(L, 1), vx = check_v3(L, 2), vy = check_v3(L, 3);
    vec3 vz = lua_gettop(L) >= 4 ? check_v3(L, 4) : v3_cross(vx, vy);
    mat3 r = { vx, vy, vz };
    push_cf(L, p, &r);
    return 1;
}

static void cf_mul(const scf *a, const scf *b, scf *out)
{
    out->r = mat3_mul(&a->r, &b->r);
    out->p = v3_add(a->p, mat3_apply(&a->r, b->p));
}

static void cf_inverse(const scf *a, scf *out)
{
    out->r = mat3_transpose(&a->r);
    out->p = v3_neg(mat3_apply(&out->r, a->p));
}

static int cf_inverse_l(lua_State *L)
{
    scf r;
    cf_inverse(check_cf(L, 1), &r);
    push_cf(L, r.p, &r.r);
    return 1;
}

static int cf_lerp_l(lua_State *L)
{
    scf *a = check_cf(L, 1), *b = check_cf(L, 2);
    f32 t = (f32)luaL_checknumber(L, 3);
    quat qa = quat_from_mat3(&a->r), qb = quat_from_mat3(&b->r);
    mat3 r = quat_to_mat3(quat_nlerp(qa, qb, t));
    push_cf(L, v3_add(a->p, v3_scale(v3_sub(b->p, a->p), t)), &r);
    return 1;
}

static int cf_toworld_l(lua_State *L)
{
    scf *a = check_cf(L, 1), r;
    cf_mul(a, check_cf(L, 2), &r);
    push_cf(L, r.p, &r.r);
    return 1;
}

static int cf_toobject_l(lua_State *L)
{
    scf inv, r;
    cf_inverse(check_cf(L, 1), &inv);
    cf_mul(&inv, check_cf(L, 2), &r);
    push_cf(L, r.p, &r.r);
    return 1;
}

static int cf_ptworld_l(lua_State *L)
{
    scf *a = check_cf(L, 1);
    push_v3(L, v3_add(a->p, mat3_apply(&a->r, check_v3(L, 2))));
    return 1;
}

static int cf_ptobject_l(lua_State *L)
{
    scf *a = check_cf(L, 1);
    push_v3(L, mat3_apply_t(&a->r, v3_sub(check_v3(L, 2), a->p)));
    return 1;
}

static int cf_vecworld_l(lua_State *L)
{
    push_v3(L, mat3_apply(&check_cf(L, 1)->r, check_v3(L, 2)));
    return 1;
}

static int cf_vecobject_l(lua_State *L)
{
    push_v3(L, mat3_apply_t(&check_cf(L, 1)->r, check_v3(L, 2)));
    return 1;
}

static int cf_components_l(lua_State *L)
{
    scf *a = check_cf(L, 1);
    lua_pushnumber(L, a->p.x);
    lua_pushnumber(L, a->p.y);
    lua_pushnumber(L, a->p.z);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) lua_pushnumber(L, el(&a->r, r, c));
    return 12;
}

static int cf_toxyz_l(lua_State *L)
{
    f64 x, y, z;
    to_euler_xyz(&check_cf(L, 1)->r, &x, &y, &z);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, z);
    return 3;
}

static int cf_toyxz_l(lua_State *L)
{
    f64 x, y, z;
    to_euler_yxz(&check_cf(L, 1)->r, &x, &y, &z);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, z);
    return 3;
}

static int cf_toaxisangle_l(lua_State *L)
{
    scf *c = check_cf(L, 1);
    quat q = quat_normalize(quat_from_mat3(&c->r));
    if (q.w < 0) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
    f64 s = sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    f64 ang = 2 * atan2(s, q.w);
    if (s < 1e-9) push_v3(L, v3(1, 0, 0));
    else push_v3(L, v3((f32)(q.x / s), (f32)(q.y / s), (f32)(q.z / s)));
    lua_pushnumber(L, ang);
    return 2;
}

static int cf_fuzzy_l(lua_State *L)
{
    scf *a = check_cf(L, 1), *b = check_cf(L, 2);
    f32 eps = (f32)luaL_optnumber(L, 3, 1e-5);
    const f32 *x = (const f32 *)&a->r, *y = (const f32 *)&b->r;
    bool same = v3_length(v3_sub(a->p, b->p)) <= eps;
    for (int i = 0; i < 9 && same; ++i) same = fabs(x[i] - y[i]) <= eps;
    lua_pushboolean(L, same);
    return 1;
}

static int cf_ortho_l(lua_State *L)
{
    scf *c = check_cf(L, 1);
    vec3 x = v3_normalize(c->r.x);
    vec3 y = v3_normalize(v3_sub(c->r.y, v3_scale(x, v3_dot(x, c->r.y))));
    mat3 r;
    r.x = x;
    r.y = y;
    r.z = v3_cross(x, y);
    push_cf(L, c->p, &r);
    return 1;
}

static int cf_index(lua_State *L)
{
    scf *c = check_cf(L, 1);
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Position") || !strcmp(k, "p")) push_v3(L, c->p);
    else if (!strcmp(k, "X") || !strcmp(k, "x")) lua_pushnumber(L, c->p.x);
    else if (!strcmp(k, "Y") || !strcmp(k, "y")) lua_pushnumber(L, c->p.y);
    else if (!strcmp(k, "Z") || !strcmp(k, "z")) lua_pushnumber(L, c->p.z);
    else if (!strcmp(k, "LookVector") || !strcmp(k, "lookVector")) push_v3(L, v3_neg(c->r.z));
    else if (!strcmp(k, "RightVector") || !strcmp(k, "rightVector") || !strcmp(k, "XVector")) push_v3(L, c->r.x);
    else if (!strcmp(k, "UpVector") || !strcmp(k, "upVector") || !strcmp(k, "YVector")) push_v3(L, c->r.y);
    else if (!strcmp(k, "ZVector")) push_v3(L, c->r.z);
    else if (!strcmp(k, "Rotation")) push_cf(L, v3(0, 0, 0), &c->r);
    else if (!strcmp(k, "Inverse") || !strcmp(k, "inverse")) lua_pushcfunction(L, cf_inverse_l);
    else if (!strcmp(k, "Lerp") || !strcmp(k, "lerp")) lua_pushcfunction(L, cf_lerp_l);
    else if (!strcmp(k, "ToWorldSpace") || !strcmp(k, "toWorldSpace")) lua_pushcfunction(L, cf_toworld_l);
    else if (!strcmp(k, "ToObjectSpace") || !strcmp(k, "toObjectSpace")) lua_pushcfunction(L, cf_toobject_l);
    else if (!strcmp(k, "PointToWorldSpace") || !strcmp(k, "pointToWorldSpace")) lua_pushcfunction(L, cf_ptworld_l);
    else if (!strcmp(k, "PointToObjectSpace") || !strcmp(k, "pointToObjectSpace")) lua_pushcfunction(L, cf_ptobject_l);
    else if (!strcmp(k, "VectorToWorldSpace") || !strcmp(k, "vectorToWorldSpace")) lua_pushcfunction(L, cf_vecworld_l);
    else if (!strcmp(k, "VectorToObjectSpace") || !strcmp(k, "vectorToObjectSpace")) lua_pushcfunction(L, cf_vecobject_l);
    else if (!strcmp(k, "GetComponents") || !strcmp(k, "components")) lua_pushcfunction(L, cf_components_l);
    else if (!strcmp(k, "ToEulerAnglesXYZ") || !strcmp(k, "toEulerAnglesXYZ")) lua_pushcfunction(L, cf_toxyz_l);
    else if (!strcmp(k, "ToEulerAnglesYXZ") || !strcmp(k, "ToOrientation")) lua_pushcfunction(L, cf_toyxz_l);
    else if (!strcmp(k, "ToAxisAngle") || !strcmp(k, "toAxisAngle")) lua_pushcfunction(L, cf_toaxisangle_l);
    else if (!strcmp(k, "FuzzyEq")) lua_pushcfunction(L, cf_fuzzy_l);
    else if (!strcmp(k, "Orthonormalize")) lua_pushcfunction(L, cf_ortho_l);
    else return sapi_error(L, "%s is not a valid member of CFrame", k);
    return 1;
}

static int cf_mul_l(lua_State *L)
{
    scf *a = check_cf(L, 1);
    if (is_v3(L, 2)) {
        push_v3(L, v3_add(a->p, mat3_apply(&a->r, check_v3(L, 2))));
    } else {
        scf r;
        cf_mul(a, check_cf(L, 2), &r);
        push_cf(L, r.p, &r.r);
    }
    return 1;
}

static int cf_add_l(lua_State *L)
{
    scf *a = check_cf(L, 1);
    push_cf(L, v3_add(a->p, check_v3(L, 2)), &a->r);
    return 1;
}

static int cf_sub_l(lua_State *L)
{
    scf *a = check_cf(L, 1);
    push_cf(L, v3_sub(a->p, check_v3(L, 2)), &a->r);
    return 1;
}

static int cf_eq_l(lua_State *L)
{
    scf *a = check_cf(L, 1), *b = check_cf(L, 2);
    lua_pushboolean(L, memcmp(a, b, sizeof *a) == 0);
    return 1;
}

static int cf_tostring(lua_State *L)
{
    scf *c = check_cf(L, 1);
    char b[400];
    snprintf(b, sizeof b, "%.14g, %.14g, %.14g, %.14g, %.14g, %.14g, %.14g, %.14g, %.14g, %.14g, %.14g, %.14g", c->p.x,
             c->p.y, c->p.z, el(&c->r, 0, 0), el(&c->r, 0, 1), el(&c->r, 0, 2), el(&c->r, 1, 0), el(&c->r, 1, 1),
             el(&c->r, 1, 2), el(&c->r, 2, 0), el(&c->r, 2, 1), el(&c->r, 2, 2));
    lua_pushstring(L, b);
    return 1;
}

/* ------------------------------------------------------------ BrickColor */

typedef struct {
    u16 number;
    u32 rgb;
    const char *name;
} brick;

static const brick k_bricks[] = {
    { 1, 0xF2F3F3, "White" }, { 2, 0xA1A5A2, "Grey" }, { 5, 0xD7C59A, "Brick yellow" }, { 18, 0xCC8E69, "Nougat" },
    { 21, 0xC4281C, "Bright red" }, { 23, 0x0D69AC, "Bright blue" }, { 24, 0xF5CD30, "Bright yellow" },
    { 26, 0x1B2A35, "Black" }, { 28, 0x287F47, "Dark green" }, { 29, 0xA1C48C, "Medium green" },
    { 37, 0x4B974B, "Bright green" }, { 38, 0xA05F35, "Dark orange" }, { 45, 0xB4D2E4, "Light blue" },
    { 101, 0xDA867A, "Medium red" }, { 102, 0x6E99CA, "Medium blue" }, { 104, 0x6B327C, "Bright violet" },
    { 105, 0xE29B40, "Br. yellowish orange" }, { 106, 0xDA8541, "Bright orange" },
    { 107, 0x008F9C, "Bright bluish green" }, { 119, 0xA4BD47, "Br. yellowish green" },
    { 125, 0xEAB892, "Light orange" }, { 135, 0x74869D, "Sand blue" }, { 141, 0x27462D, "Earth green" },
    { 151, 0x789082, "Sand green" }, { 153, 0x957977, "Sand red" }, { 192, 0x694028, "Reddish brown" },
    { 194, 0xA3A2A5, "Medium stone grey" }, { 199, 0x635F62, "Dark stone grey" },
    { 208, 0xE5E4DF, "Light stone grey" }, { 217, 0x7C5C46, "Brown" }, { 226, 0xFDEA8D, "Cool yellow" },
    { 1001, 0xF8F8F8, "Institutional white" }, { 1002, 0xCDCDCD, "Mid gray" }, { 1003, 0x111111, "Really black" },
    { 1004, 0xFF0000, "Really red" }, { 1005, 0xFFB000, "Deep orange" }, { 1006, 0xB480FF, "Alder" },
    { 1007, 0xA34B4B, "Dusty Rose" }, { 1008, 0xC1BE42, "Olive" }, { 1009, 0xFFFF00, "New Yeller" },
    { 1010, 0x0000FF, "Really blue" }, { 1011, 0x002060, "Navy blue" }, { 1012, 0x2154B9, "Deep blue" },
    { 1013, 0x04AFEC, "Cyan" }, { 1014, 0xAA5500, "CGA brown" }, { 1015, 0xAA00AA, "Magenta" },
    { 1016, 0xFF66CC, "Pink" }, { 1017, 0xFFAF00, "Deep orange" }, { 1018, 0x12EED4, "Teal" },
    { 1019, 0x00FFFF, "Toothpaste" }, { 1020, 0x00FF00, "Lime green" }, { 1021, 0x3A7D15, "Camo" },
    { 1022, 0x7F8E64, "Grime" }, { 1023, 0x8C5B9F, "Lavender" }, { 1024, 0xAFDDFF, "Pastel light blue" },
    { 1025, 0xFFC9C9, "Pastel orange" }, { 1026, 0xB1A7FF, "Pastel violet" }, { 1027, 0x9FF3E9, "Pastel blue-green" },
    { 1028, 0xCCFFCC, "Pastel green" }, { 1029, 0xFFFFCC, "Pastel yellow" }, { 1030, 0xFFCC99, "Pastel brown" },
    { 1031, 0x6225D1, "Royal purple" }, { 1032, 0xFF00BF, "Hot pink" },
};
#define BRICKS (int)(sizeof k_bricks / sizeof k_bricks[0])

static int nearest_brick(u32 rgb)
{
    int best = 26, bd = 1 << 30;
    for (int i = 0; i < BRICKS; ++i) {
        int dr = (int)((rgb >> 16) & 255) - (int)((k_bricks[i].rgb >> 16) & 255);
        int dg = (int)((rgb >> 8) & 255) - (int)((k_bricks[i].rgb >> 8) & 255);
        int db = (int)(rgb & 255) - (int)(k_bricks[i].rgb & 255);
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

u32 brickcolor_number_rgb(u32 number)
{
    for (int i = 0; i < (int)(sizeof k_bricks / sizeof k_bricks[0]); ++i)
        if (k_bricks[i].number == number) return k_bricks[i].rgb;
    return 0xA3A2A5; /* Medium stone grey */
}

u32 brickcolor_rgb_number(u32 rgb) { return k_bricks[nearest_brick(rgb)].number; }

void push_brickcolor(lua_State *L, u32 rgb)
{
    i32 *p = (i32 *)lua_newuserdata(L, sizeof(i32));
    *p = nearest_brick(rgb);
    set_meta(L, "BrickColor");
}

bool is_brickcolor(lua_State *L, int i) { return test_udata(L, i, "BrickColor") != NULL; }

u32 brickcolor_rgb(lua_State *L, int i)
{
    i32 *p = (i32 *)test_udata(L, i, "BrickColor");
    if (!p) luaL_typerror(L, i, "BrickColor");
    return k_bricks[*p].rgb;
}

static int bc_push_index(lua_State *L, int idx)
{
    i32 *p = (i32 *)lua_newuserdata(L, sizeof(i32));
    *p = idx;
    set_meta(L, "BrickColor");
    return 1;
}

static int bc_new(lua_State *L)
{
    if (lua_type(L, 1) == LUA_TSTRING) {
        const char *n = lua_tostring(L, 1);
        for (int i = 0; i < BRICKS; ++i)
            if (!strcmp(k_bricks[i].name, n)) return bc_push_index(L, i);
        return bc_push_index(L, 26); /* Roblox falls back to Medium stone grey */
    }
    if (is_c3(L, 1)) return bc_push_index(L, nearest_brick(check_c3_rgb(L, 1)));
    if (lua_gettop(L) >= 3) {
        u32 rgb = (to8((f32)luaL_checknumber(L, 1)) << 16) | (to8((f32)luaL_checknumber(L, 2)) << 8) |
                  to8((f32)luaL_checknumber(L, 3));
        return bc_push_index(L, nearest_brick(rgb));
    }
    int num = (int)luaL_optinteger(L, 1, 194);
    for (int i = 0; i < BRICKS; ++i)
        if (k_bricks[i].number == num) return bc_push_index(L, i);
    return bc_push_index(L, 26);
}

static int bc_random(lua_State *L)
{
    static u32 seed = 12345;
    seed = seed * 1103515245u + 12345u;
    return bc_push_index(L, (int)((seed >> 16) % BRICKS));
}

static int bc_named(lua_State *L)
{
    return bc_push_index(L, (int)lua_tointeger(L, lua_upvalueindex(1)));
}

static int bc_index(lua_State *L)
{
    i32 i = *(i32 *)luaL_checkudata(L, 1, "BrickColor");
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Name")) lua_pushstring(L, k_bricks[i].name);
    else if (!strcmp(k, "Number")) lua_pushinteger(L, k_bricks[i].number);
    else if (!strcmp(k, "Color")) push_c3_rgb(L, k_bricks[i].rgb);
    else if (!strcmp(k, "r")) lua_pushnumber(L, (f64)((k_bricks[i].rgb >> 16) & 255) / 255.0);
    else if (!strcmp(k, "g")) lua_pushnumber(L, (f64)((k_bricks[i].rgb >> 8) & 255) / 255.0);
    else if (!strcmp(k, "b")) lua_pushnumber(L, (f64)(k_bricks[i].rgb & 255) / 255.0);
    else return sapi_error(L, "%s is not a valid member of BrickColor", k);
    return 1;
}

static int bc_eq_l(lua_State *L)
{
    lua_pushboolean(L, *(i32 *)luaL_checkudata(L, 1, "BrickColor") == *(i32 *)luaL_checkudata(L, 2, "BrickColor"));
    return 1;
}

static int bc_tostring(lua_State *L)
{
    lua_pushstring(L, k_bricks[*(i32 *)luaL_checkudata(L, 1, "BrickColor")].name);
    return 1;
}

/* ----------------------------------------------------------------- Enum */

typedef struct {
    const char *name;
    int value;
} enum_item;

static const enum_item k_material[] = {
    { "Plastic", 256 }, { "SmoothPlastic", 272 }, { "Neon", 288 }, { "Wood", 512 }, { "WoodPlanks", 528 },
    { "Marble", 784 }, { "Basalt", 788 }, { "Slate", 800 }, { "CrackedLava", 804 }, { "Concrete", 816 },
    { "Limestone", 820 }, { "Granite", 832 }, { "Pavement", 836 }, { "Brick", 848 }, { "Pebble", 864 },
    { "Cobblestone", 880 }, { "Rock", 896 }, { "Sandstone", 912 }, { "CorrodedMetal", 1040 },
    { "DiamondPlate", 1056 }, { "Foil", 1072 }, { "Metal", 1088 }, { "Grass", 1280 }, { "LeafyGrass", 1284 },
    { "Sand", 1296 }, { "Fabric", 1312 }, { "Snow", 1328 }, { "Mud", 1344 }, { "Ground", 1360 },
    { "Asphalt", 1376 }, { "Salt", 1392 }, { "Ice", 1536 }, { "Glacier", 1552 }, { "Glass", 1568 },
    { "ForceField", 1584 }, { "Air", 1792 }, { "Water", 2048 }, { NULL, 0 },
};
static const enum_item k_parttype[] = { { "Ball", 0 }, { "Block", 1 }, { "Cylinder", 2 }, { "Wedge", 3 },
                                        { "CornerWedge", 4 }, { NULL, 0 } };
static const enum_item k_easingstyle[] = { { "Linear", 0 }, { "Sine", 1 }, { "Back", 2 }, { "Quad", 3 },
                                           { "Quart", 4 }, { "Quint", 5 }, { "Bounce", 6 }, { "Elastic", 7 },
                                           { "Exponential", 8 }, { "Circular", 9 }, { "Cubic", 10 }, { NULL, 0 } };
static const enum_item k_easingdir[] = { { "In", 0 }, { "Out", 1 }, { "InOut", 2 }, { NULL, 0 } };
static const enum_item k_playback[] = { { "Begin", 0 }, { "Delayed", 1 }, { "Playing", 2 }, { "Paused", 3 },
                                        { "Completed", 4 }, { "Cancelled", 5 }, { NULL, 0 } };
static const enum_item k_raycastfilter[] = { { "Exclude", 0 }, { "Include", 1 }, { "Blacklist", 0 },
                                             { "Whitelist", 1 }, { NULL, 0 } };
static const enum_item k_humanoidstate[] = { { "FallingDown", 0 }, { "Ragdoll", 1 }, { "GettingUp", 2 },
                                             { "Jumping", 3 }, { "Swimming", 4 }, { "Freefall", 5 },
                                             { "Flying", 6 }, { "Landed", 7 }, { "Running", 8 },
                                             { "RunningNoPhysics", 10 }, { "StrafingNoPhysics", 11 },
                                             { "Climbing", 12 }, { "Seated", 13 }, { "PlatformStanding", 14 },
                                             { "Dead", 15 }, { "Physics", 16 }, { "None", 18 }, { NULL, 0 } };
static const enum_item k_keycode[] = { { "Unknown", 0 }, { "Space", 32 }, { "W", 119 }, { "A", 97 }, { "S", 115 },
                                       { "D", 100 }, { "E", 101 }, { "F", 102 }, { "Q", 113 }, { "R", 114 },
                                       { "LeftShift", 304 }, { NULL, 0 } };
static const enum_item k_surface[] = { { "Smooth", 0 }, { "Glue", 1 }, { "Weld", 2 }, { "Studs", 3 }, { "Inlet", 4 },
                                       { "Universal", 5 }, { "Hinge", 6 }, { "Motor", 7 }, { "SteppingMotor", 8 },
                                       { "SmoothNoOutlines", 10 }, { NULL, 0 } };

static const struct {
    const char *name;
    const enum_item *items;
} k_enums[EN_COUNT] = {
    { "Material", k_material }, { "PartType", k_parttype }, { "EasingStyle", k_easingstyle },
    { "EasingDirection", k_easingdir }, { "PlaybackState", k_playback }, { "RaycastFilterType", k_raycastfilter },
    { "HumanoidStateType", k_humanoidstate }, { "KeyCode", k_keycode }, { "SurfaceType", k_surface },
};

typedef struct {
    u16 type, value;
} senum;

static char REG_ENUMITEMS;

static int m_enum_isa(lua_State *L);

/* Types below EN_COUNT are the tables above; EN_COUNT + i is enum i of
 * the class table (world/ub_classes.h: Roblox's own list). */
static bool is_gen(int t) { return t >= EN_COUNT; }

static const char *etype_name(int t)
{
    if (is_gen(t)) return ub_enum_name(t - EN_COUNT);
    return t >= 0 && t < EN_COUNT ? k_enums[t].name : "?";
}

int enum_type_named(const char *name)
{
    /* KeyCode: Roblox's full list rather than the few keys above */
    int g = ub_enum_find(name);
    if (g >= 0 && !strcmp(name, "KeyCode")) return EN_COUNT + g;
    for (int t = 0; t < EN_COUNT; ++t)
        if (!strcmp(k_enums[t].name, name)) return t;
    return g >= 0 ? EN_COUNT + g : -1;
}

const char *enum_item_name(int type, int value)
{
    if (is_gen(type)) {
        const char *n = ub_enum_name_of_value(type - EN_COUNT, value);
        return n ? n : "?";
    }
    if (type < 0 || type >= EN_COUNT) return "?";
    for (const enum_item *e = k_enums[type].items; e->name; ++e)
        if (e->value == value) return e->name;
    return "?";
}

static int enum_value_named(int type, const char *n)
{
    if (is_gen(type)) return ub_enum_value_of(type - EN_COUNT, n);
    for (const enum_item *it = k_enums[type].items; it->name; ++it)
        if (!strcmp(it->name, n)) return it->value;
    return -1;
}

void push_enum(lua_State *L, int type, int value)
{
    /* one userdata per item, so they work as table keys */
    lua_pushlightuserdata(L, &REG_ENUMITEMS);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, -1, type * 65536 + (value & 0xFFFF));
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        senum *e = (senum *)lua_newuserdata(L, sizeof(senum));
        e->type = (u16)type;
        e->value = (u16)value;
        set_meta(L, "EnumItem");
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, type * 65536 + (value & 0xFFFF));
    }
    lua_remove(L, -2);
}

bool is_enum_of(lua_State *L, int i, int type)
{
    senum *e = (senum *)test_udata(L, i, "EnumItem");
    return e && e->type == type;
}

int check_enum(lua_State *L, int i, int type)
{
    senum *e = (senum *)test_udata(L, i, "EnumItem");
    if (e) {
        if (e->type != type) luaL_argerror(L, i, "wrong kind of Enum");
        return e->value;
    }
    if (lua_type(L, i) == LUA_TNUMBER) return (int)lua_tointeger(L, i);
    if (lua_type(L, i) == LUA_TSTRING) {
        int v = enum_value_named(type, lua_tostring(L, i));
        if (v >= 0) return v;
    }
    return luaL_argerror(L, i, "expected an Enum item"), 0;
}

static int enumitem_index(lua_State *L)
{
    senum *e = (senum *)luaL_checkudata(L, 1, "EnumItem");
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Name")) lua_pushstring(L, enum_item_name(e->type, e->value));
    else if (!strcmp(k, "Value")) lua_pushinteger(L, e->value);
    else if (!strcmp(k, "EnumType")) {
        u16 *t = (u16 *)lua_newuserdata(L, sizeof(u16));
        *t = e->type;
        set_meta(L, "EnumType");
    } else if (!strcmp(k, "IsA")) {
        lua_pushcfunction(L, m_enum_isa);
    } else return sapi_error(L, "%s is not a valid member of EnumItem", k);
    return 1;
}

static int m_enum_isa(lua_State *L)
{
    senum *e = (senum *)luaL_checkudata(L, 1, "EnumItem");
    lua_pushboolean(L, !strcmp(etype_name(e->type), luaL_checkstring(L, 2)));
    return 1;
}

static int enumitem_tostring(lua_State *L)
{
    senum *e = (senum *)luaL_checkudata(L, 1, "EnumItem");
    lua_pushfstring(L, "Enum.%s.%s", etype_name(e->type), enum_item_name(e->type, e->value));
    return 1;
}

static int enumtype_items(lua_State *L)
{
    u16 t = *(u16 *)luaL_checkudata(L, 1, "EnumType");
    lua_newtable(L);
    int n = 0;
    if (is_gen(t)) {
        for (int i = 0; i < ub_enum_item_count(t - EN_COUNT); ++i) {
            push_enum(L, t, ub_enum_item_value(t - EN_COUNT, i));
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }
    for (const enum_item *it = k_enums[t].items; it->name; ++it) {
        push_enum(L, t, it->value);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

static int enumtype_index(lua_State *L)
{
    u16 t = *(u16 *)luaL_checkudata(L, 1, "EnumType");
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "GetEnumItems")) {
        lua_pushcfunction(L, enumtype_items);
        return 1;
    }
    int v = enum_value_named(t, k);
    if (v >= 0) {
        push_enum(L, t, v);
        return 1;
    }
    return sapi_error(L, "%s is not a valid member of \"Enum.%s\"", k, etype_name(t));
}

static int enumtype_tostring(lua_State *L)
{
    lua_pushstring(L, etype_name(*(u16 *)luaL_checkudata(L, 1, "EnumType")));
    return 1;
}

static int enumtype_eq(lua_State *L)
{
    lua_pushboolean(L, *(u16 *)luaL_checkudata(L, 1, "EnumType") == *(u16 *)luaL_checkudata(L, 2, "EnumType"));
    return 1;
}

static int enums_getenums(lua_State *L);

static int enums_index(lua_State *L)
{
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "GetEnums")) {
        lua_pushcfunction(L, enums_getenums);
        return 1;
    }
    int t = enum_type_named(k);
    if (t >= 0) {
        u16 *p = (u16 *)lua_newuserdata(L, sizeof(u16));
        *p = (u16)t;
        set_meta(L, "EnumType");
        return 1;
    }
    return sapi_error(L, "%s is not a valid EnumItem", k);
}

static int enums_getenums(lua_State *L)
{
    lua_newtable(L);
    int n = 0;
    for (int t = 0; t < EN_COUNT + ub_enum_count(); ++t) {
        if (is_gen(t) && enum_type_named(ub_enum_name(t - EN_COUNT)) != t) continue; /* shadowed */
        u16 *p = (u16 *)lua_newuserdata(L, sizeof(u16));
        *p = (u16)t;
        set_meta(L, "EnumType");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

static int enums_tostring(lua_State *L)
{
    lua_pushliteral(L, "Enums");
    return 1;
}

u8 material_to_ub(int m)
{
    if (m == 512 || m == 528) return UB_MAT_WOOD;
    if (m == 848) return UB_MAT_BRICK;
    if (m >= 784 && m <= 940) return UB_MAT_STONE;
    if (m >= 1040 && m <= 1100) return UB_MAT_METAL;
    if (m == 1280 || m == 1284 || m == 1344 || m == 1360) return UB_MAT_GRASS;
    if (m == 1296 || m == 1328 || m == 1376 || m == 1392) return UB_MAT_STONE;
    return UB_MAT_PLASTIC;
}

int material_from_ub(u8 m)
{
    switch (m) {
    case UB_MAT_WOOD: return 512;
    case UB_MAT_BRICK: return 848;
    case UB_MAT_STONE: return 800;
    case UB_MAT_METAL: return 1088;
    case UB_MAT_GRASS: return 1280;
    case UB_MAT_GRID: return 256;
    default: return 256;
    }
}

/* ------------------------------------------------------------ TweenInfo */

stweeninfo *check_tweeninfo(lua_State *L, int i)
{
    stweeninfo *t = (stweeninfo *)test_udata(L, i, "TweenInfo");
    if (!t) luaL_typerror(L, i, "TweenInfo");
    return t;
}

static int ti_new(lua_State *L)
{
    stweeninfo v;
    v.time = (f32)luaL_optnumber(L, 1, 1);
    v.style = lua_isnoneornil(L, 2) ? 3 : check_enum(L, 2, EN_EASINGSTYLE); /* Quad */
    v.dir = lua_isnoneornil(L, 3) ? 1 : check_enum(L, 3, EN_EASINGDIRECTION);  /* Out */
    v.repeat = (int)luaL_optinteger(L, 4, 0);
    v.reverses = lua_toboolean(L, 5) != 0;
    v.delay = (f32)luaL_optnumber(L, 6, 0);
    stweeninfo *t = (stweeninfo *)lua_newuserdata(L, sizeof(stweeninfo)); /* after reading the arguments */
    *t = v;
    set_meta(L, "TweenInfo");
    return 1;
}

static int ti_index(lua_State *L)
{
    stweeninfo *t = check_tweeninfo(L, 1);
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Time")) lua_pushnumber(L, t->time);
    else if (!strcmp(k, "EasingStyle")) push_enum(L, EN_EASINGSTYLE, t->style);
    else if (!strcmp(k, "EasingDirection")) push_enum(L, EN_EASINGDIRECTION, t->dir);
    else if (!strcmp(k, "RepeatCount")) lua_pushinteger(L, t->repeat);
    else if (!strcmp(k, "Reverses")) lua_pushboolean(L, t->reverses);
    else if (!strcmp(k, "DelayTime")) lua_pushnumber(L, t->delay);
    else return sapi_error(L, "%s is not a valid member of TweenInfo", k);
    return 1;
}

static f32 ease_in(int style, f32 t)
{
    switch (style) {
    case 0: return t;
    case 1: return 1.0f - (f32)cos(t * 3.14159265358979 / 2);
    case 2: return t * t * (2.70158f * t - 1.70158f);
    case 3: return t * t;
    case 4: return t * t * t * t;
    case 5: return t * t * t * t * t;
    case 6: {
        f32 u = 1 - t, r;
        if (u < 1 / 2.75f) r = 7.5625f * u * u;
        else if (u < 2 / 2.75f) { u -= 1.5f / 2.75f; r = 7.5625f * u * u + 0.75f; }
        else if (u < 2.5f / 2.75f) { u -= 2.25f / 2.75f; r = 7.5625f * u * u + 0.9375f; }
        else { u -= 2.625f / 2.75f; r = 7.5625f * u * u + 0.984375f; }
        return 1 - r;
    }
    case 7:
        if (t <= 0 || t >= 1) return t;
        return -(f32)(pow(2, 10 * (t - 1)) * sin((t - 1.075) * (2 * 3.14159265358979) / 0.3));
    case 8: return t <= 0 ? 0 : (f32)pow(2, 10 * (t - 1));
    case 9: return 1 - (f32)sqrt(1 - t * t);
    case 10: return t * t * t;
    default: return t;
    }
}

f32 ease(int style, int dir, f32 t)
{
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    if (dir == 0) return ease_in(style, t);
    if (dir == 1) return 1 - ease_in(style, 1 - t);
    return t < 0.5f ? ease_in(style, t * 2) / 2 : 1 - ease_in(style, (1 - t) * 2) / 2;
}

/* ------------------------------------------------------- Ray, RaycastParams */

typedef struct {
    vec3 origin, dir;
} sray;

void push_ray(lua_State *L, vec3 origin, vec3 dir)
{
    sray *r = (sray *)lua_newuserdata(L, sizeof(sray));
    r->origin = origin;
    r->dir = dir;
    set_meta(L, "Ray");
}

static int ray_new(lua_State *L)
{
    sray *r = (sray *)lua_newuserdata(L, sizeof(sray));
    r->origin = check_v3(L, 1);
    r->dir = check_v3(L, 2);
    set_meta(L, "Ray");
    return 1;
}

static int ray_index(lua_State *L)
{
    sray *r = (sray *)luaL_checkudata(L, 1, "Ray");
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Origin")) push_v3(L, r->origin);
    else if (!strcmp(k, "Direction")) push_v3(L, r->dir);
    else if (!strcmp(k, "Unit")) {
        sray *u = (sray *)lua_newuserdata(L, sizeof(sray));
        u->origin = r->origin;
        u->dir = v3_normalize(r->dir);
        set_meta(L, "Ray");
    } else return sapi_error(L, "%s is not a valid member of Ray", k);
    return 1;
}

static int rp_new(lua_State *L)
{
    lua_newtable(L);
    lua_newtable(L);
    lua_setfield(L, -2, "FilterDescendantsInstances");
    push_enum(L, EN_RAYCASTFILTER, 0);
    lua_setfield(L, -2, "FilterType");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "IgnoreWater");
    luaL_getmetatable(L, "RaycastParams");
    lua_setmetatable(L, -2);
    return 1;
}

/* ----------------------------------------------------------------- typeof */

const char *ub_typeof(lua_State *L, int i)
{
    static const char *const names[] = { "Vector3", "Color3", "CFrame", "BrickColor", "EnumItem", "EnumType",
                                         "Enums", "TweenInfo", "Ray", "Instance", "RBXScriptSignal",
                                         "RBXScriptConnection", "RaycastParams", "UDim", "UDim2", "Vector2",
                                         "Rect", "NumberRange", "NumberSequence", "NumberSequenceKeypoint",
                                         "ColorSequence", "ColorSequenceKeypoint", "Font", "InputObject", NULL };
    int t = lua_type(L, i);
    if (t != LUA_TUSERDATA && t != LUA_TTABLE) return lua_typename(L, t);
    if (!lua_getmetatable(L, i)) return lua_typename(L, t);
    lua_pushliteral(L, "__type"); /* types that name themselves (Random, DateTime, ...) */
    lua_rawget(L, -2);
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *name = lua_tostring(L, -1); /* kept alive by the metatable */
        lua_pop(L, 2);
        return name;
    }
    lua_pop(L, 1);
    for (int k = 0; names[k]; ++k) {
        luaL_getmetatable(L, names[k]);
        bool same = lua_rawequal(L, -1, -2);
        lua_pop(L, 1);
        if (same) {
            lua_pop(L, 1);
            return strcmp(names[k], "EnumType") == 0 ? "Enum" : names[k];
        }
    }
    lua_pop(L, 1);
    return lua_typename(L, t);
}

/* ------------------------------------------------------------------ setup */

static void meta(lua_State *L, const char *name, lua_CFunction index, lua_CFunction tostr)
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

/* A constructor table in the base, made read-only through a proxy. */
static void lib(lua_State *L, int base, const char *name, const luaL_Reg *fns)
{
    lua_newtable(L); /* the real table */
    for (int i = 0; fns[i].name; ++i) {
        lua_pushcfunction(L, fns[i].func);
        lua_setfield(L, -2, fns[i].name);
    }
    lua_newtable(L); /* proxy */
    lua_newtable(L); /* its metatable */
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

void open_types(lua_State *L)
{
    extern char REG_BASE;
    lua_pushlightuserdata(L, &REG_ENUMITEMS);
    lua_newtable(L);
    lua_rawset(L, LUA_REGISTRYINDEX);

    meta(L, "Vector3", v3_index, v3_tostring);
    op(L, "__add", v3_add_l);
    op(L, "__sub", v3_sub_l);
    op(L, "__mul", v3_mul_l);
    op(L, "__div", v3_div_l);
    op(L, "__unm", v3_unm_l);
    op(L, "__eq", v3_eq_l);
    lua_pop(L, 1);
    meta(L, "Color3", c3_index, c3_tostring);
    op(L, "__eq", c3_eq_l);
    lua_pop(L, 1);
    meta(L, "CFrame", cf_index, cf_tostring);
    op(L, "__mul", cf_mul_l);
    op(L, "__add", cf_add_l);
    op(L, "__sub", cf_sub_l);
    op(L, "__eq", cf_eq_l);
    lua_pop(L, 1);
    meta(L, "BrickColor", bc_index, bc_tostring);
    op(L, "__eq", bc_eq_l);
    lua_pop(L, 1);
    meta(L, "EnumItem", enumitem_index, enumitem_tostring);
    lua_pop(L, 1);
    meta(L, "EnumType", enumtype_index, enumtype_tostring);
    op(L, "__eq", enumtype_eq);
    lua_pop(L, 1);
    meta(L, "Enums", enums_index, enums_tostring);
    lua_pop(L, 1);
    meta(L, "TweenInfo", ti_index, NULL);
    lua_pop(L, 1);
    meta(L, "Ray", ray_index, NULL);
    lua_pop(L, 1);
    luaL_newmetatable(L, "RaycastParams");
    lua_pop(L, 1);

    lua_pushlightuserdata(L, &REG_BASE);
    lua_rawget(L, LUA_REGISTRYINDEX);
    int base = lua_gettop(L);
    static const luaL_Reg v3fns[] = { { "new", v3_new }, { "fromNormalId", v3_fromnormal }, { "fromAxis", v3_fromnormal },
                                      { NULL, NULL } };
    lib(L, base, "Vector3", v3fns);
    lua_getfield(L, base, "Vector3"); /* constants on the real table */
    lua_getmetatable(L, -1);
    lua_getfield(L, -1, "__index");
    push_v3(L, v3(0, 0, 0));
    lua_setfield(L, -2, "zero");
    push_v3(L, v3(1, 1, 1));
    lua_setfield(L, -2, "one");
    push_v3(L, v3(1, 0, 0));
    lua_setfield(L, -2, "xAxis");
    push_v3(L, v3(0, 1, 0));
    lua_setfield(L, -2, "yAxis");
    push_v3(L, v3(0, 0, 1));
    lua_setfield(L, -2, "zAxis");
    lua_pop(L, 3);
    static const luaL_Reg c3fns[] = { { "new", c3_new }, { "fromRGB", c3_rgb }, { "fromHSV", c3_hsv },
                                      { "fromHex", c3_hex }, { NULL, NULL } };
    lib(L, base, "Color3", c3fns);
    static const luaL_Reg cffns[] = { { "new", cf_new }, { "Angles", cf_angles }, { "fromEulerAnglesXYZ", cf_angles },
                                      { "fromEulerAnglesYXZ", cf_yxz }, { "fromOrientation", cf_yxz },
                                      { "fromAxisAngle", cf_axisangle }, { "lookAt", cf_lookat },
                                      { "fromMatrix", cf_frommatrix }, { NULL, NULL } };
    lib(L, base, "CFrame", cffns);
    lua_getfield(L, base, "CFrame");
    lua_getmetatable(L, -1);
    lua_getfield(L, -1, "__index");
    mat3 id = mat3_identity();
    push_cf(L, v3(0, 0, 0), &id);
    lua_setfield(L, -2, "identity");
    lua_pop(L, 3);
    static const luaL_Reg bcfns[] = { { "new", bc_new }, { "random", bc_random }, { NULL, NULL } };
    lib(L, base, "BrickColor", bcfns);
    /* BrickColor.Red() and friends */
    lua_getfield(L, base, "BrickColor");
    lua_getmetatable(L, -1);
    lua_getfield(L, -1, "__index");
    static const struct { const char *fn; int number; } named[] = {
        { "White", 1 }, { "Gray", 194 }, { "DarkGray", 199 }, { "Black", 26 }, { "Red", 21 }, { "Yellow", 24 },
        { "Green", 28 }, { "Blue", 23 },
    };
    for (int i = 0; i < (int)(sizeof named / sizeof named[0]); ++i) {
        int idx = 0;
        for (int k = 0; k < BRICKS; ++k)
            if (k_bricks[k].number == named[i].number) idx = k;
        lua_pushinteger(L, idx);
        lua_pushcclosure(L, bc_named, 1);
        lua_setfield(L, -2, named[i].fn);
    }
    lua_pop(L, 3);
    static const luaL_Reg tifns[] = { { "new", ti_new }, { NULL, NULL } };
    lib(L, base, "TweenInfo", tifns);
    static const luaL_Reg rayfns[] = { { "new", ray_new }, { NULL, NULL } };
    lib(L, base, "Ray", rayfns);
    static const luaL_Reg rpfns[] = { { "new", rp_new }, { NULL, NULL } };
    lib(L, base, "RaycastParams", rpfns);
    lua_newuserdata(L, 1);
    set_meta(L, "Enums");
    lua_setfield(L, base, "Enum");
    open_types2(L, base);
    lua_pop(L, 1);
}

