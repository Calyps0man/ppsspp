// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#include <malloc.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/nv40.h>
#include <sysutil/video.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include "GPU/PS3Profile.h"
#include <ppu-asm.h>

#include "PS3/rsx/RSXCore.h"
#include "PS3/rsx/RSXShaders.inc"
#include "PS3/frontend/Frontend.h"

extern "C" void __real_rsxSetTransferScaleSurface(gcmContextData *, const gcmTransferScale *, const gcmTransferSurface *);
void PS3ScaledTransfer(gcmContextData *context, const gcmTransferScale *scale, const gcmTransferSurface *surface);
extern "C" s32 rsxContextCallback(gcmContextData *context, u32 count);
extern "C" s32 __real_rsxInit(gcmContextData **context, u32 cmdSize, u32 ioSize, const void *ioAddress);

namespace RSX {

static gcmContextData *g_context = nullptr;
static uint8_t *g_io = nullptr;           // Start of our I/O-mapped region.
static uint32_t g_ioUsed = 0;             // Bump pointer (bytes from g_io).
static uint32_t g_ioMark = 0;
static uint32_t g_ioFirstFree = 0;

// Labels 64..255 are for applications; keep away from the ones samples commonly use (255).
static constexpr u8 LABEL_FENCE = 200;
static constexpr u8 LABEL_GPUWAIT = 201;
static uint32_t g_fenceValue = 0;
static uint32_t g_gpuWaitValue = 0;
static void InitLabels();

// The PPU time base (79.8 MHz on a PS3), not sysGetCurrentTime: that's a system call, and
// StepQuiet runs several times per draw (a few microseconds each, per draw, in Gran Turismo's races).
static double NowSeconds() {
	uint64_t tb;
	__asm__ volatile("mftb %0" : "=r"(tb));
	return (double)tb * (1.0 / 79800000.0);
}

// Command buffer wrap-around, our own instead of libgcm's default callback. Two freezes (Silent
// Hill Origins, Call of Duty) stopped with the RSX idle and put == get exactly at the end of a
// command buffer segment: whoever called the callback never came back. This one is simple, waits
// with a time limit report, and says in launch.log what it is waiting for.
static constexpr uint32_t CB_SEGMENT = 64 * 1024;
static uintptr_t g_cbStart = 0, g_cbEnd = 0;  // The usable command buffer [start, end).
static uint32_t g_cbWraps = 0;
static volatile double g_cbWaitSince = 0.0;    // Nonzero while waiting for the RSX to leave a segment.

uint32_t IoOffset(const void *ptr);

static s32 CommandBufferCallback(gcmContextData *ctx, u32 count) {
	gcmControlRegister *ctrl = gcmGetControlRegister();
	const uintptr_t cur = (uintptr_t)ctx->current;
	if (count * 4 + 4 > CB_SEGMENT || cur < g_cbStart || cur >= g_cbEnd) {
		PS3Frontend::Trace("RSX command buffer: can't wrap (count %u, current %08x, buffer %08x-%08x)", count, (u32)cur, (u32)g_cbStart, (u32)g_cbEnd);
		return -1;
	}
	uintptr_t next = g_cbStart + ((cur - g_cbStart) / CB_SEGMENT + 1) * CB_SEGMENT;
	if (next + CB_SEGMENT > g_cbEnd)
		next = g_cbStart;
	const uint32_t nextOffset = IoOffset((const void *)next);
	// The RSX runs up to here; the jump after it is taken once commands in the next segment are
	// flushed (put then points past it).
	__sync_synchronize();
	ctrl->put = IoOffset((const void *)cur);
	*(volatile u32 *)cur = 0x20000000 | nextOffset;  // Jump.
	__sync_synchronize();
	// Don't overwrite commands the RSX hasn't read yet (it would have to be a whole lap behind).
	PS3_PROFILE(rsxCbWait);
	const double start = NowSeconds();
	bool reported = false;
	while (true) {
		const uint32_t get = ctrl->get;
		if (get < nextOffset || get >= nextOffset + CB_SEGMENT)
			break;
		if (!g_cbWaitSince)
			g_cbWaitSince = start;
		const double waited = NowSeconds() - start;
		if (waited > 2.0 && !reported) {
			reported = true;
			PS3Frontend::Trace("RSX command buffer: waiting %.1f s for the RSX to leave %08x-%08x; get %08x put %08x ref %08x",
				waited, nextOffset, nextOffset + CB_SEGMENT, get, ctrl->put, ctrl->ref);
		}
		usleep(20);
	}
	g_cbWaitSince = 0.0;
	ctx->begin = (u32 *)next;
	ctx->current = (u32 *)next;
	ctx->end = (u32 *)(next + CB_SEGMENT - 4);
	g_cbWraps++;
	return 0;
}

}  // namespace RSX

