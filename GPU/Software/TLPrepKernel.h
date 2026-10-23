// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Direct T&L job preparation, shared by the PPU and an SPU (PS3/spu/tlprep.c): the draw's
// triangle list (unique vertices in first-use order, plus 16-bit indices into them), with each
// vertex decoded from the PSP format and skinned. Same results as ExecuteTLJob's
// DecodeTLVertices on an IEEE FPU (the SPU's single precision truncates, so it differs in the
// last bit or so). No morphing.
//
// Plain C. Memory outside the program goes through TLK_GET(ls, ea, len) and
// TLK_PUT(ls, ea, len), both 16-byte aligned with len a multiple of 16 (DMA rules), and
// TLK_SYNC() waits for them. On the PPU they're memcpy.
#pragma once
#include <stdint.h>
#include <string.h>

#define TLK_MAX_RAW 16384          // Vertices in a job's copied range (remap table entries).
#ifndef TLK_WINDOW
#define TLK_WINDOW (32 * 1024)
#endif     // Vertex bytes kept in local memory.
#ifndef TLK_VCHUNK
#define TLK_VCHUNK 448
#endif             // Output vertices per write (448 * 36 is a multiple of 16).
#ifndef TLK_ICHUNK
#define TLK_ICHUNK 4096
#endif            // Indices read / written at a time.
#define TLK_PRIM_TRIANGLES 3
#define TLK_PRIM_STRIP 4
#define TLK_PRIM_FAN 5

typedef struct TLKVertex {
	float x, y, z;
	float nx, ny, nz;
	uint8_t c[4];
	float u, v;
} TLKVertex;

// One job, as the PPU writes it (128-byte aligned). The SPU reads it whole and writes back
// the 16-byte results block at the start.
typedef struct TLKJob {
	// Results (written last by the SPU).
	uint32_t done;        // == seq when the results are ready.
	uint32_t outVerts;    // Unique vertices written.
	uint32_t outIdx;      // Indices written.
	uint32_t error;       // Non-zero: not done, the PPU decodes this one itself.
	// Request.
	uint32_t seq;         // Non-zero, increasing; the SPU starts the job when it sees it.
	uint32_t quit;
	uint32_t prim;
	uint32_t idxCount;    // GE-order vertex numbers (relative to the copied range).
	uint64_t rawEa;
	uint64_t idxEa;
	uint64_t outVertsEa;  // 16-byte aligned, room for idxCount vertices rounded up to 4.
	uint64_t outIdxEa;    // 16-byte aligned, room for idxCount * 3 + 8 indices.
	uint32_t rawBytes;
	uint32_t rawCount;
	uint32_t size;        // Vertex size.
	uint8_t wt, nw, tc, col, nrm, pos, readUV, fixedNormal;  // fixedNormal: see tlk_decode.
	uint8_t woff, tcoff, coloff, nrmoff, posoff, pad1[3];
	float nsign;
	uint32_t matAmbient;
	uint32_t pad2[2];
	float bones[96];
} __attribute__((aligned(128))) TLKJob;

typedef struct TLKState {
	// Vertex bytes [winStart, winStart + winLen) of the copied range.
	uint8_t window[TLK_WINDOW + 32] __attribute__((aligned(128)));
	uint16_t remap[TLK_MAX_RAW];
	uint16_t idxIn[TLK_ICHUNK + 16] __attribute__((aligned(128)));
	uint16_t idxOut[TLK_ICHUNK + 16] __attribute__((aligned(128)));
	TLKVertex verts[TLK_VCHUNK + 4] __attribute__((aligned(128)));
	uint32_t winStart, winLen;
	uint32_t idxInFirst, idxInCount;   // Which GE-order entries idxIn holds (idxIn[0] = entry idxInFirst - idxInSkip).
	uint32_t idxInSkip;
	uint32_t outVerts, outIdx, idxOutFill, vertFill;
	float lastNormal[3];
} TLKState;

