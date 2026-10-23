// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <sys/file.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>
#include <ppu-lv2.h>
#include <algorithm>
#include <set>
#include <atomic>
#include <new>
#include <condition_variable>
#include <cstdio>
#include <malloc.h>
#include <cctype>
#include <exception>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <cstdarg>
#include <cstring>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <vector>
#include "Common/File/DirListing.h"
#include "Common/File/FileUtil.h"
#include "Common/Net/Resolve.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/HLE/sceUtility.h"
#include "Core/HLE/sceFont.h"
#include "GPU/Debugger/Record.h"
#include "Core/MIPS/IR/IRInterpreter.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/CoreTiming.h"
#include "Core/Core.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HW/MediaEngine.h"
#include "Core/HW/Display.h"
#include "Core/HLE/HLE.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/MemMap.h"
#include "Common/FrontendTrace.h"
#include "Core/HW/SasSpu.h"
#include "Core/HW/SoundDebug.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
#include "GPU/Common/GPUDebugInterface.h"
#include "headless/Compare.h"
#include "PS3/frontend/Frontend.h"
#include "GPU/ge_constants.h"
#include "PS3/rsx/RSXSelfTest.h"
#include "PS3/jit/MapiJitProbe.h"
#include "PS3/jit/PS3JitMemory.h"
#include "PS3/frontend/Audio.h"
#include "GPU/PS3Profile.h"
std::string HLEProfileText(double ticksPerMs, double frames);  // Core/HLE/HLE.cpp
std::string HLEProfilePeek();
#include "Core/MIPS/PPC/PPCJit.h"
#include "PS3/rsx/RSXRenderer.h"
#include "PS3/rsx/RSXCore.h"
#include "PS3/rsx/TexSpu.h"
#include "GPU/Software/HardwareBackend.h"
#include "GPU/Software/TLPrepSpu.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/IR/IRInst.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/System.h"
#include "Core/MemMap.h"
#include <cmath>
#include "Core/MIPS/MIPSTables.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/JitCommon/JitBlockCache.h"
#include "PS3/frontend/Canvas.h"
#include "PS3/frontend/MenuScreens.h"
#include "PS3/frontend/GameIcons.h"
SYS_PROCESS_PARAM(1001, 0x100000);
int PS3EmulatorMain(int argc, const char *argv[]);
void PS3FrontendSetBilinear(bool enable);

#define APP_VERSION "0.1"
#define APP_BUILD "270"
// The install folder (title ID) and a name suffix, from build.sh: a diagnostics build installs
// next to the normal one (PS3_APP_ID=PPSSPP002 PS3_EDITION=diag), with its own settings, game
// configs, saves and logs.
#ifndef PPSSPP_PS3_APP_ID
#define PPSSPP_PS3_APP_ID "PPSSPP001"
#endif
#ifndef PPSSPP_PS3_EDITION
#define PPSSPP_PS3_EDITION ""
#endif
#define APP_DIR "/dev_hdd0/game/" PPSSPP_PS3_APP_ID "/USRDIR/"
#define FRONTEND_NAME "PPSSPP PS3 ver " APP_VERSION " (build " APP_BUILD PPSSPP_PS3_EDITION ")"

// Core/MIPS/IR/IRJit.cpp: the "JIT cache" setting.
namespace MIPSComp {
extern int g_irPs3BlockLimit;
extern u32 g_irPs3ArenaLimit;
extern void (*g_irPs3CacheFullHook)(int blocks);
}

// GPU/Software/SoftGpu.cpp: the "Skip keeps menus" setting.
extern bool g_frameSkipKeepOneOff;
// Core/HLE/sceSas.cpp, Core/HW/SasSpu.cpp: the "Sound mixing" setting.
extern bool g_sasNoBlock;
extern bool g_sasSpuPoll;
// Core/Dialog/PSPDialog.cpp: the "Dialog memory wipe" setting.
extern bool g_utilityWipesVolatile;
#define FRONTEND_VERSION "ver " APP_VERSION " (build " APP_BUILD PPSSPP_PS3_EDITION ")"
// Shorter, for the main screen's top bar.
#define FRONTEND_VERSION_BAR APP_VERSION " build " APP_BUILD PPSSPP_PS3_EDITION

static const char *base = APP_DIR;
static volatile bool running = true;
// "Quit to game list" in the in-game options, or X on the "Game stopped" screen: when the app ends
// it starts itself again (sysProcessExitSpawn2), which opens the game list fresh.
static bool quitToGameList = false;
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *font;
static SDL_Texture *screen;
static padData pad;
static int previousPort = -1;
static u32 previousButtons;
static unsigned frames;
static void VfpuNaNHook(const char *op, uint32_t in);
static void IrNaNWatchLog(u32 blockStart, u32 blockBytes, int reg, u32 before, u32 after, const u32 *fpuBefore);
static void StoreWatchReport(const char *text);
extern void (*g_storeWatchReport)(const char *text);
namespace MIPSComp {
extern bool g_irNaNWatch;
extern bool g_irNaNWatchVfpu;
extern bool g_irNaNWatchInf;
extern void (*g_irNaNWatchLog)(u32 blockStart, u32 blockBytes, int reg, u32 before, u32 after, const u32 *fpuBefore);
extern uint32_t g_irNativeGenericMask;
extern bool g_irNativeGenericOp[256];
}
static PS3Frontend::Settings settings;
static RSXRenderer *rsxRenderer = nullptr;

void PS3GEWorkerStuck(int queued, int type) {
	PS3Frontend::Trace("GE worker: still drawing after 2 s (%d jobs queued, current job type %d; 0 = 3D draw)", queued, type);
}

// Finishes the GE list thread's work and the 3D draws still queued on the GE worker thread, before
// using the renderer directly.
static void GESync() {
	if (gpu)
		gpu->AsyncWait(GEASYNC_PRESENT);
	if (Rasterizer::g_hardwareBackend)
		Rasterizer::g_hardwareBackend->Sync();
}
static uint8_t fontMask[192 * 144];
// Glyph coverage (0-255) for the menu canvas: 12x24 cells, and 24x48 cells for titles.
static uint8_t fontSmall[192 * 144];
static uint8_t fontLarge[384 * 288];
static bool fontLargeLoaded = false;
static SDL_Texture *canvasTextures[2];
static int canvasFlip = 0;

// ---------------------------------------------------------------------------------------------
// Logging

// While a game runs, launch.log is also written to USRDIR/logs/<game>/launch.log (see StartGameLogs).
static char gameLogPath[320] = "";

// The "Debug" setting: off, nothing is written to any log. Until the settings are read, lines are
// kept here and written (or dropped) once the setting is known.
static bool traceDecided = false, traceOn = true, traceResetPending = false;
static char earlyTrace[16384];
static unsigned earlyTraceLen = 0;

static void RawTrace(const char *line, unsigned length, bool reset = false) {
	if (!traceDecided) {
		if (reset) {
			earlyTraceLen = 0;
			traceResetPending = true;
		}
		const unsigned n = std::min<unsigned>(length, sizeof(earlyTrace) - earlyTraceLen);
		memcpy(earlyTrace + earlyTraceLen, line, n);
		earlyTraceLen += n;
		return;
	}
	if (!traceOn)
		return;
	if (gameLogPath[0]) {
		s32 fd = -1;
		if (sysLv2FsOpen(gameLogPath, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0666, nullptr, 0) == 0) {
			u64 written = 0;
			sysLv2FsWrite(fd, line, length, &written);
			sysLv2FsClose(fd);
		}
	}
	const char *paths[] = {APP_DIR "launch.log", "/dev_hdd0/tmp/PPSSPP_PS3.log",
		"/dev_usb000/PPSSPP_PS3.log", "/dev_usb001/PPSSPP_PS3.log", "/dev_usb002/PPSSPP_PS3.log",
		"/dev_usb003/PPSSPP_PS3.log", "/dev_usb004/PPSSPP_PS3.log", "/dev_usb005/PPSSPP_PS3.log",
		"/dev_usb006/PPSSPP_PS3.log", "/dev_usb007/PPSSPP_PS3.log"};
	for (const char *path : paths) {
		s32 fd = -1;
		if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | (reset ? SYS_O_TRUNC : SYS_O_APPEND), &fd, 0666, nullptr, 0) == 0) {
			u64 written = 0;
			sysLv2FsWrite(fd, line, length, &written);
			sysLv2FsFsync(fd);
			sysLv2FsClose(fd);
		}
	}
}
static void EarlyTrace() __attribute__((constructor(101)));
static void EarlyTrace() {
	const char message[] = "PPSSPP PS3 ver 0.1 (build 265): early constructor reached\n";
	RawTrace(message, sizeof(message) - 1, true);
}
static void RuntimeTrace() __attribute__((constructor(106)));
static void RuntimeTrace() {
	const char message[] = "SDK heap/syscall/libc constructors completed\n";
	RawTrace(message, sizeof(message) - 1);
}
static void SetTraceEnabled(bool on) {
	const bool first = !traceDecided;
	traceDecided = true;
	traceOn = on;
	if (first && on && earlyTraceLen) {
		RawTrace(earlyTrace, earlyTraceLen, traceResetPending);
		earlyTraceLen = 0;
	}
}

void PS3Frontend::Trace(const char *format, ...) {
	char message[1024]; va_list args; va_start(args, format);
	vsnprintf(message, sizeof(message) - 1, format, args); va_end(args);
	size_t n = strlen(message); message[n++] = '\n'; RawTrace(message, n);
}
void PS3Frontend::TraceRaw(const char *text, unsigned length) {
	RawTrace(text, length);
}
// Long reports go to launch.log in one write: each Trace opens, writes, syncs and closes the logs
// (~40 ms on the PS3's disk), so the JIT profile's 700 lines took half a minute line by line.
static void Appendf(std::string &out, const char *format, ...) {
	char message[2048]; va_list args; va_start(args, format);
	vsnprintf(message, sizeof(message) - 1, format, args); va_end(args);
	out += message;
	out += '\n';
}
static void TraceReport(const std::string &report) {
	if (!report.empty())
		RawTrace(report.data(), (unsigned)report.size());
}
// System (XMB) events. The PS button menu only opens if the application answers sysutil events
// promptly; with the core running at a few frames per second the per-frame Pump was far too
// rare, so the XMB showed its loading spinner forever. A small thread now services sysutil every
// 16 ms, the emulation pauses while the menu is open, and "Quit Game" is guaranteed to end the
// process within a few seconds even if the core is stuck.
static std::atomic<bool> systemMenuOpen{false};
static std::atomic<bool> inGameOptionsOpen{false};  // Paused in the in-game options (not a stall).

// Stall watch: every second, flush core.log (a forced power-off otherwise loses its buffer), and
// when no frame has come for 5 s, say in launch.log what the emulation thread is doing.
static std::string MemoryText();
static std::string MemoryDetailText();
static std::string GEWorkerText();
static volatile bool stallWatchRunning = false;
static void SnapshotCoreLog();
// For a stall report: the PSP threads (what each one waits on), read without stopping the
// emulation thread (it's stuck anyway; a slightly torn read only garbles this text).
static std::string PspThreadsText() {
	std::string out;
	const std::vector<DebugThreadInfo> threads = GetThreadsInfo();
	for (const DebugThreadInfo &t : threads) {
		char buf[160];
		const char *status = (t.status & THREADSTATUS_RUNNING) ? "running" : (t.status & THREADSTATUS_READY) ? "ready" :
			(t.status & THREADSTATUS_WAIT) ? "waiting" : (t.status & THREADSTATUS_SUSPEND) ? "suspended" :
			(t.status & THREADSTATUS_DORMANT) ? "dormant" : "dead";
		snprintf(buf, sizeof(buf), "%s%s(prio %d, %s%s%s, pc %08x)", out.empty() ? "" : "; ", t.name, t.priority, status,
			(t.status & THREADSTATUS_WAIT) ? " " : "", (t.status & THREADSTATUS_WAIT) ? WaitTypeToString(t.waitType) : "", t.curPC);
		out += buf;
		if (out.size() > 1800) {
			out += "; ...";
			break;
		}
	}
	return out;
}


// The save dialog's IO thread (Core/Dialog/PSPSaveDialog.cpp).
extern char g_saveIOActivity[256];
extern std::atomic<double> g_saveIOStart;
extern std::atomic<int> g_saveDialogStep, g_saveDialogDisplay;
extern std::atomic<const char *> g_saveIOStage;

static void StallWatchThread(void *) {
	unsigned lastFrames = frames;
	double lastChange = time_now_d(), lastReport = 0.0;
	while (stallWatchRunning) {
		usleep(1000 * 1000);
		fflush(stderr);
		const double now = time_now_d();
		if (frames != lastFrames || systemMenuOpen || inGameOptionsOpen) {
			lastFrames = frames;
			lastChange = now;
			continue;
		}
		if (now - lastChange < 5.0 || now - lastReport < 5.0)
			continue;
		// Debug tab "Stall reports" (and nothing to write them to without Debug).
		if (!settings.debugButtons || !settings.logStall)
			continue;
		lastReport = now;
		// The PSP threads once per stall, and again every half minute of it.
		if (now - lastChange < 6.5 || (int)(now - lastChange) % 30 < 5) {
			PS3Frontend::Trace("STALL: PSP threads: %s", PspThreadsText().c_str());
			// A stuck game usually ends with a forced power-off, and core.log is then only copied
			// to the game's log folder if we do it now.
			SnapshotCoreLog();
		}
		{
			// The code the game was last seen running, and where it was called from.
			const MIPSState *m = currentMIPS;
			if (m) {
				const u32 pcs[2] = { m->pc, m->r[MIPS_REG_RA] };
				for (int w = 0; w < 2; ++w) {
					const u32 at = pcs[w] & ~3u;
					std::string code;
					for (u32 a = at - 24; a <= at + 12; a += 4) {
						if (!Memory::IsValidAddress(a))
							continue;
						char dis[96];
						MIPSDisAsm(Memory::ReadUnchecked_Instruction(a, true), a, dis, sizeof(dis), true);
						char line[128];
						snprintf(line, sizeof(line), "%s%08x %s", code.empty() ? "" : " | ", a, dis);
						code += line;
					}
					PS3Frontend::Trace("STALL: code at %s %08x: %s", w == 0 ? "pc" : "ra", at, code.c_str());
				}
				// Registers (as last stored by the JIT), and what the call before ra went to.
				if (now - lastChange < 6.5) {
					static const char *const names[32] = {"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
						"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
					std::string regs;
					for (int i = 1; i < 32; ++i) {
						char r[24];
						snprintf(r, sizeof(r), " %s=%08x", names[i], m->r[i]);
						regs += r;
					}
					PS3Frontend::Trace("STALL: registers:%s", regs.c_str());
					const u32 call = (m->r[MIPS_REG_RA] & ~3u) - 8;
					if (Memory::IsValidAddress(call)) {
						const u32 op = Memory::ReadUnchecked_Instruction(call, true).encoding;
						if ((op >> 26) == 3) {
							const u32 target = (call & 0xF0000000) | ((op & 0x03FFFFFF) << 2);
							std::string code;
							for (u32 a = target; a < target + 16; a += 4) {
								if (!Memory::IsValidAddress(a))
									break;
								char dis[96];
								MIPSDisAsm(Memory::ReadUnchecked_Instruction(a, true), a, dis, sizeof(dis), true);
								code += std::string(code.empty() ? "" : " | ") + dis;
							}
							PS3Frontend::Trace("STALL: the call before ra went to %08x: %s", target, code.c_str());
						}
					}
				}
			}
		}
		// What the game calls while stuck (polling an IO, a semaphore, the clock...), once per stall
		// and again later; and once, the code of the loop, what it calls, and the memory its
		// argument registers point at (Frontend 112, for Midnight Club L.A.'s hang).
		if (now - lastChange < 6.5 || (now - lastChange >= 15.0 && now - lastChange < 20.0))
			PS3Frontend::Trace("STALL: HLE calls since the last perf report: %s", HLEProfilePeek().c_str());
		if (now - lastChange < 6.5 && currentMIPS) {
			const MIPSState *m = currentMIPS;
			std::string dump = "STALL: code around pc and what it calls:\n";
			auto disasm = [&](u32 from, u32 to) {
				for (u32 a = from; a < to; a += 4) {
					if (!Memory::IsValidAddress(a))
						continue;
					char dis[96];
					MIPSDisAsm(Memory::ReadUnchecked_Instruction(a, true), a, dis, sizeof(dis), true);
					char line[128];
					snprintf(line, sizeof(line), "  %08x %08x %s\n", a, Memory::ReadUnchecked_Instruction(a, true).encoding, dis);
					dump += line;
				}
			};
			const u32 pc = m->pc & ~3u;
			disasm(pc - 0x80, pc + 0x60);
			u32 targets[6];
			int nt = 0;
			for (u32 a = pc - 0x80; a < pc + 0x60 && nt < 6; a += 4) {
				if (!Memory::IsValidAddress(a))
					continue;
				const u32 op = Memory::ReadUnchecked_Instruction(a, true).encoding;
				if ((op >> 26) != 3)
					continue;
				const u32 t = (a & 0xF0000000) | ((op & 0x03FFFFFF) << 2);
				bool seen = false;
				for (int k = 0; k < nt; ++k)
					seen = seen || targets[k] == t;
				if (!seen)
					targets[nt++] = t;
			}
			for (int k = 0; k < nt; ++k) {
				char head[64];
				snprintf(head, sizeof(head), " function %08x:\n", targets[k]);
				dump += head;
				disasm(targets[k], targets[k] + 0xC0);
			}
			static const int regsToDump[] = {MIPS_REG_A0, MIPS_REG_A1, MIPS_REG_S0, MIPS_REG_S1, MIPS_REG_S2, MIPS_REG_GP};
			for (int r : regsToDump) {
				const u32 addr = m->r[r] & ~3u;
				if (!Memory::IsValidRange(addr, 64))
					continue;
				char line[96];
				snprintf(line, sizeof(line), " memory at reg %d (%08x):", r, addr);
				dump += line;
				for (int i = 0; i < 16; ++i) {
					snprintf(line, sizeof(line), " %08x", Memory::ReadUnchecked_U32(addr + i * 4));
					dump += line;
				}
				dump += "\n";
			}
			PS3Frontend::TraceRaw(dump.c_str(), (unsigned)dump.size());
		}
		const char *hle = PS3Profile::hleRunning;
		const uint32_t compiling = PS3Profile::jitCompilingPC;
		const uint32_t pc = currentMIPS ? currentMIPS->pc : 0;
		if (hle && (strncmp(hle, "sceMpeg", 7) == 0 || strncmp(hle, "scePsmf", 7) == 0))
			PS3Frontend::Trace("STALL: no frame for %.0f s; emulation thread in HLE call %s (pc %08x), %s; %s", now - lastChange, hle, pc, MediaDecodeStepText().c_str(), MemoryText().c_str());
		else if (hle)
			PS3Frontend::Trace("STALL: no frame for %.0f s; emulation thread in HLE call %s (pc %08x); %s", now - lastChange, hle, pc, MemoryText().c_str());
		else if (compiling)
			PS3Frontend::Trace("STALL: no frame for %.0f s; emulation thread compiling the block at %08x; %s", now - lastChange, compiling, MemoryText().c_str());
		else
			PS3Frontend::Trace("STALL: no frame for %.0f s; emulation thread running PSP code, pc %08x; %s", now - lastChange, pc, MemoryText().c_str());
		PS3Frontend::Trace("STALL: %s", GEWorkerText().c_str());
		PS3Frontend::Trace("STALL: memory: %s", MemoryDetailText().c_str());
		if (g_saveDialogStep != 0)
			PS3Frontend::Trace("STALL: save dialog at step %d on screen %d (steps: 2 list reload, 3-4 drawing, 10-13 list/info drawing, 20 waiting for the save thread, 22-23 reloading the list after it)", g_saveDialogStep.load(), g_saveDialogDisplay.load());
		if (g_saveIOActivity[0])
			PS3Frontend::Trace("STALL: save data work running for %.0f s: %s; now in: %s", time_now_d() - g_saveIOStart.load(), g_saveIOActivity, g_saveIOStage.load());
	}
}
static std::atomic<bool> systemDrawing{false};  // SYSUTIL_DRAW_BEGIN seen since the menu opened.
static std::atomic<bool> eventThreadActive{false};
static std::atomic<bool> eventThreadStop{false};
static volatile double exitRequestedAt = 0.0;
static sys_ppu_thread_t eventThread;

const char *PS3Frontend::ProductName() { return "PPSSPP PS3"; }
std::string PS3Frontend::ReportPath(const char *name) { return std::string(base) + name; }

// Per-game log folder: USRDIR/logs/<game file name>/ gets launch.log (the app's lines so far, then
// everything while the game runs), and at the end core.log, stdout.log, the settings used and the
// GE dumps taken during the run. Each run of a game replaces its folder's files.
static std::string gameLogDir;
static std::vector<std::string> dumpsBefore;

static bool CopyFile(const std::string &from, const std::string &to) {
	FILE *in = fopen(from.c_str(), "rb");
	if (!in)
		return false;
	FILE *out = fopen(to.c_str(), "wb");
	if (!out) {
		fclose(in);
		return false;
	}
	std::vector<char> buf(64 * 1024);
	size_t n;
	while ((n = fread(buf.data(), 1, buf.size(), in)) > 0)
		fwrite(buf.data(), 1, n, out);
	fclose(in);
	fclose(out);
	return true;
}

// core.log as it is now, into the game's log folder, synced to disk.
static void SnapshotCoreLog() {
	if (gameLogDir.empty() || settings.logLevel == 0)
		return;
	fflush(stderr);
	FILE *in = fopen((std::string(base) + "core.log").c_str(), "rb");
	if (!in)
		return;
	s32 fd = -1;
	if (sysLv2FsOpen((gameLogDir + "core.log").c_str(), SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, nullptr, 0) == 0) {
		std::vector<char> buf(64 * 1024);
		size_t n;
		while ((n = fread(buf.data(), 1, buf.size(), in)) > 0) {
			u64 written = 0;
			sysLv2FsWrite(fd, buf.data(), n, &written);
		}
		sysLv2FsFsync(fd);
		sysLv2FsClose(fd);
		PS3Frontend::Trace("STALL: core.log copied to the game's log folder (it survives a forced power-off)");
	}
	fclose(in);
}

static std::string DumpDir() { return std::string(base) + "memstick/PSP/SYSTEM/DUMP/"; }

static std::vector<std::string> ListDir(const std::string &dir) {
	std::vector<std::string> names;
	if (DIR *d = opendir(dir.c_str())) {
		while (struct dirent *e = readdir(d)) {
			if (e->d_name[0] != '.')
				names.push_back(e->d_name);
		}
		closedir(d);
	}
	return names;
}

// Where a log stream that was switched off writes: nowhere. (newlib: the stream's write function;
// freopen puts the file's back.)
static auto DiscardWrite(struct _reent *, void *, const char *, int n) -> decltype(((FILE *)nullptr)->_write(nullptr, nullptr, nullptr, 0)) {
	return n;
}

// stdout.log with Debug on, core.log (PPSSPP's own log) with Debug and Core logging on. Called at
// startup, when settings change and at game start: switching Debug or Core logging off stops the
// file at once (before build 202 it was written until the app restarted).
static void OpenLogFiles() {
	static bool stdoutOpen = false, coreOpen = false;
	const bool wantStdout = settings.debugButtons;
	const bool wantCore = settings.debugButtons && settings.logLevel > 0;
	if (wantStdout && !stdoutOpen) {
		freopen((std::string(base) + "stdout.log").c_str(), "w", stdout);
		setvbuf(stdout, nullptr, _IONBF, 0);
	} else if (!wantStdout && stdoutOpen) {
		fflush(stdout);
		stdout->_write = DiscardWrite;
	}
	stdoutOpen = wantStdout;
	if (wantCore && !coreOpen) {
		freopen((std::string(base) + "core.log").c_str(), "w", stderr);
		setvbuf(stderr, nullptr, _IONBF, 0);
	} else if (!wantCore && coreOpen) {
		fflush(stderr);
		stderr->_write = DiscardWrite;
	}
	coreOpen = wantCore;
}

static void StartGameLogs(const std::string &game) {
	if (!settings.debugButtons)
		return;
	std::string name = game.substr(game.find_last_of('/') + 1);
	const size_t dot = name.find_last_of('.');
	if (dot != std::string::npos && dot > 0)
		name = name.substr(0, dot);
	for (char &c : name) {
		if (!isalnum((unsigned char)c) && c != ' ' && c != '-' && c != '_' && c != '(' && c != ')')
			c = '_';
	}
	if (name.empty())
		name = "game";
	const std::string logs = std::string(base) + "logs";
	mkdir(logs.c_str(), 0777);
	gameLogDir = logs + "/" + name + "/";
	mkdir(gameLogDir.c_str(), 0777);
	for (const std::string &old : ListDir(gameLogDir))
		remove((gameLogDir + old).c_str());
	if (settings.debugButtons) {
		const std::string path = gameLogDir + "launch.log";
		CopyFile(std::string(base) + "launch.log", path);
		snprintf(gameLogPath, sizeof(gameLogPath), "%s", path.c_str());
	}
	dumpsBefore = ListDir(DumpDir());
	PS3Frontend::Trace("%s - logs for this game: %s", FRONTEND_NAME, gameLogDir.c_str());
}

static void EndGameLogs() {
	if (gameLogDir.empty())
		return;
	fflush(stdout);
	fflush(stderr);
	if (settings.logLevel > 0)
		CopyFile(std::string(base) + "core.log", gameLogDir + "core.log");
	CopyFile(std::string(base) + "stdout.log", gameLogDir + "stdout.log");
	CopyFile(std::string(base) + "settings.ini", gameLogDir + "settings.ini");
	// GE dumps stay in the dump folder (build 186).
	int dumps = 0;
	for (const std::string &d : ListDir(DumpDir()))
		if (std::find(dumpsBefore.begin(), dumpsBefore.end(), d) == dumpsBefore.end() && d.size() > 6 && d.compare(d.size() - 6, 6, ".ppdmp") == 0)
			dumps++;
	PS3Frontend::Trace("Logs for this game are in %s (%d GE dumps from this run, in %s)", gameLogDir.c_str(), dumps, DumpDir().c_str());
	gameLogPath[0] = 0;
	gameLogDir.clear();
}

// Free main memory: what lv2 has left (sys_memory_get_user_memory_size) plus what malloc holds
// free. malloc takes memory from lv2 as it grows and never gives it back, so lv2's figure alone
// only ever went down: memory a game's scenes freed (big draw buffers, compiled code) still
// counted as used, the "low memory" JIT clears kept firing and GE dumps were refused.
static bool FreeMemory(u32 *freeBytes, u32 *lv2Free, u32 *total) {
	struct { u32 total; u32 available; } info{};
	lv2syscall1(352, (u64)(uintptr_t)&info);
	if ((s32)p1 != 0)
		return false;
	const struct mallinfo mi = mallinfo();
	*lv2Free = info.available;
	*total = info.total;
	*freeBytes = info.available + (u32)mi.fordblks;
	return true;
}

static std::string MemoryText() {
	u32 freeBytes = 0, lv2Free = 0, total = 0;
	char text[96];
	if (!FreeMemory(&freeBytes, &lv2Free, &total)) return "memory info unavailable";
	snprintf(text, sizeof(text), "%u MB free of %u MB (%u MB not yet taken by malloc)", freeBytes >> 20, total >> 20, lv2Free >> 20);
	return text;
}

// Frontend 88: what main memory is doing, for the Silent Hill freezes (each came with ~1 MB left).
extern std::atomic<uint32_t> g_geJobBufferBytes;
extern std::atomic<int> g_geTrimJobs;
extern std::atomic<bool> g_geLowMemory;
extern std::atomic<int> g_geWorkerJob;
extern std::atomic<uint64_t> g_geWorkerJobStart;
extern std::atomic<int> g_geProducerWaiting;
extern std::atomic<int> g_geDrawLogLeft;  // SoftGpu.cpp: draws still to log (L2+R2 without memory for a dump).

static std::string GEWorkerText() {
	char text[160];
	const int job = g_geWorkerJob.load();
	if (job < 0)
		snprintf(text, sizeof(text), "GE worker idle (no job running)%s", g_geProducerWaiting.load() ? ", emulation thread waiting for a free job slot" : "");
	else
		snprintf(text, sizeof(text), "GE worker in a job of type %d for %.1f s%s", job, (PS3Profile::Now() - g_geWorkerJobStart.load()) / 79800000.0,
			g_geProducerWaiting.load() ? ", emulation thread waiting for a free job slot" : "");
	return text;
}

// The biggest block malloc can hand out right now (fragmentation: free memory in pieces too small).
static u32 LargestAllocatable() {
	for (u32 size = 16u << 20; size >= 64 * 1024; size >>= 1) {
		if (void *p = malloc(size)) {
			free(p);
			return size;
		}
	}
	return 0;
}

static std::string MemoryDetailText() {
	const struct mallinfo mi = mallinfo();
	char text[256];
	snprintf(text, sizeof(text), "malloc arena %u MB (%u MB in use, %u MB free inside), largest block malloc can give %u KB; GE job buffers %u KB; JIT %d blocks",
		(u32)mi.arena >> 20, (u32)mi.uordblks >> 20, (u32)mi.fordblks >> 20, LargestAllocatable() >> 10,
		g_geJobBufferBytes.load() >> 10, MIPSComp::jit && MIPSComp::jit->GetBlockCacheDebugInterface() ? MIPSComp::jit->GetBlockCacheDebugInterface()->GetNumBlocks() : 0);
	return text;
}

// Frontend 91: a failed C++ allocation no longer has to hang the game. 4 MB set aside at core start
// is given back when one fails, and the allocation is retried (it got that far only because main
// memory was in pieces too small: Silent Hill Origins had 19 MB free, no piece bigger than 512 KB).
// The GE job buffers are released too, and the reserve is taken again once memory allows.
static constexpr u32 OOM_RESERVE_BYTES = 4u * 1024 * 1024;
static std::atomic<void *> g_oomReserve{nullptr};
static std::atomic<int> g_oomCount{0};
static std::atomic<unsigned long long> g_geWorkerThreadId{0};

void PS3GEListThreadStarted() {
	PS3Frontend::Trace("GE list thread started");
}

void PS3GEWorkerStarted() {
	sys_ppu_thread_t id = 0;
	sysThreadGetId(&id);
	g_geWorkerThreadId = (unsigned long long)id;
}

// Return addresses up the stack (PPC64 back chain: 0(r1) is the caller's frame, whose 16(r1) holds
// the return address into it), to find the allocation in the unstripped ELF.
static void __attribute__((noinline)) TraceBacktrace() {
	char text[400];
	int len = snprintf(text, sizeof(text), "OUT OF MEMORY: called from");
	const uint64_t *sp = (const uint64_t *)__builtin_frame_address(0);
	for (int i = 0; i < 16 && sp; ++i) {
		const uint64_t *next = (const uint64_t *)(uintptr_t)sp[0];
		if (!next || next <= sp || (uintptr_t)next - (uintptr_t)sp > 1024 * 1024)
			break;
		if (len < (int)sizeof(text) - 12)
			len += snprintf(text + len, sizeof(text) - len, " %08llx", (unsigned long long)next[2]);
		sp = next;
	}
	PS3Frontend::Trace("%s", text);
}

static void OutOfMemoryHandler() {
	sys_ppu_thread_t id = 0;
	sysThreadGetId(&id);
	const bool worker = (unsigned long long)id == g_geWorkerThreadId.load();
	g_oomCount++;
	if (void *reserve = g_oomReserve.exchange(nullptr)) {
		free(reserve);
		g_geTrimJobs = 96;  // Every slot (TLWorker::RING).
		g_geLowMemory = true;
		PS3Frontend::Trace("OUT OF MEMORY: a C++ allocation failed on thread %llx%s; 4 MB reserve given back, retrying; %s",
			(unsigned long long)id, worker ? " (GE worker)" : "", MemoryText().c_str());
		TraceBacktrace();
		return;  // operator new tries again.
	}
	std::set_new_handler(nullptr);
	PS3Frontend::Trace("OUT OF MEMORY: a C++ allocation failed on thread %llx%s, no reserve left; %s",
		(unsigned long long)id, worker ? " (GE worker)" : "", MemoryText().c_str());
	TraceBacktrace();
}

// Takes the reserve (again) when there is room for it without using the last big free block.
static bool TakeOomReserve() {
	if (g_oomReserve.load())
		return false;
	u32 freeBytes = 0, lv2Free = 0, total = 0;
	if (!FreeMemory(&freeBytes, &lv2Free, &total))
		return false;
	if (lv2Free < 2 * OOM_RESERVE_BYTES && LargestAllocatable() < 2 * OOM_RESERVE_BYTES)
		return false;
	void *p = malloc(OOM_RESERVE_BYTES);
	if (!p)
		return false;
	void *expected = nullptr;
	if (!g_oomReserve.compare_exchange_strong(expected, p)) {
		free(p);
		return false;
	}
	return true;
}

static void SystemCallback(u64 status, u64 param, void *) {
	PS3Frontend::Trace("System event 0x%04llx (param %llx), %s", (unsigned long long)status, (unsigned long long)param, MemoryText().c_str());
	switch (status) {
	case SYSUTIL_EXIT_GAME:
		running = false;
		if (exitRequestedAt == 0.0) exitRequestedAt = time_now_d();
		break;
	case SYSUTIL_MENU_OPEN:
		systemDrawing = false;
		systemMenuOpen = true;
		// Immediately, from this thread: the emulation thread may need seconds to reach its pause.
		PS3Frontend::SetEmulatorThreadsBackground(true);
		break;
	case SYSUTIL_MENU_CLOSE:
		systemMenuOpen = false;
		PS3Frontend::SetEmulatorThreadsBackground(false);
		break;
	case SYSUTIL_DRAW_BEGIN: systemDrawing = true; break;
	default: break;
	}
}

static void SystemEventThread(void *) {
	while (!eventThreadStop) {
		sysUtilCheckCallback();
		if (exitRequestedAt != 0.0 && time_now_d() - exitRequestedAt > 5.0) {
			// The core did not return in time; leave now rather than hang the console.
			PS3Frontend::Trace("Quit requested from the XMB; core did not stop within 5 s, exiting directly");
			fflush(stderr); fflush(stdout);
			sysProcessExit(0);
		}
		usleep(16 * 1000);
	}
	sysThreadExit(0);
}

static void StartSystemEventThread() {
	eventThreadStop = false;
	if (sysThreadCreate(&eventThread, SystemEventThread, nullptr, 900, 0x4000, THREAD_JOINABLE, (char *)"sysutil_events") == 0)
		eventThreadActive = true;
	else
		PS3Frontend::Trace("sysutil event thread could not be created; events are polled per frame");
}

static void StopSystemEventThread() {
	if (!eventThreadActive) return;
	eventThreadStop = true;
	u64 ret; sysThreadJoin(eventThread, &ret);
	eventThreadActive = false;
}

// ---------------------------------------------------------------------------------------------
// Settings (USRDIR/settings.ini, key=value)

static std::string SettingsPath() { return std::string(base) + "settings.ini"; }

static bool LoadSettingsFrom(const std::string &path) {
	FILE *f = fopen(path.c_str(), "r");
	if (!f) return false;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		// Tolerate a UTF-8 byte order mark (Windows editors add one) and spaces around the key.
		char *p = line;
		if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
		char key[64]; int value;
		if (strncmp(p, "jitops", 6) == 0 && strchr(p, '=')) {
			// Text: IR op names, comma or space separated.
			std::string v = strchr(p, '=') + 1;
			while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ' || v.back() == '\t')) v.pop_back();
			settings.jitOps = v;
			continue;
		}
		if (strncmp(p, "nickname", 8) == 0 && strchr(p, '=')) {
			// Text, not a number: everything after '=', without surrounding spaces and the line end.
			std::string v = strchr(p, '=') + 1;
			while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ' || v.back() == '\t')) v.pop_back();
			size_t start = v.find_first_not_of(" \t");
			settings.nickname = start == std::string::npos ? std::string() : v.substr(start);
			continue;
		}
		if (sscanf(p, " %63[^= \t] = %d", key, &value) != 2) continue;
		std::string k(key);
		if (k == "screen") settings.screenMode = std::max(0, std::min(value, (int)PS3Frontend::ScreenMode::COUNT - 1));
		else if (k == "bilinear") settings.bilinear = value != 0;
		else if (k == "frameskip") settings.frameskip = std::max(-1, std::min(value, 3));
		else if (k == "speedlimit") settings.speedLimit = value != 0;
		else if (k == "showfps") settings.showFps = value != 0;
		else if (k == "log") settings.logLevel = std::max(0, std::min(value, 2));
		else if (k == "spline") settings.splineQuality = std::max(0, std::min(value, 2));
		else if (k == "geprofile") settings.collectStats = value != 0;
		else if (k == "renderer") settings.renderer = std::max(0, std::min(value, 1));
		else if (k == "resolution") settings.resolution = std::max(1, std::min(value, 3));
		else if (k == "vramsync") settings.vramSyncEveryFrame = value != 0;
		else if (k == "cpujit") settings.cpuJit = value != 0;
		else if (k == "sound") settings.sound = value != 0;
		else if (k == "sasdirect") settings.sasDirect = value != 0;
		else if (k == "discfonts") settings.discFonts = value != 0;
		else if (k == "gamesort") settings.gameSort = value >= 0 && value <= 3 ? value : 0;
		else if (k == "perflog") settings.logPerf = value >= 0 && value <= 2 ? value : 1;
		else if (k == "stalllog") settings.logStall = value != 0;
		else if (k == "videolog") settings.logVideo = value != 0;
		else if (k == "savelog") settings.logSave = value != 0;
		else if (k == "gewatchlog") settings.logGeWatch = value != 0;
		else if (k == "rsxlog") settings.logRsx = value != 0;
		else if (k == "jitlog") settings.logJit = value != 0;
		else if (k == "snddebug") settings.soundDebug = value >= 0 && value <= 2 ? value : 0;
		else if (k == "l2r2") settings.l2r2 = value == 1 ? 1 : 0;
		else if (k == "dumpram") settings.dumpRam = value != 0;
		else if (k == "dumpvram") settings.dumpVram = value != 0;
		else if (k == "videodeblock") settings.videoDeblock = value < 0 ? -1 : (value ? 1 : 0);
		else if (k == "deblock" && value == 0) settings.videoDeblock = 0;  // Builds 210-213 (on/off).
		else if (k == "ingamemenu") settings.inGameMenu = value != 0;
		else if (k == "r3fps") settings.r3Fps = value != 0;
		else if (k == "videocap") settings.videoCap = value == 1 || value < 0 ? -1 : value == 40 ? 40 : value == 30 ? 30 : 60;  // 1: Auto (builds 222-223).
		else if (k == "nanwatch") settings.nanWatch = value;
		else if (k == "storewatch") settings.storeWatch = value != 0;
		else if (k == "jitgeneric") settings.jitGeneric = value & 127;
		else if (k == "iohost") settings.ioHost = value != 0;
		else if (k == "jitlarge") settings.jitLarge = value != 0;
		else if (k == "gelists") settings.geListThread = value >= 0 && value <= 3 ? value : 0;
		else if (k == "lensflare") settings.lensFlare = value != 0;
		else if (k == "cpurows") settings.cpuRows = value > 0 ? 1 : 0;
		else if (k == "confirm") settings.confirmButton = value < 0 ? -1 : (value ? 1 : 0);
		else if (k == "language") settings.language = value >= 0 && value <= 11 ? value : -1;
		else if (k == "dateformat") settings.dateFormat = value >= 0 && value <= 2 ? value : -1;
		else if (k == "timeformat") settings.timeFormat = value >= 0 && value <= 1 ? value : -1;
		else if (k == "cpuclock") settings.cpuClock = value > 0 ? std::max(10, std::min(value, 1000)) : 0;
		else if (k == "volume") settings.volume = std::max(0, std::min(value, 100));
		else if (k == "anisotropy") settings.anisotropy = value >= 16 ? 16 : value >= 8 ? 8 : value >= 4 ? 4 : value >= 2 ? 2 : 0;
		else if (k == "fxaa") settings.fxaa = (value == 1 || value == 2) ? value : 0;
		else if (k == "audioslow") settings.audioSlow = value >= 0 && value <= 2 ? value : 0;
		else if (k == "pspmem64") settings.pspMem64 = value != 0;
		else if (k == "gethread") settings.geThread = value != 0;
		else if (k == "geasync") settings.geAsync = value != 0;
		else if (k == "vmx") settings.vmx = value != 0;
		else if (k == "spuvertex") settings.spuVertex = value != 0;
		else if (k == "vertexring") settings.vertexRingMain = value != 0;
		else if (k == "sputexture") settings.spuTexture = value != 0;
		else if (k == "jitsample") settings.jitProfile = value != 0;
		else if (k == "texcheck") settings.textureCheckFull = value != 0;
		else if (k == "wbrows") settings.writeBackAllRows = value != 0;
		else if (k == "rttreuse") settings.rttCopyReuse = value != 0;
		else if (k == "tlmerge") settings.tlMerge = value != 0;
		else if (k == "jitwarmup") settings.jitWarmup = value != 0;
		else if (k == "rsxtile") settings.rsxTiled = value == 2 ? 2 : (value != 0 ? 1 : 0);
		else if (k == "videos") settings.videos = value < 0 ? -1 : (value ? 1 : 0);
		else if (k == "skipkeep") settings.frameskipKeep = value != 0;
		else if (k == "volwipe") settings.volatileWipe = value != 0;
		else if (k == "latedepth") settings.lateDepth = value != 0;
		else if (k == "keeplayouts") settings.keepLayouts = value != 0;
		else if (k == "ftz") settings.ftz = value < 0 ? -1 : (value ? 1 : 0);
		else if (k == "rstick") settings.rightStick = value >= 0 && value <= 2 ? value : 0;
		else if (k == "fpscap") settings.fpsCap = value <= 0 || value >= 45 ? 60 : value >= 25 ? 30 : 20;  // 0 before F60: no cap.
		else if (k == "debugbuttons" || k == "debug") settings.debugButtons = value != 0;
		else if (k == "quitcombo") settings.quitCombo = value != 0;
		else if (k == "getiming") settings.geTiming = std::max(100, std::min(value, 400));
		else if (k == "fast3dmode") settings.fast3d = std::max(0, std::min(value, 2));  // "fast3d" (on/off) before Frontend 25: ignored, so the new default applies.
	}
	fclose(f);
	return true;
}

