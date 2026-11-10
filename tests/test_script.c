/* Uniblox - scripting tests. */
#include <stdlib.h>

#include "script/lua/lauxlib.h"
#include "script/lua/lua.h"
#include "script/lua/lualib.h"
#include "test.h"

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

static void *test_alloc(void *ud, void *p, size_t osize, size_t nsize)
{
    (void)ud;
    (void)osize;
    if (nsize == 0) { free(p); return NULL; }
    return realloc(p, nsize);
}

/* Lua's language, our C library and the Luau syntax, on a bare state. */
static void test_language(void)
{
    char *src = read_text("tests/data/scripts/language.lua");
    CHECK(src != NULL);
    if (!src) return;
    lua_State *L = lua_newstate(test_alloc, NULL);
    lua_pushcfunction(L, luaopen_base); lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_string); lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_table); lua_call(L, 0, 0);
    lua_pushcfunction(L, luaopen_math); lua_call(L, 0, 0);
    int r = luaL_loadbuffer(L, src, strlen(src), "=language.lua");
    if (r != 0) printf("  load error: %s\n", lua_tostring(L, -1));
    CHECK(r == 0);
    if (r == 0) {
        r = lua_pcall(L, 0, 1, 0);
        if (r != 0) printf("  run error: %s\n", lua_tostring(L, -1));
        CHECK(r == 0 && lua_tonumber(L, -1) == 0);
    }
    /* syntax errors are reported, not crashes */
    static const char *const bad[] = { "x +=", "local a += 1", "repeat continue until true", "continue", "f() += 1" };
    for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); ++i) {
        CHECK(luaL_loadbuffer(L, bad[i], strlen(bad[i]), "=bad") == LUA_ERRSYNTAX);
        lua_pop(L, 1);
    }
    lua_close(L);
    free(src);
}

void test_script(void)
{
    test_language();
}