static inline uint32_t tlk_rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t tlk_rd16(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static inline float tlk_bitsf(uint32_t v) {
	float f;
	memcpy(&f, &v, 4);
	return f;
}

// type 1: 8-bit, 2: 16-bit, 3: float. Integers times 1/128 or 1/32768 (exact).
static inline float tlk_comp(const uint8_t *p, int type, int i, int isSigned) {
	switch (type) {
	case 1: return (float)(isSigned ? (int32_t)(int8_t)p[i] : (int32_t)p[i]) * (1.0f / 128.0f);
	case 2: {
		const uint32_t v = tlk_rd16(p + i * 2);
		return (float)(isSigned ? (int32_t)(int16_t)v : (int32_t)v) * (1.0f / 32768.0f);
	}
	default: return tlk_bitsf(tlk_rd32(p + i * 4));
	}
}

static inline uint32_t tlk_color(const uint8_t *c, int col) {
	uint32_t src, dark, full;
	switch (col) {
	case 4:
		src = tlk_rd16(c);
		dark = ((src & 0x1F) << 3) | ((src & 0xF800) << 8);
		full = dark | ((dark >> 5) & 0x070007);
		dark = (src & 0x7E0) << 5;
		return full | dark | ((dark >> 6) & 0x300) | 0xFF000000;
	case 5:
		src = tlk_rd16(c);
		dark = ((src & 0x1F) << 3) | ((src & 0x3E0) << 6) | ((src & 0x7C00) << 9);
		full = dark | ((dark >> 5) & 0x070707);
		return (src >> 15) ? (full | 0xFF000000) : full;
	case 6:
		src = tlk_rd16(c);
		full = (src & 0x000F) | ((src & 0x00F0) << 4) | ((src & 0x0F00) << 8) | ((src & 0xF000) << 12);
		return full | (full << 4);
	case 7:
		return tlk_rd32(c);
	default:
		return 0;
	}
}

// A normal with a NaN or infinite component becomes (0, 0, 0): the RSX's T&L program turned it
// into NaN colors (Silent Hill Origins' burning house, a few vertices with NaN normals), where the
// CPU path lights it like any other. A zero normal gets no diffuse or specular light there.
static inline void tlk_fix_normal(float *nx, float *ny, float *nz) {
	uint32_t a, b, c;
	memcpy(&a, nx, 4); memcpy(&b, ny, 4); memcpy(&c, nz, 4);
	if ((a & 0x7F800000) == 0x7F800000 || (b & 0x7F800000) == 0x7F800000 || (c & 0x7F800000) == 0x7F800000) {
		*nx = 0.0f; *ny = 0.0f; *nz = 0.0f;
	}
}

// One vertex, in DecodeTLVertices' arithmetic order.
static inline void tlk_decode(const TLKJob *j, const uint8_t *p, TLKVertex *o, float lastNormal[3]) {
	float skin[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	int i, k;
	if (j->wt) {
		for (k = 0; k < j->nw; ++k) {
			const float w = tlk_comp(p + j->woff, j->wt, k, 0);
			if (w != 0.0f) {
				const float *bone = &j->bones[k * 12];
				for (i = 0; i < 12; ++i)
					skin[i] += w * bone[i];
			}
		}
	}
	if (j->readUV) {
		o->u = tlk_comp(p + j->tcoff, j->tc, 0, 0);
		o->v = tlk_comp(p + j->tcoff, j->tc, 1, 0);
	} else {
		o->u = 0.0f;
		o->v = 0.0f;
	}
	{
		const uint32_t rgba = j->col ? tlk_color(p + j->coloff, j->col) : j->matAmbient;
		o->c[0] = (uint8_t)rgba; o->c[1] = (uint8_t)(rgba >> 8); o->c[2] = (uint8_t)(rgba >> 16); o->c[3] = (uint8_t)(rgba >> 24);
	}
	if (j->nrm) {
		const float n0 = tlk_comp(p + j->nrmoff, j->nrm, 0, 1);
		const float n1 = tlk_comp(p + j->nrmoff, j->nrm, 1, 1);
		const float n2 = tlk_comp(p + j->nrmoff, j->nrm, 2, 1);
		if (j->wt) {
			lastNormal[0] = n0 * skin[0] + n1 * skin[3] + n2 * skin[6];
			lastNormal[1] = n0 * skin[1] + n1 * skin[4] + n2 * skin[7];
			lastNormal[2] = n0 * skin[2] + n1 * skin[5] + n2 * skin[8];
		} else {
			lastNormal[0] = n0; lastNormal[1] = n1; lastNormal[2] = n2;
		}
	}
	o->nx = lastNormal[0] * j->nsign; o->ny = lastNormal[1] * j->nsign; o->nz = lastNormal[2] * j->nsign;
	tlk_fix_normal(&o->nx, &o->ny, &o->nz);
	if (j->fixedNormal) {
		// No normal in the vertex and nothing uses it (TLDecodeSetup::fixedNormal): (0, 0, 1), not the
		// last normal decoded, which each SPU keeps on its own (so it differed from the PPU's).
		o->nx = 0.0f; o->ny = 0.0f; o->nz = 1.0f;
	}
	{
		const uint8_t *q = p + j->posoff;
		float x, y, z;
		if (j->pos == 3 && !j->wt) {
			// Unskinned float positions: NaN and infinity become 0 (checked on the bits, which
			// works on the SPU too, where those encodings are ordinary numbers).
			const uint32_t bx = tlk_rd32(q), by = tlk_rd32(q + 4), bz = tlk_rd32(q + 8);
			x = ((bx >> 23) & 0xFF) == 0xFF ? 0.0f : tlk_bitsf(bx);
			y = ((by >> 23) & 0xFF) == 0xFF ? 0.0f : tlk_bitsf(by);
			z = ((bz >> 23) & 0xFF) == 0xFF ? 0.0f : tlk_bitsf(bz);
		} else {
			x = tlk_comp(q, j->pos, 0, 1);
			y = tlk_comp(q, j->pos, 1, 1);
			z = tlk_comp(q, j->pos, 2, 1);
		}
		if (j->wt) {
			o->x = x * skin[0] + y * skin[3] + z * skin[6] + skin[9];
			o->y = x * skin[1] + y * skin[4] + z * skin[7] + skin[10];
			o->z = x * skin[2] + y * skin[5] + z * skin[8] + skin[11];
		} else {
			o->x = x; o->y = y; o->z = z;
		}
	}
}

static inline uint32_t tlk_round16(uint32_t n) {
	return (n + 15) & ~15u;
}

// Vertex number v's bytes, through the window.
static inline const uint8_t *tlk_vertex(TLKState *st, const TLKJob *j, uint32_t v) {
	const uint32_t off = v * j->size;
	if (off < st->winStart || off + j->size > st->winStart + st->winLen) {
		const uint32_t start = off & ~15u;
		uint32_t end = start + TLK_WINDOW;
		const uint32_t lim = tlk_round16(j->rawBytes);
		if (end > lim)
			end = lim;
		st->winStart = start;
		st->winLen = end - start;
		TLK_GET(st->window, j->rawEa + start, st->winLen);
		TLK_SYNC();
	}
	return st->window + (off - st->winStart);
}

// GE-order entry i of the job's vertex list.
static inline uint32_t tlk_index(TLKState *st, const TLKJob *j, uint32_t i) {
	if (i < st->idxInFirst || i >= st->idxInFirst + st->idxInCount) {
		// From a little before i: strips step back one entry.
		const uint32_t first = i >= 2 ? i - 2 : 0;
		const uint64_t ea = j->idxEa + (uint64_t)first * 2;
		const uint64_t start = ea & ~15ULL;
		uint32_t count = TLK_ICHUNK;
		const uint64_t endEa = j->idxEa + (uint64_t)j->idxCount * 2;
		uint64_t end = start + count * 2;
		if (end > ((endEa + 15) & ~15ULL))
			end = (endEa + 15) & ~15ULL;
		TLK_GET(st->idxIn, start, (uint32_t)(end - start));
		TLK_SYNC();
		st->idxInSkip = (uint32_t)(ea - start) / 2;
		st->idxInFirst = first;
		st->idxInCount = (uint32_t)(end - ea) / 2;
	}
	return st->idxIn[st->idxInSkip + (i - st->idxInFirst)];
}

static inline void tlk_flushVerts(TLKState *st, const TLKJob *j) {
	if (!st->vertFill)
		return;
	TLK_PUT(st->verts, j->outVertsEa + (uint64_t)(st->outVerts - st->vertFill) * sizeof(TLKVertex), tlk_round16(st->vertFill * sizeof(TLKVertex)));
	TLK_SYNC();
	st->vertFill = 0;
}

static inline void tlk_flushIdx(TLKState *st, const TLKJob *j) {
	if (!st->idxOutFill)
		return;
	TLK_PUT(st->idxOut, j->outIdxEa + (uint64_t)(st->outIdx - st->idxOutFill) * 2, tlk_round16(st->idxOutFill * 2));
	TLK_SYNC();
	st->idxOutFill = 0;
}

static inline void tlk_emitV(TLKState *st, const TLKJob *j, uint32_t v) {
	uint16_t r = v < j->rawCount ? st->remap[v] : 0;
	if (v < j->rawCount && r == 0xFFFF) {
		r = (uint16_t)st->outVerts;
		st->remap[v] = r;
		tlk_decode(j, tlk_vertex(st, j, v), &st->verts[st->vertFill], st->lastNormal);
		st->outVerts++;
		if (++st->vertFill == TLK_VCHUNK)
			tlk_flushVerts(st, j);
	}
	st->idxOut[st->idxOutFill] = r;
	st->outIdx++;
	if (++st->idxOutFill == TLK_ICHUNK)
		tlk_flushIdx(st, j);
}

static inline void tlk_emit(TLKState *st, const TLKJob *j, uint32_t i) {
	tlk_emitV(st, j, tlk_index(st, j, i));
}

// Runs one job. Returns 0, or an error code (the job is then left to the PPU).
static inline uint32_t tlk_run(TLKState *st, const TLKJob *j) {
	uint32_t i;
	const uint32_t n = j->idxCount;
	if (j->rawCount > TLK_MAX_RAW || j->rawCount == 0 || j->size == 0 || j->size > 1024 || j->nw > 8)
		return 1;
	if (((j->outVertsEa | j->outIdxEa | j->rawEa) & 15) != 0)
		return 2;
	for (i = 0; i < j->rawCount; ++i)
		st->remap[i] = 0xFFFF;
	st->winStart = st->winLen = 0;
	st->idxInFirst = st->idxInCount = st->idxInSkip = 0;
	st->outVerts = st->outIdx = st->idxOutFill = st->vertFill = 0;
	switch (j->prim) {
	case TLK_PRIM_TRIANGLES:
		for (i = 0; i + 2 < n; i += 3) {
			tlk_emit(st, j, i); tlk_emit(st, j, i + 1); tlk_emit(st, j, i + 2);
		}
		break;
	case TLK_PRIM_STRIP:
		for (i = 0; i + 2 < n; ++i) {
			if (i & 1) { tlk_emit(st, j, i + 1); tlk_emit(st, j, i); tlk_emit(st, j, i + 2); }
			else { tlk_emit(st, j, i); tlk_emit(st, j, i + 1); tlk_emit(st, j, i + 2); }
		}
		break;
	default:
		if (n >= 3) {
			const uint32_t v0 = tlk_index(st, j, 0);
			for (i = 2; i < n; ++i) {
				tlk_emitV(st, j, v0); tlk_emit(st, j, i - 1); tlk_emit(st, j, i);
			}
		}
		break;
	}
	tlk_flushVerts(st, j);
	tlk_flushIdx(st, j);
	return 0;
}
