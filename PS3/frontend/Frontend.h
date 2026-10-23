// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#pragma once
#include <string>
namespace PS3Frontend {

enum class ScreenMode : int {
	FIT = 0,       // Largest 480:272 rectangle that fits the output (fullscreen, aspect kept).
	STRETCH = 1,   // Fill the whole output.
	FIT_90 = 2,    // FIT at 90%, for TVs that crop the edges (overscan).
	PIXEL_2X = 3,  // Exactly 960x544, centered.
	COUNT = 4,
};

struct Settings {
	int screenMode = (int)ScreenMode::FIT;
	bool bilinear = true;
	// 0 = off, -1 = auto (skip only when behind), 1..3 = fixed frames skipped per drawn frame.
	int frameskip = -1;
	bool speedLimit = true;
	bool showFps = true;
	// 0 = off, 1 = warnings/errors, 2 = full debug (very slow; for bug reports only).
	int logLevel = 0;  // Off by default (no logs unless asked for).
	// 0 = low, 1 = medium, 2 = high (PPSSPP default) curve/spline tessellation.
	int splineQuality = 2;
	// settings.ini "geprofile=1": split emulation time into CPU vs GE display-list time. Off by
	// default because PPSSPP's stats collection disables the IR's fast syscall path.
	bool collectStats = false;
	// 0 = software rasterizer, 1 = RSX hardware renderer (experimental).
	int renderer = 1;
	// RSX renderer internal resolution multiplier (1..3).
	int resolution = 1;
	// RSX renderer: write rendered framebuffers back to PSP VRAM every frame (compatible) or only
	// when something reads them (faster).
	bool vramSyncEveryFrame = false;
	// settings.ini "cpujit": PPC JIT through PS3MAPI instead of the IR interpreter.
	bool cpuJit = true;
	// settings.ini "sound": PSP audio output.
	bool sound = true;
	// settings.ini "pspmem64": emulate a PSP-2000 (64 MB) instead of a PSP-1000 (32 MB).
	bool pspMem64 = false;
	// settings.ini "fast3dmode": 0 per-triangle software path, 1 batched draws with the CPU
	// transform, 2 batched draws transformed and lit by the RSX (RSX renderer only).
	int fast3d = 2;
	// settings.ini "gethread": decode and draw RSX transform & lighting draws on a second thread.
	bool geThread = true;
	// "geasync": with geThread, the display lists run on the second thread too, in parallel with the
	// emulated CPU (the 3D drawing then happens there, without the separate worker).
	bool geAsync = true;
	// settings.ini "vmx": the CPU JIT keeps PSP float and vector (VFPU) values in AltiVec registers.
	bool vmx = true;
	// settings.ini "spuvertex": direct T&L vertex decoding and skinning on an SPU.
	bool spuVertex = true;
	// settings.ini "vertexring": RSX vertex/index ring in main memory (1) or RSX local memory (0).
	// Read once, when the RSX renderer first draws.
	bool vertexRingMain = true;
	// settings.ini "sputexture": RSX renderer texture hashing and decoding on SPUs.
	bool spuTexture = true;
	// settings.ini "jitprofile": a 10-second sample of the busiest PSP code, 40 s into a game.
	bool jitProfile = false;
	bool jitWarmup = true;          // "jitwarmup": interpret new blocks a few times before compiling them.
	int videos = -1;                // "videos": 1 decode video cutscenes, 0 skip them, -1 auto (on).
	int videoDeblock = -1;          // "videodeblock": H.264 deblocking in videos, -1 auto (off for a movie that falls below full speed), 1 on, 0 off.
	// "skipkeep": frameskip still draws buffers drawn only now and then (menus, loading screens).
	// Experimental, off by default.
	bool frameskipKeep = false;
	// "volwipe": a system dialog (save data...) zeroes the 4 MB of volatile memory when it closes, as
	// desktop PPSSPP does. Off by default (Midnight Club L.A. needs its data kept).
	bool volatileWipe = false;
	// "latedepth": a depth buffer read as a texture gets the previous read's depth, without stopping
	// the RSX (RSXRendererOptions::lateDepth). Faster in Midnight Club L.A.
	bool lateDepth = false;
	// "keeplayouts": a buffer drawn in another layout keeps its old copy on the RSX instead of a
	// write-back (RSXRendererOptions::keepLayouts). Faster in Midnight Club L.A.
	bool keepLayouts = false;
	// "audioslow": sound while the emulator is below full speed, 0 short gaps (fades), 1 wait until
	// ~90 ms is queued again, 2 play slower (PS3Audio::SetSlowMode).
	int audioSlow = 0;
	// "sasdirect": sceSas voices are mixed (on the SPU) from the emulation thread, no SAS thread, and
	// the SPU's answer is polled (g_sasSpuPoll).
	bool sasDirect = false;
	// "discfonts": the firmware's fonts unpacked from the disc's updater and the disc's own
	// libfont.prx run with them (g_fontsFromDisc), instead of the HLE sceFont with bundled fonts.
	bool discFonts = false;
	// "nanwatch" (diagnostics, Debug tab or a per-game file; saved since build 201): with the IR
	// interpreter and Debug on, launch.log names the game code that first makes each NaN.
	int nanWatch = 0;  // 1 FPU registers, 2 also VFPU (slower)
	// "storewatch" (diagnostics, Debug tab or per-game file, IR interpreter): names the code that wrote vertex
	// colours a draw finds half-written.
	bool storeWatch = false;
	// "jitgeneric" (build 180, Debug tab or per-game file): groups of instructions the JIT leaves to the interpreter
	// (g_irNativeGenericMask): finding which part of the JIT a game's bug is in.
	int jitGeneric = 0;
	// "jitops" (build 183, per-game file): single instructions (IR op names, comma separated) the JIT
	// leaves to the interpreter: a finer search than jitgeneric.
	std::string jitOps;
	// "iohost": asynchronous file reads finish on the IO thread while the game runs (IOTIMING_HOST).
	bool ioHost = false;
	// "jitlarge": a bigger JIT cache (more blocks and IR before it is cleared and all code compiles again).
	bool jitLarge = false;
	// "gelists": display lists processed on a thread of their own while the game's code runs on (g_geAsyncLists).
	// 0 emulation thread, 1 own thread (exact GE timing), 2 own thread with run-ahead (g_geAsyncRunAhead).
	int geListThread = 0;
	// "lensflare": 0 = depth buffers read as textures aren't written back (RSXRendererOptions::skipDepthReads).
	bool lensFlare = true;
	// "cpurows": framebuffers the RSX drew into get rows the CPU wrote afterwards (RSXRendererOptions::cpuRows).
	// 0 off, 1 on (ini only: build 158's Auto froze Silent Hill Origins' boot and didn't fix its
	// loading screen).
	int cpuRows = 0;
	// PSP system settings the games read. -1: taken from the PS3's own system settings.
	int confirmButton = -1;         // "confirm": 0 Circle, 1 X (as PSP_SYSTEMPARAM_BUTTON_*).
	int language = -1;              // "language": 0-11 as PSP_SYSTEMPARAM_LANGUAGE_*.
	int dateFormat = -1;            // "dateformat": 0 YYYY-MM-DD, 1 MM-DD-YYYY, 2 DD-MM-YYYY.
	int timeFormat = -1;            // "timeformat": 0 24-hour, 1 12-hour.
	std::string nickname;           // "nickname": empty = the PS3's nickname.
	int gameSort = 0;               // "gamesort": game list order (0 name A-Z, 1 Z-A, 2 newest, 3 largest).
	// Debug tab: which groups of lines go into launch.log (with Debug on). See DEBUGGING.md.
	int logPerf = 1;                // "perflog": Perf report 0 off, 1 every 10 s, 2 every 2 s.
	bool logStall = true;           // "stalllog": STALL reports when no frame comes for 5 s.
	bool logVideo = true;           // "videolog": Video statistics once a second during a cutscene.
	bool logSave = true;            // "savelog": save dialog steps.
	bool logGeWatch = true;         // "gewatchlog": display list memory overwritten, sub-lists in VRAM.
	bool logRsx = true;             // "rsxlog": RSX renderer events.
	bool logJit = true;             // "jitlog": JIT cache clears and recompiled blocks.
	int soundDebug = 0;             // "snddebug": 0 off, 1 "Sound:" lines (clipping) every 2 s, 2 also SAS voices mixed on the PPU.
	int l2r2 = 0;                   // "l2r2": L2+R2 in game makes 0 a GE dump, 1 a text log of the next 3000 draws.
	bool dumpRam = true;            // "dumpram": a GE dump also saves PSP RAM (.ram) at the end of its frame.
	bool dumpVram = true;           // "dumpvram": a GE dump also saves what the RSX drew (.vram, RSX renderer).
	int cpuClock = 0;               // "cpuclock": emulated CPU MHz, 0 = the game's own (g_Config.iLockedCPUSpeed).
	int anisotropy = 0;             // "anisotropy": 0 off, 2, 4, 8, 16 (RSX::SetMaxAnisotropy).
	int fxaa = 0;                   // "fxaa": Edge smoothing, FXAA over the finished picture (RSX renderer): 0 Off, 1 Full, 2 Light.
	int volume = 100;               // "volume": game volume in percent (g_Config.iGameVolume).
	int ftz = -1;                   // "ftz": 1 flush denormals when the game asks (FCR31 FS), 0 never, -1 auto (off for Killzone).
	int rightStick = 0;             // "rstick": the right stick presses 1 = the face buttons, 2 = the D-pad (0 off).
	int fpsCap = 30;                // "fpscap": most game frames a second: 60 (no cap, as on a PSP), 30 or 20 (whole vblanks each).
	int videoCap = 60;              // "videocap": frame cap while a video plays: 60 none (default), 40, 30, -1 Auto (the frame cap, once the video runs slow).
	bool debugButtons = false;      // "debugbuttons": logs, and in game L3 JIT profile and L2+R2 GE dump.
	bool inGameMenu = true;         // "ingamemenu": START+SELECT in game opens the in-game options (build 212: Debug no longer needed).
	bool r3Fps = true;              // "r3fps": L3+R3 in game shows or hides the FPS overlay (R3 alone in builds 212-216).
	bool quitCombo = true;          // "quitcombo": START+SELECT (or L3+R3) held 2 s stops the game.
	int geTiming = 100;             // "getiming": GE time per vertex in percent (100, 200, 300, 400).
	int rsxTiled = 1;               // "rsxtile": render targets in tiled RSX memory (faster drawing, F56): 1 all, 2 screens only (not depth buffers), 0 none.
	// Diagnostics switches back to older RSX renderer behaviors (settings menu; see RSXRendererOptions).
	bool textureCheckFull = false;  // "texcheck"
	bool writeBackAllRows = false;  // "wbrows"
	bool rttCopyReuse = true;       // "rttreuse"
	bool tlMerge = true;            // "tlmerge": merge consecutive T&L draws.
};

const Settings &GetSettings();
void ApplyCoreSettings();

void Trace(const char *format, ...);
// Text written to launch.log as is, in one write (many lines at once: each write takes ~40 ms).
void TraceRaw(const char *text, unsigned length);
// Where diagnostics reports are saved (the running app's USRDIR).
std::string ReportPath(const char *name);
// App name for report headers ("PPSSPP PS3" / "RSX Diagnostics").
const char *ProductName();
void EnableThreadDiagnostics();
void DisableThreadDiagnostics();
// XMB support: lowest priority for all emulator threads while the system menu is open.
void RegisterMainThread();
void SetEmulatorThreadsBackground(bool background);
bool Pump(bool gameRunning);
void Present();
void Status(const char *message);
}