// SDL's PSL1GHT driver calls rsxInit with a 64 KB command buffer inside a 1 MB I/O region. A
// renderer needs a bigger command buffer, plus main memory the RSX can read (vertex staging,
// readback). Swap in our own parameters; SDL keeps working the same way on top of it.
extern "C" s32 __wrap_rsxInit(gcmContextData **context, u32 cmdSize, u32 ioSize, const void *ioAddress) {
	using namespace RSX;
	void *io = memalign(1024 * 1024, IO_REGION_SIZE);
	if (io) {
		s32 result = __real_rsxInit(context, COMMAND_BUFFER_SIZE, IO_REGION_SIZE, io);
		if (result == 0) {
			g_io = (uint8_t *)io;
			// The command buffer lives at the start of the region; leave a margin after it.
			g_ioFirstFree = COMMAND_BUFFER_SIZE + 64 * 1024;
			g_ioUsed = g_ioMark = g_ioFirstFree;
			g_context = context ? *context : gGcmContext;
			PS3Frontend::Trace("RSX init: command buffer %u KB, I/O region %u MB (requested %u KB / %u KB)",
				COMMAND_BUFFER_SIZE / 1024, IO_REGION_SIZE / (1024 * 1024), cmdSize / 1024, ioSize / 1024);
			if (g_context) {
				g_cbStart = (uintptr_t)g_context->begin;
				g_cbEnd = (uintptr_t)(g_io + COMMAND_BUFFER_SIZE);
				PS3Frontend::Trace("RSX command buffer: begin %08x (offset %08x) current %08x end %08x; buffer end %08x; own wrap-around, %u KB segments",
					(u32)(uintptr_t)g_context->begin, IoOffset(g_context->begin), (u32)(uintptr_t)g_context->current,
					(u32)(uintptr_t)g_context->end, (u32)g_cbEnd, CB_SEGMENT / 1024);
				if (g_cbStart >= (uintptr_t)g_io && g_cbStart + 2 * CB_SEGMENT <= g_cbEnd)
					g_context->callback = (gcmContextCallback)__get_opd32(CommandBufferCallback);
				else
					PS3Frontend::Trace("RSX command buffer: unexpected layout, keeping the system's wrap-around");
			}
			return 0;
		}
		PS3Frontend::Trace("RSX init with enlarged buffers failed (%d); using SDL's request", result);
		free(io);
	}
	s32 result = __real_rsxInit(context, cmdSize, ioSize, ioAddress);
	if (result == 0) {
		g_context = context ? *context : gGcmContext;
		g_io = nullptr;
	}
	return result;
}

