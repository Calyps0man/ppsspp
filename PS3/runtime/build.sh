#!/usr/bin/env bash
set -euo pipefail
: "${PS3DEV:?Set PS3DEV to the SDK root}"
sdk=$(realpath "$PS3DEV")
source_root=$(realpath "${1:?Fresh GCC 7.2.0 source directory}")
build_root=$(realpath -m "${2:?Runtime build directory}")
prefix=$(realpath -m "${3:?Runtime installation directory}")
runtime_dir=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$build_root"
cd "$build_root"
export PATH="$sdk/ppu/bin:$PATH"
export CC="$sdk/ppu/bin/ppu-gcc"
export CXX="$sdk/ppu/bin/ppu-g++"
export AR="$sdk/ppu/bin/ppu-ar"
export RANLIB="$sdk/ppu/bin/ppu-ranlib"
export NM="$sdk/ppu/bin/ppu-nm"
export CPPFLAGS="-I$runtime_dir/include -I$sdk/ppu/include -D_GLIBCXX_GTHREAD_USE_WEAK=0"
export CFLAGS="-O2"
export CXXFLAGS="-O2"
"$source_root/libstdc++-v3/configure" \
	--host=powerpc64-ps3-elf --target=powerpc64-ps3-elf \
	--prefix="$prefix" --with-newlib --disable-multilib \
	--disable-shared --enable-static --disable-libstdcxx-pch \
	--enable-libstdcxx-threads --disable-nls
make -j"${JOBS:-4}"
if [[ ${PPSSPP_RUNTIME_INSTALL:-1} == 1 ]]; then
	make install
fi
