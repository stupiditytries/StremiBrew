#!/bin/bash
# Compiles the layout checks for the Switch (run inside WSL as root) and prints one line
# per disagreement between the libc crate and the Switch's C library.
set -uo pipefail
export RUSTUP_HOME=${RUSTUP_HOME:-/root/.rustup-switch} CARGO_HOME=${CARGO_HOME:-/root/.cargo-switch}
export PATH=$CARGO_HOME/bin:$PATH
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
bash "$here/../setup.sh" || exit 1
cd "$here"
CARGO_TARGET_DIR=/root/stremio/target-switch-layout cargo check --offline \
    --target "$here/../aarch64-nintendo-switch.json" -Zbuild-std=core -Zjson-target-spec 2>&1 |
    grep -E "^error" -A6 | grep -E "^error|assert!|libc::" | grep -v "^error: could not\|^error\[E0080\]" |
    sed -E 's/^[ 0-9|]*//' | sort | uniq -c | sort -rn | head -"${LINES_SHOWN:-150}"
echo "layout check finished"
