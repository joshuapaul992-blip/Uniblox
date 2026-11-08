#!/usr/bin/env bash
# Uniblox - automated smoke test under Wine + Xvfb.
#
#   tools/wine_smoke.sh [wine-windows-version] [screenshot.png]
#   e.g. tools/wine_smoke.sh nt351
#
# Runs build/win32/uniblox.exe twice with Wine pretending to be the given
# Windows version, drives it with real X keyboard/mouse events (xdotool),
# then checks uniblox.log and uniblox.cfg for the expected results.
#
# Wine is far more forgiving than real NT 3.51/4.0: passing this proves the
# logic works, not that the .exe runs on real Windows. Always test there too.
#
# Needs: wine (32-bit), xvfb-run, xdotool, imagemagick (import).
set -euo pipefail

VER=${1:-nt351}
SHOT=${2:-build/smoke-$VER.png}
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
rm -f "$LOG" "$DIR/uniblox.old.log" "$CFG"
mkdir -p "$(dirname "$SHOT")"

# Everything that needs the X server runs inside one xvfb-run session.
xvfb-run -a -s "-screen 0 800x600x24" bash -s "$VER" "$EXE" "$SHOT" <<'EOF'
set -euo pipefail
VER=$1 EXE=$2 SHOT=$3
wine reg add 'HKCU\Software\Wine' /v Version /d "$VER" /f >/dev/null 2>&1

start_game() {
    wine "$EXE" & GAME_PID=$!
    WIN=$(timeout 60 xdotool search --sync --name '^Uniblox$' | head -1)
    xdotool windowfocus --sync "$WIN" 2>/dev/null || true
    sleep 1
}

stop_game() {
    xdotool key Escape
    for _ in $(seq 50); do kill -0 "$GAME_PID" 2>/dev/null || break; sleep 0.2; done
    if kill -0 "$GAME_PID" 2>/dev/null; then
        echo "game did not exit after Escape" >&2
        kill "$GAME_PID"
        exit 1
    fi
}

# Run 1: exercise input, then quit (which saves uniblox.cfg).
start_game
xdotool keydown w; sleep 0.4; xdotool keyup w           # move
xdotool keydown space; sleep 0.1; xdotool keyup space   # jump
sleep 0.8
xdotool key --delay 0 Return h i Return                 # chat text, all in one burst
xdotool key slash; xdotool type 'x'; xdotool key Escape  # open + cancel: must not quit
eval "$(xdotool getwindowgeometry --shell "$WIN")"
# Wine can drop a button event that arrives in the same instant as the
# pointer move onto its window, so pause after every move.
xdotool mousemove $((X + WIDTH / 4)) $((Y + HEIGHT / 2)); sleep 0.3
xdotool click 1                                         # click (left side)
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
xdotool key F8; sleep 1                                 # 8-bit palette rendering
import -window root "${SHOT%.png}-8bit.png"
xdotool key F8; sleep 0.3                               # back to 32-bit
xdotool key F6; sleep 0.5; xdotool key F6; sleep 0.3    # window 1x, then back to 2x
xdotool key F5; sleep 11.5                              # benchmark (10 s)
stop_game

# Run 2: settings must load back.
start_game
stop_game
wineserver -k 2>/dev/null || true
EOF

fail=0
expect() { # expect <file> <pattern> <description>
    if grep -q -- "$2" "$1"; then echo "  ok    $3"; else echo "  FAIL  $3 (no \"$2\" in $1)"; fail=1; fi
}
echo "Smoke test ($VER):"
expect "$LOG" "Windows" "OS detected"
expect "$LOG" "audio: " "audio initialised (device or graceful fallback)"
expect "$LOG" "demo ready" "app started"
expect "$LOG" "loaded uniblox.cfg" "settings loaded on second run"
expect "$LOG" "shutdown complete" "clean shutdown"
expect "$DIR/uniblox.old.log" "jump 1" "space bar jumped"
expect "$DIR/uniblox.old.log" "typed: hi" "typed text received"
expect "$DIR/uniblox.old.log" "click 1 at" "left click received"
expect "$DIR/uniblox.old.log" "mouse look on" "right button started mouse-look"
expect "$DIR/uniblox.old.log" "saved uniblox.cfg" "settings saved"
expect "$DIR/uniblox.old.log" "switched to 8-bit palette" "F8 switched to 8-bit rendering"
expect "$DIR/uniblox.old.log" "benchmark: [0-9]* frames" "F5 benchmark completed"
expect "$CFG" "colors=32" "colour choice saved"
expect "$DIR/uniblox.old.log" "window scale 1x" "F6 resized the window"
expect "$CFG" "zoom=14" "mouse wheel zoomed in 3 notches (22 -> 14)"
if grep -q "yaw_deg=0" "$CFG"; then echo "  FAIL  mouse-look did not turn the camera"; fail=1
else echo "  ok    mouse-look turned the camera ($(grep yaw_deg "$CFG" | tr -d '\r'))"; fi
grep -o "benchmark: [0-9]* frames.*per frame" "$DIR/uniblox.old.log" | sed 's/^/  /' || true
echo "Screenshot: $SHOT (8-bit: ${SHOT%.png}-8bit.png)"
echo "Log:        $LOG (previous run: $DIR/uniblox.old.log)"
exit $fail
