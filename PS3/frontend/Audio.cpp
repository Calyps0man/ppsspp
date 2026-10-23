// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// PS3 audio output for the PPSSPP core: the emulator pushes 44.1 kHz stereo frames (PSP mixer
// output), a high-priority thread converts them to the PS3's 48 kHz float format and feeds an
// audio port block by block. When the emulator falls behind, the output fades to silence for
// that block instead of repeating noise.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <audio/audio.h>
#include <sys/thread.h>

#include "PS3/frontend/Audio.h"
#include "PS3/frontend/Frontend.h"

namespace PS3Audio {

static constexpr int SOURCE_RATE = 44100;
static constexpr int OUTPUT_RATE = 48000;
static constexpr uint32_t RING_FRAMES = 16384;  // ~370 ms at 44.1 kHz (power of two)
static constexpr int BLOCKS_AHEAD = 3;          // Latency: 3 x 256 frames = 16 ms

static int16_t g_ring[RING_FRAMES * 2];
static std::atomic<uint32_t> g_writePos{0};  // Frames written (wraps)
static std::atomic<uint32_t> g_readPos{0};   // Frames consumed (wraps)
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_enabled{true};
static sys_ppu_thread_t g_thread;
static u32 g_port = 0;
static audioPortConfig g_config;
static uint64_t g_underruns = 0, g_overruns = 0, g_blocks = 0;
// When the emulator runs slower than real time (SetSlowMode): 0 fade each starved block to
// silence (as before), 1 after running dry wait until REFILL_FRAMES are queued (fewer, longer
// gaps instead of crackling), 2 play slower while little is queued (continuous, lower pitch).
static std::atomic<int> g_slowMode{0};
static constexpr uint32_t REFILL_FRAMES = 4096;  // ~93 ms at 44.1 kHz
static bool g_refilling = false;
static uint64_t g_stretched = 0;

void Push(const int32_t *samples, int frames, float volume) {
	if (!g_running || !g_enabled || !samples)
		return;
	uint32_t w = g_writePos.load(std::memory_order_relaxed);
	const uint32_t r = g_readPos.load(std::memory_order_acquire);
	uint32_t free = RING_FRAMES - (w - r);
	if ((uint32_t)frames > free) {
		// Consumer is behind (shouldn't happen at normal speed): drop this batch's excess.
		g_overruns++;
		frames = (int)free;
	}
	for (int i = 0; i < frames; ++i) {
		int32_t l = (int32_t)(samples[i * 2] * volume);
		int32_t rr = (int32_t)(samples[i * 2 + 1] * volume);
		if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
		if (rr > 32767) rr = 32767; else if (rr < -32768) rr = -32768;
		const uint32_t idx = (w & (RING_FRAMES - 1)) * 2;
		g_ring[idx] = (int16_t)l;
		g_ring[idx + 1] = (int16_t)rr;
		w++;
	}
	g_writePos.store(w, std::memory_order_release);
}

// Fills one 256-frame block (stereo floats) by resampling 44.1 -> 48 kHz.
static void FillBlock(float *out) {
	static double frac = 0.0;
	static float lastL = 0.0f, lastR = 0.0f;
	static double speed = 1.0;  // Mode 2: playback speed, eased towards the target.
	double step = (double)SOURCE_RATE / OUTPUT_RATE;
	uint32_t r = g_readPos.load(std::memory_order_relaxed);
	const uint32_t w = g_writePos.load(std::memory_order_acquire);
	const int mode = g_slowMode.load(std::memory_order_relaxed);
	const uint32_t queued = w - r;
	if (mode != 0 && g_refilling) {
		if (queued < REFILL_FRAMES) {
			memset(out, 0, AUDIO_BLOCK_SAMPLES * 2 * sizeof(float));
			return;
		}
		g_refilling = false;
	}
	if (mode == 2) {
		// Less than REFILL_FRAMES queued: slower, down to half speed when nearly empty. (Build 153's
		// 80% floor sounded worse: more gaps.)
		double target = queued >= REFILL_FRAMES ? 1.0 : 0.5 + 0.5 * (double)queued / REFILL_FRAMES;
		speed += (target - speed) * 0.1;
		if (speed < 0.999)
			g_stretched++;
		step *= speed;
	} else {
		speed = 1.0;
	}
	// Frames needed for this block (+1 for interpolation).
	const uint32_t need = (uint32_t)(frac + step * AUDIO_BLOCK_SAMPLES) + 2;
	if (queued < need) {
		if (mode != 0)
			g_refilling = true;
		// Underrun: fade the last sample out over the block.
		g_underruns++;
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
			const float k = 1.0f - (float)i / AUDIO_BLOCK_SAMPLES;
			out[i * 2] = lastL * k;
			out[i * 2 + 1] = lastR * k;
		}
		lastL = lastR = 0.0f;
		return;
	}
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
		const uint32_t i0 = ((r + (uint32_t)frac) & (RING_FRAMES - 1)) * 2;
		const uint32_t i1 = ((r + (uint32_t)frac + 1) & (RING_FRAMES - 1)) * 2;
		const float t = (float)(frac - (double)(uint32_t)frac);
		const float l = g_ring[i0] + (g_ring[i1] - g_ring[i0]) * t;
		const float rr = g_ring[i0 + 1] + (g_ring[i1 + 1] - g_ring[i0 + 1]) * t;
		out[i * 2] = l * (1.0f / 32768.0f);
		out[i * 2 + 1] = rr * (1.0f / 32768.0f);
		frac += step;
	}
	lastL = out[(AUDIO_BLOCK_SAMPLES - 1) * 2];
	lastR = out[(AUDIO_BLOCK_SAMPLES - 1) * 2 + 1];
	const uint32_t consumed = (uint32_t)frac;
	frac -= consumed;
	g_readPos.store(r + consumed, std::memory_order_release);
}

