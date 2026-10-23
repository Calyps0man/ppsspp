// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Sound debug (PS3 frontend: Debug tab > Sound debug): where in the sound path samples reach full
// scale, written to launch.log as "Sound:" lines every 2 seconds while a game makes sound. A
// sample clipped before the final volume (the frontend's Volume setting) is distortion that
// turning the volume down doesn't remove.

#include <cstdint>
#include "Common/FrontendTrace.h"

// Set by the frontend: SAS voices mixed by the PPU code (MixVoice) instead of the SPU mixer, to
// tell a fault of the SPU mixer from one in the game's sound.
extern bool g_sasMixOnPpu;

struct SoundDebugStats {
	double start = 0.0;
	// sceAudio channels 0-7, and 8 = the SRC channel (sceAudioSRCOutputBlocking).
	struct Channel {
		uint32_t blocks = 0;
		int volL = 0, volR = 0, maxVol = 0;
		int inPeak = 0;          // Largest sample the game handed over (0-32768).
		uint32_t inFull = 0;     // Of those, samples at full scale: clipped before they got here.
		uint32_t samples = 0;
		bool mono = false;
	} ch[9];
	// The final mix of all channels, before the frontend's volume.
	uint32_t mixBlocks = 0;
	int64_t mixPeak = 0;
	uint32_t mixOver = 0;        // Samples over full scale (clipped at the end, before the volume).
	uint32_t mixSamples = 0;
	// SAS (sceSasCore / sceSasCoreWithMix).
	uint32_t sasMixes = 0, sasWithInput = 0;
	int sasVoicesMax = 0;
	int64_t sasDryPeak = 0;      // All voices summed (before the clamp to 16 bits).
	int64_t sasWetPeak = 0;      // The effect (reverb) send.
	int sasInPeak = 0;           // sceSasCoreWithMix: the game's buffer, after its volume.
	int sasInVolL = 0, sasInVolR = 0;
	int64_t sasOutPeak = 0;      // What was written, before the clamp.
	uint32_t sasClipped = 0, sasSamples = 0;
	int sasOutputMode = 0, sasGrain = 0, sasEffectType = -1;
	bool sasDry = false, sasWet = false;
	int effectVolL = 0, effectVolR = 0;
	// Per voice over the period: the loudest settings seen.
	struct Voice {
		uint32_t mixes = 0;
		int type = 0;
		int volL = 0, volR = 0;   // 0x1000 = unity.
		int effL = 0, effR = 0;
		int env = 0;              // Envelope height, 0x40000000 = full.
		int pitch = 0;
	} voice[32];
	// sceAtracDecodeData output.
	uint32_t atracDecodes = 0;
	int atracPeak = 0;
	uint32_t atracFull = 0, atracSamples = 0;
	int atracChannels = 0;
};

extern SoundDebugStats g_soundDebug;

inline bool SoundDebugOn() {
	return FrontendLogOn(FrontendLog::SOUND);
}

// Peak and full-scale count of 16-bit samples: int16_t, or s16_le for PSP memory (byte-swapped on
// the PS3).
template <typename T>
inline void SoundDebugScan(const T *p, int n, int &peak, uint32_t &full) {
	for (int i = 0; i < n; ++i) {
		int v = (int)(int16_t)p[i];
		if (v < 0) v = -v;
		if (v > peak) peak = v;
		if (v >= 32767) full++;
	}
}

// Writes the "Sound:" lines when 2 seconds have gone by (called after each mixed block).
void SoundDebugReport(double now);
