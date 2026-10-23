// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#include "ppsspp_config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "Common/MemoryUtil.h"
#include "PS3/rsx/TexSpu.h"
#include "PS3/frontend/Frontend.h"

#if PPSSPP_PLATFORM(PS3)
#define TEXSPU_HW 1
#include <sys/spu.h>
#include <sys/systime.h>
#include "PS3/spu/TexSpu.inc"
// The kernel itself only runs on the SPUs.
#define TK_GET(ls, ea, len, tag) memcpy((ls), (const void *)(uintptr_t)(ea), (len))
#define TK_PUT(ls, ea, len, tag) memcpy((void *)(uintptr_t)(ea), (ls), (len))
#define TK_WAIT(tag)
#else
#define TEXSPU_HW 0
#define TK_GET(ls, ea, len, tag) memcpy((ls), (const void *)(uintptr_t)(ea), (len))
#define TK_PUT(ls, ea, len, tag) memcpy((void *)(uintptr_t)(ea), (ls), (len))
#define TK_WAIT(tag)
#endif
#include "PS3/rsx/TexKernel.h"

static_assert(sizeof(TKJob) == 128, "TKJob size");

namespace TexSpu {

namespace {

constexpr int SPUS = 2;
enum class State { UNTRIED, READY, FAILED };
State g_state = State::UNTRIED;
bool g_enabled = true;
char g_description[96] = "PPU";
TKJob *g_slots = nullptr;      // One per SPU, 128-byte aligned.
uint32_t g_seq[SPUS] = {};
uint32_t *g_lut = nullptr;     // 512 words, 128-byte aligned.
uint64_t g_jobs = 0;

#if TEXSPU_HW
sysSpuImage g_image;
sys_spu_group_t g_group = 0;
sys_spu_thread_t g_thread[SPUS];
const char g_groupName[] = "ppsspp_tex";
const char g_threadName[] = "ppsspp_tex";

void Fail(const char *what, int result) {
	snprintf(g_description, sizeof(g_description), "PPU; SPU %s failed: %08x", what, (unsigned)result);
	PS3Frontend::Trace("SPU textures: %s failed: %08x", what, (unsigned)result);
	g_state = State::FAILED;
}
#else
TKState *g_hostState = nullptr;
#endif

void *AlignedAlloc(size_t bytes) {
	void *p = AllocateAlignedMemory(bytes, 128);
	if (p)
		memset(p, 0, bytes);
	return p;
}

bool Init() {
	g_state = State::FAILED;
	if (!g_enabled) {
		snprintf(g_description, sizeof(g_description), "PPU (off)");
		return false;
	}
	if (!g_slots)
		g_slots = (TKJob *)AlignedAlloc(sizeof(TKJob) * SPUS);
	if (!g_lut)
		g_lut = (uint32_t *)AlignedAlloc(512 * 4);
	if (!g_slots || !g_lut)
		return false;
#if TEXSPU_HW
	sysSpuInitialize(6, 0);
	int r = sysSpuImageImport(&g_image, kTexSpuElf, 0);
	if (r != 0) { Fail("image import", r); return false; }
	sysSpuThreadGroupAttribute grpattr = { sizeof(g_groupName), g_groupName, 0, { 0 } };
	r = sysSpuThreadGroupCreate(&g_group, SPUS, 100, &grpattr);
	if (r != 0) { Fail("group create", r); return false; }
	sysSpuThreadAttribute tattr = { g_threadName, sizeof(g_threadName), SPU_THREAD_ATTR_NONE };
	for (int k = 0; k < SPUS; ++k) {
		sysSpuThreadArgument arg = { (u64)(uintptr_t)&g_slots[k], 0, 0, 0 };
		r = sysSpuThreadInitialize(&g_thread[k], g_group, k, &g_image, &tattr, &arg);
		if (r != 0) { Fail("thread init", r); return false; }
	}
	r = sysSpuThreadGroupStart(g_group);
	if (r != 0) { Fail("group start", r); return false; }
	snprintf(g_description, sizeof(g_description), "%d SPUs", SPUS);
	PS3Frontend::Trace("SPU textures: started on %d SPUs", SPUS);
#else
	if (!getenv("PPSSPP_TEXK"))
		return false;
	g_hostState = (TKState *)AlignedAlloc(sizeof(TKState));
	if (!g_hostState)
		return false;
	snprintf(g_description, sizeof(g_description), "host kernel (test)");
#endif
	g_state = State::READY;
	return true;
}

// Runs the filled-in slots 0..parts-1 and waits for them.
bool Run(int parts) {
#if TEXSPU_HW
	for (int k = 0; k < parts; ++k) {
		TKJob &j = g_slots[k];
		j.part = (uint32_t)k;
		j.parts = (uint32_t)parts;
		j.done = 0;
		__asm__ volatile("lwsync" ::: "memory");
		j.seq = ++g_seq[k];
	}
	for (int k = 0; k < parts; ++k) {
		volatile uint32_t *done = &g_slots[k].done;
		int spins = 0;
		uint64_t start = 0;
		while (*done != g_seq[k]) {
			if (++spins == 2000)
				start = sysGetSystemTime();
			if (spins > 2000 && (spins & 255) == 0) {
				// A big texture takes the SPUs a few hundred microseconds. Yielding the PPU here
				// gave the rest of its time slice to another thread, and the decode took a
				// millisecond or more; keep spinning for the first 3 ms.
				const uint64_t waited = sysGetSystemTime() - start;
				if (waited > 3000)
					std::this_thread::yield();
				if (waited > 1000000) {
					// No answer in a second: stop the SPUs so they can't write into memory we reuse.
					sysSpuThreadGroupTerminate(g_group, 0);
					Fail("job (no answer)", (int)g_seq[k]);
					return false;
				}
			}
		}
	}
	__asm__ volatile("lwsync" ::: "memory");
	bool ok = true;
	for (int k = 0; k < parts; ++k)
		ok = ok && g_slots[k].error == 0;
	g_jobs++;
	return ok;
#else
	bool ok = true;
	for (int k = 0; k < parts; ++k) {
		TKJob &j = g_slots[k];
		j.part = (uint32_t)k;
		j.parts = (uint32_t)parts;
		j.error = 0;
		if (j.op == TK_OP_HASH)
			j.hash = tk_hash(g_hostState, &j);
		else
			j.error = tk_decode(g_hostState, &j);
		ok = ok && j.error == 0;
	}
	g_jobs++;
	return ok;
#endif
}

}  // namespace

void SetEnabled(bool enabled) {
	g_enabled = enabled;
}

bool Available() {
	if (g_state == State::UNTRIED)
		Init();
	return g_state == State::READY;
}

bool Hash(const void *p, uint32_t bytes, uint64_t *out) {
	if (!Available() || ((uintptr_t)p & 15) || (bytes & 15) || bytes == 0)
		return false;
	const int parts = bytes >= 2 * TK_HCHUNK ? SPUS : 1;
	for (int k = 0; k < parts; ++k) {
		TKJob &j = g_slots[k];
		j.op = TK_OP_HASH;
		j.srcEa = (uint64_t)(uintptr_t)p;
		j.bytes = bytes;
	}
	if (!Run(parts))
		return false;
	uint64_t h = g_slots[0].hash;
	for (int k = 1; k < parts; ++k)
		h = ((h << 23) | (h >> 41)) * 0x9E3779B97F4A7C15ull ^ g_slots[k].hash;
	*out = h;
	return true;
}

bool Decode(const DecodeRequest &r) {
	if (!Available() || ((uintptr_t)r.src & 15) || ((uintptr_t)r.dst & 15) || (r.rowBytes & 15) || r.rowBytes > TK_MAX_ROWBYTES ||
		(r.w & 3) || r.w == 0 || r.w > TK_MAX_W || r.h == 0 || (r.dstPitch & 15) || r.fmt > TK_FMT_CLUT32)
		return false;
	if (r.fmt >= TK_FMT_CLUT4) {
		if (!r.lut)
			return false;
		memcpy(g_lut, r.lut, (r.fmt == TK_FMT_CLUT4 || r.fmt == TK_FMT_CLUT8) ? 256 * 4 : 512 * 4);
	}
	const uint32_t bands = (r.h + TK_BAND - 1) / TK_BAND;
	const int parts = bands >= 4 ? SPUS : 1;
	for (int k = 0; k < parts; ++k) {
		TKJob &j = g_slots[k];
		j.op = TK_OP_DECODE;
		j.fmt = r.fmt;
		j.swizzle = r.swizzle ? 1 : 0;
		j.srcEa = (uint64_t)(uintptr_t)r.src;
		j.dstEa = (uint64_t)(uintptr_t)r.dst;
		j.lutEa = (uint64_t)(uintptr_t)g_lut;
		j.rowBytes = r.rowBytes;
		j.w = r.w;
		j.h = r.h;
		j.dstPitch = r.dstPitch;
		j.clutMode = r.clutTransform ? 1 : 0;
		j.clutShift = r.clutShift;
		j.clutMask = r.clutMask;
		j.clutOffset = r.clutOffset;
	}
	return Run(parts);
}

const char *Description() {
	return g_description;
}

uint64_t Jobs() {
	return g_jobs;
}

void Shutdown() {
#if TEXSPU_HW
	if (g_group) {
		u32 cause = 0, status = 0;
		sysSpuThreadGroupTerminate(g_group, 0);
		sysSpuThreadGroupJoin(g_group, &cause, &status);
		sysSpuThreadGroupDestroy(g_group);
		sysSpuImageClose(&g_image);
		g_group = 0;
	}
#endif
	for (int k = 0; k < SPUS; ++k)
		g_seq[k] = 0;
	if (g_slots)
		memset((void *)g_slots, 0, sizeof(TKJob) * SPUS);
	if (g_state == State::READY) {
		g_state = State::UNTRIED;
		snprintf(g_description, sizeof(g_description), "PPU");
	}
}

}  // namespace TexSpu
