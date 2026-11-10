# Uniblox
A vibe-coded roblox-like sandbox game for older versions of Windows NT as far back as NT 3.51

A from-scratch sandbox building game (multiplayer later) for **Windows NT
3.51, NT 4.0, 2000, XP and later**. It is written in freestanding C99 and
cross-compiled from Linux. See [docs/PLAN.md](docs/PLAN.md) for the full
plan and milestones.

**Requirements:** Windows NT 3.51 or newer, 384 MB RAM minimum (512 MB+
recommended). The CPU only needs to be an x86 Pentium.

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

## Scripting

Places run their own Lua scripts, written the way Roblox scripts are.
Scripts in a place you import from Roblox run too.

```lua
local door = script.Parent
door.Touched:Connect(function(hit)
	if hit.Parent:FindFirstChildOfClass("Humanoid") then
		door.Transparency = 0.7
		door.CanCollide = false
		task.wait(3)
		door.Transparency = 0
		door.CanCollide = true
	end
end)
```

- **Language:** Lua 5.1, plus the Luau additions scripts use most:
  `+= -= *= /= %= ^= ..=`, `continue`, `typeof`, `table.find`/`clear`/`pack`,
  `string.split`, `math.clamp`/`sign`/`round`, and `pcall` that can wait.
- **Scripts that run:** every `Script` in the Workspace or
  ServerScriptService that isn't disabled. ModuleScripts load with
  `require`. LocalScripts don't run yet.
- **The API:** instances with their properties, methods and events
  (`Touched`, `Changed`, `ChildAdded`, `GetPropertyChangedSignal`...),
  `Instance.new`, `Clone`, `Destroy`, attributes, `game:GetService`,
  `Players` and the player's character with its `Humanoid` (health, walk
  speed, jump power, `Died`), `RunService` events, `TweenService`, `Debris`,
  `workspace:Raycast`, `Lighting`. The types are `Vector3`, `CFrame`,
  `Color3`, `BrickColor`, `Enum`, `TweenInfo`, `Ray` and `RaycastParams`.
  Scheduling uses `wait`, `task.wait`/`spawn`/`defer`/`delay`/`cancel`,
  `spawn` and `delay`. Positions use Roblox's axes, so coordinates copied
  from Studio mean the same thing.
- **Things Uniblox doesn't do yet don't crash scripts.** Properties it
  doesn't simulate are kept and read back. Services that need the internet
  (HTTP, marketplace, teleport) raise an error, as they do when they're
  offline in Roblox. DataStores keep their data until you quit.
- **Hostile scripts can't hurt the game.** There are no files, no OS
  access, no `loadstring`, no bytecode and no debug library, and
  metatables are locked. A script that runs for 250 ms without waiting is
  stopped, even if it catches the error with `pcall`. All scripts share
  64 MB of memory, and the one that uses it up is stopped. String patterns
  and `string.rep` have work limits. Scripts may make up to 100,000
  instances; destroyed and forgotten ones are reused, so games that keep
  making and destroying parts don't grow.
- **Your character** is a Model named `Player1` in the Workspace, with a
  `HumanoidRootPart` and a `Humanoid`. When its health reaches 0 it falls
  over and comes back at the spawn point 3 seconds later.

The demo place's door, spinning block, glowing lava strip and green button
(which rains balls) are scripts. Type `fast`, `slow`, `night` or `day` in
the chat line to try one more. Parts with a `Transparency` between 0 and 1
are drawn see-through, with a dither pattern that works in both renderers
and in 256 colours.

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
  and Zstandard decoders, `.ubx` files, the Roblox importer, physics, the
  character, the C library under Lua, and scripts), compiled natively with
  AddressSanitizer and UBSan:

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

  The script tests run whole games without a window. The character walks
  into a scripted door, which opens and lets it through, then closes and
  blocks it again. The API test (`tests/data/scripts/api.lua`) makes over
  100 checks. Then 28 hostile scripts run one after another, including
  busy loops (also inside `pcall`, coroutines and events), memory, table,
  pattern and `string.rep` bombs, sandbox escapes, and destroying the
  services and the character. Each must be stopped or fail within 3
  seconds, and the game, its door and an innocent script must carry on.
  Another test makes and destroys 300,000 instances and checks that the
  world and physics stay small. A last one plays the demo place: the door,
  the button, the lava and the chat commands.

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

  It also checks that the demo's loose parts settle and fall asleep, walks
  into the scripted door and checks the script opened it, checks that no
  script failed, opens a Roblox place from the command line, saves it with
  F7, and opens the result through `place=`.

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
| `uniblox.cfg` | Settings saved on exit, with comments: `renderer` (auto/opengl/software), `quality` (0-2), `gl_shaders`, `fps_cap`, `vsync`, `fullscreen`, window size, `sw_height`, `colors`, camera, mute, `scripts` (1 runs the place's scripts, 0 doesn't) and `place` |
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
| Enter or `/` | Type a message (Enter sends, Esc cancels); scripts hear it as `Player.Chatted` |
| F11 or Alt+Enter | Fullscreen on/off |
| F5 | 10-second benchmark with the frame cap off; the result goes on screen and into `uniblox.log` |
| F6 | OpenGL: graphics quality high, medium (no anti-aliasing), low (no shadows). Software: render resolution 240/360/480/720/full |
| F7 | Save the current place as `places/<name>.ubx` |
| F8 | Software renderer: switch between 32-bit colour and 8-bit palette |
| F9 | Frame rate cap: 60, 120, 144, 240, unlimited |
| F3 | Hide the text overlay |
| M | Mute / unmute |
| Esc | Quit |

The text overlay shows the place's name and the result of the last load
or save. It also shows what physics is doing (moving parts, how many are
awake, contacts, and the time physics took that frame). For scripts, it
shows how many are running and waiting, their memory and time, and the
last lines they printed, with errors in red. Below that is your health.

## License

GPLv3. See [LICENSE](LICENSE).

The Roblox test files in `tests/data/rbx/` come from
[rojo-rbx/rbx-test-files](https://github.com/rojo-rbx/rbx-test-files)
under the MIT License; see the README in that folder. Uniblox isn't
affiliated with Roblox.
