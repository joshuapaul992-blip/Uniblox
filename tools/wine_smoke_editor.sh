#!/usr/bin/env bash
# Uniblox - smoke test of the editor (ubedit.exe) under Wine + Xvfb.
#
#   tools/wine_smoke_editor.sh [wine-windows-version] [renderer]
#   e.g. tools/wine_smoke_editor.sh nt351 software
#
# Builds a little place with real mouse clicks and keys (insert a block,
# give it a behaviour, save it, play it), then opens the saved place in
# uniblox.exe and checks both logs. Screenshots go to build/smoke-editor-*.png.
#
# The clicks use fixed positions in a 1024x640 window: the editor's layout
# depends only on the window size (its font size is fixed).
set -euo pipefail

VER=${1:-nt351}
RENDERER=${2:-software}
DIR=build/win32
SHOT=build/smoke-editor-$VER-$RENDERER
for tool in wine xvfb-run xdotool import; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }
done
[ -f "$DIR/ubedit.exe" ] || { echo "build $DIR/ubedit.exe first (make)" >&2; exit 2; }
export WINEPREFIX=${WINEPREFIX:-$PWD/build/wineprefix}
export WINEDEBUG=-all
rm -f "$DIR/ubedit.log" "$DIR/ubedit.old.log" "$DIR/places/smoke.ubx" "$DIR/uniblox.log"
printf 'renderer=%s\r\nwindow_w=1024\r\nwindow_h=640\r\n' "$RENDERER" > "$DIR/ubedit.cfg"
printf 'renderer=%s\r\n' "$RENDERER" > "$DIR/uniblox.cfg"

xvfb-run -a -s "-screen 0 1280x800x24" bash -s "$VER" "$SHOT" <<'EOF'
set -euo pipefail
VER=$1 SHOT=$2
wine reg add 'HKCU\Software\Wine' /v Version /d "$VER" /f >/dev/null 2>&1
wine build/win32/ubedit.exe & P=$!
WIN=$(timeout 60 xdotool search --sync --name '^Uniblox Editor' | head -1)
xdotool windowfocus --sync "$WIN" 2>/dev/null || true
for _ in $(seq 120); do grep -q "editor ready" build/win32/ubedit.log 2>/dev/null && break; sleep 0.5; done
sleep 1
eval "$(xdotool getwindowgeometry --shell "$WIN")"
CX=$X CY=$Y   # (under Xvfb, without a window manager, this is the client area)
at() { xdotool mousemove $((CX + $1)) $((CY + $2)); sleep 0.25; xdotool click 1; sleep 0.4; }
at 355 41                       # toolbar: + Block
at 945 485                      # properties: Add behaviour (a list opens)
import -window root "$SHOT-list.png"
at 945 286                      # ... Kill brick (the list opens upwards: no room below)
import -window root "$SHOT-built.png"
at 900 485                      # properties: Edit KillBrick (the script editor)
sleep 0.5
import -window root "$SHOT-script.png"
at 829 556                      # Cancel
xdotool key ctrl+s; sleep 0.6   # a new place: Save as asks for a name
import -window root "$SHOT-saveas.png"
at 560 274                      # the name box
xdotool type --delay 60 smoke; xdotool key Return; sleep 0.3
at 409 319                      # Save
sleep 0.5
xdotool key F5; sleep 2.5       # play test
import -window root "$SHOT-play.png"
xdotool key Escape; sleep 0.8   # stop
xdotool key ctrl+z; sleep 0.3   # undo works after playing
xdotool key ctrl+y; sleep 0.3
kill $P 2>/dev/null || true
sleep 1
wineserver -k 2>/dev/null || true
# the game plays the saved place
wine build/win32/uniblox.exe 'places\smoke.ubx' & G=$!
WIN=$(timeout 60 xdotool search --sync --name '^Uniblox' | head -1)
xdotool windowfocus --sync "$WIN" 2>/dev/null || true
for _ in $(seq 120); do grep -q "demo ready" build/win32/uniblox.log 2>/dev/null && break; sleep 0.5; done
sleep 2
import -window root "$SHOT-game.png"
xdotool key Escape; sleep 0.5; xdotool key q   # the menu, then Quit
for _ in $(seq 50); do kill -0 "$G" 2>/dev/null || break; sleep 0.2; done
kill $G 2>/dev/null || true
wineserver -k 2>/dev/null || true
EOF

fail=0
expect() {
    if grep -q -- "$2" "$1"; then echo "  ok    $3"; else echo "  FAIL  $3 (no \"$2\" in $1)"; fail=1; fi
}
LOG=$DIR/ubedit.log
echo "Editor smoke test ($VER, $RENDERER renderer):"
expect "$LOG" "editor ready" "the editor started"
expect "$LOG" "editor: Inserted Block" "the toolbar inserted a block"
expect "$LOG" "editor: Kill brick added to 1 part" "a behaviour was added from the properties list"
expect "$LOG" "editor: Saved places/smoke.ubx" "Ctrl+S saved through the Save as dialog"
expect "$LOG" "editor: Play test: F5 or Esc stops" "F5 started a play test"
expect "$LOG" "script(s)\|scripts:" "scripts ran in the play test" || true
expect "$LOG" "editor: Play test stopped" "Esc stopped it"
if grep -E "script: .*(attempt to|is not a valid|was stopped|error)" "$LOG"; then echo "  FAIL  a script failed"; fail=1
else echo "  ok    no script errors"; fi
expect "$DIR/uniblox.log" "loaded place \"places\\\\smoke.ubx\"\|loaded place" "uniblox.exe opened the saved place"
expect "$DIR/uniblox.log" "1 script(s) running" "its behaviour script runs in the game"
expect "$DIR/uniblox.log" "shutdown complete" "the game shut down cleanly"
echo "Screenshots: $SHOT-list.png, $SHOT-built.png, $SHOT-script.png, $SHOT-play.png, $SHOT-game.png"
exit $fail
