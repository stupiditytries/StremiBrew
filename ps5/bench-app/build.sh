#!/bin/bash
# Builds the decode benchmark into a PS5 app folder. Run inside WSL as root, after
# ps5/ffmpeg/build.sh. The clip to decode is added to the app folder's assets afterwards.
set -euo pipefail
REPO=${REPO:-/path/to/StremiBrew}
WORK=${WORK:-/root/stremio}
TEMPLATE=${TEMPLATE:-/root/eden/ps5-native-app-boilerplate}
TITLE_ID=${TITLE_ID:-PPSA99702}

app=$WORK/bench-app
ffmpeg=$WORK/ffmpeg-ps5/install

# 1. Working copy of the app template with the benchmark's sources and the stand-ins for
#    functions the console lacks (shared with the Rust spike).
mkdir -p "$app"
rsync -a --delete --exclude .git --exclude build --exclude dist --exclude src \
    --exclude vendor "$TEMPLATE/" "$app/"
rm -rf "$app/src" "$app/vendor"
mkdir -p "$app/src" "$app/vendor/lib"
cp "$TEMPLATE/src/demo_renderer.cpp" "$TEMPLATE/src/demo_renderer.hpp" "$app/src/"
cp "$REPO"/ps5/bench-app/src/* "$app/src/"
cp "$REPO"/ps5/spike-app/src/compat*.c "$app/src/"
cp -r "$ffmpeg/include" "$app/vendor/include"
cp "$ffmpeg"/lib/libavformat.a "$ffmpeg"/lib/libavcodec.a "$ffmpeg"/lib/libswresample.a \
    "$ffmpeg"/lib/libavutil.a "$app/vendor/lib/"
cp "$REPO/ps5/spike-app/app-symbols.map" "$app/tooling/native/app-symbols.map"

# 2. Identity, then the template's own build.
(cd "$app" && make init TITLE_ID="$TITLE_ID" APP_NAME="Decode Bench" >/dev/null)
(cd "$app" && make app USE_CCACHE=0 APP_INCLUDE_PATHS="vendor/include" \
    APP_STATIC_ARCHIVES="vendor/lib/libavformat.a vendor/lib/libavcodec.a vendor/lib/libswresample.a vendor/lib/libavutil.a")

# 3. Every import must come from a module a game process loads.
if readelf -d "$app/build/llvm-pie.elf" | grep -E 'NEEDED.*lib(ScePosixForWebKit|kernel_sys|kernel_web)'; then
    echo "error: the app imports from a module that game processes do not load" >&2
    exit 1
fi
readelf -d "$app/build/llvm-pie.elf" | grep NEEDED
ls -la "$app/dist/$TITLE_ID"
