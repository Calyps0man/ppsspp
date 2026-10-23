set -xe
GCC=x86_64-w64-mingw32-gcc
GPP=x86_64-w64-mingw32-g++

$GCC -fPIC -g -c log_impl_stdc.c -o log_impl_stdc.o -O0
$GCC -fPIC -g -c test.c -o test.o -O0
$GCC -fPIC -g -c postoffice.c -o postoffice.o -O2
$GCC -fPIC -g -c sock_impl_windows.c -o sock_impl_windows.o -O2
$GPP -fPIC -g -c mutex_impl_cpp.cpp -o mutex_impl_cpp.o -O2
$GPP -fPIC -g -c delay_impl_cpp.cpp -o delay_impl_cpp.o -O2
$GCC -fPIC -g -c postoffice_mem_stdc.c -o postoffice_mem_stdc.o -O2

$GPP -O0 -static log_impl_stdc.o test.o postoffice.o sock_impl_windows.o mutex_impl_cpp.o delay_impl_cpp.o postoffice_mem_stdc.o -lws2_32 -lpthread -o test.exe
$GPP -fPIC -shared -static log_impl_stdc.o postoffice.o sock_impl_windows.o mutex_impl_cpp.o delay_impl_cpp.o postoffice_mem_stdc.o -lws2_32 -o libaemu_postoffice_client.dll
