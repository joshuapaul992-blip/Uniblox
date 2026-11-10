#!/bin/bash
# Uniblox - compares the renderers on the demo place under Wine:
# "uniblox.exe -benchmark" flies round it for 10 s with the frame cap off,
# logs the result and quits.
#   tools/wine_bench.sh [wine-windows-version] [renderer...]
# e.g. tools/wine_bench.sh nt40 software fast
# Extra settings for every run can be given in UB_BENCH_CFG, e.g.
# UB_BENCH_CFG='render_threads=1' or 'colors=8'.
set -euo pipefail
cd "$(dirname "$0")/.."
VER=${1:-nt40}
shift || true
RENDERERS=${*:-software fast}
DIR=build/win32
for tool in wine xvfb-run; do command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }; done
[ -f "$DIR/uniblox.exe" ] || { echo "build $DIR/uniblox.exe first (make)" >&2; exit 2; }
export WINEPREFIX=${WINEPREFIX:-$PWD/build/wineprefix}
export WINEDEBUG=-all
cp -f "$DIR/uniblox.cfg" "$DIR/uniblox.cfg.bench-saved" 2>/dev/null || true
for r in $RENDERERS; do
    printf 'renderer=%s\r\nsw_height=0\r\n' "$r" > "$DIR/uniblox.cfg"
    for kv in ${UB_BENCH_CFG:-}; do printf '%s\r\n' "$kv" >> "$DIR/uniblox.cfg"; done
    rm -f "$DIR/uniblox.log"
    xvfb-run -a -s "-screen 0 1280x720x24" bash -c "
        wine reg add 'HKCU\\Software\\Wine' /v Version /d '$VER' /f >/dev/null 2>&1
        timeout 120 wine '$DIR/uniblox.exe' -benchmark >/dev/null 2>&1 || true
        wineserver -k 2>/dev/null || true" 2>/dev/null
    line=$(grep -o "benchmark: [0-9]* frames.*" "$DIR/uniblox.log" | head -1 || true)
    desc=$(grep -o "renderer: .*" "$DIR/uniblox.log" | head -1 || true)
    echo "$r: ${line:-no result (see $DIR/uniblox.log)}"
    echo "    $desc"
done
mv -f "$DIR/uniblox.cfg.bench-saved" "$DIR/uniblox.cfg" 2>/dev/null || rm -f "$DIR/uniblox.cfg"
