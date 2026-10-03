#!/usr/bin/env bash
# Builds FFmpeg's libraries for the PS5 as static archives. Run inside WSL as root.
#   FFMPEG_SOURCE  an FFmpeg source tree
#   WORK           build area on the WSL disk
#   PS5_PAYLOAD_SDK  the PS5 Payload SDK
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
FFMPEG_SOURCE=${FFMPEG_SOURCE:?set FFMPEG_SOURCE to an FFmpeg source tree}
WORK=${WORK:-/root/stremio}
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-/root/eden/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk}

# The wrapper is copied to the WSL disk so it runs with Unix line endings and permissions.
mkdir -p "$WORK/bin" "$WORK/ffmpeg-ps5"
tr -d '\r' < "$here/ps5-cc.sh" > "$WORK/bin/ffmpeg-ps5-cc"
chmod +x "$WORK/bin/ffmpeg-ps5-cc"

cd "$WORK/ffmpeg-ps5"
# Software decoding of what streams carry: video (HEVC, H.264, VP9), the common audio
# codecs, and text subtitles. Assembly stays enabled; it is what makes 4K decoding viable.
bash "$FFMPEG_SOURCE/configure" --prefix="$WORK/ffmpeg-ps5/install" \
    --enable-cross-compile --arch=x86_64 --target-os=freebsd \
    --cc="$WORK/bin/ffmpeg-ps5-cc" --ld="$WORK/bin/ffmpeg-ps5-cc" \
    --ar=llvm-ar-18 --ranlib=llvm-ranlib-18 --nm=llvm-nm-18 --x86asmexe=nasm \
    --disable-autodetect --disable-everything --disable-programs --disable-doc \
    --disable-avdevice --disable-network --disable-debug \
    --enable-decoder=hevc,h264,vp9,aac,ac3,eac3,dca,truehd,flac,opus,vorbis,mp3,pcm_s16le,subrip,ass,movtext,pgssub \
    --enable-parser=hevc,h264,vp9,aac,ac3,dca,flac,opus,vorbis,mpegaudio \
    --enable-demuxer=matroska,mov,mpegts,hevc,h264 \
    --enable-protocol=file \
    --enable-bsf=hevc_mp4toannexb,h264_mp4toannexb,extract_extradata \
    --enable-pic --enable-pthreads --disable-shared --enable-static
make -j"${JOBS:-$(nproc)}"
make install
ls -la "$WORK/ffmpeg-ps5/install/lib"
