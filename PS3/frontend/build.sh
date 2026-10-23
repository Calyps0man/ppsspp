#!/usr/bin/env bash
set -euo pipefail
: "${PS3DEV:?Set PS3DEV to the PSL1GHT SDK root}"
: "${PPSSPP_PS3_RUNTIME:?Set PPSSPP_PS3_RUNTIME to the rebuilt threaded C++ runtime}"
export PS3DEV PPSSPP_PS3_RUNTIME
export PSL1GHT=${PSL1GHT:-$PS3DEV}
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=$(realpath -m "${1:-$repo/../build-ps3}")
output_dir=$(realpath -m "${2:-$repo/../ps3-frontend-artifacts}")
cmake_bin=${CMAKE:-cmake}
# The frontend version: window title, PKG name, XMB title and icon.
version=270
release=0.1
# Install folder and name: PS3_APP_ID=PPSSPP002 PS3_EDITION=diag makes a build that installs next to
# the normal one (its own folder, settings, game configs, saves and logs).
app_id=${PS3_APP_ID:-PPSSPP001}
edition=${PS3_EDITION:-}
edition_text=${edition:+ $edition}
"$cmake_bin" -S "$repo" -B "$build_dir" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$repo/PS3/psl1ght.cmake" \
	-DCMAKE_MAKE_PROGRAM="${NINJA:-ninja}" -DCMAKE_BUILD_TYPE=Release \
	-DHEADLESS=ON -DHEADLESS_CROSS=ON -DPS3_FRONTEND=ON \
	-DPS3_APP_ID="$app_id" -DPS3_EDITION="$edition_text"
"$cmake_bin" --build "$build_dir" --target PPSSPPPS3 --parallel "${JOBS:-4}"
mkdir -p "$output_dir/pkg/USRDIR/memstick/PSP"
python3 "$repo/PS3/frontend/make-assets.py" "$output_dir/pkg" "PPSSPP PS3$edition_text" "ver $release (build $version$edition_text)"
cp -a "$repo/assets" "$output_dir/pkg/USRDIR/"
"$PS3DEV/ppu/bin/ppu-strip" "$build_dir/PPSSPPPS3" -o "$output_dir/PPSSPPPS3.elf"
"$PS3DEV/bin/sprxlinker" "$output_dir/PPSSPPPS3.elf"
python3 "$repo/PS3/cpu-test/verify-elf.py" "$output_dir/PPSSPPPS3.elf"
# Signing: PS3_SIGN=hen makes the package for official firmware with HEN (retail-signed), the
# default the one for custom firmware (fake-signed, as always). Both install to the same
# folder; the HEN one has " HEN" after the build number (PS3_EDITION=HEN).
if [ "${PS3_SIGN:-cfw}" = hen ]; then
	# Content ID: 36 characters, the last part 16 (the CFW package keeps PPSSPPPS3FRONT<build>, 17
	# characters from build 100 on). The same ID goes into the SELF's NPDRM header and the PKG.
	content_id="UP0001-${app_id}_00-PPSSPPPS3FR$(printf '%05d' "$version")"
	# HEN package (build 265 on): EBOOT.BIN is a retail-signed NPDRM SELF (free license), as PSL1GHT's own "make pkg"
	# makes it, so it also starts on official firmware with HEN (4.89). The fake-signed one (fself -n,
	# key revision 0x8000) only started on custom firmware; HEN refused it ("An error occurred during
	# the start operation", 80010009).
	"$PS3DEV/bin/make_self_npdrm" "$output_dir/PPSSPPPS3.elf" "$output_dir/pkg/USRDIR/EBOOT.BIN" "$content_id"
	# "Quit to game list" restarts the app with sysProcessExitSpawn2, which can't start an NPDRM SELF
	# but can start a plain one: RELOAD.SELF, retail-signed too. Both are compressed (~10 MB each,
	# the fake-signed EBOOT.BIN was 32 MB). RELOAD.BLD tells the app RELOAD.SELF is this build's (until
	# build 264 the app wrote RELOAD.SELF itself from RELOAD.HDR and EBOOT.BIN).
	"$PS3DEV/bin/make_self" "$output_dir/PPSSPPPS3.elf" "$output_dir/pkg/USRDIR/RELOAD.SELF"
	printf '%s' "$version" > "$output_dir/pkg/USRDIR/RELOAD.BLD"
	python3 - "$output_dir/pkg/USRDIR/EBOOT.BIN" "$output_dir/pkg/USRDIR/RELOAD.SELF" "$content_id" <<-'PYEOF'
	import struct, sys
	eboot = open(sys.argv[1], 'rb').read(0x1000)
	reload_ = open(sys.argv[2], 'rb').read(0x100)
	assert len(sys.argv[3]) == 36, 'content ID must be 36 characters'
	for name, d in (('EBOOT.BIN', eboot), ('RELOAD.SELF', reload_)):
	    assert d[:4] == b'SCE\0', name + ': not a SELF'
	    assert struct.unpack('>H', d[8:10])[0] != 0x8000, name + ': fake-signed'
	i = eboot.find(b'NPD\0')
	assert i >= 0, 'EBOOT.BIN: no NPDRM header'
	assert struct.unpack('>I', eboot[i + 8:i + 12])[0] == 3, 'EBOOT.BIN: license not free'
	assert eboot[i + 16:i + 16 + 36] == sys.argv[3].encode(), 'EBOOT.BIN: content ID differs'
	PYEOF
