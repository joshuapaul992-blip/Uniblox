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
src/render/     software and OpenGL renderers: meshes, procedural textures, palette
src/world/      object tree, welds, place files (.ubx) and the Roblox importer
src/physics/    convex hull + sphere collision, soft-step solver, sleeping islands, character
src/game/       a running place: world + physics + character + scripts; the demo place
src/script/     sandboxed Lua 5.1 + Roblox-style API, on its own small C library
src/net/        loopback + UDP transports, reliability, replication
src/ui/         immediate-mode GUI (the editor's panels, menus, dialogs, code editor)
src/editor/     the editor's model: selection, changes, undo, behaviours
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
| 3 | World model + place files | Save/load round-trips byte for byte | **Built.** `.ubx` round-trips byte for byte; real Roblox `.rbxl`/`.rbxm` files import. Passes unit tests and the Wine smoke test with both renderers. Needs a run on a real install |
| 4 | Physics + character | Walk, jump, collide; a box stack comes to rest | **Built.** Towers of 20 parts come to rest and sleep; the character walks, jumps, steps up, collides and pushes. Passes unit tests and the Wine smoke test with both renderers. Needs a run on a real install |
| 5 | Lua scripting | A touch-activated door works; hostile scripts are contained | **Built.** The door opens when walked into (unit test and Wine smoke test), and 28 hostile scripts are stopped or fail while the game carries on. Needs a run on a real install |
| 6 | In-game editor | Build an obstacle course without writing code | **Built** as a separate program, `ubedit.exe`. A course built with editor actions only is played to the finish by the character (unit test); the real editor is driven through its UI in tests and under Wine. Needs a run on a real install |
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
- **Real NT 3.51 report (video):**
  - An `opengl32.dll` built for newer Windows, sitting next to the `.exe`,
    made the loader show "Entry Point Not Found: GetDiskFreeSpaceExW". The
    game now loads OpenGL with `SetErrorMode` (no dialog), logs why a copy
    was rejected, and falls back to the system's `opengl32.dll`, then to
    the software renderer.
  - A second run (2560x1421, Sandy Bridge CPU) rendered in 8-14 ms but
    presented in 153 ms. The breakdown said only ~7 ms because NT 3.51's
    GDI runs in CSRSS and queues calls; the cost landed at the next flush.
    Present timing now calls GdiFlush after each step, and the 32-bit path
    renders through a DIB section (shared memory, NT 3.5+, looked up at run
    time), so frames are no longer shipped to CSRSS with StretchDIBits.
    Jumping also accepts a press that began and ended between two frames,
    which matters at low frame rates.
  - The tester's display driver is WINVBE (VBE frame buffer). On NT 3.x
    it doesn't enable write combining (NT 4.0+ only), so every byte
    written to the screen is an uncached PCIe write: about 95 MB/s
    measured, or 150 ms for a 2560x1421x32 frame. No user-mode change gets
    past that; fewer bytes per frame does (smaller window, or a 16-bit
    desktop).
  - WINVBE's own OpenGL for NT 3.x is Mesa 7.8.2 in software and doesn't
    mark its pixel formats generic. Auto mode now recognises software
    OpenGL by its renderer name (software / llvmpipe / softpipe / swrast /
    GDI Generic) and uses Uniblox's renderer instead; `renderer=opengl`
    still forces it.
  - The software renderer took 2.7 ms per frame, but presenting at
    1920x1080 took 90 ms, so the game ran at 10-12 fps. Presenting now
    uses a 32-bit `BI_BITFIELDS` DIB when the system accepts one (no
    per-pixel colour conversion; falls back to 24-bit), and stretches
    straight onto the window when there's no text (F3). The overlay and
    the benchmark log show where present time goes (copy / stretch / blit).
- **Bugs found while testing:**
  - The wedge's slope normal pointed inward, because the slope passes
    through the shape's centre, so the old "away from the centre"
    orientation test was undecided. It now compares against the vertex
    average.
  - The 2D pass left the depth test off, so the shadow pass wrote no depth.
    All 3D state is now reset at the start of each frame.

## Milestone 3 notes

- **World model** (`src/world/ub_world.c`): a tree of instances (Workspace,
  Lighting, Folder, Model, Part, SpawnLocation; anything else is kept by
  class name only), parts in a flat array, and lighting settings. Each
  part has a position, rotation matrix, size, colour, shape, material,
  transparency and flags (anchored, can-collide, placeholder, glow).
- **`.ubx` format:** the header `UBXPLACE` plus a version, then
  length-prefixed chunks: `INFO` (name), `STRS` (string table), `INST`
  (20 bytes per instance), `PART` (68 bytes per part), `LITE`, `END `.
  Everything is little-endian and written in tree order, so saving the
  same world always gives the same bytes. Readers skip chunks they don't
  know, so later versions can add some. The loader treats files as
  untrusted: every count and index is bounds-checked, a parent must come
  before its children (so there are no cycles), and all values are
  clamped. It's fuzzed in the unit tests.
- **Roblox importer** (`src/world/ub_rbxl.c`), written from the public
  format description in rbx-dom's `docs/binary.md`:
  - Reads the `INST`, `PROP`, `PRNT`, `SSTR`, `META` and `END` chunks. Each
    chunk can be uncompressed, LZ4 or Zstandard.
  - `src/core/ub_compress.c` is a complete LZ4 block decoder and a
    Zstandard frame decoder (RFC 8878: Huffman literals, FSE sequences,
    repeat offsets, skippable frames). It rejects dictionaries and skips
    the checksum. Both are checked against the real `lz4`/`zstd` tools at
    several levels, and fuzzed.
  - Roblox is right-handed and Uniblox is left-handed: positions get z
    negated, and rotations become S·R·S with S = diag(1, 1, −1).
    Cylinders turn from Roblox's X axis to our Y axis. Wedges and corner
    wedges turn 180° because our slopes face the other way. All 24 compact
    rotation IDs are tested against the XML copy of the same model.
  - Mesh parts and unions become placeholder boxes, because their geometry
    is online and Uniblox doesn't download anything. Negate parts are never
    drawn. Terrain is counted and reported, not imported.
  - Speed: a generated place with 18,306 parts imports in 9–14 ms under
    Wine. The OpenGL renderer keeps static parts in vertex buffers (22.8 MB
    for that place), and the software renderer still ran at 87–100 fps.
- **Standing on parts:** the character finds the ground with a ray cast
  against every can-collide part (`src/world/ub_query.c`, a convex ray
  test against each part's low-detail mesh). It can step up 2.2 studs.
  Falling 500 studs below the spawn point puts you back on it. (Milestone
  4 replaced this with the physics engine and character controller.)
- **Bugs found while testing:**
  - Vector3, Color3 and CFrame positions are stored as three separately
    interleaved arrays, not one. Reading them as one gave nonsense
    positions; the XML comparison caught it.
  - A relative path on the command line wasn't found when the game was
    started from another folder. It now also looks next to the `.exe`.
  - The settings file is rewritten on exit, which dropped a `place=` line
    the user had added. It's now written back.

## Milestone 4 notes

- **Bodies** (`src/physics/ub_physics.c`): parts joined by welds (union-find
  over the world's welds) form one rigid body, with mass, centre of mass
  and inertia worked out exactly from each shape's hull (tetrahedra from
  the origin, Blow & Binstock) and the material's density. An assembly with
  an anchored part goes into one static body. Static shapes live in a
  bounding-volume tree built once (median splits); moving shapes are
  sorted along x each step (sweep and prune) and tested against the tree.
- **Collision** (`src/physics/ub_collide.c`): separating-axis test over
  face normals and only those edge pairs that form a face of the Minkowski
  difference (Gregorius, GDC 2013), then the incident face is clipped
  against the reference face for up to four points. Balls are true
  spheres. Contacts up to 0.04 studs apart (plus how far things move in a
  step) are kept as speculative contacts, so fast parts don't tunnel and
  resting contacts don't flicker.
- **Solver: soft step**, after Box2D 3. Collisions are found 120 times a
  second; each step is solved in 6 substeps with soft contacts (a 180 Hz
  spring with damping ratio 3), warm started, with a relax pass after each
  substep and one restitution pass. Friction acts at the centre of each
  contact patch, with a twist limit. Balls get rolling resistance. Impacts
  are first caught by the patch as a whole, so a dropped part lands flat.
- **What it took to get stacks to stand:** plain sequential impulses
  converge too slowly for a tall stack under Roblox's strong gravity (5.6
  g in real units). Each step leaves some sinking speed, the boxes tip, and
  the tower explodes. Tests showed the solver itself was correct: the
  contact coupling is symmetric to 3e-8 and a single solve never adds
  energy. More iterations, warm-start scaling, alternating order, patch
  friction and shock propagation all failed to fix it (shock propagation
  made it worse, since it breaks momentum when the lower box is bouncing).
  The soft step fixed it.
- **Sleeping:** resting groups (islands of touching loose bodies) sleep
  after 0.5 s below 0.35 studs/s and 0.35 rad/s. They wake when something
  awake touches them or when pushed. Two sleepers are never tested against
  each other.
- **Character** (`src/physics/ub_character.c`): a body of two spheres from
  2.2 to 5.4 studs above the feet, with no rotation and no gravity from the
  engine. Five ground rays find the floor, so it steps up to 2.2 studs,
  snaps down small drops and follows platforms. Walking speed is steered
  with limited acceleration, so heavy things push back. The jump speed
  comes from the jump height and gravity, with half a step of gravity
  applied on the jump step so it peaks at exactly 7.2 studs.
- **Speed** (modern PC, -O2): the 18,306-part city builds its physics in
  5.6 ms. 500 loose boxes dropped on it take 0.73 ms per step and sleep
  after 3.2 s. 300 tumbling parts in a pit take about 2.5 ms per step.
  Cylinders cost the most (12-sided hulls; 16 sides was 60% slower).
- **Bugs found on the way:**
  - A `.rbxm` model file has no Workspace, so its parts were never drawn
    (since milestone 3) or simulated. They're now put in one.
  - Clipping repeated points along shared edges, and one old contact could
    warm-start several new ones. Both are fixed, though neither caused the
    stack trouble.

## Milestone 5 notes

- **Lua 5.1.5** (`src/script/lua`, MIT) is built on Uniblox's own small C
  library (`src/script/ub_libc.c`, headers in `src/script/libc`). It has
  x87 maths, an exact `strtod`, `printf`-style formatting that matches
  glibc digit for digit, and `setjmp`. It's tested against glibc. Changes
  to Lua are marked `UNIBLOX` and listed in `src/script/lua/UNIBLOX.md`:
  work limits in patterns and `string.rep`, Luau's compound assignments and
  `continue`.
- **The game layer** (`src/game/ub_game.c`) runs a place: physics, the
  character (with a Model, HumanoidRootPart and Humanoid for scripts),
  health, death and respawn, and the scripts. It doesn't touch the screen
  or the OS, so the tests run whole games. The app draws what it holds.
  Parts are drawn from a static set until something moves or changes
  them, then every frame.
- **Scripts** (`src/script/ub_script.c`): one Lua state. Each script gets
  its own globals: copies of the library tables, `script`, and the shared
  `_G`/`shared`. Everything else is read from a locked base table. Every
  script runs as a coroutine; `wait`, `task.*` and event `Wait` park it in
  a scheduler. Waits are woken in game time, which only moves with physics
  steps. Events fire handlers on new threads. All calls into Lua from the
  game are protected (`lua_cpcall`), so a script error never reaches C.
- **The API** (`ub_sapi_inst.c`, `ub_sapi_types.c`): instances are cached
  userdata, one per instance, so `==` and table keys work. Properties map
  onto the world, converting between Roblox's axes and Uniblox's (z
  flipped, rotations as S·R·S, the same turns for cylinders and wedges as
  the importer). Unknown properties go into a per-instance table. Services
  that aren't world instances (RunService, TweenService, Debris...) are
  virtual instances with negative indices.
- **pcall can wait**, as in Luau. Lua 5.1 can't yield across `pcall`, so
  `pcall` and `xpcall` run the function as a coroutine and pass its yields
  up. A wait inside one wakes the thread that called `pcall` (and
  `coroutine.running` returns that thread), and the resume flows back
  down.
- **Containment:**
  - *Time:* a count hook checks the clock every 10,000 instructions. Past
    250 ms, the hook switches to fire on every instruction, so `pcall`
    can't catch the error and carry on. The script is then stopped (its
    threads and connections dropped), even if the error was caught.
  - *Memory:* the allocator holds all scripts to 64 MB. The script that
    hits the limit is stopped the same way. After any out-of-memory error a
    full collection runs, since Lua 5.1 has no emergency collector.
  - *C-side work:* pattern matching has step and depth limits,
    `string.rep` and string building are capped at 16 MB, and `find` with
    plain text has a work limit.
  - *Sandbox:* no `load`, `loadstring`, `dofile`, `getfenv`/`setfenv`,
    `string.dump`, `debug`, `io` or OS functions beyond `os.time`/`clock`.
    Bytecode is refused. The string metatable and the API's metatables are
    locked.
  - *Instances:* scripts may add up to 100,000 live instances. Destroy
    disconnects an instance's events (as in Roblox), so handlers that refer
    to it don't keep it alive.