// Diag builds (PS3_EDITION containing "diag", installed as PPSSPP002 next to the normal app).
static bool IsDiagEdition() { return strstr(PPSSPP_PS3_EDITION, "diag") != nullptr; }

static void LoadSettings() {
	// A diag build writes its logs from the start (Debug on until its settings say otherwise).
	if (IsDiagEdition())
		settings.debugButtons = true;
	LoadSettingsFrom(SettingsPath());
}

// Per-game overrides: USRDIR/games/<image name without extension>.ini, same keys as settings.ini
// (only the ones listed are changed), applied when that game starts.
static void LoadGameSettings(const std::string &game) {
	std::string name = game.substr(game.find_last_of('/') + 1);
	const size_t dot = name.find_last_of('.');
	if (dot != std::string::npos) name = name.substr(0, dot);
	const std::string path = std::string(base) + "games/" + name + ".ini";
	if (LoadSettingsFrom(path)) {
		PS3Frontend::Trace("Per-game settings applied from %s", path.c_str());
		return;
	}
	PS3Frontend::Trace("No per-game settings (%s). Files in games/ ending in .ini:", path.c_str());
	if (DIR *d = opendir((std::string(base) + "games").c_str())) {
		while (struct dirent *e = readdir(d)) {
			const std::string n = e->d_name;
			if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".ini") == 0)
				PS3Frontend::Trace("  %s", n.c_str());
			// The same name in other letter case (DOA.INI for DOA.iso).
			if (strcasecmp(n.c_str(), (name + ".ini").c_str()) == 0) {
				const std::string other = std::string(base) + "games/" + n;
				if (LoadSettingsFrom(other)) {
					PS3Frontend::Trace("Per-game settings applied from %s", other.c_str());
					break;
				}
			}
		}
		closedir(d);
	}
}

static std::string SettingsText(const PS3Frontend::Settings &st) {
	char buf[1536];
	snprintf(buf, sizeof(buf), "screen=%d\nbilinear=%d\nframeskip=%d\nspeedlimit=%d\nshowfps=%d\nlog=%d\nspline=%d\ngeprofile=%d\nrenderer=%d\nresolution=%d\nvramsync=%d\ncpujit=%d\nsound=%d\npspmem64=%d\nfast3dmode=%d\ngethread=%d\nvmx=%d\nspuvertex=%d\nvertexring=%d\nsputexture=%d\njitsample=%d\ntexcheck=%d\nwbrows=%d\nrttreuse=%d\ntlmerge=%d\njitwarmup=%d\nrsxtile=%d\ngetiming=%d\nfpscap=%d\ndebugbuttons=%d\nquitcombo=%d\ngeasync=%d\nrstick=%d\nvideos=%d\nftz=%d\nskipkeep=%d\nvolwipe=%d\nlatedepth=%d\nkeeplayouts=%d\naudioslow=%d\nsasdirect=%d\niohost=%d\njitlarge=%d\ngelists=%d\nlensflare=%d\ncpurows=%d\nconfirm=%d\nlanguage=%d\ndateformat=%d\ntimeformat=%d\ncpuclock=%d\nvolume=%d\nanisotropy=%d\ndiscfonts=%d\n",
		st.screenMode, st.bilinear ? 1 : 0, st.frameskip, st.speedLimit ? 1 : 0,
		st.showFps ? 1 : 0, st.logLevel, st.splineQuality, st.collectStats ? 1 : 0,
		st.renderer, st.resolution, st.vramSyncEveryFrame ? 1 : 0, st.cpuJit ? 1 : 0, st.sound ? 1 : 0, st.pspMem64 ? 1 : 0, st.fast3d, st.geThread ? 1 : 0, st.vmx ? 1 : 0, st.spuVertex ? 1 : 0, st.vertexRingMain ? 1 : 0, st.spuTexture ? 1 : 0, st.jitProfile ? 1 : 0,
		st.textureCheckFull ? 1 : 0, st.writeBackAllRows ? 1 : 0, st.rttCopyReuse ? 1 : 0, st.tlMerge ? 1 : 0, st.jitWarmup ? 1 : 0, st.rsxTiled, st.geTiming, st.fpsCap, st.debugButtons ? 1 : 0, st.quitCombo ? 1 : 0, st.geAsync ? 1 : 0, st.rightStick, st.videos, st.ftz, st.frameskipKeep ? 1 : 0, st.volatileWipe ? 1 : 0, st.lateDepth ? 1 : 0, st.keepLayouts ? 1 : 0, st.audioSlow, st.sasDirect ? 1 : 0, st.ioHost ? 1 : 0, st.jitLarge ? 1 : 0, st.geListThread, st.lensFlare ? 1 : 0, st.cpuRows, st.confirmButton, st.language, st.dateFormat, st.timeFormat, st.cpuClock, st.volume, st.anisotropy, st.discFonts ? 1 : 0);
	std::string text = buf;
	text += "fxaa=" + std::to_string(st.fxaa) + "\n";
	text += "nickname=" + st.nickname + "\n";
	text += "gamesort=" + std::to_string(st.gameSort) + "\n";
	// Debug tab.
	char debugText[256];
	snprintf(debugText, sizeof(debugText), "perflog=%d\nstalllog=%d\nvideolog=%d\nsavelog=%d\ngewatchlog=%d\nrsxlog=%d\njitlog=%d\nl2r2=%d\nnanwatch=%d\nstorewatch=%d\njitgeneric=%d\n",
		st.logPerf, st.logStall ? 1 : 0, st.logVideo ? 1 : 0, st.logSave ? 1 : 0, st.logGeWatch ? 1 : 0, st.logRsx ? 1 : 0, st.logJit ? 1 : 0,
		st.l2r2, st.nanWatch, st.storeWatch ? 1 : 0, st.jitGeneric);
	text += debugText;
	text += "snddebug=" + std::to_string(st.soundDebug) + "\n";
	text += std::string("dumpram=") + (st.dumpRam ? "1" : "0") + "\ndumpvram=" + (st.dumpVram ? "1" : "0") + "\n";
	text += "videodeblock=" + std::to_string(st.videoDeblock) + "\n";
	text += std::string("ingamemenu=") + (st.inGameMenu ? "1" : "0") + "\nr3fps=" + (st.r3Fps ? "1" : "0") + "\n";
	text += "videocap=" + std::to_string(st.videoCap) + "\n";
	return text;
}

// Only rewrites settings.ini when a value really changed, so a file edited or copied in by hand
// (with comments, or while the app was running) is left exactly as it is otherwise. Per-game files
// in games/ are never written.
// The Debug tab's log groups for the core (FrontendLogOn); none without Debug.
static void ApplyLogMask() {
	unsigned mask = 0;
	if (settings.debugButtons) {
		if (settings.logVideo) mask |= (unsigned)FrontendLog::VIDEO;
		if (settings.logSave) mask |= (unsigned)FrontendLog::SAVEDATA;
		if (settings.logGeWatch) mask |= (unsigned)FrontendLog::GE_WATCH;
		if (settings.logRsx) mask |= (unsigned)FrontendLog::RSX;
		if (settings.logJit) mask |= (unsigned)FrontendLog::JIT;
		if (settings.soundDebug) mask |= (unsigned)FrontendLog::SOUND;
	}
	g_frontendLogMask = mask;
	g_sasMixOnPpu = settings.debugButtons && settings.soundDebug == 2;
}

static void SaveSettings() {
	const std::string wanted = SettingsText(settings);
	{
		const PS3Frontend::Settings current = settings;
		const bool exists = LoadSettingsFrom(SettingsPath());
		const std::string onDisk = SettingsText(settings);
		settings = current;
		if (exists && onDisk == wanted)
			return;
	}
	FILE *f = fopen(SettingsPath().c_str(), "w");
	if (!f) { PS3Frontend::Trace("Could not save settings.ini"); return; }
	fputs(wanted.c_str(), f);
	fclose(f);
	PS3Frontend::Trace("settings.ini updated");
}

const PS3Frontend::Settings &PS3Frontend::GetSettings() { return settings; }

// Frame cap choices (fps): gameplay, and during a slow video.
static const int kFpsCaps[3] = {60, 30, 20};
static const int kVideoCaps[4] = {60, 40, 30, -1};
template <int N>
static int CycleCap(const int (&caps)[N], int value, int delta) {
	int i = 0;
	while (i < N && caps[i] != value) ++i;
	return caps[((i < N ? i : 0) + N + (delta < 0 ? -1 : 1)) % N];
}
static std::string VideoCapName(int cap) {
	return cap == 60 ? std::string("Off") : cap < 0 ? std::string("Auto (when slow)") : std::to_string(cap) + " fps";
}

// "Anisotropic filtering" steps.
static const int kAniso[5] = {0, 2, 4, 8, 16};
static int AnisoIndex(int a) { return a >= 16 ? 4 : a >= 8 ? 3 : a >= 4 ? 2 : a >= 2 ? 1 : 0; }
static std::string AnisoName(int a) { return a ? std::to_string(a) + "x" : std::string("Off"); }

// A PS3 system setting (sysutil), or fallback when it can't be read.
static int Ps3SystemParam(s32 id, int fallback) {
	s32 value = 0;
	return sysUtilGetSystemParamInt(id, &value) == 0 ? (int)value : fallback;
}

// The PSP system settings games read (confirm button, language, date and time format, nickname),
// the emulated CPU clock and the game volume. "Auto" values come from the PS3's own settings.
static void ApplySystemSettings() {
	const int ps3Enter = Ps3SystemParam(SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN, 1);
	const int ps3Lang = Ps3SystemParam(SYSUTIL_SYSTEMPARAM_ID_LANG, SYSUTIL_LANG_ENGLISH_US);
	const int ps3Date = Ps3SystemParam(SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT, 0);
	const int ps3Time = Ps3SystemParam(SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT, 1);
	// PS3 enter button: 0 Circle, 1 X (the same values as the PSP's).
	g_Config.iButtonPreference = settings.confirmButton >= 0 ? settings.confirmButton :
		(ps3Enter == 0 ? PSP_SYSTEMPARAM_BUTTON_CIRCLE : PSP_SYSTEMPARAM_BUTTON_CROSS);
	if (settings.language >= 0) {
		g_Config.iLanguage = settings.language;
	} else if (ps3Lang >= SYSUTIL_LANG_JAPANESE && ps3Lang <= SYSUTIL_LANG_CHINESE_S) {
		g_Config.iLanguage = ps3Lang;  // 0-11 are the same languages in the same order on both.
	} else if (ps3Lang == SYSUTIL_LANG_PORTUGUESE_BR) {
		g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_PORTUGUESE;
	} else {
		g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;  // Languages the PSP doesn't have.
	}
	// PS3 date format: 0 YYYY/MM/DD, 1 DD/MM/YYYY, 2 MM/DD/YYYY. Time: 0 12-hour, 1 24-hour.
	static const int dateFromPs3[3] = {PSP_SYSTEMPARAM_DATE_FORMAT_YYYYMMDD, PSP_SYSTEMPARAM_DATE_FORMAT_DDMMYYYY, PSP_SYSTEMPARAM_DATE_FORMAT_MMDDYYYY};
	g_Config.iDateFormat = settings.dateFormat >= 0 ? settings.dateFormat :
		(ps3Date >= 0 && ps3Date <= 2 ? dateFromPs3[ps3Date] : PSP_SYSTEMPARAM_DATE_FORMAT_YYYYMMDD);
	g_Config.iTimeFormat = settings.timeFormat >= 0 ? settings.timeFormat :
		(ps3Time == 0 ? PSP_SYSTEMPARAM_TIME_FORMAT_12HR : PSP_SYSTEMPARAM_TIME_FORMAT_24HR);
	std::string nick = settings.nickname;
	if (nick.empty()) {
		char buf[SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE + 1] = {};
		if (sysUtilGetSystemParamString(SYSUTIL_SYSTEMPARAM_ID_NICKNAME, buf, SYSUTIL_SYSTEMPARAM_NICKNAME_SIZE) == 0)
			nick = buf;
	}
	// Games keep the nickname in small buffers: at most 32 bytes, not cutting a UTF-8 character.
	if (nick.size() > 32) {
		size_t n = 32;
		while (n > 0 && ((unsigned char)nick[n] & 0xC0) == 0x80) n--;
		nick.resize(n);
	}
	g_Config.sNickName = nick.empty() ? std::string("PPSSPP") : nick;
	g_Config.iLockedCPUSpeed = settings.cpuClock;
	g_Config.iGameVolume = settings.volume;
	RSX::SetMaxAnisotropy(settings.anisotropy);
	PS3Frontend::Trace("PSP system settings: confirm %s, language %d, date format %d, time format %d, nickname \"%s\" (PS3: enter %d, language %d, date %d, time %d); CPU clock %s; volume %d%%",
		g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CROSS ? "X" : "Circle", g_Config.iLanguage, g_Config.iDateFormat,
		g_Config.iTimeFormat, g_Config.sNickName.c_str(), ps3Enter, ps3Lang, ps3Date, ps3Time,
		settings.cpuClock > 0 ? (std::to_string(settings.cpuClock) + " MHz").c_str() : "the game's own", settings.volume);
}

void PS3Frontend::ApplyCoreSettings() {
	// The headless harness forces some values for deterministic tests; a game wants the app's.
	g_Config.bSkipDeadbeefFilling = true;  // App default: don't scribble 0xDEADBEEF over freed memory.
	g_Config.bIgnoreBadMemAccess = true;   // App default: a stray access doesn't stop the game.
	// Memory stick size the games see: 1 GB. With PPSSPP's 16 GB, the free space in bytes doesn't fit
	// the 32 bits games of this age use for it (16 GB minus the saves comes out as a negative number
	// or zero), and some report "not enough free space" though there's plenty. 1 GB is far more
	// than any PSP game's saves need.
	g_Config.iMemStickSizeGB = 1;
	g_flushDenormalsSetting = settings.ftz;
	g_mediaVideoDecode = settings.videos;
	g_mediaDeblock = settings.videoDeblock;
	g_frameSkipKeepOneOff = settings.frameskipKeep;
	g_utilityWipesVolatile = settings.volatileWipe;
	g_Config.iSplineBezierQuality = settings.splineQuality;
	g_Config.bEnableSound = settings.sound;
	// The SDK reports one core, which would mix sceSas voices on the emulation thread. Mix them on
	// their own thread instead (which hands the work to an SPU when it can, see Core/HW/SasSpu) -
	// unless "Sound mixing" is on the emulation thread (build 141): then the emulation thread
	// gives the SPU the work itself and polls for the answer, without the thread hand-overs.
	g_Config.bSeparateSASThread = !settings.sasDirect;
	g_sasSpuPoll = settings.sasDirect;
	// "PSP fonts" (build 167): the game's own font library with the firmware's fonts from its disc.
	g_fontsFromDisc = settings.discFonts;
	g_vfpuNaNHook = settings.debugButtons ? &VfpuNaNHook : nullptr;
	// NaN watch (build 169): IR interpreter only, Debug on, nanwatch=1 in a per-game file.
	MIPSComp::g_irNaNWatch = settings.nanWatch > 0 && settings.debugButtons && !settings.cpuJit;
	// nanwatch: 1 FPU NaNs, +2 VFPU registers too, +4 infinities too (7 = all).
	MIPSComp::g_irNaNWatchVfpu = (settings.nanWatch & 2) != 0;
	MIPSComp::g_irNaNWatchInf = (settings.nanWatch & 4) != 0;
	MIPSComp::g_irNaNWatchLog = MIPSComp::g_irNaNWatch ? &IrNaNWatchLog : nullptr;
	{
		// Store watch (build 176): IR interpreter only, Debug on, storewatch=1 in a per-game file.
		const bool on = settings.storeWatch && settings.debugButtons && !settings.cpuJit;
		MIPSComp::IRStoreWatchEnable(on);
		g_storeWatchReport = on ? &StoreWatchReport : nullptr;
		if (settings.storeWatch)
			PS3Frontend::Trace("Store watch: %s", MIPSComp::g_irStoreWatch ? "on (IR interpreter)" : (on ? "off - no memory for its log" : "off - needs CPU core IR interpreter (cpujit=0) and Debug on"));
	}
	// JIT bisect (build 180): jitgeneric=<groups> in a per-game file.
	MIPSComp::g_irNativeGenericMask = (uint32_t)settings.jitGeneric;
	{
		// Build 183: jitops=<IR op names> in a per-game file, single instructions.
		std::string found, unknown;
		for (int op = 0; op < 256; ++op) {
			MIPSComp::g_irNativeGenericOp[op] = false;
		}
		InitIR();  // The op names table is filled when the JIT starts: not yet here.
		std::string token;
		const std::string list = settings.jitOps + ",";
		for (char ch : list) {
			if (ch == ',' || ch == ' ' || ch == ';' || ch == '\t') {
				if (!token.empty()) {
					bool ok = false;
					for (int op = 0; op < 256; ++op) {
						const IRMeta *meta = GetIRMeta((IROp)op);
						if (meta && meta->name && token == meta->name) {
							MIPSComp::g_irNativeGenericOp[op] = true;
							ok = true;
						}
					}
					(ok ? found : unknown) += " " + token;
					token.clear();
				}
			} else {
				token += ch;
			}
		}
		if (!found.empty() || !unknown.empty())
			PS3Frontend::Trace("JIT: single instructions left to the interpreter (jitops):%s%s%s", found.c_str(), unknown.empty() ? "" : "; unknown names:", unknown.c_str());
	}
	if (settings.jitGeneric)
		PS3Frontend::Trace("JIT: left to the interpreter (jitgeneric=%d):%s%s%s%s%s%s%s", settings.jitGeneric,
			(settings.jitGeneric & 1) ? " integer ALU" : "", (settings.jitGeneric & 2) ? " integer loads/stores" : "",
			(settings.jitGeneric & 4) ? " float/vector loads/stores" : "", (settings.jitGeneric & 8) ? " FPU arithmetic" : "",
			(settings.jitGeneric & 16) ? " GPR<->FPR moves" : "", (settings.jitGeneric & 32) ? " vector ops" : "",
			(settings.jitGeneric & 64) ? " (no fused patterns or shortcuts)" : "");
	if (settings.nanWatch)
		PS3Frontend::Trace("NaN watch: %s", MIPSComp::g_irNaNWatch ? ((settings.nanWatch & 2) ? ((settings.nanWatch & 4) ? "on, NaN and infinity, FPU and VFPU registers (IR interpreter)" : "on, FPU and VFPU registers (IR interpreter)") : ((settings.nanWatch & 4) ? "on, NaN and infinity, FPU registers (IR interpreter)" : "on, FPU registers (IR interpreter)")) : "off - needs CPU core IR interpreter (cpujit=0) and Debug on");
	// "File loading" (build 142): Host lets the game run on while the IO thread reads.
	g_Config.iIOTimingMethod = settings.ioHost ? IOTIMING_HOST : IOTIMING_FAST;
	// 32 MB (PSP-1000) unless asked: every MB counts on the PS3, and the XMB seems to need some.
	g_Config.iPSPModel = settings.pspMem64 ? PSP_MODEL_SLIM : PSP_MODEL_FAT;
	if (settings.frameskip < 0) {
		// Auto: skip drawing only when behind, up to 4 frames in a row (min ~12 fps drawn).
		g_Config.iFrameSkip = 4;
		g_Config.bAutoFrameSkip = true;
	} else {
		g_Config.iFrameSkip = settings.frameskip;
		g_Config.bAutoFrameSkip = false;
	}
	g_geTimingPercent = settings.geTiming;
	g_displayMinVblanksPerFlip = 60 / settings.fpsCap;
	g_displayVideoCapFps = settings.videoCap;
	ApplySystemSettings();
	Trace("Core settings: frameskip=%d auto=%d speedlimit=%d spline=%d log=%d",
		g_Config.iFrameSkip, g_Config.bAutoFrameSkip ? 1 : 0, settings.speedLimit ? 1 : 0,
		g_Config.iSplineBezierQuality, settings.logLevel);
}

// ---------------------------------------------------------------------------------------------
// Drawing helpers (main thread for menus, presenter thread in game; never both at once)

static void Text(int x, int y, const char *value, int scale = 1) {
	for (; *value; ++value, x += 12 * scale) {
		unsigned c = (unsigned char)*value;
		if (c < 32 || c > 126) c = '?';
		c -= 32;
		SDL_Rect source = {(int)(c % 16) * 12, (int)(c / 16) * 24, 12, 24};
		SDL_Rect destination = {x, y, 12 * scale, 24 * scale};
		SDL_RenderCopy(renderer, font, &source, &destination);
	}
}
static void Clear() { SDL_SetRenderDrawColor(renderer, 12, 18, 28, 255); SDL_RenderClear(renderer); }

// Menu screens are composed on the CPU (Canvas) and copied to the screen in one transfer. The
// canvas takes 7.4 MB of main memory and 7.4 MB of RSX memory, so it's released before a game starts;
// screens after that use the plain text path.
static bool MenuCanvasReady() { return Canvas::Ready() && canvasTextures[0] && canvasTextures[1]; }

static void MenuCanvasInit() {
	if (!Canvas::Init(fontSmall, fontLargeLoaded ? fontLarge : nullptr)) {
		PS3Frontend::Trace("Menu canvas: not enough memory, plain menus");
		return;
	}
	for (auto &texture : canvasTextures)
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Canvas::W, Canvas::H);
	if (!canvasTextures[0] || !canvasTextures[1]) {
		PS3Frontend::Trace("Menu canvas: texture failed (%s), plain menus", SDL_GetError());
		for (auto &texture : canvasTextures) {
			if (texture) SDL_DestroyTexture(texture);
			texture = nullptr;
		}
		Canvas::Release();
	}
}

static void MenuCanvasRelease() {
	for (auto &texture : canvasTextures) {
		if (texture) SDL_DestroyTexture(texture);
		texture = nullptr;
	}
	Canvas::Release();
}

static void MenuCanvasPresent() {
	SDL_Texture *texture = canvasTextures[canvasFlip];
	canvasFlip ^= 1;
	SDL_UpdateTexture(texture, nullptr, Canvas::Pixels(), Canvas::W * 4);
	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	SDL_RenderPresent(renderer);
}

