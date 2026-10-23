// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Texture hashing and decoding on SPUs for the RSX renderer (kernel: TexKernel.h, program:
// PS3/spu/texspu.c). Calls wait for the result; on any failure they return false and the
// renderer does the work on the PPU (after a timeout the SPUs aren't used again). In the host
// simulation build (PS3/hostsim) the kernel runs on the calling thread.
#pragma once
#include <cstdint>

namespace TexSpu {

void SetEnabled(bool enabled);
bool Available();
// Hash of [p, p + bytes), bytes a multiple of 16, p 16-byte aligned. Its own hash function: only
// compare it with itself.
bool Hash(const void *p, uint32_t bytes, uint64_t *out);

struct DecodeRequest {
	const void *src;      // 16-byte aligned.
	uint32_t rowBytes;    // A multiple of 16, up to 4096.
	uint32_t fmt;         // GETextureFormat, CLUT32 at most.
	bool swizzle;
	uint32_t w, h;        // w a multiple of 4, up to 1024.
	void *dst;            // RSX copy, 16-byte aligned.
	uint32_t dstPitch;    // Bytes.
	const uint32_t *lut;  // CLUT formats: 256 words (CLUT4/8, already through the index transform) or 512.
	bool clutTransform;   // CLUT16/32: index through shift/mask/offset.
	uint32_t clutShift, clutMask, clutOffset;
};
bool Decode(const DecodeRequest &r);
const char *Description();
uint64_t Jobs();
// Stops the SPUs (end of a game). The next call starts them again.
void Shutdown();

}  // namespace TexSpu
