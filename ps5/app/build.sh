#!/bin/bash
# Builds the app into a PS5 app folder. Run inside WSL as root.
#   REPO      this repository as seen from WSL
#   WORK      build area on the WSL disk
#   TEMPLATE  checkout of ps5-native-app-boilerplate: the PS5 Payload SDK, the startup code,
#             the linker layout and the packaging tool
#   GL_SDK    the PS5 OpenGL SDK's `sdk` folder (headers and static libraries)
set -euo pipefail
REPO=${REPO:-/path/to/StremiBrew}
WORK=${WORK:-/root/stremio}
TEMPLATE=${TEMPLATE:-/root/eden/ps5-native-app-boilerplate}
GL_SDK=${GL_SDK:-/root/eden/edenfork/.deps/ps5-opengl-sdk-1.0.0/sdk}
JOBS=${JOBS:-$(nproc)}
export RUSTUP_HOME=/root/.rustup CARGO_HOME=/root/.cargo PATH=/root/.cargo/bin:$PATH

sdk=$TEMPLATE/.deps/native/ps5-payload-sdk
out=$WORK/app
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' \
    "$REPO/ps5/app/sce_sys/param.json")
mkdir -p "$out/obj" "$out/stubs" "$WORK/bin"

# The compiler, with the SDK's target settings.
printf '#!/bin/sh\nPS5_PAYLOAD_SDK=%s exec sh %s/tooling/prospero-clang18 "$@"\n' \
    "$sdk" "$TEMPLATE" > "$WORK/bin/ps5-cc"
chmod +x "$WORK/bin/ps5-cc"

# 1. The core bridge (Rust). See ps5/spike-app/build.sh for the reason behind each setting.
export RUSTFLAGS='--cfg libc_unstable_freebsd_version="11"'
bash "$REPO/ps5/patch-rust-src.sh"
export CC_x86_64_ps5_freebsd=$WORK/bin/ps5-cc AR_x86_64_ps5_freebsd=llvm-ar-18
(cd "$REPO/ps5/core-bridge" && CARGO_TARGET_DIR=$WORK/target-bridge cargo build --release \
    --lib --target "$REPO/ps5/x86_64-ps5-freebsd.json" -Zbuild-std=std,panic_abort \
    -Zjson-target-spec)
"$sdk/bin/prospero-nm" -u "$WORK/target-bridge/x86_64-ps5-freebsd/release/libstremio_core_ps5.a" \
    2>/dev/null | awk '{print $NF}' | grep '@FBSD_' | sort -u |
    sed -E 's/^(.*)@FBSD_.*$/& \1/' > "$out/versioned-symbols.txt"
"$sdk/bin/prospero-objcopy" --redefine-syms="$out/versioned-symbols.txt" \
    "$WORK/target-bridge/x86_64-ps5-freebsd/release/libstremio_core_ps5.a" \
    "$out/libstremio_core_ps5.a"

# 2. C and C++ sources: the app, the UI, the drawing and image libraries, the stand-ins for
#    functions the console lacks, the app's heap, and the template's startup code.
ui=$REPO/ps5/ui
includes=(-I"$ui/src" -I"$ui/third_party/nanovg" -I"$ui/third_party"
    -I"$ui/third_party/libwebp/src" -I"$ui/third_party/libwebp"
    -I"$GL_SDK/include" -DGL_GLEXT_PROTOTYPES=1)