void PS3Frontend::Status(const char *message) {
	Trace("Status: %s", message);
	if (!renderer || !font) return;
	if (MenuCanvasReady()) {
		MenuScreens::Message(time_now_d(), FRONTEND_NAME, message,
			std::string(settings.cpuJit ? "JIT" : "IR interpreter") + (settings.renderer ? " / RSX hardware renderer" : " / software graphics") + (settings.sound ? " / sound on" : " / sound off"),
			// One line of at most 94 characters (MenuScreens::Message clips the rest).
			!settings.inGameMenu && !settings.r3Fps && !settings.debugButtons ? std::string("PS button: quit the game") :
				std::string("PS: quit") + (settings.inGameMenu ? "  START+SELECT: options" : "") + (settings.r3Fps ? "  L3+R3: FPS" : "") +
				(settings.debugButtons ? std::string("  L2+R2: ") + (settings.l2r2 ? "draw log" : "GE dump") + "  hold L3: JIT profile" : std::string()));
		MenuCanvasPresent();
		return;
	}
	Clear(); Text(50, 50, FRONTEND_NAME, 2);
	Text(50, 155, std::string(message).substr(0, 94).c_str());
	Text(50, 570, (std::string(settings.cpuJit ? "JIT (experimental)" : "IR interpreter") + (settings.renderer ? " / RSX hardware renderer" : " / software graphics") + (settings.sound ? " / sound on" : " / sound off")).c_str());
	Text(50, 625, "PS button: quit the game");
	SDL_RenderPresent(renderer);
}

// The menu background with one line of text in the middle, nothing else (Quit to game list). After
// a game the canvas was released, so it is set up again first.
static void ShowSplash(const char *text) {
	PS3Frontend::Trace("Status: %s", text);
	if (!renderer || !font) return;
	if (!MenuCanvasReady())
		MenuCanvasInit();
	if (MenuCanvasReady()) {
		MenuScreens::Splash(time_now_d(), text);
		MenuCanvasPresent();
		return;
	}
	Clear();
	Text((1280 - (int)strlen(text) * 24) / 2, 336, text, 2);
	SDL_RenderPresent(renderer);
}

// ---------------------------------------------------------------------------------------------
// PSP framebuffer -> ARGB8888 conversion

enum { PSP_W = 480, PSP_H = 272 };

// 16-bit PSP pixels are little-endian bytes in memory; one table per format maps them straight to
// opaque ARGB8888 (0xFFRRGGBB, which the RSX reads as A8R8G8B8 on this big-endian host).
static u32 *g_lut[3];

static inline u32 Expand5(u32 v) { return (v << 3) | (v >> 2); }
static inline u32 Expand6(u32 v) { return (v << 2) | (v >> 4); }

static const u32 *Lut(int fmt) {
	if (g_lut[fmt]) return g_lut[fmt];
	u32 *t = new u32[65536];
	for (u32 v = 0; v < 65536; ++v) {
		u32 r, g, b;
		if (fmt == GPU_DBG_FORMAT_565) {
			r = Expand5(v & 31); g = Expand6((v >> 5) & 63); b = Expand5((v >> 11) & 31);
		} else if (fmt == GPU_DBG_FORMAT_5551) {
			r = Expand5(v & 31); g = Expand5((v >> 5) & 31); b = Expand5((v >> 10) & 31);
		} else {
			r = (v & 15) * 17; g = ((v >> 4) & 15) * 17; b = ((v >> 8) & 15) * 17;
		}
		t[v] = 0xFF000000 | (r << 16) | (g << 8) | b;
	}
	g_lut[fmt] = t;
	return t;
}