else
	# CFW package (as always): EBOOT.BIN fake-signed (fself -n), which custom firmware
	# starts. "Quit to game list" restarts the app with sysProcessExitSpawn2, which can't start an
	# NPDRM SELF (EBOOT.BIN) but can start a plain one. The two differ only in the header: ship the
	# plain header (RELOAD.HDR, ~2.4 KB); the app writes RELOAD.SELF from it and the rest of EBOOT.BIN
	# when needed.
	content_id="UP0001-${app_id}_00-PPSSPPPS3FRONT$version"
	"$PS3DEV/bin/fself" -n "$output_dir/PPSSPPPS3.elf" "$output_dir/pkg/USRDIR/EBOOT.BIN"
	"$PS3DEV/bin/fself" "$output_dir/PPSSPPPS3.elf" "$output_dir/plain.self"
	python3 - "$output_dir/plain.self" "$output_dir/pkg/USRDIR/EBOOT.BIN" "$output_dir/pkg/USRDIR/RELOAD.HDR" <<-'PYEOF'
	import struct, sys
	plain = open(sys.argv[1], 'rb').read()
	npdrm = open(sys.argv[2], 'rb').read()
	plain_len = struct.unpack('>Q', plain[16:24])[0]
	npdrm_len = struct.unpack('>Q', npdrm[16:24])[0]
	assert plain[plain_len:] == npdrm[npdrm_len:], 'plain and NPDRM SELF bodies differ'
	open(sys.argv[3], 'wb').write(plain[:plain_len])
	PYEOF
	rm -f "$output_dir/plain.self"
fi
"$PS3DEV/bin/sfo" --fromxml "$PS3DEV/bin/sfo.xml" --title="PPSSPP PS3 ver $release (build $version$edition_text)" --appid="$app_id" "$output_dir/pkg/PARAM.SFO"
cp "$repo/LICENSE.TXT" "$output_dir/pkg/USRDIR/LICENSE.TXT"
cp "$repo/PS3/cpu-test/THIRD_PARTY.txt" "$output_dir/pkg/USRDIR/THIRD_PARTY.txt"
cp "$repo/PS3/frontend/README.md" "$output_dir/pkg/USRDIR/README.txt"
cp "$repo/PS3/frontend/DEBUGGING.md" "$output_dir/pkg/USRDIR/DEBUGGING.txt"
mkdir -p "$output_dir/pkg/USRDIR/games"
cp "$repo/PS3/frontend/games-README.txt" "$output_dir/pkg/USRDIR/games/README.txt"
git -C "$repo" rev-parse HEAD > "$output_dir/pkg/USRDIR/SOURCE_COMMIT.txt"
pkg_name="PPSSPP_PS3_build_$version${edition:+_$edition}"
"$PS3DEV/bin/pkg" --contentid="$content_id" "$output_dir/pkg" "$output_dir/$pkg_name.pkg"
printf 'Experimental game-loading PKG: %s\n' "$output_dir/$pkg_name.pkg"
