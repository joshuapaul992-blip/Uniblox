/* Uniblox - Luau's additions to the standard library, as Roblox had them
 * in 2022: bit32, utf8, table.move/freeze/clone, math.noise and
 * math.log(x, base), os.date and os.time(t), the safe part of debug,
 * coroutine.close/isyieldable, newproxy, rawlen, gcinfo, Random,
 * DateTime, string.pack/unpack, and HttpService's JSON and URL helpers.
 *
 * All written here from the documented behaviour (Luau's reference and
 * Lua 5.3's manual for the functions Luau took from it). math.noise is
 * Ken Perlin's improved noise (2002), whose reference permutation is
 * published with the algorithm. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "script/ub_sapi.h"

static void set_type(lua_State *L, const char *name)
{
    luaL_getmetatable(L, name);
    lua_setmetatable(L, -2);
}

static void reg(lua_State *L, const luaL_Reg *fns)
{
    for (int i = 0; fns[i].name; ++i) {
        lua_pushcfunction(L, fns[i].func);
        lua_setfield(L, -2, fns[i].name);
    }
}

/* -------------------------------------------------------------- bit32 */

static u32 to_u32(lua_State *L, int i)
{
    f64 d = luaL_checknumber(L, i);
    if (d != d || d > 1e300 || d < -1e300) return 0;
    d = d < 0 ? ceil(d) : floor(d);
    d = fmod(d, 4294967296.0);
    if (d < 0) d += 4294967296.0;
    return (u32)d;
}

static int push_u32(lua_State *L, u32 v)
{
    lua_pushnumber(L, (f64)v);
    return 1;
}

static int b_and(lua_State *L)
{
    u32 r = 0xFFFFFFFFu;
    for (int i = 1, n = lua_gettop(L); i <= n; ++i) r &= to_u32(L, i);
    return push_u32(L, r);
}

static int b_or(lua_State *L)
{
    u32 r = 0;
    for (int i = 1, n = lua_gettop(L); i <= n; ++i) r |= to_u32(L, i);
    return push_u32(L, r);
}

static int b_xor(lua_State *L)
{
    u32 r = 0;
    for (int i = 1, n = lua_gettop(L); i <= n; ++i) r ^= to_u32(L, i);
    return push_u32(L, r);
}

static int b_test(lua_State *L)
{
    u32 r = 0xFFFFFFFFu;
    for (int i = 1, n = lua_gettop(L); i <= n; ++i) r &= to_u32(L, i);
    lua_pushboolean(L, r != 0);
    return 1;
}

static int b_not(lua_State *L) { return push_u32(L, ~to_u32(L, 1)); }

static u32 shift(u32 x, int d) /* left for d > 0 */
{
    if (d <= -32 || d >= 32) return 0;
    return d >= 0 ? x << d : x >> -d;
}

static int b_lshift(lua_State *L) { return push_u32(L, shift(to_u32(L, 1), luaL_checkint(L, 2))); }
static int b_rshift(lua_State *L) { return push_u32(L, shift(to_u32(L, 1), -luaL_checkint(L, 2))); }

static int b_arshift(lua_State *L)
{
    u32 x = to_u32(L, 1);
    int d = luaL_checkint(L, 2);
    if (d < 0 || !(x & 0x80000000u)) return push_u32(L, shift(x, -d));
    if (d >= 32) return push_u32(L, 0xFFFFFFFFu);
    return push_u32(L, (x >> d) | ~(0xFFFFFFFFu >> d));
}

static int b_lrotate(lua_State *L)
{
    u32 x = to_u32(L, 1);
    int d = luaL_checkint(L, 2) & 31;
    return push_u32(L, d ? (x << d) | (x >> (32 - d)) : x);
}

static int b_rrotate(lua_State *L)
{
    u32 x = to_u32(L, 1);
    int d = (-luaL_checkint(L, 2)) & 31;
    return push_u32(L, d ? (x << d) | (x >> (32 - d)) : x);
}

static int fieldargs(lua_State *L, int fi, int *width)
{
    int f = luaL_checkint(L, fi);
    int w = luaL_optint(L, fi + 1, 1);
    luaL_argcheck(L, f >= 0, fi, "field cannot be negative");
    luaL_argcheck(L, w > 0, fi + 1, "width must be positive");
    if (f + w > 32) luaL_error(L, "trying to access non-existent bits");
    *width = w;
    return f;
}

static u32 mask(int w) { return w >= 32 ? 0xFFFFFFFFu : (1u << w) - 1u; }

static int b_extract(lua_State *L)
{
    u32 x = to_u32(L, 1);
    int w;
    int f = fieldargs(L, 2, &w);
    return push_u32(L, (x >> f) & mask(w));
}

static int b_replace(lua_State *L)
{
    u32 x = to_u32(L, 1), v = to_u32(L, 2);
    int w;
    int f = fieldargs(L, 3, &w);
    u32 m = mask(w);
    return push_u32(L, (x & ~(m << f)) | ((v & m) << f));
}

static int b_countlz(lua_State *L)
{
    u32 x = to_u32(L, 1);
    int n = 0;
    while (n < 32 && !(x & (0x80000000u >> n))) n++;
    lua_pushinteger(L, n);
    return 1;
}

static int b_countrz(lua_State *L)
{
    u32 x = to_u32(L, 1);
    int n = 0;
    while (n < 32 && !(x & (1u << n))) n++;
    lua_pushinteger(L, n);
    return 1;
}

static int b_byteswap(lua_State *L)
{
    u32 x = to_u32(L, 1);
    return push_u32(L, (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24));
}

static const luaL_Reg k_bit32[] = {
    { "band", b_and }, { "bor", b_or }, { "bxor", b_xor }, { "btest", b_test }, { "bnot", b_not },
    { "lshift", b_lshift }, { "rshift", b_rshift }, { "arshift", b_arshift }, { "lrotate", b_lrotate },
    { "rrotate", b_rrotate }, { "extract", b_extract }, { "replace", b_replace }, { "countlz", b_countlz },
    { "countrz", b_countrz }, { "byteswap", b_byteswap }, { NULL, NULL }
};

/* --------------------------------------------------------------- utf8 */

#define MAXUTF 0x10FFFFu
#define iscont(p) ((*(const u8 *)(p) & 0xC0) == 0x80)

/* Decodes one sequence; NULL if invalid (overlong, too big, cut short). */
static const char *utf8_decode(const char *s, u32 *val)
{
    static const u32 limits[] = { 0xFFFFFFFFu, 0x80, 0x800, 0x10000u };
    const u8 *p = (const u8 *)s;
    u32 c = p[0], res = 0;
    if (c < 0x80) res = c;
    else {
        int count = 0;
        for (; c & 0x40; c <<= 1) {
            u32 cc = p[++count];
            if ((cc & 0xC0) != 0x80) return NULL;
            res = (res << 6) | (cc & 0x3F);
        }
        res |= ((c & 0x7F) << (count * 5));
        if (count > 3 || res > MAXUTF || res < limits[count]) return NULL;
        p += count;
    }
    if (val) *val = res;
    return (const char *)p + 1;
}

