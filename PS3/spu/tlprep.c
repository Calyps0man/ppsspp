// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// SPU program: direct T&L job preparation (GPU/Software/TLPrepKernel.h) for the GE worker
// (GPU/Software/TLPrepSpu.cpp). Jobs come through a ring of TLKJob slots in main memory (address
// and slot count are the thread arguments): job number c (1, 2, ...) is in slot (c - 1) % count
// with seq == c. The SPU polls for the next one, runs it, and writes the results block back
// after its output. A job with quit set ends the thread.
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <stdint.h>
#include <string.h>

#define TAG 2

static void Get(void *ls, uint64_t ea, uint32_t size) {
	uint8_t *p = (uint8_t *)ls;
	while (size) {
		uint32_t n = size > 16384 ? 16384 : size;
		mfc_get(p, ea, n, TAG, 0, 0);
		p += n; ea += n; size -= n;
	}
}

static void Put(void *ls, uint64_t ea, uint32_t size) {
	uint8_t *p = (uint8_t *)ls;
	while (size) {
		uint32_t n = size > 16384 ? 16384 : size;
		mfc_put(p, ea, n, TAG, 0, 0);
		p += n; ea += n; size -= n;
	}
}

static void Sync(void) {
	mfc_write_tag_mask(1 << TAG);
	mfc_read_tag_status_all();
}

#define TLK_GET(ls, ea, len) Get((ls), (ea), (len))
#define TLK_PUT(ls, ea, len) Put((ls), (ea), (len))
#define TLK_SYNC() Sync()
#include "../../GPU/Software/TLPrepKernel.h"

static TLKJob job __attribute__((aligned(128)));
static TLKState state __attribute__((aligned(128)));
static uint32_t header[8] __attribute__((aligned(128)));
static uint32_t results[4] __attribute__((aligned(128)));

// Several SPUs share the ring: SPU k of n takes jobs k + 1, k + 1 + n, ... Argument 3 is
// (n << 32) | k: the startup code (crt1) passes only three arguments to main.
int main(uint64_t ringEa, uint64_t slots, uint64_t arg3) {
	const uint32_t step = (uint32_t)(arg3 >> 32) ? (uint32_t)(arg3 >> 32) : 1;
	uint32_t c = 1 + (uint32_t)arg3;
	state.lastNormal[0] = 0.0f;
	state.lastNormal[1] = 0.0f;
	state.lastNormal[2] = 1.0f;
	for (;;) {
		const uint64_t slotEa = ringEa + ((c - 1) % (uint32_t)slots) * sizeof(TLKJob);
		// Wait for job c: the header (results and request words) until seq matches.
		for (;;) {
			Get(header, slotEa, 32);
			Sync();
			if (header[4] == c)
				break;
			// About a microsecond between looks.
			for (volatile int d = 0; d < 400; ++d) {
			}
		}
		Get(&job, slotEa, sizeof(job));
		Sync();
		if (job.quit)
			break;
		const uint32_t err = tlk_run(&state, &job);
		results[0] = c;
		results[1] = err ? 0 : state.outVerts;
		results[2] = err ? 0 : state.outIdx;
		results[3] = err;
		// Fenced: lands after the output above.
		mfc_putf(results, slotEa, 16, TAG, 0, 0);
		Sync();
		c += step;
	}
	spu_thread_exit(0);
	return 0;
}