namespace RSX {

static volatile const char *g_step = "idle";
static volatile double g_stepStart = 0.0;
static volatile bool g_watchdogRunning = false;

std::string GpuStateText() {
	char text[320];
	gcmControlRegister *ctrl = gcmGetControlRegister();
	const double cbWait = g_cbWaitSince ? NowSeconds() - g_cbWaitSince : 0.0;
	snprintf(text, sizeof(text), "put %08x get %08x ref %08x, fence label %u (sent %u), wait label %u (sent %u); command buffer wraps %u%s",
		ctrl ? ctrl->put : 0, ctrl ? ctrl->get : 0, ctrl ? ctrl->ref : 0,
		*(volatile u32 *)gcmGetLabelAddress(LABEL_FENCE), g_fenceValue,
		*(volatile u32 *)gcmGetLabelAddress(LABEL_GPUWAIT), g_gpuWaitValue, g_cbWraps,
		cbWait > 0.0 ? ", WAITING IN WRAP-AROUND" : "");
	return text;
}

void Step(const char *name) {
	g_step = name;
	g_stepStart = NowSeconds();
	PS3Frontend::Trace("RSX step: %s", name);
}

void StepQuiet(const char *name) {
	g_step = name;
	g_stepStart = NowSeconds();
}

static void GraphicsException(const u32 value) {
	PS3Frontend::Trace("RSX GRAPHICS EXCEPTION 0x%08x during step '%s'; %s", value, (const char *)g_step, GpuStateText().c_str());
}

static void WatchdogThread(void *) {
	const char *reported = nullptr;
	while (g_watchdogRunning) {
		usleep(500 * 1000);
		const char *step = (const char *)g_step;
		const double age = NowSeconds() - g_stepStart;
		if (step && strcmp(step, "idle") != 0 && age > 3.0 && step != reported) {
			PS3Frontend::Trace("WATCHDOG: still in RSX step '%s' after %.1f s; %s", step, age, GpuStateText().c_str());
			reported = step;
		}
	}
}

void InitDiagnostics() {
	if (g_watchdogRunning || !g_context) return;
	InitLabels();
	gcmSetGraphicsHandler(GraphicsException);
	g_watchdogRunning = true;
	sys_ppu_thread_t id;
	if (sysThreadCreate(&id, WatchdogThread, nullptr, 1000, 0x4000, 0, (char *)"rsx_watchdog") != 0) {
		g_watchdogRunning = false;
		PS3Frontend::Trace("RSX watchdog thread could not start");
	} else {
		PS3Frontend::Trace("RSX diagnostics: watchdog and exception handler installed; %s", GpuStateText().c_str());
	}
}

bool Ready() { return g_context != nullptr; }
_gcmCtxData *Context() { return g_context; }

std::string Describe() {
	gcmConfiguration config{};
	gcmGetConfiguration(&config);
	char text[512];
	snprintf(text, sizeof(text), "%u MB video memory, I/O %u MB, core %u MHz, memory %u MHz",
		config.localSize / (1024 * 1024), config.ioSize / (1024 * 1024), config.coreFreq / 1000000, config.memoryFreq / 1000000);
	return text;
}

void *LocalAlloc(uint32_t size, uint32_t align) { return rsxMemalign(align, size); }

// Regions 0 and 1 are left alone (system/SDL use); 2..14 for render targets.
static bool g_tileUsed[15];
static int g_tileFailures = 0;

bool TilePitchSupported(uint32_t pitch) {
	static const uint32_t pitches[] = { 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800, 0xA00, 0xC00, 0xD00, 0xE00, 0x1000,
		0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800, 0x4000, 0x5000, 0x6000, 0x6800, 0x7000,
		0x8000, 0xA000, 0xC000, 0xD000, 0xE000, 0x10000 };
	for (uint32_t p : pitches)
		if (p == pitch)
			return true;
	return false;
}

int BindTileRegion(uint32_t offset, uint32_t size, uint32_t pitch, int bank) {
	if (!TilePitchSupported(pitch) || (offset & 0xFFFF) || (size & 0xFFFF) || size == 0)
		return -1;
	// The RSX must not be drawing while the memory controller's tile setup changes: binding a region
	// mid-frame hung the RSX now and then (Dead to Rights sometimes never got past its first
	// frames: fence timeouts with the command buffer fully read).
	Finish();
	for (int i = 2; i < 15; ++i) {
		if (g_tileUsed[i])
			continue;
		const s32 r1 = gcmSetTileInfo((u8)i, GCM_LOCATION_RSX, offset, size, pitch, GCM_COMPMODE_DISABLED, 0, (u8)(bank & 3));
		const s32 r2 = r1 == 0 ? gcmBindTile((u8)i) : -1;
		if (r1 != 0 || r2 != 0) {
			if (g_tileFailures++ < 4)
				PS3Frontend::Trace("RSX: tile region %d at %08x (%u bytes, pitch %u) not set: %08x %08x", i, offset, size, pitch, (u32)r1, (u32)r2);
			return -1;
		}
		g_tileUsed[i] = true;
		return i;
	}
	return -1;
}

void UnbindTileRegion(int region) {
	if (region < 2 || region >= 15 || !g_tileUsed[region])
		return;
	Finish();
	gcmUnbindTile((u8)region);
	g_tileUsed[region] = false;
}
void LocalFree(void *ptr) { if (ptr) rsxFree(ptr); }
uint32_t LargestFreeLocal() {
	uint32_t lo = 0, hi = 256u * 1024 * 1024;
	while (hi - lo > 1024 * 1024) {
		const uint32_t mid = lo + (hi - lo) / 2;
		void *p = LocalAlloc(mid, 1024);
		if (p) { LocalFree(p); lo = mid; } else hi = mid;
	}
	return lo;
}

uint32_t LocalOffset(const void *ptr) {
	u32 offset = 0;
	rsxAddressToOffset((void *)ptr, &offset);
	return offset;
}

void *IoAlloc(uint32_t size, uint32_t align) {
	if (!g_io) return nullptr;
	uint32_t start = (g_ioUsed + align - 1) & ~(align - 1);
	if (start + size > IO_REGION_SIZE - IO_RING_SIZE) return nullptr;
	g_ioUsed = start + size;
	return g_io + start;
}
uint32_t IoOffset(const void *ptr) {
	u32 offset = 0;
	gcmAddressToOffset((void *)ptr, &offset);
	return offset;
}
void IoMark() { g_ioMark = g_ioUsed; }
void IoRelease() { g_ioUsed = g_ioMark; }

void Flush() {
	__sync_synchronize();
	rsxFlushBuffer(g_context);
}

// Label memory starts with whatever was left there (Frontend 07 found 322420947), so fences
// compared against it looked finished before the RSX had done anything and every readback was
// empty. Our labels are zeroed before first use; counting then starts at 1.
static void InitLabels() {
	static bool done = false;
	if (done) return;
	done = true;
	*(volatile u32 *)gcmGetLabelAddress(LABEL_FENCE) = 0;
	*(volatile u32 *)gcmGetLabelAddress(LABEL_GPUWAIT) = 0;
	__sync_synchronize();
	g_fenceValue = 0;
	g_gpuWaitValue = 0;
}

uint32_t Fence() {
	InitLabels();
	++g_fenceValue;
	if (g_fenceValue == 0) g_fenceValue = 1;
	rsxSetWriteBackendLabel(g_context, LABEL_FENCE, g_fenceValue);
	return g_fenceValue;
}

bool FenceDone(uint32_t value) {
	volatile u32 *label = (volatile u32 *)gcmGetLabelAddress(LABEL_FENCE);
	return (int32_t)(*label - value) >= 0;
}

bool WaitFence(uint32_t value, double timeoutSeconds) {
	if (FenceDone(value)) return true;
	PS3_PROFILE(rsxFenceWait);
	PS3Profile::rsxFenceWaits++;
	Flush();
	double start = NowSeconds();
	while (!FenceDone(value)) {
		if (NowSeconds() - start > timeoutSeconds) {
			PS3Frontend::Trace("RSX fence %u timed out (label %u); %s", value, *(volatile u32 *)gcmGetLabelAddress(LABEL_FENCE), GpuStateText().c_str());
			// Once: the commands around where the RSX stopped (the command buffer starts the I/O region).
			static bool dumped = false;
			gcmControlRegister *ctrl = gcmGetControlRegister();
			if (!dumped && ctrl && g_io) {
				dumped = true;
				const uint32_t base = IoOffset(g_io);
				const uint32_t get = ctrl->get;
				if (get >= base + 64 && get < base + COMMAND_BUFFER_SIZE - 64) {
					const uint32_t *p = (const uint32_t *)(g_io + (get - base) - 64);
					for (int i = 0; i < 32; i += 8)
						PS3Frontend::Trace("RSX commands at %08x: %08x %08x %08x %08x %08x %08x %08x %08x", get - 64 + i * 4,
							p[i], p[i + 1], p[i + 2], p[i + 3], p[i + 4], p[i + 5], p[i + 6], p[i + 7]);
				}
			}
			return false;
		}
		usleep(20);
	}
	return true;
}

bool Finish(double timeoutSeconds) {
	return WaitFence(Fence(), timeoutSeconds);
}

void GpuWaitForRendering() {
	InitLabels();
	++g_gpuWaitValue;
	if (g_gpuWaitValue == 0) g_gpuWaitValue = 1;
	rsxSetWriteBackendLabel(g_context, LABEL_GPUWAIT, g_gpuWaitValue);
	rsxSetWaitLabel(g_context, LABEL_GPUWAIT, g_gpuWaitValue);
	rsxSetWaitForIdle(g_context);
}

// ---------------------------------------------------------------------------------------------
// Programs

static rsxVertexProgram *g_vp = nullptr;
static void *g_vpUcode = nullptr;
static rsxVertexProgram *g_vpTL = nullptr;
static void *g_vpTLUcode = nullptr;
static rsxVertexProgram *g_vpTLMin = nullptr;
static void *g_vpTLMinUcode = nullptr;
static uint32_t g_fpOffset[RSX_FRAGMENT_PROGRAM_COUNT];
static void *g_fpPtr[RSX_FRAGMENT_PROGRAM_COUNT];
static bool g_programsLoaded = false;
static int g_boundFragment = -1;

bool LoadPrograms() {
	if (g_programsLoaded) return true;
	g_vp = (rsxVertexProgram *)rsx_vp_main;
	u32 size = 0;
	rsxVertexProgramGetUCode(g_vp, &g_vpUcode, &size);
	if (!g_vpUcode || size == 0) {
		PS3Frontend::Trace("RSX: vertex program has no microcode");
		return false;
	}
	for (int i = 0; i < RSX_FRAGMENT_PROGRAM_COUNT; ++i) {
		g_fpOffset[i] = 0;
		const rsxFragmentProgram *fp = (const rsxFragmentProgram *)rsx_fp_table[i];
		if (!fp) continue;
		void *ucode = nullptr;
		u32 fpSize = 0;
		rsxFragmentProgramGetUCode(fp, &ucode, &fpSize);
		void *copy = LocalAlloc(fpSize, 64);
		if (!copy) {
			PS3Frontend::Trace("RSX: out of local memory for fragment program %d", i);
			return false;
		}
		memcpy(copy, ucode, fpSize);
		g_fpOffset[i] = LocalOffset(copy);
		g_fpPtr[i] = copy;
	}
	g_vpTL = (rsxVertexProgram *)rsx_vp_tl;
	size = 0;
	rsxVertexProgramGetUCode(g_vpTL, &g_vpTLUcode, &size);
	if (!g_vpTLUcode || size == 0) {
		PS3Frontend::Trace("RSX: T&L vertex program has no microcode");
		g_vpTL = nullptr;
	}
	g_vpTLMin = (rsxVertexProgram *)rsx_vp_tlmin;
	size = 0;
	rsxVertexProgramGetUCode(g_vpTLMin, &g_vpTLMinUcode, &size);
	if (!g_vpTLMinUcode || size == 0) g_vpTLMin = nullptr;
	g_programsLoaded = true;
	return true;
}

bool BindVertexProgramTLMin() {
	if (!g_vpTLMin) return false;
	rsxLoadVertexProgram(g_context, g_vpTLMin, g_vpTLMinUcode);
	g_boundFragment = -1;
	return true;
}

bool BindTestVertexProgram(int which) {
	static const unsigned char *const programs[3] = {rsx_vp_tlpos, rsx_vp_tlposdirect, rsx_vp_tlnolights};
	if (which < 0 || which >= 3) return false;
	rsxVertexProgram *vp = (rsxVertexProgram *)programs[which];
	void *ucode = nullptr;
	u32 size = 0;
	rsxVertexProgramGetUCode(vp, &ucode, &size);
	if (!ucode || !size) return false;
	rsxLoadVertexProgram(g_context, vp, ucode);
	g_boundFragment = -1;
	return true;
}

void BindVertexProgram() {
	rsxLoadVertexProgram(g_context, g_vp, g_vpUcode);
	g_boundFragment = -1;
}

bool BindVertexProgramTL() {
	if (!g_vpTL) return false;
	rsxLoadVertexProgram(g_context, g_vpTL, g_vpTLUcode);
	g_boundFragment = -1;
	return true;
}

void SetVertexConstantBlock(int first, int count, const float *values) {
	rsxLoadVertexProgramParameterBlock(g_context, (u32)first, (u32)count, values);
}

int FragmentIndex(bool textured, int texFunc, bool rgba, bool doubling, bool fog, bool blendMul) {
	if (texFunc > 4) texFunc = 4;
	if (!textured) {
		texFunc = 0;
		rgba = false;
	}
	return 1 + ((((textured ? 1 : 0) * 5 + texFunc) * 2 + (rgba ? 1 : 0)) * 2 + (doubling ? 1 : 0)) * 4 + (fog ? 2 : 0) + (blendMul ? 1 : 0);
}

int FragmentFixSub(bool rgba, bool doubling, bool reverse) {
	return RSX_FRAGMENT_FIXSUB + (rgba ? 4 : 0) + (doubling ? 2 : 0) + (reverse ? 1 : 0);
}

void BindFragmentProgram(int index) {
	if (index < 0 || index >= RSX_FRAGMENT_PROGRAM_COUNT || !rsx_fp_table[index]) index = FRAGMENT_CLEAR;
	if (index == g_boundFragment) return;
	rsxLoadFragmentProgramLocation(g_context, (const rsxFragmentProgram *)rsx_fp_table[index], g_fpOffset[index], GCM_LOCATION_RSX);
	g_boundFragment = index;
}

void SetVertexConstants(const float env[4], const float fog[4], const float mul[4]) {
	float block[12];
	memcpy(block, env, 16);
	memcpy(block + 4, fog, 16);
	memcpy(block + 8, mul, 16);
	rsxLoadVertexProgramParameterBlock(g_context, g_vp->const_start, 3, block);
}

// ---------------------------------------------------------------------------------------------
// Targets and state

void SetTarget(const Target &t) {
	gcmSurface sf{};
	sf.colorFormat = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = t.colorInMain ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	sf.colorOffset[0] = t.colorOffset;
	sf.colorPitch[0] = t.colorPitch;
	for (int i = 1; i < 4; ++i) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = 0;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = t.hasDepth ? t.depthOffset : 0;
	sf.depthPitch = t.hasDepth ? t.depthPitch : 64;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = t.width;
	sf.height = t.height;
	sf.x = 0;
	sf.y = 0;
	rsxSetSurface(g_context, &sf);
	for (int i = 0; i < 8; ++i)
		rsxSetViewportClip(g_context, i, t.width, t.height);
}

void SetPixelViewport(uint16_t width, uint16_t height) {
	// The renderer sends normalized device coordinates: x_ndc = 2*px/width - 1, y_ndc = 2*py/height - 1,
	// z_ndc = 2*z - 1. This maps them back to pixels with y growing downwards and z in 0..1.
	float scale[4] = {width * 0.5f, height * 0.5f, 0.5f, 0.0f};
	float offset[4] = {width * 0.5f, height * 0.5f, 0.5f, 0.0f};
	rsxSetViewport(g_context, 0, 0, width, height, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(g_context, 0, 0, width, height);
}

void SetScissor(int x, int y, int w, int h) {
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (w < 0) w = 0;
	if (h < 0) h = 0;
	rsxSetScissor(g_context, (u16)x, (u16)y, (u16)w, (u16)h);
}

void ClearTarget(uint32_t argb, bool color, bool depthStencil, uint32_t depthStencilValue) {
	u32 mask = 0;
	if (color) mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
	if (depthStencil) mask |= GCM_CLEAR_Z | GCM_CLEAR_S;
	rsxSetClearColor(g_context, argb);
	rsxSetClearDepthStencil(g_context, depthStencilValue);
	rsxClearSurface(g_context, mask);
}

void SetDefaultState() {
	rsxSetColorMask(g_context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(g_context, 0);
	rsxSetBlendEnable(g_context, GCM_FALSE);
	rsxSetDepthTestEnable(g_context, GCM_FALSE);
	rsxSetDepthWriteEnable(g_context, GCM_FALSE);
	rsxSetAlphaTestEnable(g_context, GCM_FALSE);
	rsxSetStencilTestEnable(g_context, GCM_FALSE);
	rsxSetCullFaceEnable(g_context, GCM_FALSE);
	rsxSetLogicOpEnable(g_context, GCM_FALSE);
	rsxSetDitherEnable(g_context, GCM_FALSE);
	rsxSetShadeModel(g_context, GCM_SHADE_MODEL_SMOOTH);
	rsxSetFrontFace(g_context, GCM_FRONTFACE_CCW);
	rsxSetZMinMaxControl(g_context, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxSetUserClipPlaneControl(g_context, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
}

// ---------------------------------------------------------------------------------------------
// Vertex ring: 8 segments of 1 MB in local memory. A segment is reused only after the fence
// written when the renderer last left it has passed.

static constexpr uint32_t RING_SEGMENT_BYTES = 1024 * 1024;
static constexpr uint32_t RING_SEGMENT_VERTS = RING_SEGMENT_BYTES / sizeof(Vertex);
static constexpr uint32_t RING_MAX_SEGMENTS = 8;
static uint32_t g_ringSegments = 8;
static Vertex *g_ring = nullptr;
static uint32_t g_ringOffset = 0;
static uint32_t g_ringSegment = 0;
static uint32_t g_ringPos = 0;  // Vertices used in the current segment.
static uint32_t g_ringFence[RING_MAX_SEGMENTS]{};
static bool g_ringBound = false;
static bool g_ringWantMain = true;
static u8 g_ringLocation = GCM_LOCATION_RSX;
// The T&L layout bound once at the start of the ring (see AllocTLVertices); off whenever anything
// else binds attributes.
static bool g_tlRingBound = false;

void SetVertexRingInMain(bool inMain) {
	g_ringWantMain = inMain;
	if (!g_ring || (g_ringLocation == GCM_LOCATION_CELL) == inMain)
		return;
	// Switching (settings menu, between games): the RSX must be done with the old ring.
	Finish();
	if (g_ringLocation == GCM_LOCATION_RSX)
		LocalFree(g_ring);
	g_ring = nullptr;
	g_ringBound = false;
	g_tlRingBound = false;
	for (uint32_t &f : g_ringFence)
		f = 0;
}
bool VertexRingInMain() { return g_ringLocation == GCM_LOCATION_CELL; }

uint32_t RingOffset(const void *ptr) {
	return g_ringOffset + (uint32_t)((const uint8_t *)ptr - (const uint8_t *)g_ring);
}

static void BindRing() {
	const u32 stride = sizeof(Vertex);
	const u8 loc = g_ringLocation;
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_POS, 0, g_ringOffset + offsetof(Vertex, x), stride, 4, GCM_VERTEX_DATA_TYPE_F32, loc);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_COLOR0, 0, g_ringOffset + offsetof(Vertex, c0), stride, 4, GCM_VERTEX_DATA_TYPE_U8, loc);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_COLOR1, 0, g_ringOffset + offsetof(Vertex, c1), stride, 4, GCM_VERTEX_DATA_TYPE_U8, loc);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_TEX0, 0, g_ringOffset + offsetof(Vertex, s), stride, 4, GCM_VERTEX_DATA_TYPE_F32, loc);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_TEX1, 0, g_ringOffset + offsetof(Vertex, fog), stride, 1, GCM_VERTEX_DATA_TYPE_F32, loc);
	// The T&L layout's normal array; unused here.
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_NORMAL, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, loc);
	g_ringBound = true;
	g_tlRingBound = false;
}

