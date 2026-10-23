set -xe

CC=gcc
CPPC=g++

BUILD_FLAGS="-fPIC -g -O2 -Wformat"

$CC $BUILD_FLAGS -c log_impl_stdc.c -o log_impl_stdc.o
$CC $BUILD_FLAGS -c test.c -o test.o
$CC $BUILD_FLAGS -c postoffice.c -o postoffice.o
$CC $BUILD_FLAGS -c sock_impl_linux.c -o sock_impl_linux.o
$CPPC $BUILD_FLAGS -c mutex_impl_cpp.cpp -o mutex_impl_cpp.o
$CPPC $BUILD_FLAGS -c delay_impl_cpp.cpp -o delay_impl_cpp.o
$CC $BUILD_FLAGS -c postoffice_mem_stdc.c -o postoffice_mem_stdc.o

$CPPC $BUILD_FLAGS log_impl_stdc.o test.o postoffice.o sock_impl_linux.o mutex_impl_cpp.o delay_impl_cpp.o postoffice_mem_stdc.o -o test.out
$CPPC $BUILD_FLAGS -shared log_impl_stdc.o postoffice.o sock_impl_linux.o mutex_impl_cpp.o delay_impl_cpp.o postoffice_mem_stdc.o -o libaemu_postoffice_client.so
