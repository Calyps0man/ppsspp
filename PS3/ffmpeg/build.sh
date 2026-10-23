#!/usr/bin/env bash
# Builds FFmpeg for the PS3 PPU from PPSSPP's FFmpeg tree (https://github.com/hrydgard/ppsspp-ffmpeg,
# FFmpeg 3.0.2) with only what the PS3 build needs: PSP video (H.264 in PMF/MPEG-PS), MP3, AAC and PCM.
# ATRAC3/3+ use PPSSPP's own decoder; every other decoder costs main memory, of which there's little.
# Static libraries and headers go to the prefix, which the PS3 CMake build uses when PPSSPP_PS3_FFMPEG points at it (default prefix: ../ffmpeg-ps3 next to this repository).
#
#   bash PS3/ffmpeg/build.sh /path/to/ppsspp-ffmpeg [build dir] [prefix]
set -euo pipefail
: "${PS3DEV:?Set PS3DEV to the PSL1GHT SDK root}"
export PSL1GHT=${PSL1GHT:-$PS3DEV}
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
src=$(realpath "${1:?FFmpeg source (ppsspp-ffmpeg)}")
build=$(realpath -m "${2:-$repo/../build-ffmpeg-ps3}")
prefix=$(realpath -m "${3:-$repo/../ffmpeg-ps3}")
mkdir -p "$build"
cd "$build"
export PATH="$PS3DEV/ppu/bin:$PATH"

"$src/configure" \
	--prefix="$prefix" \
	--enable-cross-compile --target-os=none --arch=ppc64 --cpu=cell \
	--cross-prefix=ppu- --cc=ppu-gcc --nm=ppu-nm --ar=ppu-ar --ranlib=ppu-ranlib \
	--extra-cflags="-O3 -mcpu=cell -I$PS3DEV/ppu/include -I$PS3DEV/portlibs/ppu/include -ffunction-sections -fdata-sections -fno-strict-aliasing -Wno-psabi" \
	--extra-ldflags="-L$PS3DEV/ppu/lib -L$PS3DEV/portlibs/ppu/lib" \
	--extra-libs="-lrt -llv2 -lm" \
	--disable-shared --enable-static \
	--disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-postproc \
	--disable-network --disable-pthreads --disable-w32threads --disable-os2threads \
	--disable-iconv --disable-lzma --disable-bzlib --enable-zlib \
	--disable-hwaccels --disable-vaapi --disable-vdpau --disable-debug \
	--disable-everything \
	--enable-decoder=h264 --enable-decoder=aac --enable-decoder=aac_latm --enable-decoder=mp3 \
	--enable-decoder=pcm_s16le --enable-decoder=pcm_s8 \
	--enable-demuxer=mpegps --enable-demuxer=h264 --enable-demuxer=mp3 --enable-demuxer=aac \
	--enable-demuxer=oma --enable-demuxer=wav --enable-demuxer=pcm_s16le --enable-demuxer=pcm_s8 \
	--enable-parser=h264 --enable-parser=aac --enable-parser=aac_latm --enable-parser=mpegaudio \
	--enable-protocol=file \
	--enable-swscale --enable-swresample
make -j"${JOBS:-4}"
make install
echo "FFmpeg for PS3 installed in $prefix"
