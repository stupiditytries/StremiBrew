#!/bin/bash
# Builds the core bridge and a small app that runs it into a PS5 app folder. Run inside WSL
# as root.
#   REPO   this repository as seen from WSL
#   WORK   build area on the WSL disk
#   TEMPLATE  checkout of ps5-native-app-boilerplate (provides the SDK and packaging tool)
set -euo pipefail
REPO=${REPO:-/path/to/StremiBrew}
WORK=${WORK:-/root/stremio}
TEMPLATE=${TEMPLATE:-/root/eden/ps5-native-app-boilerplate}
TITLE_ID=${TITLE_ID:-PPSA99703}
export RUSTUP_HOME=/root/.rustup CARGO_HOME=/root/.cargo PATH=/root/.cargo/bin:$PATH

app=$WORK/core-app
sdk=$TEMPLATE/.deps/native/ps5-payload-sdk
target=$REPO/ps5/x86_64-ps5-freebsd.json

# 1. Rust static library for the PS5 target (see ps5/spike-app/build.sh for the reasons
#    behind each setting).
export RUSTFLAGS='--cfg libc_unstable_freebsd_version="11"'
bash "$REPO/ps5/patch-rust-src.sh"
mkdir -p "$WORK/bin"
printf '#!/bin/sh\nPS5_PAYLOAD_SDK=%s exec sh %s/tooling/prospero-clang18 "$@"\n' \
    "$sdk" "$TEMPLATE" > "$WORK/bin/ps5-cc"
chmod +x "$WORK/bin/ps5-cc"
export CC_x86_64_ps5_freebsd=$WORK/bin/ps5-cc AR_x86_64_ps5_freebsd=llvm-ar-18
(cd "$REPO/ps5/core-bridge" && CARGO_TARGET_DIR=$WORK/target-bridge cargo build --release \
    --lib --target "$target" -Zbuild-std=std,panic_abort -Zjson-target-spec)
rust_lib=$WORK/target-bridge/x86_64-ps5-freebsd/release/libstremio_core_ps5.a

# 2. Working copy of the app template with the app's sources, the stand-ins for functions
#    the console lacks, and the app's own heap.
mkdir -p "$app"
rsync -a --delete --exclude .git --exclude build --exclude dist --exclude src \
    --exclude vendor "$TEMPLATE/" "$app/"
rm -rf "$app/src" "$app/vendor"
mkdir -p "$app/src" "$app/vendor"
cp "$TEMPLATE/src/demo_renderer.cpp" "$TEMPLATE/src/demo_renderer.hpp" "$app/src/"
cp "$REPO"/ps5/core-app/src/* "$app/src/"
cp "$REPO"/ps5/spike-app/src/compat*.c "$REPO/ps5/runtime/heap.c" "$app/src/"
"$sdk/bin/prospero-nm" -u "$rust_lib" 2>/dev/null | awk '{print $NF}' | grep '@FBSD_' |
    sort -u | sed -E 's/^(.*)@FBSD_.*$/& \1/' > "$WORK/target-bridge/versioned-symbols.txt"
"$sdk/bin/prospero-objcopy" --redefine-syms="$WORK/target-bridge/versioned-symbols.txt" \
    "$rust_lib" "$app/vendor/libstremio_core_ps5.a"
cp "$REPO/ps5/spike-app/app-symbols.map" "$app/tooling/native/app-symbols.map"

# 3. Identity, then the template's own build.
(cd "$app" && make init TITLE_ID="$TITLE_ID" APP_NAME="Stremio Core" >/dev/null)
(cd "$app" && make app USE_CCACHE=0 APP_STATIC_ARCHIVES="vendor/libstremio_core_ps5.a")

# 4. Every import must come from a module a game process loads.
if readelf -d "$app/build/llvm-pie.elf" | grep -E 'NEEDED.*lib(ScePosixForWebKit|SceRandom|kernel_sys|kernel_web)'; then
    echo "error: the app imports from a module that game processes do not load" >&2
    exit 1
fi
readelf -d "$app/build/llvm-pie.elf" | grep NEEDED
ls -la "$app/dist/$TITLE_ID"
