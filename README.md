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

**The settings menu** (Esc) has Resume, Settings, Load the place again and
Quit. Settings has three tabs:
- **Display:** renderer, fullscreen, fullscreen size, graphics quality or
  render resolution, colours, sending only what changed, frame rate cap,
  text overlay.
- **Sound and controls:** volume, mute, mouse-look speed, invert mouse,
  field of view.
- **Game:** scripts, timer.

Changes are saved at once. The renderer, timer and scripts take effect the
next time Uniblox starts or loads a place; everything else changes
immediately. Every setting is a `< value >` stepper rather than a pop-up
list, because on the software renderer the OS draws text on top of
everything.

When the renderer casts no shadows (software, or OpenGL on low), the
character gets a soft round shadow on the ground. It shrinks and fades as
you jump.

## NT 3.51 and 4.0 on a modern PC

Uniblox runs on NT 3.51 and 4.0 installed on today's hardware, for
example an i7-8700 with the [WINVBE](https://github.com/ages2001/WINVBE)
VESA driver. There, the slow part is putting each frame on the screen, not
drawing it. On NT 3.51 with WINVBE, the graphics card's memory is written
uncached. At 2560×1421 a whole frame took about 150 ms to send, against
3 ms to draw. What Uniblox does about it:

- **It sends only what changed.** Each frame is compared with the last one
  in normal memory (cheap), and only the changed tiles are written to the
  screen. The text overlay is redrawn only where it changes. Standing
  still, or working in the editor, sends almost nothing. When the camera
  moves, everything changes and a whole frame is sent. The overlay's
  `Present:` line shows how much changed. Turn it off with Settings >
  Display > *Send only what changed* (`send_changes_only=0`).
- **It can lower the screen's resolution while fullscreen** (NT 4.0 and
  later: Settings > Display > *Fullscreen size*). At 1280×720 a frame is a
  quarter of the bytes of 1440p, and the monitor does the stretching. Use
  Alt+Tab and the desktop gets its own mode back; it's also restored on
  exit. NT 3.51 cannot change display modes, so there a smaller window or
  a 16-bit desktop is the way to send fewer bytes.
- **Steady timing.** On CPUs with a steady (invariant) time-stamp counter
  (Nehalem, Haswell and later), frames are timed with it. That's cheaper
  and finer than NT's timer chip, which NT 3.51 and 4.0 read through slow
  I/O ports. `timer=qpc` goes back to Windows' timer.
- **What the CPU can and cannot do:** NT 3.51 and 4.0 never turn on SSE,
  because they don't save its registers. Uniblox uses only instructions
  the original Pentium has, on every Windows, and `make check` enforces
  this. The CPU line in the log names the chip and what it offers.
- **Fonts:** Tahoma isn't on NT 3.51 or a plain NT 4.0, so Uniblox checks
  which font GDI actually picked and uses Arial when there's no Tahoma.
