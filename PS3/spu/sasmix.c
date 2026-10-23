// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// SPU program: mixes sceSas voices (Core/HW/SasMixKernel.h) for the PPU (Core/HW/SasSpu.cpp).
// Waits on signal 1 for a job, reads the job record at the address given as the first thread
// argument, mixes, writes the voices and the two mix buffers back, and sends a user event (or,
// with job.doneEa set, writes the job's sequence number there for the PPU to poll).
// Signal value 0xFFFFFFFF ends the thread.
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <sys/spu_event.h>
#include <stdint.h>
#include <string.h>

#define SASMIX_SPUP 12
#define TAG 1
#define WINDOW 4096

static const void *SpuMap(uint64_t ea, uint32_t len);
#define SASK_MAP(ctx, ea, len) SpuMap((ea), (len))
#include "../../Core/HW/SasMixKernel.h"

static SasKJob job __attribute__((aligned(128)));
static SasKVoice voices[SASK_MAX_VOICES] __attribute__((aligned(128)));
static int32_t mix[SASK_MAX_GRAIN * 2] __attribute__((aligned(128)));
static int32_t send[SASK_MAX_GRAIN * 2] __attribute__((aligned(128)));
static int16_t mixTemp[SASK_TEMP_SAMPLES + 8] __attribute__((aligned(128)));
static uint8_t window[WINDOW + 32] __attribute__((aligned(128)));
static uint64_t windowEa, limitEa;
static uint32_t windowLen;

static void WaitDma(void) {
	mfc_write_tag_mask(1 << TAG);
	mfc_read_tag_status_all();
}

// Large transfers in pieces of at most 16 KB (the MFC limit).
static void Get(void *ls, uint64_t ea, uint32_t size) {
	uint8_t *p = (uint8_t *)ls;
	while (size) {
		uint32_t n = size > 16384 ? 16384 : size;
		mfc_get(p, ea, n, TAG, 0, 0);
		p += n; ea += n; size -= n;
	}
	WaitDma();
}

static void Put(void *ls, uint64_t ea, uint32_t size) {
	uint8_t *p = (uint8_t *)ls;
	while (size) {
		uint32_t n = size > 16384 ? 16384 : size;
		mfc_put(p, ea, n, TAG, 0, 0);
		p += n; ea += n; size -= n;
	}
	WaitDma();
}

// A window of guest memory. Never reads past the voice's data (rounded to 16 bytes, which stays
// on the same page), so it can't fault at the end of RAM.
static const void *SpuMap(uint64_t ea, uint32_t len) {
	if (windowLen && ea >= windowEa && ea + len <= windowEa + windowLen)
		return window + (ea - windowEa);
	uint64_t start = ea & ~15ULL;
	uint64_t end = start + WINDOW;
	uint64_t lim = (limitEa + 15) & ~15ULL;
	if (end > lim)
		end = lim;
	uint64_t need = (ea + len + 15) & ~15ULL;
	if (end < need)
		end = need;
	windowEa = start;
	windowLen = (uint32_t)(end - start);
	Get(window, start, windowLen);
	return window + (ea - start);
}

int main(uint64_t jobEa, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
	(void)arg2; (void)arg3; (void)arg4;
	for (;;) {
		uint32_t sig = spu_read_signal1();
		if (sig == 0xFFFFFFFF)
			break;
		Get(&job, jobEa, sizeof(job));
		const uint32_t count = job.count > SASK_MAX_VOICES ? SASK_MAX_VOICES : job.count;
		const int grain = job.grainSize > SASK_MAX_GRAIN ? SASK_MAX_GRAIN : (int)job.grainSize;
		if (count)
			Get(voices, job.voicesEa, count * sizeof(SasKVoice));
		memset(mix, 0, grain * 2 * sizeof(int32_t));
		memset(send, 0, grain * 2 * sizeof(int32_t));
		for (uint32_t i = 0; i < count; ++i) {
			windowLen = 0;
			limitEa = voices[i].dataEa + voices[i].dataBytes;
			sask_mixVoice(0, &voices[i], grain, mixTemp, mix, send);
		}
		if (count)
			Put(voices, job.voicesEa, count * sizeof(SasKVoice));
		Put(mix, job.mixEa, grain * 2 * sizeof(int32_t));
		Put(send, job.sendEa, grain * 2 * sizeof(int32_t));
		if (job.doneEa) {
			// After the writes above have completed (Put waits): the PPU polls this word.
			static uint32_t done[4] __attribute__((aligned(16)));
			done[0] = job.seq;
			Put(done, job.doneEa, 16);
		} else {
			spu_thread_send_event(SASMIX_SPUP, job.seq & 0xFFFFFF, sig);
		}
	}
	spu_thread_exit(0);
	return 0;
}