/* Writes c as UTF-8 into b (4 bytes); returns the length. */
static int utf8_encode(char *b, u32 c)
{
    if (c < 0x80) { b[0] = (char)c; return 1; }
    if (c < 0x800) {
        b[0] = (char)(0xC0 | (c >> 6));
        b[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000u) {
        b[0] = (char)(0xE0 | (c >> 12));
        b[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        b[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    b[0] = (char)(0xF0 | (c >> 18));
    b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    b[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    b[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static long posrelat(long pos, size_t len)
{
    if (pos >= 0) return pos;
    if ((size_t)-pos > len) return 0;
    return (long)len + pos + 1;
}

static int u_char(lua_State *L)
{
    int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        f64 c = luaL_checknumber(L, i);
        luaL_argcheck(L, c >= 0 && c <= MAXUTF, i, "value out of range");
        char e[4];
        luaL_addlstring(&b, e, (size_t)utf8_encode(e, (u32)c));
    }
    luaL_pushresult(&b);
    return 1;
}

static int u_codepoint(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    long i = posrelat(luaL_optlong(L, 2, 1), len);
    long j = posrelat(luaL_optlong(L, 3, i), len);
    luaL_argcheck(L, i >= 1, 2, "out of range");
    luaL_argcheck(L, j <= (long)len, 3, "out of range");
    if (i > j) return 0;
    int n = 0;
    luaL_checkstack(L, (int)(j - i + 1), "string slice too long");
    const char *se = s + j;
    for (s += i - 1; s < se;) {
        u32 c;
        s = utf8_decode(s, &c);
        if (!s) return luaL_error(L, "invalid UTF-8 code");
        lua_pushinteger(L, (lua_Integer)c);
        n++;
    }
    return n;
}

static int u_len(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    long i = posrelat(luaL_optlong(L, 2, 1), len);
    long j = posrelat(luaL_optlong(L, 3, -1), len);
    luaL_argcheck(L, 1 <= i && --i <= (long)len, 2, "initial position out of string");
    luaL_argcheck(L, --j < (long)len, 3, "final position out of string");
    long n = 0;
    while (i <= j) {
        const char *s1 = utf8_decode(s + i, NULL);
        if (!s1) {
            lua_pushnil(L);
            lua_pushinteger(L, i + 1);
            return 2;
        }
        i = (long)(s1 - s);
        n++;
    }
    lua_pushinteger(L, n);
    return 1;
}

static int u_offset(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    long n = luaL_checklong(L, 2);
    long posi = n >= 0 ? 1 : (long)len + 1;
    posi = posrelat(luaL_optlong(L, 3, posi), len);
    luaL_argcheck(L, 1 <= posi && --posi <= (long)len, 3, "position out of range");
    if (n == 0) {
        while (posi > 0 && iscont(s + posi)) posi--;
    } else {
        if (iscont(s + posi)) return luaL_error(L, "initial position is a continuation byte");
        if (n < 0) {
            while (n < 0 && posi > 0) {
                do posi--;
                while (posi > 0 && iscont(s + posi));
                n++;
            }
        } else {
            n--;
            while (n > 0 && posi < (long)len) {
                do posi++;
                while (iscont(s + posi));
                n--;
            }
        }
    }
    if (n == 0) lua_pushinteger(L, posi + 1);
    else lua_pushnil(L);
    return 1;
}

static int u_codes_next(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    long n = (long)lua_tointeger(L, 2) - 1;
    if (n < 0) n = 0;
    else if (n < (long)len) {
        n++;
        while (iscont(s + n)) n++;
    }
    if (n >= (long)len) return 0;
    u32 c;
    if (!utf8_decode(s + n, &c)) return luaL_error(L, "invalid UTF-8 code");
    lua_pushinteger(L, n + 1);
    lua_pushinteger(L, (lua_Integer)c);
    return 2;
}

static int u_codes(lua_State *L)
{
    luaL_checkstring(L, 1);
    lua_pushcfunction(L, u_codes_next);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    return 3;
}

/* Joins the next code point to a grapheme cluster? (combining marks,
 * variation selectors, skin tones, joiners; enough for most text) */
static bool extends(u32 c)
{
    return (c >= 0x300 && c <= 0x36F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0xFE00 && c <= 0xFE0F) ||
           (c >= 0x1F3FB && c <= 0x1F3FF) || c == 0x200D || (c >= 0xE0020 && c <= 0xE007F);
}

static int u_graphemes_next(lua_State *L)
{
    size_t len;
    const char *s = lua_tolstring(L, lua_upvalueindex(1), &len);
    long pos = (long)lua_tointeger(L, lua_upvalueindex(2));
    long end = (long)lua_tointeger(L, lua_upvalueindex(3));
    if (pos >= end) return 0;
    long start = pos;
    u32 c, prev = 0;
    const char *p = utf8_decode(s + pos, &c);
    if (!p) p = s + pos + 1; /* a bad byte is a cluster of its own */
    else prev = c;
    pos = (long)(p - s);
    if (prev == '\r' && pos < end && s[pos] == '\n') pos++;
    while (pos < end) {
        u32 n;
        const char *q = utf8_decode(s + pos, &n);
        if (!q || !(extends(n) || prev == 0x200D)) break;
        prev = n;
        pos = (long)(q - s);
    }
    lua_pushinteger(L, pos);
    lua_replace(L, lua_upvalueindex(2));
    lua_pushinteger(L, start + 1);
    lua_pushinteger(L, pos);
    return 2;
}

static int u_graphemes(lua_State *L)
{
    size_t len;
    luaL_checklstring(L, 1, &len);
    long i = posrelat(luaL_optlong(L, 2, 1), len);
    long j = posrelat(luaL_optlong(L, 3, -1), len);
    luaL_argcheck(L, i >= 1 && i <= (long)len + 1, 2, "out of range");
    luaL_argcheck(L, j <= (long)len, 3, "out of range");
    lua_pushvalue(L, 1);
    lua_pushinteger(L, i - 1);
    lua_pushinteger(L, j);
    lua_pushcclosure(L, u_graphemes_next, 3);
    return 1;
}

/* No Unicode tables here: text comes back as it was (already NFC for
 * almost everything people type). */
static int u_normalize(lua_State *L)
{
    luaL_checkstring(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

static const luaL_Reg k_utf8[] = {
    { "char", u_char }, { "codepoint", u_codepoint }, { "len", u_len }, { "offset", u_offset },
    { "codes", u_codes }, { "graphemes", u_graphemes }, { "nfcnormalize", u_normalize },
    { "nfdnormalize", u_normalize }, { NULL, NULL }
};

/* -------------------------------------------------------------- table */

static int t_move(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    int f = luaL_checkint(L, 2), e = luaL_checkint(L, 3), t = luaL_checkint(L, 4);
    int tt = lua_isnoneornil(L, 5) ? 1 : 5;
    luaL_checktype(L, tt, LUA_TTABLE);
    if (e >= f) {
        luaL_argcheck(L, f > 0 || e < 0x7FFFFFFF + f, 3, "too many elements to move");
        luaL_argcheck(L, t <= 0x7FFFFFFF - (e - f), 4, "destination wrap around");
        if (t > e || t <= f || (tt != 1 && !lua_rawequal(L, 1, tt))) {
            for (int i = 0; i <= e - f; ++i) {
                lua_rawgeti(L, 1, f + i);
                lua_rawseti(L, tt, t + i);
            }
        } else {
            for (int i = e - f; i >= 0; --i) {
                lua_rawgeti(L, 1, f + i);
                lua_rawseti(L, tt, t + i);
            }
        }
    }
    lua_pushvalue(L, tt);
    return 1;
}

static bool protected_meta(lua_State *L, int i)
{
    if (!lua_getmetatable(L, i)) return false;
    lua_pushliteral(L, "__metatable");
    lua_rawget(L, -2);
    bool p = !lua_isnil(L, -1);
    lua_pop(L, 2);
    return p;
}

static int t_freeze(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    if (protected_meta(L, 1)) return luaL_argerror(L, 1, "table has a protected metatable");
    lua_setreadonly(L, 1, 1);
    lua_settop(L, 1);
    return 1;
}

static int t_isfrozen(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, lua_getreadonly(L, 1));
    return 1;
}

static int t_clone(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    if (protected_meta(L, 1)) return luaL_argerror(L, 1, "table has a protected metatable");
    lua_createtable(L, (int)lua_objlen(L, 1), 0);
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, -4);
    }
    if (lua_getmetatable(L, 1)) lua_setmetatable(L, -2);
    return 1;
}

/* --------------------------------------------------------------- math */

static const u8 k_perm[256] = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,  69,  142,
    8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203,
    117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136, 171, 168, 68,  175, 74,  165,
    71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,
    55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,
    18,  169, 200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250,
    124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189,
    28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,
    129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,
    242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,
    181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,  222, 114,
    67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180,
};

#define PERM(i) k_perm[(i) & 255]

static f64 fade(f64 t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static f64 lerp(f64 t, f64 a, f64 b) { return a + t * (b - a); }

static f64 grad(int hash, f64 x, f64 y, f64 z)
{
    int h = hash & 15;
    f64 u = h < 8 ? x : y;
    f64 v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

static f64 perlin(f64 x, f64 y, f64 z)
{
    f64 fx = floor(x), fy = floor(y), fz = floor(z);
    int X = (int)fmod(fx, 256.0), Y = (int)fmod(fy, 256.0), Z = (int)fmod(fz, 256.0);
    if (X < 0) X += 256;
    if (Y < 0) Y += 256;
    if (Z < 0) Z += 256;
    x -= fx;
    y -= fy;
    z -= fz;
    f64 u = fade(x), v = fade(y), w = fade(z);
    int A = PERM(X) + Y, AA = PERM(A) + Z, AB = PERM(A + 1) + Z;
    int B = PERM(X + 1) + Y, BA = PERM(B) + Z, BB = PERM(B + 1) + Z;
    return lerp(w,
                lerp(v, lerp(u, grad(PERM(AA), x, y, z), grad(PERM(BA), x - 1, y, z)),
                     lerp(u, grad(PERM(AB), x, y - 1, z), grad(PERM(BB), x - 1, y - 1, z))),
                lerp(v, lerp(u, grad(PERM(AA + 1), x, y, z - 1), grad(PERM(BA + 1), x - 1, y, z - 1)),
                     lerp(u, grad(PERM(AB + 1), x, y - 1, z - 1), grad(PERM(BB + 1), x - 1, y - 1, z - 1))));
}

static int m_noise(lua_State *L)
{
    f64 x = luaL_checknumber(L, 1), y = luaL_optnumber(L, 2, 0), z = luaL_optnumber(L, 3, 0);
    lua_pushnumber(L, perlin(x, y, z));
    return 1;
}

static int m_log(lua_State *L)
{
    f64 x = luaL_checknumber(L, 1);
    if (lua_isnoneornil(L, 2)) lua_pushnumber(L, log(x));
    else {
        f64 b = luaL_checknumber(L, 2);
        lua_pushnumber(L, b == 2.0 ? log(x) / 0.69314718055994530942 : b == 10.0 ? log10(x) : log(x) / log(b));
    }
    return 1;
}

static int m_clamp(lua_State *L)
{
    f64 x = luaL_checknumber(L, 1), lo = luaL_checknumber(L, 2), hi = luaL_checknumber(L, 3);
    luaL_argcheck(L, lo <= hi, 3, "max must be greater than or equal to min");
    lua_pushnumber(L, x < lo ? lo : x > hi ? hi : x);
    return 1;
}

/* ---------------------------------------------------------- dates */

static const char *const k_days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const k_months[] = { "January", "February", "March",     "April",   "May",      "June",
                                        "July",    "August",   "September", "October", "November", "December" };

/* days since 1970-01-01 of a civil date (any month: it is normalised) */
static f64 days_from_civil(f64 y, f64 m, f64 d)
{
    f64 mm = m - 1;
    y += floor(mm / 12);
    mm -= floor(mm / 12) * 12;
    m = mm + 1;
    y -= m <= 2;
    f64 era = floor(y / 400);
    f64 yoe = y - era * 400;
    f64 doy = floor((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
    f64 doe = yoe * 365 + floor(yoe / 4) - floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}

typedef struct {
    int year, month, day, hour, min, sec, ms, wday, yday; /* wday 0 = Sunday; yday 1..366 */
} civil;

static civil civil_from_ms(f64 ms)
{
    civil c;
    f64 days = floor(ms / 86400000.0);
    f64 rem = ms - days * 86400000.0;
    f64 z = days + 719468;
    f64 era = floor(z / 146097);
    f64 doe = z - era * 146097;
    f64 yoe = floor((doe - floor(doe / 1460) + floor(doe / 36524) - floor(doe / 146096)) / 365);
    f64 y = yoe + era * 400;
    f64 doy = doe - (365 * yoe + floor(yoe / 4) - floor(yoe / 100));
    f64 mp = floor((5 * doy + 2) / 153);
    f64 d = doy - floor((153 * mp + 2) / 5) + 1;
    f64 m = mp + (mp < 10 ? 3 : -9);
    c.year = (int)(y + (m <= 2));
    c.month = (int)m;
    c.day = (int)d;
    int r = (int)rem;
    c.hour = r / 3600000;
    c.min = r / 60000 % 60;
    c.sec = r / 1000 % 60;
    c.ms = r % 1000;
    int w = (int)fmod(days + 4, 7.0);
    c.wday = w < 0 ? w + 7 : w;
    c.yday = (int)(days - days_from_civil(c.year, 1, 1)) + 1;
    return c;
}

static f64 dt_ms(const ub_datetime *t)
{
    return (days_from_civil(t->year, t->month, t->day) * 86400.0 + t->hour * 3600.0 + t->minute * 60.0 + t->second) *
               1000.0 +
           t->millisecond;
}

static f64 now_utc_ms(void)
{
    ub_datetime t;
    plat_utc_time(&t);
    return dt_ms(&t);
}

/* local time minus UTC, in ms, to the minute */
static f64 local_offset_ms(void)
{
    ub_datetime l, u;
    plat_local_time(&l);
    plat_utc_time(&u);
    l.millisecond = u.millisecond = 0;
    f64 d = dt_ms(&l) - dt_ms(&u);
    return floor(d / 60000.0 + 0.5) * 60000.0;
}

static int o_time(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        lua_pushnumber(L, floor(now_utc_ms() / 1000.0));
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    static const char *const fields[] = { "year", "month", "day", "hour", "min", "sec" };
    static const int defaults[] = { -1, -1, -1, 12, 0, 0 };
    f64 v[6];
    for (int i = 0; i < 6; ++i) {
        lua_getfield(L, 1, fields[i]);
        if (lua_isnumber(L, -1)) v[i] = floor(lua_tonumber(L, -1));
        else if (defaults[i] < 0) return luaL_error(L, "field '%s' missing in date table", fields[i]);
        else v[i] = defaults[i];
        lua_pop(L, 1);
    }
    lua_pushnumber(L, days_from_civil(v[0], v[1], v[2]) * 86400 + v[3] * 3600 + v[4] * 60 + v[5]);
    return 1;
}

static void setint(lua_State *L, const char *k, int v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}

static int o_date(lua_State *L)
{
    const char *fmt = luaL_optstring(L, 1, "%c");
    f64 secs = lua_isnoneornil(L, 2) ? floor(now_utc_ms() / 1000.0) : floor(luaL_checknumber(L, 2));
    bool utc = fmt[0] == '!';
    if (utc) fmt++;
    f64 off = utc ? 0 : local_offset_ms();
    civil c = civil_from_ms(secs * 1000.0 + off);
    if (strcmp(fmt, "*t") == 0) {
        lua_createtable(L, 0, 9);
        setint(L, "year", c.year);
        setint(L, "month", c.month);
        setint(L, "day", c.day);
        setint(L, "hour", c.hour);
        setint(L, "min", c.min);
        setint(L, "sec", c.sec);
        setint(L, "wday", c.wday + 1);
        setint(L, "yday", c.yday);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "isdst");
        return 1;
    }
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char t[64];
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            luaL_addchar(&b, *p);
            continue;
        }
        int wk;
        switch (*++p) {
        case 'a': snprintf(t, sizeof t, "%.3s", k_days[c.wday]); break;
        case 'A': snprintf(t, sizeof t, "%s", k_days[c.wday]); break;
        case 'b': case 'h': snprintf(t, sizeof t, "%.3s", k_months[c.month - 1]); break;
        case 'B': snprintf(t, sizeof t, "%s", k_months[c.month - 1]); break;
        case 'c':
            snprintf(t, sizeof t, "%.3s %.3s %2d %02d:%02d:%02d %d", k_days[c.wday], k_months[c.month - 1], c.day,
                     c.hour, c.min, c.sec, c.year);
            break;
        case 'd': snprintf(t, sizeof t, "%02d", c.day); break;
        case 'e': snprintf(t, sizeof t, "%2d", c.day); break;
        case 'H': snprintf(t, sizeof t, "%02d", c.hour); break;
        case 'I': snprintf(t, sizeof t, "%02d", (c.hour + 11) % 12 + 1); break;
        case 'j': snprintf(t, sizeof t, "%03d", c.yday); break;
        case 'm': snprintf(t, sizeof t, "%02d", c.month); break;
        case 'M': snprintf(t, sizeof t, "%02d", c.min); break;
        case 'p': snprintf(t, sizeof t, "%s", c.hour < 12 ? "AM" : "PM"); break;
        case 'S': snprintf(t, sizeof t, "%02d", c.sec); break;
        case 'U': wk = (c.yday - 1 + 7 - c.wday) / 7; snprintf(t, sizeof t, "%02d", wk); break;
        case 'W': wk = (c.yday - 1 + 7 - (c.wday + 6) % 7) / 7; snprintf(t, sizeof t, "%02d", wk); break;
        case 'w': snprintf(t, sizeof t, "%d", c.wday); break;
        case 'x': snprintf(t, sizeof t, "%02d/%02d/%02d", c.month, c.day, c.year % 100); break;
        case 'X': snprintf(t, sizeof t, "%02d:%02d:%02d", c.hour, c.min, c.sec); break;
        case 'y': snprintf(t, sizeof t, "%02d", c.year % 100); break;
        case 'Y': snprintf(t, sizeof t, "%d", c.year); break;
        case 'z': {
            int m = (int)(off / 60000.0);
            snprintf(t, sizeof t, "%c%02d%02d", m < 0 ? '-' : '+', (m < 0 ? -m : m) / 60, (m < 0 ? -m : m) % 60);
            break;
        }
        case 'Z': snprintf(t, sizeof t, "%s", utc ? "UTC" : "Local Time"); break;
        case '%': snprintf(t, sizeof t, "%%"); break;
        default:
            return luaL_error(L, "bad argument #1 to 'date' (invalid conversion specifier '%%%c')", *p ? *p : ' ');
        }
        luaL_addstring(&b, t);
    }
    luaL_pushresult(&b);
    return 1;
}

/* tick(): seconds since 1970, with the milliseconds */
static int g_tick(lua_State *L)
{
    lua_pushnumber(L, now_utc_ms() / 1000.0);
    return 1;
}

/* -------------------------------------------------------------- debug */

static lua_State *opt_thread(lua_State *L, int *arg)
{
    if (lua_isthread(L, 1)) {
        *arg = 1;
        return lua_tothread(L, 1);
    }
    *arg = 0;
    return L;
}

static int d_traceback(lua_State *L)
{
    int arg;
    lua_State *L1 = opt_thread(L, &arg);
    const char *msg = lua_tostring(L, arg + 1);
    if (!msg && !lua_isnoneornil(L, arg + 1)) { /* a non-string message comes back as it is */
        lua_pushvalue(L, arg + 1);
        return 1;
    }
    int level = luaL_optint(L, arg + 2, L1 == L ? 1 : 0);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    if (msg) {
        luaL_addstring(&b, msg);
        luaL_addchar(&b, '\n');
    }
    lua_Debug ar;
    char line[300];
    for (int n = 0; lua_getstack(L1, level, &ar) && n < 40; ++level, ++n) {
        lua_getinfo(L1, "Sln", &ar);
        if (ar.currentline <= 0) continue; /* C functions: Roblox leaves them out */
        if (ar.name) snprintf(line, sizeof line, "%s:%d function %s\n", ar.short_src, ar.currentline, ar.name);
        else snprintf(line, sizeof line, "%s:%d\n", ar.short_src, ar.currentline);
        luaL_addstring(&b, line);
    }
    luaL_pushresult(&b);
    return 1;
}

/* debug.info([thread,] level | function, options): s source, l line,
 * n name, a parameters and vararg, f function */
static int d_info(lua_State *L)
{
    int arg;
    lua_State *L1 = opt_thread(L, &arg);
    const char *opts = luaL_checkstring(L, arg + 2);
    lua_Debug ar;
    bool is_fn = lua_isfunction(L, arg + 1);
    if (is_fn) {
        lua_pushvalue(L, arg + 1);
        lua_getinfo(L, ">Sln", &ar);
    } else {
        int level = luaL_checkint(L, arg + 1);
        luaL_argcheck(L, level >= 0, arg + 1, "level can't be negative");
        if (!lua_getstack(L1, level, &ar)) return 0;
        lua_getinfo(L1, "Slnf", &ar);
        if (L1 != L) lua_xmove(L1, L, 1);
    }
    if (is_fn) lua_pushvalue(L, arg + 1); /* getinfo took it */
    int fn = lua_gettop(L); /* the function, for 'a' and 'f' */
    int n = 0;
    for (const char *o = opts; *o; ++o) {
        luaL_checkstack(L, 2, "too many options");
        switch (*o) {
        case 's': lua_pushstring(L, ar.short_src); n++; break;
        case 'l': lua_pushinteger(L, is_fn ? ar.linedefined : ar.currentline); n++; break;
        case 'n':
            if (ar.name && !is_fn) lua_pushstring(L, ar.name);
            else lua_pushnil(L);
            n++;
            break;
        case 'a': {
            int va;
            lua_pushinteger(L, lua_getparams(L, fn, &va));
            lua_pushboolean(L, va);
            n += 2;
            break;
        }
        case 'f': lua_pushvalue(L, fn); n++; break;
        default: return luaL_argerror(L, arg + 2, "invalid option");
        }
    }
    return n;
}

static int d_noop(lua_State *L)
{
    (void)L;
    return 0;
}

static const luaL_Reg k_debug[] = {
    { "traceback", d_traceback }, { "info", d_info }, { "profilebegin", d_noop }, { "profileend", d_noop },
    { "setmemorycategory", d_noop }, { "resetmemorycategory", d_noop }, { NULL, NULL }
};

/* ---------------------------------------------------------- coroutine */

static int c_close(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTHREAD);
    lua_State *co = lua_tothread(L, 1);
    if (co == L) return luaL_error(L, "cannot close running coroutine");
    int st = lua_status(co);
    lua_Debug ar;
    if (st == 0 && lua_getstack(co, 0, &ar)) return luaL_error(L, "cannot close normal coroutine");
    if (st != 0 && st != LUA_YIELD) { /* it failed: the error comes back */
        lua_pushboolean(L, 0);
        if (lua_gettop(co) > 0) lua_xmove(co, L, 1);
        else lua_pushnil(L);
        lua_resetthread(co);
        return 2;
    }
    lua_resetthread(co);
    lua_pushboolean(L, 1);
    return 1;
}

static int c_isyieldable(lua_State *L)
{
    int main = lua_pushthread(L);
    lua_pop(L, 1);
    lua_pushboolean(L, !main);
    return 1;
}

/* ---------------------------------------------------------- the base */

static int g_newproxy(lua_State *L)
{
    lua_settop(L, 1);
    lua_newuserdata(L, 0);
    if (lua_toboolean(L, 1)) {
        lua_newtable(L);
        lua_setmetatable(L, -2);
    }
    return 1;
}

static int g_rawlen(lua_State *L)
{
    int t = lua_type(L, 1);
    luaL_argcheck(L, t == LUA_TTABLE || t == LUA_TSTRING, 1, "table or string expected");
    lua_pushinteger(L, (lua_Integer)lua_objlen(L, 1));
    return 1;
}

static int g_gcinfo(lua_State *L)
{
    lua_pushinteger(L, lua_gc(L, LUA_GCCOUNT, 0));
    return 1;
}

/* ------------------------------------------------------------- Random */

typedef struct { u64 state, inc; } srandom;

static u32 pcg_next(srandom *r)
{
    u64 old = r->state;
    r->state = old * 6364136223846793005ull + r->inc;
    u32 xs = (u32)(((old >> 18) ^ old) >> 27);
    u32 rot = (u32)(old >> 59);
    return (xs >> rot) | (xs << ((32 - rot) & 31));
}

static void pcg_seed(srandom *r, u64 seed)
{
    r->state = 0;
    r->inc = (105u << 1) | 1u;
    pcg_next(r);
    r->state += seed;
    pcg_next(r);
}

static f64 pcg_unit(srandom *r) /* [0, 1) with 53 bits */
{
    u64 hi = pcg_next(r) >> 5, lo = pcg_next(r) >> 6;
    return ((f64)hi * 67108864.0 + (f64)lo) / 9007199254740992.0;
}

static srandom *check_random(lua_State *L) { return (srandom *)luaL_checkudata(L, 1, "Random"); }

static int r_new(lua_State *L)
{
    u64 seed;
    if (lua_isnoneornil(L, 1)) {
        static u32 count;
        seed = plat_time_us() * 2654435761u + (u64)now_utc_ms() + ++count;
    } else {
        f64 d = luaL_checknumber(L, 1);
        d = d < 0 ? ceil(d) : floor(d);
        seed = (u64)(i64)d;
    }
    srandom *r = (srandom *)lua_newuserdata(L, sizeof(srandom));
    pcg_seed(r, seed);
    set_type(L, "Random");
    return 1;
}

static int r_nextinteger(lua_State *L)
{
    srandom *r = check_random(L);
    f64 lo = floor(luaL_checknumber(L, 2)), hi = floor(luaL_checknumber(L, 3));
    luaL_argcheck(L, lo <= hi, 3, "interval is empty");
    f64 span = hi - lo + 1;
    lua_pushnumber(L, lo + floor(pcg_unit(r) * span));
    return 1;
}

static int r_nextnumber(lua_State *L)
{
    srandom *r = check_random(L);
    f64 lo = luaL_optnumber(L, 2, 0), hi = luaL_optnumber(L, 3, 1);
    lua_pushnumber(L, lo + (hi - lo) * pcg_unit(r));
    return 1;
}

static int r_nextunitvector(lua_State *L)
{
    srandom *r = check_random(L);
    for (;;) {
        f64 x = pcg_unit(r) * 2 - 1, y = pcg_unit(r) * 2 - 1, z = pcg_unit(r) * 2 - 1;
        f64 d = x * x + y * y + z * z;
        if (d > 1e-6 && d <= 1) {
            d = sqrt(d);
            push_v3(L, v3((f32)(x / d), (f32)(y / d), (f32)(z / d)));
            return 1;
        }
    }
}

static int r_shuffle(lua_State *L)
{
    srandom *r = check_random(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    int n = (int)lua_objlen(L, 2);
    for (int i = n; i > 1; --i) {
        int j = 1 + (int)floor(pcg_unit(r) * i);
        lua_rawgeti(L, 2, i);
        lua_rawgeti(L, 2, j);
        lua_rawseti(L, 2, i);
        lua_rawseti(L, 2, j);
    }
    return 0;
}

static int r_clone(lua_State *L)
{
    srandom *r = check_random(L);
    srandom *c = (srandom *)lua_newuserdata(L, sizeof(srandom));
    *c = *r;
    set_type(L, "Random");
    return 1;
}

static int r_index(lua_State *L)
{
    check_random(L);
    const char *k = luaL_checkstring(L, 2);
    static const luaL_Reg m[] = { { "NextInteger", r_nextinteger }, { "NextNumber", r_nextnumber },
                                  { "NextUnitVector", r_nextunitvector }, { "Shuffle", r_shuffle },
                                  { "Clone", r_clone }, { NULL, NULL } };
    for (int i = 0; m[i].name; ++i)
        if (strcmp(m[i].name, k) == 0) {
            lua_pushcfunction(L, m[i].func);
            return 1;
        }
    return sapi_error(L, "%s is not a valid member of Random", k);
}

static int r_tostring(lua_State *L)
{
    lua_pushliteral(L, "Random");
    return 1;
}

/* ----------------------------------------------------------- DateTime */

typedef struct { f64 ms; } sdatetime; /* since 1970, UTC */

static sdatetime *check_dt(lua_State *L, int i) { return (sdatetime *)luaL_checkudata(L, i, "DateTime"); }

static int push_dt(lua_State *L, f64 ms)
{
    sdatetime *d = (sdatetime *)lua_newuserdata(L, sizeof(sdatetime));
    d->ms = floor(ms);
    set_type(L, "DateTime");
    return 1;
}

static int dt_now(lua_State *L) { return push_dt(L, now_utc_ms()); }
static int dt_fromunix(lua_State *L) { return push_dt(L, floor(luaL_checknumber(L, 1)) * 1000.0); }
static int dt_fromunixms(lua_State *L) { return push_dt(L, luaL_checknumber(L, 1)); }

static f64 civil_args_ms(lua_State *L)
{
    f64 y = luaL_optnumber(L, 1, 1970), mo = luaL_optnumber(L, 2, 1), d = luaL_optnumber(L, 3, 1);
    f64 h = luaL_optnumber(L, 4, 0), mi = luaL_optnumber(L, 5, 0), s = luaL_optnumber(L, 6, 0);
    f64 ms = luaL_optnumber(L, 7, 0);
    return (days_from_civil(floor(y), floor(mo), floor(d)) * 86400.0 + floor(h) * 3600.0 + floor(mi) * 60.0 +
            floor(s)) * 1000.0 + floor(ms);
}

static int dt_fromuniversal(lua_State *L) { return push_dt(L, civil_args_ms(L)); }
static int dt_fromlocal(lua_State *L) { return push_dt(L, civil_args_ms(L) - local_offset_ms()); }

static int digits(const char **p, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; ++i) {
        if ((*p)[i] < '0' || (*p)[i] > '9') return 0;
        v = v * 10 + ((*p)[i] - '0');
    }
    *p += n;
    *out = v;
    return 1;
}

/* ISO 8601: 2020-01-02, 2020-01-02T10:30:45, with .sss and Z or +hh:mm */
static int dt_fromiso(lua_State *L)
{
    const char *p = luaL_checkstring(L, 1);
    int y, mo, d, h = 0, mi = 0, s = 0, ms = 0;
    if (!digits(&p, 4, &y) || *p++ != '-' || !digits(&p, 2, &mo) || *p++ != '-' || !digits(&p, 2, &d)) goto bad;
    if (*p == 'T' || *p == 't' || *p == ' ') {
        p++;
        if (!digits(&p, 2, &h) || *p++ != ':' || !digits(&p, 2, &mi)) goto bad;
        if (*p == ':') {
            p++;
            if (!digits(&p, 2, &s)) goto bad;
            if (*p == '.' || *p == ',') {
                p++;
                int scale = 100, any = 0;
                while (*p >= '0' && *p <= '9') {
                    ms += (*p++ - '0') * scale;
                    scale /= 10;
                    any = 1;
                }
                if (!any) goto bad;
            }
        }
    }
    f64 off = 0;
    if (*p == 'Z' || *p == 'z') p++;
    else if (*p == '+' || *p == '-') {
        int sign = *p++ == '-' ? -1 : 1, oh, om = 0;
        if (!digits(&p, 2, &oh)) goto bad;
        if (*p == ':') p++;
        if (*p) {
            if (!digits(&p, 2, &om)) goto bad;
        }
        off = sign * (oh * 60 + om) * 60000.0;
    }
    if (*p || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || s > 60) goto bad;
    return push_dt(L, (days_from_civil(y, mo, d) * 86400.0 + h * 3600.0 + mi * 60.0 + s) * 1000.0 + ms - off);
bad:
    lua_pushnil(L);
    return 1;
}

static int dt_totable(lua_State *L, bool local)
{
    sdatetime *dt = check_dt(L, 1);
    civil c = civil_from_ms(dt->ms + (local ? local_offset_ms() : 0));
    lua_createtable(L, 0, 7);
    setint(L, "Year", c.year);
    setint(L, "Month", c.month);
    setint(L, "Day", c.day);
    setint(L, "Hour", c.hour);
    setint(L, "Minute", c.min);
    setint(L, "Second", c.sec);
    setint(L, "Millisecond", c.ms);
    return 1;
}

static int dt_touniversal(lua_State *L) { return dt_totable(L, false); }
static int dt_tolocal(lua_State *L) { return dt_totable(L, true); }

static int dt_toiso(lua_State *L)
{
    civil c = civil_from_ms(check_dt(L, 1)->ms);
    char b[48];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", c.year, c.month, c.day, c.hour, c.min, c.sec);
    lua_pushstring(L, b);
    return 1;
}

static const char *ordinal(int n)
{
    int t = n % 100;
    if (t >= 11 && t <= 13) return "th";
    switch (n % 10) {
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    default: return "th";
    }
}

/* Moment-style tokens, English (the only locale here). */
static void dt_format(luaL_Buffer *b, const char *f, const civil *c)
{
    static const struct { const char *tok, *expand; } k_long[] = {
        { "LLLL", "dddd, MMMM D, YYYY h:mm A" }, { "llll", "ddd, MMM D, YYYY h:mm A" },
        { "LLL", "MMMM D, YYYY h:mm A" },        { "lll", "MMM D, YYYY h:mm A" },
        { "LTS", "h:mm:ss A" },                  { "LL", "MMMM D, YYYY" },
        { "ll", "MMM D, YYYY" },                 { "LT", "h:mm A" },
        { "L", "MM/DD/YYYY" },                   { "l", "M/D/YYYY" },
    };
    char t[64];
    while (*f) {
        if (*f == '[') { /* literal text */
            const char *e = strchr(f, ']');
            if (!e) e = f + strlen(f);
            luaL_addlstring(b, f + 1, (size_t)(e - f - 1));
            f = *e ? e + 1 : e;
            continue;
        }
        bool done = false;
        for (size_t i = 0; i < sizeof k_long / sizeof k_long[0]; ++i) {
            size_t n = strlen(k_long[i].tok);
            if (strncmp(f, k_long[i].tok, n) == 0) {
                dt_format(b, k_long[i].expand, c);
                f += n;
                done = true;
                break;
            }
        }
        if (done) continue;
        int run = 1;
        while (f[run] == f[0]) run++;
        t[0] = 0;
        int used = run;
        int h12 = (c->hour + 11) % 12 + 1;
        switch (f[0]) {
        case 'Y':
            if (run >= 4) snprintf(t, sizeof t, "%04d", c->year), used = 4;
            else if (run >= 2) snprintf(t, sizeof t, "%02d", c->year % 100), used = 2;
            else used = 0;
            break;
        case 'Q': snprintf(t, sizeof t, "%d", (c->month - 1) / 3 + 1), used = 1; break;
        case 'M':
            if (f[1] == 'o') snprintf(t, sizeof t, "%d%s", c->month, ordinal(c->month)), used = 2;
            else if (run >= 4) snprintf(t, sizeof t, "%s", k_months[c->month - 1]), used = 4;
            else if (run == 3) snprintf(t, sizeof t, "%.3s", k_months[c->month - 1]);
            else if (run == 2) snprintf(t, sizeof t, "%02d", c->month);
            else snprintf(t, sizeof t, "%d", c->month);
            break;
        case 'D':
            if (f[1] == 'o') snprintf(t, sizeof t, "%d%s", c->day, ordinal(c->day)), used = 2;
            else if (run >= 4) snprintf(t, sizeof t, "%03d", c->yday), used = 4;
            else if (run == 3) snprintf(t, sizeof t, "%d", c->yday);
            else if (run == 2) snprintf(t, sizeof t, "%02d", c->day);
            else snprintf(t, sizeof t, "%d", c->day);
            break;
        case 'd':
            if (run >= 4) snprintf(t, sizeof t, "%s", k_days[c->wday]), used = 4;
            else if (run == 3) snprintf(t, sizeof t, "%.3s", k_days[c->wday]);
            else if (run == 2) snprintf(t, sizeof t, "%.2s", k_days[c->wday]);
            else snprintf(t, sizeof t, "%d", c->wday);
            break;
        case 'H': used = run > 2 ? 2 : run; snprintf(t, sizeof t, used == 2 ? "%02d" : "%d", c->hour); break;
        case 'h': used = run > 2 ? 2 : run; snprintf(t, sizeof t, used == 2 ? "%02d" : "%d", h12); break;
        case 'm': used = run > 2 ? 2 : run; snprintf(t, sizeof t, used == 2 ? "%02d" : "%d", c->min); break;
        case 's': used = run > 2 ? 2 : run; snprintf(t, sizeof t, used == 2 ? "%02d" : "%d", c->sec); break;
        case 'S':
            used = run > 3 ? 3 : run;
            snprintf(t, sizeof t, "%03d", c->ms);
            t[used] = 0;
            break;
        case 'A': used = 1; snprintf(t, sizeof t, "%s", c->hour < 12 ? "AM" : "PM"); break;
        case 'a': used = 1; snprintf(t, sizeof t, "%s", c->hour < 12 ? "am" : "pm"); break;
        default: used = 0; break;
        }
        if (used == 0) { /* not a token */
            luaL_addchar(b, *f++);
            continue;
        }
        luaL_addstring(b, t);
        f += used;
    }
}

static int dt_formatwith(lua_State *L, bool local)
{
    sdatetime *dt = check_dt(L, 1);
    const char *f = luaL_checkstring(L, 2);
    luaL_optstring(L, 3, "en-us");
    civil c = civil_from_ms(dt->ms + (local ? local_offset_ms() : 0));
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    dt_format(&b, f, &c);
    luaL_pushresult(&b);
    return 1;
}

static int dt_formatuniversal(lua_State *L) { return dt_formatwith(L, false); }
static int dt_formatlocal(lua_State *L) { return dt_formatwith(L, true); }

static int dt_index(lua_State *L)
{
    sdatetime *dt = check_dt(L, 1);
    const char *k = luaL_checkstring(L, 2);
    if (strcmp(k, "UnixTimestamp") == 0) { lua_pushnumber(L, floor(dt->ms / 1000.0)); return 1; }
    if (strcmp(k, "UnixTimestampMillis") == 0) { lua_pushnumber(L, dt->ms); return 1; }
    static const luaL_Reg m[] = { { "ToUniversalTime", dt_touniversal }, { "ToLocalTime", dt_tolocal },
                                  { "ToIsoDate", dt_toiso }, { "FormatUniversalTime", dt_formatuniversal },
                                  { "FormatLocalTime", dt_formatlocal }, { NULL, NULL } };
    for (int i = 0; m[i].name; ++i)
        if (strcmp(m[i].name, k) == 0) {
            lua_pushcfunction(L, m[i].func);
            return 1;
        }
    return sapi_error(L, "%s is not a valid member of DateTime", k);
}

static int dt_eq(lua_State *L) { lua_pushboolean(L, check_dt(L, 1)->ms == check_dt(L, 2)->ms); return 1; }
static int dt_lt(lua_State *L) { lua_pushboolean(L, check_dt(L, 1)->ms < check_dt(L, 2)->ms); return 1; }
static int dt_le(lua_State *L) { lua_pushboolean(L, check_dt(L, 1)->ms <= check_dt(L, 2)->ms); return 1; }

/* -------------------------------------------------- string.pack/unpack */

typedef struct {
    lua_State *L;
    bool little;
    int maxalign;
} pheader;

typedef enum { K_INT, K_UINT, K_FLOAT, K_DOUBLE, K_CHAR, K_STRING, K_ZSTR, K_PADDING, K_PADDALIGN, K_NOP } pkind;

static bool is_digit(int c) { return c >= '0' && c <= '9'; }

static int getnum(const char **fmt, int df)
{
    if (!is_digit(**fmt)) return df;
    int a = 0;
    do a = a * 10 + (*((*fmt)++) - '0');
    while (is_digit(**fmt) && a <= (0x7FFFFFFF - 9) / 10);
    return a;
}

static int getnumlimit(pheader *h, const char **fmt, int df)
{
    int sz = getnum(fmt, df);
    if (sz > 8 || sz <= 0) return luaL_error(h->L, "integral size (%d) out of limits [1,8]", sz);
    return sz;
}

static pkind getoption(pheader *h, const char **fmt, int *size)
{
    int opt = *((*fmt)++);
    *size = 0;
    switch (opt) {
    case 'b': *size = 1; return K_INT;
    case 'B': *size = 1; return K_UINT;
    case 'h': *size = 2; return K_INT;
    case 'H': *size = 2; return K_UINT;
    case 'l': case 'j': *size = 8; return K_INT;
    case 'L': case 'J': case 'T': *size = 8; return K_UINT;
    case 'f': *size = 4; return K_FLOAT;
    case 'd': case 'n': *size = 8; return K_DOUBLE;
    case 'i': *size = getnumlimit(h, fmt, 4); return K_INT;
    case 'I': *size = getnumlimit(h, fmt, 4); return K_UINT;
    case 's': *size = getnumlimit(h, fmt, 8); return K_STRING;
    case 'c':
        *size = getnum(fmt, -1);
        if (*size == -1) luaL_error(h->L, "missing size for format option 'c'");
        return K_CHAR;
    case 'z': return K_ZSTR;
    case 'x': *size = 1; return K_PADDING;
    case 'X': return K_PADDALIGN;
    case ' ': break;
    case '<': h->little = true; break;
    case '>': h->little = false; break;
    case '=': h->little = true; break;
    case '!': h->maxalign = getnumlimit(h, fmt, 8); break;
    default: luaL_error(h->L, "invalid format option '%c'", opt);
    }
    return K_NOP;
}

static pkind getdetails(pheader *h, size_t total, const char **fmt, int *psize, int *ntoalign)
{
    pkind opt = getoption(h, fmt, psize);
    int align = *psize;
    if (opt == K_PADDALIGN) {
        if (**fmt == '\0' || getoption(h, fmt, &align) == K_CHAR || align == 0)
            luaL_argerror(h->L, 1, "invalid next option for option 'X'");
    }
    if (align <= 1 || opt == K_CHAR) *ntoalign = 0;
    else {
        if (align > h->maxalign) align = h->maxalign;
        if ((align & (align - 1)) != 0) luaL_argerror(h->L, 1, "format asks for alignment not power of 2");
        *ntoalign = (align - (int)(total & (size_t)(align - 1))) & (align - 1);
    }
    return opt;
}

static void packint(luaL_Buffer *b, u64 n, bool little, int size, bool neg)
{
    char buff[8];
    for (int i = 0; i < size; ++i) {
        int at = little ? i : size - 1 - i;
        buff[at] = (char)(i < 8 ? (n >> (8 * i)) & 0xFF : (neg ? 0xFF : 0));
    }
    luaL_addlstring(b, buff, (size_t)size);
}

static void copywithendian(char *dst, const char *src, int size, bool little)
{
    /* the host (x86) is little-endian */
    if (little) memcpy(dst, src, (size_t)size);
    else
        for (int i = 0; i < size; ++i) dst[i] = src[size - 1 - i];
}

static int s_pack(lua_State *L)
{
    const char *fmt = luaL_checkstring(L, 1);
    pheader h = { L, true, 1 };
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int arg = 1;
    size_t total = 0;
    while (*fmt) {
        int size, ntoalign;
        pkind opt = getdetails(&h, total, &fmt, &size, &ntoalign);
        total += (size_t)(ntoalign + size);
        while (ntoalign-- > 0) luaL_addchar(&b, 0);
        arg++;
        switch (opt) {
        case K_INT: {
            f64 d = luaL_checknumber(L, arg);
            i64 n = (i64)d;
            if (size < 8) {
                i64 lim = (i64)1 << ((size * 8) - 1);
                luaL_argcheck(L, -lim <= n && n < lim, arg, "integer overflow");
            }
            packint(&b, (u64)n, h.little, size, n < 0);
            break;
        }
        case K_UINT: {
            f64 d = luaL_checknumber(L, arg);
            i64 n = d < 0 ? (i64)d : (i64)(u64)d;
            if (size < 8) luaL_argcheck(L, (u64)n < ((u64)1 << (size * 8)), arg, "unsigned overflow");
            packint(&b, (u64)n, h.little, size, false);
            break;
        }
        case K_FLOAT: {
            float f = (float)luaL_checknumber(L, arg);
            char buff[4];
            copywithendian(buff, (const char *)&f, 4, h.little);
            luaL_addlstring(&b, buff, 4);
            break;
        }
        case K_DOUBLE: {
            f64 f = luaL_checknumber(L, arg);
            char buff[8];
            copywithendian(buff, (const char *)&f, 8, h.little);
            luaL_addlstring(&b, buff, 8);
            break;
        }
        case K_CHAR: {
            size_t len;
            const char *s = luaL_checklstring(L, arg, &len);
            luaL_argcheck(L, len <= (size_t)size, arg, "string longer than given size");
            luaL_addlstring(&b, s, len);
            while (len++ < (size_t)size) luaL_addchar(&b, 0);
            break;
        }
        case K_STRING: {
            size_t len;
            const char *s = luaL_checklstring(L, arg, &len);
            luaL_argcheck(L, size >= 8 || len < ((size_t)1 << (size * 8)), arg, "string length does not fit in given size");
            packint(&b, (u64)len, h.little, size, false);
            luaL_addlstring(&b, s, len);
            total += len;
            break;
        }
        case K_ZSTR: {
            size_t len;
            const char *s = luaL_checklstring(L, arg, &len);
            luaL_argcheck(L, strlen(s) == len, arg, "string contains zeros");
            luaL_addlstring(&b, s, len);
            luaL_addchar(&b, 0);
            total += len + 1;
            break;
        }
        case K_PADDING: luaL_addchar(&b, 0); /* fall through */
        case K_PADDALIGN: case K_NOP: arg--; break;
        }
    }
    luaL_pushresult(&b);
    return 1;
}

static int s_packsize(lua_State *L)
{
    const char *fmt = luaL_checkstring(L, 1);
    pheader h = { L, true, 1 };
    size_t total = 0;
    while (*fmt) {
        int size, ntoalign;
        pkind opt = getdetails(&h, total, &fmt, &size, &ntoalign);
        luaL_argcheck(L, opt != K_STRING && opt != K_ZSTR, 1, "variable-length format");
        total += (size_t)(ntoalign + size);
        luaL_argcheck(L, total <= 0x7FFFFFFF, 1, "format result too large");
    }
    lua_pushinteger(L, (lua_Integer)total);
    return 1;
}

static f64 unpackint(lua_State *L, const char *str, bool little, int size, bool issigned)
{
    u64 res = 0;
    int limit = size <= 8 ? size : 8;
    for (int i = limit - 1; i >= 0; i--) {
        res <<= 8;
        res |= (u64)(u8)str[little ? i : size - 1 - i];
    }
    if (size < 8) {
        if (issigned) {
            u64 m = (u64)1 << (size * 8 - 1);
            res = ((res ^ m) - m);
        }
    }
    (void)L;
    return issigned ? (f64)(i64)res : (f64)res;
}

static int s_unpack(lua_State *L)
{
    size_t ld;
    const char *fmt = luaL_checkstring(L, 1);
    const char *data = luaL_checklstring(L, 2, &ld);
    size_t pos = (size_t)posrelat(luaL_optlong(L, 3, 1), ld) - 1;
    luaL_argcheck(L, pos <= ld, 3, "initial position out of string");
    pheader h = { L, true, 1 };
    int n = 0;
    while (*fmt) {
        int size, ntoalign;
        pkind opt = getdetails(&h, pos, &fmt, &size, &ntoalign);
        if ((size_t)ntoalign + (size_t)size > ld - pos) luaL_argerror(L, 2, "data string too short");
        pos += (size_t)ntoalign;
        luaL_checkstack(L, 2, "too many results");
        n++;
        switch (opt) {
        case K_INT: case K_UINT: lua_pushnumber(L, unpackint(L, data + pos, h.little, size, opt == K_INT)); break;
        case K_FLOAT: {
            float f;
            copywithendian((char *)&f, data + pos, 4, h.little);
            lua_pushnumber(L, f);
            break;
        }
        case K_DOUBLE: {
            f64 f;
            copywithendian((char *)&f, data + pos, 8, h.little);
            lua_pushnumber(L, f);
            break;
        }
        case K_CHAR: lua_pushlstring(L, data + pos, (size_t)size); break;
        case K_STRING: {
            size_t len = (size_t)unpackint(L, data + pos, h.little, size, false);
            luaL_argcheck(L, len <= ld - pos - (size_t)size, 2, "data string too short");
            lua_pushlstring(L, data + pos + size, len);
            pos += len;
            break;
        }
        case K_ZSTR: {
            size_t len = strlen(data + pos);
            luaL_argcheck(L, pos + len < ld, 2, "unfinished string for format 'z'");
            lua_pushlstring(L, data + pos, len);
            pos += len + 1;
            break;
        }
        case K_PADDALIGN: case K_PADDING: case K_NOP: n--; break;
        }
        pos += (size_t)size;
    }
    lua_pushinteger(L, (lua_Integer)pos + 1);
    return n + 1;
}

/* -------------------------------------------------------------- JSON */

#define JSON_DEPTH 200

/* The text grows in a userdata kept at a fixed stack slot, so the stack
 * is free to use while tables are walked (a luaL_Buffer is not). */
typedef struct {
    lua_State *L;
    int slot;
    char *p;
    size_t n, cap;
} jbuf;

static void jb_add(jbuf *b, const char *s, size_t len)
{
    if (b->n + len > b->cap) {
        size_t nc = b->cap * 2;
        while (nc < b->n + len) nc *= 2;
        char *np = (char *)lua_newuserdata(b->L, nc);
        memcpy(np, b->p, b->n);
        lua_replace(b->L, b->slot);
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
}

static void jb_str(jbuf *b, const char *s) { jb_add(b, s, strlen(s)); }
static void jb_char(jbuf *b, char c) { jb_add(b, &c, 1); }

static void json_str(jbuf *b, const char *s, size_t len)
{
    jb_char(b, '"');
    size_t run = 0; /* characters that need no escape go in together */
    for (size_t i = 0; i < len; ++i) {
        u8 c = (u8)s[i];
        const char *esc = NULL;
        char e[8];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7F) {
                snprintf(e, sizeof e, "\\u%04x", c);
                esc = e;
            }
        }
        if (!esc) continue;
        jb_add(b, s + run, i - run);
        jb_str(b, esc);
        run = i + 1;
    }
    jb_add(b, s + run, len - run);
    jb_char(b, '"');
}

static void json_num(jbuf *b, f64 v)
{
    char t[40];
    if (v != v || v > 1.7976931348623157e308 || v < -1.7976931348623157e308) {
        jb_str(b, "null");
        return;
    }
    if (v == floor(v) && v > -1e15 && v < 1e15) snprintf(t, sizeof t, "%.0f", v);
    else {
        snprintf(t, sizeof t, "%.14g", v);
        if (strtod(t, NULL) != v) snprintf(t, sizeof t, "%.17g", v);
    }
    jb_str(b, t);
}

/* the table at idx is a list: keys 1..n and nothing else */
static bool json_is_array(lua_State *L, int idx, size_t *n_out)
{
    size_t n = lua_objlen(L, idx), count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        f64 k = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1;
        if (k != floor(k) || k < 1 || k > (f64)n) {
            lua_pop(L, 1);
            return false;
        }
        count++;
    }
    *n_out = n;
    return count == n;
}

static void json_value(lua_State *L, jbuf *b, int idx, int depth)
{
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: jb_str(b, lua_toboolean(L, idx) ? "true" : "false"); break;
    case LUA_TNUMBER: json_num(b, lua_tonumber(L, idx)); break;
    case LUA_TSTRING: {
        size_t len;
        const char *s = lua_tolstring(L, idx, &len);
        json_str(b, s, len);
        break;
    }
    case LUA_TTABLE: {
        if (depth > JSON_DEPTH) luaL_error(L, "Cannot encode a table nested this deep (or one that contains itself)");
        luaL_checkstack(L, 6, "JSON too deep");
        size_t n;
        if (json_is_array(L, idx, &n)) {
            jb_char(b, '[');
            for (size_t i = 1; i <= n; ++i) {
                if (i > 1) jb_char(b, ',');
                lua_rawgeti(L, idx, (int)i);
                json_value(L, b, lua_gettop(L), depth + 1);
                lua_pop(L, 1);
            }
            jb_char(b, ']');
            break;
        }
        jb_char(b, '{');
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            int kt = lua_type(L, -2);
            if (kt == LUA_TSTRING || kt == LUA_TNUMBER) {
                if (!first) jb_char(b, ',');
                first = false;
                lua_pushvalue(L, -2); /* a copy: tolstring would change a number key */
                size_t len;
                const char *k = lua_tolstring(L, -1, &len);
                json_str(b, k, len);
                lua_pop(L, 1);
                jb_char(b, ':');
                json_value(L, b, lua_gettop(L), depth + 1);
            }
            lua_pop(L, 1);
        }
        jb_char(b, '}');
        break;
    }
    default: jb_str(b, "null"); break; /* nil, functions, instances, ... */
    }
}

