#!/bin/bash
# Builds the app for the Switch as an .nro. Run in Git Bash on Windows, with devkitPro's
# devkitA64, libnx and the Switch portlibs (mesa, glad, libdrm_nouveau) installed.
#   DEVKITPRO_WIN  the devkitPro folder (default C:/devkitPro; the DEVKITPRO variable its
#                  installer sets is a path of its own shell's, no use here)
#   OUT        where the build goes (default build/switch in this repository)
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
dkp=$(cygpath -m "${DEVKITPRO_WIN:-/c/devkitPro}")
out=${OUT:-$repo/build/switch}
jobs=${JOBS:-$(nproc)}
export PATH="$(cygpath -u "$dkp")/devkitA64/bin:$(cygpath -u "$dkp")/tools/bin:$PATH"
# The linker's settings file finds its script through this variable.
export DEVKITPRO=$dkp
mkdir -p "$out/obj" "$out/romfs/fonts"

ui=$repo/shared/ui
arch=(-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE)
# The console's own libraries come first: the UI's third_party folder has the PC
# preview's OpenGL loader under the same name as the Switch's.
includes=(-I"$dkp/portlibs/switch/include" -I"$dkp/libnx/include" -I"$ui/src" -I"$ui/third_party/nanovg"
    -I"$ui/third_party" -I"$ui/third_party/libwebp/src" -I"$ui/third_party/libwebp")
common=(-g -O2 -ffunction-sections -D__SWITCH__ "${arch[@]}" "${includes[@]}")
# The core bridge (built by switch/rust/build.sh inside WSL), when it is there.
core=()
if [[ -f $out/libstremio_core.a ]]; then
    common+=(-DWITH_CORE)
    core=("$out/libstremio_core.a" "$out/libring_asm.a")
fi

sources=("$repo"/switch/src/*.cpp "$repo"/switch/src/*.c "$ui"/src/*.cpp "$ui/third_party/nanovg/nanovg.c")
while IFS= read -r file; do
    sources+=("$file")
done < <(find "$ui/third_party/libwebp/src/dec" "$ui/third_party/libwebp/src/dsp" \
    "$ui/third_party/libwebp/src/utils" -name '*.c' |
    grep -v -E '/dsp/(enc|cost|ssim|picture)[^/]*\.c$|_enc[^/]*\.c$' | sort)

object_of() {
    echo "$out/obj/$(echo "${1#"$repo"/}" | tr '/' '_').o"
}
compile() {
    local source=$1 object
    object=$(object_of "$source")
    # Third-party objects are kept between builds; the app's own are always rebuilt (they
    # include each other's headers).
    if [[ $source == */third_party/* && $object -nt $source && $object -nt $repo/switch/build.sh ]]; then
        return 0
    fi
    case $source in
        # The one file that asks what an unhandled C++ failure was needs exceptions on.
        */crash_log.cpp) aarch64-none-elf-g++ -std=gnu++20 "${common[@]}" -c "$source" -o "$object" ;;
        *.cpp) aarch64-none-elf-g++ -std=gnu++20 -fno-rtti -fno-exceptions "${common[@]}" -c "$source" -o "$object" ;;
        *) aarch64-none-elf-gcc -std=gnu11 "${common[@]}" -c "$source" -o "$object" ;;
    esac
}
running=0
failed=0
for source in "${sources[@]}"; do
    compile "$source" || failed=1 &
    if (( ++running % jobs == 0 )); then wait || failed=1; fi
done
wait || failed=1
objects=()
for source in "${sources[@]}"; do
    object=$(object_of "$source")
    [[ -f $object ]] || { echo "error: $source did not compile" >&2; exit 1; }
    objects+=("$object")
done

aarch64-none-elf-g++ -specs="$dkp/libnx/switch.specs" -g "${arch[@]}" -Wl,-Map,"$out/StremiBrew.map" \
    "${objects[@]}" "${core[@]}" -L"$dkp/portlibs/switch/lib" -L"$dkp/libnx/lib" \
    -lglad -lEGL -lglapi -ldrm_nouveau -lnx -lm -o "$out/StremiBrew.elf"

# The app's own files: the fonts, and for now the sample data the UI is shown with.
cp "$ui"/assets/fonts/*.ttf "$out/romfs/fonts/"
if [[ -f $repo/build/preview-data/board.json ]]; then
    cp "$repo/build/preview-data/board.json" "$out/romfs/"
    mkdir -p "$out/romfs/images"
    cp -u "$repo"/build/preview-data/images/* "$out/romfs/images/"
fi
nacptool --create "StremiBrew" "stupiditytries" "0.1.0" "$out/StremiBrew.nacp"
elf2nro "$out/StremiBrew.elf" "$out/StremiBrew.nro" --icon="$dkp/libnx/default_icon.jpg" \
    --nacp="$out/StremiBrew.nacp" --romfsdir="$out/romfs"
ls -la "$out/StremiBrew.nro"