// Returns false if the buffer is in a layout this fast path doesn't handle.
static bool ConvertFramebuffer(const GPUDebugBuffer &buffer, u32 *dst) {
	if (buffer.GetFlipped()) return false;
	const u32 stride = buffer.GetStride();
	if (stride < PSP_W || buffer.GetHeight() < PSP_H) return false;
	const u8 *src = buffer.GetData();
	const int fmt = buffer.GetFormat();
	switch (fmt) {
	case GPU_DBG_FORMAT_565:
	case GPU_DBG_FORMAT_5551:
	case GPU_DBG_FORMAT_4444: {
		const u32 *lut = Lut(fmt);
		for (int y = 0; y < PSP_H; ++y) {
			const u8 *row = src + y * stride * 2;
			u32 *out = dst + y * PSP_W;
			for (int x = 0; x < PSP_W; ++x)
				out[x] = lut[row[x * 2] | (row[x * 2 + 1] << 8)];
		}
		return true;
	}
	case GPU_DBG_FORMAT_8888:  // Bytes R, G, B, A.
		for (int y = 0; y < PSP_H; ++y) {
			const u8 *row = src + y * stride * 4;
			u32 *out = dst + y * PSP_W;
			for (int x = 0; x < PSP_W; ++x)
				out[x] = 0xFF000000 | (row[x * 4] << 16) | (row[x * 4 + 1] << 8) | row[x * 4 + 2];
		}
		return true;
	case GPU_DBG_FORMAT_8888_BGRA:  // Bytes B, G, R, A.
		for (int y = 0; y < PSP_H; ++y) {
			const u8 *row = src + y * stride * 4;
			u32 *out = dst + y * PSP_W;
			for (int x = 0; x < PSP_W; ++x)
				out[x] = 0xFF000000 | (row[x * 4 + 2] << 16) | (row[x * 4 + 1] << 8) | row[x * 4];
		}
		return true;
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------------------------
// Presenter thread. SDL_RenderPresent on PSL1GHT blocks until the vsync flip completes, which
// stalled emulation for up to a full refresh every frame. Now the emulation thread converts the
// frame into a free slot and moves on; this thread uploads, scales (RSX) and flips. If it falls
// behind, the newest frame replaces the waiting one (mailbox), so emulation never waits on it.

static std::thread presenterThread;
static std::mutex presentMutex;
static std::string pendingVramPath;  // GE dump: save the RSX's frame at the next Present (build 181).
static std::string pendingRamPath;   // GE dump: save PSP RAM at the next Present (build 182).
static std::condition_variable presentCond;
static bool presenterActive = false;
static bool presenterQuit = false;
static u32 *slots[3];
static int readySlot = -1;
static int busySlot = -1;
static std::string overlayText;
static std::atomic<int> clearFrames{0};
static std::atomic<unsigned> droppedFrames{0};
static std::atomic<unsigned> flips{0};
// Presents that showed a new picture (the display buffer was drawn since the last one, or a video
// plays): the FPS overlay's number. A game that keeps one display buffer and copies each finished
// frame into it (Midnight Club) is otherwise shown again at vblanks in between, which counted as
// frames too.
static unsigned newPictures = 0;
bool SoftGPUTakeNewPicture();
static SDL_Rect gameRect;

static void ComputeGameRect() {
	SDL_DisplayMode mode{};
	int w = 1280, h = 720;
	if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0) {
		w = mode.w; h = mode.h;
	}
	int rw = w, rh = h;
	switch ((PS3Frontend::ScreenMode)settings.screenMode) {
	case PS3Frontend::ScreenMode::STRETCH:
		break;
	case PS3Frontend::ScreenMode::PIXEL_2X:
		rw = std::min(w, PSP_W * 2); rh = std::min(h, PSP_H * 2);
		break;
	case PS3Frontend::ScreenMode::FIT:
	case PS3Frontend::ScreenMode::FIT_90:
	default:
		rh = h; rw = (h * PSP_W + PSP_H / 2) / PSP_H;
		if (rw > w) { rw = w; rh = (w * PSP_H + PSP_W / 2) / PSP_W; }
		if (settings.screenMode == (int)PS3Frontend::ScreenMode::FIT_90) { rw = rw * 9 / 10; rh = rh * 9 / 10; }
		break;
	}
	gameRect = {(w - rw) / 2, (h - rh) / 2, rw, rh};
	PS3Frontend::Trace("Output %dx%d, game rectangle %dx%d at %d,%d (screen mode %d, %s)", w, h, rw, rh,
		gameRect.x, gameRect.y, settings.screenMode, settings.bilinear ? "bilinear" : "nearest");
}

static void PresenterLoop() {
	PS3Frontend::Trace("Presenter thread started");
	while (true) {
		int slot;
		std::string overlay;
		{
			std::unique_lock<std::mutex> lock(presentMutex);
			presentCond.wait(lock, [] { return presenterQuit || readySlot >= 0; });
			if (presenterQuit) break;
			slot = readySlot; readySlot = -1; busySlot = slot;
			overlay = overlayText;
		}
		SDL_UpdateTexture(screen, nullptr, slots[slot], PSP_W * 4);
		// The SDL clear is a CPU fill of the whole RSX framebuffer - skip it except right after
		// starting (both display buffers), since the game rectangle covers the same pixels each time.
		if (clearFrames.load() > 0) {
			SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
			SDL_RenderClear(renderer);
			clearFrames--;
		}
		SDL_RenderCopy(renderer, screen, nullptr, &gameRect);
		if (!overlay.empty())
			Text(gameRect.x + 8, gameRect.y + 6, overlay.c_str());
		SDL_RenderPresent(renderer);
		flips++;
		std::lock_guard<std::mutex> lock(presentMutex);
		busySlot = -1;
	}
	PS3Frontend::Trace("Presenter thread stopped");
}

static void StartPresenter() {
	for (auto &slot : slots) {
		if (!slot) slot = new u32[PSP_W * PSP_H];
		std::fill(slot, slot + PSP_W * PSP_H, 0xFF000000);
	}
	// Game drawing uses real output pixels, not the menus' 1280x720 logical canvas.
	SDL_RenderSetLogicalSize(renderer, 0, 0);
	ComputeGameRect();
	PS3FrontendSetBilinear(settings.bilinear);
	readySlot = busySlot = -1;
	presenterQuit = false;
	clearFrames = 3;
	presenterActive = true;
	presenterThread = std::thread(PresenterLoop);
}

static void StopPresenter() {
	if (!presenterActive) return;
	{
		std::lock_guard<std::mutex> lock(presentMutex);
		presenterQuit = true;
	}
	presentCond.notify_one();
	presenterThread.join();
	presenterActive = false;
	SDL_RenderSetLogicalSize(renderer, 1280, 720);
}

// ---------------------------------------------------------------------------------------------
// Frame-time profiling

static double lastPresentEnd = 0.0;
static double statStart = 0.0;
static double statEmu = 0.0, statGE = 0.0, statFlush = 0.0, statConvert = 0.0;
static double statPresentGESync = 0.0;  // Of statFlush: waiting for the GE worker thread.
static unsigned statFrames = 0;
static u64 statEmuUsStart = 0;
static unsigned statFlipsStart = 0, statDroppedStart = 0, statNewPicturesStart = 0;

// JIT profile: where the PSP CPU time goes, block by block, into launch.log.
// - L3 (left stick click) held half a second during a game: a 5 second sample right now (a busy scene, an explosion).
//   With "jitsample" off this clears the JIT cache to compile blocks with entry counters (a short
//   pause first), and once more afterwards to go back to code without them.
// - "jitsample=1": the counters are compiled in all game long (a little slower), so L3 samples start
//   with no pause, and a 10 s sample is also taken automatically every minute, five times.
static std::atomic<bool> jitProfileRequest{false};
static std::string noticeText;   // Shown on screen for a few seconds (presentMutex).
static double noticeUntil = 0.0;

static void ShowNotice(const char *text, double seconds) {
	std::lock_guard<std::mutex> lock(presentMutex);
	noticeText = text;
	noticeUntil = time_now_d() + seconds;
}

static void JitProfileTick(double now, double frames) {
	static int phase = 0;  // 0 waiting, 1 measuring, 2 warming up after the cache clear.
	static int autoSamples = 0;
	static bool manual = false, clearAfter = false;
	static double nextAuto = 0.0, sampleStart = 0.0, sampleLength = 10.0, sampleFrames = 0.0;
	if (!settings.cpuJit || !MIPSComp::jit)
		return;
	// Debug tab "JIT profile" (jitsample): only with Debug on (without it the reports go nowhere).
	const bool jitSample = settings.jitProfile && settings.debugButtons;
	if (jitSample && !PS3Profile::jitBlockProfile && phase == 0) {
		// Counters on for the whole game (blocks compiled from here on carry them).
		PS3Profile::jitBlockProfile = true;
		MIPSComp::jit->ClearCache();
		nextAuto = now + 40.0;
		PS3Frontend::Trace("JIT profile: block counters on (jitsample=1); hold L3 for a 5 s sample at any time");
		return;
	}
	const bool request = jitProfileRequest.exchange(false);
	if (phase == 0) {
		const bool autoDue = jitSample && autoSamples < 5 && nextAuto != 0.0 && now >= nextAuto;
		if (!request && !autoDue)
			return;
		manual = request;
		sampleLength = manual ? 5.0 : 10.0;
		memset(PS3Profile::jitBlockCounts, 0, sizeof(PS3Profile::jitBlockCounts));
		memset(PS3Profile::jitBlockTicks, 0, sizeof(PS3Profile::jitBlockTicks));
		// The first entry charges a slot no block uses.
		PS3Profile::jitProfState.lastTb = PS3Profile::Now();
		PS3Profile::jitProfState.lastBlockX4 = (PS3Profile::JIT_BLOCK_COUNTS - 1) * 4;
		clearAfter = !PS3Profile::jitBlockProfile;
		if (clearAfter) {
			PS3Profile::jitBlockProfile = true;
			// Present runs between CPU slices (the headless loop), not inside the JIT: clear right away.
			// (ClearJitCacheDeferred would end the run loop, which stopped the game.)
			MIPSComp::jit->ClearCache();
		}
		sampleStart = now;
		sampleFrames = 0.0;
		// After a cache clear, everything is compiled again for a while: measure only after 3 s
		// (compiles and warm-up interpreter runs are also left out of block time).
		phase = clearAfter ? 2 : 1;
		PS3Frontend::Trace("JIT profile: measuring for %.0f s (%s%s)", sampleLength, manual ? "L3 held" : "automatic sample",
			clearAfter ? "; JIT cache cleared, a short pause is expected, measuring starts 3 s later" : "");
		if (manual)
			ShowNotice(clearAfter ? "JIT profile: measuring (8 s)" : "JIT profile: measuring 5 s", sampleLength + (clearAfter ? 3.0 : 0.0));
		return;
	}
	if (phase == 2) {
		if (now - sampleStart < 3.0)
			return;
		memset(PS3Profile::jitBlockCounts, 0, sizeof(PS3Profile::jitBlockCounts));
		memset(PS3Profile::jitBlockTicks, 0, sizeof(PS3Profile::jitBlockTicks));
		PS3Profile::jitProfState.lastTb = PS3Profile::Now();
		PS3Profile::jitProfState.lastBlockX4 = (PS3Profile::JIT_BLOCK_COUNTS - 1) * 4;
		sampleStart = now;
		sampleFrames = 0.0;
		phase = 1;
		return;
	}
	sampleFrames += frames;
	if (now - sampleStart < sampleLength)
		return;
	JitBlockCacheDebugInterface *cache = MIPSComp::jit->GetBlockCacheDebugInterface();
	struct Hot { uint64_t ticks; uint32_t block, addr, instrs, entries, native; };
	std::vector<Hot> hot;
	uint64_t totalTicks = 0, totalInstrs = 0;
	const int n = cache ? std::min(cache->GetNumBlocks(), PS3Profile::JIT_BLOCK_COUNTS - 1) : 0;
	for (int i = 0; i < n; ++i) {
		const uint32_t c = PS3Profile::jitBlockCounts[i];
		if (!c)
			continue;
		const JitBlockMeta meta = cache->GetBlockMeta(i);
		if (!meta.valid)
			continue;
		const uint32_t instrs = std::max(1u, meta.sizeInBytes / 4);
		const uint32_t t = PS3Profile::jitBlockTicks[i];
		totalTicks += t;
		totalInstrs += (uint64_t)c * instrs;
		hot.push_back({t, (uint32_t)i, meta.addr, instrs, c, PS3Profile::jitBlockNativeBytes[i] / 4u});
	}
	std::sort(hot.begin(), hot.end(), [](const Hot &a, const Hot &b) { return a.ticks > b.ticks; });
	const double f = std::max(1.0, sampleFrames);
	// Time base ticks are 40 PPU cycles (79.8 MHz vs 3.2 GHz).
	std::string report;
	Appendf(report, "JIT profile: %.1f s, %.0f frames, %d blocks run; ~%.0fk MIPS instructions and %.1f ms in blocks per frame "
		"(%.0f PPU cycles per MIPS instruction, profiling included; a block's time runs to the next block's start, HLE calls and compiling left out).",
		now - sampleStart, sampleFrames, (int)hot.size(), totalInstrs / f / 1000.0, totalTicks / 79800.0 / f,
		totalInstrs ? totalTicks * 40.0 / totalInstrs : 0.0);
	Appendf(report, "JIT profile: block     MIPS  PPC   entries/frame  cycles/entry  cycles/MIPS instr  share of time");
	for (size_t i = 0; i < hot.size() && i < 30; ++i) {
		const Hot &h = hot[i];
		const double cyc = h.ticks * 40.0 / h.entries;
		Appendf(report, "JIT profile: %08x %4u %5u %10.0f %13.0f %12.1f %12.1f%%", h.addr, h.instrs, h.native,
			h.entries / f, cyc, cyc / h.instrs, totalTicks ? 100.0 * h.ticks / totalTicks : 0.0);
	}
	// The code of the busiest blocks (a block whose time is mostly an HLE call shows which call).
	int shown = 0;
	for (size_t i = 0; i < hot.size() && shown < 12; ++i) {
		const Hot &h = hot[i];
		shown++;
		const JitBlockDebugInfo info = cache->GetBlockDebugInfo((int)h.block);
		Appendf(report, "JIT block %08x: %u MIPS instructions, %u IR, %u PPC", h.addr, h.instrs, (unsigned)info.irDisasm.size(), h.native);
		for (size_t k = 0; k < info.origDisasm.size() && k < 48; ++k)
			Appendf(report, "  mips %s", info.origDisasm[k].c_str());
		for (size_t k = 0; k < info.irDisasm.size() && k < 64; ++k)
			Appendf(report, "  ir   %s", info.irDisasm[k].c_str());
		const uint32_t *code = (const uint32_t *)(PS3Jit::CodeArea() + PS3Profile::jitBlockNativeOffset[h.block]);
		const uint32_t words = std::min(h.native, 256u);
		for (uint32_t k = 0; k < words; k += 8) {
			char line[96];
			int len = snprintf(line, sizeof(line), "  ppc ");
			for (uint32_t m = k; m < words && m < k + 8; ++m)
				len += snprintf(line + len, sizeof(line) - len, " %08x", code[m]);
			Appendf(report, "%s", line);
		}
	}
	TraceReport(report);
	if (clearAfter) {
		PS3Profile::jitBlockProfile = false;
		MIPSComp::jit->ClearCache();
	}
	if (manual) {
		ShowNotice("JIT profile saved to launch.log", 3.0);
	} else {
		// Again a minute later (a race, a busy scene...), five samples in all.
		autoSamples++;
		nextAuto = now + 50.0;
	}
	phase = 0;
}

// When a game crawls (a frame a second or less, as Dead to Rights did after a while): what the
// running PSP thread is doing (registers, the code around its PC and return address) and where
// each GE display list is, so a hang can be told from a slow scene. A few times per game.
static int slowDiagnostics = 0;
static void SlowDiagnostics() {
	if (slowDiagnostics >= 3 || !currentMIPS)
		return;
	slowDiagnostics++;
	std::string slowReport;
	const MIPSState *m = currentMIPS;
	static const char *const names[32] = { "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
		"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra" };
	for (int row = 0; row < 32; row += 8) {
		char line[192];
		int len = snprintf(line, sizeof(line), "Slow: regs");
		for (int i = row; i < row + 8; ++i)
			len += snprintf(line + len, sizeof(line) - len, " %s=%08x", names[i], m->r[i]);
		Appendf(slowReport, "%s", line);
	}
	Appendf(slowReport, "Slow: pc %08x hi %08x lo %08x fcr31 %08x downcount %d", m->pc, m->hi, m->lo, m->fcr31, m->downcount);
	auto dis = [&](const char *what, u32 center) {
		if (!Memory::IsValid4AlignedAddress(center))
			return;
		for (u32 a = center - 32; a <= center + 32; a += 4) {
			if (!Memory::IsValid4AlignedAddress(a))
				continue;
			char text[128];
			MIPSDisAsm(Memory::Read_Instruction(a, true), a, text, sizeof(text), true);
			Appendf(slowReport, "Slow: %s %c%08x %08x %s", what, a == center ? '>' : ' ', a, Memory::Read_Instruction(a, true).encoding, text);
		}
	};
	for (int row = 0; row < 32; row += 8) {
		char line[192];
		int len = snprintf(line, sizeof(line), "Slow: fpu");
		for (int i = row; i < row + 8; ++i)
			len += snprintf(line + len, sizeof(line) - len, " f%d=%08x", i, m->fi[i]);
		Appendf(slowReport, "%s", line);
	}
	// The stack (return addresses of the callers further up are in it).
	for (u32 row = 0; row < 0x80; row += 0x20) {
		const u32 a = m->r[MIPS_REG_SP] + row;
		if (!Memory::IsValidRange(a, 0x20))
			break;
		char line[160];
		int len = snprintf(line, sizeof(line), "Slow: stack %08x", a);
		for (u32 i = 0; i < 0x20; i += 4)
			len += snprintf(line + len, sizeof(line) - len, " %08x", Memory::ReadUnchecked_U32(a + i));
		Appendf(slowReport, "%s", line);
	}
	dis("pc", m->pc);
	dis("ra", m->r[MIPS_REG_RA]);
	if (gpu) {
		for (const DisplayList &dl : static_cast<GPUCommon *>(gpu)->ActiveDisplayLists()) {
			Appendf(slowReport, "Slow: GE list %d state %d start %08x pc %08x stall %08x signal %d stack %d interrupted %d pending interrupt %d started %d",
				dl.id, (int)dl.state, dl.startpc, dl.pc, dl.stall, (int)dl.signal, dl.stackptr, dl.interrupted ? 1 : 0, dl.pendingInterrupt ? 1 : 0, dl.started ? 1 : 0);
		}
	}
	TraceReport(slowReport);
}

static void UpdateStats(double emu, double ge, double flush, double convert) {
	double now = time_now_d();
	if (statStart == 0.0) {
		statStart = now; statEmuUsStart = CoreTiming::GetGlobalTimeUs();
		statFlipsStart = flips; statDroppedStart = droppedFrames; statNewPicturesStart = newPictures;
		return;
	}
	statEmu += emu; statGE += ge; statFlush += flush; statConvert += convert; statFrames++;
	JitProfileTick(now, 1.0);
	double elapsed = now - statStart;
	if (elapsed < 2.0 || statFrames == 0) return;
	if (!settings.debugButtons) {
		// No logs: only the FPS overlay. (Until build 217 its line was only made with the Perf report
		// below, so without Debug the overlay stayed empty, whatever R3 or the in-game options said.)
		const double tbMs = 1000.0 / 79800000.0;
		const double n = statFrames;
		static uint64_t oGe = 0, oXform = 0, oRealWait = 0;
		const double geMs = (PS3Profile::ge - oGe) * tbMs / n, xformMs = (PS3Profile::transform - oXform) * tbMs / n;
		const double realWaitMs = (PS3Profile::realTimeWait - oRealWait) * tbMs / n;
		oGe = PS3Profile::ge; oXform = PS3Profile::transform; oRealWait = PS3Profile::realTimeWait;
		const double speed = 100.0 * (double)(CoreTiming::GetGlobalTimeUs() - statEmuUsStart) / (elapsed * 1000000.0);
		const double newFps = (newPictures - statNewPicturesStart) / elapsed;
		const double cpuMs = std::max(0.0, statEmu * 1000.0 / n - geMs - realWaitMs);
		char line[160];
		snprintf(line, sizeof(line), "FPS %.1f  Speed %.0f%%  CPU %.0fms  GE %.0fms (vertex %.0f)", newFps, speed, cpuMs, geMs, xformMs);
		{
			std::lock_guard<std::mutex> lock(presentMutex);
			overlayText = settings.showFps ? line : "";
		}
		statStart = now; statEmuUsStart = CoreTiming::GetGlobalTimeUs(); statFlipsStart = flips; statDroppedStart = droppedFrames; statNewPicturesStart = newPictures;
		statEmu = statGE = statFlush = statConvert = 0.0; statFrames = 0; statPresentGESync = 0.0;
		return;
	}
	std::string perfReport;

	u64 emuUs = CoreTiming::GetGlobalTimeUs();
	double speed = 100.0 * (double)(emuUs - statEmuUsStart) / (elapsed * 1000000.0);
	double gameFps = statFrames / elapsed;
	double shownFps = (flips - statFlipsStart) / elapsed;
	const double newFps = (newPictures - statNewPicturesStart) / elapsed;
	double n = statFrames;
	// Time-base counters from the GE and the software transform (GPU/PS3Profile.h).
	static uint64_t lastGe = 0, lastXform = 0;
	const double tbMs = 1000.0 / 79800000.0;
	const double geMs = (PS3Profile::ge - lastGe) * tbMs / n, xformMs = (PS3Profile::transform - lastXform) * tbMs / n;
	lastGe = PS3Profile::ge;
	lastXform = PS3Profile::transform;
	// "GE lists" on their own thread (build 148): the emulation thread only waits for that thread.
	static uint64_t lastAsyncWait = 0, lastAsyncWaits = 0, lastAsyncGate = 0, lastAsyncGates = 0;
	const double asyncFullMs = (PS3Profile::geAsyncWait - lastAsyncWait) * tbMs / n;
	const double asyncWaits = (PS3Profile::geAsyncWaits - lastAsyncWaits) / n;
	const double asyncGateMs = (PS3Profile::geAsyncGate - lastAsyncGate) * tbMs / n;
	const double asyncGates = (PS3Profile::geAsyncGates - lastAsyncGates) / n;
	lastAsyncWait = PS3Profile::geAsyncWait;
	lastAsyncWaits = PS3Profile::geAsyncWaits;
	lastAsyncGate = PS3Profile::geAsyncGate;
	lastAsyncGates = PS3Profile::geAsyncGates;
	const double asyncWaitMs = asyncFullMs + asyncGateMs;
	const bool listThread = g_geAsyncLists && (asyncWaits > 0.0 || asyncGates > 0.0);
	const double geOnEmuMs = listThread ? asyncWaitMs : geMs;
	// Frames since the last 10 s report: for the counters only read there.
	static double framesSinceLog = 0.0;
	framesSinceLog += n;
	std::string workerWaits;
	{
		// The GE worker waiting for the RSX, and its slow jobs (build 155).
		static uint64_t lCb = 0, lFence = 0, lFenceN = 0, lSlow[16], lSlowT[16];
		static const char *jobNames[16] = {"T&L draw", "T&L draw (decoded)", "indexed", "triangles", "triangle", "rectangle", "line", "point", "flush", "primitives"};
		char head[160];
		snprintf(head, sizeof(head), "RSX command buffer full %.1fms, RSX fences %.1fms (%.1f); jobs over 0.5 ms:",
			(PS3Profile::rsxCbWait - lCb) * tbMs / n, (PS3Profile::rsxFenceWait - lFence) * tbMs / n, (PS3Profile::rsxFenceWaits - lFenceN) / n);
		workerWaits = head;
		lCb = PS3Profile::rsxCbWait;
		lFence = PS3Profile::rsxFenceWait;
		lFenceN = PS3Profile::rsxFenceWaits;
		for (int t = 0; t < 16; ++t) {
			const uint64_t c = PS3Profile::geSlowJobs[t] - lSlow[t];
			if (c) {
				char part[96];
				snprintf(part, sizeof(part), " %s %.1f (%.1fms)", jobNames[t] ? jobNames[t] : "?", c / n, (PS3Profile::geSlowJobTicks[t] - lSlowT[t]) * tbMs / n);
				workerWaits += part;
			}
			lSlow[t] = PS3Profile::geSlowJobs[t];
			lSlowT[t] = PS3Profile::geSlowJobTicks[t];
		}
	}
	std::string asyncCauses;
	{
		static uint64_t lt[GEASYNC_COUNT], lc[GEASYNC_COUNT];
		static const char *names[GEASYNC_COUNT] = {"other", "game GE calls/interrupts", "next lists", "vblank", "frame shown", "idle"};
		for (int r = 0; r < GEASYNC_COUNT; ++r) {
			char part[96];
			snprintf(part, sizeof(part), "%s%s %.1fms/%.1f", r ? ", " : "", names[r], (g_geAsyncWaitTicks[r] - lt[r]) * tbMs / n, (g_geAsyncWaitCount[r] - lc[r]) / n);
			asyncCauses += part;
			lt[r] = g_geAsyncWaitTicks[r];
			lc[r] = g_geAsyncWaitCount[r];
		}
	}
	// Emulation-thread GE parts (updated every report, so each covers just this interval).
	static uint64_t lPrim = 0, lRF = 0, lSync = 0, lXfer = 0, lRFn = 0, lSyncn = 0, lXfern = 0, lPrimN = 0;
	const double primMs = (PS3Profile::gePrim - lPrim) * tbMs / n, rfMs = (PS3Profile::geReaderFlush - lRF) * tbMs / n;
	// Parts of the draw commands (Frontend 88).
	static uint64_t lParts[6] = {}, lCpuPrims = 0;
	const uint64_t parts[6] = { PS3Profile::geState, PS3Profile::geXformState, PS3Profile::geDirect, PS3Profile::geMerge, PS3Profile::gePrep, PS3Profile::geCpuPath };
	double partMs[6];
	for (int i = 0; i < 6; ++i) {
		partMs[i] = (parts[i] - lParts[i]) * tbMs / n;
		lParts[i] = parts[i];
	}
	static uint64_t lFast = 0, lFastN = 0, lSkip = 0, lSkipN = 0;
	const double fastMs = (PS3Profile::geFastMerge - lFast) * tbMs / n, fastPerFrame = (PS3Profile::geFastMerges - lFastN) / n;
	const double skipMs = (PS3Profile::geSkipT - lSkip) * tbMs / n, skipPerFrame = (PS3Profile::geSkips - lSkipN) / n;
	lFast = PS3Profile::geFastMerge; lFastN = PS3Profile::geFastMerges; lSkip = PS3Profile::geSkipT; lSkipN = PS3Profile::geSkips;
	static uint64_t lCmds = 0, lCmdsSkip = 0;
	const double cmdsPerFrame = (PS3Profile::geCmds - lCmds) / n, cmdsSkipPerFrame = (PS3Profile::geCmdsSkipped - lCmdsSkip) / n;
	lCmds = PS3Profile::geCmds; lCmdsSkip = PS3Profile::geCmdsSkipped;
	static uint64_t lStPix = 0, lStSam = 0, lStN = 0, lXfL = 0;
	const double stPixMs = (PS3Profile::geStPixel - lStPix) * tbMs / n, stSamMs = (PS3Profile::geStSampler - lStSam) * tbMs / n;
	const double stPerFrame = (PS3Profile::geStCount - lStN) / n, xfLightMs = (PS3Profile::geXfLight - lXfL) * tbMs / n;
	lStPix = PS3Profile::geStPixel; lStSam = PS3Profile::geStSampler; lStN = PS3Profile::geStCount; lXfL = PS3Profile::geXfLight;
	const double cpuPrimsPerFrame = (PS3Profile::geCpuPrims - lCpuPrims) / n;
	lCpuPrims = PS3Profile::geCpuPrims;
	const double syncMs = (PS3Profile::geSyncT - lSync) * tbMs / n, xferMs = (PS3Profile::geXfer - lXfer) * tbMs / n;
	const double rfPerFrame = (PS3Profile::geReaderFlushes - lRFn) / n, syncPerFrame = (PS3Profile::geSyncs - lSyncn) / n;
	const double xferPerFrame = (PS3Profile::geXfers - lXfern) / n, primsPerFrame = (PS3Profile::gePrims - lPrimN) / n;
	lPrim = PS3Profile::gePrim; lRF = PS3Profile::geReaderFlush; lSync = PS3Profile::geSyncT; lXfer = PS3Profile::geXfer;
	lRFn = PS3Profile::geReaderFlushes; lSyncn = PS3Profile::geSyncs; lXfern = PS3Profile::geXfers; lPrimN = PS3Profile::gePrims;
	static uint64_t lastDecode = 0, lastBatch = 0;
	const double decodeMs = (PS3Profile::decode - lastDecode) * tbMs / n, batchMs = (PS3Profile::batch - lastBatch) * tbMs / n;
	lastDecode = PS3Profile::decode;
	lastBatch = PS3Profile::batch;
	static uint64_t lastDirect = 0;
	const unsigned long long directDraws = (unsigned long long)(PS3Profile::directTL - lastDirect);
	lastDirect = PS3Profile::directTL;
	static uint64_t lastMiss[4] = {};
	double missPerFrame[4];
	for (int i = 0; i < 4; ++i) {
		missPerFrame[i] = (PS3Profile::tlMiss[i] - lastMiss[i]) / n;
		lastMiss[i] = PS3Profile::tlMiss[i];
	}
	static uint64_t lastMerged = 0;
	const double mergedPerFrame = (PS3Profile::tlMerged - lastMerged) / n;
	lastMerged = PS3Profile::tlMerged;
	static uint64_t lastWorker = 0, lastWait = 0, lastDrains = 0, lastFull = 0;
	const double drainsPerFrame = (PS3Profile::geDrains - lastDrains) / n, fullPerFrame = (PS3Profile::geWaitFull - lastFull) / n;
	lastDrains = PS3Profile::geDrains;
	lastFull = PS3Profile::geWaitFull;
	const double workerMs = (PS3Profile::geWorker - lastWorker) * tbMs / n, waitMs = (PS3Profile::geWait - lastWait) * tbMs / n;
	lastWorker = PS3Profile::geWorker;
	lastWait = PS3Profile::geWait;
	static uint64_t lastSas = 0, lastAudioDecode = 0;
	const double sasMs = (PS3Profile::sasMix - lastSas) * tbMs / n, audioDecodeMs = (PS3Profile::audioDecode - lastAudioDecode) * tbMs / n;
	lastSas = PS3Profile::sasMix;
	lastAudioDecode = PS3Profile::audioDecode;
	static uint64_t lastRealWait = 0;
	const double realWaitMs = (PS3Profile::realTimeWait - lastRealWait) * tbMs / n;
	lastRealWait = PS3Profile::realTimeWait;
	// With the GE thread, the GE's time isn't on the emulation thread; what is, is waiting for it.
	const double cpuMs = std::max(0.0, statEmu * 1000.0 / n - geOnEmuMs - realWaitMs);
	std::string jitHleText;
	{
		static uint64_t lastJit = 0, lastBlocks = 0, lastInval = 0, lastClears = 0, lastFront = 0, lastNative = 0;
		static uint64_t lastPasses = 0, lastLink = 0, lastInterpRuns = 0, lastInterp = 0, lastPromoted = 0;
		char jl[560];
		static uint64_t lastRecompiles = 0, lastGen = 0, lastLoop = 0, lastRevived = 0;
		snprintf(jl, sizeof(jl), "JIT compiling %.1fms per frame (MIPS to IR %.1f of which passes %.1f, IR to PPC %.1f of which code generation %.1f (instruction loop %.1f), linking %.1f; interpreter %.1fms, %llu runs; %llu blocks, %llu compiled after warm-up, %llu compiled again (last %08x), %llu invalidated, %llu brought back unchanged, %llu cache clears); ",
			(PS3Profile::jitCompile - lastJit) * tbMs / n, (PS3Profile::jitFrontend - lastFront) * tbMs / n, (PS3Profile::jitPasses - lastPasses) * tbMs / n,
			(PS3Profile::jitNative - lastNative) * tbMs / n, (PS3Profile::jitCodeGen - lastGen) * tbMs / n, (PS3Profile::jitCodeLoop - lastLoop) * tbMs / n,
			(PS3Profile::jitLink - lastLink) * tbMs / n,
			(PS3Profile::jitInterp - lastInterp) * tbMs / n, (unsigned long long)(PS3Profile::jitInterpRuns - lastInterpRuns),
			(unsigned long long)(PS3Profile::jitBlocks - lastBlocks), (unsigned long long)(PS3Profile::jitPromoted - lastPromoted),
			(unsigned long long)(PS3Profile::jitRecompiles - lastRecompiles), (unsigned)PS3Profile::jitLastRecompilePC,
			(unsigned long long)(PS3Profile::jitInvalidated - lastInval), (unsigned long long)(PS3Profile::jitRevived - lastRevived), (unsigned long long)(PS3Profile::jitClears - lastClears));
		lastRevived = PS3Profile::jitRevived;
		lastRecompiles = PS3Profile::jitRecompiles;
		lastGen = PS3Profile::jitCodeGen; lastLoop = PS3Profile::jitCodeLoop;
		lastPasses = PS3Profile::jitPasses; lastLink = PS3Profile::jitLink; lastInterpRuns = PS3Profile::jitInterpRuns;
		lastInterp = PS3Profile::jitInterp; lastPromoted = PS3Profile::jitPromoted;
		jitHleText = jl + HLEProfileText(79800.0, n);
		lastJit = PS3Profile::jitCompile; lastBlocks = PS3Profile::jitBlocks;
		lastFront = PS3Profile::jitFrontend; lastNative = PS3Profile::jitNative;
		lastInval = PS3Profile::jitInvalidated; lastClears = PS3Profile::jitClears;
	}
	char line[160];
	snprintf(line, sizeof(line), "FPS %.1f  Speed %.0f%%  CPU %.0fms  GE %.0fms (vertex %.0f)",
		newFps, speed, cpuMs, geMs, xformMs);
	{
		std::lock_guard<std::mutex> lock(presentMutex);
		overlayText = settings.showFps ? line : "";
	}
	// Present waits, summed up to the next report.
	static double presentSync = 0.0, presentFlip = 0.0, presentFrames = 0.0;
	if (rsxRenderer) {
		presentSync += statPresentGESync;
		presentFlip += RSX::TakeFlipWait();
		presentFrames += n;
	}
	// Every ~10 seconds into launch.log, so a report can say where the time goes.
	static double lastLog = 0.0;
	// Debug tab "Perf report": off, every 10 s or every 2 s (each statistics window).
	if (settings.logPerf && now - lastLog >= (settings.logPerf == 2 ? 2.0 : 10.0)) {
		lastLog = now;
		const double n10 = framesSinceLog > 0.0 ? framesSinceLog : 1.0;
		framesSinceLog = 0.0;
		{
			// The game's own frames (flips) per real second, drawn or skipped by auto frameskip.
			static uint32_t lastFlips = 0;
			static double lastFlipTime = 0.0;
			const uint32_t f = (uint32_t)gpuStats.totals.numFlips;
			if (lastFlipTime > 0.0)
				Appendf(perfReport, "Perf: the game's own frames: %.1f/s (drawn %.1f/s); new pictures shown %.1f/s", (f - lastFlips) / (now - lastFlipTime), gameFps, newFps);
			lastFlips = f;
			lastFlipTime = now;
			// Frame cap: whether it's holding the game (since the last log line).
			const DisplayCapStats cap = g_displayCapStats;
			g_displayCapStats = DisplayCapStats{};
			if (cap.frames)
				Appendf(perfReport, "Perf: frame cap: %u new frames (%u from interrupts), %u held in sceDisplaySetFrameBuf (%.1f ms each), %u vblank waits made longer",
					cap.frames, cap.inInterrupt, cap.held, cap.held ? cap.heldUs / 1000.0 / cap.held : 0.0, cap.waitsExtended);
		}
		Appendf(perfReport, "Perf: game frames %.1f/s, shown %.1f/s, speed %.0f%%, per frame: emulation %.1fms "
			"= PSP CPU %.1fms + GE %.1fms (of which vertex transform + drawing %.1fms: decode %.1f, batched transform %.1f; %.0f direct T&L draws per frame) + waiting to keep real-time speed %.1fms, present %.1fms, dropped %u",
			gameFps, shownFps, speed, statEmu * 1000.0 / n, cpuMs, geMs, xformMs, decodeMs, batchMs, directDraws / n, realWaitMs,
			statFlush * 1000.0 / n, (unsigned)(droppedFrames - statDroppedStart));
		if (settings.collectStats)
			Appendf(perfReport, "Perf: display lists (geprofile, PPSSPP's own count): %.1fms per frame", statGE * 1000.0 / n);
		if (listThread)
			Appendf(perfReport, "Perf: GE lists on their own thread: %.1fms per frame there; the emulation thread waited for it to finish %.1fms (%.1f times) and to catch up in emulated time %.1fms (%.1f times)",
				geMs, asyncFullMs, asyncWaits, asyncGateMs, asyncGates);
		if (listThread)
			Appendf(perfReport, "Perf: waits for the GE list thread to finish, by cause, per frame: %s", asyncCauses.c_str());
		if (rsxRenderer && presentFrames > 0) {
			// Present = waiting for the GE worker to finish the frame's drawing + waiting for the RSX
			// to finish the previous frame (its flip) + the rest. Long flip waits: the RSX is the limit.
			Appendf(perfReport, "Perf: present per frame: waiting for the GE thread %.1fms, waiting for the RSX to finish the previous frame %.1fms",
				presentSync * 1000.0 / presentFrames, presentFlip * 1000.0 / presentFrames);
			presentSync = presentFlip = presentFrames = 0.0;
		}
		if (settings.geThread && settings.fast3d == 2) {
			static uint64_t lastPrep = 0;
			const uint64_t prep = TLPrep::JobsDone();
			Appendf(perfReport, "Perf: GE worker thread per frame: %.1fms drawing, emulation thread waited %.1fms for it (%.1f waits for all, %.1f for a free slot); T&L vertex prep on %s: %.0f draws per frame",
				workerMs, waitMs, drainsPerFrame, fullPerFrame, TLPrep::Description(), (double)(prep - lastPrep) / n10);
			lastPrep = prep;
			Appendf(perfReport, "Perf: GE worker waits per frame: %s", workerWaits.c_str());
			Appendf(perfReport, "Perf: GE on the emulation thread per frame: draw commands %.1fms (%.0f), flushes for reads %.1fms (%.1f), syncs %.1fms (%.1f), block transfers %.1fms (%.1f)",
				primMs, primsPerFrame, rfMs, rfPerFrame, syncMs, syncPerFrame, xferMs, xferPerFrame);
			Appendf(perfReport, "Perf: draw commands per frame: state %.1fms, transform state %.1fms, direct T&L %.1fms (merging %.1fms, new jobs %.1fms), CPU paths %.1fms (%.0f draws)",
				partMs[0], partMs[1], partMs[2], partMs[3], partMs[4], partMs[5], cpuPrimsPerFrame);
			Appendf(perfReport, "Perf: draw commands per frame by path: added to the open T&L job %.1fms (%.0f), skipped frames %.1fms (%.0f), all others %.0f",
				fastMs, fastPerFrame, skipMs, skipPerFrame, primsPerFrame - fastPerFrame - skipPerFrame);
			Appendf(perfReport, "Perf: GE commands per frame: %.0f in drawn frames, %.0f in skipped frames", cmdsPerFrame, cmdsSkipPerFrame);
			Appendf(perfReport, "Perf: draw state per frame: %.0f new states (pixel setup %.1fms, texture setup and the rest %.1fms), lighting setup %.1fms",
				stPerFrame, stPixMs, stSamMs, xfLightMs);
			{
				static uint64_t lh = 0, lm = 0;
				Appendf(perfReport, "Perf: transform matrix products per frame: %.0f reused, %.0f computed",
					(PS3Profile::xfMemoHits - lh) / n10, (PS3Profile::xfMemoMisses - lm) / n10);
				lh = PS3Profile::xfMemoHits;
				lm = PS3Profile::xfMemoMisses;
			}
			Appendf(perfReport, "Perf: timed events per frame (vblank, audio, GE interrupts...) %s", CoreTiming::PerfEventSummary(1.0 / tbMs, n10).c_str());
			{
				// GE commands with a handler, by time (Frontend 138): what the GE time that isn't drawing is.
				static uint64_t lExecT[256], lExecN[256];
				std::string top;
				uint64_t d[256];
				for (int c = 0; c < 256; ++c) d[c] = PS3Profile::geExecTicks[c] - lExecT[c];  // By time (every handler timed, build 152).
				for (int k = 0; k < 10; ++k) {
					int best = -1;
					for (int c = 0; c < 256; ++c)
						if (d[c] && (best < 0 || d[c] > d[best])) best = c;
					if (best < 0) break;
					char part[96];
					snprintf(part, sizeof(part), "%s%s %.1fms/%.0f", k ? ", " : "", GeCmdToString((GECommand)best), (PS3Profile::geExecTicks[best] - lExecT[best]) * tbMs / n10, (PS3Profile::geExecCount[best] - lExecN[best]) / n10);
					top += part;
					d[best] = 0;
				}
				uint64_t handlerTicks = 0;
				for (int c = 0; c < 256; ++c) handlerTicks += PS3Profile::geExecTicks[c] - lExecT[c];
				static uint64_t lLoop = 0;
				const double loopMs = (PS3Profile::geLoop - lLoop) * tbMs / n10;
				lLoop = PS3Profile::geLoop;
				for (int c = 0; c < 256; ++c) { lExecT[c] = PS3Profile::geExecTicks[c]; lExecN[c] = PS3Profile::geExecCount[c]; }
				Appendf(perfReport, "Perf: GE command loop per frame %.1fms: handlers %.1fms, the loop itself %.1fms; handlers by time (ms/calls): %s",
					loopMs, handlerTicks * tbMs / n10, loopMs - handlerTicks * tbMs / n10, top.c_str());
				static uint64_t lt[8];
				const uint64_t cur[8] = { PS3Profile::tlSetup, PS3Profile::tlScan, PS3Profile::tlCopy, PS3Profile::tlCopyBytes, PS3Profile::tlCapture, PS3Profile::tlParams, PS3Profile::tlExpand, PS3Profile::tlNote };
				Appendf(perfReport, "Perf: new T&L jobs per frame: decode setup %.1fms, index scan %.1fms, vertex copy %.1fms (%.0f KB), state capture %.1fms, parameters %.1fms, strip to list %.1fms, target note %.1fms",
					(cur[0] - lt[0]) * tbMs / n10, (cur[1] - lt[1]) * tbMs / n10, (cur[2] - lt[2]) * tbMs / n10, (cur[3] - lt[3]) / 1024.0 / n10,
					(cur[4] - lt[4]) * tbMs / n10, (cur[5] - lt[5]) * tbMs / n10, (cur[6] - lt[6]) * tbMs / n10, (cur[7] - lt[7]) * tbMs / n10);
				for (int i = 0; i < 8; ++i) lt[i] = cur[i];
			}
		}
		Appendf(perfReport, "Perf: 3D draws per frame not on the direct T&L path: %.0f lines/points/rectangles, %.0f generated UVs or powered diffuse, %.0f 32-bit indices, %.0f other; %.0f T&L draws merged into the one before",
			missPerFrame[0], missPerFrame[1], missPerFrame[2], missPerFrame[3], mergedPerFrame);
		{
			static uint64_t lastFail[16] = {}, lastFast = 0;
			double f[16];
			for (int i = 0; i < 16; ++i) {
				f[i] = (PS3Profile::tlMergeFail[i] - lastFail[i]) / n10;
				lastFail[i] = PS3Profile::tlMergeFail[i];
			}
			const double fast = (PS3Profile::tlMergeFast - lastFast) / n10;
			lastFast = PS3Profile::tlMergeFast;
			Appendf(perfReport, "Perf: T&L draw merging per frame: %.0f without state comparison; not merged: %.0f other kind/primitive, %.0f vertex format, %.0f too big, %.0f bones, %.0f buffers/scissor, %.0f texture or render state, %.0f palette, %.0f transform/lighting",
				fast, f[1], f[2], f[3] + f[4], f[5], f[6], f[7], f[8], f[9]);
			// Where merging's time goes (build 255 diagnostics).
			static uint64_t lastT[5] = {}, lastTries = 0;
			double t[5];
			for (int i = 0; i < 5; ++i) {
				t[i] = (PS3Profile::tlMergeT[i] - lastT[i]) * tbMs / n10;
				lastT[i] = PS3Profile::tlMergeT[i];
			}
			const double tries = (PS3Profile::tlMergeTries - lastTries) / n10;
			lastTries = PS3Profile::tlMergeTries;
			Appendf(perfReport, "Perf: T&L merge attempts per frame: %.0f, %.2fms in all (%.1f us each): kind and format checks %.2fms, index range %.2fms, state comparison %.2fms, transform parameters %.2fms, appending %.2fms",
				tries, t[0] + t[1] + t[2] + t[3] + t[4], tries > 0 ? (t[0] + t[1] + t[2] + t[3] + t[4]) * 1000.0 / tries : 0.0, t[0], t[1], t[2], t[3], t[4]);
		}
		GESync();
		if (rsxRenderer) Appendf(perfReport, "Perf: %s; texture hashing/decoding on %s; %s", RSXRendererStats(rsxRenderer).c_str(), TexSpu::Description(), MemoryText().c_str());
		Appendf(perfReport, "Perf: memory: %s", MemoryDetailText().c_str());
		{
			// Running low: have the GE worker's job slots give back their buffers.
			u32 freeBytes = 0, lv2Free = 0, total = 0;
			if (FreeMemory(&freeBytes, &lv2Free, &total) && lv2Free < 4u * 1024 * 1024 && g_geTrimJobs.load() == 0) {
				g_geTrimJobs = 96;  // Every slot (TLWorker::RING).
				g_geLowMemory = true;  // Merged 3D draws stay small from now on.
				Appendf(perfReport, "Perf: memory low (%u KB not yet taken by malloc): GE job buffers released", lv2Free >> 10);
			}
			if (!g_oomReserve.load() && g_oomCount.load() > 0 && TakeOomReserve()) {
				Appendf(perfReport, "Perf: memory: 4 MB reserve for a failed allocation taken again (%d failed so far)", g_oomCount.load());
				std::set_new_handler(OutOfMemoryHandler);
			}
		}
		if (settings.sound) Appendf(perfReport, "Perf: %s; per frame: sound mixing %.1fms (%s), audio decoding %.1fms", PS3Audio::StatsText().c_str(),
			sasMs, SasMixerDescription(), audioDecodeMs);
		if (settings.cpuJit) Appendf(perfReport, "Perf: %s", MIPSComp::PPCJitStats().c_str());
		Appendf(perfReport, "Perf: %s", jitHleText.c_str());
		if (gameFps < 3.0) {
			// Very slow or stuck: say what each PSP thread is doing.
			std::string text;
			for (const DebugThreadInfo &t : GetThreadsInfo()) {
				char tb[112];
				snprintf(tb, sizeof(tb), "%s%s(%s%s, pc %08x)", text.empty() ? "" : ", ", t.name,
					t.isCurrent ? "current " : "", (t.status & THREADSTATUS_WAIT) ? WaitTypeToString(t.waitType) : (t.status & THREADSTATUS_READY) ? "ready" : (t.status & THREADSTATUS_RUNNING) ? "running" : "other", t.curPC);
				text += tb;
			}
			Appendf(perfReport, "Perf: slow; PSP threads: %s", text.c_str());
			TraceReport(perfReport);
			perfReport.clear();
			SlowDiagnostics();
		}
	}
	statStart = now; statEmuUsStart = emuUs; statFlipsStart = flips; statDroppedStart = droppedFrames; statNewPicturesStart = newPictures;
	statEmu = statGE = statFlush = statConvert = 0.0; statFrames = 0; statPresentGESync = 0.0;
	TraceReport(perfReport);
}

// ---------------------------------------------------------------------------------------------
// Game fixes, applied between run slices (the CPU state is in MIPSState there).

static float FloatBits(u32 v) { float f; memcpy(&f, &v, 4); return f; }

// Dead to Rights: Reckoning (ULES00195) froze for good when the dog was used: the code that
// wraps an angle difference into (0, 2*pi] (088f7688: while (!(a <= 2*pi)) a -= 2*pi) never ends
// when the difference is NaN or infinite, which it is in some dog moves. Frontend 58 made the
// loop end on NaN, but the NaN then went on into the dog's heading (it fell through the world).
// Now a NaN difference becomes 0 before the loop (the same 9 instructions, rearranged):
//   c.un.s f12,f12; bc1tl done; mov.s f12,f17 (f17 = 0 here)
//   c.le.s f12,f13; bc1t done; nop
//   loop: sub.s f12,f12,f13; c.ule.s f12,f13; bc1f loop   (delay slot: the compare at done)
// An infinite angle caught spinning in the loop between run slices is set to 0.
static void DeadToRightsFix() {
	static bool patched = false;
	const u32 loopStart = 0x088f7688;
	if (!Memory::IsValidRange(loopStart, 0x28))
		return;
	auto insn = [](u32 a) { return Memory::Read_Instruction(a, true).encoding; };
	static const u32 original[9] = { 0x460d603e, 0x00000000, 0x45010006, 0x00000000, 0x460d6301, 0x460d603e, 0x00000000, 0x4502fffd, 0x460d6301 };
	static const u32 fixed[9] = { 0x460c6031, 0x45030007, 0x46008b06, 0x460d603e, 0x45010004, 0x00000000, 0x460d6301, 0x460d6037, 0x4500fffd };
	bool isOriginal = insn(loopStart + 36) == 0x460e603e;  // c.le.s f12, f14 after it.
	for (int i = 0; i < 9 && isOriginal; ++i)
		isOriginal = insn(loopStart + i * 4) == original[i];
	if (isOriginal) {
		if (MIPSComp::jit)
			MIPSComp::jit->InvalidateCacheAt(loopStart, 0x28);
		for (int i = 0; i < 9; ++i)
			Memory::WriteUnchecked_U32(fixed[i], loopStart + i * 4);
		if (MIPSComp::jit)
			MIPSComp::jit->InvalidateCacheAt(loopStart, 0x28);
		if (!patched)
			PS3Frontend::Trace("Game fix: Dead to Rights angle loop at %08x patched (NaN angles from the dog become 0)", loopStart);
		patched = true;
	}
	MIPSState *m = currentMIPS;
	if (!m || m->pc < loopStart || m->pc > loopStart + 36 || insn(loopStart + 32) != fixed[8])
		return;
	const float a = m->f[12];
	if (std::isfinite(a) && std::fabs(a) < 100000.0f)
		return;
	static int rescues = 0;
	const u32 obj = m->r[MIPS_REG_S0];
	const bool objOk = Memory::IsValidAddress(obj) && Memory::IsValidAddress(obj + 3);
	const u32 heading = objOk ? Memory::ReadUnchecked_U32(obj) : 0;
	if (rescues++ < 10)
		PS3Frontend::Trace("Game fix: Dead to Rights angle loop stuck (angle %08x, atan2 result f0 %08x, object %08x heading %08x); angle set to 0",
			m->fi[12], m->fi[0], obj, heading);
	m->f[12] = 0.0f;
	if (objOk && !std::isfinite(FloatBits(heading)))
		Memory::WriteUnchecked_U32(0, obj);
}

// Main memory is short on the PS3 (~25 MB once a game runs). When it gets very low, the JIT
// cache (compiled code bookkeeping, IR) is the one big thing that can go and come back. (Build 210
// also cleared it whenever no piece of 1 MB was left: every 30 s in Silent Hill Origins, a pause
// each time while the code compiled again, and the biggest piece stayed 256 KB anyway.)
static void MemoryGuard() {
	static double lastCheck = 0.0, lastClear = 0.0;
	const double now = time_now_d();
	if (now - lastCheck < 2.0)
		return;
	lastCheck = now;
	u32 freeBytes = 0, lv2Free = 0, total = 0;
	if (!FreeMemory(&freeBytes, &lv2Free, &total) || freeBytes >= 6 * 1024 * 1024 || now - lastClear < 30.0)
		return;
	lastClear = now;
	PS3Frontend::Trace("Low memory (%u KB free): JIT cache cleared", freeBytes >> 10);
	if (MIPSComp::jit)
		MIPSComp::jit->ClearCache();
}

// The JIT cache filled its 65536 blocks (Midnight Club L.A. does every few minutes of driving, and
// then compiles everything again for seconds at 10-15 fps). With room for it (the block tables
// grow by ~2-3 MB, reserved after the clear), the limit goes up to the "JIT cache: Large" one for
// the rest of the game, so it fills far less often. The 8 MB of native code space then sets the
// limit (~85000 blocks of Midnight Club's).
static void JitCacheFullHook(int blocks) {
	if (MIPSComp::g_irPs3BlockLimit >= 98304)
		return;
	const u32 largest = LargestAllocatable();
	if (largest < 8u * 1024 * 1024) {
		PS3Frontend::Trace("JIT cache full at %d blocks; not enlarged (biggest free piece %u KB)", blocks, largest >> 10);
		return;
	}
	MIPSComp::g_irPs3BlockLimit = 98304;
	PS3Frontend::Trace("JIT cache full at %d blocks: room for 98304 from now on (as \"JIT cache: Large\")", blocks);
}

// MediaEngine (g_mediaMemoryHook), on the emulation thread before a movie opens: with little
// room in one piece the GE job buffers are released (cheap; they grow back as needed).
static void MediaMemoryHook(const char *what, int bytes) {
	const u32 largest = LargestAllocatable();
	if (largest >= 1024 * 1024 && largest >= (u32)bytes * 2)
		return;
	g_geTrimJobs = 96;  // Every slot (TLWorker::RING).
	g_geLowMemory = true;
	static int logged = 0;
	if (logged++ < 10)
		PS3Frontend::Trace("Low memory: %s needs %d KB and the biggest free piece is %u KB; GE job buffers released",
			what, bytes >> 10, largest >> 10);
}

// From MIPSVFPUUtils (Debug on): a VFPU function made a NaN out of a number. The first ones of a
// game, with where the PSP code was (the JIT updates pc per block, so it's the block's start or
// the last call), to find where a game's maths first goes wrong.
static void VfpuNaNHook(const char *op, uint32_t in) {
	static int reported = 0;
	if (reported >= 24)
		return;
	reported++;
	MIPSState *m = currentMIPS;
	PS3Frontend::Trace("VFPU NaN: %s(%08x) (frame %u, PSP pc %08x ra %08x)%s", op, in, frames,
		m ? m->pc : 0, m ? m->r[MIPS_REG_RA] : 0, reported == 24 ? " - no more of these" : "");
}

// From the IR interpreter (nanwatch=1): a block left a NaN in a float/VFPU register that held a
// number before it. Once per block; the first ones with the block's code.
static void IrNaNWatchLog(u32 blockStart, u32 blockBytes, int reg, u32 before, u32 after, const u32 *fpuBefore) {
	static int reported = 0;
	reported++;
	char regName[16];
	if (reg < 32) snprintf(regName, sizeof(regName), "f%d", reg);
	else snprintf(regName, sizeof(regName), "vfpu reg %d", reg - 32);
	PS3Frontend::Trace("NaN watch #%d: block %08x (%u instructions) made %s %08x (was %08x), frame %u", reported,
		blockStart, blockBytes / 4, regName, after, before, frames);
	if (reported > 80)
		return;
	for (int row = 0; row < 4; ++row) {
		const u32 *f = fpuBefore + row * 8;
		PS3Frontend::Trace("NaN watch #%d:   before f%d-f%d: %08x %08x %08x %08x %08x %08x %08x %08x", reported, row * 8, row * 8 + 7,
			f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
	}
	if (reg >= 32) {
		// The 16 VFPU values of that register's matrix (internal order), before the block.
		const u32 *v = fpuBefore + 32 + ((reg - 32) & ~15);
		for (int row = 0; row < 2; ++row)
			PS3Frontend::Trace("NaN watch #%d:   before vfpu matrix %d [%d]: %08x %08x %08x %08x %08x %08x %08x %08x", reported, (reg - 32) / 16, row * 8,
				v[row * 8], v[row * 8 + 1], v[row * 8 + 2], v[row * 8 + 3], v[row * 8 + 4], v[row * 8 + 5], v[row * 8 + 6], v[row * 8 + 7]);
	}
	const u32 count = std::min<u32>(blockBytes / 4, 64);
	for (u32 i = 0; i < count; ++i) {
		const u32 a = blockStart + i * 4;
		if (!Memory::IsValidAddress(a))
			break;
		char dis[128];
		MIPSDisAsm(Memory::Read_Instruction(a, true), a, dis, sizeof(dis), true);
		PS3Frontend::Trace("NaN watch #%d:   %08x %08x %s", reported, a, Memory::Read_Instruction(a, true).encoding, dis);
	}
}

// From the GE (SoftGpu.cpp, storewatch=1): lines about vertex colours found half-written; a line
// "@block <addr>" asks for that block's code (once per block, up to 20 blocks).
static void StoreWatchReport(const char *text) {
	unsigned block = 0;
	if (sscanf(text, "@block %x", &block) == 1) {
		static std::set<u32> listed;
		if (listed.size() >= 20 || !listed.insert(block).second || !Memory::IsValidAddress(block))
			return;
		for (u32 i = 0; i < 48; ++i) {
			const u32 a = block + i * 4;
			if (!Memory::IsValidAddress(a))
				break;
			char dis[128];
			MIPSDisAsm(Memory::Read_Instruction(a, true), a, dis, sizeof(dis), true);
			PS3Frontend::Trace("Store watch block %08x:   %08x %08x %s", block, a, Memory::Read_Instruction(a, true).encoding, dis);
		}
		return;
	}
	PS3Frontend::Trace("%s", text);
}

// From the GE (SoftGpu.cpp): the game uploaded a NaN/infinite camera.
void PS3NoteNaNCamera(int element, u32 value) {
	PS3Frontend::Trace("NaN camera: view matrix element %d = %08x (frame %u); the game's camera or player position became NaN", element, value, frames);
	SlowDiagnostics();
}

static void GameFixes() {
	MemoryGuard();
	static std::string lastID;
	static bool dtr = false;
	static int checks = 0;
	// The disc ID once in a while (the same game until it ends).
	if (lastID.empty() || (++checks & 63) == 0) {
		lastID = g_paramSFO.GetDiscID();
		dtr = lastID == "ULES00195";
		// (Frontend 63 to 104 turned the JIT block counters on for Killzone here, for its freeze after
		// the autosave notice. That freeze was the denormal flushing, fixed in Frontend 63; the
		// counters cost every block a time base read and two load-hit-store stalls. Off from
		// Frontend 112 - holding L3 still turns them on for a sample.)
	}
	if (dtr)
		DeadToRightsFix();
}

// ---------------------------------------------------------------------------------------------
// Input


// The first connected pad into `pad` (released when none); its port, or -1.
static int ReadPad() {
	padInfo2 info{};
	int port = -1;
	if (ioPadGetInfo2(&info) == 0) for (int i = 0; i < 7; ++i) if (info.port_status[i] & 1) { port = i; break; }
	if (port < 0 || port != previousPort) {
		memset(&pad, 0, sizeof(pad));
		pad.ANA_L_H = pad.ANA_L_V = pad.ANA_R_H = pad.ANA_R_V = 128;
	}
	previousPort = port;
	if (port >= 0) { padData data{}; if (ioPadGetData(port, &data) == 0 && data.len > 0) pad = data; }
	return port;
}

// Settings that can change while a game runs (in-game options): pushed to the core and renderer.
// Called with the emulation thread stopped in Pump and the GE worker idle.
static void ApplyLiveSettings() {
	if (settings.frameskip < 0) {
		g_Config.iFrameSkip = 4;
		g_Config.bAutoFrameSkip = true;
	} else {
		g_Config.iFrameSkip = settings.frameskip;
		g_Config.bAutoFrameSkip = false;
	}
	g_geTimingPercent = settings.geTiming;
	g_displayMinVblanksPerFlip = 60 / settings.fpsCap;
	g_displayVideoCapFps = settings.videoCap;
	PSP_CoreParameter().fastForward = !settings.speedLimit;
	g_Config.iIOTimingMethod = settings.ioHost ? IOTIMING_HOST : IOTIMING_FAST;
	PS3Audio::SetSlowMode(settings.audioSlow);
	Rasterizer::g_tlMergeEnabled = settings.tlMerge;
	RSXRendererSetLiveOptions(rsxRenderer, settings.bilinear, settings.keepLayouts, !settings.lensFlare, settings.fxaa);
	g_geAsyncLists = settings.geListThread != 0;
	g_geAsyncRunAhead = settings.geListThread >= 2;
	g_geAsyncNoSyncWait = settings.geListThread == 3;
	g_Config.iGameVolume = settings.volume;
	RSX::SetMaxAnisotropy(settings.anisotropy);
}

// Edge smoothing (settings.fxaa): 0 Off, 1 Full, 2 Light. The menus go Off -> Light -> Full.
static const char *FxaaName(int v) {
	return v == 1 ? "Full" : v == 2 ? "Light" : "Off";
}

static int NextFxaa(int v) {
	return v == 0 ? 2 : v == 2 ? 1 : 0;
}

enum InGameRow {
	IG_FRAMESKIP, IG_SPEEDLIMIT, IG_FPSCAP, IG_GETIMING, IG_SHOWFPS, IG_AUDIOSLOW, IG_IOHOST,
	IG_BILINEAR, IG_TLMERGE, IG_KEEPLAYOUTS, IG_RSTICK, IG_GELISTS, IG_LENSFLARE, IG_VOLUME, IG_ANISO, IG_FXAA, IG_VIDEOCAP, IG_COUNT
};

// The settings from before the game's per-game file (main sets it while a game runs).
static PS3Frontend::Settings *globalSettingsDuringGame = nullptr;

// Options changed in a game (bit per in-game row): they go into the normal settings and
// settings.ini, for all games (a game's own file still wins for that game).
static uint32_t inGameChanged = 0;

static void CopyInGameSetting(PS3Frontend::Settings &to, const PS3Frontend::Settings &from, int row) {
	switch (row) {
	case IG_FRAMESKIP: to.frameskip = from.frameskip; break;
	case IG_SPEEDLIMIT: to.speedLimit = from.speedLimit; break;
	case IG_FPSCAP: to.fpsCap = from.fpsCap; break;
	case IG_GETIMING: to.geTiming = from.geTiming; break;
	case IG_SHOWFPS: to.showFps = from.showFps; break;
	case IG_AUDIOSLOW: to.audioSlow = from.audioSlow; break;
	case IG_IOHOST: to.ioHost = from.ioHost; break;
	case IG_BILINEAR: to.bilinear = from.bilinear; break;
	case IG_TLMERGE: to.tlMerge = from.tlMerge; break;
	case IG_KEEPLAYOUTS: to.keepLayouts = from.keepLayouts; break;
	case IG_RSTICK: to.rightStick = from.rightStick; break;
	case IG_GELISTS: to.geListThread = from.geListThread; break;
	case IG_LENSFLARE: to.lensFlare = from.lensFlare; break;
	case IG_VOLUME: to.volume = from.volume; break;
	case IG_ANISO: to.anisotropy = from.anisotropy; break;
	case IG_FXAA: to.fxaa = from.fxaa; break;
	case IG_VIDEOCAP: to.videoCap = from.videoCap; break;
	}
}

static void SaveInGameSettings() {
	if (!globalSettingsDuringGame)
		return;
	for (int row = 0; row < IG_COUNT; ++row)
		if (inGameChanged & (1u << row))
			CopyInGameSetting(*globalSettingsDuringGame, settings, row);
	const PS3Frontend::Settings current = settings;
	settings = *globalSettingsDuringGame;
	SaveSettings();
	settings = current;
}

static std::string InGameValue(int row) {
	char buf[48];
	switch (row) {
	case IG_FRAMESKIP:
		if (settings.frameskip < 0) return "Auto";
		if (settings.frameskip == 0) return "Off";
		snprintf(buf, sizeof(buf), "%d (draw 1 of %d)", settings.frameskip, settings.frameskip + 1);
		return buf;
	case IG_SPEEDLIMIT: return settings.speedLimit ? "On" : "Off";
	case IG_FPSCAP: return settings.fpsCap == 60 ? "60 fps (no cap)" : std::to_string(settings.fpsCap) + " fps";
	case IG_GETIMING: snprintf(buf, sizeof(buf), "%d%%", settings.geTiming); return buf;
	case IG_SHOWFPS: return settings.showFps ? "On" : "Off";
	case IG_AUDIOSLOW: return settings.audioSlow == 1 ? "Wait and refill" : settings.audioSlow == 2 ? "Play slower" : "Short gaps";
	case IG_IOHOST: return settings.ioHost ? "Host" : "Fast";
	case IG_BILINEAR: return settings.bilinear ? "Bilinear" : "Nearest";
	case IG_TLMERGE: return settings.tlMerge ? "On" : "Off";
	case IG_KEEPLAYOUTS: return settings.keepLayouts ? "On" : "Off";
	case IG_RSTICK: return settings.rightStick == 1 ? "Face buttons" : settings.rightStick == 2 ? "D-pad" : "Off";
	case IG_GELISTS: return settings.geListThread == 3 ? "Don't wait (HACK)" : settings.geListThread == 2 ? "Own thread, run ahead" : settings.geListThread ? "Own thread" : "Emulation thread";
	case IG_LENSFLARE: return settings.lensFlare ? "On" : "Off";
	case IG_VOLUME: return std::to_string(settings.volume) + "%";
	case IG_ANISO: return AnisoName(settings.anisotropy);
	case IG_FXAA: return FxaaName(settings.fxaa);
	case IG_VIDEOCAP: return VideoCapName(settings.videoCap);
	}
	return "";
}

static void ChangeInGameSetting(int row, int delta) {
	switch (row) {
	case IG_FRAMESKIP:
		settings.frameskip += delta;
		if (settings.frameskip > 3) settings.frameskip = -1;
		if (settings.frameskip < -1) settings.frameskip = 3;
		break;
	case IG_SPEEDLIMIT: settings.speedLimit = !settings.speedLimit; break;
	case IG_FPSCAP: settings.fpsCap = CycleCap(kFpsCaps, settings.fpsCap, delta); break;
	case IG_GETIMING:
		settings.geTiming += delta * 100;
		if (settings.geTiming > 400) settings.geTiming = 100;
		if (settings.geTiming < 100) settings.geTiming = 400;
		break;
	case IG_SHOWFPS: settings.showFps = !settings.showFps; break;
	case IG_AUDIOSLOW: settings.audioSlow = (settings.audioSlow + 3 + delta) % 3; break;
	case IG_IOHOST: settings.ioHost = !settings.ioHost; break;
	case IG_BILINEAR: settings.bilinear = !settings.bilinear; break;
	case IG_TLMERGE: settings.tlMerge = !settings.tlMerge; break;
	case IG_KEEPLAYOUTS: settings.keepLayouts = !settings.keepLayouts; break;
	case IG_RSTICK: settings.rightStick = (settings.rightStick + 3 + delta) % 3; break;
	case IG_GELISTS: settings.geListThread = (settings.geListThread + 4 + delta) % 4; break;
	case IG_LENSFLARE: settings.lensFlare = !settings.lensFlare; break;
	case IG_VOLUME: settings.volume = (settings.volume / 10 * 10 + 110 + delta * 10) % 110; break;
	case IG_ANISO: settings.anisotropy = kAniso[(AnisoIndex(settings.anisotropy) + 5 + delta) % 5]; break;
	case IG_FXAA: settings.fxaa = NextFxaa(settings.fxaa); break;
	case IG_VIDEOCAP: settings.videoCap = CycleCap(kVideoCaps, settings.videoCap, delta); break;
	}
	inGameChanged |= 1u << row;
}

// START+SELECT in a game ("START+SELECT menu" setting): the game pauses and the options that work without a
// restart are listed in the top left corner. START+SELECT or Circle goes back.
static void InGameOptions() {
	if (!rsxRenderer) {
		ShowNotice("In-game options need the RSX renderer", 3.0);
		return;
	}
	static const char *names[IG_COUNT] = {
		"Frameskip", "Speed limit", "Frame cap", "GE timing", "FPS overlay", "Sound when slow",
		"File loading", "Filtering", "Draw merging", "Keep buffer layouts", "Right stick", "GE lists",
		"Lens flare occlusion", "Volume", "Anisotropic filtering", "Edge smoothing", "Frame cap in videos",
	};
	PS3Frontend::Trace("In-game options opened; emulation paused");
	inGameOptionsOpen = true;
	static int selection = 0;
	bool confirmQuit = false, confirmYes = false;
	padData last = pad;
	bool previousCombo = true;  // Still held from opening.
	while (running) {
		if (!eventThreadActive) sysUtilCheckCallback();
		ReadPad();
		const bool combo = pad.BTN_START && pad.BTN_SELECT;
		const bool close = (combo && !previousCombo) || (pad.BTN_CIRCLE && !last.BTN_CIRCLE);
		previousCombo = combo;
		const bool up = pad.BTN_UP && !last.BTN_UP, down = pad.BTN_DOWN && !last.BTN_DOWN;
		const bool left = pad.BTN_LEFT && !last.BTN_LEFT;
		const bool cross = pad.BTN_CROSS && !last.BTN_CROSS;
		const bool change = (pad.BTN_RIGHT && !last.BTN_RIGHT) || cross;
		last = pad;
		// The last row (after the settings) quits to the game list, after a question.
		const int rows = IG_COUNT + 1;
		if (confirmQuit && !systemMenuOpen) {
			// Circle or START+SELECT goes back to the options; No is selected to start with.
			if (up || down) confirmYes = !confirmYes;
			if (close) {
				confirmQuit = false;
			} else if (cross) {
				if (confirmYes) {
					PS3Frontend::Trace("In-game options: quit to the game list");
					quitToGameList = true;
					running = false;
					break;
				}
				confirmQuit = false;
			}
		} else if (close && !systemMenuOpen) {
			break;
		} else if (!systemMenuOpen && selection == IG_COUNT) {
			if (up) selection = (selection + rows - 1) % rows;
			if (down) selection = (selection + 1) % rows;
			if (cross) {
				confirmQuit = true;
				confirmYes = false;
			}
		} else if (!systemMenuOpen) {
			if (up) selection = (selection + rows - 1) % rows;
			if (down) selection = (selection + 1) % rows;
			if (selection < IG_COUNT && (left || change)) {
				ChangeInGameSetting(selection, left ? -1 : 1);
				GESync();
				ApplyLiveSettings();
				PS3Frontend::Trace("In-game options: %s = %s", names[selection], InGameValue(selection).c_str());
			}
		}
		char line[96];
		std::string text;
		if (confirmQuit) {
			text = "QUIT THE GAME?\n";
			text += "Are you sure you want to quit the game and go back\nto the game list? Unsaved progress is lost.\n\n";
			text += confirmYes ? "  No\n> Yes, quit to the game list\n" : "> No\n  Yes, quit to the game list\n";
			text += "\nX: OK   O: back";
		} else {
			text = "OPTIONS (game paused, saved for all games)\n";
			for (int i = 0; i < IG_COUNT; ++i) {
				snprintf(line, sizeof(line), "%c %-20s %s\n", i == selection ? '>' : ' ', names[i], InGameValue(i).c_str());
				text += line;
			}
			text += selection == IG_COUNT ? "> Quit to game list\n" : "  Quit to game list\n";
			text += "START+SELECT / O: back";
		}
		GESync();
		RSXRendererPresent(rsxRenderer, text);
		usleep(33 * 1000);
	}
	// Circle or X still held shouldn't reach the game (START and SELECT are held back in Pump
	// until both are up).
	const double waitStart = time_now_d();
	while (running && time_now_d() - waitStart < 2.0) {
		ReadPad();
		if (!pad.BTN_CIRCLE && !pad.BTN_CROSS)
			break;
		GESync();
		RSXRendererPresent(rsxRenderer, std::string());
		usleep(16 * 1000);
	}
	if (inGameChanged)
		SaveInGameSettings();
	lastPresentEnd = 0.0;
	inGameOptionsOpen = false;
	PS3Frontend::Trace("In-game options closed; emulation resumed");
}

bool PS3Frontend::Pump(bool gameRunning) {
	// While the presenter owns SDL, only the native pad/sysutil APIs are used here.
	if (!presenterActive && !rsxRenderer) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) running = false;
	}
	if (!eventThreadActive) sysUtilCheckCallback();
	if (gameRunning && systemMenuOpen) {
		// XMB in-game menu is up: stop emulating, keep the picture alive and wait.
		Trace("System menu opened; emulation paused; %s; RSX local memory: largest free block %u MB", MemoryText().c_str(),
			RSX::LargestFreeLocal() >> 20);
		// (Frontend 112: no longer quits by itself when the XMB menu doesn't appear within 30 s.)
		while (systemMenuOpen && running) {
			if (rsxRenderer) {
				GESync();
				RSXRendererPresent(rsxRenderer, std::string());
			}
			if (!eventThreadActive) sysUtilCheckCallback();
			// The XMB draws over our picture as we show frames: at 30 a second (33 ms here) its own
			// animation ran at 30 fps and felt sluggish. 60 a second, like a game.
			usleep(16 * 1000);
		}
		Trace("System menu closed; emulation resumed");
		memset(&pad, 0, sizeof(pad));
		pad.ANA_L_H = pad.ANA_L_V = pad.ANA_R_H = pad.ANA_R_V = 128;
		lastPresentEnd = 0.0;
		if (!running) return false;
	}
	if (!gameRunning && systemMenuOpen && renderer && !presenterActive && !rsxRenderer && MenuCanvasReady()) {
		// The XMB is up over our own menus: they redraw the whole 1280x720 screen on the PPU and
		// upload it every frame, which slowed the XMB (it runs on the same PPU). Keep showing the
		// last menu picture until it closes (the present waits for the vsync: 60 a second).
		Trace("System menu opened over the app's menus; menu drawing paused");
		SDL_Texture *last = canvasTextures[canvasFlip ^ 1];
		while (systemMenuOpen && running) {
			const double start = time_now_d();
			SDL_RenderCopy(renderer, last, nullptr, nullptr);
			SDL_RenderPresent(renderer);
			if (!eventThreadActive) sysUtilCheckCallback();
			// Should the present not wait for the vsync, don't spin: ~60 a second anyway.
			const double took = time_now_d() - start;
			if (took < 0.010)
				usleep((useconds_t)((0.0166 - took) * 1000000.0));
		}
		Trace("System menu closed; menu drawing resumed");
		memset(&pad, 0, sizeof(pad));
		pad.ANA_L_H = pad.ANA_L_V = pad.ANA_R_H = pad.ANA_R_V = 128;
		if (!running) return false;
	}
	const int port = ReadPad();
	if (gameRunning)
		GameFixes();
	if (gameRunning) {
		u32 buttons = 0;
		if (pad.BTN_UP) buttons |= CTRL_UP;
		if (pad.BTN_DOWN) buttons |= CTRL_DOWN;
		if (pad.BTN_LEFT) buttons |= CTRL_LEFT;
		if (pad.BTN_RIGHT) buttons |= CTRL_RIGHT;
		if (pad.BTN_CROSS) buttons |= CTRL_CROSS;
		if (pad.BTN_CIRCLE) buttons |= CTRL_CIRCLE;
		if (pad.BTN_SQUARE) buttons |= CTRL_SQUARE;
		if (pad.BTN_TRIANGLE) buttons |= CTRL_TRIANGLE;
		// "Right stick" setting: the right stick (which the PSP doesn't have) presses the face buttons
		// (up Triangle, down X, left Square, right Circle) or the D-pad, for games that turn the
		// camera with those (Call of Duty).
		if (settings.rightStick != 0 && port >= 0) {
			const int rx = (int)pad.ANA_R_H - 128, ry = (int)pad.ANA_R_V - 128;
			const int dead = 48;
			const bool face = settings.rightStick == 1;
			if (ry < -dead) buttons |= face ? CTRL_TRIANGLE : CTRL_UP;
			if (ry > dead) buttons |= face ? CTRL_CROSS : CTRL_DOWN;
			if (rx < -dead) buttons |= face ? CTRL_SQUARE : CTRL_LEFT;
			if (rx > dead) buttons |= face ? CTRL_CIRCLE : CTRL_RIGHT;
		}
		// With the START+SELECT menu on, START+SELECT opens the in-game options, and the game shouldn't get either
		// button for that. START or SELECT alone reaches the game 0.15 s late (a quick tap: when
		// released); pressed together within that time, neither does.
		bool openOptions = false;
		{
			static bool combo = false, tapStart = false, tapSelect = false;
			static double since = 0.0;
			const bool st = pad.BTN_START, se = pad.BTN_SELECT;
			// (The in-game options need the RSX renderer; with software graphics the buttons go to the game.)
			if (!settings.inGameMenu || !rsxRenderer) {
				if (st) buttons |= CTRL_START;
				if (se) buttons |= CTRL_SELECT;
				combo = tapStart = tapSelect = false;
				since = 0.0;
			} else if (st && se && !combo) {
				combo = true;
				openOptions = true;
			} else if (combo) {
				if (!st && !se) combo = false;
				tapStart = tapSelect = false;
				since = 0.0;
			} else if (st || se) {
				if (since == 0.0) since = time_now_d();
				if (time_now_d() - since >= 0.15) {
					if (st) buttons |= CTRL_START;
					if (se) buttons |= CTRL_SELECT;
					tapStart = tapSelect = false;
				} else {
					tapStart |= st;
					tapSelect |= se;
				}
			} else {
				if (tapStart) buttons |= CTRL_START;
				if (tapSelect) buttons |= CTRL_SELECT;
				tapStart = tapSelect = false;
				since = 0.0;
			}
		}
		if (pad.BTN_L1) buttons |= CTRL_LTRIGGER;
		if (pad.BTN_R1) buttons |= CTRL_RTRIGGER;
		__CtrlUpdateButtons(buttons, previousButtons & ~buttons); previousButtons = buttons;
		float x = port < 0 ? 0.0f : ((int)pad.ANA_L_H - 128) / 128.0f;
		// The pad reports 0 at the top; PPSSPP's analog y is positive upwards.
		float y = port < 0 ? 0.0f : (128 - (int)pad.ANA_L_V) / 128.0f;
		__CtrlSetAnalogXY(0, x, y);
		// (Build 143 and before: START+SELECT held 2 s stopped the game, the "Quit combo" setting.
		// The PS button menu quits.)
		// L3 (the PSP has no such button, so the game never sees it) held half a second on its own:
		// a JIT profile sample now (see JitProfileTick). Held, so a stray stick click doesn't start one.
		static double l3Since = 0.0;
		static bool l3Fired = false;
		// L3+R3 together: the FPS overlay on or off (build 217; R3 alone before, but games may use R3
		// through the right stick setting, and a stick click is easy to make by accident). L3 still
		// held after it doesn't start a JIT profile.
		static bool previousStickCombo = false, l3UsedForCombo = false;
		const bool stickCombo = pad.BTN_L3 && pad.BTN_R3;
		if (settings.r3Fps && stickCombo && !previousStickCombo) {
			settings.showFps = !settings.showFps;
			l3UsedForCombo = true;
		}
		previousStickCombo = stickCombo;
		if (!pad.BTN_L3)
			l3UsedForCombo = false;
		// The debugging buttons only with the "Debug buttons" setting: they're easy to press by accident.
		if (settings.debugButtons && pad.BTN_L3 && !pad.BTN_R3 && !l3UsedForCombo) {
			if (l3Since == 0.0) {
				l3Since = time_now_d();
			} else if (!l3Fired && time_now_d() - l3Since > 0.5 && settings.cpuJit) {
				l3Fired = true;
				Trace("L3 held: JIT profile sample requested");
				jitProfileRequest = true;
			}
		} else {
			l3Since = 0.0;
			l3Fired = false;
		}

		if (openOptions) {
			InGameOptions();
			if (!running) return false;
		}
		// L2+R2: record the next frame's graphics commands (a GE dump) for debugging.
		static bool previousDumpCombo = false;
		const bool dumpCombo = settings.debugButtons && pad.BTN_L2 && pad.BTN_R2;
		bool dumpMemoryOk = true;
		if (dumpCombo && !previousDumpCombo) {
			// A dump holds copies of everything the frame uses (17 MB for a Call of Duty frame):
			// with little main memory left the emulator ran out and quit to the XMB.
			// Frontend 99: the free total isn't enough - a dump needs blocks of several MB in one
			// piece (Silent Hill Origins: 21 MB free in small pieces, the dump ran out and quit).
			// Build 175: the recorder keeps its data in 64 KB pieces and compresses piece by piece,
			// so it needs no big block any more; what it may use is capped at the memory free now
			// less 4 MB, and a frame that needs more is dropped (with a notice) instead.
			u32 freeBytes = 0, lv2Free = 0, total = 0;
			const u32 largest = LargestAllocatable();
			const u32 reserve = 4 * 1024 * 1024 + 2 * 1024 * 1024;  // Headroom, and the 2 MB VRAM copy.
			if (settings.l2r2 == 1) {
				// Debug tab "L2+R2 in game": a text log of the next draws instead of a dump.
				dumpMemoryOk = false;
				Trace("L2+R2: the next 3000 draws are logged here (l2r2=1)");
				g_geDrawLogLeft = 3000;
				ShowNotice("Logging the next 3000 draws to launch.log", 3.0);
			} else if (FreeMemory(&freeBytes, &lv2Free, &total) && (freeBytes < reserve + 1024 * 1024 || largest < 128 * 1024)) {
				dumpMemoryOk = false;
				// Instead, a text log of the next draws (no big buffers): what each one draws with.
				Trace("GE dump skipped: %u KB of main memory free, biggest piece %u KB (needs 7 MB); the next 3000 draws are logged here instead", freeBytes >> 10, largest >> 10);
				g_geDrawLogLeft = 3000;
				ShowNotice("GE dump skipped (memory): draws logged to launch.log", 3.0);
			} else {
				GPURecord::g_dumpByteLimit = freeBytes > reserve ? freeBytes - reserve : 1024 * 1024;
				GPURecord::g_dumpDroppedHook = [](u32 bytes) {
					Trace("GE dump dropped: the frame needed more than the %u KB of memory left for it (%u KB recorded)", (u32)(GPURecord::g_dumpByteLimit >> 10), bytes >> 10);
					ShowNotice("GE dump dropped (memory)", 3.0);
				};
				// A frame that takes this long to record is given up: the game stalls meanwhile
				// (Midnight Club: 20+ s, then a hung PS3, build 257). Since build 263 the recorder
				// finds repeated data by hash, so a frame should take a few seconds at most; this
				// is only a safety net (8 s in 261-262).
				GPURecord::g_dumpTimeLimit = 10.0;
				GPURecord::g_dumpAbandonedHook = [](double seconds, u32 bytes) {
					Trace("GE dump given up: the frame took over %.0f s to record (%u KB recorded); the game goes on", seconds, bytes >> 10);
					ShowNotice("GE dump given up (took too long)", 3.0);
				};
				Trace("GE dump: %u KB of main memory free (biggest piece %u KB), up to %u KB for the dump", freeBytes >> 10, largest >> 10, (u32)(GPURecord::g_dumpByteLimit >> 10));
			}
		}
		if (dumpCombo && !previousDumpCombo && gpu && dumpMemoryOk) {
			if (gpu->GetRecorder()->RecordNextFrame([](const Path &path) {
					// Build 186: everything stays in the dump folder (memstick/PSP/SYSTEM/DUMP), the .ram and
					// .vram files next to their dump.
					const std::string outBase = path.ToString();
					Trace("GE dump saved: %s", outBase.c_str());
					// Build 181: also what the PS3 drew in that frame, saved at the frame's Present (on
					// the emulation thread: this can run on the GE list thread).
					if (!path.empty()) {
						std::lock_guard<std::mutex> lock(presentMutex);
						if (rsxRenderer && settings.renderer && settings.dumpVram)
							pendingVramPath = outBase + ".vram";
						// Build 182: and PSP RAM (the game's code and data) at the end of that frame.
						if (settings.dumpRam)
							pendingRamPath = outBase + ".ram";
					}
				}))
				Trace("GE dump: recording the next frame");
		}
		previousDumpCombo = dumpCombo;
	}
	return running;
}

