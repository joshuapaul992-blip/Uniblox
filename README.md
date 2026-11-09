# Uniblox
A vibe-coded roblox-like sandbox game for older versions of Windows NT as far back as NT 3.51

A from-scratch sandbox building game (multiplayer later) for **Windows NT
3.51, NT 4.0, 2000, XP and later**. It is written in freestanding C99 and
cross-compiled from Linux. See [docs/PLAN.md](docs/PLAN.md) for the full
plan and milestones.

**Requirements:** 384 MB RAM minimum, 512 MB+ recommended. The CPU only
needs to be an x86 Pentium.

## Two renderers

- **OpenGL** (any graphics card with an OpenGL driver, from the XP era to
  today). Renders at the window's full resolution, including 1080p, 1440p
  and 4K fullscreen. Effects: per-pixel sun and sky lighting, shiny
  highlights, soft sun shadows, 4x anti-aliasing, distance fog, a sky with a
  sun, and sharp filtered textures. Plain OpenGL 1.1 drivers get a simpler
  version without shadows.
- **Software** (no graphics card needed; works on NT 3.51 and 4.0). Renders
  at a lower resolution (480 lines by default) and stretches to the window.
  It supports 256-colour desktops.

`renderer=auto`, the default, uses OpenGL when a hardware driver is present
and software otherwise. The frame rate is capped at 120 fps by default (F9
cycles through 60/120/144/240/unlimited).

## Places

A place is a saved world: a tree of objects (models, folders, parts) plus
lighting. Uniblox has its own format, `.ubx`, and can also open Roblox
Studio places (`.rbxl`) and models (`.rbxm`) saved in the binary format.
Saving the same world twice gives byte-identical files.

Open a place by naming it on the command line, or by dragging it onto
`uniblox.exe` in Explorer (NT 4.0 and later):

```bat
uniblox.exe C:\places\castle.rbxl
```

To open one every time, set `place=` in `uniblox.cfg` to a path inside the
game folder, such as `place=places/castle.ubx`. A file named on the command
line overrides it. With neither, the built-in demo world loads.

Press **F7** to save whatever is loaded as `places/<name>.ubx`. That
converts a Roblox file into a `.ubx`.

**What the Roblox importer reads:**

- Parts, wedges, corner wedges, trusses, spawn locations and seats, with
  position, rotation, size, colour (`Color3uint8` or `BrickColor`),
  material, shape (block, ball, cylinder), transparency, `Anchored` and
  `CanCollide`. Fully transparent parts are kept but not drawn.
- Models and folders, so the tree keeps its structure. A model file's
  contents go into the Workspace, so they show up like a place.
- Joints (`Weld`, `ManualWeld`, `Snap`, `Glue`, `Motor`, `Motor6D`,
  `WeldConstraint`) become welds. Motors are held still for now.
- `Workspace.Gravity` and `FallenPartsDestroyHeight`.
- Lighting: `ClockTime`/`TimeOfDay`, `GeographicLatitude`, `Brightness`,
  `Ambient`, `OutdoorAmbient`, `FogEnd` and `FogColor`. The sun's position
  follows the clock time.
- Uncompressed, LZ4 and Zstandard chunks. Uniblox has its own decoders.

**What it doesn't read yet:**

- Mesh parts and unions become plain grey boxes of the same size, because
  their shapes are stored online, not in the file. Uniblox never downloads
  anything.
- Terrain is counted but skipped.
- Hinges, springs, ropes and other constraints are ignored, so parts that
  rely on them fall loose.
- Textures, decals, scripts and GUIs are ignored.
- Partly see-through parts are drawn solid.
- The old XML format (`.rbxlx`) isn't supported. Studio can save the
  binary format.

The importer is for places you made or have the right to use. Files are
read only from disk.

## Physics

Unanchored parts fall, slide, tumble, stack and come to rest, as in
Roblox:

- **Assemblies:** parts joined by welds move as one rigid body. If any
  part of an assembly is anchored, the whole assembly stays put.
- **Shapes:** blocks, wedges, corner wedges and cylinders collide as their
  real shapes. Balls are true spheres. Mesh and union placeholders collide
  as boxes. Parts with `CanCollide` off pass through everything, but still
  fall if they're loose.
- **Materials** set the weight, friction and bounce. Metal is heavy and
  slippery, wood is light, and grass and stone grip.
- **Gravity** is the place's own (Roblox's default is 196.2 studs/s²).
  Loose parts that fall below the place's `FallenPartsDestroyHeight` (-500
  by default) are removed.
- **Sleeping:** things that have come to rest stop costing CPU time until
  something touches them. A city of 18,000 anchored parts with 500 loose
  boxes dropped on it settles in about 3 seconds. After that, physics
  takes 0.03 ms per step on a modern PC.
- Stacks of about 20 parts stand. Balls roll and slowly come to a stop.

Physics runs at a fixed 120 steps per second, whatever the frame rate, and
moving parts are drawn blended between steps, so motion stays smooth at
60, 144 or 240 fps. Below about 10 fps the game slows down instead of
stalling.