Vertex *AllocVertices(int count, uint32_t *firstIndex) {
	if (count <= 0 || (uint32_t)count > RING_SEGMENT_VERTS) return nullptr;
	if (!g_ring) {
		if (g_ringWantMain && g_io) {
			// The reserved end of the I/O region.
			g_ring = (Vertex *)(g_io + IO_REGION_SIZE - IO_RING_SIZE);
			g_ringOffset = IoOffset(g_ring);
			g_ringLocation = GCM_LOCATION_CELL;
			g_ringSegments = IO_RING_SIZE / RING_SEGMENT_BYTES;
		} else {
			g_ring = (Vertex *)LocalAlloc(RING_MAX_SEGMENTS * RING_SEGMENT_BYTES, 128);
			if (!g_ring) return nullptr;
			g_ringOffset = LocalOffset(g_ring);
			g_ringLocation = GCM_LOCATION_RSX;
			g_ringSegments = RING_MAX_SEGMENTS;
		}
		g_ringSegment = 0;
		g_ringPos = 0;
		PS3Frontend::Trace("RSX vertex ring: %u MB in %s memory", g_ringSegments, g_ringLocation == GCM_LOCATION_CELL ? "main" : "RSX local");
	}
	if (!g_ringBound) BindRing();
	if (g_ringPos + count > RING_SEGMENT_VERTS) {
		g_ringFence[g_ringSegment] = Fence();
		g_ringSegment = (g_ringSegment + 1) % g_ringSegments;
		g_ringPos = 0;
		if (g_ringFence[g_ringSegment])
			WaitFence(g_ringFence[g_ringSegment]);
	}
	uint32_t first = g_ringSegment * RING_SEGMENT_VERTS + g_ringPos;
	g_ringPos += count;
	*firstIndex = first;
	return g_ring + first;
}

