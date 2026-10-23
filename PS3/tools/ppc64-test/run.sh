#!/bin/bash
# Runs pspautotests through the PPC JIT on 64-bit PowerPC Linux under qemu, and compares each
# test's output with the IR interpreter's and with the expected output.
#
# Build once (Debian/Ubuntu: apt install g++-powerpc64-linux-gnu qemu-user-static):
#   cmake -S . -B ../build-ppc64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=PS3/tools/ppc64-test/toolchain.cmake \
#     -DHEADLESS=ON -DHEADLESS_CROSS=ON -DUSE_FFMPEG=OFF -DPPC_JIT_LINUX_TEST=ON
#   cmake --build ../build-ppc64 --target PPSSPPHeadless
# Tests: git clone https://github.com/hrydgard/pspautotests (prebuilt .prx files are included).
#
# usage: B=../build-ppc64/PPSSPPHeadless T=/path/to/pspautotests/tests R=results \
#          bash PS3/tools/ppc64-test/run.sh [list] [outfile]
# Each output line: SAME/DIFF (JIT vs interpreter), interpreter exit code/ok|bad (vs expected),
# JIT exit code, test name. PPC_VMX=1 enables the AltiVec VFPU code, PPC_TIER=3 compiles right away.
HERE="$(cd "$(dirname "$0")" && pwd)"
LIST=${1:-$HERE/tests.list}
OUT=${2:-/dev/stdout}
: "${B:?Set B to the ppc64 PPSSPPHeadless}" "${T:?Set T to pspautotests/tests}"
R=${R:-results}
mkdir -p "$R"
export QEMU_LD_PREFIX=${QEMU_LD_PREFIX:-/usr/powerpc64-linux-gnu}
while read -r t; do
  [ -z "$t" ] && continue
  name=$(echo "$t" | sed 's|/|_|g')
  timeout 20 qemu-ppc64-static "$B" --ir --graphics=software "$T/$t.prx" > "$R/$name.ir" 2>/dev/null; irc=$?
  timeout 20 qemu-ppc64-static "$B" --jit-ir --graphics=software "$T/$t.prx" > "$R/$name.jit" 2>"$R/$name.jiterr"; jrc=$?
  exp=$(diff -q <(tr -d '\r' < "$T/$t.expected") "$R/$name.ir" >/dev/null && echo ok || echo bad)
  same=$(cmp -s "$R/$name.ir" "$R/$name.jit" && echo SAME || echo DIFF)
  echo "$same ir=$irc/$exp jit=$jrc $name"
done < "$LIST" > "$OUT"