common=(-O2 -ffunction-sections -fdata-sections "${includes[@]}")
sources=("$REPO"/ps5/app/src/*.cpp "$ui"/src/*.cpp "$ui/third_party/nanovg/nanovg.c"
    "$REPO"/ps5/spike-app/src/compat*.c "$REPO/ps5/runtime/heap.c"
    "$TEMPLATE/tooling/native/app_crt.cpp")
while IFS= read -r file; do
    sources+=("$file")
done < <(find "$ui/third_party/libwebp/src/dec" "$ui/third_party/libwebp/src/dsp" \
    "$ui/third_party/libwebp/src/utils" -name '*.c' |
    grep -v -E '/dsp/(enc|cost|ssim|picture)[^/]*\.c$|_enc[^/]*\.c$' | sort)

compile() {
    local source=$1 object
    object=$out/obj/$(echo "${source#/}" | tr '/' '_').o
    if [[ $object -nt $source && $object -nt $REPO/ps5/app/build.sh ]]; then
        return 0
    fi
    case $source in
        *.cpp) "$WORK/bin/ps5-cc" -std=c++20 -fno-exceptions -fno-rtti "${common[@]}" \
            -c "$source" -o "$object" ;;
        */libwebp/*) "$WORK/bin/ps5-cc" -std=c11 -msse4.1 "${common[@]}" -c "$source" -o "$object" ;;
        # The app links the real unwinder, so the stand-in accessors are left out.
        *) "$WORK/bin/ps5-cc" -std=c11 -DAPP_HAS_UNWINDER "${common[@]}" -c "$source" -o "$object" ;;
    esac
}
export -f compile
export out WORK REPO
declare -p common > "$out/compile-flags.sh"
running=0
failed=0
for source in "${sources[@]}"; do
    compile "$source" || failed=1 &
    if (( ++running % JOBS == 0 )); then wait || failed=1; fi
done
wait || failed=1
objects=()
for source in "${sources[@]}"; do
    object=$out/obj/$(echo "${source#/}" | tr '/' '_').o
    [[ -f $object ]] || { echo "error: $source did not compile" >&2; exit 1; }
    objects+=("$object")
done

# 3. Link. The graphics library is C++ and static, so the C++ runtime and the SDK's C
#    library archive come along; console modules are tried first, so the archive only
#    supplies what no module has. The loader has no dynamic-loading hooks to offer, and
#    weak references nothing defines must stay null instead of becoming imports.
cp "$sdk"/target/lib/*.so "$GL_SDK/lib/libSceAgc.so" "$GL_SDK/lib/libSceAgcDriver.so" "$out/stubs/"
"$sdk/bin/prospero-lld" \
    -z nodynamic-undefined-weak \
    --defsym=__cxa_thread_atexit_impl=0 \
    --defsym=__dlopen=0 --defsym=__dlsym=0 --defsym=__dladdr=0 \
    --defsym=__dlclose=0 --defsym=__dlerror=0 \
    -L "$sdk/target/lib" -L "$GL_SDK/lib" \
    -T "$TEMPLATE/tooling/native/ps5-pie.ld" -T "$REPO/ps5/app/unwind.ld" \
    --eh-frame-hdr --gc-sections --version-script "$REPO/ps5/spike-app/app-symbols.map" \
    -e _start -u ps5_agc_gate2_run --error-limit=40 -Map="$out/llvm-pie.map" \
    -o "$out/llvm-pie.elf" "${objects[@]}" \
    --start-group "$out/libstremio_core_ps5.a" -lPS5OpenGL \
    "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a" \
    --end-group \
    --as-needed "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so" \
    "$sdk/target/lib/libSceLibcInternal.so" "$sdk/target/lib/libkernel.so" \
    "$sdk/target/lib/libSceVideoOut.so" "$sdk/target/lib/libSceSystemService.so" \
    "$sdk/target/lib/libSceUserService.so" "$sdk/target/lib/libScePad.so" \
    "$sdk/target/lib/libSceNet.so" "$sdk/target/lib/libc.a"

# Every import must come from a module a game process loads.
if readelf -d "$out/llvm-pie.elf" | grep -E 'NEEDED.*lib(ScePosixForWebKit|SceRandom|kernel_sys|kernel_web)'; then
    echo "error: the app imports from a module that game processes do not load" >&2
    exit 1
fi
readelf -d "$out/llvm-pie.elf" | grep NEEDED

# 4. Convert to the console's executable format, sign, and assemble the app folder.
bash "$TEMPLATE/tools/build-host-tools.sh" >/dev/null
tool=$TEMPLATE/build/host/ps5-native-tool
"$tool" link --in "$out/llvm-pie.elf" --out "$out/eboot.elf" --stub-dir "$out/stubs" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
app=$out/dist/$title_id
rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/assets/fonts"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$REPO/ps5/app/sce_sys/param.json" "$app/sce_sys/"
# Placeholder artwork from the template until the app has its own.
for asset in icon0.png pic0.dds pic1.dds; do
    cp "$TEMPLATE/sce_sys/$asset" "$app/sce_sys/"
done
cp "$TEMPLATE/runtime/libc.prx" "$app/sce_module/"
cp "$ui"/assets/fonts/*.ttf "$ui/assets/fonts/OFL.txt" "$app/assets/fonts/"
ls -la "$app" "$app/assets/fonts"