- **Slot reuse:** the world never used to delete anything, so a game that
  made and destroyed parts grew for ever. Now an instance's slot (and its
  part's) is reused once it's destroyed, or was never parented, and no
  script holds it. That's found through the instance userdata's `__gc`,
  queued and handled at safe points. Physics reuses emptied one-shape
  bodies and their shape slots after a step has passed, and wakes
  whatever slept with them first. The string pool shares repeated names.
  300,000 parts made and destroyed grow the world by about 600 slots.
- **See-through parts** use a 4x4 ordered-dither screen door: dithered
  spans in the software renderer and `glPolygonStipple` in OpenGL. No
  sorting is needed, and it works in 8-bit colour. It covers imported
  places' windows too.
- **Bugs found on the way:**
  - A running coroutine isn't a GC root in Lua 5.1. The scheduler was
    popping a thread before resuming it.
  - A weak-keyed table mapping pcall coroutines to their callers kept dead
    threads alive. There are no ephemerons in 5.1, so it needs weak values
    too.
  - `TweenInfo.new` read its arguments after pushing its result.
  - Tween goals read a Color3's components with a relative stack index
    after pushing. The Wine smoke test caught this through the demo's lava
    script.

## Milestone 6 notes

- **A separate program:** `ubedit.exe` shares everything with the game
  (platform layer, renderers, world, physics, scripts) except its app
  file. `app_info()` gives each program its own window title and log.
  `app_play.c` is the play view both use: the camera, keys to movement,
  the character's figure, and drawing a running game's parts.
