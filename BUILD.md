# Building StremiBrew

StremiBrew doesn't have a one-command build yet. The scripts in this repo are the ones the
app is made with, on one Windows machine with WSL, and they expect the tools below to
be where the scripts' variables say. Every path can be changed with the variable named at
the top of each script.

If you would just like to use the app, see
[INSTALLATION.md](INSTALLATION.md).

## Repo Overview

| Folder | What it is |
|---|---|
| `src`, `stremio-derive`, `stremio-watched-bitfield` | Stremio's `stremio-core` (Rust), unchanged in purpose |
| `shared/core-bridge` | A Rust crate that wraps `stremio-core` in a C interface for the apps |
| `shared/ui` | The interface (C++20, drawn with NanoVG), shared by every console, and a PC preview of it |
| `ps5` | The PS5 app: start-up, controller, sound, keyboard, the video player, FFmpeg's build script |
| `switch` | The Switch app, and the Rust toolchain set-up the Switch needs |

## Rendering UI Previews/Changes

This is the quickest thing to build, and an easy way to iterate on the interface without a console. It
renders the real screens with sample data, in a window or straight to a PNG.

Needs, on Windows: Visual Studio's CMake and Ninja, LLVM's clang, Git Bash, and a GLFW
source tree.

```bash
export STREMIO_GLFW_DIR=/path/to/glfw
bash shared/ui/tools/preview.sh build/shots/home.png ""
```

The sample data is read from `build/preview-data`, which is not in git. `board.json` is
written by the bridge's example (`cargo run --release --example board` in
`shared/core-bridge`), and `shared/ui/tools/fetch_preview_images.py` downloads the
pictures it refers to.

## The core bridge on a PC

This is what the build badge checks. Any recent Rust toolchain will do:

```bash
cd shared/core-bridge
cargo check --locked --lib --examples
```

## PS5

Built inside WSL (Ubuntu 22.04) as root.

**Needs**

- Rust nightly with the `rust-src` component (the standard library is built for the
  console with `-Zbuild-std`, against `ps5/x86_64-ps5-freebsd.json`)
- clang 18 and LLVM's tools
- [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
  which brings the PS5 Payload SDK, the start-up code, the linker layout and the
  packaging tool (`TEMPLATE`)
- The PS5 OpenGL SDK's `sdk` folder: headers and static libraries (`GL_SDK`)
- An FFmpeg source tree (`FFMPEG_SOURCE`)
- [whisper.cpp](https://github.com/ggml-org/whisper.cpp) 1.7.6, for subtitle
  auto-calibration (`WHISPER`)

**Steps**

```bash
# once: FFmpeg's libraries for the console
FFMPEG_SOURCE=/path/to/ffmpeg bash ps5/ffmpeg/build.sh

# the app
bash ps5/app/build.sh
```

The result is an app folder at `$WORK/app/dist/PPSA99710` (`WORK` defaults to
`/root/stremio`), holding `eboot.bin`, `sce_sys`, `sce_module` and `assets`.

## Switch

Built in two halves, because the Rust side is built in WSL and the rest with devkitPro on
Windows.

**Needs**

- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitA64 and libnx, and
  these packages: `switch-mesa`, `switch-glad`, `switch-libdrm_nouveau`, `switch-ffmpeg`,
  `switch-dav1d`, `switch-zlib`, `switch-bzip2`
- In WSL: the PS5 build's Rust nightly (it is copied, then changed, so the two builds
  never share a toolchain) and clang 18

**Steps**

```bash
# in WSL, as root: the core bridge
bash switch/rust/build.sh

# in Git Bash on Windows: the app
bash switch/build.sh
```

The result is `build/switch/StremiBrew.nro`.

`switch/rust` exists because Rust does not support the Switch out of the box. It holds a
target description, a patch for the `libc` crate, and `layout-check`, which fails the
build if Rust and the console's C library disagree about the layout of any structure they
share.

## Licenses of what gets linked

The source in this repository is under the MIT license. The console binaries also contain
other people's work, and some of it is under the GPL:

| Build | Component | License |
|---|---|---|
| Switch | FFmpeg, as packaged by devkitPro | GPL v2 or later |
| PS5 | FFmpeg, as built by `ps5/ffmpeg/build.sh` | LGPL v2.1 or later |
| PS5 | PS5 Payload SDK start-up code, the PS5 OpenGL library, the app template | GPL v3 |
| Both | `stremio-core`, whisper.cpp (PS5), NanoVG, libwebp, dav1d (Switch), libnx (Switch) | MIT, zlib, BSD or ISC |

A built `.nro` or PS5 app is a GPL work as a whole. If you release any sort of build relating to StremiBrew, pass on the GPL's terms with it and say where this source is.
