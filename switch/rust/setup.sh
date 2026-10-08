#!/bin/bash
# Sets up a Rust toolchain of the Switch build's own (run inside WSL as root). The Switch
# needs a changed libc crate (see patch.py); it is changed in a copy of cargo's home, so
# the toolchain and crates the PS5 build uses are never touched.
#   BASE_RUSTUP, BASE_CARGO   the toolchain and crates to copy (the PS5 build's)
#   RUSTUP_HOME, CARGO_HOME   where the Switch's copies go
set -euo pipefail
BASE_RUSTUP=${BASE_RUSTUP:-/root/.rustup}
BASE_CARGO=${BASE_CARGO:-/root/.cargo}
export RUSTUP_HOME=${RUSTUP_HOME:-/root/.rustup-switch} CARGO_HOME=${CARGO_HOME:-/root/.cargo-switch}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# The version of the libc crate the standard library is built against (and so the one
# everything else has to use too: see layout-check/Cargo.toml).
libc_version=0.2.190
if [[ ! -d $RUSTUP_HOME ]]; then
    cp -a "$BASE_RUSTUP" "$RUSTUP_HOME"
    echo "copied the toolchain to $RUSTUP_HOME"
fi
if [[ ! -d $CARGO_HOME ]]; then
    cp -a "$BASE_CARGO" "$CARGO_HOME"
    echo "copied cargo's home to $CARGO_HOME"
fi
# The standard library is built against that version too (its own lock file names an
# older one).
std_lock=$(ls "$RUSTUP_HOME"/toolchains/*/lib/rustlib/src/rust/library/Cargo.lock)
if [[ $(python3 "$here/pin-libc.py" "$std_lock" "$here/../../shared/core-bridge/Cargo.lock" $libc_version) == changed ]]; then
    echo "the standard library now builds against libc $libc_version"
    rm -rf /root/stremio/target-switch /root/stremio/target-switch-layout
fi
# The libc crate is patched from its pristine form every time, and only when the result
# would differ from what is there (so that nothing is rebuilt for no reason).
pristine=$(ls -d "$BASE_CARGO"/registry/src/*/libc-$libc_version)
target=$(dirname "$(ls -d "$CARGO_HOME"/registry/src/*/libc-$libc_version)")/libc-$libc_version
work=$(mktemp -d)
cp -a "$pristine" "$work/libc"
python3 "$here/patch.py" "$work/libc" >/dev/null
if ! diff -rq "$work/libc" "$target" >/dev/null 2>&1; then
    rm -rf "$target"
    cp -a "$work/libc" "$target"
    echo "patched the libc crate for the Switch"
    # Cargo does not notice a changed crate of the registry's: what was built from the
    # old one is thrown away.
    rm -rf /root/stremio/target-switch /root/stremio/target-switch-layout
fi
rm -rf "$work"