int json_encode(lua_State *L)
{
    luaL_checkany(L, 2);
    lua_settop(L, 2);
    jbuf b = { L, 3, NULL, 0, 256 };
    b.p = (char *)lua_newuserdata(L, b.cap);
    json_value(L, &b, 2, 0);
    lua_pushlstring(L, b.p, b.n);
    return 1;
}

typedef struct {
    lua_State *L;
    const char *s, *p, *e;
} jparse;

static int json_fail(jparse *j)
{
    return luaL_error(j->L, "Can't parse JSON");
}

static void ws(jparse *j)
{
    while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static u32 hex4(jparse *j)
{
    if (j->e - j->p < 4) json_fail(j);
    u32 v = 0;
    for (int i = 0; i < 4; ++i) {
        char c = *j->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (u32)(c - 'A' + 10);
        else json_fail(j);
    }
    return v;
}

static void jstring(jparse *j)
{
    luaL_Buffer b;
    luaL_buffinit(j->L, &b);
    j->p++; /* " */
    for (;;) {
        if (j->p >= j->e) json_fail(j);
        char c = *j->p++;
        if (c == '"') break;
        if ((u8)c < 0x20) json_fail(j);
        if (c != '\\') {
            luaL_addchar(&b, c);
            continue;
        }
        if (j->p >= j->e) json_fail(j);
        c = *j->p++;
        switch (c) {
        case '"': case '\\': case '/': luaL_addchar(&b, c); break;
        case 'b': luaL_addchar(&b, '\b'); break;
        case 'f': luaL_addchar(&b, '\f'); break;
        case 'n': luaL_addchar(&b, '\n'); break;
        case 'r': luaL_addchar(&b, '\r'); break;
        case 't': luaL_addchar(&b, '\t'); break;
        case 'u': {
            u32 cp = hex4(j);
            if (cp >= 0xD800 && cp <= 0xDBFF && j->e - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                const char *save = j->p;
                j->p += 2;
                u32 lo = hex4(j);
                if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000u + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else j->p = save;
            }
            char e[4];
            luaL_addlstring(&b, e, (size_t)utf8_encode(e, cp));
            break;
        }
        default: json_fail(j);
        }
    }
    luaL_pushresult(&b);
}

static void jvalue(jparse *j, int depth)
{
    lua_State *L = j->L;
    if (depth > JSON_DEPTH) json_fail(j);
    luaL_checkstack(L, 4, "JSON too deep");
    ws(j);
    if (j->p >= j->e) json_fail(j);
    char c = *j->p;
    if (c == '{') {
        j->p++;
        lua_newtable(L);
        ws(j);
        if (j->p < j->e && *j->p == '}') { j->p++; return; }
        for (;;) {
            ws(j);
            if (j->p >= j->e || *j->p != '"') json_fail(j);
            jstring(j);
            ws(j);
            if (j->p >= j->e || *j->p++ != ':') json_fail(j);
            jvalue(j, depth + 1);
            lua_rawset(L, -3);
            ws(j);
            if (j->p >= j->e) json_fail(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p++ == '}') return;
            json_fail(j);
        }
    }
    if (c == '[') {
        j->p++;
        lua_newtable(L);
        ws(j);
        if (j->p < j->e && *j->p == ']') { j->p++; return; }
        for (int i = 1;; ++i) {
            jvalue(j, depth + 1);
            lua_rawseti(L, -2, i);
            ws(j);
            if (j->p >= j->e) json_fail(j);
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p++ == ']') return;
            json_fail(j);
        }
    }
    if (c == '"') { jstring(j); return; }
    if (j->e - j->p >= 4 && strncmp(j->p, "true", 4) == 0) { j->p += 4; lua_pushboolean(L, 1); return; }
    if (j->e - j->p >= 5 && strncmp(j->p, "false", 5) == 0) { j->p += 5; lua_pushboolean(L, 0); return; }
    if (j->e - j->p >= 4 && strncmp(j->p, "null", 4) == 0) { j->p += 4; lua_pushnil(L); return; }
    /* a number, strictly as JSON writes them */
    const char *q = j->p;
    if (q < j->e && *q == '-') q++;
    if (q >= j->e || !is_digit(*q)) json_fail(j);
    if (*q == '0') q++;
    else while (q < j->e && is_digit(*q)) q++;
    if (q < j->e && *q == '.') {
        q++;
        if (q >= j->e || !is_digit(*q)) json_fail(j);
        while (q < j->e && is_digit(*q)) q++;
    }
    if (q < j->e && (*q == 'e' || *q == 'E')) {
        q++;
        if (q < j->e && (*q == '+' || *q == '-')) q++;
        if (q >= j->e || !is_digit(*q)) json_fail(j);
        while (q < j->e && is_digit(*q)) q++;
    }
    char t[64];
    size_t n = (size_t)(q - j->p);
    if (n >= sizeof t) { /* very long: the digits that matter fit */
        lua_pushlstring(L, j->p, n);
        lua_pushnumber(L, strtod(lua_tostring(L, -1), NULL));
        lua_remove(L, -2);
    } else {
        memcpy(t, j->p, n);
        t[n] = 0;
        lua_pushnumber(L, strtod(t, NULL));
    }
    j->p = q;
}

