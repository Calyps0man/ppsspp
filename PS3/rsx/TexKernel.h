// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Texture work for the RSX renderer, shared by the SPUs (PS3/spu/texspu.c) and the host test
// build: hashing PSP memory, and decoding a PSP texture (16/32-bit and CLUT formats, linear or
// swizzled) to 0xAARRGGBB words straight into the RSX copy. Same output as
// RSXRenderer::FastDecode. Several SPUs share a job: part k of n takes chunks (hash) or
// 8-row bands (decode) k, k + n, ...
//
// Plain C. Memory outside the program goes through TK_GET(ls, ea, len, tag) and
// TK_PUT(ls, ea, len, tag) (16-byte aligned, len a multiple of 16 or one of 4 and 8),
// TK_WAIT(tag) waits for a tag's transfers. On the PPU they're memcpy.
#pragma once
#include <stdint.h>
#include <string.h>

#define TK_OP_HASH 1
#define TK_OP_DECODE 2
#define TK_OP_QUIT 3

#define TK_HCHUNK (16 * 1024)       // Hash chunk.
#define TK_MAX_ROWBYTES 4096        // Source row (1024 32-bit texels).
#define TK_MAX_W 1024
#define TK_BAND 8                   // Rows per band (a swizzle block row).

// PSP texture formats (GETextureFormat).
#define TK_FMT_5650 0
#define TK_FMT_5551 1
#define TK_FMT_4444 2
#define TK_FMT_8888 3
#define TK_FMT_CLUT4 4
#define TK_FMT_CLUT8 5
#define TK_FMT_CLUT16 6
#define TK_FMT_CLUT32 7

// One job (one per SPU taking part), 128-byte aligned. The SPU writes the first 32 bytes back.
typedef struct TKJob {
	// Results.
	uint32_t done;        // == seq when finished.
	uint32_t error;
	uint32_t pad0[2];
	uint64_t hash;        // TK_OP_HASH: this part's hash.
	uint32_t pad1[2];
	// Request.
	uint32_t seq;         // Non-zero, increasing per slot.
	uint32_t op;
	uint32_t fmt;
	uint32_t swizzle;
	uint64_t srcEa;       // 16-byte aligned.
	uint64_t dstEa;       // Decode: first output row (16-byte aligned).
	uint64_t lutEa;       // Decode, CLUT formats: 256 (CLUT4/8, already indexed) or 512 words, 128-byte aligned.
	uint32_t bytes;       // Hash: a multiple of 16.
	uint32_t rowBytes;    // Decode: source bytes per row, a multiple of 16.
	uint32_t w, h;        // Decode: output size, w a multiple of 4.
	uint32_t dstPitch;    // Bytes, a multiple of 16.
	uint32_t clutMode;    // CLUT16/32: 1 = index through shift/mask/offset, 0 = index & 0xFF.
	uint32_t clutShift, clutMask, clutOffset;
	uint32_t part, parts;
	uint32_t pad2[3];
} __attribute__((aligned(128))) TKJob;

typedef struct TKState {
	uint8_t in[2][TK_MAX_ROWBYTES * TK_BAND] __attribute__((aligned(128)));
	uint32_t out[2][TK_MAX_W * TK_BAND] __attribute__((aligned(128)));
	uint32_t lut[512] __attribute__((aligned(128)));
} TKState;

static inline uint32_t tk_rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static inline uint32_t tk_fmix(uint32_t h) {
	h ^= h >> 16; h *= 0x85EBCA6Bu;
	h ^= h >> 13; h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}

// Sixteen add-rotate lanes over 64-byte blocks (four vectors on the SPU), finished with a
// murmur mix. Only ever compared with itself.
static void tk_hash_block(uint32_t a[16], const uint32_t *w, uint32_t words) {
#ifdef __SPU__
	vec_uint4 v0 = *(vec_uint4 *)&a[0], v1 = *(vec_uint4 *)&a[4], v2 = *(vec_uint4 *)&a[8], v3 = *(vec_uint4 *)&a[12];
	const vec_uint4 k = spu_splats(0x9E3779B9u);
	const vec_uint4 *p = (const vec_uint4 *)w;
	for (uint32_t i = 0; i < words / 16; ++i, p += 4) {
		v0 = spu_add(spu_rl(spu_add(v0, p[0]), 11), k);
		v1 = spu_add(spu_rl(spu_add(v1, p[1]), 11), k);
		v2 = spu_add(spu_rl(spu_add(v2, p[2]), 11), k);
		v3 = spu_add(spu_rl(spu_add(v3, p[3]), 11), k);
	}
	*(vec_uint4 *)&a[0] = v0; *(vec_uint4 *)&a[4] = v1; *(vec_uint4 *)&a[8] = v2; *(vec_uint4 *)&a[12] = v3;
#else
	for (uint32_t i = 0; i < words; i += 16)
		for (int j = 0; j < 16; ++j)
			a[j] = tk_rotl(a[j] + w[i + j], 11) + 0x9E3779B9u;
#endif
}

