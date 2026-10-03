#!/bin/bash
# Builds the web-view spike into a PS5 app folder. Run inside WSL as root.
set -euo pipefail
REPO=${REPO:-/path/to/StremiBrew}
WORK=${WORK:-/root/stremio}
TEMPLATE=${TEMPLATE:-/root/eden/ps5-native-app-boilerplate}
TITLE_ID=${TITLE_ID:-PPSA99704}

app=$WORK/web-app

# 1. Working copy of the app template with the spike's sources, the stand-ins for functions
#    the console lacks, and the app's own heap.
mkdir -p "$app"
rsync -a --delete --exclude .git --exclude build --exclude dist --exclude src \
    --exclude vendor "$TEMPLATE/" "$app/"
rm -rf "$app/src"
mkdir -p "$app/src"
cp "$TEMPLATE/src/demo_renderer.cpp" "$TEMPLATE/src/demo_renderer.hpp" "$app/src/"
cp "$REPO"/ps5/web-app/src/* "$app/src/"
cp "$REPO"/ps5/spike-app/src/compat*.c "$REPO/ps5/runtime/heap.c" "$app/src/"
cp "$REPO/ps5/spike-app/app-symbols.map" "$app/tooling/native/app-symbols.map"

# 2. Identity, then the template's own build.
(cd "$app" && make init TITLE_ID="$TITLE_ID" APP_NAME="Web View Spike" >/dev/null)
(cd "$app" && make app USE_CCACHE=0)

# 3. Every import must come from a module a game process loads.
if readelf -d "$app/build/llvm-pie.elf" | grep -E 'NEEDED.*lib(ScePosixForWebKit|SceRandom|kernel_sys|kernel_web)'; then
    echo "error: the app imports from a module that game processes do not load" >&2
    exit 1
fi
readelf -d "$app/build/llvm-pie.elf" | grep NEEDED
ls -la "$app/dist/$TITLE_ID"
