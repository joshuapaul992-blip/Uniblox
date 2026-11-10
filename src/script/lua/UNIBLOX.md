# Lua 5.1.5 in Uniblox

These files are Lua 5.1.5 from <https://www.lua.org/ftp/lua-5.1.5.tar.gz>
(SHA-256 `2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333`),
MIT licence: see `COPYRIGHT`. Left out: the standalone interpreter and
compiler, and the io, os, debug and package libraries, which scripts must
not have.

Lua is compiled against Uniblox's own C library (`src/script/libc`), since
the game links no C runtime.

## Changes

Every change is marked `UNIBLOX` in the source.

- `luaconf.h`: the limits below.
- `lstrlib.c`: limits that keep scripts inside the sandbox. The script time
  limit is an instruction-count hook, which only runs between Lua
  instructions. These functions run in C and could otherwise run for ever
  or overflow the C stack:
  - pattern matching counts its steps (at most 2,000,000 per call) and
    limits its recursion to 200 levels, as Lua 5.2 does;
  - plain `string.find` counts the bytes it compares;
  - `string.rep` refuses results over 16 MB, and returns at once for empty
    strings (`(""):rep(1e9)` would otherwise loop a billion times).
- `llex.c`, `lparser.c`: the compound assignments of Roblox's Luau (`+=`
  `-=` `*=` `/=` `%=` `^=` `..=`) and `continue` in loops.
- `llex.c`, `lparser.c` (milestone 8B): the rest of Luau's 2022 syntax.
  Type annotations are parsed and dropped, as Roblox's VM ignores them:
  `local x: T`, typed parameters, `...: T`, return types, generic `<T>`
  on functions, `for i: number = ...`, `type Name<T> = ...`,
  `export type`, and `expr :: T` casts (new tokens `::` and `->`). Also
  if-then-else expressions, `0b` binary literals, `_` digit separators,
  and the `\x`, `\z` and `\u{...}` string escapes.
- `lvm.c` (OP_TFORLOOP): generalized iteration, `for k, v in t do` over a
  table that cannot be called, walks it like `next`.
- `lobject.h`, `ltable.c`, `lvm.c`, `lapi.c` (milestone 8B): Luau's
  `table.freeze`. Tables have a `readonly` byte; assigning to a frozen
  table (also through `rawset`, `table.insert` and `setmetatable`) raises
  "attempt to modify a readonly table". `lua_setreadonly` and
  `lua_getreadonly` reach it.
- `lapi.c` (milestone 8B): `lua_resetthread`, which ends a suspended or
  failed coroutine for `coroutine.close`, and `lua_getparams`, the
  parameter count of a Lua function for `debug.info(f, "a")`.
- `lstrlib.c` (milestone 8B): `string.format("%s", x)` accepts any value,
  through `__tostring` or the usual `tostring` text, as Luau does.

The rest of Luau's library (bit32, utf8, Random, DateTime, os.date,
string.pack and the others) is written outside the Lua sources, in
`src/script/ub_luaulib.c`.
