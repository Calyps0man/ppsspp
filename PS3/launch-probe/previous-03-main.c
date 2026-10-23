// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal SDK launch control: no PPSSPP, SDL, C++, or stdio.
#include <sys/process.h>
#include <sys/file.h>
#include <sys/systime.h>
SYS_PROCESS_PARAM(1001, 0x100000);
static const char *const paths[]={
    "/dev_hdd0/game/PPSCPU001/USRDIR/PPSCPU03.txt",
    "/dev_hdd0/tmp/PPSCPU03.txt",
    "/dev_usb000/PPSCPU03.txt", "/dev_usb001/PPSCPU03.txt",
    "/dev_usb002/PPSCPU03.txt", "/dev_usb003/PPSCPU03.txt",
    "/dev_usb004/PPSCPU03.txt", "/dev_usb005/PPSCPU03.txt",
    "/dev_usb006/PPSCPU03.txt", "/dev_usb007/PPSCPU03.txt"
};
static unsigned log_line(const char *line,unsigned length,int reset) {
    unsigned successes=0;
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i) {
        s32 fd=-1;
        s32 flags=SYS_O_WRONLY|SYS_O_CREAT|(reset?SYS_O_TRUNC:SYS_O_APPEND);
        if(sysLv2FsOpen(paths[i],flags,&fd,0666,0,0)==0) {
            u64 written=0;
            if(sysLv2FsWrite(fd,line,length,&written)==0 && written==length) ++successes;
            sysLv2FsFsync(fd); sysLv2FsClose(fd);
        }
    }
    return successes;
}
// Runs before the SDK heap, stdio, and ordinary user constructors.
static void early(void) __attribute__((constructor(101)));
static void early(void) {
    static const char line[]="PPSSPP PS3 launch probe 03: early constructor reached\n";
    log_line(line,sizeof(line)-1,1);
}
int main(void) {
    static const char entered[]="main reached; direct LV2 filesystem writes\n";
    unsigned saved=log_line(entered,sizeof(entered)-1,0);
    volatile unsigned a=0x12345678u,b=3u;
    static const char pass[]="Simple PPU arithmetic PASS; this is not an emulator test\n";
    static const char fail[]="Simple PPU arithmetic FAIL\n";
    if(a*b==0x369d0368u) log_line(pass,sizeof(pass)-1,0);
    else log_line(fail,sizeof(fail)-1,0);
    static const char exitline[]="Returning to XMB intentionally after 3 seconds\n";
    log_line(exitline,sizeof(exitline)-1,0);
    sysUsleep(3000000);
    sysProcessExit(saved?0:3);
    return 0;
}
