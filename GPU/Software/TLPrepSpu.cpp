// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#include "ppsspp_config.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include "Common/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/TimeUtil.h"
#include "GPU/Software/TLPrepSpu.h"

#if PPSSPP_PLATFORM(PS3)
// The kernel only runs on the SPU here; these are never used on the PPU.
#define TLK_GET(ls, ea, len) memcpy((ls), (const void *)(uintptr_t)(ea), (len))
#define TLK_PUT(ls, ea, len) memcpy((void *)(uintptr_t)(ea), (ls), (len))
#define TLK_SYNC()
#include <sys/spu.h>
#include "PS3/spu/TLPrepSpu.inc"
#else
// Test build: tiny windows and chunks, so the fuzz crosses every boundary.
#define TLK_WINDOW 256
#define TLK_VCHUNK 8
#define TLK_ICHUNK 16
#define TLK_GET(ls, ea, len) memcpy((ls), (const void *)(uintptr_t)(ea), (len))
#define TLK_PUT(ls, ea, len) memcpy((void *)(uintptr_t)(ea), (ls), (len))
#define TLK_SYNC()
#endif
#include "GPU/Software/TLPrepKernel.h"
#include "GPU/Software/HardwareBackend.h"

static_assert(sizeof(TLKVertex) == sizeof(Rasterizer::TLVertex), "T&L vertex layouts");
static_assert(offsetof(TLKVertex, c) == offsetof(Rasterizer::TLVertex, c), "T&L vertex layouts");
static_assert(offsetof(TLKVertex, u) == offsetof(Rasterizer::TLVertex, u), "T&L vertex layouts");
static_assert(sizeof(TLKJob) % 128 == 0, "TLKJob size");

