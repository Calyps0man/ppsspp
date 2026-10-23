// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal SDK launch control: no PPSSPP, SDL, C++, or stdio.
#include <sys/process.h>
#include <sys/file.h>
#include <sys/systime.h>
#include <sys/heap.h>
SYS_PROCESS_PARAM(1001, 0x100000);
static const char *const paths[]={
    "/dev_hdd0/game/PPSCPU001/USRDIR/PPSCPU04.txt",
    "/dev_hdd0/tmp/PPSCPU04.txt",
    "/dev_usb000/PPSCPU04.txt", "/dev_usb001/PPSCPU04.txt",
    "/dev_usb002/PPSCPU04.txt", "/dev_usb003/PPSCPU04.txt",
    "/dev_usb004/PPSCPU04.txt", "/dev_usb005/PPSCPU04.txt",
    "/dev_usb006/PPSCPU04.txt", "/dev_usb007/PPSCPU04.txt"
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
    static const char line[]="PPSSPP PS3 launch probe 04: early constructor reached\n";
    log_line(line,sizeof(line)-1,1);
}

static void note(const char *line) {
    unsigned n=0; while(line[n]) ++n; log_line(line,n,0);
}
static void hexnote(const char *label,unsigned long long value) {
    char line[128]; unsigned n=0; while(label[n] && n<90) { line[n]=label[n]; ++n; }
    line[n++]=' '; line[n++]='0'; line[n++]='x';
    for(int shift=60;shift>=0;shift-=4) line[n++]="0123456789abcdef"[(value>>shift)&15];
    line[n++]='\n'; log_line(line,n,0);
}
static void stage102(void) __attribute__((constructor(102)));
static void stage103(void) __attribute__((constructor(103)));
static void stage104(void) __attribute__((constructor(104)));
static void stage105(void) __attribute__((constructor(105)));
static void stage106(void) __attribute__((constructor(106)));
static void stage102(void) { unsigned long long tls; __asm__ volatile("mr %0,13":"=r"(tls)); hexnote("Stage 102: r13/TLS register",tls); }
static void stage103(void) { note("Stage 103 marker (SDK heap ctor should precede this)\n"); }
static void stage104(void) { note("Stage 104 marker (immediately before SDK syscall setup)\n"); }
static void stage105(void) { note("Stage 105 marker (libc lock/cwd setup should precede this)\n"); }
static void stage106(void) { note("Stage 106 reached\n"); }
extern uintptr_t __real_heapInit(heap_cntrl *,void *,uintptr_t);
uintptr_t __wrap_heapInit(heap_cntrl *heap,void *start,uintptr_t size) {
    note("ENTER SDK heapInit\n");
    uintptr_t result=__real_heapInit(heap,start,size);
    hexnote("LEAVE heapInit result",result);
    return result;
}
extern void *__real_heapAllocate(heap_cntrl *,uintptr_t);
void *__wrap_heapAllocate(heap_cntrl *heap,uintptr_t size) {
    hexnote("ENTER SDK heapAllocate size",size);
    void *result=__real_heapAllocate(heap,size);
    hexnote("LEAVE heapAllocate pointer",(unsigned long long)result);
    return result;
}
extern void __real_sysSpinlockInitialize(s32 *lock);
void __wrap_sysSpinlockInitialize(s32 *lock) {
    note("ENTER SDK spinlock initialize\n");
    __real_sysSpinlockInitialize(lock);
    note("LEAVE SDK spinlock initialize\n");
}
struct _reent;
extern struct _reent *__real___getreent(void);
struct _reent *__wrap___getreent(void) {
    note("ENTER libc thread-local reentrancy state (__getreent)\n");
    struct _reent *result=__real___getreent();
    hexnote("LEAVE __getreent pointer",(unsigned long long)result);
    return result;
}
extern int __real_sys_lwmutex_create(void *,const void *);
int __wrap_sys_lwmutex_create(void *mutex,const void *attributes) {
    note("ENTER libc lightweight mutex create\n");
    int result=__real_sys_lwmutex_create(mutex,attributes);
    hexnote("LEAVE lightweight mutex create result",(unsigned)result);
    return result;
}
extern char *__real_strdup(const char *);
char *__wrap_strdup(const char *s) {
    note("ENTER SDK cwd strdup\n");
    char *result=__real_strdup(s);
    hexnote("LEAVE strdup pointer",(unsigned long long)result);
    return result;
}
void __wrap_abort(void) __attribute__((noreturn));
void __wrap_abort(void) {
    hexnote("ABORT caller",(unsigned long long)__builtin_return_address(0));
    sysProcessExit(99);
    for(;;) {}
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