- **UI toolkit** (`src/ui/ub_ui.c`), immediate mode. Every widget call
  draws itself and returns what the user did. Between frames it remembers
  only:
  - which widget is pressed or edited
  - scroll positions
  - open menus and pop-ups

  Input is layered (base, dialog, pop-up): the mouse belongs to the top
  layer open last frame. So clicking outside a menu only closes it, and a
  dialog blocks what is beneath it. Pop-up choices come back to their
  widget the next frame. Widgets record their rectangles by label, so
  tests click things by name.
- **Renderer additions:** 2D rectangles and clipping in both renderers
  (glScissor in OpenGL). Text can be drawn into the software frame from
  the same font atlas OpenGL uses, so panels and menus overlap correctly;
  the OS-drawn text queue always lands on top. There's also a fixed font
  size, text widths from the atlas (the same Tahoma both renderers use),
  and the screen-ray and projection maths shared by picking and the
  handles.
- **Editor model** (`src/editor/ub_editor.c`): OS-independent, so the
  tests make the same calls as the menus.
  - *Undo:* each change saves the place as a `.ubx` snapshot first, with
    the selection remapped to its position in the file
    (`ub_world_save_remap`), so undo restores the selection too. Repeats
    of the same change within a second (dragging a slider) are one step.
    The history holds 64 steps or 48 MB.
  - *Deleting* reclaims the slots at once (the editor owns every index).
  - *The clipboard* is a separate world, so copies survive undo and
    loading another place.
  - *Selection* lookups use a per-instance mark, so Select All on 20,000
    parts isn't quadratic. The app also caches what is inside the
    selection, once per change.