static uint64_t tk_hash(TKState *st, const TKJob *j) {
	uint32_t a[16] __attribute__((aligned(16)));
	for (int i = 0; i < 16; ++i)
		a[i] = 0x01000193u * (uint32_t)(i + 1) + j->part;
	const uint32_t chunks = (j->bytes + TK_HCHUNK - 1) / TK_HCHUNK;
	uint32_t c = j->part;
	int buf = 0;
	if (c < chunks) {
		const uint32_t len = j->bytes - c * TK_HCHUNK < TK_HCHUNK ? j->bytes - c * TK_HCHUNK : TK_HCHUNK;
		TK_GET(st->in[0], j->srcEa + (uint64_t)c * TK_HCHUNK, len, 0);
	}
	for (; c < chunks; c += j->parts) {
		const uint32_t len = j->bytes - c * TK_HCHUNK < TK_HCHUNK ? j->bytes - c * TK_HCHUNK : TK_HCHUNK;
		const uint32_t next = c + j->parts;
		if (next < chunks) {
			const uint32_t nlen = j->bytes - next * TK_HCHUNK < TK_HCHUNK ? j->bytes - next * TK_HCHUNK : TK_HCHUNK;
			TK_GET(st->in[buf ^ 1], j->srcEa + (uint64_t)next * TK_HCHUNK, nlen, buf ^ 1);
		}
		TK_WAIT(buf);
		// A whole number of 64-byte blocks; the rest (under 64 bytes) one word at a time.
		const uint32_t words = len / 4;
		tk_hash_block(a, (const uint32_t *)st->in[buf], words & ~15u);
		for (uint32_t i = words & ~15u; i < words; ++i)
			a[i & 15] = tk_rotl(a[i & 15] ^ ((const uint32_t *)st->in[buf])[i], 7) + 0x7F4A7C15u;
		a[c & 15] ^= c;
		buf ^= 1;
	}
	uint32_t h0 = j->bytes, h1 = j->bytes ^ 0x5bd1e995u;
	for (int i = 0; i < 16; ++i) {
		h0 = tk_fmix(h0 ^ a[i]) + (uint32_t)i;
		h1 = tk_fmix(h1 + tk_rotl(a[i], 16)) ^ (uint32_t)i;
	}
	return ((uint64_t)h0 << 32) | h1;
}

static inline uint32_t tk_565(uint32_t v) {
	const uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
	return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}
static inline uint32_t tk_5551(uint32_t v) {
	const uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
	return ((v & 0x8000) ? 0xFF000000u : 0) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}
static inline uint32_t tk_4444(uint32_t v) {
	return (((v >> 12) & 15) * 17 << 24) | ((v & 15) * 17 << 16) | (((v >> 4) & 15) * 17 << 8) | ((v >> 8) & 15) * 17;
}

// Row r of a band: source byte offset of byte xb.
static inline uint32_t tk_off(uint32_t xb, uint32_t r, uint32_t rowBytes, uint32_t swz) {
	return swz ? (xb >> 4) * 128 + r * 16 + (xb & 15) : r * rowBytes + xb;
}

