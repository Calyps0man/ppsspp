// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Direct T&L job preparation on an SPU (kernel: TLPrepKernel.h, program: PS3/spu/tlprep.c).
// The GE worker submits jobs ahead of the one it is drawing, in queue order, and collects the
// results in the same order. In the test build PPSSPP_TLK=1 runs the kernel on the PPU at submit.
#pragma once
#include <cstdint>

namespace TLPrep {

struct Request {
	const uint8_t *raw;
	uint32_t rawBytes;
	uint32_t vertexSize;
	const uint16_t *idx;
	uint32_t idxCount;
	uint32_t prim;  // TLK_PRIM_*
	uint8_t wt, nw, tc, col, nrm, pos, readUV, fixedNormal;
	uint8_t woff, tcoff, coloff, nrmoff, posoff;
	float nsign;
	uint32_t matAmbient;
	const float *bones;  // 96 floats.
};

// Before the first job (frontend setting "spuvertex").
void SetEnabled(bool enabled);
bool Available();
// Job number (> 0), or 0 when there's no free slot or the request doesn't fit.
uint32_t Submit(const Request &r);
// Waits for job c. False: the PPU has to do it (and the SPU path may now be off).
bool Wait(uint32_t c, const void **verts, int *nVerts, const uint16_t **idx, int *nIdx);
// Job c's output is no longer needed (call for every submitted job, in order).
void Finish(uint32_t c);
const char *Description();
uint64_t JobsDone();
// Stops the SPUs (end of a game). The next job starts them again.
void Shutdown();

}  // namespace TLPrep
