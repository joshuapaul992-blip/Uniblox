# Uniblox: project plan

Uniblox is a from-scratch sandbox platform in the user-generated-content
genre. Players build places out of simple parts, script them, and (later)
play them together online. All art, branding, avatar style and scripting
API are original.

## Fixed decisions

| Topic | Decision |
|---|---|
| Codename | **Uniblox** |
| Target OS | Windows NT 3.51, NT 4.0, 2000, XP and later (one .exe for all) |
| RAM | **384 MB minimum, 512 MB+ recommended** |
| CPU | Must run correctly on a plain Pentium (no MMX/SSE); faster paths chosen at runtime. SSE only on Windows 2000+, after checking that the OS saves SSE registers |
| Language | **C99**, freestanding: no Microsoft C runtime is ever linked |
| Toolchain | **Cross-compiled from Linux with GCC or clang** (mingw-w64 headers and import libraries) |
| Architecture | Client + server from day one. Single-player runs an in-process server over a loopback transport |

### What the RAM budget buys us

At 384 MB we can afford:
- whole places and their asset packs held in RAM
- 16/32-bit textures instead of only 8-bit palettized ones
- generous Lua heaps (64 MB+ for scripts)
- large audio buffers
- part limits set by CPU rather than memory: roughly 10,000 static and 500 moving parts as an initial target, adjusted after profiling

The engine checks installed RAM at startup and warns below 384 MB.

## Platform rules (NT 3.51 baseline)

- Direct imports are limited to functions on `tools/nt351_imports.txt`.
  `make check` fails the build otherwise.
- Newer APIs are looked up at runtime with `GetProcAddress` and must have a
  fallback. Examples: `GlobalMemoryStatusEx`, mouse wheel, Raw Input,
  OpenGL 1.1 extras, Direct3D, Winsock 2.
- The PE image is marked OS/subsystem 3.10, with DllCharacteristics 0, no
  TLS directory and no symbol table.
- Text is UTF-16 at the Win32 boundary (the `W` APIs) and UTF-8 inside the
  engine. We convert ourselves because NT 3.51 lacks `CP_UTF8`.
- No `InterlockedCompareExchange` (it arrived in NT 4.0). On NT 3.51,
  `InterlockedIncrement`/`InterlockedDecrement` return only the sign of the
  result.
- Networking uses Winsock 1.1 (`wsock32.dll`), `select()` and IPv4.
- Sound uses `waveOut` with our own mixer.

## Architecture

```
src/crt/        memcpy/memset/... (GCC emits calls to these); later
                setjmp/longjmp, strtod, formatting for Lua
src/core/       types, strings, math, containers, memory arenas
src/platform/   OS interface; win32/ backend (later posix/ for the server)
src/render/     sw/ software rasterizer (guaranteed path), gl/ OpenGL 1.x
src/world/      object tree, typed properties, events, place file I/O
src/physics/    box/sphere collision, impulse solver, sleeping islands
src/script/     sandboxed Lua 5.1 + Uniblox API
src/net/        loopback + UDP transports, reliability, replication
src/ui/         immediate-mode GUI (HUD and editor)
src/app/        client / server / editor front ends
tools/          build and verification tools
```

## Milestones

| # | Milestone | Exit test | Status |
|---|---|---|---|
| 0 | Toolchain + hello window | One .exe runs on NT 3.51, 4.0, 2000, XP | **Built; passes `make check` and Wine. Still needs a run on real NT 3.51/4.0 in 86Box or PCem** |
| 1 | Platform layer | Input, timing, audio (waveOut), file I/O, a log file | |
| 2 | Software renderer | Textured, lit room of parts at 15+ fps, 320x240, on a Pentium-class VM, in 8-bit and 16-bit display modes | |
| 3 | World model + place files | Save/load round-trips byte for byte | |
| 4 | Physics + character | Walk, jump, collide; a box stack comes to rest | |
| 5 | Lua scripting | A touch-activated door works; hostile scripts are contained | |
| 6 | In-game editor | Build an obstacle course without writing code | |
| 7 | OpenGL renderer + settings | Hardware path on 2000/XP | |
| 8 | Loopback networking | Single-player runs entirely through replication | |
| 9 | UDP multiplayer | 8 players on LAN; playable over a simulated 56k link | |
| 10 | Platform services | Server browser, accounts, publish/download places | |

## Multiplayer design notes

- Server-authoritative UDP with two channels: unreliable for positions,
  reliable and ordered for object creation, property changes and events.
- Budget 2–4 KB/s per client: compressed positions and rotations, delta
  snapshots, interest management.
- Clients predict their own character; other players are shown about
  100 ms behind and interpolated.
- Encryption: embedded Monocypher (X25519 + ChaCha20-Poly1305), with the
  server key pinned in the client. Entropy comes from timer jitter and
  input, because NT 3.51 has no OS random-number source.
- The dedicated server also builds for Linux (POSIX backend).

## Open items

- Real-hardware/emulator validation of milestone 0 on NT 3.51 and NT 4.0
  (86Box or PCem). Wine is only a smoke test.
- `.eh_frame` from libgcc ends up in the image (shown truncated as
  `.eh_fram`). The loader ignores it; drop it later with a linker script.
- 8-bit display mode: GDI currently maps our 24-bit output onto the
  system palette. Milestone 2 adds a proper palette path.
- The name "Uniblox" is close to an existing trademark in the same genre.
  Fine as a codename; check it before any public release.