- **Behaviours** are ordinary scripts written for the Roblox API, kept in
  `ub_editor.c`. Two API pieces were added for them:
  `Player.RespawnLocation`, and setting the `HumanoidRootPart`'s velocity
  to launch the character.
- **Roblox axes in the editor:** the conversion (`ub_part_get_rbx` and
  friends, and orientation in degrees) moved from the script module to
  the world, so the Properties panel and scripts agree.
- **Bugs found on the way:**
  - Mesh lookups (`ub_mesh_get`) returned empty meshes until a renderer
    started, so ray tests hit everything at distance 0. Meshes now set
    themselves up on first use.
  - On a slow frame Ctrl can be let go before the frame sees S. Shortcuts
    now count a modifier released during that frame. The Wine OpenGL run
    caught this.
  - `ui_wants_mouse` looked two frames back, which swallowed the first
    click in the 3D view after using a panel.

## Open items

- Milestone 6 on a real install: build a short course in `ubedit.exe`
  (blocks, a kill brick, a checkpoint, a finish line), save it, and play it
  in `uniblox.exe`. Then send `ubedit.log`.
- Editor, not done yet: several items selected show shared properties
  only for colour, material and so on; there's no rotation handle (R and T
  turn by 90 degrees, or type an orientation); no terrain; no reparenting
  by dragging in the Explorer (cut, select a model, paste); and the script
  editor has no text selection or clipboard.