uint32_t *AllocIndices(int count, uint32_t *offset) {
	const int slots = (int)((count * 4 + sizeof(Vertex) - 1) / sizeof(Vertex));
	uint32_t first = 0;
	Vertex *v = AllocVertices(slots, &first);
	if (!v) return nullptr;
	*offset = RingOffset(v);
	return (uint32_t *)v;
}

void DrawIndexed(uint32_t primType, uint32_t indexOffset, uint32_t count) {
	if (!count) return;
	if (!g_ringBound) BindRing();
	// The same for the CPU-transformed indexed draws (build 161): their index numbers are ring
	// slots, which a T&L draw before can have used for its own vertices. Silent Hill Origins' pause
	// screen: one triangle of its full-screen darkening quad showed part of the 3D scene drawn
	// before it, a different shape every frame.
	rsxInvalidateVertexCache(g_context);
	rsxDrawIndexArray(g_context, (u8)primType, indexOffset, count, GCM_INDEX_TYPE_32B, g_ringLocation);
}

void BindTLAttribs(uint32_t offset) {
	const u32 stride = sizeof(TLVertex);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_POS, 0, offset + offsetof(TLVertex, x), stride, 3, GCM_VERTEX_DATA_TYPE_F32, g_ringLocation);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_NORMAL, 0, offset + offsetof(TLVertex, nx), stride, 3, GCM_VERTEX_DATA_TYPE_F32, g_ringLocation);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_COLOR0, 0, offset + offsetof(TLVertex, c), stride, 4, GCM_VERTEX_DATA_TYPE_U8, g_ringLocation);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_TEX0, 0, offset + offsetof(TLVertex, u), stride, 2, GCM_VERTEX_DATA_TYPE_F32, g_ringLocation);
	// Not read by the T&L program; size 0 disables them.
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_COLOR1, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, g_ringLocation);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_TEX1, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, g_ringLocation);
	g_ringBound = false;  // The next ring draw binds its own layout again.
	g_tlRingBound = false;
}

