#!/usr/bin/env bash
# Uniblox - automated smoke test under Wine + Xvfb.
#
#   tools/wine_smoke.sh [wine-windows-version] [renderer] [screenshot.png]
#   e.g. tools/wine_smoke.sh nt351 software
#        tools/wine_smoke.sh winxp opengl
#
# renderer is "software" (default) or "opengl"; OpenGL needs a GLX-capable
# X server (Xvfb with Mesa works).
#
# Runs build/win32/uniblox.exe three times with Wine pretending to be the given
# Windows version, drives it with real X keyboard/mouse events (xdotool),
# then checks uniblox.log and uniblox.cfg for the expected results.
#
# Wine is far more forgiving than real NT 3.51/4.0: passing this proves the
# logic works, not that the .exe runs on real Windows. Always test there too.
#
# Needs: wine (32-bit), xvfb-run, xdotool, imagemagick (import).
set -euo pipefail

VER=${1:-nt351}
RENDERER=${2:-software}
SHOT=${3:-build/smoke-$VER-$RENDERER.png}
DIR=build/win32
EXE=$DIR/uniblox.exe
LOG=$DIR/uniblox.log
CFG=$DIR/uniblox.cfg

for tool in wine xvfb-run xdotool import; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }
done
[ -f "$EXE" ] || { echo "build $EXE first (make)" >&2; exit 2; }

export WINEPREFIX=${WINEPREFIX:-$PWD/build/wineprefix}
export WINEDEBUG=-all
RUN1=$DIR/smoke-run1.log
RUN2=$DIR/smoke-run2.log
rm -f "$LOG" "$DIR/uniblox.old.log" "$CFG" "$DIR/places/castle-generated.ubx" "$RUN1" "$RUN2"
printf 'renderer=%s\r\n' "$RENDERER" > "$CFG"
mkdir -p "$(dirname "$SHOT")"

# Everything that needs the X server runs inside one xvfb-run session.
xvfb-run -a -s "-screen 0 1280x720x24" bash -s "$VER" "$EXE" "$SHOT" "$RENDERER" <<'EOF'
set -euo pipefail
VER=$1 EXE=$2 SHOT=$3 RENDERER=$4
wine reg add 'HKCU\Software\Wine' /v Version /d "$VER" /f >/dev/null 2>&1

start_game() {
    wine "$EXE" "$@" & GAME_PID=$!
    WIN=$(timeout 60 xdotool search --sync --name '^Uniblox' | head -1)
    xdotool windowfocus --sync "$WIN" 2>/dev/null || true
    # the window exists before the renderer is ready (OpenGL setup can take
    # seconds under load); input sent earlier would pile up into one frame
    for _ in $(seq 120); do grep -q "demo ready" build/win32/uniblox.log 2>/dev/null && break; sleep 0.5; done
    sleep 1
}

stop_game() {
    xdotool key Escape; sleep 0.5                       # the game menu
    xdotool key l; sleep 0.3                            # Leave...
    xdotool key Return                                  # ...yes
    for _ in $(seq 50); do kill -0 "$GAME_PID" 2>/dev/null || break; sleep 0.2; done
    if kill -0 "$GAME_PID" 2>/dev/null; then
        echo "game did not exit after Escape" >&2
        kill "$GAME_PID"
        exit 1
    fi
}

