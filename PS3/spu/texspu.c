// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// SPU program: texture hashing and decoding for the RSX renderer (PS3/rsx/TexKernel.h, driven by
// PS3/rsx/TexSpu.cpp). Each SPU of the group has its own job slot in main memory (thread argument
// 1); job number c in it has seq == c. It polls for the next one, runs it and writes the
// results block back. TK_OP_QUIT ends the thread.
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <stdint.h>
#include <string.h>

static void Xfer(void *ls, uint64_t ea, uint32_t size, uint32_t tag, int put) {
	uint8_t *p = (uint8_t *)ls;
	while (size) {
		const uint32_t n = size > 16384 ? 16384 : size;
		if (put)
			mfc_put(p, ea, n, tag, 0, 0);
		else
			mfc_get(p, ea, n, tag, 0, 0);
		p += n; ea += n; size -= n;
	}
}

static void Wait(uint32_t tag) {
	mfc_write_tag_mask(1 << tag);
	mfc_read_tag_status_all();
}

#define TK_GET(ls, ea, len, tag) Xfer((ls), (ea), (len), (tag), 0)
#define TK_PUT(ls, ea, len, tag) Xfer((ls), (ea), (len), (tag), 1)
#define TK_WAIT(tag) Wait(tag)
#include "../rsx/TexKernel.h"
#include <stddef.h>

_Static_assert(sizeof(TKJob) == 128, "TKJob size");
_Static_assert(offsetof(TKJob, seq) == 32 && offsetof(TKJob, hash) == 16, "TKJob layout");

static TKJob job __attribute__((aligned(128)));
static TKState state __attribute__((aligned(128)));
static uint32_t header[16] __attribute__((aligned(128)));
static uint32_t results[8] __attribute__((aligned(128)));

int main(uint64_t slotEa, uint64_t arg2, uint64_t arg3) {
	uint32_t c = 1;
	for (;;) {
		// Wait for job c: seq is the first request word (byte 32).
		for (;;) {
			Xfer(header, slotEa, 64, 3, 0);
			Wait(3);
			if (header[8] == c)
				break;
			for (volatile int d = 0; d < 200; ++d) {
			}
		}
		Xfer(&job, slotEa, sizeof(job), 3, 0);
		Wait(3);
		if (job.op == TK_OP_QUIT)
			break;
		uint64_t hash = 0;
		uint32_t err = 0;
		if (job.op == TK_OP_HASH)
			hash = tk_hash(&state, &job);
		else if (job.op == TK_OP_DECODE)
			err = tk_decode(&state, &job);
		else
			err = 2;
		results[0] = c;
		results[1] = err;
		results[2] = 0;
		results[3] = 0;
		results[4] = (uint32_t)(hash >> 32);
		results[5] = (uint32_t)hash;
		results[6] = 0;
		results[7] = 0;
		// Everything this job wrote has landed before the results say it's done.
		Wait(0);
		Wait(1);
		mfc_sync(3);  // Visible to the RSX too (decoded textures go to its local memory).
		Wait(3);
		Xfer(results, slotEa, 32, 3, 1);
		Wait(3);
		c++;
	}
	spu_thread_exit(0);
	return 0;
}