namespace TLPrep {

namespace {

constexpr int SLOTS = 16;
// SPUs preparing jobs in parallel (the sound mixer has one more; a game gets six).
constexpr int SPUS = 3;

enum class State { UNTRIED, READY, FAILED };
State g_state = State::UNTRIED;
char g_description[96] = "PPU";

TLKJob *g_ring = nullptr;  // SLOTS jobs, 128-byte aligned.
struct SlotBuffers {
	uint8_t *verts = nullptr;
	uint32_t vertsCap = 0;  // Bytes.
	uint16_t *idx = nullptr;
	uint32_t idxCap = 0;    // Entries.
};
SlotBuffers g_buffers[SLOTS];
uint32_t g_next = 1;       // Next job number.
uint32_t g_finished = 0;   // Jobs up to this one are finished.
std::atomic<uint64_t> g_jobsDone{0};

#if PPSSPP_PLATFORM(PS3)
sysSpuImage g_image;
sys_spu_group_t g_group = 0;
sys_spu_thread_t g_thread[SPUS];
const char g_groupName[] = "ppsspp_tlprep";
const char g_threadName[] = "ppsspp_tlprep";

void Fail(const char *what, int result) {
	snprintf(g_description, sizeof(g_description), "PPU; SPU %s failed: %08x", what, (unsigned)result);
	ERROR_LOG(Log::G3D, "SPU T&L prep: %s failed: %08x", what, (unsigned)result);
	g_state = State::FAILED;
}
#else
TLKState *g_testState = nullptr;
#endif

bool Init() {
	g_state = State::FAILED;
	if (!g_ring)
		g_ring = (TLKJob *)AllocateAlignedMemory(sizeof(TLKJob) * SLOTS, 128);
	if (!g_ring)
		return false;
	memset((void *)g_ring, 0, sizeof(TLKJob) * SLOTS);
#if PPSSPP_PLATFORM(PS3)
	sysSpuInitialize(6, 0);  // Already done by the sound mixer, most likely; harmless then.
	int r = sysSpuImageImport(&g_image, kTLPrepSpuElf, 0);
	if (r != 0) { Fail("image import", r); return false; }
	sysSpuThreadGroupAttribute grpattr = { sizeof(g_groupName), g_groupName, 0, { 0 } };
	r = sysSpuThreadGroupCreate(&g_group, SPUS, 100, &grpattr);
	if (r != 0) { Fail("group create", r); return false; }
	sysSpuThreadAttribute tattr = { g_threadName, sizeof(g_threadName), SPU_THREAD_ATTR_NONE };
	for (int k = 0; k < SPUS; ++k) {
		sysSpuThreadArgument arg = { (u64)(uintptr_t)g_ring, (u64)SLOTS, ((u64)SPUS << 32) | (u64)k, 0 };  // SPU main gets 3 arguments.
		r = sysSpuThreadInitialize(&g_thread[k], g_group, k, &g_image, &tattr, &arg);
		if (r != 0) { Fail("thread init", r); return false; }
	}
	r = sysSpuThreadGroupStart(g_group);
	if (r != 0) { Fail("group start", r); return false; }
	snprintf(g_description, sizeof(g_description), "%d SPUs", SPUS);
	INFO_LOG(Log::G3D, "SPU T&L prep started");
#else
	if (!getenv("PPSSPP_TLK"))
		return false;
	g_testState = (TLKState *)AllocateAlignedMemory(sizeof(TLKState), 128);
	if (!g_testState)
		return false;
	g_testState->lastNormal[0] = 0.0f;
	g_testState->lastNormal[1] = 0.0f;
	g_testState->lastNormal[2] = 1.0f;
	snprintf(g_description, sizeof(g_description), "PPU kernel (test)");
#endif
	g_state = State::READY;
	return true;
}

bool Grow(SlotBuffers &b, uint32_t vertBytes, uint32_t idxCount) {
	if (b.vertsCap < vertBytes) {
		if (b.verts)
			FreeAlignedMemory(b.verts);
		b.vertsCap = vertBytes + 4096;
		b.verts = (uint8_t *)AllocateAlignedMemory(b.vertsCap, 128);
		if (!b.verts) {
			b.vertsCap = 0;
			return false;
		}
	}
	if (b.idxCap < idxCount) {
		if (b.idx)
			FreeAlignedMemory(b.idx);
		b.idxCap = idxCount + 2048;
		b.idx = (uint16_t *)AllocateAlignedMemory(b.idxCap * 2, 128);
		if (!b.idx) {
			b.idxCap = 0;
			return false;
		}
	}
	return true;
}

}  // namespace

void SetEnabled(bool enabled) {
	if (!enabled && g_state == State::UNTRIED) {
		g_state = State::FAILED;
		snprintf(g_description, sizeof(g_description), "PPU (spuvertex=0)");
	}
}

bool Available() {
	if (g_state == State::UNTRIED)
		Init();
	return g_state == State::READY;
}

uint32_t Submit(const Request &r) {
	if (!Available())
		return 0;
	// The slot is free once the job SLOTS before this one is finished.
	if (g_next - g_finished > (uint32_t)SLOTS)
		return 0;
	if (((uintptr_t)r.raw & 15) != 0 || r.vertexSize == 0 || r.nw > 8)
		return 0;
	const uint32_t rawCount = r.rawBytes / r.vertexSize;
	if (rawCount == 0 || rawCount > TLK_MAX_RAW)
		return 0;
	// Triangles out: at most idxCount * 3 indices (strips and fans), vertices at most idxCount.
	const uint32_t maxIdx = r.idxCount * 3 + 16;
	const uint32_t maxVerts = r.idxCount + 8;
	const uint32_t c = g_next;
	const int slot = (int)((c - 1) % SLOTS);
	SlotBuffers &b = g_buffers[slot];
	if (!Grow(b, maxVerts * sizeof(TLKVertex), maxIdx))
		return 0;
	TLKJob &j = g_ring[slot];
	j.prim = r.prim;
	j.idxCount = r.idxCount;
	j.quit = 0;
	j.rawEa = (uint64_t)(uintptr_t)r.raw;
	j.idxEa = (uint64_t)(uintptr_t)r.idx;
	j.outVertsEa = (uint64_t)(uintptr_t)b.verts;
	j.outIdxEa = (uint64_t)(uintptr_t)b.idx;
	j.rawBytes = r.rawBytes;
	j.rawCount = rawCount;
	j.size = r.vertexSize;
	j.wt = r.wt; j.nw = r.nw; j.tc = r.tc; j.col = r.col; j.nrm = r.nrm; j.pos = r.pos; j.readUV = r.readUV;
	j.woff = r.woff; j.tcoff = r.tcoff; j.coloff = r.coloff; j.nrmoff = r.nrmoff; j.posoff = r.posoff;
	j.nsign = r.nsign;
	j.fixedNormal = r.fixedNormal;
	j.matAmbient = r.matAmbient;
	memcpy(j.bones, r.bones, sizeof(j.bones));
	g_next++;
#if PPSSPP_PLATFORM(PS3)
	// Everything above (and the vertex and index data) before the sequence number.
	__asm__ volatile("lwsync" ::: "memory");
	j.seq = c;
#else
	j.seq = c;
	const uint32_t err = tlk_run(g_testState, &j);
	j.outVerts = err ? 0 : g_testState->outVerts;
	j.outIdx = err ? 0 : g_testState->outIdx;
	j.error = err;
	j.done = c;
#endif
	return c;
}

bool Wait(uint32_t c, const void **verts, int *nVerts, const uint16_t **idx, int *nIdx) {
	if (g_state != State::READY)
		return false;
	const int slot = (int)((c - 1) % SLOTS);
	TLKJob &j = g_ring[slot];
	volatile uint32_t *done = &j.done;
	if (*done != c) {
		const double start = time_now_d();
		int spins = 0;
		while (*done != c) {
			// A job takes the SPU tens to hundreds of microseconds. Yielding the PPU after 200
			// spins gave the rest of the GE worker's time slice to another thread on every wait
			// that ran a little long; keep spinning for the first 3 ms instead.
			if (++spins > 200 && (spins & 255) == 0) {
				const double waited = time_now_d() - start;
				if (waited > 0.003)
					std::this_thread::yield();
				if (waited > 1.0) {
#if PPSSPP_PLATFORM(PS3)
					// No answer in a second: the SPU thread is gone or stuck. Stop it, so it
					// can't write into buffers we reuse, and decode on the PPU from now on.
					sysSpuThreadGroupTerminate(g_group, 0);
					Fail("job (no answer)", (int)c);
#endif
					g_state = State::FAILED;
					return false;
				}
			}
		}
	}
#if PPSSPP_PLATFORM(PS3)
	__asm__ volatile("lwsync" ::: "memory");
#endif
	if (j.error)
		return false;
	*verts = g_buffers[slot].verts;
	*nVerts = (int)j.outVerts;
	*idx = g_buffers[slot].idx;
	*nIdx = (int)j.outIdx;
	g_jobsDone++;
	return true;
}

void Finish(uint32_t c) {
	if (c > g_finished)
		g_finished = c;
}

const char *Description() {
	return g_description;
}

uint64_t JobsDone() {
	return g_jobsDone.load();
}

void Shutdown() {
	if (g_state == State::UNTRIED)
		return;
#if PPSSPP_PLATFORM(PS3)
	if (g_state == State::READY || g_group) {
		u32 cause = 0, status = 0;
		sysSpuThreadGroupTerminate(g_group, 0);
		sysSpuThreadGroupJoin(g_group, &cause, &status);
		sysSpuThreadGroupDestroy(g_group);
		sysSpuImageClose(&g_image);
		g_group = 0;
	}
#endif
	// Every slot free again for a fresh start (the SPUs count jobs from 1).
	if (g_ring)
		memset((void *)g_ring, 0, sizeof(TLKJob) * SLOTS);
	g_next = 1;
	g_finished = 0;
	if (g_state == State::READY) {
		g_state = State::UNTRIED;
		snprintf(g_description, sizeof(g_description), "PPU");
	}
}

}  // namespace TLPrep