int json_decode(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);
    jparse j = { L, s, s, s + len };
    jvalue(&j, 0);
    ws(&j);
    if (j.p != j.e) json_fail(&j);
    return 1;
}

int url_encode(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (size_t i = 0; i < len; ++i) {
        u8 c = (u8)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            luaL_addchar(&b, (char)c);
        else {
            char e[4];
            snprintf(e, sizeof e, "%%%02X", c);
            luaL_addstring(&b, e);
        }
    }
    luaL_pushresult(&b);
    return 1;
}

/* -------------------------------------------------------------- setup */

static void sub_lib(lua_State *L, int base, const char *name, const luaL_Reg *fns)
{
    lua_getfield(L, base, name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, base, name);
    }
    reg(L, fns);
    lua_pop(L, 1);
}

static void type_meta(lua_State *L, const char *name, lua_CFunction index, lua_CFunction tostr)
{
    luaL_newmetatable(L, name);
    lua_pushcfunction(L, index);
    lua_setfield(L, -2, "__index");
    if (tostr) {
        lua_pushcfunction(L, tostr);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pushstring(L, name);
    lua_setfield(L, -2, "__type");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
}

/* A read-only constructor table (Random.new, DateTime.now...). */
static void ctor_lib(lua_State *L, int base, const char *name, const luaL_Reg *fns)
{
    lua_newtable(L);
    reg(L, fns);
    lua_newtable(L); /* proxy */
    lua_newtable(L);
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "The metatable is locked");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_setreadonly(L, -1, 1);
    lua_setfield(L, base, name);
    lua_pop(L, 1);
}

