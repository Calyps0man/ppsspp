#!/usr/bin/env bash
set -euo pipefail
# The PS3 ELFv1 objects use Linux libc only to run the pure crypto tests under QEMU.
# This is native compiler/code validation, not a PS3 runtime or game boot test.
: "${PS3DEV:?Set PS3DEV to the SDK root}"
: "${PPSSPP_PS3_RUNTIME:?Set PPSSPP_PS3_RUNTIME to the threaded runtime}"
: "${PPC64_CXX:?Set PPC64_CXX to the Linux PowerPC64 compiler wrapper}"
: "${PPC64_QEMU:?Set PPC64_QEMU to qemu-ppc64}"
: "${PPC64_SYSROOT:?Set PPC64_SYSROOT to the Linux PowerPC64 runtime root}"
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(realpath "${1:?Pass the completed native CMake build directory}")
out=$(realpath -m "${2:-$repo/../prx-decryption-test}")
mkdir -p "$out"
flags=(-std=gnu++17 -O2 -nostdinc++
	-I"$PPSSPP_PS3_RUNTIME/include/c++/7.2.0"
	-I"$PPSSPP_PS3_RUNTIME/include/c++/7.2.0/powerpc64-ps3-elf"
	-I"$repo/PS3/runtime/include" -I"$PS3DEV/ppu/include" -I"$repo")
"$PS3DEV/ppu/bin/ppu-g++" "${flags[@]}" -c "$repo/PS3/tests/prx_decryption.cpp" -o "$out/test.o"
"$PPC64_CXX" "$out/test.o" "$build_dir/Core/CMakeFiles/Core.dir/ELF/PrxDecrypter.cpp.obj" \
	"$build_dir/lib/libkirk.a" -o "$out/test"
"$PPC64_QEMU" -L "$PPC64_SYSROOT" "$out/test" \
	"$repo/PS3/tests/prx-fixtures/expected.bin" \
	"$repo/PS3/tests/prx-fixtures/type0.prx" \
	"$repo/PS3/tests/prx-fixtures/type2.prx"