- **Icons and version information** in both programs (Program Manager on
  NT 3.51, Explorer's Properties on NT 4.0).

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
- GUIs, with every property: ScreenGuis, Frames, labels, buttons, text
  boxes, images, ScrollingFrames, layouts and constraints, UICorner,
  UIStroke, UIGradient, BillboardGuis and SurfaceGuis (see "GUIs" below).
- Scripts with their code, LocalScripts and ModuleScripts; value objects
  (`IntValue`, `StringValue`, `ObjectValue`...); RemoteEvents,
  RemoteFunctions and bindables. Files from 2024 on, which store image
  addresses in the newer Content type, work too.

**What it doesn't read yet:**

- Mesh parts and unions become plain grey boxes of the same size, because
  their shapes are stored online, not in the file. Uniblox never downloads
  anything.
- Terrain is counted but skipped.
- Hinges, springs, ropes and other constraints are ignored, so parts that
  rely on them fall loose.
- Textures and decals on parts are ignored.
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

- **Language:** Luau as Roblox had it in 2022, on a Lua 5.1 core: type
  annotations (`local x: number`, typed parameters and returns, generics,
  `type` and `export type`, `::` casts) are read and ignored as Roblox
  does; if-then-else expressions; `for k, v in t do` without `pairs`;
  `+= -= *= /= %= ^= ..=` and `continue`; `0b1010`, `1_000_000`, and the
  `\x`, `\z` and `\u{...}` escapes.
- **Luau's library:** `bit32`, `utf8`, `table.move`/`freeze`/`clone`/`find`/
  `create`, `string.split`/`pack`/`unpack`, `math.noise` (Perlin),
  `math.clamp`/`sign`/`round`, `os.date`/`time`, `debug.traceback`/`info`,
  `coroutine.close`/`isyieldable`, `Random`, `DateTime`, `newproxy`, and a
  `pcall` that can wait. `HttpService` encodes and decodes JSON.
- **Scripts that run:** every `Script` in the Workspace,
  ServerScriptService or under Players that isn't disabled; every
  `LocalScript` in the player's PlayerGui, Backpack, PlayerScripts or
  character, or in ReplicatedFirst. Uniblox is one machine, so both sides
  run here, and a RemoteEvent fired from a LocalScript reaches the server
  script straight away. ModuleScripts load with `require`.
- **The API:** instances with their properties, methods and events
  (`Touched`, `Changed`, `ChildAdded`, `GetPropertyChangedSignal`...),
  `Instance.new`, `Clone`, `Destroy`, attributes and tags (read from place
  files and saved; `CollectionService` with its added and removed
  signals), `game:GetService`,
  `Players` and the player's character with its `Humanoid` (health, walk
  speed, jump power, `Died`, `StateChanged`, `Running`, `Jumping`,
  `FreeFalling`), `RunService` events, `TweenService`, `Debris`,
  `workspace:Raycast`, `GetPartBoundsInBox`/`InRadius`, `GetPartsInPart`,
  `FindPartsInRegion3`, `Lighting`, `ClickDetector`s (click and hover),
  `ProximityPrompt`s (shown near your character, triggered by their key or
  by holding it; drawn in Uniblox's own style), the GUI (below), `UserInputService`,
  `Player:GetMouse()`, `workspace.CurrentCamera` (its CFrame, ViewportSize,
  FieldOfView, WorldToViewportPoint and rays), `GuiService`,
  `TextService:GetTextSize`, remotes and bindables. The types are
  `Vector3`, `Vector2`, `CFrame`, `Color3`, `BrickColor`, `UDim`, `UDim2`,
  `Rect`, `NumberRange`, `NumberSequence`, `ColorSequence`, `Font`,
  `InputObject`, `TweenInfo`, `Ray`, `RaycastParams`, `OverlapParams`,
  `Region3`, `Vector3int16`, `PhysicalProperties`, `Faces`, `Axes`,
  `Random` and `DateTime`, and Roblox's enums (`Enum.KeyCode`,
  `Enum.Font`...). 118 classes (Sound, lights, effects, Tool, Team,
  Attachment, Highlight...) have Roblox's properties and defaults.
  Scheduling uses `wait`, `task.wait`/`spawn`/`defer`/`delay`/`cancel`,
  `spawn` and `delay`. Positions use Roblox's axes, so coordinates copied
  from Studio mean the same thing.
- **Things Uniblox doesn't do yet don't crash scripts.** Properties it
  doesn't simulate are kept and read back. Services that need the internet
  (HTTP, marketplace, teleport) raise an error, as they do when they're
  offline in Roblox. DataStores keep their data until you quit. Policy,
  badge, group and localization questions get offline answers (no badges,
  no groups, English), and PathfindingService gives straight paths.
- **Hostile scripts can't hurt the game.** There are no files, no OS
  access, no `loadstring`, no bytecode and only the harmless part of the
  debug library, and
  metatables are locked. A script that runs for 250 ms without waiting is
  stopped, even if it catches the error with `pcall`. All scripts share
  64 MB of memory, and the one that uses it up is stopped. String patterns
  and `string.rep` have work limits. Scripts may make up to 100,000
  instances; destroyed and forgotten ones are reused, so games that keep
  making and destroying parts don't grow.
- **Your character** is a Model named `Player1` in the Workspace, with a
  `HumanoidRootPart` and a `Humanoid`. When its health reaches 0 it falls
  over and comes back at the spawn point 3 seconds later.

The demo place's door, spinning block (click it to turn it the other way),
glowing lava strip, green button (which rains balls) and lamp (press E
beside it) are scripts. Type `fast`, `slow`, `night` or `day` in
the chat line to try one more. Parts with a `Transparency` between 0 and 1
are drawn see-through, with a dither pattern that works in both renderers
and in 256 colours.

## GUIs

Places can have Roblox's GUIs, made in Studio or by scripts. What is in
StarterGui is copied into the player's PlayerGui when they join (and again
after a respawn, unless `ResetOnSpawn` is off), and its LocalScripts run.