- Milestone 5 on a real install: walk into the demo's door, step on the
  green button, and send the `script:` lines from `uniblox.log`. Then try
  a Roblox place with scripts.
- Not done yet: LocalScripts and GUI (no screen GUI exists yet), animations
  (`LoadAnimation` returns a track that does nothing), `Humanoid:MoveTo`,
  sounds from scripts, constraints, and `CollectionService` tags.

- Milestone 4 on a real install: knock over the demo's tower and crates,
  and check that they settle. The overlay's physics time should stay well
  under 1 ms on the Sandy Bridge machine.
- Physics on a Pentium-class machine is untested. Sleeping keeps quiet
  scenes cheap, but a big pile of tumbling parts will be slow.
- No hinges, motors, springs or ropes yet (Roblox constraints import as
  nothing; motors are held still as welds). No continuous collision, so a
  very fast part can still pass through a thin one.
- Stacks taller than about 20 parts, or of tiny 1-stud cubes with
  alternating materials, can still topple.

- Milestone 3 on a real install: open a place you made in Studio (save it
  as binary `.rbxl`), and send the `imported Roblox place` line from
  `uniblox.log`. Things to look at: rotated parts, wedges, and cylinders.
- Corner wedges are an approximation; their slope direction hasn't been
  checked against an image of the real shape.
- Not imported yet: mesh and union geometry (needs local mesh files),
  terrain, textures and decals. (See-through parts are drawn since
  milestone 5.)
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