static void AudioThread(void *) {
	volatile u64 *readIndex = (volatile u64 *)(uintptr_t)g_config.readIndex;
	const u32 numBlocks = (u32)g_config.numBlocks;
	float *data = (float *)(uintptr_t)g_config.audioDataStart;
	u64 nextFill = (*readIndex + 1) % numBlocks;
	while (g_running) {
		const u64 playing = *readIndex;
		// Keep BLOCKS_AHEAD blocks queued after the one playing.
		while (((nextFill + numBlocks - playing) % numBlocks) <= (u64)BLOCKS_AHEAD && g_running) {
			float *block = data + nextFill * AUDIO_BLOCK_SAMPLES * 2;
			if (g_enabled)
				FillBlock(block);
			else
				memset(block, 0, AUDIO_BLOCK_SAMPLES * 2 * sizeof(float));
			nextFill = (nextFill + 1) % numBlocks;
			g_blocks++;
		}
		usleep(1500);
	}
	sysThreadExit(0);
}

bool Start() {
	if (g_running)
		return true;
	if (audioInit() != 0) {
		PS3Frontend::Trace("Audio: audioInit failed");
		return false;
	}
	audioPortParam params{};
	params.numChannels = AUDIO_PORT_2CH;
	params.numBlocks = AUDIO_BLOCK_8;
	params.attrib = 0;
	params.level = 1.0f;
	if (audioPortOpen(&params, &g_port) != 0) {
		PS3Frontend::Trace("Audio: audioPortOpen failed");
		audioQuit();
		return false;
	}
	if (audioGetPortConfig(g_port, &g_config) != 0) {
		PS3Frontend::Trace("Audio: audioGetPortConfig failed");
		audioPortClose(g_port);
		audioQuit();
		return false;
	}
	memset((void *)(uintptr_t)g_config.audioDataStart, 0, g_config.portSize);
	g_writePos = 0;
	g_readPos = 0;
	g_running = true;
	if (sysThreadCreate(&g_thread, AudioThread, nullptr, 100, 0x4000, THREAD_JOINABLE, (char *)"ppsspp_audio") != 0) {
		PS3Frontend::Trace("Audio: thread creation failed");
		g_running = false;
		audioPortClose(g_port);
		audioQuit();
		return false;
	}
	audioPortStart(g_port);
	PS3Frontend::Trace("Audio: started, %u blocks of %d frames at 48 kHz", (unsigned)g_config.numBlocks, AUDIO_BLOCK_SAMPLES);
	return true;
}

void Stop() {
	if (!g_running)
		return;
	g_running = false;
	u64 ret;
	sysThreadJoin(g_thread, &ret);
	audioPortStop(g_port);
	audioPortClose(g_port);
	audioQuit();
	PS3Frontend::Trace("Audio: stopped (%llu blocks, %llu underruns, %llu overruns)",
		(unsigned long long)g_blocks, (unsigned long long)g_underruns, (unsigned long long)g_overruns);
}

void SetEnabled(bool enabled) {
	g_enabled = enabled;
}

void SetSlowMode(int mode) {
	g_slowMode = mode;
}

std::string StatsText() {
	char text[128];
	snprintf(text, sizeof(text), "audio %llu blocks, %llu underruns, %llu played slower", (unsigned long long)g_blocks, (unsigned long long)g_underruns,
		(unsigned long long)g_stretched);
	return text;
}

}  // namespace PS3Audio
