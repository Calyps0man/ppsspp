// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Runs the sceSas voice mixer (SasMixKernel.h) on an SPU on the PS3 (program: PS3/spu/sasmix.c).
// Called from the SAS thread only; it blocks on an event queue while the SPU works, so the PPU
// is free for the emulation thread.

#include "ppsspp_config.h"
#include <cstdint>
#include <cstring>
#include <cstdio>

static inline const void *SasPpuMap(uint64_t ea) { return (const void *)(uintptr_t)ea; }
#define SASK_MAP(ctx, ea, len) SasPpuMap(ea)
#include "Core/HW/SasSpu.h"

#if PPSSPP_PLATFORM(PS3)
#include <sys/spu.h>
#include <sys/event_queue.h>
#include "Common/Log.h"
#include "PS3/spu/SasMixSpu.inc"

#define SASMIX_SPUP 12

// The PS3 "Sound mixing" setting (on the emulation thread): poll the SPU's done word.
bool g_sasSpuPoll = false;

namespace {
enum class SpuState { UNTRIED, READY, FAILED };
SpuState g_state = SpuState::UNTRIED;
char g_description[96] = "PPU";
sysSpuImage g_image;
sys_spu_group_t g_group = 0;
sys_spu_thread_t g_thread;
sys_event_queue_t g_queue;
uint32_t g_seq = 0;
SasKJob g_job __attribute__((aligned(128)));
int32_t g_spuMix[SASK_MAX_GRAIN * 2] __attribute__((aligned(128)));
int32_t g_spuSend[SASK_MAX_GRAIN * 2] __attribute__((aligned(128)));
// With g_sasSpuPoll: the SPU writes the job's seq here when done, and the PPU polls it instead of
// sleeping on the event queue (each sleep and wake-up through lv2 took ~0.3 ms per mix with the
// PPU busy; Midnight Club mixes ~60 times a frame).
volatile uint32_t g_spuDone[32] __attribute__((aligned(128)));
const char g_groupName[] = "ppsspp_sas";
const char g_threadName[] = "ppsspp_sasmix";

void Fail(const char *what, int result) {
	snprintf(g_description, sizeof(g_description), "PPU; SPU %s failed: %08x", what, (unsigned)result);
	ERROR_LOG(Log::SasMix, "SPU sceSas mixer: %s failed: %08x", what, (unsigned)result);
	g_state = SpuState::FAILED;
}

bool InitSpu() {
	g_state = SpuState::FAILED;
	// The process may already have done this (it can only be done once); a failure here is fine.
	sysSpuInitialize(6, 0);
	int r = sysSpuImageImport(&g_image, kSasMixSpuElf, 0);
	if (r != 0) { Fail("image import", r); return false; }
	sysSpuThreadGroupAttribute grpattr = { sizeof(g_groupName), g_groupName, 0, { 0 } };
	r = sysSpuThreadGroupCreate(&g_group, 1, 100, &grpattr);
	if (r != 0) { Fail("group create", r); return false; }
	sys_event_queue_attr_t qattr = { SYS_EVENT_QUEUE_FIFO, SYS_EVENT_QUEUE_PPU, "sasmixq" };
	r = sysEventQueueCreate(&g_queue, &qattr, SYS_EVENT_QUEUE_KEY_LOCAL, 4);
	if (r != 0) { Fail("event queue create", r); return false; }
	sysSpuThreadAttribute tattr = { g_threadName, sizeof(g_threadName), SPU_THREAD_ATTR_NONE };
	sysSpuThreadArgument arg = { (u64)(uintptr_t)&g_job, 0, 0, 0 };
	r = sysSpuThreadInitialize(&g_thread, g_group, 0, &g_image, &tattr, &arg);
	if (r != 0) { Fail("thread init", r); return false; }
	r = sysSpuThreadSetConfiguration(g_thread, SPU_SIGNAL1_OVERWRITE | SPU_SIGNAL2_OVERWRITE);
	if (r != 0) { Fail("thread config", r); return false; }
	r = sysSpuThreadConnectEvent(g_thread, g_queue, SPU_THREAD_EVENT_USER, SASMIX_SPUP);
	if (r != 0) { Fail("event connect", r); return false; }
	r = sysSpuThreadGroupStart(g_group);
	if (r != 0) { Fail("group start", r); return false; }
	g_state = SpuState::READY;
	snprintf(g_description, sizeof(g_description), "SPU");
	INFO_LOG(Log::SasMix, "SPU sceSas mixer started");
	return true;
}

bool RunOnSpu(SasKVoice *voices, int count, int grainSize, int32_t *mix, int32_t *send) {
	if (g_state == SpuState::UNTRIED)
		InitSpu();
	if (g_state != SpuState::READY)
		return false;
	g_job.voicesEa = (uint64_t)(uintptr_t)voices;
	g_job.mixEa = (uint64_t)(uintptr_t)g_spuMix;
	g_job.sendEa = (uint64_t)(uintptr_t)g_spuSend;
	g_job.count = count;
	g_job.grainSize = grainSize;
	g_job.seq = ++g_seq;
	const bool poll = g_sasSpuPoll;
	g_job.doneEa = poll ? (uint64_t)(uintptr_t)g_spuDone : 0;
	__asm__ volatile("sync" ::: "memory");
	int r = sysSpuThreadWriteSignal(g_thread, 0, g_job.seq);
	if (r != 0) { Fail("signal", r); return false; }
	if (poll) {
		uint64_t start, now;
		__asm__ volatile("mftb %0" : "=r"(start));
		bool done = true;
		while (g_spuDone[0] != g_job.seq) {
			__asm__ volatile("or 1,1,1");  // Low thread priority while spinning (the other thread runs).
			__asm__ volatile("mftb %0" : "=r"(now));
			if (now - start > 79800000ULL) {  // A second: the SPU thread is gone.
				done = false;
				break;
			}
		}
		__asm__ volatile("or 2,2,2");  // Back to normal priority.
		if (!done) {
			sysSpuThreadGroupTerminate(g_group, 0);
			Fail("mix (no answer, polled)", 0);
			return false;
		}
		__asm__ volatile("lwsync" ::: "memory");
		const int n = grainSize * 2;
		for (int i = 0; i < n; ++i) {
			mix[i] += g_spuMix[i];
			send[i] += g_spuSend[i];
		}
		return true;
	}
	sys_event_t ev;
	// A mix takes well under a millisecond; a second means the SPU thread is gone.
	r = sysEventQueueReceive(g_queue, &ev, 1000000);
	if (r != 0) {
		// The voices may be half written; terminate so a late SPU can't touch them again.
		sysSpuThreadGroupTerminate(g_group, 0);
		Fail("mix (no answer)", r);
		return false;
	}
	__asm__ volatile("lwsync" ::: "memory");
	const int n = grainSize * 2;
	for (int i = 0; i < n; ++i) {
		mix[i] += g_spuMix[i];
		send[i] += g_spuSend[i];
	}
	return true;
}
}  // namespace

