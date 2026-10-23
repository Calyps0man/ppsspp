// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// The per-voice sceSas mixing loop (VAG/PCM/noise voices: decode, resample, envelope, volume) as
// plain C over a self-contained voice record, so the same code runs on the PS3's SPUs
// (PS3/spu/sasmix) and on the PPU. It produces exactly what SasInstance::MixVoice does.
//
// For the mixing functions, before including define SASK_MAP(ctx, ea, len): returns a pointer to len bytes of guest
// memory at host address ea, valid until the next SASK_MAP call (len <= SASK_MAX_MAP).
#pragma once

#include <stdint.h>
#include <string.h>

#define SASK_MAX_MAP 2048
#define SASK_MAX_GRAIN 2048
#define SASK_TEMP_SAMPLES (SASK_MAX_GRAIN * 4 + 2 + 16)
#define SASK_MAX_VOICES 32

enum {
	SASK_TYPE_VAG = 1,
	SASK_TYPE_PCM = 5,
	// Anything else mixes silence (noise / waves are not implemented), but walks the envelope.

	SASK_ENV_KEYON_STEP = -42,
	SASK_ENV_KEYON = -2,
	SASK_ENV_OFF = -1,
	SASK_ENV_ATTACK = 0,
	SASK_ENV_DECAY = 1,
	SASK_ENV_SUSTAIN = 2,
	SASK_ENV_RELEASE = 3,

	SASK_HEIGHT_MAX = 0x40000000,
	SASK_PITCH_BASE = 0x1000,
	SASK_PITCH_MASK = 0xFFF,
	SASK_PITCH_SHIFT = 12,
};

// 16-byte multiple, 64-bit fields first: the same layout for the PPU (64-bit) and the SPU.
typedef struct SasKVoice {
	uint64_t dataEa;       // Host address of the VAG data start / PCM start.
	int64_t height;        // Envelope height.
	uint32_t dataBytes;    // Valid bytes from dataEa (VAG: blocks * 16, PCM: samples * 2).
	int32_t type;
	int32_t pitch;
	uint32_t sampleFrac;
	int32_t volumeLeft, volumeRight, effectLeft, effectRight;
	int32_t envState;
	int32_t attackRate, decayRate, sustainRate, sustainLevel, releaseRate;
	int32_t attackType, decayType, sustainType, releaseType;
	// VAG decoder.
	int32_t vagCurSample, vagReadOff, vagCurBlock, vagLoopStartBlock, vagNumBlocks, vagS1, vagS2;
	// PCM.
	int32_t pcmSize, pcmIndex, pcmLoopPos;
	int16_t resampleHist[2];
	int16_t vagSamples[28];
	uint8_t vagLoopEnabled, vagLoopAtNextBlock, vagEnd, loop;
	uint8_t on, playing, pad0, pad1;
	uint8_t pad2[8];
} SasKVoice;


typedef struct SasKJob {
	uint64_t voicesEa;     // SasKVoice[count]
	uint64_t mixEa;        // int32[grain * 2], accumulated into
	uint64_t sendEa;       // int32[grain * 2], accumulated into
	uint32_t count;
	uint32_t grainSize;
	uint32_t seq;          // Echoed back when done.
	uint32_t pad0;
	// Not 0: when done, the SPU writes seq to this 16-byte aligned address (the PPU polls it)
	// instead of sending an event (a sleep and a wake-up through lv2 for every mix).
	uint64_t doneEa;
	uint32_t pad[20];
} SasKJob;  // 128 bytes.

#ifdef __cplusplus
static_assert(sizeof(SasKVoice) % 16 == 0, "SasKVoice must be DMA-able");
static_assert(sizeof(SasKJob) == 128, "SasKJob must be 128 bytes");
#else
_Static_assert(sizeof(SasKVoice) % 16 == 0, "SasKVoice must be DMA-able");
_Static_assert(sizeof(SasKJob) == 128, "SasKJob must be 128 bytes");
#endif

