# Uniblox's C library for Lua

Uniblox has no C runtime (it is built with `-nostdlib` so it loads on
Windows NT 3.51). Lua's source files are compiled with this folder first on
the include path, so `<string.h>`, `<stdio.h>` and friends come from here.
Every function is renamed to `ubc_*` (see each header) and implemented in
`src/script/ub_libc.c`, so the host unit tests can link the same code next
to the system C library.

Only what Lua 5.1's core and its base, string, table and math libraries use
is here. File and stdio functions exist so Lua links, but files never open
(scripts cannot reach the disk); text written to stdout/stderr goes to the
log.