**The character** works like a Roblox humanoid. It walks at 16 studs/s
and jumps 7.2 studs high (Roblox's defaults). It walks up anything up to
2.2 studs high without jumping, so stairs and kerbs just work. Walls and
ceilings stop it. It pushes loose parts, presses on what it stands on, and
rides moving platforms. If you fall off the world, you go back to the
spawn point.

The demo place has a tower of loose blocks, a pyramid of crates, three
balls in the house, a welded cart and a spinning block to try it on.

## Building (Linux)

Install the 32-bit mingw-w64 cross compiler (Debian/Ubuntu):

```bash
sudo apt-get install gcc-mingw-w64-i686 binutils-mingw-w64-i686
```

Build `build/win32/uniblox.exe` and verify that it can load on NT 3.51:

```bash
make check
```

To build with clang + lld instead (it still uses mingw-w64 headers and libgcc):

```bash
make check CC="clang --target=i686-w64-windows-gnu --sysroot=/usr/i686-w64-mingw32 -fuse-ld=lld"
```

## What `make check` verifies

`tools/checkpe.py` fails the build if the executable:

- is not a 32-bit GUI image marked OS/subsystem version 3.10
- has DllCharacteristics bits set, a TLS directory or a symbol table
- imports any DLL or function not on `tools/nt351_imports.txt`, the
  vetted list of APIs that exist on NT 3.51

`make check` also disassembles the `.exe` and fails if it finds any
instruction newer than the original Pentium (P6 `cmov`/`fcomi`/`fcmov`,
MMX or SSE). The toolchain's own support library is built for the Pentium
Pro, so this catches anything it pulls in.

Anything newer has to be loaded at runtime with `GetProcAddress`, with a
fallback.

## Testing

- **Unit tests** for the OS-independent code (strings, UTF-8, paths, maths,
  WAV loading, audio mixer, palette, the renderer, the C runtime, the LZ4
  and Zstandard decoders, `.ubx` files, the Roblox importer, physics and
  the character), compiled natively with AddressSanitizer and UBSan:

  ```bash
  make test
  ```

  The decoders are checked against files made by the real `lz4` and `zstd`
  tools (`tests/data/`, from `tools/gen_compress_vectors.py`). The importer
  is checked against real Roblox files in `tests/data/rbx/`. Each one is
  compared with a `.expected` list made from an XML copy of the same file
  (`tools/gen_rbx_expected.py`), so the binary reader is checked against an
  independent source. `tools/make_test_rbxl.py` builds the castle test
  place, which uses all three chunk encodings. Both the loader and the
  decoders are also fuzzed with random and cut-off data.

  The physics tests drop single parts and towers of 5 to 20 parts and
  check that they come to rest upright, asleep, at the right height. They
  also cover a pyramid, every shape, friction on a ramp, welded
  assemblies, waking up, kinematic pushers, and 300 random parts thrown
  into a pit (nothing escapes or goes wrong numerically). The character
  tests check its walking speed, jump height, walls, steps, high ledges,
  ceilings, pushing a crate, standing on a loose slab and riding a moving
  platform.

- **Wine smoke test.** This drives the real `.exe` with keyboard and mouse
  events under Xvfb, then checks the log and settings file. It needs
  `wine` (32-bit), `xvfb-run`, `xdotool` and `imagemagick`, plus 32-bit
  Mesa (`libgl1-mesa-dri:i386`) for the OpenGL renderer.

  ```bash
  tools/wine_smoke.sh nt351 software
  ```

  ```bash
  tools/wine_smoke.sh winxp opengl
  ```

  It also checks that the demo's loose parts settle and fall asleep, opens
  a Roblox place from the command line, saves it with F7, and opens the
  result through `place=`.

  The first argument is the Windows version Wine pretends to be: `nt351`,
  `nt40`, `win2k` or `winxp`. The second picks the renderer. Wine is
  forgiving, so passing proves the logic works, not that the `.exe` runs on
  real NT.
- **Real test:** copy the `.exe` into NT 3.51 and NT 4.0 guests in 86Box or
  PCem, and Windows 2000 and XP guests in QEMU or VirtualBox. It's a single
  file with no DLLs to install.

## Files next to the .exe

| File | What it is |
|---|---|
| `uniblox.log` | Log of the current run (the previous run's is `uniblox.old.log`). Send this when reporting a problem |
| `uniblox.cfg` | Settings saved on exit, with comments: `renderer` (auto/opengl/software), `quality` (0-2), `gl_shaders`, `fps_cap`, `vsync`, `fullscreen`, window size, `sw_height`, `colors`, camera, mute and `place` |
| `places/` | Places saved with F7 |
| `sounds/jump.wav`, `sounds/land.wav`, `sounds/click.wav` | Optional. Replace the built-in sounds; any PCM 8/16-bit mono/stereo WAV works |

If the folder is read-only (e.g. a CD-ROM), the game still runs; it just
can't write the log or settings.

## Controls

| Input | Action |
|---|---|
| W A S D, or Up/Down arrows | Move (relative to the camera) |
| Left/Right arrows | Turn the camera |
| Hold right mouse button | Orbit the camera (left/right and up/down) |
| Mouse wheel | Zoom (NT 4.0 and later, or NT 3.51 with a wheel driver) |
| Space | Jump (hold to keep jumping) |
| Left click | Beep, panned left/right and pitched by where you click |
| Enter or `/` | Type a message (Enter sends, Esc cancels) |
| F11 or Alt+Enter | Fullscreen on/off |
| F5 | 10-second benchmark with the frame cap off; the result goes on screen and into `uniblox.log` |
| F6 | OpenGL: graphics quality high, medium (no anti-aliasing), low (no shadows). Software: render resolution 240/360/480/720/full |
| F7 | Save the current place as `places/<name>.ubx` |
| F8 | Software renderer: switch between 32-bit colour and 8-bit palette |
| F9 | Frame rate cap: 60, 120, 144, 240, unlimited |
| F3 | Hide the text overlay |
| M | Mute / unmute |
| Esc | Quit |

The text overlay shows the place's name, the result of the last load or
save, and what physics is doing: loose parts, how many are awake,
contacts, and the time physics took that frame.

## License

GPLv3. See [LICENSE](LICENSE).

The Roblox test files in `tests/data/rbx/` come from
[rojo-rbx/rbx-test-files](https://github.com/rojo-rbx/rbx-test-files)
under the MIT License; see the README in that folder. Uniblox isn't
affiliated with Roblox.