// ---------------------------------------------------------------------------------------------
// Called on the emulation thread once per PSP frame the core decided to show.

void PS3Frontend::Present() {
	const double start = time_now_d();
	{
		// A GE dump was just written: PSP RAM too (the game's code and the frame's vertex data), so
		// the code that built the frame can be run on a PC (finding JIT bugs: Call of Duty's wall).
		std::string ramPath;
		{
			std::lock_guard<std::mutex> lock(presentMutex);
			ramPath.swap(pendingRamPath);
		}
		if (!ramPath.empty()) {
			if (FILE *f = fopen(ramPath.c_str(), "wb")) {
				const size_t wrote = fwrite(Memory::GetPointerUnchecked(0x08000000), 1, Memory::g_MemorySize, f);
				fclose(f);
				Trace("GE dump: PSP RAM saved as %s (%u KB)", ramPath.c_str(), (unsigned)(wrote >> 10));
			}
		}
	}
	if (rsxRenderer) {
		const double emu = lastPresentEnd > 0.0 ? start - lastPresentEnd : 0.0;
		double ge = 0.0;
		if (settings.collectStats) {
			ge = gpuStats.perFrame.msProcessingDisplayLists;
			gpuStats.perFrame.msProcessingDisplayLists = 0.0;
		}
		std::string overlay;
		{
			std::lock_guard<std::mutex> lock(presentMutex);
			overlay = settings.showFps ? overlayText : std::string();
			if (start < noticeUntil)
				overlay = overlay.empty() ? noticeText : overlay + "  " + noticeText;
		}
		const double syncStart = time_now_d();
		PS3Profile::NoteGeBarrier();  // Showing a frame needs its drawing done too.
		GESync();
		statPresentGESync += time_now_d() - syncStart;
		if (SoftGPUTakeNewPicture() || __DisplayVideoPlaying())
			newPictures++;
		{
			// A GE dump was just written: with the RSX renderer the dump holds the frame's commands;
			// this saves their result on the RSX (all 2 MB of PSP video memory, every buffer the RSX
			// drew written back first), to compare with a replay of the dump.
			std::string vramPath;
			{
				std::lock_guard<std::mutex> lock(presentMutex);
				vramPath.swap(pendingVramPath);
			}
			if (!vramPath.empty()) {
				RSXRendererWriteBackAll(rsxRenderer);
				if (FILE *f = fopen(vramPath.c_str(), "wb")) {
					const size_t wrote = fwrite(Memory::GetPointerUnchecked(0x04000000), 1, 0x200000, f);
					fclose(f);
					Trace("GE dump: the RSX's result of that frame saved as %s (%u KB)", vramPath.c_str(), (unsigned)(wrote >> 10));
				}
			}
		}
		RSXRendererPresent(rsxRenderer, overlay);
		++frames;
		if (Rasterizer::g_spuVertexCheck && frames % 120 == 0) {
			std::string report;
			Rasterizer::TakeSpuVertexCheckReport(report);
			TraceReport(report);
		}
		flips++;
		if (frames == 1 || frames % 1800 == 0) Trace("RSX frame presented: %u", frames);
		const double end = time_now_d();
		// "Raster" here is the RSX present/write-back time.
		UpdateStats(emu, ge, end - start, 0.0);
		lastPresentEnd = end;
		return;
	}
	const double emu = lastPresentEnd > 0.0 ? start - lastPresentEnd : 0.0;
	double ge = 0.0;
	if (settings.collectStats) {
		ge = gpuStats.perFrame.msProcessingDisplayLists;  // Seconds, despite the name.
		gpuStats.perFrame.msProcessingDisplayLists = 0.0;
	}

	// Wait for the software rasterizer threads to finish this frame.
	if (gpu) gpu->Flush();
	const double flushed = time_now_d();
	if (SoftGPUTakeNewPicture() || __DisplayVideoPlaying())
		newPictures++;

	static GPUDebugBuffer buffer;
	bool ok = gpu && gpu->GetCurrentFramebuffer(buffer, GPU_DBG_FRAMEBUF_DISPLAY) &&
		buffer.GetData() && buffer.GetStride() && buffer.GetHeight();
	if (ok && presenterActive) {
		int slot;
		{
			std::lock_guard<std::mutex> lock(presentMutex);
			slot = 0;
			while (slot == readySlot || slot == busySlot) ++slot;
		}
		u32 *dst = slots[slot];
		if (!ConvertFramebuffer(buffer, dst)) {
			auto pixels = TranslateDebugBufferToCompare(&buffer, PSP_W, PSP_H);
			if (pixels.size() == PSP_W * PSP_H) {
				for (size_t i = 0; i < pixels.size(); ++i) dst[i] = pixels[i] | 0xFF000000;
			}
		}
		{
			std::lock_guard<std::mutex> lock(presentMutex);
			if (readySlot >= 0) droppedFrames++;
			readySlot = slot;
		}
		presentCond.notify_one();
		++frames;
		if (frames == 1 || frames % 1800 == 0) Trace("PSP display frame presented: %u", frames);
	}
	const double end = time_now_d();
	UpdateStats(emu, ge, flushed - start, end - flushed);
	lastPresentEnd = end;
}

// ---------------------------------------------------------------------------------------------
// Menus

static std::vector<File::FileInfo> ScanGames() {
	std::vector<File::FileInfo> games;
	const std::vector<std::string> directories = {"/dev_hdd0/PSPISO"};
	for (const auto &directory : directories) {
		std::vector<File::FileInfo> files;
		if (!File::GetFilesInDir(Path(directory), &files)) continue;
		for (const auto &file : files) {
			std::string ext = file.fullName.GetFileExtension();
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
			if (!file.isDirectory && (ext == ".iso" || ext == ".cso" || ext == ".pbp" || ext == ".elf" || ext == ".prx")) games.push_back(file);
		}
	}
	std::sort(games.begin(), games.end(), [](const File::FileInfo &a, const File::FileInfo &b) { return a.fullName.ToString() < b.fullName.ToString(); });
	PS3Frontend::Trace("Game scan: %u files", (unsigned)games.size()); return games;
}

// Edge-triggered buttons for menus.
struct MenuKeys {
	bool up = false, down = false, left = false, right = false, cross = false, circle = false, triangle = false, square = false, select = false, start = false, l1 = false, r1 = false, l2 = false, r2 = false;
	padData last{};
	void Update() {
		up = pad.BTN_UP && !last.BTN_UP; down = pad.BTN_DOWN && !last.BTN_DOWN;
		left = pad.BTN_LEFT && !last.BTN_LEFT; right = pad.BTN_RIGHT && !last.BTN_RIGHT;
		cross = pad.BTN_CROSS && !last.BTN_CROSS; circle = pad.BTN_CIRCLE && !last.BTN_CIRCLE;
		triangle = pad.BTN_TRIANGLE && !last.BTN_TRIANGLE; square = pad.BTN_SQUARE && !last.BTN_SQUARE;
		select = pad.BTN_SELECT && !last.BTN_SELECT; start = pad.BTN_START && !last.BTN_START;
		l1 = pad.BTN_L1 && !last.BTN_L1; r1 = pad.BTN_R1 && !last.BTN_R1;
		l2 = pad.BTN_L2 && !last.BTN_L2; r2 = pad.BTN_R2 && !last.BTN_R2;
		last = pad;
	}
};

static const char *const kLanguageNames[12] = {"Japanese", "English", "French", "Spanish", "German", "Italian",
	"Dutch", "Portuguese", "Russian", "Korean", "Chinese (traditional)", "Chinese (simplified)"};
static const char *const kDateFormatNames[3] = {"YYYY-MM-DD", "MM-DD-YYYY", "DD-MM-YYYY"};
// "CPU clock" menu steps (MHz, 0 = the game's own). A real PSP runs 222 or 333.
static const int kCpuClocks[] = {0, 111, 166, 222, 266, 333, 444};
static const int CPU_CLOCK_COUNT = (int)(sizeof(kCpuClocks) / sizeof(kCpuClocks[0]));

static const int SETTING_OPTIONS = 77;
// Set by the "Save for a game" row: the game list then asks which game (ChooseGame).
static bool saveForGameRequested = false;
// Set by the "Load from a game" row (build 267): the game list asks which game's config to load.
static bool loadFromGameRequested = false;

// Debug tab steps.
static const int kNanWatchSteps[5] = {0, 1, 3, 5, 7};
static const char *const kNanWatchNames[5] = {"Off", "FPU", "FPU + VFPU (slow)", "FPU + infinities", "FPU + VFPU + inf. (slow)"};
static const int kJitGenericSteps[9] = {0, 1, 2, 4, 8, 16, 32, 64, 127};
static const char *const kJitGenericNames[9] = {"Off (all compiled)", "Integer arithmetic", "Integer loads/stores", "Float/vector loads/stores",
	"FPU arithmetic", "Int<->float moves", "Vector ops", "No fused patterns", "All groups"};