#ifdef SASK_MAP

// VagDecoder's table, with the second coefficient negated.
static const int16_t sask_vagCoef[16][2] = {
	{ 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 }, { 0, 0 }, { 0, 0 }, { 52, 0 }, { 55, -2 }, { 60, -125 }, { 0, 0 }, { 0, -91 }, { 0, 0 }, { 2, -216 }, { 125, -6 }, { 0, -151 },
};

static inline int sask_clamp16(int v) {
	return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

// --- Envelope (ADSREnvelope) ---

static inline void sask_walk(SasKVoice *v, int type, int rate) {
	int64_t expDelta;
	switch (type) {
	case 0:  // LINEAR_INCREASE
		v->height += rate;
		break;
	case 1:  // LINEAR_DECREASE
		v->height -= rate;
		break;
	case 2:  // LINEAR_BENT
		if (v->height <= (int64_t)SASK_HEIGHT_MAX * 3 / 4)
			v->height += rate;
		else
			v->height += rate / 4;
		break;
	case 3:  // EXPONENT_DECREASE
		expDelta = v->height - SASK_HEIGHT_MAX;
		expDelta += (-expDelta * rate) >> 32;
		// (rate + 3UL) / 4UL with a 64-bit unsigned long, as on the 64-bit hosts.
		v->height = (int64_t)((uint64_t)(expDelta + SASK_HEIGHT_MAX) - ((uint64_t)(int64_t)rate + 3) / 4);
		break;
	case 4:  // EXPONENT_INCREASE
		expDelta = v->height - SASK_HEIGHT_MAX;
		expDelta += (-expDelta * rate) >> 32;
		v->height = expDelta + 0x4000 + SASK_HEIGHT_MAX;
		break;
	case 5:  // DIRECT
		v->height = rate;
		break;
	}
}

static inline void sask_setState(SasKVoice *v, int state) {
	if (v->height > SASK_HEIGHT_MAX)
		v->height = SASK_HEIGHT_MAX;
	v->envState = state;
}

static inline void sask_step(SasKVoice *v) {
	switch (v->envState) {
	case SASK_ENV_ATTACK:
		sask_walk(v, v->attackType, v->attackRate);
		if (v->height >= SASK_HEIGHT_MAX || v->height < 0)
			sask_setState(v, SASK_ENV_DECAY);
		break;
	case SASK_ENV_DECAY:
		sask_walk(v, v->decayType, v->decayRate);
		if (v->height < v->sustainLevel)
			sask_setState(v, SASK_ENV_SUSTAIN);
		break;
	case SASK_ENV_SUSTAIN:
		sask_walk(v, v->sustainType, v->sustainRate);
		if (v->height <= 0) {
			v->height = 0;
			sask_setState(v, SASK_ENV_RELEASE);
		}
		break;
	case SASK_ENV_RELEASE:
		sask_walk(v, v->releaseType, v->releaseRate);
		if (v->height <= 0) {
			v->height = 0;
			sask_setState(v, SASK_ENV_OFF);
		}
		break;
	case SASK_ENV_OFF:
		break;
	case SASK_ENV_KEYON:
		v->height = 0;
		sask_setState(v, SASK_ENV_KEYON_STEP);
		break;
	case SASK_ENV_KEYON_STEP:
		v->height++;
		if (v->height >= 31) {
			v->height = 0;
			sask_setState(v, SASK_ENV_ATTACK);
		}
		break;
	}
}

static inline int sask_height(const SasKVoice *v) {
	return (int)(v->height > (int64_t)SASK_HEIGHT_MAX ? SASK_HEIGHT_MAX : v->height);
}

// --- VAG (VagDecoder) ---

// Decodes the block at *pos. Returns 0 at the end (vagEnd set, *pos unchanged).
static inline int sask_vagDecodeBlock(void *ctx, SasKVoice *v, int32_t *pos) {
	if (v->vagCurBlock == v->vagNumBlocks - 1) {
		v->vagEnd = 1;
		return 0;
	}
	const uint8_t *readp = (const uint8_t *)SASK_MAP(ctx, v->dataEa + (uint32_t)*pos, 16);
	int predict_nr = readp[0];
	int shift_factor = predict_nr & 0xf;
	predict_nr >>= 4;
	int flags = readp[1];
	if (flags == 7) {
		v->vagEnd = 1;
		return 0;
	} else if (flags == 6) {
		v->vagLoopStartBlock = v->vagCurBlock;
	} else if (flags == 3) {
		if (v->vagLoopEnabled)
			v->vagLoopAtNextBlock = 1;
	}
	int s1 = v->vagS1;
	int s2 = v->vagS2;
	const int coef1 = sask_vagCoef[predict_nr][0];
	const int coef2 = sask_vagCoef[predict_nr][1];
	for (int i = 0; i < 28; i += 2) {
		uint8_t d = readp[2 + i / 2];
		int sample1 = (int16_t)((d & 0xf) << 12) >> shift_factor;
		int sample2 = (int16_t)((d & 0xf0) << 8) >> shift_factor;
		s2 = sask_clamp16(sample1 + ((s1 * coef1 + s2 * coef2) >> 6));
		s1 = sask_clamp16(sample2 + ((s2 * coef1 + s1 * coef2) >> 6));
		v->vagSamples[i] = (int16_t)s2;
		v->vagSamples[i + 1] = (int16_t)s1;
	}
	v->vagS1 = s1;
	v->vagS2 = s2;
	v->vagCurSample = 0;
	v->vagCurBlock++;
	*pos += 16;
	return 1;
}

static inline void sask_vagGetSamples(void *ctx, SasKVoice *v, int16_t *out, int numSamples) {
	if (v->vagEnd) {
		memset(out, 0, numSamples * sizeof(int16_t));
		return;
	}
	int32_t pos = v->vagReadOff;
	int32_t orig = pos;
	for (int i = 0; i < numSamples; i++) {
		if (v->vagCurSample == 28) {
			if (v->vagLoopAtNextBlock) {
				v->vagReadOff = 16 * v->vagLoopStartBlock + 16;
				pos = orig = v->vagReadOff;
				v->vagCurBlock = v->vagLoopStartBlock;
				v->vagLoopAtNextBlock = 0;
			}
			if (!sask_vagDecodeBlock(ctx, v, &pos)) {
				// Like VagDecoder::GetSamples, returns without committing the read position.
				memset(&out[i], 0, (numSamples - i) * sizeof(int16_t));
				return;
			}
		}
		out[i] = v->vagSamples[v->vagCurSample++];
	}
	v->vagReadOff += pos - orig;
}

// --- PCM ---

static inline void sask_pcmGetSamples(void *ctx, SasKVoice *v, int16_t *out, int numSamples) {
	int needed = numSamples;
	while (needed > 0) {
		int size = v->pcmSize - v->pcmIndex;
		if (size > needed)
			size = needed;
		if (!v->on) {
			v->pcmIndex = 0;
			break;
		}
		int done = 0;
		while (done < size) {
			int chunk = size - done;
			if (chunk > SASK_MAX_MAP / 2)
				chunk = SASK_MAX_MAP / 2;
			const uint8_t *src = (const uint8_t *)SASK_MAP(ctx, v->dataEa + (uint32_t)(v->pcmIndex + done) * 2, (uint32_t)chunk * 2);
			for (int i = 0; i < chunk; ++i)
				out[done + i] = (int16_t)(src[i * 2] | (src[i * 2 + 1] << 8));
			done += chunk;
		}
		v->pcmIndex += size;
		needed -= size;
		out += size;
		if (v->pcmIndex >= v->pcmSize) {
			if (!v->loop)
				break;
			v->pcmIndex = v->pcmLoopPos;
		}
	}
	if (needed > 0)
		memset(out, 0, needed * sizeof(int16_t));
}

// --- The voice (SasInstance::MixVoice) ---

static inline void sask_mixVoice(void *ctx, SasKVoice *v, int grainSize, int16_t *mixTemp, int32_t *mixBuffer, int32_t *sendBuffer) {
	int delay = 0;
	const int needsKeyOn = v->envState == SASK_ENV_KEYON;
	if (needsKeyOn) {
		const int ignorePitch = v->type == SASK_TYPE_PCM && v->pitch > SASK_PITCH_BASE;
		delay = ignorePitch ? 32 : (int)((32 * (uint32_t)v->pitch) >> SASK_PITCH_SHIFT);
		if (v->type == SASK_TYPE_VAG)
			++delay;
	}

	mixTemp[0] = v->resampleHist[0];
	mixTemp[1] = v->resampleHist[1];

	const int voicePitch = v->pitch;
	uint32_t sampleFrac = v->sampleFrac;
	int samplesToRead = (int)((sampleFrac + voicePitch * (grainSize - delay > 0 ? grainSize - delay : 0)) >> SASK_PITCH_SHIFT);
	if ((unsigned)samplesToRead > SASK_TEMP_SAMPLES - 2)
		samplesToRead = SASK_TEMP_SAMPLES - 2;
	int readPos = 2;
	if (needsKeyOn) {
		readPos = 0;
		samplesToRead += 2;
	}
	if (v->type == SASK_TYPE_VAG)
		sask_vagGetSamples(ctx, v, &mixTemp[readPos], samplesToRead);
	else if (v->type == SASK_TYPE_PCM)
		sask_pcmGetSamples(ctx, v, &mixTemp[readPos], samplesToRead);
	else
		memset(&mixTemp[readPos], 0, samplesToRead * sizeof(int16_t));
	const int tempPos = readPos + samplesToRead;

	for (int i = 0; i < delay; ++i)
		sask_step(v);

	const int needsInterp = voicePitch != SASK_PITCH_BASE || (sampleFrac & SASK_PITCH_MASK) != 0;
	const int volL = v->volumeLeft, volR = v->volumeRight, effL = v->effectLeft, effR = v->effectRight;
	for (int i = delay; i < grainSize; i++) {
		const int16_t *s = mixTemp + (sampleFrac >> SASK_PITCH_SHIFT);
		int sample = s[0];
		if (needsInterp) {
			int f = sampleFrac & SASK_PITCH_MASK;
			sample = s[0] - (((s[0] - s[1]) * f) >> SASK_PITCH_SHIFT);
		}
		sampleFrac += voicePitch;

		int envelopeValue = sask_height(v);
		sask_step(v);
		envelopeValue = (envelopeValue + (1 << 14)) >> 15;
		sample = ((sample * envelopeValue) + (1 << 14)) >> 15;

		mixBuffer[i * 2] += (sample * volL) >> 12;
		mixBuffer[i * 2 + 1] += (sample * volR) >> 12;
		sendBuffer[i * 2] += sample * effL >> 12;
		sendBuffer[i * 2 + 1] += sample * effR >> 12;
	}

	v->resampleHist[0] = mixTemp[tempPos - 2];
	v->resampleHist[1] = mixTemp[tempPos - 1];
	v->sampleFrac = sampleFrac - (tempPos - 2) * SASK_PITCH_BASE;

	int ended = 0;
	if (v->type == SASK_TYPE_VAG)
		ended = v->vagEnd;
	else if (v->type == SASK_TYPE_PCM)
		ended = v->pcmIndex >= v->pcmSize;
	if (ended) {
		sask_setState(v, SASK_ENV_OFF);
		v->height = 0;
	}
	if (v->envState == SASK_ENV_OFF) {
		v->playing = 0;
		v->on = 0;
	}
}

#endif  // SASK_MAP