- **Objects:** ScreenGui, Frame, TextLabel, TextButton, TextBox,
  ImageLabel, ImageButton, ScrollingFrame, BillboardGui (over a part) and
  SurfaceGui (on a part's face). Every property is kept, read and saved;
  setting one checks the type as Roblox does.
- **Layout, worked out as Roblox does:** Size and Position (scale and
  offset), AnchorPoint, SizeConstraint, Rotation, ZIndex in both
  ZIndexBehavior modes, Visible, ClipsDescendants, AutomaticSize, the
  36-pixel bar at the top that ScreenGuis start below (IgnoreGuiInset),
  UIListLayout, UIGridLayout, UIPageLayout, UIPadding, UIScale,
  UIAspectRatioConstraint, UISizeConstraint and UITextSizeConstraint.
  Scripts read the results: AbsolutePosition, AbsoluteSize,
  AbsoluteRotation, TextBounds, AbsoluteContentSize, AbsoluteCanvasSize,
  and their changed signals fire.
- **Drawing:** rounded corners (UICorner), outlines (UIStroke), gradients
  (UIGradient), borders, see-through backgrounds, images (stretched,
  9-slice, tiled, fitted, cropped, ImageRectOffset/Size), scrollbars.
  The same drawing code serves both renderers, so a GUI looks the same in
  OpenGL, in software and in 256 colours. BillboardGuis and SurfaceGuis
  are hidden by parts in front of them unless AlwaysOnTop.
- **Text:** TextSize, TextScaled, TextWrapped, alignment, LineHeight,
  TextTruncate, MaxVisibleGraphemes, the text stroke, and rich text
  (`<b>`, `<i>`, `<u>`, `<s>`, `<font color size face weight
  transparency>`, `<stroke>`, `<br/>`, `<uppercase>`, `<smallcaps>`,
  comments and `&lt;`-style escapes). Uniblox ships no fonts: Roblox's
  fonts are drawn with the nearest one Windows has (Arial for the sans
  fonts, Times New Roman, Courier New, Comic Sans MS where installed, Arial
  Black for the heaviest). Text is anti-aliased by Uniblox itself, the
  same way on NT 3.51 as on Windows 11.
- **Input:** hovering, clicking (MouseEnter/Leave, MouseButton1Click,
  Activated, InputBegan...), AutoButtonColor, text boxes (click to type,
  Enter, Esc, arrows, Home, End, Delete; FocusLost), the mouse wheel and
  dragging scrollbars. GUI that takes the mouse keeps it from the world, a
  text box keeps the keyboard from the character, and scripts are told
  (`gameProcessedEvent`).
- **Images** are never downloaded. Put your own pictures in a `content`
  folder next to `uniblox.exe`: `rbxassetid://123` (or a roblox.com asset
  link) is read from `content\123.png` or `content\123.bmp`, and
  `rbxasset://textures/x.png` from `content\textures\x.png`. PNG (every
  kind) and BMP work. A missing picture draws nothing, like one still
  loading in Roblox, and is noted once in the log.
- **Not yet:** 3D inside a ViewportFrame, videos, CanvasGroup's group
  transparency, UITableLayout (laid out as a list), gamepad selection,
  rich text's `<mark>`, and clicking buttons on BillboardGuis and
  SurfaceGuis.

The demo place has a panel in the top right (a count of door openings and
a "Back to the start" button that asks a server script, through a
RemoteEvent, to move you), messages that slide up from the bottom, a
SurfaceGui sign by the spawn and a BillboardGui over the door.

## The editor

`ubedit.exe` is a separate program for building places. It runs on the
same machines as the game: NT 3.51 and later, either renderer, and 256
colours. It saves to `places/<name>.ubx`, which `uniblox.exe` plays (open
one from the command line, or with `place=` in `uniblox.cfg`).

- **Explorer** (left): every object in the place as a tree. Click to
  select (Ctrl or Shift adds); double-click a script to edit it.
- **Properties** (right): the selection's name, position, size and
  orientation, in Roblox's axes, so numbers match scripts and Studio. Also
  colour, material, shape, transparency, Anchored, CanCollide and Neon.
  With nothing selected, the place itself: name, time of day, gravity,
  fall limit and the grid.
- **The 3D view:** hold the right mouse button to look around, use WASD to
  fly (Q/E down/up, Shift faster), the wheel to move, and F to look at the
  selection. Click to select; Alt-click picks a part inside a model. Drag
  a selected part to slide it along the ground, snapped to the grid.
- **Tools:** 1 Select, 2 Move (drag the coloured arrows along an axis),
  3 Scale (pull a face out or in), 4 Rotate. R turns the selection 90
  degrees and T tips it over.
- **Insert:** blocks, wedges, corner wedges, cylinders, balls and spawn
  points from the toolbar or the Insert menu. Models, folders and scripts
  come from the Insert menu.
- **Behaviours** make parts do things without writing code. Select parts,
  then pick one from the Behaviours menu or the Properties panel's
  *Add behaviour* list:
  - *Kill brick* knocks the player out.
  - *Checkpoint*: the player comes back here after falling.
  - *Finish line* says how long the course took.
  - *Spinner* turns round and round.
  - *Mover* slides back and forth, carrying players.
  - *Disappearing* fades when stepped on, then comes back.
  - *Bounce pad* launches the player upwards.
  - *Speed pad* runs the player faster for a few seconds.
  - *Door* opens when touched.

  Each behaviour is a short, readable Roblox-style script inside the part,
  so you can open it and change it (its speed, distance or power).
- **Play test:** F5, or the Play button, runs the place right there with
  your character and its scripts. F5 or Esc goes back to editing, with the
  place as it was before you played.
- **Editing:** undo and redo with Ctrl+Z/Ctrl+Y (64 steps), copy, cut and
  paste with Ctrl+C/X/V (copies land beside the originals), Ctrl+D
  duplicates, Delete deletes, Ctrl+G/Ctrl+U group into a model and
  ungroup, Ctrl+A selects all.
- **Files:** File > Open lists the places in `places/` and any `.rbxl`,
  `.rbxm` or `.ubx` next to the program (Roblox places are imported). New
  and Open ask before throwing away unsaved changes. If you close the
  window with changes unsaved, they're kept in `places/autosave.ubx`.

The script editor numbers its lines, keeps the indentation when you press
Enter, and has Tab, Home/End, PgUp/PgDn and Ctrl+Home/End. Big imported
places stay usable: with 20,000 parts, selecting everything takes about
70 ms, and a frame with all of them selected about 15 ms.

## Building (Linux)

Install the 32-bit mingw-w64 cross compiler (Debian/Ubuntu):

```bash
sudo apt-get install gcc-mingw-w64-i686 binutils-mingw-w64-i686
```

Build `build/win32/uniblox.exe` (the game) and `build/win32/ubedit.exe` (the
editor), and verify that both can load on NT 3.51:

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
  character, the C library under Lua, scripts, the UI toolkit and the
  editor), compiled natively with AddressSanitizer and UBSan:

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
  decoders are also fuzzed with random and cut-off data. Fifteen GUI and
  value-object models from the same project check the GUI import: every
  property in each model's XML copy (`<name>.props.expected`, from
  `tools/gen_rbx_props_expected.py`) must come out of the binary file with
  the same value, and survive a `.ubx` round trip. The deflate decoder
  (for PNG) is checked against Python's zlib with stored, fixed, dynamic,
  Huffman-only and RLE blocks.

  The GUI tests draw with the rasteriser and check pixels (corners,
  outlines, clipping, gradients, rotation), decode PNGs of every filter,
  plain and interlaced, and fuzz the decoder. They lay out text with known
  glyph sizes (wrapping, rich text, TextScaled, truncation). They check
  Roblox's layout rules number by number: anchors, the top inset, lists,
  grids, padding, constraints, UIScale, AutomaticSize, scrolling and both
  ZIndex modes. Input is checked too: hovering, clicks through
  non-Active frames, an Active frame taking them, and typing in a text
  box. The GUI script test (`tests/data/scripts/gui.lua`) is a LocalScript
  that has to be copied from StarterGui to run. It checks the new types
  and enums, defaults, type errors, AbsoluteSize and its signal,
  TextBounds, fonts, layouts, tweens, value objects, a RemoteEvent and
  RemoteFunction round trip with a server Script, bindables, text-box
  focus, and a real click and key press sent through the GUI.

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
  100 checks. `luau.lua` checks every piece of Luau's 2022 syntax (and
  that broken forms are still refused); `luaulib.lua` the library (bit32,
  utf8, frozen tables, Perlin noise, dates against a fixed clock, Random,
  DateTime, coroutine.close, string.pack, JSON), attributes and tags;
  `api2022.lua` the classes, new types and overlap queries, while the test
  clicks a ClickDetector, taps one ProximityPrompt's key, holds another's
  and jumps (StateChanged, Jumping, FreeFalling). Attributes and tags are
  also read from rbx-test-files models and must survive `.ubx`. Then 28 hostile scripts run one after another, including
  busy loops (also inside `pcall`, coroutines and events), memory, table,
  pattern and `string.rep` bombs, sandbox escapes, and destroying the
  services and the character. Each must be stopped or fail within 3
  seconds, and the game, its door and an innocent script must carry on.
  Another test makes and destroys 300,000 instances and checks that the
  world and physics stay small. A last one plays the demo place: the door,
  the button, the lava, the lamp's prompt, the chat commands, and its GUI. The panel must
  count the door, survive a death, and be drawn, along with the sign and
  the door's label. The door's label is also drawn by the software
  renderer over the real scene, to check it sits in front of the wall.

  The editor tests build an obstacle course with editor actions only (no
  code written): a lava floor, platforms over gaps, a checkpoint and a
  finish line. The place is saved, loaded and played by the character,
  which jumps the gaps, dies in the lava on purpose, comes back at the
  checkpoint and reaches the finish. Every behaviour is also checked
  working in a game. Another test drives the real editor app through its
  own UI with made-up mouse and keyboard input on the software renderer:
  - inserting from the toolbar and menus
  - adding behaviours
  - typing numbers into the properties
  - dragging in the 3D view and with the Move and Scale arrows
  - undo
  - the script editor
  - the play test
  - Save as, New and Open, and the unsaved-changes question
  - a 20,000-part place

  The UI toolkit is tested the same way, and so is the game's settings
  menu: its file reads back what it wrote, and clicking through its steppers
  changes the settings. Sending only what changed is checked against a full
  redraw on random frames and odd window sizes: the picture must be
  identical, pixel for pixel.

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

  It photographs the demo's GUI at the start, clicks the panel's button
  and checks that a server script got the RemoteEvent and that the panel's
  LocalScript ran. It also opens the menu with Esc, changes the frame rate cap there and
  checks that it took effect and was saved, and quits from the menu. It
  checks that the demo's loose parts settle and fall asleep, walks
  into the scripted door and checks the script opened it, checks that no
  script failed, opens a Roblox place from the command line, saves it with
  F7, and opens the result through `place=`.

  The editor has its own. It clicks through building a place (insert, add
  a behaviour, open its script, Save as), play-tests it, then checks that
  `uniblox.exe` opens the saved place and runs its script:

  ```bash
  tools/wine_smoke_editor.sh nt351 software
  ```

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
| `uniblox.cfg` | Settings, saved by the menu and on exit, with comments: `renderer` (auto/opengl/software), `quality` (0-2), `gl_shaders`, `fps_cap`, `vsync`, `fullscreen`, window size, `fullscreen_w`/`fullscreen_h` (the screen's mode while fullscreen, NT 4.0+), `sw_height`, `colors`, `send_changes_only`, `timer` (auto/qpc), `overlay`, `volume`, `muted`, `mouse`, `invert_y`, `fov`, camera, `scripts` and `place` |
| `places/` | Places saved with F7 in the game or from the editor (`autosave.ubx`: the editor's unsaved changes when it was closed) |
| `content/` | Optional. Your own pictures for places' GUIs: `content\<asset id>.png` (or `.bmp`) for `rbxassetid://<asset id>`, and paths under it for `rbxasset://` addresses |
| `ubedit.log`, `ubedit.cfg` | The editor's log and settings (`renderer`, `quality`, `gl_shaders`, `fps_cap`, window size, `sw_height`, grid size) |
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
| Esc | The menu: resume, settings, load the place again, quit (Q) |

The text overlay shows the place's name and the result of the last load
or save. It also shows what physics is doing (moving parts, how many are
awake, contacts, and the time physics took that frame). For scripts, it
shows how many are running and waiting, their memory and time, and the
last lines they printed, with errors in red. Below that is your health.

## License

GPLv3. See [LICENSE](LICENSE).

The Roblox test files in `tests/data/rbx/` come from
[rojo-rbx/rbx-test-files](https://github.com/rojo-rbx/rbx-test-files)
under the MIT License; see the README in that folder.
`src/world/ub_classdb.inc` (the GUI classes' properties, their defaults
and Roblox's enums) is generated by `tools/gen_classdb.py` from the
reflection database of [rojo-rbx/rbx-dom](https://github.com/rojo-rbx/rbx-dom)
(MIT License), whose description of the binary format the importer also
follows. Uniblox isn't affiliated with Roblox.
