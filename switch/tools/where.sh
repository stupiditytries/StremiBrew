#!/bin/bash
# Turns the places a crash was logged at ("program+0x1234") into functions and lines.
# Run in Git Bash on Windows after switch/build.sh, with the build that crashed.
#   where.sh 0x1234 0x5678 ...
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
"${DEVKITPRO_WIN:-/c/devkitPro}/devkitA64/bin/aarch64-none-elf-addr2line.exe" -f -C -i -p \
    -e "$repo/build/switch/StremiBrew.elf" "$@"
