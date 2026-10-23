// Copyright (c) 2026 PPSSPP PS3 portability work.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sys/systime.h>
#include <malloc.h>
#include "font-mask.h"
#include <sys/process.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <sys/file.h>
SYS_PROCESS_PARAM(1001, 0x100000);
int RunIRTests(int *checksOut, int *failuresOut);
static bool running = true;
static const char *const logPaths[]={
    "/dev_hdd0/game/PPSCPU001/USRDIR/PPSCPU06.txt",
    "/dev_hdd0/tmp/PPSCPU06.txt",
    "/dev_usb000/PPSCPU06.txt", "/dev_usb001/PPSCPU06.txt",
    "/dev_usb002/PPSCPU06.txt", "/dev_usb003/PPSCPU06.txt",
    "/dev_usb004/PPSCPU06.txt", "/dev_usb005/PPSCPU06.txt",
    "/dev_usb006/PPSCPU06.txt", "/dev_usb007/PPSCPU06.txt"
};
static void rawTrace(const char *line,unsigned length,bool reset=false) {
    for(const char *path:logPaths) {
        s32 fd=-1;
        if(sysLv2FsOpen(path,SYS_O_WRONLY|SYS_O_CREAT|(reset?SYS_O_TRUNC:SYS_O_APPEND),&fd,0666,nullptr,0)==0) {
            u64 written=0; sysLv2FsWrite(fd,line,length,&written);
            sysLv2FsFsync(fd); sysLv2FsClose(fd);
        }
    }
}
static void earlyTrace() __attribute__((constructor(101)));
static void earlyTrace() {
    static const char line[]="PPSSPP PS3 CPU Test 06: early constructor reached\n";
    rawTrace(line,sizeof(line)-1,true);
}
static void runtimeTrace() __attribute__((constructor(106)));
static void runtimeTrace() {
    static const char line[]="SDK heap/syscall/libc constructors completed\n";
    rawTrace(line,sizeof(line)-1);
}
static void trace(const char *format,...) {
    char line[512]; va_list args; va_start(args,format);
    ::vsnprintf(line,sizeof(line)-1,format,args); va_end(args);
    unsigned n=0; while(line[n] && n<sizeof(line)-2) ++n;
    line[n++]='\n'; rawTrace(line,n);
}
static void systemCallback(u64 status,u64,void *) {
    if(status==SYSUTIL_EXIT_GAME) running=false;
}
static void rect(u32 *pixels,int width,int height,int x,int y,int w,int h,u32 color) {
    int left=x*width/1280, right=(x+w)*width/1280;
    int top=y*height/720, bottom=(y+h)*height/720;
    if(left<0) left=0; if(top<0) top=0;
    if(right>width) right=width; if(bottom>height) bottom=height;
    for(int row=top;row<bottom;++row)
        for(int col=left;col<right;++col) pixels[row*width+col]=color;
}
static void text(u32 *pixels,int width,int height,int x,int y,const char *str,u32 color=0xFFFFFFFF) {
    for(;*str;++str,x+=24) {
        unsigned c=(unsigned char)*str; if(c<32 || c>126) c='?';
        for(int row=0;row<24;++row)
            for(int col=0;col<12;++col)
                if(fontRows[c-32][row]&(1u<<col)) rect(pixels,width,height,x+col*2,y+row*2,2,2,color);
    }
}
int main(int,char **) {
    trace("PPSSPP PS3 CPU Test 06: main entered");
    int checks=0,failures=0;
    trace("Starting actual PPSSPP IR tests before display initialization");
    int result=RunIRTests(&checks,&failures);
    trace("IR tests finished: checks=%d failures=%d result=%s",checks,failures,result==0?"PASS":"FAIL");
    FILE *log=std::fopen("/dev_hdd0/game/PPSCPU001/USRDIR/results.txt","wb");
    if(log) { std::fprintf(log,"CPU Test 06\nChecks: %d\nFailures: %d\nResult: %s\n",checks,failures,result==0?"PASS":"FAIL"); std::fclose(log); }
    void *host=memalign(1024*1024,1024*1024);
    if(!host) { trace("FAIL RSX host buffer allocation"); return 10; }
    gcmContextData *context=nullptr;
    s32 ret=rsxInit(&context,0x10000,1024*1024,host);
    trace("rsxInit: %d",ret);
    if(ret!=0 || !context) return 11;
    videoState state{}; videoResolution res{};
    ret=videoGetState(VIDEO_PRIMARY,0,&state);
    if(ret!=0) { trace("FAIL videoGetState: %d",ret); return 12; }
    ret=videoGetResolution(state.displayMode.resolution,&res);
    if(ret!=0) { trace("FAIL videoGetResolution: %d",ret); return 13; }
    videoConfiguration config{};
    config.resolution=state.displayMode.resolution;
    config.format=VIDEO_BUFFER_FORMAT_XRGB; config.aspect=VIDEO_ASPECT_AUTO;
    config.pitch=res.width*4;
    ret=videoConfigure(VIDEO_PRIMARY,&config,nullptr,1);
    trace("videoConfigure: %d resolution=%dx%d",ret,res.width,res.height);
    if(ret!=0 || res.width==0 || res.height==0) return 14;
    u32 *pixels=(u32*)rsxMemalign(64,(u32)res.height*config.pitch);
    if(!pixels) { trace("FAIL display buffer allocation"); return 15; }
    u32 offset=0;
    ret=rsxAddressToOffset(pixels,&offset);
    if(ret!=0) { trace("FAIL display buffer offset: %d",ret); return 16; }
    ret=gcmSetDisplayBuffer(0,offset,config.pitch,res.width,res.height);
    if(ret!=0) { trace("FAIL gcmSetDisplayBuffer: %d",ret); return 17; }
    for(unsigned i=0;i<(unsigned)res.width*res.height;++i) pixels[i]=0xFF101820;
    text(pixels,res.width,res.height,60,70,"PPSSPP PS3 CPU test 06");
    text(pixels,res.width,res.height,60,160,result==0?"INTERPRETER: PASS":"INTERPRETER: FAIL",result==0?0xFF70FF90:0xFFFF7070);
    char summary[80]; ::snprintf(summary,sizeof(summary),"Checks: %d    Failures: %d",checks,failures);
    text(pixels,res.width,res.height,60,240,summary);
    text(pixels,res.width,res.height,60,340,"CPU test only. No game loading yet.");
    text(pixels,res.width,res.height,60,440,"Log: PPSCPU06.txt on USB or HDD");
    text(pixels,res.width,res.height,60,540,"Circle: Exit    PS button: XMB");
    __asm__ volatile("sync" ::: "memory");
    gcmSetFlipMode(GCM_FLIP_VSYNC); gcmResetFlipStatus();
    ret=gcmSetFlip(context,0); rsxFlushBuffer(context);
    trace("Direct framebuffer drawn; gcmSetFlip: %d",ret);
    if(ret!=0) return 18;
    trace("Exit callback: %d; pad init: %d",sysUtilRegisterCallback(0,systemCallback,nullptr),ioPadInit(7));
    while(running) {
        sysUtilCheckCallback();
        for(int port=0;port<7;++port) {
            padData pad{};
            if(ioPadGetData(port,&pad)==0 && pad.len>0 && pad.BTN_CIRCLE) running=false;
        }
        sysUsleep(16000);
    }
    trace("Normal exit requested");
    sysUtilUnregisterCallback(0); ioPadEnd();
    return 0;
}
