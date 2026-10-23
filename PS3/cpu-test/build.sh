#!/usr/bin/env bash
set -euo pipefail
: "${PS3DEV:?Set PS3DEV to the ps3dev SDK root}"
: "${PSL1GHT:=$PS3DEV}"
export PS3DEV PSL1GHT
source_dir=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out=${1:-./ps3-cpu-build}
mkdir -p "$out"; out=$(realpath "$out")
"$PS3DEV/ppu/bin/ppu-g++" -std=gnu++17 -O2 -DPPSSPP_PS3_TEST_HARNESS \
 -I"$source_dir" -I"$PSL1GHT/ppu/include" -I"$PSL1GHT/ppu/include/simdmath" \
 -I"$PS3DEV/portlibs/ppu/include/SDL2" -I"$PS3DEV/portlibs/ppu/include" \
 "$source_dir/PS3/cpu-test/main.cpp" "$source_dir/PS3/cpu-test/font-transfer-fix.cpp" "$source_dir/Core/MIPS/IR/IRInterpreter.cpp" \
 "$source_dir/PS3/cpu-test/ir_stubs.cpp" "$source_dir/PS3/cpu-test/ir_endian.cpp" \
 -L"$PS3DEV/portlibs/ppu/lib" -L"$PSL1GHT/ppu/lib" \
 -T "$PSL1GHT/ppu/powerpc64-ps3-elf/lib/lv2.ld" \
 -Wl,--wrap=rsxSetTransferScaleSurface -lSDL2 -lpthread -lgcm_sys -lrsx -lsysutil -lrt -lio -laudio -lsysmodule -llv2 -lm \
 -o "$out/cpu-test.elf"
python3 "$source_dir/PS3/cpu-test/make-assets.py" "$out/pkg"
# PSL1GHT requires import counts, Cell OSABI, and the native descriptor fixups.
"$PS3DEV/ppu/bin/ppu-strip" "$out/cpu-test.elf" -o "$out/cpu-test.packaged.elf"
"$PS3DEV/bin/sprxlinker" "$out/cpu-test.packaged.elf"
python3 "$source_dir/PS3/cpu-test/verify-elf.py" "$out/cpu-test.packaged.elf"
"$PS3DEV/bin/fself" -n "$out/cpu-test.packaged.elf" "$out/pkg/USRDIR/EBOOT.BIN"
"$PS3DEV/bin/sfo" --fromxml "$PS3DEV/bin/sfo.xml" --title='PPSSPP PS3 CPU Test 07' --appid=PPSCPU001 "$out/pkg/PARAM.SFO"
cp "$source_dir/LICENSE.TXT" "$out/pkg/USRDIR/LICENSE.TXT"
cp "$source_dir/PS3/cpu-test/THIRD_PARTY.txt" "$out/pkg/USRDIR/THIRD_PARTY.txt"
"$PS3DEV/bin/pkg" --contentid=UP0001-PPSCPU001_00-PPSSPPCPUTEST0007 "$out/pkg" "$out/PPSSPP_PS3_CPU_Test_07.pkg"