static void tk_row(const TKJob *j, const uint8_t *band, uint32_t r, uint32_t *out, const uint32_t *lut) {
	const uint32_t w = j->w, rb = j->rowBytes, swz = j->swizzle;
	const uint32_t shift = j->clutShift, mask = j->clutMask, offset = j->clutOffset, mode = j->clutMode;
	uint32_t x;
	switch (j->fmt) {
	case TK_FMT_5650:
		for (x = 0; x < w; ++x) { const uint8_t *p = band + tk_off(x * 2, r, rb, swz); out[x] = tk_565(p[0] | (p[1] << 8)); }
		break;
	case TK_FMT_5551:
		for (x = 0; x < w; ++x) { const uint8_t *p = band + tk_off(x * 2, r, rb, swz); out[x] = tk_5551(p[0] | (p[1] << 8)); }
		break;
	case TK_FMT_4444:
		for (x = 0; x < w; ++x) { const uint8_t *p = band + tk_off(x * 2, r, rb, swz); out[x] = tk_4444(p[0] | (p[1] << 8)); }
		break;
	case TK_FMT_8888:
		for (x = 0; x < w; ++x) {
			const uint8_t *p = band + tk_off(x * 4, r, rb, swz);
			out[x] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
		}
		break;
	case TK_FMT_CLUT4:
		for (x = 0; x < w; x += 2) {
			const uint8_t b = band[tk_off(x >> 1, r, rb, swz)];
			out[x] = lut[b & 15];
			out[x + 1] = lut[b >> 4];
		}
		break;
	case TK_FMT_CLUT8:
		for (x = 0; x < w; ++x) out[x] = lut[band[tk_off(x, r, rb, swz)]];
		break;
	case TK_FMT_CLUT16:
		for (x = 0; x < w; ++x) {
			const uint8_t *p = band + tk_off(x * 2, r, rb, swz);
			const uint32_t v = p[0] | (p[1] << 8);
			out[x] = lut[mode ? (((v >> shift) & mask) | offset) & 511 : v & 0xFF];
		}
		break;
	default:
		for (x = 0; x < w; ++x) {
			const uint8_t *p = band + tk_off(x * 4, r, rb, swz);
			const uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
			out[x] = lut[mode ? (((v >> shift) & mask) | offset) & 511 : v & 0xFF];
		}
		break;
	}
}

// Output rows [0, h). The source holds whole bands (8 rows; a swizzled texture's last band in full).
static uint32_t tk_decode(TKState *st, const TKJob *j) {
	if (j->rowBytes > TK_MAX_ROWBYTES || (j->rowBytes & 15) || j->w > TK_MAX_W || (j->w & 3) || j->w == 0)
		return 1;
	if (j->fmt >= TK_FMT_CLUT4) {
		const uint32_t n = (j->fmt == TK_FMT_CLUT4 || j->fmt == TK_FMT_CLUT8) ? 256 : 512;
		TK_GET(st->lut, j->lutEa, n * 4, 2);
		TK_WAIT(2);
	}
	const uint32_t bands = (j->h + TK_BAND - 1) / TK_BAND;
	const uint32_t bandBytes = j->rowBytes * TK_BAND;
	// The rows of band b that are in the source (all 8 if swizzled).
	#define TK_BAND_ROWS(b) (j->swizzle ? TK_BAND : (j->h - (b) * TK_BAND < TK_BAND ? j->h - (b) * TK_BAND : TK_BAND))
	uint32_t b = j->part;
	int buf = 0;
	if (b < bands)
		TK_GET(st->in[0], j->srcEa + (uint64_t)b * bandBytes, TK_BAND_ROWS(b) * j->rowBytes, 0);
	for (; b < bands; b += j->parts) {
		const uint32_t next = b + j->parts;
		if (next < bands) {
			// The other output buffer's writes must be done before its input slot's tag is reused.
			TK_WAIT(buf ^ 1);
			TK_GET(st->in[buf ^ 1], j->srcEa + (uint64_t)next * bandBytes, TK_BAND_ROWS(next) * j->rowBytes, buf ^ 1);
		}
		TK_WAIT(buf);
		const uint32_t y0 = b * TK_BAND;
		const uint32_t rows = j->h - y0 < TK_BAND ? j->h - y0 : TK_BAND;
		for (uint32_t r = 0; r < rows; ++r) {
			uint32_t *out = st->out[buf] + r * j->w;
			tk_row(j, st->in[buf], r, out, st->lut);
			TK_PUT(out, j->dstEa + (uint64_t)(y0 + r) * j->dstPitch, j->w * 4, buf);
		}
		buf ^= 1;
	}
	#undef TK_BAND_ROWS
	TK_WAIT(0);
	TK_WAIT(1);
	return 0;
}
