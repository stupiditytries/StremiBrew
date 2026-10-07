#!/bin/bash
# Builds the core bridge for the Switch as a static library (run inside WSL as root) and
# copies it to build/switch in this repository, where switch/build.sh links it.
set -uo pipefail
export RUSTUP_HOME=${RUSTUP_HOME:-/root/.rustup-switch} CARGO_HOME=${CARGO_HOME:-/root/.cargo-switch}
export PATH=$CARGO_HOME/bin:$PATH
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
bash "$here/setup.sh" || exit 1
# Crates that compile C get clang (the Switch's own compiler is installed on the Windows
# side, out of reach from here), aimed at the Switch's processor and reading devkitA64's
# C library headers.
dkp=${DEVKITPRO_WSL:-/mnt/c/devkitPro}
export CC_aarch64_nintendo_switch=clang-18 CXX_aarch64_nintendo_switch=clang++-18
export AR_aarch64_nintendo_switch=llvm-ar-18
export CFLAGS_aarch64_nintendo_switch="--target=aarch64-none-elf -march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIE -D__SWITCH__ -isystem $dkp/devkitA64/aarch64-none-elf/include -isystem $dkp/libnx/include"
cd "$here/../../shared/core-bridge"
CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-/root/stremio/target-switch} cargo build --release --lib \
    --target "$here/aarch64-nintendo-switch.json" -Zbuild-std=std,panic_abort -Zjson-target-spec 2>&1 |
    grep -E "^(error|warning: unused)" -A"${CONTEXT:-9}" | head -"${LINES_SHOWN:-90}"
# The ring crate (cryptography, under TLS) has its fastest routines in assembly, which
# its own build leaves out for a system it does not know. The ones it ships ready-made
# for 64-bit ARM Linux are in a form the Switch takes as they are; they are assembled
# here into a library of their own.
version=$(grep -A1 '^name = "ring"$' Cargo.lock | sed -n 's/^version = "\(.*\)"$/\1/p')
ring=$(ls -d "$CARGO_HOME"/registry/src/*/ring-"$version")
work=/root/stremio/switch/ring-asm
rm -rf "$work" && mkdir -p "$work"
for source in "$ring"/pregenerated/*-linux64.S; do
    clang-18 --target=aarch64-none-elf -march=armv8-a+crc+crypto -fPIE -I"$ring/include" \
        -I"$ring/pregenerated" -c "$source" -o "$work/$(basename "$source" .S).o" || exit 1
done
mkdir -p "$here/../../build/switch"
rm -f "$here/../../build/switch/libring_asm.a"
llvm-ar-18 rcs "$here/../../build/switch/libring_asm.a" "$work"/*.o
lib=${CARGO_TARGET_DIR:-/root/stremio/target-switch}/aarch64-nintendo-switch/release/libstremio_core_ps5.a
mkdir -p "$here/../../build/switch"
cp "$lib" "$here/../../build/switch/libstremio_core.a" && ls -la "$here/../../build/switch/libstremio_core.a"