static int StepIndex(const int *steps, int count, int value) {
	for (int i = 0; i < count; ++i)
		if (steps[i] == value) return i;
	return 0;
}

// "Reset to defaults" (Settings tab) shows "Done" once pressed, until another option is changed.
static bool resetDone = false;

// Changes a setting one step (option: its number in the tables below).
static void ChangeSetting(int option, int delta) {
	resetDone = false;
	switch (option) {
	case 0: settings.screenMode = (settings.screenMode + (int)PS3Frontend::ScreenMode::COUNT + delta) % (int)PS3Frontend::ScreenMode::COUNT; break;
	case 1: settings.bilinear = !settings.bilinear; break;
	case 2: settings.frameskip += delta; if (settings.frameskip > 3) settings.frameskip = -1; if (settings.frameskip < -1) settings.frameskip = 3; break;
	case 3: settings.speedLimit = !settings.speedLimit; break;
	case 4: settings.showFps = !settings.showFps; break;
	case 5: if (settings.debugButtons) settings.logLevel = (settings.logLevel + 3 + delta) % 3; break;
	case 6: settings.splineQuality = (settings.splineQuality + 3 + delta) % 3; break;
	case 7: settings.renderer ^= 1; break;
	case 8: settings.resolution = (settings.resolution - 1 + 3 + delta) % 3 + 1; break;
	case 9: settings.vramSyncEveryFrame = !settings.vramSyncEveryFrame; break;
	case 10: settings.collectStats = !settings.collectStats; break;
	case 11: settings.cpuJit = !settings.cpuJit; break;
	case 12: settings.sound = !settings.sound; break;
	case 13: settings.pspMem64 = !settings.pspMem64; break;
	case 14: settings.fast3d = (settings.fast3d + 3 + delta) % 3; break;
	case 15:
		settings.geThread = !settings.geThread;
		break;
	case 16: settings.vmx = !settings.vmx; break;
	case 17: settings.spuVertex = !settings.spuVertex; break;
	case 18: settings.spuTexture = !settings.spuTexture; break;
	case 19: settings.vertexRingMain = !settings.vertexRingMain; break;
	case 20: settings.textureCheckFull = !settings.textureCheckFull; break;
	case 21: settings.writeBackAllRows = !settings.writeBackAllRows; break;
	case 22: settings.rttCopyReuse = !settings.rttCopyReuse; break;
	case 23: settings.tlMerge = !settings.tlMerge; break;
	case 24: settings.jitProfile = !settings.jitProfile; break;
	case 25: settings.jitWarmup = !settings.jitWarmup; break;
	case 26: settings.rsxTiled = settings.rsxTiled == 1 ? (delta < 0 ? 0 : 2) : settings.rsxTiled == 2 ? (delta < 0 ? 1 : 0) : (delta < 0 ? 2 : 1); break;  // On, Screens only, Off
	case 27: settings.geTiming = settings.geTiming >= 400 ? 100 : settings.geTiming + 100; break;
	case 28: settings.fpsCap = CycleCap(kFpsCaps, settings.fpsCap, delta); break;
	case 29: {
		// Every option on the Settings and Advanced tabs back to its default (not the Debug tab).
		// Up to build 262 only the older performance switches were reset (GE thread ... tiling),
		// which are rarely changed; in 263 the whole Advanced tab. Since 264 it is on the
		// Settings tab and resets both.
		const PS3Frontend::Settings d;
		settings.screenMode = d.screenMode; settings.bilinear = d.bilinear; settings.anisotropy = d.anisotropy;
		settings.fxaa = d.fxaa; settings.renderer = d.renderer; settings.resolution = d.resolution;
		settings.vramSyncEveryFrame = d.vramSyncEveryFrame; settings.fast3d = d.fast3d; settings.splineQuality = d.splineQuality;
		settings.frameskip = d.frameskip; settings.frameskipKeep = d.frameskipKeep; settings.speedLimit = d.speedLimit;
		settings.fpsCap = d.fpsCap; settings.showFps = d.showFps; settings.cpuJit = d.cpuJit; settings.cpuClock = d.cpuClock;
		settings.pspMem64 = d.pspMem64; settings.discFonts = d.discFonts; settings.sound = d.sound; settings.volume = d.volume;
		settings.audioSlow = d.audioSlow; settings.rightStick = d.rightStick; settings.confirmButton = d.confirmButton;
		settings.language = d.language; settings.dateFormat = d.dateFormat; settings.timeFormat = d.timeFormat;
		settings.nickname = d.nickname;
		settings.geThread = d.geThread; settings.spuVertex = d.spuVertex; settings.spuTexture = d.spuTexture;
		settings.vertexRingMain = d.vertexRingMain; settings.textureCheckFull = d.textureCheckFull;
		settings.writeBackAllRows = d.writeBackAllRows; settings.rttCopyReuse = d.rttCopyReuse; settings.tlMerge = d.tlMerge;
		settings.rsxTiled = d.rsxTiled; settings.geTiming = d.geTiming; settings.lateDepth = d.lateDepth;
		settings.keepLayouts = d.keepLayouts; settings.lensFlare = d.lensFlare; settings.geListThread = d.geListThread;
		settings.vmx = d.vmx; settings.jitWarmup = d.jitWarmup; settings.jitLarge = d.jitLarge; settings.ftz = d.ftz;
		settings.sasDirect = d.sasDirect; settings.ioHost = d.ioHost; settings.volatileWipe = d.volatileWipe;
		settings.videos = d.videos; settings.videoDeblock = d.videoDeblock; settings.videoCap = d.videoCap;
		settings.inGameMenu = d.inGameMenu; settings.r3Fps = d.r3Fps;
		resetDone = true;
		return;
	}
	case 30: settings.quitCombo = !settings.quitCombo; break;
	case 31: settings.debugButtons = !settings.debugButtons; break;
	case 32: settings.rightStick = (settings.rightStick + 3 + delta) % 3; break;
	case 33: settings.ftz = settings.ftz < 0 ? 1 : (settings.ftz == 1 ? 0 : -1); break;  // Auto -> On -> Off
	case 34: settings.videos = settings.videos < 0 ? 1 : (settings.videos == 1 ? 0 : -1); break;  // Auto -> On -> Off
	case 35: settings.frameskipKeep = !settings.frameskipKeep; break;
	case 36: settings.volatileWipe = !settings.volatileWipe; break;
	case 37: settings.lateDepth = !settings.lateDepth; break;
	case 38: settings.keepLayouts = !settings.keepLayouts; break;
	case 39: settings.audioSlow = (settings.audioSlow + 3 + delta) % 3; break;
	case 40: settings.sasDirect = !settings.sasDirect; break;
	case 41: settings.ioHost = !settings.ioHost; break;
	case 42: settings.jitLarge = !settings.jitLarge; break;
	case 43: settings.geListThread = (settings.geListThread + 4 + delta) % 4; break;
	case 44: settings.lensFlare = !settings.lensFlare; break;
	case 45: settings.cpuRows = (settings.cpuRows + 1 + 3 + delta) % 3 - 1; break;
	case 46: settings.confirmButton = (settings.confirmButton + 1 + 3 + delta) % 3 - 1; break;
	case 47: settings.language = (settings.language + 1 + 13 + delta) % 13 - 1; break;
	case 48: settings.dateFormat = (settings.dateFormat + 1 + 4 + delta) % 4 - 1; break;
	case 49: settings.timeFormat = (settings.timeFormat + 1 + 3 + delta) % 3 - 1; break;
	case 50: settings.nickname = settings.nickname.empty() ? "PPSSPP" : ""; break;
	case 51: {
		// The next or previous step (also from a value set in settings.ini that isn't a step).
		int i = 0;
		while (i < CPU_CLOCK_COUNT && kCpuClocks[i] < settings.cpuClock) i++;
		const bool exact = i < CPU_CLOCK_COUNT && kCpuClocks[i] == settings.cpuClock;
		const int next = exact || delta < 0 ? i + delta : i;
		settings.cpuClock = kCpuClocks[(next + CPU_CLOCK_COUNT) % CPU_CLOCK_COUNT];
		break;
	}
	case 53: settings.anisotropy = kAniso[(AnisoIndex(settings.anisotropy) + 5 + delta) % 5]; break;
	case 52: settings.volume = (settings.volume / 10 * 10 + 110 + delta * 10) % 110; break;
	case 54: settings.discFonts = !settings.discFonts; break;
	case 55: settings.logPerf = (settings.logPerf + 3 + delta) % 3; break;
	case 56: settings.logStall = !settings.logStall; break;
	case 57: settings.logVideo = !settings.logVideo; break;
	case 58: settings.logSave = !settings.logSave; break;
	case 59: settings.logGeWatch = !settings.logGeWatch; break;
	case 60: settings.logRsx = !settings.logRsx; break;
	case 61: settings.nanWatch = kNanWatchSteps[(StepIndex(kNanWatchSteps, 5, settings.nanWatch) + 5 + delta) % 5]; break;
	case 62: settings.storeWatch = !settings.storeWatch; break;
	case 63: settings.jitGeneric = kJitGenericSteps[(StepIndex(kJitGenericSteps, 9, settings.jitGeneric) + 9 + delta) % 9]; break;
	case 64: break;  // jitops: text, only in an ini file.
	case 65: settings.l2r2 ^= 1; break;
	case 66: settings.logJit = !settings.logJit; break;
	case 67: settings.dumpRam = !settings.dumpRam; break;
	case 68: settings.dumpVram = !settings.dumpVram; break;
	case 69: settings.videoDeblock = settings.videoDeblock < 0 ? 1 : (settings.videoDeblock == 1 ? 0 : -1); break;  // Auto -> On -> Off
	case 70: settings.inGameMenu = !settings.inGameMenu; break;
	case 71: settings.r3Fps = !settings.r3Fps; break;
	case 72: settings.videoCap = CycleCap(kVideoCaps, settings.videoCap, delta); break;
	case 73: saveForGameRequested = true; break;  // The game list asks which game (ChooseGame).
	case 74: settings.soundDebug = (settings.soundDebug + 3 + delta) % 3; break;
	case 75: settings.fxaa = NextFxaa(settings.fxaa); break;
	case 76: loadFromGameRequested = true; break;  // The game list asks which game.
	}
}

// Every setting's label and value, by option number.
static std::vector<MenuScreens::SettingRow> SettingRows() {
	auto onOff = [](bool v) { return std::string(v ? "On" : "Off"); };
	static const char *screenShort[] = {"Fit (keep aspect)", "Stretch", "Fit 90% (overscan)", "Pixel 2x (960x544)"};
	static const char *logShort[] = {"Off", "Warnings", "Full debug (slow)"};
	static const char *splineShort[] = {"Low", "Medium", "High"};
	char shortSkip[32];
	if (settings.frameskip < 0) snprintf(shortSkip, sizeof(shortSkip), "Auto");
	else if (settings.frameskip == 0) snprintf(shortSkip, sizeof(shortSkip), "Off");
	else snprintf(shortSkip, sizeof(shortSkip), "%d (draw 1 of %d)", settings.frameskip, settings.frameskip + 1);
	const bool rsx = settings.renderer != 0;
	const bool dbg = settings.debugButtons;
	return {
		{"Screen size", screenShort[settings.screenMode]},
		{"Filtering", settings.bilinear ? "Bilinear (smooth)" : "Nearest (sharp)"},
		{"Frameskip", shortSkip},
		{"Speed limit", settings.speedLimit ? "On (PSP speed)" : "Off"},
		{"FPS overlay", onOff(settings.showFps)},
		{"Core logging", settings.debugButtons ? logShort[settings.logLevel] : "Off (needs Debug on)", !settings.debugButtons},
		{"Curve quality", splineShort[settings.splineQuality]},
		{"Renderer", rsx ? "RSX hardware" : "Software"},
		{"Resolution", std::to_string(settings.resolution) + "x (" + std::to_string(480 * settings.resolution) + "x" + std::to_string(272 * settings.resolution) + ")" + (rsx ? "" : " RSX only")},
		{"VRAM sync", std::string(settings.vramSyncEveryFrame ? "Every frame" : "Minimal") + (rsx ? "" : " (RSX only)")},
		{"CPU/GE timing", onOff(settings.collectStats), !settings.debugButtons},
		{"CPU core", settings.cpuJit ? "JIT" : "IR interpreter"},
		{"Sound", onOff(settings.sound)},
		{"PSP memory", settings.pspMem64 ? "64 MB" : "32 MB"},
		{"Fast 3D path", settings.fast3d == 2 ? "RSX transform & lighting" : settings.fast3d == 1 ? "CPU transform" : "Off"},
		{"GE thread", settings.geThread ? "On" : "Off"},
		{"JIT floats", settings.vmx ? "AltiVec" : "FPU"},
		{"SPU vertices", onOff(settings.spuVertex)},
		{"SPU textures", onOff(settings.spuTexture)},
		{"Vertex data", settings.vertexRingMain ? "Main memory" : "RSX memory"},
		{"Texture check", settings.textureCheckFull ? "Every use" : "After writes"},
		{"Write-back", settings.writeBackAllRows ? "All rows" : "Drawn rows"},
		{"Screen copies", settings.rttCopyReuse ? "Reused" : "Fresh each draw"},
		{"Draw merging", onOff(settings.tlMerge)},
		{"JIT profile", settings.cpuJit ? onOff(settings.jitProfile) : std::string("Off (needs the JIT)"), !settings.debugButtons || !settings.cpuJit},
		{"JIT warm-up", onOff(settings.jitWarmup)},
		{"RSX tiling", settings.rsxTiled == 2 ? "Screens only" : settings.rsxTiled ? "On" : "Off"},
		{"GE timing", std::to_string(settings.geTiming) + "%" + (settings.geTiming == 100 ? " (default)" : "")},
		{"Frame cap", settings.fpsCap == 60 ? "60 fps (no cap)" : std::to_string(settings.fpsCap) + " fps"},
		{"Reset to defaults", resetDone ? "Done" : ""},
		{"Quit combo", settings.quitCombo ? "On (hold 2 s)" : "Off"},
		{"Debug", settings.debugButtons ? "On (logs, test buttons)" : "Off (no logs)"},
		{"Right stick", settings.rightStick == 1 ? "Face buttons" : settings.rightStick == 2 ? "D-pad" : "Off"},
		{"Denormals (FTZ)", settings.ftz < 0 ? "Auto" : settings.ftz ? "Flush to zero" : "Keep"},
		{"Videos", !MediaEngineHasFFmpeg() ? "Off (no FFmpeg)" : settings.videos < 0 ? "Auto" : settings.videos ? "On" : "Off"},
		{"Skip keeps menus", settings.frameskipKeep ? "On (experimental)" : "Off"},
		{"Dialog memory wipe", settings.volatileWipe ? "On (as PPSSPP)" : "Off"},
		{"Late depth reads", settings.lateDepth ? "On (1 frame late)" : "Off (exact)"},
		{"Keep buffer layouts", settings.keepLayouts ? "On (experimental)" : "Off"},
		{"Sound when slow", settings.audioSlow == 1 ? "Wait and refill" : settings.audioSlow == 2 ? "Play slower" : "Short gaps"},
		{"Sound mixing", settings.sasDirect ? "Emulation thread" : "Own thread"},
		{"File loading", settings.ioHost ? "Host (experimental)" : "Fast"},
		{"JIT cache", settings.jitLarge ? "Large (experimental)" : "Normal"},
		{"GE lists", settings.geListThread == 3 ? "Don't wait (HACK)" : settings.geListThread == 2 ? "Own thread, run ahead" : settings.geListThread ? "Own thread (exp.)" : "Emulation thread"},
		{"Lens flare occlusion", settings.lensFlare ? "On" : "Off"},
		{"CPU writes to drawn", settings.cpuRows < 0 ? "Auto" : settings.cpuRows ? "On" : "Off"},
		{"Confirm button", settings.confirmButton < 0 ? "Auto (as PS3)" : settings.confirmButton ? "X" : "Circle"},
		{"Language", settings.language < 0 ? std::string("Auto (as PS3)") : std::string(kLanguageNames[settings.language])},
		{"Date format", settings.dateFormat < 0 ? "Auto (as PS3)" : kDateFormatNames[settings.dateFormat]},
		{"Time format", settings.timeFormat < 0 ? "Auto (as PS3)" : settings.timeFormat ? "12-hour" : "24-hour"},
		{"Nickname", settings.nickname.empty() ? std::string("PS3 nickname") : settings.nickname},
		{"CPU clock", settings.cpuClock > 0 ? std::to_string(settings.cpuClock) + " MHz" : std::string("Default")},
		{"Volume", std::to_string(settings.volume) + "%"},
		{"Anisotropic filter", AnisoName(settings.anisotropy)},
		{"PSP fonts", settings.discFonts ? "Game's own" : "Emulated"},
		{"Perf report", settings.logPerf == 2 ? "Every 2 s" : settings.logPerf ? "Every 10 s" : "Off", !dbg},
		{"Stall reports", onOff(settings.logStall), !dbg},
		{"Video statistics", onOff(settings.logVideo), !dbg},
		{"Save dialog steps", onOff(settings.logSave), !dbg},
		{"GE list watch", onOff(settings.logGeWatch), !dbg},
		{"RSX renderer events", onOff(settings.logRsx), !dbg},
		{"NaN watch", settings.cpuJit ? std::string("Needs IR interpreter") : kNanWatchNames[StepIndex(kNanWatchSteps, 5, settings.nanWatch)], !dbg || settings.cpuJit},
		{"Store watch", settings.cpuJit ? std::string("Needs IR interpreter") : onOff(settings.storeWatch), !dbg || settings.cpuJit},
		{"JIT: interpret", kJitGenericNames[StepIndex(kJitGenericSteps, 9, settings.jitGeneric)], !settings.cpuJit},
		{"JIT: interpret ops", settings.jitOps.empty() ? std::string("None (jitops= in an ini)") : settings.jitOps, true},
		{"L2+R2 in game", settings.l2r2 ? "Log the next 3000 draws" : "GE dump (next frame)", !dbg},
		{"JIT events", onOff(settings.logJit), !dbg},
		{"Dump: PSP RAM", onOff(settings.dumpRam), !dbg || settings.l2r2 == 1},
		{"Dump: RSX picture", rsx ? onOff(settings.dumpVram) : std::string("Off (RSX only)"), !dbg || settings.l2r2 == 1 || !rsx},
		{"Video deblocking", settings.videoDeblock < 0 ? "Auto (off if slow)" : settings.videoDeblock ? "On (smoother)" : "Off (faster)", settings.videos == 0},
		{"START+SELECT menu", settings.inGameMenu ? (rsx ? "On" : "On (RSX renderer)") : "Off", !rsx},
		{"L3+R3 FPS toggle", onOff(settings.r3Fps)},
		{"Frame cap in videos", VideoCapName(settings.videoCap)},
		{"Save for a game", "Press X: choose the game"},
		{"Sound debug", settings.soundDebug == 2 ? "Log, voices on the PPU" : settings.soundDebug ? "Log" : "Off", !dbg},
		{"Edge smoothing", rsx ? std::string(FxaaName(settings.fxaa)) : std::string("Off (RSX only)"), !rsx},
		{"Load from a game", "Press X: choose the game"},
	};
}

	static const char *const kSettingHelp[SETTING_OPTIONS] = {
		"How the PSP picture fills the TV. Fit keeps its shape, Stretch fills the screen, Fit 90% leaves room for TVs that crop the edges, Pixel 2x is exactly 960x544.",
		"How the picture is scaled up to the TV: Bilinear is smooth, Nearest keeps sharp, blocky pixels.",
		"Auto skips drawing frames only when the game runs slow (up to 4 in a row). A number always draws 1 of every N+1 frames.",
		"Keeps the emulated PSP from running faster than a real one (its clock, music and timers). Leave it on. Off runs as fast as the PS3 can. To limit the frame rate, use Frame cap.",
		"Frames per second in the corner of the screen. With Advanced > L3+R3 FPS toggle on, L3+R3 turns it on and off in a game.",
		"PPSSPP's own log in core.log, copied to the game's log folder. A switch of its own: Debug on doesn't turn it on, but it needs Debug (greyed out without). Full debug is very slow: only for bug reports. ini: log=0/1/2",
		"Detail of curved surfaces (splines and beziers) in games that use them. Lower is faster.",
		"RSX draws with the PS3's graphics chip: fast, and allows 2x/3x resolution. Software is the slow fallback if graphics break.",
		"Internal resolution of the RSX renderer. Higher is sharper and needs more RSX work.",
		"Every frame copies the PSP's screen memory each frame (most compatible). Minimal is faster; some screen effects may break.",
		"Adds PPSSPP's own count of display list time to the Perf report (\"Perf: display lists\") and times every HLE call. Slightly slower. Needs Debug. ini: geprofile=1",
		"The JIT turns PSP code into PS3 code: much faster. Needs PS3MAPI (CFW). The IR interpreter is the slow fallback.",
		"Sound output. It stutters while a game runs below full speed.",
		"32 MB (PSP-1000) leaves more PS3 memory to the emulator. 64 MB only for games that need a PSP-2000 or later.",
		"RSX transform & lighting is the fastest. CPU transform and Off are slower fallbacks for broken 3D graphics.",
		"3D drawing on the PS3's second thread, in parallel with the game. Off: all on one thread (slower).",
		"Which PS3 registers hold the PSP's float and vector values. AltiVec is faster.",
		"Vertex decoding and skinning on the SPUs instead of the PPU (needs GE thread).",
		"Texture change checks and decoding on the SPUs instead of the PPU.",
		"Where vertices for the RSX are written: main memory (build 43 and later) or RSX memory.",
		"When textures are checked for changes: after the game writes memory, or every use (before build 42, slower).",
		"How much of a screen drawn by the RSX is copied back to PSP memory: the drawn rows, or all (before build 41).",
		"Copies of the screen that the game uses as a texture are reused within a frame (build 40), or made fresh for every draw.",
		"Merges similar 3D draws that follow each other into one RSX draw (build 42).",
		"Where the PSP code spends its time: JIT counters all game (a little slower) and 10 s samples at 40 s and every minute after. Hold L3 in game for a 5 s sample any time. Needs Debug and the JIT. ini: jitsample=1",
		"New code runs in the interpreter 20 times before it is compiled: fewer hitches from code that runs only once.",
		"Render targets in tiled RSX memory: faster RSX drawing at 2x/3x. Screens only: screen buffers tiled, depth buffers not (for surfaces drawn twice at the same depth that flicker). Off: nothing tiled; turn off if screens look scrambled. ini: rsxtile=1/2/0",
		"How long the emulated GE takes per vertex. 200-400 can make heavy scenes run at 30 game frames instead of 60, with half the work. 100 if a game runs in slow motion.",
		"Most frames a second the game may make: 60 (no cap, as on a PSP), 30 or 20. A game that would run faster does less work. 20 for games that can't hold 30. ini: fpscap=60/30/20",
		"Sets every option on this tab and on the Advanced tab back to its default (not the Debug tab). Settings saved for a game (Save for a game) are not changed.",
		"Hold START+SELECT for 2 seconds in a game to stop it. Off if you press them by accident; the PS button menu can still quit.",
		"The master switch. On: launch.log (in USRDIR/logs/<game>/), the switches on this tab, and in a game hold L3 for a JIT profile, L2+R2 for a GE dump. Off: no logs at all. ini: debug=1",
		"The PSP has no right stick. Face buttons: up/down/left/right press Triangle/X/Square/Circle (camera in Call of Duty). D-pad: it presses the arrows.",
		"Tiny float values (denormals): flushed to zero when the game asks, as on a PSP, or kept. Auto = flushed. Per game: ftz=",
		"Video cutscenes (needs the FFmpeg build). Off: not decoded; most games then skip them. Auto plays them, like On. Can be set per game (videos=).",
		"EXPERIMENTAL. With Frameskip on, skipped frames still draw what a game draws only now and then (menus, loading screens). Helped Gran Turismo's menus. Per game: skipkeep=1",
		"Leave this Off. On: when a save or message dialog closes, the 4 MB of memory the game lent it is zeroed, as desktop PPSSPP does. Per game: volwipe=1",
		"EXPERIMENTAL. A depth buffer read as a texture (Midnight Club, once a frame) gets the previous frame's depth: no waiting for the RSX, but the effect lags a frame. Per game: latedepth=1",
		"EXPERIMENTAL. One buffer drawn in two layouts (Midnight Club) keeps both on the RSX, no wait at each switch. The game's CPU then can't read those pixels. Per game: keeplayouts=1",
		"While the game runs below full speed. Short gaps: crackles. Wait and refill: fewer, longer gaps. Play slower: no gaps, the sound slows down and drops in pitch. Per game: audioslow=",
		"EXPERIMENTAL. Emulation thread: the game's sound (SAS) is mixed on an SPU straight from the emulation thread, without a thread hand-over (~0.3 ms a mix). Faster. Per game: sasdirect=1",
		"EXPERIMENTAL. Host: while a file is read (Midnight Club streams its city as you drive), the game keeps running instead of the whole emulator waiting for the disk. Per game: iohost=1",
		"EXPERIMENTAL. Large: room for 50% more compiled code before the JIT starts over and compiles everything again (hitches). Needs ~9 MB more memory. Per game: jitlarge=1",
		"EXPERIMENTAL. Own thread: lists on another thread, same timing. Run ahead: GE seems slower. Don't wait: the game doesn't wait for its lists (HACK, may glitch). Per game: gelists=1-3",
		"Off: the depth the game reads once a frame to hide the sun's lens flare behind things isn't copied back from the RSX (~2.5 ms a frame in Midnight Club). Per game: lensflare=0",
		"On: a buffer the game wrote with its CPU after drawing into it shows those rows. Auto: on for Silent Hill Origins (its loading screen). Per game: cpurows=1",
		"The button that means OK in the game's menus. Auto: the same as your PS3 (X or Circle). Takes effect when a game starts. Per game: confirm=1 (X) or 0 (Circle)",
		"The PSP system language, for games with several languages. Auto: your PS3's language (English if the PSP doesn't have it). Takes effect when a game starts. Per game: language=",
		"How games that show dates write them. Auto: as your PS3. Takes effect when a game starts. Per game: dateformat=0 (YYYY-MM-DD), 1 (MM-DD-YYYY), 2 (DD-MM-YYYY)",
		"How games that show the time write it. Auto: as your PS3. Takes effect when a game starts. Per game: timeformat=0 (24-hour) or 1 (12-hour)",
		"The PSP user name some games show (save files, multiplayer names). Your PS3 nickname, or PPSSPP. Any name: nickname=YourName in settings.ini.",
		"Emulated PSP CPU speed. Higher can smooth games limited by the PSP's CPU, but needs more PS3 time; lower does less work. Some games break. Per game: cpuclock=",
		"Game sound volume. Can also be changed in a game (the START+SELECT menu). Per game: volume=0-100",
		"Sharper textures on surfaces seen at an angle (roads, floors) when the game filters them. Costs some RSX time. Also in the in-game options. Per game: anisotropy=0/2/4/8/16",
		"Game's own: the PSP's fonts are unpacked from the game disc's updater and the game's own font library draws text (may fix Silent Hill Origins' letter spacing). Per game: discfonts=1",
		"\"Perf:\" lines: frame rate, speed, where each frame's time goes (PSP CPU, GE, drawing, waiting), JIT and HLE calls, memory. For slow games. Every 2 s writes more often (more log, a little slower). ini: perflog=0/1/2",
		"\"STALL:\" lines when no frame comes for 5 s: what each PSP thread waits for, the code it is stuck in, the last HLE calls, the GE and the save dialog. For freezes. ini: stalllog=0",
		"\"Video\" lines once a second during a cutscene: pictures decoded, audio, disc reads, what the game's threads wait for, GE syncs. For jerky or frozen videos. ini: videolog=0",
		"\"Savedata:\" lines: each step of a save/load dialog, sizes and free space it reported, how long the reads and writes took. For save problems. ini: savelog=0",
		"\"GE list watch\" lines: code that wrote over a display list the GE hadn't finished, and display lists the game runs from VRAM. For garbage or missing graphics. ini: gewatchlog=0",
		"\"RSX renderer:\" lines: new framebuffers and their layouts, write-backs to PSP memory, depth and texture reads from drawn buffers, unusual blends. For RSX-only glitches. ini: rsxlog=0",
		"Names the PSP code that first makes a NaN (not-a-number) float: for broken cameras, objects flying away, black screens. Needs the IR interpreter (CPU core). VFPU is slow. ini: nanwatch=1/3/5/7",
		"Names the code that wrote vertex colours a draw finds half-written: for flickering or wrongly coloured models. Needs the IR interpreter. Costs nothing when off. ini: storewatch=1",
		"The JIT leaves one group of instructions to the interpreter. If a bug seen with the JIT goes away with one group, that group of the JIT has the bug. Slower. ini: jitgeneric=0-127 (sum of groups)",
		"Single instructions left to the interpreter: a finer search than the groups. Only set in an ini file: jitops=Add,Sub (IR op names, comma separated). Shown here when set.",
		"GE dump: the next frame's graphics commands saved to memstick/PSP/SYSTEM/DUMP (to replay on a PC). Draw log: the next 3000 draws written to launch.log as text (no big memory needed). ini: l2r2=0/1",
		"\"JIT cache full\" (all code is compiled again: a hitch) and blocks the JIT had to compile again. For hitches every so often with the JIT. ini: jitlog=0",
		"With each GE dump, PSP RAM (the game's code and data, 32 or 64 MB) saved as .ram next to it: to look at the game's code and data in that frame. ini: dumpram=0",
		"With each GE dump, what the RSX drew in that frame saved as .vram next to it (RSX renderer): to compare with the software result. ini: dumpvram=0",
		"Smooths blocky edges in videos, but on the PS3 it can nearly double decoding time. Auto: on, and off for a movie that falls below full speed (sound crackles then). Per game: videodeblock=-1/1/0",
		"START+SELECT in a game pauses it and opens the in-game options (frameskip, GE timing, volume...; RSX renderer). Off: START and SELECT go straight to the game, with no delay. ini: ingamemenu=0",
		"L3+R3 (both sticks pressed in) in a game shows or hides the FPS counter and the other numbers in the corner. Off: the combination does nothing. ini: r3fps=0",
		"While a video plays: Off (no cap), 40 or 30 fps, or Auto: the Frame cap once a video runs slow. Any cap is lifted while the game is behind the video, until it catches up. ini: videocap=60/40/30/-1",
		"Saves the Basic options and Advanced settings shown now as one game's config (USRDIR/games/<game>.ini; replaces it). That game then starts with them; other games keep these tabs.",
		"\"Sound:\" lines every 2 s: volumes, loudest samples and clipping at each step of the sound (channels, SAS voices, ATRAC). For distorted sound. Voices on the PPU: mixed without the SPU, to compare. ini: snddebug=0/1/2",
		"Smooths jagged edges of the picture (FXAA, before it is scaled to the TV). Light: smooths less, text stays sharper. Full: smoother edges, softer text (most at 1x). Costs some RSX time. Also in the in-game options. Per game: fxaa=2 Light, fxaa=1 Full",
		"Loads one game's config (games/<game>.ini) into the Basic options and Advanced tabs, to change it here. Save for a game then writes it back. Games without a config use these tabs too.",
	};

// A tab of settings: option numbers, the left column first.
struct SettingsPage {
	// Option numbers, the left column first; a negative entry is a section heading: -1 - its index
	// in headings.
	std::vector<int> order;
	std::vector<const char *> headings;
	int split;
	int selection;
};

// Quit combo (30) and CPU writes to drawn buffers (45, crashed Silent Hill Origins from the menu in
// build 159) are only in settings.ini.
static SettingsPage generalPage = {
	// Build 270: the right column is CONFIG PER GAME (Load, Save), RESET, CONTROLS, PSP SYSTEM, AUDIO.
	{-1, 0, 1, 53, 75, 7, 8, 9, 14, 6, 2, 35, 3, 28, 4, 11, 51, 13, 54,
	 -5, 76, 73, -6, 29, -3, 32, 46, -4, 47, 48, 49, 50, -2, 12, 52, 39},
	{"GRAPHICS", "AUDIO", "CONTROLS", "PSP SYSTEM", "CONFIG PER GAME", "RESET"}, 19, 1};
