#!/bin/bash
# Builds the PC preview on Windows (clang with Visual Studio's CMake and Ninja) and, given
# a file name, saves a screenshot.
#   preview.sh [shot.png] [keys] [backdrop: soft, light or sharp]
#              [calibration mock-up: a or b for the look, then l, w, d or f for the state]
# Sample data comes from build/preview-data (see ps5/core-bridge's board example and
# tools/fetch_preview_images.py).
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
vs="/c/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake"
export PATH="$vs/Ninja:/c/Program Files/LLVM/bin:$PATH"
cmake="$vs/CMake/bin/cmake.exe"
build=$repo/build/preview

glfw=${STREMIO_GLFW_DIR:-/path/to/glfw}
if [[ ! -f $build/build.ninja ]]; then
    "$cmake" -S "$repo/ps5/ui" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DSTREMIO_GLFW_DIR="$(cygpath -m "$glfw")" > "$build.configure.log" 2>&1 ||
        { tail -30 "$build.configure.log"; exit 1; }
fi
"$cmake" --build "$build" 2>&1 | grep -E "error|FAILED|undefined|warning: unused" | head -40 || true
[[ -f $build/stremio_preview.exe ]] || { echo "the preview did not build" >&2; exit 1; }

if [[ $# -ge 1 ]]; then
    "$build/stremio_preview.exe" "$repo/build/preview-data/board.json" \
        "$repo/build/preview-data/images" "$repo/ps5/ui/assets/fonts" \
        --shot "$1" --keys "${2:-}" ${3:+--backdrop "$3"} ${4:+--calibrate "$4"}
    ls -la "$1"
fi
