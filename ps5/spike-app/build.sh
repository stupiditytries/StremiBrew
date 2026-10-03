#!/bin/bash
# Builds the Rust spike into a PS5 app folder. Run inside WSL as root.
#   REPO   this repository as seen from WSL
#   WORK   build area on the WSL disk
#   TEMPLATE  checkout of ps5-native-app-boilerplate (provides the SDK and packaging tool)
set -euo pipefail
REPO=${REPO:-/path/to/StremiBrew}
WORK=${WORK:-/root/stremio}
TEMPLATE=${TEMPLATE:-/root/eden/ps5-native-app-boilerplate}
TITLE_ID=${TITLE_ID:-PPSA99701}
RUST_TARGET=${RUST_TARGET:-x86_64-unknown-freebsd}
export RUSTUP_HOME=/root/.rustup CARGO_HOME=/root/.cargo PATH=/root/.cargo/bin:$PATH

app=$WORK/spike-app
sdk=$TEMPLATE/.deps/native/ps5-payload-sdk

# 1. Rust static library.
cargo_args=(build --release --target "$RUST_TARGET")
if [[ $RUST_TARGET == *.json ]]; then
    cargo_args+=(-Zbuild-std=std,panic_abort -Zjson-target-spec)
    # The console's kernel is FreeBSD 11: file status, directory entries and kernel events
    # use the layouts from before FreeBSD 12, which is what the libc crate assumes by default.
    export RUSTFLAGS='--cfg libc_unstable_freebsd_version="11"'
    bash "$REPO/ps5/patch-rust-src.sh"
fi
(cd "$REPO/ps5/spike-rust" && CARGO_TARGET_DIR=$WORK/target-spike cargo "${cargo_args[@]}")
triple=$(basename "$RUST_TARGET" .json)
rust_lib=$WORK/target-spike/$triple/release/libspike_rust.a

# 2. Working copy of the app template with the spike's sources.
mkdir -p "$app"
rsync -a --delete --exclude .git --exclude build --exclude dist --exclude src \
    --exclude vendor "$TEMPLATE/" "$app/"
mkdir -p "$app/src" "$app/vendor"
cp "$TEMPLATE/src/demo_renderer.cpp" "$TEMPLATE/src/demo_renderer.hpp" "$app/src/"
cp "$REPO"/ps5/spike-app/src/*.cpp "$REPO"/ps5/spike-app/src/*.c "$app/src/"
# With FreeBSD 11 layouts the libc crate asks for versioned names such as stat@FBSD_1.0.
# The console's modules export the same functions unversioned, so the suffix is removed.
"$sdk/bin/prospero-nm" -u "$rust_lib" 2>/dev/null | awk '{print $NF}' | grep '@FBSD_' |
    sort -u | sed -E 's/^(.*)@FBSD_.*$/& \1/' > "$WORK/target-spike/versioned-symbols.txt"
"$sdk/bin/prospero-objcopy" --redefine-syms="$WORK/target-spike/versioned-symbols.txt" \
    "$rust_lib" "$app/vendor/libspike_rust.a"
cp "$REPO/ps5/spike-app/app-symbols.map" "$app/tooling/native/app-symbols.map"

# 3. Identity, then the template's own build.
(cd "$app" && make init TITLE_ID="$TITLE_ID" APP_NAME="Rust Spike" >/dev/null)
(cd "$app" && make app USE_CCACHE=0 \
    APP_STATIC_ARCHIVES="vendor/libspike_rust.a")

# 4. Every import must come from a module a game process loads; an import that only
#    libScePosixForWebKit or libkernel_sys provides is a null pointer on the console.
if readelf -d "$app/build/llvm-pie.elf" | grep -E 'NEEDED.*lib(ScePosixForWebKit|kernel_sys|kernel_web)'; then
    echo "error: the app imports from a module that game processes do not load" >&2
    exit 1
fi
readelf -d "$app/build/llvm-pie.elf" | grep NEEDED
ls -la "$app/dist/$TITLE_ID"