static SettingsPage advancedPage = {
	{-1, 15, 17, 18, 19, 20, 21, 22, 23, 26, 27, 37, 38, 44, 43,
	 -2, 16, 25, 42, 33, 40, 41, 36, -3, 34, 69, 72, -4, 70, 71},
	{"GRAPHICS", "OTHER", "VIDEO PROCESSING", "BUTTONS"}, 15, 1};
static SettingsPage debugPage = {
	{-1, 31, 5, 55, 10, 56, 57, 58, 59, 60, 66, 74,
	 -2, 65, 67, 68, 24, 61, 62, 63, 64},
	{"LOGS", "FINDING A BUG"}, 12, 1};
static bool settingsChanged = false;

static void SaveChangedSettings() {
	if (!settingsChanged)
		return;
	settingsChanged = false;
	SaveSettings();
	SetTraceEnabled(settings.debugButtons);
	ApplyLogMask();
	OpenLogFiles();
	std::string text = SettingsText(settings);
	for (char &c : text)
		if (c == '\n') c = ' ';
	PS3Frontend::Trace("Settings saved: %s", text.c_str());
}

// The buttons on a settings tab; true when the cursor goes up to the tabs. Headings are skipped.
static bool SettingsPageInput(MenuKeys &keys, SettingsPage &page) {
	const int count = (int)page.order.size();
	int &sel = page.selection;
	const bool right = sel >= page.split;
	const int start = right ? page.split : 0, end = right ? count : page.split;
	auto isOption = [&](int i) { return page.order[i] >= 0; };
	if (keys.circle)
		return true;
	if (keys.up) {
		int i = sel - 1;
		while (i >= start && !isOption(i))
			i--;
		if (i < start)
			return true;
		sel = i;
	}
	if (keys.down) {
		int i = sel + 1;
		while (i < end && !isOption(i))
			i++;
		if (i < end)
			sel = i;
	}
	if (keys.l2 || keys.r2) {
		const int otherStart = right ? 0 : page.split, otherEnd = right ? page.split : count;
		if (otherEnd > otherStart) {
			const int i = std::min(otherStart + (sel - start), otherEnd - 1);
			int j = i;
			while (j < otherEnd && !isOption(j))
				j++;
			if (j >= otherEnd) {
				j = i;
				while (j >= otherStart && !isOption(j))
					j--;
			}
			if (j >= otherStart)
				sel = j;
		}
	}
	const int delta = keys.right || keys.cross ? 1 : (keys.left ? -1 : 0);
	if (delta) {
		ChangeSetting(page.order[sel], delta);
		settingsChanged = true;
	}
	return false;
}

// Menus without the canvas (it didn't fit in memory): a plain list.
static void PlainMenu(const char *title, const std::vector<std::string> &lines, int selected, const char *footer) {
	const int visible = 15;
	const int count = (int)lines.size();
	const int top = std::max(0, std::min(selected - visible / 2, count - visible));
	Clear(); Text(50, 30, title, 2);
	for (int i = top; i < top + visible && i < count; ++i) {
		SDL_SetTextureColorMod(font, i == selected ? 90 : 255, 240, i == selected ? 140 : 255);
		Text(50, 96 + (i - top) * 30, ((i == selected ? "> " : "  ") + lines[i]).substr(0, 100).c_str());
	}
	SDL_SetTextureColorMod(font, 255, 255, 255);
	Text(50, 615, footer);
	SDL_RenderPresent(renderer); SDL_Delay(16);
}

// Canvas: drawn but not presented (a popup may go on top); true. Plain menus: shown; false.
static bool DrawSettingsPage(const MenuScreens::Tabs &tabs, const SettingsPage &page, bool cursorOnPage, const char *tabHelp) {
	const std::vector<MenuScreens::SettingRow> all = SettingRows();
	std::vector<MenuScreens::SettingRow> shown;
	for (int option : page.order) {
		if (option >= 0) {
			shown.push_back(all[option]);
		} else {
			MenuScreens::SettingRow heading;
			heading.label = page.headings[-1 - option];
			heading.header = true;
			shown.push_back(heading);
		}
	}
	if (MenuCanvasReady()) {
		MenuScreens::Settings(time_now_d(), tabs, FRONTEND_VERSION_BAR, shown, page.split, "", "",
			cursorOnPage ? page.selection : -1, cursorOnPage ? kSettingHelp[page.order[page.selection]] : tabHelp,
			cursorOnPage ? "Up/Down: select  Left/Right/X: change  L2/R2: column  L1/R1: tabs" : "Left/Right or L1/R1: tabs   X/Down: open");
		return true;
	}
	std::vector<std::string> lines;
	for (const auto &row : shown)
		lines.push_back(row.header ? row.label : row.label + ": " + row.value);
	PlainMenu(tabs.names[tabs.active], lines, cursorOnPage ? page.selection : -1,
		cursorOnPage ? "Up/Down: select   Left/Right/X: change   Circle: tabs" : "Left/Right: tabs   X: open");
	return false;
}

// Scrollable list of report lines (self-test results).
static void ShowReport(MenuKeys &keys, const char *title, const std::vector<std::string> &lines, const char *note = "saved in USRDIR") {
	size_t top = 0;
	const size_t visible = MenuCanvasReady() ? MenuScreens::ReportVisibleLines() : 17;
	while (PS3Frontend::Pump(false)) {
		keys.Update();
		if (keys.circle || keys.cross) break;
		if (keys.down && top + visible < lines.size()) top++;
		if (keys.up && top > 0) top--;
		if (keys.right) top = std::min(top + visible, lines.size() > visible ? lines.size() - visible : 0);
		if (keys.left) top = top > visible ? top - visible : 0;
		if (MenuCanvasReady()) {
			MenuScreens::Report(time_now_d(), title, lines, top, visible, note);
			MenuCanvasPresent();
			SDL_Delay(8);
			continue;
		}
		Clear(); Text(50, 30, title, 2);
		for (size_t i = top; i < lines.size() && i < top + visible; ++i) {
			const std::string &line = lines[i];
			bool fail = line.compare(0, 4, "FAIL") == 0, pass = line.compare(0, 4, "PASS") == 0;
			SDL_SetTextureColorMod(font, fail ? 255 : (pass ? 120 : 200), fail ? 90 : 240, fail ? 90 : (pass ? 140 : 255));
			Text(40, 100 + (int)(i - top) * 30, line.substr(0, 100).c_str());
		}
		SDL_SetTextureColorMod(font, 255, 255, 255);
		Text(40, 640, "Up/Down: scroll   Left/Right: page   X/Circle: back   (saved in USRDIR)");
		SDL_RenderPresent(renderer); SDL_Delay(16);
	}
}

// A game's settings file as lines (tabs as spaces). False if it couldn't be opened.
static bool ReadGameIni(const std::string &path, std::vector<std::string> &lines) {
	FILE *f = fopen(path.c_str(), "r");
	if (!f)
		return false;
	char buf[512];
	while (fgets(buf, sizeof(buf), f) && lines.size() < 1000) {
		std::string text = buf;
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
			text.pop_back();
		for (char &c : text)
			if (c == '\t') c = ' ';
		lines.push_back(text);
	}
	fclose(f);
	return true;
}

// Held buttons repeat after 0.35 s, every 0.07 s.
struct HeldRepeat {
	double next = 0.0;
	bool held = false;
	bool Pressed(bool down, double now) {
		if (!down) {
			held = false;
			return false;
		}
		if (!held) {
			held = true;
			next = now + 0.35;
			return true;
		}
		if (now >= next) {
			next = now + 0.07;
			return true;
		}
		return false;
	}
};

// A game's settings file to read, the whole of it: Up/Down move the selected line, Left/Right and
// L2/R2 a page. X edits from the selected line; returns its number, or -1 when closed.
static int ViewGameIni(MenuKeys &keys, const std::string &path) {
	std::vector<std::string> lines;
	if (!ReadGameIni(path, lines))
		lines.push_back("(could not read " + path + ")");
	if (lines.empty())
		lines.push_back("");
	const size_t slash = path.rfind('/');
	const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
	const int count = (int)lines.size();
	const int visible = (int)MenuScreens::ReportVisibleLines();
	int sel = 0, top = 0;
	HeldRepeat rUp, rDown, rLeft, rRight, rL2, rR2;
	while (PS3Frontend::Pump(false)) {
		keys.Update();
		const double now = time_now_d();
		if (keys.circle)
			return -1;
		if (keys.cross)
			return sel;
		if (rUp.Pressed(pad.BTN_UP, now) && sel > 0) sel--;
		if (rDown.Pressed(pad.BTN_DOWN, now) && sel + 1 < count) sel++;
		const bool pageUp = rLeft.Pressed(pad.BTN_LEFT, now) | rL2.Pressed(pad.BTN_L2, now);
		const bool pageDown = rRight.Pressed(pad.BTN_RIGHT, now) | rR2.Pressed(pad.BTN_R2, now);
		if (pageUp) sel = std::max(0, sel - visible);
		if (pageDown) sel = std::min(count - 1, sel + visible);
		if (sel < top) top = sel;
		if (sel >= top + visible) top = sel - visible + 1;
		char status[160];
		snprintf(status, sizeof(status), "%s   line %d of %d", file.c_str(), sel + 1, count);
		const char *hint = "Up/Down: line  Left/Right or L2/R2: page  X: edit this line  O: close";
		if (MenuCanvasReady()) {
			MenuScreens::Viewer(now, "Game config", "USRDIR/games", lines, (size_t)top, (size_t)visible, sel, status, hint);
			MenuCanvasPresent();
			SDL_Delay(8);
		} else {
			Clear();
			Text(40, 30, ("Game config: " + file).c_str());
			for (int i = 0; i < visible && top + i < count; ++i)
				Text(40, 80 + i * 30, ((top + i == sel ? "> " : "  ") + lines[top + i]).substr(0, 100).c_str());
			Text(40, 640, hint);
			SDL_RenderPresent(renderer);
			SDL_Delay(16);
		}
	}
	return -1;
}

// A game's settings file in USRDIR/games, edited as text with an on-screen keyboard. Creates the
// file on saving if there was none. The cursor starts at the end of line startLine (an existing
// file) or on the empty last line (a new one). False if nothing was saved.
static bool EditGameIni(MenuKeys &keys, const std::string &path, const std::string &gameTitle, int startLine = 0) {
	std::vector<std::string> lines;
	const bool existed = ReadGameIni(path, lines);
	if (!existed) {
		lines.push_back("# " + (gameTitle.empty() ? std::string("Settings for this game") : gameTitle));
		lines.push_back("# One setting per line, key=value (the keys are in README.txt in this folder)");
	}
	lines.push_back("");
	// Without a trailing empty line the cursor can't get past the last one.
	while (lines.size() > 1 && lines.back().empty() && lines[lines.size() - 2].empty())
		lines.pop_back();
	int curLine = (int)lines.size() - 1, curCol = 0, top = 0;
	if (existed) {
		curLine = std::max(0, std::min(startLine, (int)lines.size() - 1));
		curCol = (int)lines[curLine].size();
	}
	static const std::vector<std::vector<std::string>> lower = {
		{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "="},
		{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "#", "_"},
		{"a", "s", "d", "f", "g", "h", "j", "k", "l", ".", ",", ";"},
		{"z", "x", "c", "v", "b", "n", "m", "(", ")", "/", ":", "'"},
		{"Shift", "Space", "Delete", "New line"},
	};
	int keyRow = 1, keyCol = 0;
	bool shift = false, changed = false;
	const size_t slash = path.rfind('/');
	const std::string title = "Game config";
	const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
	std::string message;
	HeldRepeat rUp, rDown, rLeft, rRight, rL1, rR1, rL2, rR2, rL3, rR3, rSquare;
	auto pressed = [](HeldRepeat &r, bool held, double now) { return r.Pressed(held, now); };
	auto backspace = [&]() {
		if (curCol > 0) {
			lines[curLine].erase(curCol - 1, 1);
			curCol--;
		} else if (curLine > 0) {
			curCol = (int)lines[curLine - 1].size();
			lines[curLine - 1] += lines[curLine];
			lines.erase(lines.begin() + curLine);
			curLine--;
		} else {
			return;
		}
		changed = true;
	};
	auto type = [&](const std::string &text) {
		lines[curLine].insert(curCol, text);
		curCol += (int)text.size();
		changed = true;
	};
	auto save = [&]() -> bool {
		mkdir((std::string(base) + "games").c_str(), 0777);
		FILE *f = fopen(path.c_str(), "w");
		if (!f) {
			PS3Frontend::Trace("Game config: could not write %s", path.c_str());
			return false;
		}
		size_t n = lines.size();
		while (n > 0 && lines[n - 1].empty())
			n--;
		for (size_t i = 0; i < n; ++i)
			fprintf(f, "%s\n", lines[i].c_str());
		fclose(f);
		PS3Frontend::Trace("Game config saved: %s (%u lines)", path.c_str(), (unsigned)n);
		return true;
	};
	// The question on closing with changes (save, don't save, back to editing), or on START
	// (savingAsk: save, back to editing).
	bool asking = false, savingAsk = false;
	int askSel = 0;
	while (PS3Frontend::Pump(false)) {
		keys.Update();
		const double now = time_now_d();
		const bool up = pressed(rUp, pad.BTN_UP, now), down = pressed(rDown, pad.BTN_DOWN, now);
		const bool left = pressed(rLeft, pad.BTN_LEFT, now), right = pressed(rRight, pad.BTN_RIGHT, now);
		const bool l1 = pressed(rL1, pad.BTN_L1, now), r1 = pressed(rR1, pad.BTN_R1, now);
		const bool l2 = pressed(rL2, pad.BTN_L2, now), r2 = pressed(rR2, pad.BTN_R2, now);
		const bool square = pressed(rSquare, pad.BTN_SQUARE, now);
		const bool l3 = pressed(rL3, pad.BTN_L3, now), r3 = pressed(rR3, pad.BTN_R3, now);
		if (asking) {
			const int choices = savingAsk ? 2 : 3;
			if (up) askSel = (askSel + choices - 1) % choices;
			if (down) askSel = (askSel + 1) % choices;
			if (keys.circle) {
				asking = false;
			} else if (keys.cross) {
				asking = false;
				if (savingAsk) {
					if (askSel == 0) {
						if (save())
							return true;
						message = "Could not save " + file + ".";
					}
				} else if (askSel == 0) {
					if (save())
						return true;
					message = "Could not save " + file + ".";
				} else if (askSel == 1) {
					return false;
				}
			}
		} else {
			if (up) keyRow = (keyRow + (int)lower.size() - 1) % (int)lower.size();
			if (down) keyRow = (keyRow + 1) % (int)lower.size();
			keyCol = std::min(keyCol, (int)lower[keyRow].size() - 1);
			if (left) keyCol = (keyCol + (int)lower[keyRow].size() - 1) % (int)lower[keyRow].size();
			if (right) keyCol = (keyCol + 1) % (int)lower[keyRow].size();
			if (l1) {
				if (curCol > 0) curCol--;
				else if (curLine > 0) { curLine--; curCol = (int)lines[curLine].size(); }
			}
			if (r1) {
				if (curCol < (int)lines[curLine].size()) curCol++;
				else if (curLine + 1 < (int)lines.size()) { curLine++; curCol = 0; }
			}
			if (l2 && curLine > 0) curLine--;
			if (r2 && curLine + 1 < (int)lines.size()) curLine++;
			// A page of lines.
			if (l3) curLine = std::max(0, curLine - (int)MenuScreens::EDITOR_VISIBLE_LINES);
			if (r3) curLine = std::min((int)lines.size() - 1, curLine + (int)MenuScreens::EDITOR_VISIBLE_LINES);
			curCol = std::min(curCol, (int)lines[curLine].size());
			if (square) backspace();
			if (keys.triangle) type(" ");
			if (keys.select) shift = !shift;
			if (keys.cross) {
				const std::string &key = lower[keyRow][keyCol];
				if (key == "Shift") {
					shift = !shift;
				} else if (key == "Space") {
					type(" ");
				} else if (key == "Delete") {
					backspace();
				} else if (key == "New line") {
					lines.insert(lines.begin() + curLine + 1, lines[curLine].substr(curCol));
					lines[curLine].erase(curCol);
					curLine++;
					curCol = 0;
					changed = true;
				} else {
					std::string text = key;
					if (shift && text[0] >= 'a' && text[0] <= 'z')
						text[0] = (char)(text[0] - 'a' + 'A');
					type(text);
				}
			}
			if (keys.start) {
				if (!changed)
					return false;
				asking = true;
				savingAsk = true;
				askSel = 0;
			}
			if (keys.circle) {
				if (!changed)
					return false;
				asking = true;
				savingAsk = false;
				askSel = 0;
			}
		}
		if (curLine < top) top = curLine;
		if (curLine >= top + MenuScreens::EDITOR_VISIBLE_LINES) top = curLine - MenuScreens::EDITOR_VISIBLE_LINES + 1;
		std::vector<std::vector<std::string>> shown = lower;
		if (shift) {
			for (auto &row : shown)
				for (auto &k : row)
					if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z') k[0] = (char)(k[0] - 'a' + 'A');
			shown[4][0] = "SHIFT";
		}
		char status[128];
		snprintf(status, sizeof(status), "%s   line %d of %d, column %d%s%s", file.c_str(), curLine + 1, (int)lines.size(), curCol + 1,
			changed ? "   (changed)" : "", message.empty() ? "" : ("   " + message).c_str());
		const std::string hint = "X type  SQUARE del  TRIANGLE space  L1/R1 move  L2/R2 line  L3/R3 page  START save";
		if (MenuCanvasReady()) {
			MenuScreens::Editor(now, title.c_str(), "USRDIR/games", lines, top, curLine, curCol, shown, keyRow, keyCol, status,
				hint + "  O close");
			if (asking && savingAsk)
				MenuScreens::Popup("Save the config?", "The current config (" + file + ") will be saved. Do you want to proceed?", {"Yes, save", "No, back to editing"}, askSel, "Up/Down: select   X: OK   Circle: back");
			else if (asking)
				MenuScreens::Popup("Save the changes?", file, {"Save", "Don't save", "Back to editing"}, askSel, "Up/Down: select   X: OK   Circle: back");
			MenuCanvasPresent();
			SDL_Delay(8);
		} else {
			Clear();
			Text(40, 30, (title + ": " + file).c_str());
			for (int i = 0; i < MenuScreens::EDITOR_VISIBLE_LINES && top + i < (int)lines.size(); ++i)
				Text(40, 80 + i * 30, ((top + i == curLine ? "> " : "  ") + lines[top + i]).substr(0, 100).c_str());
			Text(40, 400, ("Key: " + shown[keyRow][keyCol]).c_str());
			Text(40, 640, asking ? (savingAsk ? "Save the current config? X: OK   Circle: back" : "Save the changes? X: OK   Circle: back") : hint.c_str());
			SDL_RenderPresent(renderer);
			SDL_Delay(16);
		}
	}
	return false;
}

// RSX tests live in the separate RSX Diagnostics app; the emulator keeps only the JIT check.
static void DiagnosticsMenu(MenuKeys &keys) {
	PS3Frontend::Status("Checking whether the JIT can run (PS3MAPI)...");
	ShowReport(keys, "JIT check (PS3MAPI)", MapiJitProbe());
}

// The main screen: tabs along the top, the game icons (or a settings page) below.
enum MenuTab { TAB_GAMES, TAB_SETTINGS, TAB_ADVANCED, TAB_DEBUG, TAB_COUNT };
// No Exit tab: the PS button menu quits.
static const char *const kTabNames[TAB_COUNT] = {"Games", "Basic options", "Advanced", "Debug"};
static const char *const kTabHelp[TAB_COUNT] = {
	"Your games (ISO, CSO, PBP) in /dev_hdd0/PSPISO. Press Down or X to choose one.",
	"Screen, speed, sound, controls and system options. Press Down or X to change them.",
	"Switches for testing: most go back to how an older build did something. Leave them at their defaults unless a game needs one.",
	"What goes into the logs (USRDIR/logs), for finding bugs. Everything here is off by default; most of it slows games down.",
};

struct GameEntry {
	std::string path;
	std::string name;   // File name without the extension.
	std::string kind;   // "ISO", "CSO"... shown while there's no icon.
	std::string title;  // From PARAM.SFO (ASCII only), or empty.
	std::vector<uint32_t> icon;
	bool read = false;  // Title and icon read (the icon may be missing, or dropped again to save memory).
	uint64_t size = 0, mtime = 0;
};

static const char *const kSortNames[4] = {"Name A-Z", "Name Z-A", "Newest first", "Largest first"};

static void SortGames(std::vector<GameEntry> &games) {
	auto nameLess = [](const GameEntry &a, const GameEntry &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; };
	switch (settings.gameSort) {
	case 1: std::stable_sort(games.begin(), games.end(), [&](const GameEntry &a, const GameEntry &b) { return nameLess(b, a); }); break;
	case 2: std::stable_sort(games.begin(), games.end(), [&](const GameEntry &a, const GameEntry &b) { return a.mtime != b.mtime ? a.mtime > b.mtime : nameLess(a, b); }); break;
	case 3: std::stable_sort(games.begin(), games.end(), [&](const GameEntry &a, const GameEntry &b) { return a.size != b.size ? a.size > b.size : nameLess(a, b); }); break;
	default: std::stable_sort(games.begin(), games.end(), nameLess); break;
	}
}

static std::string LowerName(const std::string &name) {
	std::string out = name;
	for (char &c : out)
		c = (char)tolower((unsigned char)c);
	return out;
}

// USRDIR/games/<name>.ini (any letter case), or empty.
static std::string FindGameIni(const GameEntry &game) {
	const std::string dir = std::string(base) + "games";
	std::string found;
	if (DIR *d = opendir(dir.c_str())) {
		while (struct dirent *e = readdir(d)) {
			if (strcasecmp(e->d_name, (game.name + ".ini").c_str()) == 0) {
				found = dir + "/" + e->d_name;
				break;
			}
		}
		closedir(d);
	}
	return found;
}

// "Save for a game": the settings of the Basic options and Advanced tabs as the game's own config,
// replacing the file it had. settings.ini's lines without the Debug tab, the ini-only switches and
// the game list order.
static bool SaveSettingsForGame(const GameEntry &game, std::string *path) {
	static const char *const kNotSaved[] = {"log", "geprofile", "jitsample", "debugbuttons", "quitcombo", "geasync", "cpurows",
		"gamesort", "perflog", "stalllog", "videolog", "savelog", "gewatchlog", "rsxlog", "jitlog", "l2r2", "nanwatch",
		"storewatch", "jitgeneric", "dumpram", "dumpvram", "snddebug"};
	const std::string existing = FindGameIni(game);
	*path = existing.empty() ? std::string(base) + "games/" + game.name + ".ini" : existing;
	mkdir((std::string(base) + "games").c_str(), 0777);
	std::string out = "# " + (game.title.empty() ? game.name : game.title) + ": Basic options and Advanced settings, saved with build " APP_BUILD "\n";
	const std::string text = SettingsText(settings);
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		const std::string key = line.substr(0, line.find('='));
		bool saved = !key.empty();
		for (const char *k : kNotSaved)
			if (key == k)
				saved = false;
		if (saved)
			out += line + "\n";
	}
	FILE *f = fopen(path->c_str(), "w");
	if (!f) {
		PS3Frontend::Trace("Could not write %s", path->c_str());
		return false;
	}
	fputs(out.c_str(), f);
	fclose(f);
	PS3Frontend::Trace("Settings saved as the game config %s", path->c_str());
	return true;
}

static std::vector<GameEntry> ListGames() {
	std::vector<GameEntry> games;
	for (const auto &file : ScanGames()) {
		GameEntry game;
		game.path = file.fullName.ToString();
		game.size = file.size;
		game.mtime = file.mtime;
		const size_t dot = file.name.rfind('.');
		const bool hasExt = dot != std::string::npos && dot > 0 && file.name.size() - dot <= 5;
		game.name = hasExt ? file.name.substr(0, dot) : file.name;
		if (hasExt) {
			for (size_t i = dot + 1; i < file.name.size(); ++i)
				game.kind += (char)toupper((unsigned char)file.name[i]);
		}
		games.push_back(std::move(game));
	}
	SortGames(games);
	return games;
}

// The canvas font only has ASCII: titles keep it and drop the rest (like (R) and TM signs); a
// mostly non-ASCII title (Japanese) isn't used.
static std::string AsciiTitle(const std::string &title) {
	std::string out;
	size_t other = 0;
	for (unsigned char c : title) {
		if (c >= 32 && c < 127)
			out += (char)c;
		else if (c == '\n' || c == '\r' || c == '\t')
			out += ' ';
		else if (c < 0x80 || c >= 0xC0)
			other++;  // Counts each UTF-8 character once (not its continuation bytes).
	}
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	while (!out.empty() && out[0] == ' ')
		out.erase(0, 1);
	if (out.size() < 3 || other * 4 > out.size())
		return std::string();
	return out;
}

// Reads one icon a frame: the rows on screen, then one row below and above. Icons far away are
// dropped again (about 100 KB each).
static void LoadNextIcon(std::vector<GameEntry> &games, int topRow) {
	const int cols = MenuScreens::GRID_COLUMNS, rows = MenuScreens::GRID_ROWS;
	const int count = (int)games.size();
	const int first = std::min(count, topRow * cols), last = std::min(count, (topRow + rows) * cols);
	const int nearFirst = std::max(0, first - cols), nearLast = std::min(count, last + cols);
	int pick = -1;
	for (int i = first; i < last && pick < 0; ++i)
		if (!games[i].read) pick = i;
	for (int i = last; i < nearLast && pick < 0; ++i)
		if (!games[i].read) pick = i;
	for (int i = nearFirst; i < first && pick < 0; ++i)
		if (!games[i].read) pick = i;
	if (pick < 0)
		return;
	const int MAX_ICONS = 45;
	int loaded = 0;
	for (const auto &game : games)
		if (!game.icon.empty()) loaded++;
	while (loaded >= MAX_ICONS) {
		int farthest = -1, distance = 0;
		for (int i = 0; i < count; ++i) {
			if (games[i].icon.empty() || (i >= nearFirst && i < nearLast))
				continue;
			const int d = i < nearFirst ? nearFirst - i : i - nearLast;
			if (d > distance) { farthest = i; distance = d; }
		}
		if (farthest < 0)
			break;
		std::vector<uint32_t>().swap(games[farthest].icon);
		games[farthest].read = false;
		loaded--;
	}
	GameEntry &game = games[pick];
	const double start = time_now_d();
	GameIcons::Info info;
	const bool ok = GameIcons::Load(game.path, MenuScreens::ICON_W, MenuScreens::ICON_H, &info);
	game.read = true;
	game.title = AsciiTitle(info.title);
	game.icon = std::move(info.icon);
	PS3Frontend::Trace("Game icon: %s: %s, title \"%s\" (%.0f ms)", game.name.c_str(),
		!ok ? "not readable" : (game.icon.empty() ? "no icon" : "icon"), info.title.c_str(), (time_now_d() - start) * 1000.0);
}

