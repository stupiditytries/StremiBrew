#!/bin/bash
# Type-checks the core bridge for the Switch (run inside WSL as root, after setup.sh).
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
CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-/root/stremio/target-switch} cargo check --release --lib \
    --target "$here/aarch64-nintendo-switch.json" -Zbuild-std=std,panic_abort -Zjson-target-spec 2>&1 |
    grep -E "^(error|warning: unused)" -A"${CONTEXT:-9}" | head -"${LINES_SHOWN:-90}"
echo "check finished"
