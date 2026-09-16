#!/usr/bin/env bash
set -eu
# MSVC environment is supplied by build.ps1; use Git for Windows' Unix tools.
export PATH="/usr/bin:$PATH"
export PATH="$(cygpath -u "$RECORDER_FFMPEG_CC_DIR"):$PATH"
source_dir=$(cygpath -u "$1")
make_program=$(cygpath -u "$2")
log_file=$(cygpath -u "$3")
exec > "$log_file" 2>&1
cd "$source_dir"
./configure \
    --toolchain=msvc --arch=x86_64 \
    --disable-autodetect --disable-everything \
    --disable-x86asm --disable-inline-asm \
    --disable-network --disable-doc --disable-debug \
    --disable-ffplay --disable-ffprobe --disable-avdevice \
    --enable-ffmpeg \
    --enable-protocol=file,pipe \
    --enable-demuxer=rawvideo,wav,matroska \
    --enable-muxer=matroska,rawvideo,pcm_f32le,wav \
    --enable-decoder=rawvideo,ffv1,pcm_f32le \
    --enable-encoder=ffv1,rawvideo,pcm_f32le \
    --enable-filter=buffer,buffersink,abuffer,abuffersink,format,aformat,null,anull,scale,aresample
"$make_program" -r -j "${NUMBER_OF_PROCESSORS:-4}" ffmpeg.exe
