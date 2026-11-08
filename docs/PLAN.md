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
src/core/       types, strings, UTF-8, safe paths, log, math (later containers, arenas)
src/audio/      WAV loader, software mixer (32 voices, pitch, pan)
src/platform/   OS interface; win32/ backend (later posix/ for the server)
src/render/     software renderer: meshes, procedural textures, palette (later gl/ for OpenGL 1.x)
src/world/      object tree, typed properties, events, place file I/O
src/physics/    box/sphere collision, impulse solver, sleeping islands
src/script/     sandboxed Lua 5.1 + Uniblox API
src/net/        loopback + UDP transports, reliability, replication
src/ui/         immediate-mode GUI (HUD and editor)
src/app/        client / server / editor front ends
tools/          build and verification tools (PE checker, Wine smoke test)
tests/          host unit tests (make test)
```

## Milestones

| # | Milestone | Exit test | Status |
|---|---|---|---|
| 0 | Toolchain + hello window | One .exe runs on NT 3.51, 4.0, 2000, XP | **Done.** Confirmed running on a real install |
| 1 | Platform layer | Input, timing, audio (waveOut), file I/O, a log file | **Done.** Confirmed working on a real install |
| 2 | Software renderer | Textured, lit room of parts at 15+ fps, 320x240, on a Pentium-class VM, in 8-bit and 16-bit display modes | **Built.** Passes unit tests and the Wine smoke test. Needs: F5 benchmark on a Pentium-class VM, and a run on a real 256-colour desktop |
| 2.5 | High-quality OpenGL renderer | Native 1080p/1440p, resizable and fullscreen, shadows, specular, anti-aliasing; 120 fps cap by default | **Built.** Passes unit tests and the Wine smoke test with both renderers. Needs a run on a real GPU |
| 3 | World model + place files | Save/load round-trips byte for byte | |
| 4 | Physics + character | Walk, jump, collide; a box stack comes to rest | |
| 5 | Lua scripting | A touch-activated door works; hostile scripts are contained | |
| 6 | In-game editor | Build an obstacle course without writing code | |
| 7 | Renderer polish + settings menu | In-game settings UI; more effects | Hardware OpenGL path moved forward into milestone 2.5 |
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

## Milestone 1 notes

- **Audio:** `waveOut` with 6 buffers of 512 frames (about 140 ms queued),
  refilled by a polling thread. The format falls back from 22 kHz 16-bit
  stereo to 11 kHz 8-bit mono for old cards. With no device the game runs
  silently.
- **Mouse-look:** the cursor is re-centred every frame (Raw Input is XP-only).
- **Mouse wheel:** `WM_MOUSEWHEEL` on NT 4.0+, plus the registered
  `MSWHEEL_ROLLMSG` message that IntelliPoint drivers send on NT 3.51.
- **Files:** every game path is relative to the `.exe` folder and checked
  by `ub_path_is_safe()`, which rejects `..`, drive letters and DOS device
  names. This matters once places and scripts from other players can name
  files. Saves go to a `.tmp` file first and are renamed over the old one.
- **Text input:** chat-style, with Enter or `/` to open it, so movement keys
  don't leak into typed text.

## Milestone 2 notes

- **Pipeline:** per part, a bounding-sphere frustum cull; then per face, a
  back-face cull, flat sun + sky lighting, clipping against the near plane
  and all four screen edges, and scan conversion as one convex polygon with
  the top-left fill rule (no gaps or overlaps between faces).
- **Visibility:** a 32-bit z-buffer of 1/z, so intersecting parts sort
  correctly per pixel.
- **Textures:** perspective-correct. 1/z, u/z and v/z are interpolated and
  divided once every 16 pixels. Textures are 64x64 brightness maps
  generated at startup (grid, brick, wood, grass, metal, stone), so any
  part colour works with any material and the `.exe` needs no image files.
- **Per-pixel cost:** each face's colour comes from a 32-entry lookup table
  (lighting x texel brightness, or palette index with 2x2 dither). The
  tables are cached across frames, so a static scene rebuilds almost none.
- **8-bit mode:** 236 colours (6x6x6 cube + 20 greys) in palette slots
  10-245, leaving Windows' 20 static colours alone. On a 256-colour desktop
  the game builds and realises a Windows palette and blits with
  `DIB_PAL_COLORS`.
- **Shapes:** box, wedge, cylinder (12 sides) and ball (6x10), all flat
  shaded.
- **Measuring:** F5 runs a fixed 10-second flythrough and logs fps, render
  and display time, CPU (from CPUID, with a clock measured from the TSC),
  OS and colour depth. F6 drops the window to 1x to skip GDI's stretch.

## Milestone 2.5 notes

- **Two backends behind one interface** (`ub_render_*`). Parts are queued
  per frame, so the OpenGL path can draw the scene twice (shadow map, then
  the screen).
- **OpenGL is loaded at run time** (`LoadLibraryA("opengl32.dll")`, the
  pixel-format calls looked up in gdi32), so nothing GL-related is
  imported and NT 3.51 still starts. With `renderer=auto`, Microsoft's
  unaccelerated "GDI Generic" driver is skipped, because the software
  renderer is faster. 256-colour desktops always use software.
- **Shader path (GL 2.0 + framebuffer objects):**
  - per-pixel Lambert sun plus hemisphere sky/ground ambient
  - Blinn-Phong highlights tuned per material, and a soft Fresnel rim
  - 2048x2048 shadow map around the camera focus, snapped to texels so
    edges don't shimmer, with 3x3 PCF and a normal-offset bias
  - per-pixel sky with a sun disc and glow, and exponential fog towards the
    horizon colour
  - 4x MSAA through a multisampled framebuffer resolved by a blit
- **Fixed-function path (GL 1.1, or `gl_shaders=0`):** per-vertex lighting
  with specular, fog and gradient sky. Big quads are split into ~6-unit
  cells so per-vertex fog and lighting stay correct.
- **Textures:** the same procedural designs regenerated at 256x256 from
  continuous coordinates, box-filtered mipmaps, up to 16x anisotropic
  filtering.
- **Shapes:** high-detail meshes (32-sided cylinders, 16x32 balls) with
  smooth vertex normals and seam-free wrapped texture coordinates. The
  software renderer keeps the low-detail meshes.
- **Text:** a GDI-rasterised font atlas for OpenGL, with padded cells and
  glyph-sized quads. The software renderer draws its text with GDI at
  window resolution after stretching, so it stays sharp.
- **Window:**
  - resizable; borderless fullscreen on the window's monitor (F11 or
    Alt+Enter)
  - `SetProcessDPIAware` on Vista and later, so high-DPI monitors aren't
    blurry
  - default size is 75% of the screen at 16:9
- **Frame pacing:** an absolute schedule, with Sleep for the bulk of the
  wait and a short spin at the end. It never waits when running below the
  cap. Measured under Wine at exactly 120.0, 144.0 and 59.9 fps for caps of
  120, 144 and 60. Vsync is off by default so the cap governs.
- **Bugs found while testing:**
  - The wedge's slope normal pointed inward, because the slope passes
    through the shape's centre, so the old "away from the centre"
    orientation test was undecided. It now compares against the vertex
    average.
  - The 2D pass left the depth test off, so the shadow pass wrote no depth.
    All 3D state is now reset at the start of each frame.

## Open items

- Milestone 2 speed on a Pentium-class machine: run F5 in 86Box or PCem
  and send the `benchmark:` line from `uniblox.log`. On a modern CPU under
  Wine a frame takes about 1.1 ms to render and 0.9 ms to display; that
  says little about a Pentium.
- The 256-colour palette path can't be tested in Wine: its 8-bit display
  mode shows almost nothing in colour even for the milestone 1 build, which
  has no palette code. It needs a run on a real 256-colour NT desktop.
- Milestone 2.5 on real hardware: check the OpenGL path on an actual GPU
  (expected well above 120 fps at 1440p for this scene) and send the F5
  `benchmark:` line. Under Wine it was only tested on Mesa's software GPU.
- `.eh_frame` from libgcc ends up in the image (shown truncated as
  `.eh_fram`). The loader ignores it; drop it later with a linker script.
- The name "Uniblox" is close to an existing trademark in the same genre.
  Fine as a codename; check it before any public release.
