#!/usr/bin/env bash
set -euo pipefail
: "${PS3DEV:?Set PS3DEV to the complete ps3dev SDK root}"
: "${PSL1GHT:=$PS3DEV}"
export PS3DEV PSL1GHT
source_dir=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out=${1:-./ps3-launch-probe}
mkdir -p "$out/pkg/USRDIR"; out=$(realpath "$out")
"$PS3DEV/ppu/bin/ppu-gcc" -std=gnu11 -O2 -Wall -Wextra -Werror -I"$PSL1GHT/ppu/include" \
 "$source_dir/PS3/launch-probe/previous-03-main.c" -L"$PSL1GHT/ppu/lib" \
 -T "$PSL1GHT/ppu/powerpc64-ps3-elf/lib/lv2.ld" -lrt -llv2 \
 -o "$out/launch-probe.elf"
"$PS3DEV/bin/fself" -n "$out/launch-probe.elf" "$out/pkg/USRDIR/EBOOT.BIN"
"$PS3DEV/bin/sfo" --fromxml "$PS3DEV/bin/sfo.xml" --title='PPSSPP PS3 Launch Probe 03' --appid=PPSCPU001 "$out/pkg/PARAM.SFO"
cp "$source_dir/LICENSE.TXT" "$out/pkg/USRDIR/LICENSE.TXT"
cp "$source_dir/PS3/cpu-test/THIRD_PARTY.txt" "$out/pkg/USRDIR/THIRD_PARTY.txt"
"$PS3DEV/bin/pkg" --contentid=UP0001-PPSCPU001_00-PPSSPPLAUNCH00003 "$out/pkg" "$out/PPSSPP_PS3_Launch_Probe_03.pkg"
