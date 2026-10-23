source /etc/profile.d/pspsdk.sh

set -xe

PSPDEV=${PSPDEV:-/usr/local/pspdev}

gcc_build_args="-Os -fno-builtin -G0 -Wall -fno-pic -I$PSPDEV/psp/sdk/include -D_PSP_FW_VERSION=600"
gcc_prx_args="-L$PSPDEV/psp/sdk/lib -specs=$PSPDEV/psp/sdk/lib/prxspecs -Wl,-q,-T$PSPDEV/psp/sdk/lib/linkfile.prx -nostartfiles -Wl,-zmax-page-size=128"
gcc_prx_libs="-nostdlib -lpspuser -lpspsdk -lpspmodinfo -lpspnet_inet"

psp-gcc $gcc_build_args -c log_impl_psp.c -o log_impl_psp.o

psp-gcc $gcc_build_args -c sock_impl_psp.c -o sock_impl_psp.o
psp-gcc $gcc_build_args -c mutex_impl_psp.c -o mutex_impl_psp.o
psp-gcc $gcc_build_args -c delay_impl_psp.c -o delay_impl_psp.o

psp-gcc $gcc_build_args -c postoffice.c -o postoffice.o
psp-gcc $gcc_build_args -c psp_main.c -o psp_main.o
psp-gcc $gcc_build_args -c postoffice_mem_psp.c -o postoffice_mem_psp.o
psp-gcc $gcc_build_args -c ATPRO.S -o ATPRO.o
psp-build-exports -b postoffice_client.exp > exports.c
psp-gcc $gcc_build_args -c exports.c -o exports.o
rm exports.c

psp-gcc $gcc_prx_args psp_main.o log_impl_psp.o sock_impl_psp.o mutex_impl_psp.o delay_impl_psp.o postoffice.o postoffice_mem_psp.o ATPRO.o exports.o -o postoffice.elf $gcc_prx_libs

psp-fixup-imports postoffice.elf
psp-prxgen postoffice.elf aemu_postoffice.prx
