#!/bin/bash
# Builds the web-view spike into a PS5 app folder. Run inside WSL as root.
set -euo pipefail
REPO=${REPO:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)}
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

# 2. Import stubs for console modules the SDK does not cover. A stub is a shared library
#    named after the module that exports the functions' names; the packaging tool only
#    reads the names. They go into the working copy's SDK, not the shared one.
sdk=$app/.deps/native/ps5-payload-sdk
stubs=$WORK/web-app-stubs
mkdir -p "$stubs"
: > "$app/src/dialog_nids.h"
grep -v '^#' "$REPO/ps5/web-app/stubs.txt" | tr -d '\r' | while read -r module functions; do
    [[ -n $module ]] || continue
    : > "$stubs/$module.c"
    for function in $functions; do
        printf 'void %s(void) {}\n' "$function" >> "$stubs/$module.c"
        # The hashed name (NID) the console's loader knows the function by.
        printf '#define NID_%s "%s"\n' "$function" "$("$sdk/bin/prospero-nid" "$function")" \
            >> "$app/src/dialog_nids.h"
    done
    # A generic ELF target: the PS5 target hides symbols that are not marked for export.
    clang-18 --target=x86_64-unknown-freebsd -fPIC -ffreestanding -c "$stubs/$module.c" \
        -o "$stubs/$module.o"
    # The plain linker: the SDK's wrapper adds options meant for executables.
    "$sdk/bin/ld.lld" -m elf_x86_64 -shared -soname "$module.sprx" "$stubs/$module.o" \
        -o "$sdk/target/lib/$module.so"
done

# 3. Identity, then the template's own build.
(cd "$app" && make init TITLE_ID="$TITLE_ID" APP_NAME="Web View Spike" >/dev/null)
(cd "$app" && make app USE_CCACHE=0)

# 4. Every import must come from a module a game process loads or the app loads itself.
if readelf -d "$app/build/llvm-pie.elf" | grep -E 'NEEDED.*lib(ScePosixForWebKit|SceRandom|kernel_sys|kernel_web)'; then
    echo "error: the app imports from a module that game processes do not load" >&2
    exit 1
fi
readelf -d "$app/build/llvm-pie.elf" | grep NEEDED
ls -la "$app/dist/$TITLE_ID"