// T&L vertices in the ring, starting on a slot that is a multiple of 3: three 48-byte ring slots
// hold exactly four 36-byte T&L vertices, so every T&L draw can use one attribute binding at the
// start of the ring, with its indices offset by the returned base. Rebinding six attribute arrays
// for each draw cost PPU time and RSX stalls (Gran Turismo: ~7,600 small draws a frame).
TLVertex *AllocTLVertices(int count, uint32_t *baseIndex) {
	const uint32_t bytes = (uint32_t)count * sizeof(TLVertex);
	const int slots = (int)((bytes + sizeof(Vertex) - 1) / sizeof(Vertex)) + 2;
	uint32_t first = 0;
	Vertex *v = AllocVertices(slots, &first);
	if (!v) return nullptr;
	const uint32_t aligned = (first + 2) / 3 * 3;
	*baseIndex = aligned / 3 * 4;
	return (TLVertex *)(g_ring + aligned);
}

void BindTLAttribsRing() {
	if (g_tlRingBound) return;
	BindTLAttribs(g_ringOffset);
	g_tlRingBound = true;
}

void DrawIndexedBound(uint32_t primType, uint32_t indexOffset, uint32_t count) {
	if (!count) return;
	// The RSX's vertex cache can still hold ring memory from the draw before (its index data ends
	// right where this draw's vertices start), read before the PPU wrote this draw's data there.
	// Killzone's menu background lost one triangle now and then on the T&L path (Frontend 115).
	rsxInvalidateVertexCache(g_context);
	rsxDrawIndexArray(g_context, (u8)primType, indexOffset, count, GCM_INDEX_TYPE_32B, g_ringLocation);
}

void DrawVertices(uint32_t primType, uint32_t firstIndex, uint32_t count) {
	if (!count) return;
	if (!g_ringBound) BindRing();
	// As for the indexed draws: the vertex cache can hold ring memory read ahead by an earlier
	// draw (a T&L draw's indices end where these vertices start) before the PPU wrote them.
	// Silent Hill Origins: its full-screen fog quad came out now and then as one huge cyan, blue
	// or grey triangle for a frame.
	rsxInvalidateVertexCache(g_context);
	rsxDrawVertexArray(g_context, primType, firstIndex, count);
}

