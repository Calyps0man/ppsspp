#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
for test in memory_arena memory_pages timing sockets dns; do
	case "$test" in
		memory_arena) source=MemArenaPS3.cpp ;;
		memory_pages) source=MemoryUtilPS3.cpp ;;
		timing) source=TimeUtil.cpp ;;
		sockets) source=File/FileDescriptor.cpp ;;
		dns) source=Net/ResolvePS3.cpp ;;
	esac
	g++ -std=c++17 -ffunction-sections -fdata-sections -Wl,--gc-sections -Wall -Wextra -Wno-unused-parameter -D__PPU__ \
		-I"$repo/PS3/tests/mocks" -I"$repo" \
		"$repo/PS3/tests/$test.cpp" "$repo/Common/$source" -o "$build_dir/$test"
	"$build_dir/$test"
done
g++ -std=c++17 -Wall -Wextra -I"$repo" \
	"$repo/PS3/tests/timezone.cpp" -o "$build_dir/timezone"
"$build_dir/timezone"
g++ -std=c++17 -Wall -Wextra -D__PPU__ -I"$repo/PS3/tests/mocks" -I"$repo" \
	"$repo/PS3/tests/socket_select.cpp" -o "$build_dir/socket_select"
"$build_dir/socket_select"
python3 "$repo/PS3/prepare-postoffice.py" "$repo/ext/aemu_postoffice/client" "$build_dir/postoffice/client"
g++ -std=c++17 -Wall -Wextra -D__PPU__ -I"$repo/PS3/tests/mocks" -I"$build_dir/postoffice/client" \
	"$repo/PS3/tests/postoffice.cpp" -x c++ "$repo/PS3/PostofficeSocket.c" -o "$build_dir/postoffice_test"
"$build_dir/postoffice_test"
g++ -std=c++17 -Wall -Wextra -I"$repo/PS3/tests/mocks" -I"$repo" \
	"$repo/PS3/tests/socket_send.cpp" -o "$build_dir/socket_send"
"$build_dir/socket_send"
g++ -std=c++17 -Wall -Wextra -pthread -I"$repo" \
	"$repo/PS3/tests/positioned_read.cpp" -o "$build_dir/positioned_read"
"$build_dir/positioned_read"
gcc -std=gnu11 -DHAVE_STDBOOL_H -I"$repo/PS3/tests/mocks" -I"$repo" \
	-c "$repo/PS3/ZipRandom.c" -o "$build_dir/zip_random.o"
g++ -std=c++17 -Wall -Wextra -I"$repo" \
	"$repo/PS3/tests/zip_random.cpp" "$build_dir/zip_random.o" -o "$build_dir/zip_random"
"$build_dir/zip_random"