# Run 1: exercise input, then quit (which saves uniblox.cfg).
start_game
eval "$(xdotool getwindowgeometry --shell "$WIN")"
sleep 0.5
import -window root "${SHOT%.png}-gui.png"              # the GUI: the panel, the sign, the door's label
# the panel's "Back to the start" button: top right under the player list, below the title and the count
xdotool mousemove $((X + WIDTH - 12 - 105)) $((Y + 187)); sleep 0.3   # (inside it in both renderers: software stretches 480 lines to 540)
xdotool click 1; sleep 0.5
xdotool mousemove $((X + WIDTH / 2)) $((Y + HEIGHT * 3 / 4)); sleep 0.2
xdotool keydown w; sleep 1.3                            # walk into the scripted door...
import -window root "${SHOT%.png}-door.png"              # (open: see-through)
sleep 0.3; xdotool keyup w                              # ...and through it
xdotool keydown space; sleep 0.1; xdotool keyup space   # jump
sleep 0.8
xdotool key --delay 0 Return h i Return                 # chat text, all in one burst
xdotool key slash; xdotool type 'x'; xdotool key Escape  # open + cancel: must not quit
sleep 0.5
import -window root "${SHOT%.png}-chat.png"              # the chat line and the bubble over the head
xdotool key Tab; sleep 0.3; xdotool key Tab             # the player list: hide, show
eval "$(xdotool getwindowgeometry --shell "$WIN")"
# Wine can drop a button event that arrives in the same instant as the
# pointer move onto its window, so pause after every move.
xdotool mousemove $((X + WIDTH / 4)) $((Y + HEIGHT * 3 / 4)); sleep 0.3
xdotool click 1                                         # click (left side, below the chat)
sleep 0.2
xdotool click 4 click 4 click 4                         # wheel up x3 = zoom in
sleep 0.2
xdotool mousemove $((X + WIDTH / 2)) $((Y + HEIGHT / 2)); sleep 0.3
xdotool mousedown 3                                     # mouse-look
sleep 0.3
for _ in 1 2 3 4 5; do xdotool mousemove_relative -- 20 0; sleep 0.1; done
xdotool mouseup 3
sleep 0.5
import -window root "$SHOT"
xdotool key shift; sleep 0.4                            # shift lock on: the mouse turns the camera, no button
for _ in 1 2 3; do xdotool mousemove_relative -- 15 0; sleep 0.1; done
sleep 0.3
import -window root "${SHOT%.png}-shiftlock.png"
xdotool key shift; sleep 0.3                            # and off
for _ in 1 2 3 4 5 6; do xdotool click 4; sleep 0.05; done   # zoom all the way in: first person
sleep 0.6
import -window root "${SHOT%.png}-firstperson.png"
for _ in 1 2 3 4 5; do xdotool click 5; sleep 0.05; done     # out again: 2.5, then 4 notches to 6.1 studs
sleep 0.4
if [ "$RENDERER" != opengl ]; then
    xdotool key F8; sleep 1                             # 8-bit palette rendering
    import -window root "${SHOT%.png}-8bit.png"
    xdotool key F8; sleep 0.3                           # back to 32-bit
fi
xdotool key F6; sleep 0.5                               # quality (GL) / resolution (software)
xdotool key F9; sleep 0.3                               # fps cap 120 -> 144
xdotool key Escape; sleep 0.6                           # the game menu
import -window root "${SHOT%.png}-menu.png"
xdotool key Tab; sleep 0.3; xdotool key Tab; sleep 0.4  # its Help tab
import -window root "${SHOT%.png}-help.png"
xdotool key r; sleep 0.3; xdotool key Escape; sleep 0.3 # reset? no
xdotool key s; sleep 0.5                                # Settings
import -window root "${SHOT%.png}-settings.png"
# Frame rate cap > (240): the 7th row with software, the 5th with OpenGL (no resolution, colours, sending rows)
FPS_ROW_Y=$([ "$RENDERER" = opengl ] && echo 275 || echo 321)
xdotool mousemove $((X + 742)) $((Y + FPS_ROW_Y)); sleep 0.3; xdotool click 1; sleep 0.5
xdotool key Escape; sleep 0.3; xdotool key Escape; sleep 0.5                         # back to the game menu, resume
xdotool key F11; sleep 1.5                              # fullscreen
import -window root "${SHOT%.png}-fullscreen.png"
xdotool key F11; sleep 1                                # back to a window
xdotool key F5; sleep 11.5                              # benchmark (10 s)
stop_game

# Run 2: settings must load back; also import a Roblox place from the
# command line and save it as .ubx with F7.
start_game 'tests\\data\\rbx\\castle-generated.rbxl'
xdotool key F7; sleep 1
stop_game
cp build/win32/uniblox.old.log build/win32/smoke-run1.log
cp build/win32/uniblox.log build/win32/smoke-run2.log

# Run 3: open the saved .ubx through place= in the settings file.
sed -i 's|^place=.*|place=places/castle-generated.ubx\r|' build/win32/uniblox.cfg
start_game
stop_game
wineserver -k 2>/dev/null || true
EOF

