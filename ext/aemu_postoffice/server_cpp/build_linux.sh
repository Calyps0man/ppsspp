set -xe

SRC="log native_socket_linux session server semaphore main"

BUILD_FLAGS="-g -O2 -fPIC --std=c++20 -Wformat"
LINK_FLAGS=""

CPPC=g++

built=""
for f in $SRC
do
	$CPPC $BUILD_FLAGS -c ${f}.cpp -o ${f}.o
	built="$built ${f}.o"
done

$CPPC $LINK_FLAGS $built -o aemu_postoffice