void open_luaulib(lua_State *L, int base)
{
    lua_newtable(L);
    reg(L, k_bit32);
    lua_setfield(L, base, "bit32");
    lua_newtable(L);
    reg(L, k_utf8);
    lua_pushliteral(L, "[%z\x01-\x7F\xC2-\xF4][\x80-\xBF]*"); /* %z: Lua 5.1 patterns stop at a zero */
    lua_setfield(L, -2, "charpattern");
    lua_setfield(L, base, "utf8");
    lua_newtable(L);
    reg(L, k_debug);
    lua_setfield(L, base, "debug");

    static const luaL_Reg tab[] = { { "move", t_move }, { "freeze", t_freeze }, { "isfrozen", t_isfrozen },
                                    { "clone", t_clone }, { NULL, NULL } };
    sub_lib(L, base, "table", tab);
    static const luaL_Reg math[] = { { "noise", m_noise }, { "log", m_log }, { "clamp", m_clamp }, { NULL, NULL } };
    sub_lib(L, base, "math", math);
    static const luaL_Reg os[] = { { "time", o_time }, { "date", o_date }, { NULL, NULL } };
    sub_lib(L, base, "os", os);
    static const luaL_Reg co[] = { { "close", c_close }, { "isyieldable", c_isyieldable }, { NULL, NULL } };
    sub_lib(L, base, "coroutine", co);
    static const luaL_Reg str[] = { { "pack", s_pack }, { "packsize", s_packsize }, { "unpack", s_unpack },
                                    { NULL, NULL } };
    sub_lib(L, base, "string", str);
    static const luaL_Reg g[] = { { "newproxy", g_newproxy }, { "rawlen", g_rawlen }, { "gcinfo", g_gcinfo },
                                  { "tick", g_tick }, { NULL, NULL } };
    for (int i = 0; g[i].name; ++i) {
        lua_pushcfunction(L, g[i].func);
        lua_setfield(L, base, g[i].name);
    }

    type_meta(L, "Random", r_index, r_tostring);
    lua_pop(L, 1);
    static const luaL_Reg rnd[] = { { "new", r_new }, { NULL, NULL } };
    ctor_lib(L, base, "Random", rnd);

    type_meta(L, "DateTime", dt_index, dt_toiso);
    lua_pushcfunction(L, dt_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, dt_lt);
    lua_setfield(L, -2, "__lt");
    lua_pushcfunction(L, dt_le);
    lua_setfield(L, -2, "__le");
    lua_pop(L, 1);
    static const luaL_Reg dt[] = { { "now", dt_now }, { "fromUnixTimestamp", dt_fromunix },
                                   { "fromUnixTimestampMillis", dt_fromunixms },
                                   { "fromUniversalTime", dt_fromuniversal }, { "fromLocalTime", dt_fromlocal },
                                   { "fromIsoDate", dt_fromiso }, { NULL, NULL } };
    ctor_lib(L, base, "DateTime", dt);
}