// Re-copies the fragment program microcode into its RSX memory (in case a test overwrote it)
// and forgets all cached bindings, so the next draw starts from a known state.
void ResetDrawState() {
	if (g_programsLoaded) {
		for (int i = 0; i < RSX_FRAGMENT_PROGRAM_COUNT; ++i) {
			const rsxFragmentProgram *fp = (const rsxFragmentProgram *)rsx_fp_table[i];
			if (!fp || !g_fpPtr[i]) continue;
			void *ucode = nullptr;
			u32 fpSize = 0;
			rsxFragmentProgramGetUCode(fp, &ucode, &fpSize);
			memcpy(g_fpPtr[i], ucode, fpSize);
		}
		__sync_synchronize();
	}
	g_boundFragment = -1;
	g_ringBound = false;
	g_tlRingBound = false;
	if (g_context) {
		rsxInvalidateVertexCache(g_context);
		rsxInvalidateTextureCache(g_context, GCM_INVALIDATE_TEXTURE);
	}
}

void EndFrame() {
	// Nothing queued per frame beyond what AllocVertices handles; the vertex program binding
	// survives. Re-bind attributes next frame in case another user (self-test) changed them.
	g_ringBound = false;
	g_tlRingBound = false;
}

// ---------------------------------------------------------------------------------------------
// Textures

static volatile u8 g_maxAniso = GCM_TEXTURE_MAX_ANISO_1;

void SetMaxAnisotropy(int samples) {
	g_maxAniso = samples >= 16 ? GCM_TEXTURE_MAX_ANISO_16 : samples >= 8 ? GCM_TEXTURE_MAX_ANISO_8 :
		samples >= 4 ? GCM_TEXTURE_MAX_ANISO_4 : samples >= 2 ? GCM_TEXTURE_MAX_ANISO_2 : GCM_TEXTURE_MAX_ANISO_1;
}