static std::string ChooseGame() {
	std::vector<GameEntry> games = ListGames();
	const int cols = MenuScreens::GRID_COLUMNS, rows = MenuScreens::GRID_ROWS;
	int selection = 0, topRow = 0;
	int page = TAB_GAMES, highlight = TAB_GAMES;
	bool onTabs = false;
	// On the games page the cursor is on a game, or on the buttons above them (Refresh, Sort).
	enum { BUTTON_REFRESH, BUTTON_SORT, BUTTON_COUNT };
	bool onButtons = games.empty();
	int button = BUTTON_REFRESH;
	// Triangle on a game: its options, then the delete question or a message.
	// "Save for a game" on an options tab: the game to save for, then the question.
	enum { POPUP_NONE, POPUP_GAME, POPUP_DELETE, POPUP_DELETE_INI, POPUP_MESSAGE, POPUP_PICK, POPUP_SAVE_GAME };
	enum { ACT_START, ACT_VIEW_INI, ACT_INI, ACT_DELETE_INI, ACT_DELETE };
	int popup = POPUP_NONE, popupSel = 0;
	int pickSel = 0;        // POPUP_PICK / POPUP_SAVE_GAME: the game (index into games).
	bool pickForLoad = false;          // POPUP_PICK for "Load from a game" (else Save for a game).
	std::set<std::string> pickConfigs;  // Lower-case names of the games/*.ini files (for the picker).
	int popupShownSel = 0;  // The selected item as drawn (the picker shows a window of the games).
	constexpr int PICK_VISIBLE = 9;
	std::vector<int> popupActions;
	std::string popupIni, popupMessage;
	MenuKeys keys; keys.last = pad;
	auto resort = [&]() {
		const std::string selected = selection < (int)games.size() ? games[selection].path : std::string();
		SortGames(games);
		for (int i = 0; i < (int)games.size(); ++i)
			if (games[i].path == selected) selection = i;
		settingsChanged = true;
	};
	auto popupContent = [&](std::string &title, std::string &text, std::vector<std::string> &items, std::string &hint) {
		popupShownSel = popup == POPUP_MESSAGE ? 0 : popupSel;
		if (popup == POPUP_PICK) {
			const int count = (int)games.size();
			const int top = std::max(0, std::min(pickSel - PICK_VISIBLE / 2, count - PICK_VISIBLE));
			title = std::string(pickForLoad ? "Load the config of which game? (" : "Save the settings for which game? (") + std::to_string(pickSel + 1) + "/" + std::to_string(count) + ")";
			text = pickForLoad ? "Its config replaces the Basic options and Advanced settings shown now. Games marked [config] have one." :
				"The Basic options and Advanced settings shown now become that game's config.";
			for (int i = top; i < count && i < top + PICK_VISIBLE; ++i) {
				std::string item = games[i].title.empty() ? games[i].name : games[i].title;
				if (pickForLoad && pickConfigs.count(LowerName(games[i].name + ".ini")))
					item += "  [config]";
				items.push_back(item);
			}
			popupShownSel = pickSel - top;
			hint = "Up/Down: select  L2/R2: page  X: OK  Circle: cancel";
			return;
		}
		if (popup == POPUP_SAVE_GAME) {
			const GameEntry &game = games[pickSel];
			const std::string ini = FindGameIni(game);
			title = "Save the settings for " + (game.title.empty() ? game.name : game.title) + "?";
			text = ini.empty() ? "As games/" + game.name + ".ini (new). The game then starts with these settings." :
				ini.substr(ini.rfind('/') + 1) + " is replaced. The game then starts with these settings.";
			items = {"Cancel", "Save"};
			hint = "Up/Down: select   X: OK   Circle: cancel";
			return;
		}
		if (popup == POPUP_MESSAGE && games.empty()) {
			title = pickForLoad ? "Load from a game" : "Save for a game";
			text = popupMessage;
			items = {"OK"};
			hint = "X: OK";
			return;
		}
		const GameEntry &game = games[selection];
		const size_t slash = game.path.rfind('/');
		const std::string file = slash == std::string::npos ? game.path : game.path.substr(slash + 1);
		title = game.title.empty() ? game.name : game.title;
		if (popup == POPUP_GAME) {
			text = file;
			for (int action : popupActions) {
				if (action == ACT_START) items.push_back("Start game");
				else if (action == ACT_VIEW_INI) items.push_back("View/edit game config");
				else if (action == ACT_INI) items.push_back("New game config");
				else if (action == ACT_DELETE_INI) items.push_back("Delete game config");
				else if (action == ACT_DELETE) items.push_back("Delete game");
			}
			hint = "Up/Down: select   X: OK   Circle: close";
		} else if (popup == POPUP_DELETE) {
			title = "Delete this game?";
			text = file + " will be removed from your PS3. Its saves and game settings are kept.";
			items = {"Cancel", "Delete"};
			hint = "Up/Down: select   X: OK   Circle: cancel";
		} else if (popup == POPUP_DELETE_INI) {
			title = "Delete this game's config?";
			text = popupIni.substr(popupIni.rfind('/') + 1) + " will be deleted: the game then uses your main settings.";
			items = {"Cancel", "Delete"};
			hint = "Up/Down: select   X: OK   Circle: cancel";
		} else {
			text = popupMessage;
			items = {"OK"};
			hint = "X: OK";
		}
	};
	auto rescan = [&]() {
		games = ListGames();
		selection = 0;
		topRow = 0;
	};
	auto openPage = [&](int tab) {
		page = tab;
		highlight = tab;
		onTabs = false;
		if (tab == TAB_GAMES)
			onButtons = games.empty();
	};
	auto settingsPageFor = [&](int tab) -> SettingsPage & {
		return tab == TAB_SETTINGS ? generalPage : (tab == TAB_ADVANCED ? advancedPage : debugPage);
	};
	while (PS3Frontend::Pump(false)) {
		keys.Update();
		if (popup == POPUP_PICK) {
			const int count = (int)games.size();
			if (keys.up) pickSel = (pickSel + count - 1) % count;
			if (keys.down) pickSel = (pickSel + 1) % count;
			if (keys.l2) pickSel = std::max(0, pickSel - PICK_VISIBLE);
			if (keys.r2) pickSel = std::min(count - 1, pickSel + PICK_VISIBLE);
			if (keys.circle) {
				popup = POPUP_NONE;
			} else if (keys.cross && pickForLoad) {
				// Load from a game: the config straight into the tabs (they are saved as settings.ini
				// when the menu leaves the options tabs, as after any change).
				const GameEntry &game = games[pickSel];
				const std::string ini = FindGameIni(game);
				const std::string name = game.title.empty() ? game.name : game.title;
				popup = POPUP_MESSAGE;
				popupSel = 0;
				if (ini.empty()) {
					popupMessage = name + " has no config yet (games/" + game.name + ".ini). Save for a game makes one.";
				} else if (LoadSettingsFrom(ini)) {
					settingsChanged = true;
					resetDone = false;
					PS3Frontend::Trace("Game config %s loaded into the options tabs", ini.c_str());
					popupMessage = "Loaded games/" + ini.substr(ini.rfind('/') + 1) + " into the Basic options and Advanced tabs. Change what you want, then use Save for a game to write it back.";
				} else {
					popupMessage = "Could not read " + ini + ".";
				}
				selection = pickSel;  // The message is about this game.
			} else if (keys.cross) {
				popup = POPUP_SAVE_GAME;
				popupSel = 0;
			}
		} else if (popup != POPUP_NONE) {
			const int items = popup == POPUP_GAME ? (int)popupActions.size() : (popup == POPUP_DELETE || popup == POPUP_DELETE_INI || popup == POPUP_SAVE_GAME ? 2 : 1);
			if (keys.up) popupSel = (popupSel + items - 1) % items;
			if (keys.down) popupSel = (popupSel + 1) % items;
			if (keys.circle) {
				popup = POPUP_NONE;
			} else if (keys.cross && popup == POPUP_SAVE_GAME) {
				popup = POPUP_NONE;
				if (popupSel == 1) {
					const GameEntry &game = games[pickSel];
					std::string path;
					popup = POPUP_MESSAGE;
					popupSel = 0;
					if (SaveSettingsForGame(game, &path))
						popupMessage = "Saved as games/" + path.substr(path.rfind('/') + 1) + ". Triangle on the game: View/edit or Delete game config.";
					else
						popupMessage = "Could not write " + path + ".";
					selection = pickSel;  // The message is about this game.
				}
			} else if (keys.cross && popup == POPUP_GAME) {
				const int action = popupActions[popupSel];
				popup = POPUP_NONE;
				if (action == ACT_START) {
					SaveChangedSettings();
					return games[selection].path;
				} else if (action == ACT_VIEW_INI) {
					const GameEntry &game = games[selection];
					const int line = ViewGameIni(keys, popupIni);
					if (line >= 0)
						EditGameIni(keys, popupIni, game.title.empty() ? game.name : game.title, line);
					continue;
				} else if (action == ACT_INI) {
					const GameEntry &game = games[selection];
					const std::string path = popupIni.empty() ? std::string(base) + "games/" + game.name + ".ini" : popupIni;
					EditGameIni(keys, path, game.title.empty() ? game.name : game.title);
					continue;
				} else if (action == ACT_DELETE_INI) {
					popup = POPUP_DELETE_INI;
					popupSel = 0;
				} else if (action == ACT_DELETE) {
					popup = POPUP_DELETE;
					popupSel = 0;
				}
			} else if (keys.cross && popup == POPUP_DELETE_INI) {
				popup = POPUP_NONE;
				if (popupSel == 1) {
					if (remove(popupIni.c_str()) == 0) {
						PS3Frontend::Trace("Game config deleted: %s", popupIni.c_str());
					} else {
						popup = POPUP_MESSAGE;
						popupSel = 0;
						popupMessage = "Could not delete " + popupIni + ".";
					}
				}
			} else if (keys.cross && popup == POPUP_DELETE) {
				popup = POPUP_NONE;
				if (popupSel == 1) {
					const std::string path = games[selection].path;
					if (File::Delete(Path(path))) {
						PS3Frontend::Trace("Game deleted: %s", path.c_str());
						games.erase(games.begin() + selection);
						selection = std::max(0, std::min(selection, (int)games.size() - 1));
						if (games.empty())
							onButtons = true;
					} else {
						PS3Frontend::Trace("Game delete failed: %s", path.c_str());
						popup = POPUP_MESSAGE;
						popupSel = 0;
						popupMessage = "Could not delete " + path + ".";
					}
				}
			} else if (keys.cross) {
				popup = POPUP_NONE;
			}
		} else if (keys.l1 || keys.r1) {
			// L1/R1 switch tabs from anywhere, straight into the page.
			openPage((page + TAB_COUNT + (keys.r1 ? 1 : -1)) % TAB_COUNT);
		} else if (onTabs) {
			if (keys.left || keys.right) {
				highlight = (highlight + TAB_COUNT + (keys.right ? 1 : -1)) % TAB_COUNT;
				page = highlight;
			} else if (keys.cross || keys.down || keys.circle) {
				openPage(highlight);
			}
		} else if (page == TAB_GAMES && onButtons) {
			if (keys.left) button = (button + BUTTON_COUNT - 1) % BUTTON_COUNT;
			if (keys.right) button = (button + 1) % BUTTON_COUNT;
			if (keys.up || keys.circle) {
				onTabs = true;
				highlight = TAB_GAMES;
			} else if (keys.down && !games.empty()) {
				onButtons = false;
			} else if (keys.cross && button == BUTTON_REFRESH) {
				rescan();
				onButtons = games.empty();
			} else if (keys.cross && button == BUTTON_SORT) {
				settings.gameSort = (settings.gameSort + 1) % 4;
				resort();
			}
		} else if (page == TAB_GAMES) {
			const int count = (int)games.size();
			if (keys.cross && count > 0) {
				SaveChangedSettings();
				return games[selection].path;
			} else if (keys.circle) {
				onTabs = true;
				highlight = TAB_GAMES;
			} else if (keys.triangle && count > 0) {
				popup = POPUP_GAME;
				popupSel = 0;
				popupIni = FindGameIni(games[selection]);
				popupActions = {ACT_START};
				// Build 269: a config is viewed and edited from one entry (View/edit: X on a line edits
				// it); "New game config" only when the game has none.
				if (!popupIni.empty()) {
					popupActions.push_back(ACT_VIEW_INI);
					popupActions.push_back(ACT_DELETE_INI);
				} else {
					popupActions.push_back(ACT_INI);
				}
				popupActions.push_back(ACT_DELETE);
			} else if (keys.square) {
				settings.gameSort = (settings.gameSort + 1) % 4;
				resort();
			} else if (keys.select) {
				DiagnosticsMenu(keys);
				continue;
			} else if ((keys.up && selection < cols) || count == 0) {
				onButtons = true;
			} else {
				if (keys.left && selection > 0) selection--;
				if (keys.right && selection + 1 < count) selection++;
				if (keys.up) selection -= cols;
				if (keys.down) {
					if (selection + cols < count) selection += cols;
					else if (selection / cols < (count - 1) / cols) selection = count - 1;
				}
				if (keys.r2) selection = std::min(selection + cols * rows, count - 1);
				if (keys.l2) selection = std::max(selection - cols * rows, 0);
			}
		} else {
			SettingsPage &settingsPage = settingsPageFor(page);
			if (SettingsPageInput(keys, settingsPage)) {
				onTabs = true;
				highlight = page;
			}
			if (saveForGameRequested || loadFromGameRequested) {
				pickForLoad = loadFromGameRequested;
				saveForGameRequested = false;
				loadFromGameRequested = false;
				SaveChangedSettings();
				pickConfigs.clear();
				if (pickForLoad) {
					if (DIR *d = opendir((std::string(base) + "games").c_str())) {
						while (struct dirent *e = readdir(d))
							pickConfigs.insert(LowerName(e->d_name));
						closedir(d);
					}
				}
				popupSel = 0;
				if (games.empty()) {
					popup = POPUP_MESSAGE;
					popupMessage = "No games in /dev_hdd0/PSPISO.";
				} else {
					popup = POPUP_PICK;
					pickSel = std::max(0, std::min(selection, (int)games.size() - 1));
				}
			}
		}
		if (page == TAB_GAMES)
			SaveChangedSettings();

		const MenuScreens::Tabs tabs = {kTabNames, TAB_COUNT, page, onTabs ? highlight : -1};
		const char *tabHelp = kTabHelp[highlight];
		auto drawPopup = [&]() {
			std::string title, text, hint;
			std::vector<std::string> items;
			popupContent(title, text, items, hint);
			MenuScreens::Popup(title, text, items, popupShownSel, hint);
		};
		if (page != TAB_GAMES) {
			if (!MenuCanvasReady() && popup != POPUP_NONE) {
				std::string title, text, hint;
				std::vector<std::string> items;
				popupContent(title, text, items, hint);
				PlainMenu(title.c_str(), items, popupShownSel, hint.c_str());
				continue;
			}
			if (DrawSettingsPage(tabs, settingsPageFor(page), !onTabs, tabHelp)) {
				if (popup != POPUP_NONE)
					drawPopup();
				MenuCanvasPresent();
				SDL_Delay(8);
			}
			continue;
		}
		const int count = (int)games.size();
		const int totalRows = (count + cols - 1) / cols;
		const int selRow = selection / cols;
		if (selRow < topRow) topRow = selRow;
		if (selRow >= topRow + rows) topRow = selRow - rows + 1;
		topRow = std::max(0, std::min(topRow, totalRows - rows));
		LoadNextIcon(games, topRow);
		if (!MenuCanvasReady() && popup != POPUP_NONE && count > 0) {
			std::string title, text, hint;
			std::vector<std::string> items;
			popupContent(title, text, items, hint);
			PlainMenu(title.c_str(), items, popupShownSel, hint.c_str());
			continue;
		}
		if (!MenuCanvasReady()) {
			std::vector<std::string> lines;
			for (const auto &game : games)
				lines.push_back(game.name);
			PlainMenu(onTabs ? kTabNames[highlight] : "Games", lines, onTabs || onButtons ? -1 : selection,
				onTabs ? "Left/Right: tabs   X: open" : onButtons ? "X: Refresh   Down: games   Up: tabs" : "X: start   Triangle: options   Square: sort   SELECT: JIT check   Up: buttons");
			continue;
		}
		std::vector<MenuScreens::GameTile> tiles(count);
		for (int i = 0; i < count; ++i) {
			tiles[i].title = games[i].title.empty() ? games[i].name : games[i].title;
			tiles[i].icon = games[i].icon.empty() ? nullptr : games[i].icon.data();
			tiles[i].kind = games[i].kind;
			tiles[i].loading = !games[i].read;
		}
		std::string status, hint;
		if (count > 0) {
			const GameEntry &game = games[selection];
			status = (game.title.empty() ? game.name : game.title) + "   " + std::to_string(selection + 1) + "/" + std::to_string(count);
		} else {
			status = "no games";
		}
		if (onTabs)
			hint = "Left/Right or L1/R1: tabs   X/Down: open";
		else if (onButtons)
			hint = button == BUTTON_REFRESH ? "X: look for new games   Left/Right: buttons   Down: games   L1/R1: tabs" :
				"X: change the order   Left/Right: buttons   Down: games   L1/R1: tabs";
		else
			hint = "X: start  Triangle: options  Square: sort  SELECT: JIT check  L2/R2: page  L1/R1: tabs";
		const std::vector<std::string> buttons = {"Refresh", std::string("Sort: ") + kSortNames[settings.gameSort]};
		MenuScreens::Games(time_now_d(), tabs, FRONTEND_VERSION_BAR, buttons, !onTabs && onButtons ? button : -1, status, tiles,
			onTabs || onButtons ? -1 : selection, topRow, hint, "/dev_hdd0/PSPISO");
		if (popup != POPUP_NONE && count > 0)
			drawPopup();
		MenuCanvasPresent();
		SDL_Delay(8);
	}
	SaveChangedSettings();
	return {};
}

// "Quit to game list" (build 219): sysProcessExitSpawn2 can't start the NPDRM EBOOT.BIN (in build
// 218 it returned and the app ended to the XMB), but it starts a plain fake-signed SELF. The two
// differ only in the header (checked when the package is built), so RELOAD.SELF is RELOAD.HDR
// (the plain header) followed by EBOOT.BIN after its own header. Written once per build
// (RELOAD.BLD holds the build number it was made for). Since build 265 the HEN package brings
// RELOAD.SELF (retail-signed, so that it also runs with HEN) and RELOAD.BLD for its build, and
// nothing is written here; the rest is for older packages' files.
static uint64_t ReadBE64(const uint8_t *p) {
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i)
		v = (v << 8) | p[i];
	return v;
}

static bool PrepareReloadSelf() {
	const std::string dir = base;
	{
		char stamp[16] = {};
		if (FILE *f = fopen((dir + "RELOAD.BLD").c_str(), "rb")) {
			fread(stamp, 1, sizeof(stamp) - 1, f);
			fclose(f);
			FILE *self = fopen((dir + "RELOAD.SELF").c_str(), "rb");
			if (self) fclose(self);
			if (self && strcmp(stamp, APP_BUILD) == 0)
				return true;
		}
	}
	const double start = time_now_d();
	std::vector<uint8_t> hdr;
	if (FILE *f = fopen((dir + "RELOAD.HDR").c_str(), "rb")) {
		fclose(f);
	} else if (FILE *self = fopen((dir + "RELOAD.SELF").c_str(), "rb")) {
		// From the package (RELOAD.BLD missing or another build's): use it as it is.
		fclose(self);
		PS3Frontend::Trace("RELOAD.SELF: from the package (RELOAD.BLD missing or not %s)", APP_BUILD);
		return true;
	}
	if (FILE *f = fopen((dir + "RELOAD.HDR").c_str(), "rb")) {
		uint8_t buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			hdr.insert(hdr.end(), buf, buf + n);
		fclose(f);
	}
	FILE *in = fopen((dir + "EBOOT.BIN").c_str(), "rb");
	uint8_t ebootHead[32] = {};
	if (hdr.size() < 32 || !in || fread(ebootHead, 1, 32, in) != 32) {
		PS3Frontend::Trace("RELOAD.SELF: RELOAD.HDR (%u bytes) or EBOOT.BIN missing", (unsigned)hdr.size());
		if (in) fclose(in);
		return false;
	}
	fseek(in, 0, SEEK_END);
	const uint64_t ebootSize = (uint64_t)ftell(in);
	const uint64_t ebootHdrLen = ReadBE64(ebootHead + 16), bodyLen = ReadBE64(hdr.data() + 24);
	if (memcmp(ebootHead, "SCE\0", 4) != 0 || memcmp(hdr.data(), "SCE\0", 4) != 0 || ReadBE64(hdr.data() + 16) != hdr.size() ||
		ebootHdrLen >= ebootSize || ebootSize - ebootHdrLen != bodyLen) {
		PS3Frontend::Trace("RELOAD.SELF: RELOAD.HDR doesn't match EBOOT.BIN (EBOOT %llu bytes, header %llu, body %llu)",
			(unsigned long long)ebootSize, (unsigned long long)ebootHdrLen, (unsigned long long)bodyLen);
		fclose(in);
		return false;
	}
	FILE *out = fopen((dir + "RELOAD.SELF").c_str(), "wb");
	bool ok = out && fwrite(hdr.data(), 1, hdr.size(), out) == hdr.size();
	fseek(in, (long)ebootHdrLen, SEEK_SET);
	const size_t chunk = 1024 * 1024;
	uint8_t *buf = ok ? (uint8_t *)malloc(chunk) : nullptr;
	uint64_t copied = 0;
	while (ok && buf) {
		const size_t n = fread(buf, 1, chunk, in);
		if (n == 0)
			break;
		ok = fwrite(buf, 1, n, out) == n;
		copied += n;
	}
	free(buf);
	fclose(in);
	if (out && fclose(out) != 0)
		ok = false;
	ok = ok && copied == bodyLen;
	if (ok) {
		if (FILE *f = fopen((dir + "RELOAD.BLD").c_str(), "wb")) {
			fputs(APP_BUILD, f);
			fclose(f);
		}
	}
	PS3Frontend::Trace("RELOAD.SELF: %s (%llu bytes, %.1f s)", ok ? "written" : "writing failed", (unsigned long long)(hdr.size() + copied), time_now_d() - start);
	return ok;
}

int main(int, char **) {
	PS3Frontend::Trace("Main entered: full PPSSPP Core frontend");
	g_assertLogHook = [](const char *text) { PS3Frontend::Trace("ASSERT failed: %s", text); };
	g_frontendTraceHook = [](const char *text) { PS3Frontend::Trace("%s", text); };
	g_mediaMemoryHook = MediaMemoryHook;
	if (chdir(base) != 0) { SetTraceEnabled(true); PS3Frontend::Trace("Cannot access installed USRDIR"); return 16; }
	LoadSettings();
	ApplyLogMask();
	SetTraceEnabled(settings.debugButtons);
	OpenLogFiles();
	SDL_SetMainReady();
	if (SDL_Init(SDL_INIT_VIDEO) != 0) { PS3Frontend::Trace("SDL_Init failed: %s", SDL_GetError()); return 10; }
	PS3Frontend::Trace("SDL video initialized: %s", SDL_GetCurrentVideoDriver());
	// The window covers the whole TV picture (the video mode the PS3 is in: 1080p draws a 1280x720
	// window into the top-left two thirds). The menus are laid out for 1280x720 and scaled up to it.
	int windowW = 1280, windowH = 720;
	{
		SDL_DisplayMode displayMode{};
		if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0 && displayMode.w >= 640 && displayMode.h >= 480) {
			windowW = displayMode.w;
			windowH = displayMode.h;
		}
	}
	window = SDL_CreateWindow("PPSSPP PS3", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, windowW, windowH, SDL_WINDOW_SHOWN);
	PS3Frontend::Trace("Window: %dx%d (menus drawn at 1280x720, scaled)", windowW, windowH);
	if (!window) { PS3Frontend::Trace("Window failed: %s", SDL_GetError()); return 11; }
	PS3Frontend::Trace("Window created; creating renderer");
	renderer = SDL_CreateRenderer(window, -1, 0);
	if (!renderer) { PS3Frontend::Trace("Renderer failed: %s", SDL_GetError()); return 12; }
	SDL_RendererInfo rendererInfo{};
	if (SDL_GetRendererInfo(renderer, &rendererInfo) == 0)
		PS3Frontend::Trace("Renderer: %s", rendererInfo.name ? rendererInfo.name : "?");
	PS3Frontend::Trace("Renderer created; loading font");
	SDL_RenderSetLogicalSize(renderer, 1280, 720);
	SDL_Surface *surface = SDL_LoadBMP((std::string(base) + "font.bmp").c_str());
	if (!surface) { PS3Frontend::Trace("Font load failed: %s", SDL_GetError()); return 13; }
	// CPU copy of the glyphs for the RSX renderer's overlay (it can't use SDL in game).
	if (surface->w >= 192 && surface->h >= 144 && SDL_LockSurface(surface) == 0) {
		for (int y = 0; y < 144; ++y) {
			for (int x = 0; x < 192; ++x) {
				const uint8_t *px = (const uint8_t *)surface->pixels + y * surface->pitch + x * surface->format->BytesPerPixel;
				uint32_t value = 0;
				memcpy(&value, px, std::min<int>(surface->format->BytesPerPixel, 4));
				uint8_t r, g, b;
				SDL_GetRGB(surface->format->BytesPerPixel == 4 ? value : (value >> (8 * (4 - surface->format->BytesPerPixel))), surface->format, &r, &g, &b);
				fontMask[y * 192 + x] = (r + g + b) > 384 ? 1 : 0;
				fontSmall[y * 192 + x] = (uint8_t)((r + g + b) / 3);
			}
		}
		SDL_UnlockSurface(surface);
		RSXRendererSetFont(fontMask);
	}
	if (SDL_Surface *large = SDL_LoadBMP((std::string(base) + "font_large.bmp").c_str())) {
		if (large->w >= 384 && large->h >= 288 && SDL_LockSurface(large) == 0) {
			const int bpp = large->format->BytesPerPixel;
			for (int y = 0; y < 288; ++y) {
				for (int x = 0; x < 384; ++x) {
					const uint8_t *px = (const uint8_t *)large->pixels + y * large->pitch + x * bpp;
					uint32_t value = 0;
					memcpy(&value, px, std::min<int>(bpp, 4));
					uint8_t r, g, b;
					SDL_GetRGB(bpp == 4 ? value : (value >> (8 * (4 - bpp))), large->format, &r, &g, &b);
					fontLarge[y * 384 + x] = (uint8_t)((r + g + b) / 3);
				}
			}
			SDL_UnlockSurface(large);
			fontLargeLoaded = true;
		}
		SDL_FreeSurface(large);
	}
	SDL_SetColorKey(surface, SDL_TRUE, SDL_MapRGB(surface->format, 0, 0, 0));
	font = SDL_CreateTextureFromSurface(renderer, surface); SDL_FreeSurface(surface);
	RSX::InitDiagnostics();
	PS3Frontend::Trace("Font texture created; creating game texture");
	screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 480, 272);
	if (!font || !screen) { PS3Frontend::Trace("Texture failed: %s", SDL_GetError()); return 14; }
	if (sysUtilRegisterCallback(1, SystemCallback, nullptr) != 0 || ioPadInit(7) != 0) { PS3Frontend::Trace("Native pad/callback setup failed"); return 15; }
	PS3Frontend::RegisterMainThread();
	StartSystemEventThread();
	PS3Frontend::Trace("Memory at startup: %s", MemoryText().c_str());
	MenuCanvasInit();
	if (remove((std::string(base) + "RELOAD.FLAG").c_str()) == 0)
		ShowSplash("Back to the game list...");
	else
		PS3Frontend::Status("Display initialized. Scanning for games...");
	std::string game = ChooseGame(); int result = 0;
	if (!game.empty() && running) {
		StartGameLogs(game);
		slowDiagnostics = 0;
		PS3Frontend::Trace("Selected game: %s", game.c_str()); PS3Frontend::Status("Starting PPSSPP Core...");
		// The screen keeps showing that while the game loads; the game needs the memory.
		MenuCanvasRelease();
		// Per-game values only last for this run: settings.ini is saved from the global ones.
		PS3Frontend::Settings globalSettings = settings;
		globalSettingsDuringGame = &globalSettings;
		inGameChanged = 0;
		LoadGameSettings(game);
		ApplyLogMask();
		{
			std::string text = SettingsText(settings);
			for (char &c : text)
				if (c == '\n') c = ' ';
			PS3Frontend::Trace("Settings for this game: %s", text.c_str());
		}
		// Read by the JIT when it is created, so it must be set before the core starts.
		MIPSComp::g_ppcJitVMX = settings.cpuJit && settings.vmx;
		MIPSComp::g_ppcJitTierRuns = settings.jitWarmup ? 20 : 0;
		MIPSComp::g_irPs3BlockLimit = settings.jitLarge ? 98304 : 65536;
		MIPSComp::g_irPs3CacheFullHook = JitCacheFullHook;
		g_geAsyncLists = settings.geListThread != 0;
		g_geAsyncRunAhead = settings.geListThread >= 2;
		g_geAsyncNoSyncWait = settings.geListThread == 3;
		MIPSComp::g_irPs3ArenaLimit = settings.jitLarge ? 2304 * 1024 : 1536 * 1024;
		TLPrep::SetEnabled(settings.spuVertex);
		// Diag build: every draw whose vertices the SPUs prepare is checked against the PPU's decode.
		Rasterizer::g_spuVertexCheck = IsDiagEdition();
		if (Rasterizer::g_spuVertexCheck)
			PS3Frontend::Trace("SPU vertex check on (diag build): SPU-prepared draws are compared with the PPU's decode");
		TexSpu::SetEnabled(settings.spuTexture);
		PS3Frontend::Trace("CPU JIT float/vector registers: %s", MIPSComp::g_ppcJitVMX ? "AltiVec (vmx=1)" : "FPU (vmx=0)");
		PS3Frontend::Trace("Memory before core start: %s", MemoryText().c_str());
		TakeOomReserve();
		std::set_new_handler(OutOfMemoryHandler);
		PS3Frontend::Trace("Network initialization entered");
		net::Init();
		PS3Frontend::Trace("Network initialization returned; entering PPSSPP Core");
		PS3Frontend::EnableThreadDiagnostics();
		if (settings.logLevel < 2) {
			// Buffered: a line-per-syscall unbuffered log on the HDD was a large part of the slowdown.
			setvbuf(stderr, nullptr, _IOFBF, 64 * 1024);
			setvbuf(stdout, nullptr, _IOFBF, 16 * 1024);
		}
		if (settings.renderer == 1) {
			RSXRendererOptions options;
			options.scale = settings.resolution;
			options.syncEveryFrame = settings.vramSyncEveryFrame;
			options.bilinear = settings.bilinear;
			options.lateDepth = settings.lateDepth;
			options.keepLayouts = settings.keepLayouts;
			options.fxaa = settings.fxaa;
			options.skipDepthReads = !settings.lensFlare;
			options.cpuRows = settings.cpuRows == 1;
			options.screenMode = settings.screenMode;
			options.batched = settings.fast3d >= 1;
			options.hardwareTL = settings.fast3d == 2;
			options.textureCheckFull = settings.textureCheckFull;
			options.writeBackAllRows = settings.writeBackAllRows;
			options.rttCopyReuse = settings.rttCopyReuse;
			options.tiled = settings.rsxTiled != 0;
			options.tiledDepth = settings.rsxTiled == 1;
			Rasterizer::g_tlMergeEnabled = settings.tlMerge;
			RSX::SetVertexRingInMain(settings.vertexRingMain);
			rsxRenderer = RSXRendererCreate(options);
			if (rsxRenderer) {
				Rasterizer::g_hardwareBackend = RSXRendererBackend(rsxRenderer);
				if (settings.geThread && settings.fast3d == 2)
					Rasterizer::g_hardwareBackend = Rasterizer::StartGEWorker(Rasterizer::g_hardwareBackend);
				PS3Frontend::Trace("Renderer: RSX hardware, %dx%s", settings.resolution,
					settings.geThread && settings.fast3d == 2 ? ", 3D draws on the GE worker thread" : "");
			} else {
				PS3Frontend::Trace("Renderer: RSX setup failed, falling back to software");
			}
		}
		if (!rsxRenderer)
			StartPresenter();
		std::string memstick = "--memstick=" + std::string(base) + "memstick";
		std::vector<const char *> args = {APP_DIR "EBOOT.BIN", "--graphics=software", settings.cpuJit ? "--jit-ir" : "--ir"};
		PS3Frontend::Trace("CPU core: %s", settings.cpuJit ? "JIT requested" : "IR interpreter");
		if (settings.cpuJit)
			PS3Frontend::Trace("JIT: %s - %s", PS3Jit::Available() ? "available" : "NOT available, using the IR interpreter", PS3Jit::StatusText());
		// Core logging (separate from Debug): PPSSPP's log in core.log.
		OpenLogFiles();
		if (settings.debugButtons && settings.logLevel > 0)
			args.push_back("--log");
		args.push_back(memstick.c_str());
		args.push_back(game.c_str());
		PS3Audio::SetSlowMode(settings.audioSlow);
		g_sasNoBlock = false;
		if (settings.sound)
			PS3Audio::Start();
		stallWatchRunning = true;
		sys_ppu_thread_t stallThread;
		const bool stallWatch = sysThreadCreate(&stallThread, StallWatchThread, nullptr, 900, 0x4000, THREAD_JOINABLE, (char *)"stall_watch") == 0;
		try {
			result = PS3EmulatorMain((int)args.size(), args.data());
		} catch (const std::exception &error) {
			PS3Frontend::Trace("Core startup exception: %s", error.what());
			fprintf(stderr, "Core startup exception: %s\n", error.what());
			result = 17;
		}
		PS3Audio::Stop();
		StopPresenter();
		if (rsxRenderer) {
			Rasterizer::StopGEWorker();
			Rasterizer::g_hardwareBackend = nullptr;
			RSXRendererDestroy(rsxRenderer);
			rsxRenderer = nullptr;
		}
		// SPU thread groups have to be joined before the app exits (an unjoined one can hang the
		// exit to the XMB); stopped after every game, started again by the next one.
		TLPrep::Shutdown();
		TexSpu::Shutdown();
		SasSpuShutdown();
		RSX::StepQuiet("idle");
		PS3Frontend::Trace("SPU work stopped");
		stallWatchRunning = false;
		if (stallWatch) {
			u64 stallRet = 0;
			sysThreadJoin(stallThread, &stallRet);
		}
		fflush(stderr); fflush(stdout);
		PS3Frontend::Trace("PPSSPP Core returned: %d, frames presented: %u, flips: %u, dropped: %u",
			result, frames, (unsigned)flips, (unsigned)droppedFrames);
		{
			// Back to the normal settings (in-game options changes are in them already): the game's
			// own values end with it. An L3+R3 overlay toggle is kept.
			const bool showFps = settings.showFps;
			settings = globalSettings;
			settings.showFps = showFps;
			globalSettingsDuringGame = nullptr;
		}
		SaveSettings();
		// Also when the game was quit from the PS button menu: core.log and the rest still go to
		// the game's log folder.
		EndGameLogs();
		if (running) {
			PS3Frontend::Status(result ? "Game stopped with an error (see USRDIR/logs). X: back to the game list   Circle: exit" :
				"Game stopped. X: back to the game list   Circle: exit to the XMB");
			// Buttons still held from the game don't count.
			while (PS3Frontend::Pump(false) && (pad.BTN_CIRCLE || pad.BTN_CROSS)) SDL_Delay(16);
			while (PS3Frontend::Pump(false) && !pad.BTN_CIRCLE && !pad.BTN_CROSS) SDL_Delay(16);
			if (pad.BTN_CROSS)
				quitToGameList = true;
		}
		if (quitToGameList) {
			ShowSplash("Back to the game list...");
			if (!PrepareReloadSelf()) {
				quitToGameList = false;
				PS3Frontend::Status("Could not prepare the restart (see launch.log). Circle: exit to the XMB.");
				while (PS3Frontend::Pump(false) && !pad.BTN_CIRCLE) SDL_Delay(16);
			}
		}
	}
	PS3Frontend::Trace("Normal frontend shutdown");
	StopSystemEventThread();
	sysUtilUnregisterCallback(1); ioPadEnd();
	MenuCanvasRelease();
	SDL_DestroyTexture(screen); SDL_DestroyTexture(font); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
	if (quitToGameList) {
		// Back to the game list: the app starts itself again, fresh (nothing left over from the game in
		// memory), through RELOAD.SELF (see PrepareReloadSelf). Only returns if that failed; then the
		// app just ends.
		PS3Frontend::Trace("Back to the game list: starting the emulator again (RELOAD.SELF)");
		// Tells the new instance to show the same screen while it starts.
		if (FILE *flag = fopen((std::string(base) + "RELOAD.FLAG").c_str(), "wb"))
			fclose(flag);
		fflush(stderr); fflush(stdout);
		sysProcessExitSpawn2((std::string(base) + "RELOAD.SELF").c_str(), nullptr, nullptr, nullptr, 0, 1001, SYS_PROCESS_SPAWN_STACK_SIZE_1M);
		PS3Frontend::Trace("Starting the emulator again failed");
		remove((std::string(base) + "RELOAD.FLAG").c_str());
	}
	return result;
}
