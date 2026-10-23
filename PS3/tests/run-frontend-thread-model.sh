#!/usr/bin/env bash
set -euo pipefail
: "${PS3DEV:?Set PS3DEV to the SDK root}"
: "${PPSSPP_PS3_RUNTIME:?Set PPSSPP_PS3_RUNTIME to the threaded runtime}"
: "${PPC64_CXX:?Set PPC64_CXX to the Linux PowerPC64 compiler wrapper}"
: "${PPC64_QEMU:?Set PPC64_QEMU to qemu-ppc64}"
: "${PPC64_SYSROOT:?Set PPC64_SYSROOT to the Linux PowerPC64 runtime root}"
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(realpath -m "${1:-$repo/../frontend-thread-model}")
mkdir -p "$out"
flags=(-std=gnu++17 -O2 -nostdinc++
	-I"$PPSSPP_PS3_RUNTIME/include/c++/7.2.0"
	-I"$PPSSPP_PS3_RUNTIME/include/c++/7.2.0/powerpc64-ps3-elf"
	-I"$repo/PS3/runtime/include" -I"$PS3DEV/ppu/include" -I"$repo")
"$PS3DEV/ppu/bin/ppu-g++" "${flags[@]}" -c "$repo/PS3/frontend/Threads.cpp" -o "$out/threads.o"
"$PS3DEV/ppu/bin/ppu-g++" "${flags[@]}" -c "$repo/PS3/tests/frontend_threads.cpp" -o "$out/test.o"
"$PPC64_CXX" "$out/test.o" "$out/threads.o" -o "$out/test"
"$PPC64_QEMU" -L "$PPC64_SYSROOT" "$out/test"