void BindTexture(int unit, uint32_t offset, uint32_t width, uint32_t height, uint32_t pitch,
	bool linearFilter, bool clampS, bool clampT, bool inMain, bool linearMag) {
	gcmTexture tex{};
	tex.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
	tex.width = (u16)width;
	tex.height = (u16)height;
	tex.depth = 1;
	tex.location = inMain ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	tex.pitch = pitch;
	tex.offset = offset;
	rsxLoadTexture(g_context, (u8)unit, &tex);
	rsxTextureControl(g_context, (u8)unit, GCM_TRUE, 0 << 8, 12 << 8, linearFilter ? g_maxAniso : GCM_TEXTURE_MAX_ANISO_1);
	// linearFilter selects minification; magnification is linear if either asks for it.
	const u8 minFilter = linearFilter ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
	const u8 magFilter = (linearFilter || linearMag) ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
	rsxTextureFilter(g_context, (u8)unit, 0, minFilter, magFilter, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(g_context, (u8)unit, clampS ? GCM_TEXTURE_CLAMP_TO_EDGE : GCM_TEXTURE_REPEAT,
		clampT ? GCM_TEXTURE_CLAMP_TO_EDGE : GCM_TEXTURE_REPEAT, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

void InvalidateTextureCache() {
	rsxInvalidateTextureCache(g_context, GCM_INVALIDATE_TEXTURE);
}

// ---------------------------------------------------------------------------------------------
// Transfers

static u8 TransferMode(Mem src, Mem dst) {
	if (src == Mem::Local)
		return dst == Mem::Local ? GCM_TRANSFER_LOCAL_TO_LOCAL : GCM_TRANSFER_LOCAL_TO_MAIN;
	return dst == Mem::Local ? GCM_TRANSFER_MAIN_TO_LOCAL : GCM_TRANSFER_MAIN_TO_MAIN;
}

void ScaleImage(Mem srcMem, uint32_t srcOffset, uint32_t srcPitch, int srcW, int srcH,
	Mem dstMem, uint32_t dstOffset, uint32_t dstPitch, int dstX, int dstY, int dstW, int dstH, bool linear) {
	if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
	rsxSetTransferScaleMode(g_context, TransferMode(srcMem, dstMem), GCM_TRANSFER_SURFACE);
	gcmTransferScale scale{};
	scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
	scale.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
	scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
	scale.clipX = (u16)dstX;
	scale.clipY = (u16)dstY;
	scale.clipW = (u16)dstW;
	scale.clipH = (u16)dstH;
	scale.outX = (u16)dstX;
	scale.outY = (u16)dstY;
	scale.outW = (u16)dstW;
	scale.outH = (u16)dstH;
	scale.ratioX = rsxGetFixedSint32((float)srcW / (float)dstW);
	scale.ratioY = rsxGetFixedSint32((float)srcH / (float)dstH);
	scale.inX = 0;
	scale.inY = 0;
	scale.inW = (u16)srcW;
	scale.inH = (u16)srcH;
	scale.offset = srcOffset;
	scale.pitch = (u16)srcPitch;
	scale.origin = linear ? GCM_TRANSFER_ORIGIN_CENTER : GCM_TRANSFER_ORIGIN_CORNER;
	scale.interp = linear ? GCM_TRANSFER_INTERPOLATOR_LINEAR : GCM_TRANSFER_INTERPOLATOR_NEAREST;
	gcmTransferSurface surface{};
	surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
	surface.pitch = (u16)dstPitch;
	surface.offset = dstOffset;
	PS3ScaledTransfer(g_context, &scale, &surface);
}

// Room for count words in the command buffer (the library's own wrap-around callback).
static bool Reserve(u32 count) {
	if (g_context->current + count > g_context->end)
		return rsxContextCallback(g_context, count) == 0;
	return true;
}

void CopyRows(Mem srcMem, uint32_t srcOffset, uint32_t srcPitch, Mem dstMem, uint32_t dstOffset, uint32_t dstPitch,
	uint32_t bytesPerRow, uint32_t rows) {
	// Not rsxSetTransferData: PSL1GHT's version compares its unsigned pitch against a negative
	// limit, so it always takes its per-row path, where "while (--rowCount >= 0)" on an unsigned
	// count never ends. It fills the command buffer forever - the Frontend 06 freeze.
	if (!bytesPerRow || !rows) return;
	if (srcPitch == bytesPerRow && dstPitch == bytesPerRow && (bytesPerRow * rows) % 4096 == 0) {
		// Contiguous: copy as 4 KB lines, which keeps pitches inside the engine's limits.
		rows = bytesPerRow * rows / 4096;
		bytesPerRow = srcPitch = dstPitch = 4096;
	}
	if (srcPitch >= 32768 || dstPitch >= 32768) {
		// Too wide for one transfer: one line at a time.
		for (uint32_t i = 0; i < rows; ++i)
			CopyRows(srcMem, srcOffset + i * srcPitch, bytesPerRow, dstMem, dstOffset + i * dstPitch, bytesPerRow, bytesPerRow, 1);
		return;
	}
	if (!Reserve(3)) return;
	u32 *cmd = g_context->current;
	cmd[0] = (2u << 18) | (1u << 13) | NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN;
	cmd[1] = srcMem == Mem::Main ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
	cmd[2] = dstMem == Mem::Main ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
	g_context->current += 3;
	const uint32_t MAX_ROWS = 0x7FF;
	while (rows > 0) {
		const uint32_t n = rows > MAX_ROWS ? MAX_ROWS : rows;
		if (!Reserve(9)) return;
		cmd = g_context->current;
		cmd[0] = (8u << 18) | (1u << 13) | NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN;
		cmd[1] = srcOffset;
		cmd[2] = dstOffset;
		cmd[3] = srcPitch;
		cmd[4] = dstPitch;
		cmd[5] = bytesPerRow;
		cmd[6] = n;
		cmd[7] = (1u << 8) | 1u;  // Byte formats in/out.
		cmd[8] = 0;               // Buffer notify.
		g_context->current += 9;
		srcOffset += n * srcPitch;
		dstOffset += n * dstPitch;
		rows -= n;
	}
}

// ---------------------------------------------------------------------------------------------
// Display

static int g_displayW = 0, g_displayH = 0;
static uint32_t g_displayPitch = 0;
static void *g_displayBuffer[2]{};
static uint32_t g_displayOffset[2]{};
static int g_back = 0;
static bool g_firstFlip = true;
static constexpr int DISPLAY_ID_BASE = 2;

bool InitDisplay() {
	if (g_displayBuffer[0]) return true;
	videoState state{};
	videoResolution res{};
	if (videoGetState(0, 0, &state) != 0 || videoGetResolution(state.displayMode.resolution, &res) != 0) {
		PS3Frontend::Trace("RSX: could not read the video mode");
		return false;
	}
	g_displayW = res.width;
	g_displayH = res.height;
	g_displayPitch = g_displayW * 4;
	for (int i = 0; i < 2; ++i) {
		g_displayBuffer[i] = LocalAlloc(g_displayPitch * g_displayH, 64);
		if (!g_displayBuffer[i]) {
			PS3Frontend::Trace("RSX: out of local memory for display buffers");
			return false;
		}
		memset(g_displayBuffer[i], 0, g_displayPitch * g_displayH);
		g_displayOffset[i] = LocalOffset(g_displayBuffer[i]);
		gcmSetDisplayBuffer(DISPLAY_ID_BASE + i, g_displayOffset[i], g_displayPitch, g_displayW, g_displayH);
	}
	g_back = 0;
	g_firstFlip = true;
	PS3Frontend::Trace("RSX display buffers: %dx%d", g_displayW, g_displayH);
	return true;
}

int DisplayWidth() { return g_displayW; }
int DisplayHeight() { return g_displayH; }
uint32_t BackBufferOffset() { return g_displayOffset[g_back]; }
void *BackBufferPtr() { return g_displayBuffer[g_back]; }
bool WaitFlip(double timeoutSeconds) {
	if (g_firstFlip)
		return true;
	const double start = NowSeconds();
	while (gcmGetFlipStatus() != 0) {
		if (NowSeconds() - start > timeoutSeconds)
			return false;
		usleep(200);
	}
	return true;
}
uint32_t DisplayPitch() { return g_displayPitch; }

static double g_flipWait = 0.0;
double TakeFlipWait() {
	const double w = g_flipWait;
	g_flipWait = 0.0;
	return w;
}

void Present() {
	if (!g_firstFlip) {
		// The previous flip must have happened before we queue another; otherwise we'd draw into a
		// buffer that's still on screen. Waits when we're producing frames faster than vsync, or
		// when the RSX is still drawing the previous frame (then it's the bottleneck).
		double start = NowSeconds();
		while (gcmGetFlipStatus() != 0) {
			if (NowSeconds() - start > 0.5) break;
			usleep(100);
		}
		g_flipWait += NowSeconds() - start;
	}
	gcmResetFlipStatus();
	gcmSetFlip(g_context, DISPLAY_ID_BASE + g_back);
	Flush();
	// No wait for the flip here (until Frontend 89 there was): it held all of the next frame's
	// drawing, which goes into the game's own framebuffers, until the next vblank. Only drawing
	// into the new back buffer - still on screen until this flip - waits (GpuWaitFlip).
	g_back ^= 1;
	g_firstFlip = false;
}

void GpuWaitFlip() {
	if (!g_firstFlip)
		gcmSetWaitFlip(g_context);
}

void ResetDisplay() {
	// Let the last flip complete so SDL's own flip logic starts from a clean status.
	double start = NowSeconds();
	while (!g_firstFlip && gcmGetFlipStatus() != 0 && NowSeconds() - start < 0.5)
		usleep(100);
	Finish();
	g_firstFlip = true;
}

}  // namespace RSX
