#!/usr/bin/env bash
# Builds the standalone RSX Diagnostics PKG (title RSXD00001).
set -euo pipefail
: "${PS3DEV:?Set PS3DEV to the PSL1GHT SDK root}"
: "${PPSSPP_PS3_RUNTIME:?Set PPSSPP_PS3_RUNTIME to the rebuilt threaded C++ runtime}"
export PS3DEV PPSSPP_PS3_RUNTIME
export PSL1GHT=${PSL1GHT:-$PS3DEV}
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(realpath -m "${1:-$repo/../build-ps3}")
output_dir=$(realpath -m "${2:-$repo/../rsxdiag-artifacts}")
cmake_bin=${CMAKE:-cmake}
"$cmake_bin" -S "$repo" -B "$build_dir" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$repo/PS3/psl1ght.cmake" \
	-DCMAKE_MAKE_PROGRAM="${NINJA:-ninja}" -DCMAKE_BUILD_TYPE=Release \
	-DHEADLESS=ON -DHEADLESS_CROSS=ON -DPS3_FRONTEND=ON
"$cmake_bin" --build "$build_dir" --target RSXDiag --parallel "${JOBS:-4}"
mkdir -p "$output_dir/pkg/USRDIR"
python3 "$repo/PS3/frontend/make-assets.py" "$output_dir/pkg" "RSX" "DIAGNOSTICS"
"$PS3DEV/ppu/bin/ppu-strip" "$build_dir/RSXDiag" -o "$output_dir/RSXDiag.elf"
"$PS3DEV/bin/sprxlinker" "$output_dir/RSXDiag.elf"
"$PS3DEV/bin/fself" -n "$output_dir/RSXDiag.elf" "$output_dir/pkg/USRDIR/EBOOT.BIN"
"$PS3DEV/bin/sfo" --fromxml "$PS3DEV/bin/sfo.xml" --title='RSX Diagnostics' --appid=RSXD00001 "$output_dir/pkg/PARAM.SFO"
cp "$repo/LICENSE.TXT" "$output_dir/pkg/USRDIR/LICENSE.TXT"
cp "$repo/PS3/diag/README.md" "$output_dir/pkg/USRDIR/README.txt"
git -C "$repo" rev-parse HEAD > "$output_dir/pkg/USRDIR/SOURCE_COMMIT.txt"
"$PS3DEV/bin/pkg" --contentid=UP0001-RSXD00001_00-RSXDIAGNOSTICS15 "$output_dir/pkg" "$output_dir/RSX_Diagnostics_1.5.pkg"
printf 'RSX Diagnostics PKG: %s\n' "$output_dir/RSX_Diagnostics_1.5.pkg"
