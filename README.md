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
  WAV loading, audio mixer, palette, the renderer and the C runtime),
  compiled natively with AddressSanitizer and UBSan:

  ```bash
  make test
  ```

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
| `uniblox.cfg` | Settings saved on exit, with comments: `renderer` (auto/opengl/software), `quality` (0-2), `gl_shaders`, `fps_cap`, `vsync`, `fullscreen`, window size, `sw_height`, `colors`, camera and mute |
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
| Space | Jump |
| Left click | Beep, panned left/right and pitched by where you click |
| Enter or `/` | Type a message (Enter sends, Esc cancels) |
| F11 or Alt+Enter | Fullscreen on/off |
| F5 | 10-second benchmark with the frame cap off; the result goes on screen and into `uniblox.log` |
| F6 | OpenGL: graphics quality high, medium (no anti-aliasing), low (no shadows). Software: render resolution 240/360/480/720/full |
| F8 | Software renderer: switch between 32-bit colour and 8-bit palette |
| F9 | Frame rate cap: 60, 120, 144, 240, unlimited |
| F3 | Hide the text overlay |
| M | Mute / unmute |
| Esc | Quit |

There's no collision yet (that's milestone 4), so you can walk through walls.

## License

GPLv3. See [LICENSE](LICENSE).