fail=0
expect() { # expect <file> <pattern> <description>
    if grep -q -- "$2" "$1"; then echo "  ok    $3"; else echo "  FAIL  $3 (no \"$2\" in $1)"; fail=1; fi
}
echo "Smoke test ($VER, $RENDERER renderer):"
expect "$RUN1" "Windows" "OS detected"
expect "$RUN1" "audio: " "audio initialised (device or graceful fallback)"
expect "$RUN1" "demo ready" "app started"
expect "$RUN1" "physics: [0-9]* loose part group" "physics started"
expect "$RUN1" "physics: all loose parts at rest after" "the demo's loose parts settled and slept"
expect "$RUN2" "loaded uniblox.cfg" "settings loaded on second run"
expect "$RUN2" "shutdown complete" "clean shutdown"
expect "$RUN2" "imported Roblox place" "Roblox place imported from the command line"
expect "$RUN2" "18 parts, .* 1 mesh/union placeholders" "all castle parts imported"
expect "$RUN2" "saved places/castle-generated.ubx" "F7 saved the place as .ubx"
expect "$LOG" "loaded place \"places/castle-generated.ubx\"" "place= in the settings opened the saved .ubx"
expect "$LOG" "shutdown complete" "third run shut down cleanly"
expect "$CFG" "place=places/castle-generated.ubx" "place= kept when settings are rewritten"
expect "$RUN1" "script(s) running" "the demo's scripts started"
expect "$RUN1" "script: Welcome, Player1" "a script greeted the player (PlayerAdded)"
expect "$RUN1" "script: Player1 opened the door" "walking into the door opened it (Touched)"
expect "$RUN1" "script: The demo panel is ready" "the demo's GUI started (a LocalScript from StarterGui)"
expect "$RUN1" "script: Player1 went back to the start" "clicking a GUI button reached a server script (RemoteEvent)"
if grep -E "script: .*(attempt to|is not a valid|was stopped|error)" "$RUN1" "$RUN2" "$LOG"; then
    echo "  FAIL  a script failed"; fail=1
else echo "  ok    no script errors"; fi
expect "$RUN1" "jump 1" "space bar jumped"
expect "$RUN1" "typed: hi" "typed text received"
expect "$RUN1" "click 1 at" "left click received"
expect "$RUN1" "mouse look on" "right button started mouse-look"
expect "$RUN1" "saved uniblox.cfg" "settings saved"
expect "$RUN1" "benchmark: [0-9]* frames" "F5 benchmark completed"
expect "$RUN1" "frame rate cap 144" "F9 changed the fps cap"
expect "$RUN1" "game menu opened" "Esc opened the game menu"
expect "$RUN1" "menu opened" "S in the game menu opened the settings"
expect "$CFG" "fps_cap=240" "the menu changed the fps cap and saved it"
expect "$RUN1" "frame rate cap 240" "the new cap took effect at once"
expect "$RUN1" "menu closed" "Esc closed the menu"
expect "$RUN1" "leaving from the game menu" "Leave in the game menu"
if grep -q "reset character from the menu" "$RUN1"; then echo "  FAIL  Esc did not cancel the reset"; fail=1
else echo "  ok    Esc cancelled the reset question"; fi
expect "$RUN1" "display: fullscreen" "F11 went fullscreen"
expect "$RUN1" "display: windowed" "F11 came back to a window"
if [ "$RENDERER" = opengl ]; then
    expect "$RUN1" "renderer: OpenGL" "OpenGL renderer started"
    expect "$RUN1" "quality: medium" "F6 changed graphics quality"
elif [ "$RENDERER" = fast ]; then
    expect "$RUN1" "renderer: fast software renderer, [0-9]* thread" "fast software renderer started"
    expect "$RUN1" "CPU: .*; [0-9]* processor" "processors counted"
    expect "$RUN1" "switched to 8-bit palette" "F8 switched to 8-bit rendering"
    expect "$CFG" "colors=32" "colour choice saved"
    expect "$RUN1" "quality: medium" "F6 changed the fast renderer's quality"
else
    expect "$RUN1" "renderer: software" "software renderer started"
    expect "$RUN1" "switched to 8-bit palette" "F8 switched to 8-bit rendering"
    expect "$CFG" "colors=32" "colour choice saved"
    expect "$RUN1" "software render height" "F6 changed the render resolution"
fi
expect "$CFG" "zoom=6" "mouse wheel zoomed in 3 notches, into first person and out (13 -> 6.7 -> 0.5 -> 2.5 -> 6.1)"
expect "$RUN1" "shift lock on" "Shift turned shift lock on"
expect "$RUN1" "shift lock off" "and off"
expect "$RUN1" "camera: first person" "the wheel went into first person"
expect "$RUN1" "camera: third person" "and out of it"
if grep -q "yaw_deg=0" "$CFG"; then echo "  FAIL  mouse-look did not turn the camera"; fail=1
else echo "  ok    mouse-look turned the camera ($(grep yaw_deg "$CFG" | tr -d '\r'))"; fi
grep -o "benchmark: [0-9]* frames.*render [0-9x]*" "$RUN1" | sed 's/^/  /' || true
echo "Screenshots: $SHOT, ${SHOT%.png}-gui.png, ${SHOT%.png}-door.png, ${SHOT%.png}-chat.png, ${SHOT%.png}-menu.png, ${SHOT%.png}-help.png, ${SHOT%.png}-fullscreen.png"
echo "Logs:       $RUN1, $RUN2, $LOG"
exit $fail
