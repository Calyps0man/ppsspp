// Copyright (c) 2026 PPSSPP PS3 portability work.
// SPDX-License-Identifier: GPL-2.0-or-later
#define SDL_MAIN_HANDLED
#include <SDL.h>
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
    "/dev_hdd0/game/PPSCPU001/USRDIR/PPSCPU07.txt",
    "/dev_hdd0/tmp/PPSCPU07.txt",
    "/dev_usb000/PPSCPU07.txt", "/dev_usb001/PPSCPU07.txt",
    "/dev_usb002/PPSCPU07.txt", "/dev_usb003/PPSCPU07.txt",
    "/dev_usb004/PPSCPU07.txt", "/dev_usb005/PPSCPU07.txt",
    "/dev_usb006/PPSCPU07.txt", "/dev_usb007/PPSCPU07.txt"
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
    static const char line[]="PPSSPP PS3 CPU Test 07: early constructor reached\n";
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
static int fail(int code,const char *stage) {
    trace("FAIL stage=%s exit=%d SDL error=%s",stage,code,SDL_GetError()); return code;
}
static void systemCallback(u64 status,u64,void *) {
    if(status==SYSUTIL_EXIT_GAME) running=false;
}
static void text(SDL_Renderer *renderer,SDL_Texture *font,int x,int y,const char *str,int scale=2) {
    for(;*str;++str,x+=12*scale) {
        unsigned c=(unsigned char)*str; if(c<32 || c>126) c='?'; c-=32;
        SDL_Rect source={(int)(c%16)*12,(int)(c/16)*24,12,24};
        SDL_Rect dest={x,y,12*scale,24*scale};
        SDL_RenderCopy(renderer,font,&source,&dest);
    }
}
int main(int,char **) {
    const char *base="/dev_hdd0/game/PPSCPU001/USRDIR/";
    trace("PPSSPP PS3 CPU Test 07: main entered (Test 05 SDL baseline, font fix only)");
    int checks=0,failures=0;
    trace("Starting actual PPSSPP IR tests before display initialization");
    int result=RunIRTests(&checks,&failures);
    trace("IR tests finished: checks=%d failures=%d result=%s",checks,failures,result==0?"PASS":"FAIL");
    char resultPath[160]; ::snprintf(resultPath,sizeof(resultPath),"%sresults.txt",base);
    FILE *log=std::fopen(resultPath,"wb"); bool saved=false;
    if(log) {
        std::fprintf(log,"PPSSPP PS3 CPU test 07\nChecks: %d\nFailures: %d\nResult: %s\nActual PPSSPP IR interpreter; isolated test harness.\n",checks,failures,result==0?"PASS":"FAIL");
        saved=std::fclose(log)==0;
    }
    SDL_SetMainReady(); trace("Starting SDL_Init VIDEO only (native pad input)");
    if(SDL_Init(SDL_INIT_VIDEO)!=0) return fail(10,"SDL_Init");
    trace("SDL_Init succeeded video=%s",SDL_GetCurrentVideoDriver());
    for(int i=0;i<SDL_GetNumRenderDrivers();++i) {
        SDL_RendererInfo info{};
        if(SDL_GetRenderDriverInfo(i,&info)==0) trace("Renderer %d: %s flags=%u",i,info.name,info.flags);
    }
    trace("Creating window");
    SDL_Window *window=SDL_CreateWindow("PPSSPP PS3 CPU test 07",SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,1280,720,SDL_WINDOW_SHOWN);
    if(!window) return fail(11,"SDL_CreateWindow");
    trace("Window created; creating renderer");
    SDL_Renderer *renderer=SDL_CreateRenderer(window,-1,0);
    if(!renderer) return fail(12,"SDL_CreateRenderer");
    trace("Renderer created"); SDL_RenderSetLogicalSize(renderer,1280,720);
    char path[160]; ::snprintf(path,sizeof(path),"%sfont.bmp",base);
    trace("Loading font: %s",path);
    SDL_Surface *surface=SDL_LoadBMP(path);
    if(!surface) return fail(13,"SDL_LoadBMP");
    trace("Font loaded; creating texture");
    SDL_SetColorKey(surface,SDL_TRUE,SDL_MapRGB(surface->format,0,0,0));
    SDL_Texture *font=SDL_CreateTextureFromSurface(renderer,surface); SDL_FreeSurface(surface);
    if(!font) return fail(14,"SDL_CreateTextureFromSurface");
    trace("Font texture created");
    int callbackResult=sysUtilRegisterCallback(1,systemCallback,nullptr);
    trace("Exit callback registration: %d; pad init: %d",callbackResult,ioPadInit(7));
    SDL_SetRenderDrawColor(renderer,12,18,28,255); SDL_RenderClear(renderer);
    text(renderer,font,60,70,"PPSSPP PS3 CPU test 07");
    text(renderer,font,60,160,"Interpreter checks completed."); SDL_RenderPresent(renderer);
    trace("First frame presented");
    char summary[80]; ::snprintf(summary,sizeof(summary),"Checks: %d    Failures: %d",checks,failures);
    bool loggedFrame=false;
    while(running) {
        SDL_Event event;
        while(SDL_PollEvent(&event)) {
            if(event.type==SDL_QUIT) running=false;
            if(event.type==SDL_CONTROLLERBUTTONDOWN && event.cbutton.button==SDL_CONTROLLER_BUTTON_B) running=false;
        }
        for(int port=0;port<7;++port) {
            padData pad{};
            if(ioPadGetData(port,&pad)==0 && pad.len>0 && pad.BTN_CIRCLE) running=false;
        }
        sysUtilCheckCallback();
        SDL_SetRenderDrawColor(renderer,12,18,28,255); SDL_RenderClear(renderer);
        text(renderer,font,60,70,"PPSSPP PS3 CPU test 07");
        SDL_SetTextureColorMod(font,result==0?90:255,result==0?240:90,140);
        text(renderer,font,60,160,result==0?"INTERPRETER: PASS":"INTERPRETER: FAIL");
        SDL_SetTextureColorMod(font,255,255,255);
        text(renderer,font,60,240,summary);
        text(renderer,font,60,340,"CPU bring-up only.");
        text(renderer,font,60,395,"No game loading in this build.");
        text(renderer,font,60,490,saved?"Result saved to USRDIR/results.txt":"Result file could not be written.");
        text(renderer,font,60,590,"Circle: Exit    PS button: XMB");
        SDL_RenderPresent(renderer);
        if(!loggedFrame) { trace("Result frame presented; waiting for Circle or XMB exit"); loggedFrame=true; }
        SDL_Delay(16);
    }
    trace("Normal exit requested"); sysUtilUnregisterCallback(1); ioPadEnd();
    SDL_DestroyTexture(font); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
