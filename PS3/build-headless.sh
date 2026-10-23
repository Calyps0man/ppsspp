#!/usr/bin/env bash
set -euo pipefail
: "${PS3DEV:?Set PS3DEV to the PSL1GHT SDK root}"
: "${PPSSPP_PS3_RUNTIME:?Set PPSSPP_PS3_RUNTIME to the rebuilt threaded C++ runtime}"
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(realpath -m "${1:-$repo/../build-ps3}")
output_dir=$(realpath -m "${2:-$build_dir/native}")
cmake_bin=${CMAKE:-cmake}
"$cmake_bin" -S "$repo" -B "$build_dir" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$repo/PS3/psl1ght.cmake" \
	-DCMAKE_MAKE_PROGRAM="${NINJA:-ninja}" -DCMAKE_BUILD_TYPE=Release \
	-DHEADLESS=ON -DHEADLESS_CROSS=ON
"$cmake_bin" --build "$build_dir" --target PPSSPPHeadless --parallel "${JOBS:-4}"
mkdir -p "$output_dir"
cp "$build_dir/PPSSPPHeadless" "$output_dir/PPSSPPHeadless.elf"
"$PS3DEV/bin/sprxlinker" "$output_dir/PPSSPPHeadless.elf"
python3 "$repo/PS3/cpu-test/verify-elf.py" "$output_dir/PPSSPPHeadless.elf"
printf 'Native headless ELF: %s\n' "$output_dir/PPSSPPHeadless.elf"