#endif

void SasKernelMix(SasKVoice *voices, int count, int grainSize, int32_t *mix, int32_t *send) {
	if (count <= 0 || grainSize <= 0)
		return;
#if PPSSPP_PLATFORM(PS3)
	if ((grainSize & 1) == 0 && grainSize <= SASK_MAX_GRAIN && RunOnSpu(voices, count, grainSize, mix, send))
		return;
#endif
	static int16_t mixTemp[SASK_TEMP_SAMPLES + 8];
	for (int i = 0; i < count; ++i)
		sask_mixVoice(nullptr, &voices[i], grainSize, mixTemp, mix, send);
}

void SasSpuShutdown() {
#if PPSSPP_PLATFORM(PS3)
	if (g_group) {
		u32 cause = 0, status = 0;
		sysSpuThreadGroupTerminate(g_group, 0);
		sysSpuThreadGroupJoin(g_group, &cause, &status);
		sysSpuThreadGroupDestroy(g_group);
		sysEventQueueDestroy(g_queue, 0);
		sysSpuImageClose(&g_image);
		g_group = 0;
	}
	if (g_state == SpuState::READY) {
		g_state = SpuState::UNTRIED;
		snprintf(g_description, sizeof(g_description), "PPU");
	}
#endif
}

const char *SasMixerDescription() {
#if PPSSPP_PLATFORM(PS3)
	return g_description;
#else
	return "PPU";
#endif
}
