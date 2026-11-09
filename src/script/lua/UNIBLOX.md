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
