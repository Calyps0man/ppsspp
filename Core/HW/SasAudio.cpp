// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "GPU/PS3Profile.h"
#include <algorithm>
#include <vector>
#include <cstdlib>

#include "Common/Profiler/Profiler.h"

#include "Common/Serialize/SerializeFuncs.h"
#include "Core/MemMapHelpers.h"
#include "Core/HLE/sceAtrac.h"
#include "Core/HLE/scePower.h"
#include "Core/Config.h"
#include "Core/Reporting.h"
#include "Core/Util/AudioFormat.h"
#include "Core/System.h"
#include "Core/HW/SoundDebug.h"
#include "SasAudio.h"
#include "Core/HW/SasSpu.h"

static const u8 f[16][2] = {
	{   0,   0 },
	{  60,   0 },
	{ 115,  52 },
	{  98,  55 },
	{ 122,  60 },
	// Everything past index 4 is the hardware reading off the end of its own table, so this is garbage extra values.
	{   0,   0 },
	{   0,   0 },
	{  52,   0 },
	{  55,   2 },
	{  60, 125 },
	{   0,   0 },
	{   0,  91 },
	{   0,   0 },
	{   2, 216 },
	{ 125,   6 },
	{   0, 151 },
};

void VagDecoder::Start(u32 data, u32 vagSize, bool loopEnabled) {
	loopEnabled_ = loopEnabled;
	loopAtNextBlock_ = false;
	loopStartBlock_ = -1;
	numBlocks_ = vagSize / 16;
	end_ = false;
	data_ = data;
	read_ = data;
	curSample = 28;
	curBlock_ = -1;
	s_1 = 0;	// per block?
	s_2 = 0;
}

void VagDecoder::DecodeBlock(const u8 *&read_pointer) {
	if (curBlock_ == numBlocks_ - 1) {
		end_ = true;
		return;
	}

	_dbg_assert_(curBlock_ < numBlocks_);

	const u8 *readp = read_pointer;
	int predict_nr = *readp++;
	int shift_factor = predict_nr & 0xf;
	predict_nr >>= 4;
	int flags = *readp++;
	if (flags == 7) {
		VERBOSE_LOG(Log::SasMix, "VAG ending block at %d", curBlock_);
		end_ = true;
		return;
	}
	else if (flags == 6) {
		loopStartBlock_ = curBlock_;
	}
	else if (flags == 3) {
		if (loopEnabled_) {
			loopAtNextBlock_ = true;
		}
	}

	// Keep state in locals to avoid bouncing to memory.
	int s1 = s_1;
	int s2 = s_2;

	int coef1 = f[predict_nr][0];
	int coef2 = -f[predict_nr][1];

	// TODO: Unroll once more and interleave the unpacking with the decoding more?
	for (int i = 0; i < 28; i += 2) {
		u8 d = *readp++;
		int sample1 = (short)((d & 0xf) << 12) >> shift_factor;
		int sample2 = (short)((d & 0xf0) << 8) >> shift_factor;
		s2 = clamp_s16(sample1 + ((s1 * coef1 + s2 * coef2) >> 6));
		s1 = clamp_s16(sample2 + ((s2 * coef1 + s1 * coef2) >> 6));
		samples[i] = s2;
		samples[i + 1] = s1;
	}

	s_1 = s1;
	s_2 = s2;
	curSample = 0;
	curBlock_++;

	read_pointer = readp;
}

void VagDecoder::GetSamples(s16 *outSamples, int numSamples) {
	if (end_) {
		memset(outSamples, 0, numSamples * sizeof(s16));
		return;
	}
	if (!Memory::IsValidRange(read_, numBlocks_ * 16)) {
		WARN_LOG_REPORT_ONCE(badvagaddr, Log::SasMix, "Bad VAG samples address? %08x / %d", read_, numBlocks_);
		return;
	}

	const u8 *readp = Memory::GetPointerUnchecked(read_);
	const u8 *origp = readp;

	for (int i = 0; i < numSamples; i++) {
		if (curSample == 28) {
			if (loopAtNextBlock_) {
				VERBOSE_LOG(Log::SasMix, "Looping VAG from block %d/%d to %d", curBlock_, numBlocks_, loopStartBlock_);
				// data_ starts at curBlock = -1.
				read_ = data_ + 16 * loopStartBlock_ + 16;
				readp = Memory::GetPointerUnchecked(read_);
				origp = readp;
				curBlock_ = loopStartBlock_;
				loopAtNextBlock_ = false;
			}
			DecodeBlock(readp);
			if (end_) {
				// Clear the rest of the buffer and return.
				memset(&outSamples[i], 0, (numSamples - i) * sizeof(s16));
				return;
			}
		}
		_dbg_assert_(curSample < 28);
		outSamples[i] = samples[curSample++];
	}

	if (readp > origp) {
		if (MemBlockInfoDetailed())
			NotifyMemInfo(MemBlockFlags::READ, read_, readp - origp, "SasVagDecoder");
		read_ += readp - origp;
	}
}

void VagDecoder::DoState(PointerWrap &p) {
	auto s = p.Section("VagDecoder", 1, 2);
	if (!s)
		return;

	if (s >= 2) {
		DoArray(p, samples, ARRAY_SIZE(samples));
	} else {
		int samplesOld[ARRAY_SIZE(samples)];
		DoArray(p, samplesOld, ARRAY_SIZE(samples));
		for (size_t i = 0; i < ARRAY_SIZE(samples); ++i) {
			samples[i] = samplesOld[i];
		}
	}
	Do(p, curSample);

	Do(p, data_);
	Do(p, read_);
	Do(p, curBlock_);
	Do(p, loopStartBlock_);
	Do(p, numBlocks_);

	Do(p, s_1);
	Do(p, s_2);

	Do(p, loopEnabled_);
	Do(p, loopAtNextBlock_);
	Do(p, end_);
}

// The context pointer is assumed to be valid.
int SasAtrac3::SetContext(u32 contextAddr) {
	contextAddr_ = contextAddr;
	// Note: On hardware, atracID_ is also stored in the loopNum member of the context.
	// But we don't actually mirror our struct to memory, so it doesn't really matter.
	atracID_ = AtracSasBindContextAndGetID(contextAddr);
	if (!sampleQueue_)
		sampleQueue_ = new BufferQueue();
	sampleQueue_->clear();
	end_ = false;
	return 0;
}

void SasAtrac3::GetNextSamples(s16 *outbuf, int wantedSamples) {
	if (atracID_ < 0) {
		end_ = true;
		return;
	}

	if (!buf_) {
		buf_ = new s16[0x800];
	}

	int finish = 0;
	int wantedbytes = wantedSamples * sizeof(s16);
	while (!finish && sampleQueue_->getQueueSize() < wantedbytes) {
		int numSamples = 0;
		AtracSasDecodeData(atracID_, (u8*)buf_, &numSamples, &finish);
		if (numSamples > 0)
			sampleQueue_->push((u8*)buf_, numSamples * sizeof(s16));
		else
			finish = 1;
	}
	sampleQueue_->pop_front((u8*)outbuf, wantedbytes);
	end_ = finish == 1;
}

int SasAtrac3::Concatenate(u32 bufPtr, u32 addbytes) {
	if (atracID_ >= 0) {
		AtracSasAddStreamData(atracID_, bufPtr, addbytes);
	}
	return 0;
}

void SasAtrac3::DoState(PointerWrap &p) {
	auto s = p.Section("SasAtrac3", 1, 2);
	if (!s)
		return;

	Do(p, contextAddr_);
	Do(p, atracID_);
	if (p.mode == p.MODE_READ && atracID_ >= 0 && !sampleQueue_) {
		sampleQueue_ = new BufferQueue();
	}
	if (s >= 2) {
		Do(p, end_);
	}
}

// http://code.google.com/p/jpcsp/source/browse/trunk/src/jpcsp/HLE/modules150/sceSasCore.java

static int simpleRate(int n) {
	n &= 0x7F;
	if (n == 0x7F) {
		return 0;
	}
	int rate = ((7 - (n & 0x3)) << 26) >> (n >> 2);
	if (rate == 0) {
		return 1;
	}
	return rate;
}

static int exponentRate(int n) {
	n &= 0x7F;
	if (n == 0x7F) {
		return 0;
	}
	int rate = ((7 - (n & 0x3)) << 24) >> (n >> 2);
	if (rate == 0) {
		return 1;
	}
	return rate;
}

static int getAttackRate(int bitfield1) {
	return simpleRate(bitfield1 >> 8);
}

static int getAttackType(int bitfield1) {
	return (bitfield1 & 0x8000) == 0 ? PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE : PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT;
}

static int getDecayRate(int bitfield1) {
	int n = (bitfield1 >> 4) & 0x000F;
	if (n == 0)
		return 0x7FFFFFFF;
	return 0x80000000 >> n;
}

static int getSustainType(int bitfield2) {
	return (bitfield2 >> 14) & 3;
}

static int getSustainRate(int bitfield2) {
	if (getSustainType(bitfield2) == PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE) {
		return exponentRate(bitfield2 >> 6);
	} else {
		return simpleRate(bitfield2 >> 6);
	}
}

static int getReleaseType(int bitfield2) {
	return (bitfield2 & 0x0020) == 0 ? PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE : PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE;
}

static int getReleaseRate(int bitfield2) {
	int n = bitfield2 & 0x001F;
	if (n == 31) {
		return 0;
	}
	if (getReleaseType(bitfield2) == PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE) {
		if (n == 30) {
			return 0x40000000;
		} else if (n == 29) {
			return 1;
		}
		return 0x10000000 >> n;
	}
	if (n == 0)
		return 0x7FFFFFFF;
	return 0x80000000 >> n;
}

static int getSustainLevel(int bitfield1) {
	return ((bitfield1 & 0x000F) + 1) << 26;
}

void ADSREnvelope::SetEnvelope(int flag, int a, int d, int s, int r) {
	if ((flag & 0x1) != 0)
		attackType = (SasADSRCurveMode)a;
	if ((flag & 0x2) != 0)
		decayType = (SasADSRCurveMode)d;
	if ((flag & 0x4) != 0)
		sustainType = (SasADSRCurveMode)s;
	if ((flag & 0x8) != 0)
		releaseType = (SasADSRCurveMode)r;

	if (PSP_CoreParameter().compat.flags().RockmanDash2SoundFix && sustainType == PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE) {
		sustainType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
	}
}

void ADSREnvelope::SetRate(int flag, int a, int d, int s, int r) {
	if ((flag & 0x1) != 0)
		attackRate = a;
	if ((flag & 0x2) != 0)
		decayRate = d;
	if ((flag & 0x4) != 0)
		sustainRate = s;
	if ((flag & 0x8) != 0)
		releaseRate = r;
}

void ADSREnvelope::SetSimpleEnvelope(u32 ADSREnv1, u32 ADSREnv2) {
	attackRate 		= getAttackRate(ADSREnv1);
	attackType 		= (SasADSRCurveMode)getAttackType(ADSREnv1);
	decayRate 		= getDecayRate(ADSREnv1);
	decayType 		= PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE;
	sustainRate 	= getSustainRate(ADSREnv2);
	sustainType 	= (SasADSRCurveMode)getSustainType(ADSREnv2);
	releaseRate 	= getReleaseRate(ADSREnv2);
	releaseType 	= (SasADSRCurveMode)getReleaseType(ADSREnv2);
	sustainLevel 	= getSustainLevel(ADSREnv1);

	if (PSP_CoreParameter().compat.flags().RockmanDash2SoundFix && sustainType == PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE) {
		sustainType = PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE;
	}

	if (attackRate < 0 || decayRate < 0 || sustainRate < 0 || releaseRate < 0) {
		ERROR_LOG_REPORT(Log::SasMix, "Simple ADSR resulted in invalid rates: %04x, %04x", ADSREnv1, ADSREnv2);
	}
}

SasInstance::SasInstance() {
	memset(&waveformEffect, 0, sizeof(waveformEffect));
	waveformEffect.type = PSP_SAS_EFFECT_TYPE_OFF;
	waveformEffect.isDryOn = 1;
	memset(mixTemp_, 0, sizeof(mixTemp_));  // just to avoid a static analysis warning.
}

SasInstance::~SasInstance() {
	ClearGrainSize();
}

void SasInstance::GetDebugText(char *text, size_t bufsize) {
	char voiceBuf[4096];
	voiceBuf[0] = '\0';
	char *p = voiceBuf;
	for (int i = 0; i < maxVoices; i++) {
		if (voices[i].playing) {
			uint32_t readAddr = voices[i].GetReadAddress();
			const char *indicator = "";
			switch (voices[i].type) {
			case VOICETYPE_VAG:
				if (readAddr < voices[i].vagAddr || readAddr > voices[i].vagAddr + voices[i].vagSize) {
					indicator = " (BAD!)";
				}
				break;
			default:
				break;
			}
			p += snprintf(p, sizeof(voiceBuf) - (p - voiceBuf), " %d: Pitch %04x L/R,FX: %d,%d|%d,%d VAG: %08x:%d:%08x%s Height:%d%%\n", i,
				voices[i].pitch, voices[i].volumeLeft, voices[i].volumeRight, voices[i].effectLeft, voices[i].effectRight,
				voices[i].vagAddr, voices[i].vagSize, voices[i].GetReadAddress(), indicator, (int)((int64_t)voices[i].envelope.GetHeight() * 100 / PSP_SAS_ENVELOPE_HEIGHT_MAX));
			p += snprintf(p, sizeof(voiceBuf) - (p - voiceBuf), "  - ADSR: %s/%s/%s/%s\n",
				ADSRCurveModeAsString(voices[i].envelope.attackType),
				ADSRCurveModeAsString(voices[i].envelope.decayType),
				ADSRCurveModeAsString(voices[i].envelope.sustainType),
				ADSRCurveModeAsString(voices[i].envelope.releaseType)
			);
		}
	}

	snprintf(text, bufsize,
		"SR: %d Mode: %s Grain: %d\n"
		"Effect: Type: %s Dry: %d Wet: %d L: %d R: %d Delay: %d Feedback: %d\n"
		"\n%s\n",
		sampleRate, outputMode == PSP_SAS_OUTPUTMODE_RAW ? "Raw" : "Mixed", grainSize,
		SasReverb::GetPresetName(waveformEffect.type), waveformEffect.isDryOn, waveformEffect.isWetOn, waveformEffect.leftVol, waveformEffect.rightVol, waveformEffect.delay, waveformEffect.feedback,
		voiceBuf);
}

void SasInstance::ClearGrainSize() {
	delete[] mixBuffer;
	delete[] sendBuffer;
	delete[] sendBufferDownsampled;
	delete[] sendBufferProcessed;
	mixBuffer = nullptr;
	sendBuffer = nullptr;
	sendBufferDownsampled = nullptr;
	sendBufferProcessed = nullptr;
}

void SasInstance::SetGrainSize(int newGrainSize) {
	grainSize = newGrainSize;

	// If you change the sizes here, don't forget DoState().
	delete[] mixBuffer;
	delete[] sendBuffer;
	delete[] sendBufferDownsampled;
	delete[] sendBufferProcessed;

	mixBuffer = new s32[grainSize * 2];
	sendBuffer = new s32[grainSize * 2];
	sendBufferDownsampled = new s16[grainSize];
	sendBufferProcessed = new s16[grainSize * 2];
	memset(mixBuffer, 0, sizeof(int) * grainSize * 2);
	memset(sendBuffer, 0, sizeof(int) * grainSize * 2);
	memset(sendBufferDownsampled, 0, sizeof(s16) * grainSize);
	memset(sendBufferProcessed, 0, sizeof(s16) * grainSize * 2);
}

// How long the Media Engine takes to mix one grain. Measured on a PSP (pspautotests
// audio/timing/sastiming) at 222MHz: a fixed 110us plus 0.49us per sample of grain, plus per
// playing voice and sample 0.445us + 0.0675us per unit of pitch ratio for VAG (a faster voice
// decodes more ADPCM), 0.405us + 0.0675us for PCM and 0.41us for noise, plus 0.64us per sample
// when a reverb type is set (the type doesn't matter; wet with no reverb type is free). Linear to
// within 1% from 64 to 2048 samples and 0 to 32 voices - 32 VAG voices at 512 samples take 8.7ms.
int SasInstance::EstimateMixUs() {
	float us = 110.0f + 0.49f * grainSize;
	for (int v = 0; v < PSP_SAS_VOICES_MAX; v++) {
		const SasVoice &voice = voices[v];
		if (!voice.playing || voice.paused) {
			continue;
		}
		const float pitchRatio = voice.pitch / (float)PSP_SAS_PITCH_BASE;
		float perSample;
		switch (voice.type) {
		case VOICETYPE_PCM: perSample = 0.405f + 0.0675f * pitchRatio; break;
		case VOICETYPE_NOISE:
		case VOICETYPE_TRIWAVE:
		case VOICETYPE_PULSEWAVE: perSample = 0.41f; break;
		default: perSample = 0.445f + 0.0675f * pitchRatio; break;
		}
		us += perSample * grainSize;
	}
	if (waveformEffect.type >= 0 && waveformEffect.isWetOn) {
		us += 0.64f * grainSize;
	}
	return PowerScaleFromDefaultClock((int)us);
}

void SasVoice::ReadSamples(s16 *output, int numSamples) {
	// Read N samples into the resample buffer. Could do either PCM or VAG here.
	switch (type) {
	case VOICETYPE_VAG:
		vag.GetSamples(output, numSamples);
		break;
	case VOICETYPE_PCM:
		{
			int needed = numSamples;
			s16 *out = output;
			while (needed > 0) {
				u32 size = std::min(pcmSize - pcmIndex, needed);
				if (!on) {
					pcmIndex = 0;
					break;
				}
				Memory::Memcpy(out, pcmAddr + pcmIndex * sizeof(s16), size * sizeof(s16), "SasVoicePCM");
#if COMMON_BIG_ENDIAN
				for (u32 i = 0; i < size; ++i) {
					s16_le sample;
					memcpy(&sample, out + i, sizeof(sample));
					out[i] = sample;
				}
#endif
				pcmIndex += size;
				needed -= size;
				out += size;
				if (pcmIndex >= pcmSize) {
					if (!loop) {
						// All out, quit.  We'll end in HaveSamplesEnded().
						break;
					}
					pcmIndex = pcmLoopPos;
				}
			}
			if (needed > 0) {
				memset(out, 0, needed * sizeof(s16));
			}
		}
		break;
	case VOICETYPE_ATRAC3:
		atrac3.GetNextSamples(output, numSamples);
		break;
	default:
		memset(output, 0, numSamples * sizeof(s16));
		break;
	}
}

bool SasVoice::HaveSamplesEnded() const {
	switch (type) {
	case VOICETYPE_VAG:
		return vag.End();

	case VOICETYPE_PCM:
		return pcmIndex >= pcmSize;

	case VOICETYPE_ATRAC3:
		return atrac3.End();

	default:
		return false;
	}
}

void SasInstance::MixVoice(SasVoice &voice) {
	switch (voice.type) {
	case VOICETYPE_VAG:
		if (voice.type == VOICETYPE_VAG && !voice.vagAddr)
			break;
		// else fallthrough! Don't change the check above.
		[[fallthrough]];
	case VOICETYPE_PCM:
		if (voice.type == VOICETYPE_PCM && !voice.pcmAddr)
			break;
		// else fallthrough! Don't change the check above.
		[[fallthrough]];
	default:
		// This feels a bit hacky.  The first 32 samples after a keyon are 0s.
		int delay = 0;
		if (voice.envelope.NeedsKeyOn()) {
			const bool ignorePitch = voice.type == VOICETYPE_PCM && voice.pitch > PSP_SAS_PITCH_BASE;
			delay = ignorePitch ? 32 : (32 * (u32)voice.pitch) >> PSP_SAS_PITCH_BASE_SHIFT;
			// VAG seems to have an extra sample delay (not shared by PCM.)
			if (voice.type == VOICETYPE_VAG)
				++delay;
		}

		// Resample to the correct pitch, writing exactly "grainSize" samples. We need a buffer that can
		// fit 4x that, as the max pitch is 0x4000.
		// TODO: Special case no-resample case (and 2x and 0.5x) for speed, it's not uncommon

		// Two passes: First read, then resample.
		mixTemp_[0] = voice.resampleHist[0];
		mixTemp_[1] = voice.resampleHist[1];

		int voicePitch = voice.pitch;
		u32 sampleFrac = voice.sampleFrac;
		int samplesToRead = (sampleFrac + voicePitch * std::max(0, grainSize - delay)) >> PSP_SAS_PITCH_BASE_SHIFT;
		if (samplesToRead > ARRAY_SIZE(mixTemp_) - 2) {
			ERROR_LOG(Log::sceSas, "Too many samples to read (%d)! This shouldn't happen.", samplesToRead);
			samplesToRead = ARRAY_SIZE(mixTemp_) - 2;
		}
		int readPos = 2;
		if (voice.envelope.NeedsKeyOn()) {
			readPos = 0;
			samplesToRead += 2;
		}
		voice.ReadSamples(&mixTemp_[readPos], samplesToRead);
		int tempPos = readPos + samplesToRead;

		for (int i = 0; i < delay; ++i) {
			// Walk the curve.  This means we'll reach ATTACK already, likely.
			// This matches the results of tests (but maybe we can just remove the STATE_KEYON_STEP hack.)
			voice.envelope.Step();
		}

		const bool needsInterp = voicePitch != PSP_SAS_PITCH_BASE || (sampleFrac & PSP_SAS_PITCH_MASK) != 0;
		for (int i = delay; i < grainSize; i++) {
			const int16_t *s = mixTemp_ + (sampleFrac >> PSP_SAS_PITCH_BASE_SHIFT);

			// Two-tap linear interpolation. The hardware does the same, unlike the PSX there's no bicubic lookup table etc.
			int sample = s[0];
			if (needsInterp) {
				int f = sampleFrac & PSP_SAS_PITCH_MASK;
				sample = s[0] - (((s[0] - s[1]) * f) >> PSP_SAS_PITCH_BASE_SHIFT);
			}
			sampleFrac += voicePitch;

			// The maximum envelope height (PSP_SAS_ENVELOPE_HEIGHT_MAX) is (1 << 30) - 1.
			// Reduce it to 14 bits, by shifting off 15.  Round up by adding (1 << 14) first.
			int envelopeValue = voice.envelope.GetHeight();
			voice.envelope.Step();
			envelopeValue = (envelopeValue + (1 << 14)) >> 15;

			// We just scale by the envelope before we scale by volumes.
			// Again, we round up by adding (1 << 14) first (*after* multiplying.)
			sample = ((sample * envelopeValue) + (1 << 14)) >> 15;

			// We mix into this 32-bit temp buffer and clip in a second loop
			// Ideally, the shift right should be there too but for now I'm concerned about
			// not overflowing.
			mixBuffer[i * 2] += (sample * voice.volumeLeft) >> 12;
			mixBuffer[i * 2 + 1] += (sample * voice.volumeRight) >> 12;
			sendBuffer[i * 2] += sample * voice.effectLeft >> 12;
			sendBuffer[i * 2 + 1] += sample * voice.effectRight >> 12;
		}

		voice.resampleHist[0] = mixTemp_[tempPos - 2];
		voice.resampleHist[1] = mixTemp_[tempPos - 1];

		voice.sampleFrac = sampleFrac - (tempPos - 2) * PSP_SAS_PITCH_BASE;

		if (voice.HaveSamplesEnded())
			voice.envelope.End();
		if (voice.envelope.HasEnded()) {
			// NOTICE_LOG(Log::SasMix, "Hit end of envelope");
			voice.playing = false;
			voice.on = false;
		}
	}
}

// Copies voices to and from the self-contained records of SasMixKernel.h (mixed on an SPU on the PS3).
struct SasKernelBridge {
	// False if the voice needs SasInstance::MixVoice (ATRAC3, or memory the kernel can't vouch for).
	static bool Export(const SasVoice &voice, SasKVoice &k) {
		memset(&k, 0, sizeof(k));
		switch (voice.type) {
		case VOICETYPE_VAG: {
			const VagDecoder &vag = voice.vag;
			if (!vag.end_) {
				const u32 bytes = (u32)vag.numBlocks_ * 16;
				if (vag.numBlocks_ < 0 || !Memory::IsValidRange(vag.read_, bytes) || !Memory::IsValidRange(vag.data_, bytes) || vag.read_ < vag.data_)
					return false;
				if (vag.curSample < 0 || vag.curSample > 28 || (vag.loopAtNextBlock_ && vag.loopStartBlock_ < -1))
					return false;
				k.dataEa = (uint64_t)(uintptr_t)Memory::GetPointerUnchecked(vag.data_);
				k.dataBytes = bytes;
			}
			k.type = SASK_TYPE_VAG;
			k.vagCurSample = vag.curSample;
			k.vagReadOff = (int32_t)(vag.read_ - vag.data_);
			k.vagCurBlock = vag.curBlock_;
			k.vagLoopStartBlock = vag.loopStartBlock_;
			k.vagNumBlocks = vag.numBlocks_;
			k.vagS1 = vag.s_1;
			k.vagS2 = vag.s_2;
			memcpy(k.vagSamples, vag.samples, sizeof(k.vagSamples));
			k.vagLoopEnabled = vag.loopEnabled_;
			k.vagLoopAtNextBlock = vag.loopAtNextBlock_;
			k.vagEnd = vag.end_;
			break;
		}
		case VOICETYPE_PCM:
			if (voice.pcmSize <= 0 || voice.pcmIndex < 0 || voice.pcmIndex > voice.pcmSize)
				return false;
			if (voice.loop && (voice.pcmLoopPos < 0 || voice.pcmLoopPos >= voice.pcmSize))
				return false;
			if (!Memory::IsValidRange(voice.pcmAddr, (u32)voice.pcmSize * 2))
				return false;
			k.type = SASK_TYPE_PCM;
			k.dataEa = (uint64_t)(uintptr_t)Memory::GetPointerUnchecked(voice.pcmAddr);
			k.dataBytes = (u32)voice.pcmSize * 2;
			k.pcmSize = voice.pcmSize;
			k.pcmIndex = voice.pcmIndex;
			k.pcmLoopPos = voice.pcmLoopPos;
			break;
		case VOICETYPE_ATRAC3:
			return false;
		default:
			k.type = 0;  // Silence, but the envelope runs.
			break;
		}
		k.pitch = voice.pitch;
		k.sampleFrac = voice.sampleFrac;
		k.volumeLeft = voice.volumeLeft;
		k.volumeRight = voice.volumeRight;
		k.effectLeft = voice.effectLeft;
		k.effectRight = voice.effectRight;
		k.resampleHist[0] = voice.resampleHist[0];
		k.resampleHist[1] = voice.resampleHist[1];
		k.loop = voice.loop;
		k.on = voice.on;
		k.playing = voice.playing;
		const ADSREnvelope &e = voice.envelope;
		k.height = e.height_;
		k.envState = (int32_t)e.state_;
		k.attackRate = e.attackRate;
		k.decayRate = e.decayRate;
		k.sustainRate = e.sustainRate;
		k.sustainLevel = e.sustainLevel;
		k.releaseRate = e.releaseRate;
		k.attackType = e.attackType;
		k.decayType = e.decayType;
		k.sustainType = e.sustainType;
		k.releaseType = e.releaseType;
		return true;
	}

	static void Import(const SasKVoice &k, SasVoice &voice) {
		if (voice.type == VOICETYPE_VAG) {
			VagDecoder &vag = voice.vag;
			vag.curSample = k.vagCurSample;
			vag.read_ = vag.data_ + (u32)k.vagReadOff;
			vag.curBlock_ = k.vagCurBlock;
			vag.loopStartBlock_ = k.vagLoopStartBlock;
			vag.s_1 = k.vagS1;
			vag.s_2 = k.vagS2;
			memcpy(vag.samples, k.vagSamples, sizeof(vag.samples));
			vag.loopAtNextBlock_ = k.vagLoopAtNextBlock != 0;
			vag.end_ = k.vagEnd != 0;
		} else if (voice.type == VOICETYPE_PCM) {
			voice.pcmIndex = k.pcmIndex;
		}
		voice.sampleFrac = k.sampleFrac;
		voice.resampleHist[0] = k.resampleHist[0];
		voice.resampleHist[1] = k.resampleHist[1];
		voice.on = k.on != 0;
		voice.playing = k.playing != 0;
		voice.envelope.height_ = k.height;
		voice.envelope.state_ = (ADSREnvelope::ADSRState)k.envState;
	}
};

static SasKVoice g_kernelVoices[PSP_SAS_VOICES_MAX] __attribute__((aligned(128)));

#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
// Test build (PPSSPP_SAS_FUZZ=n): mixes n random voices with MixVoice and with the kernel and
// compares every grain. Uses guest memory at 0x08400000 (the volatile area) for sample data.
static void SasKernelFuzz(SasInstance *sas, int scenarios) {
	uint32_t rng = 12345;
	auto rnd = [&](uint32_t n) { rng = rng * 1103515245u + 12345u; return (uint32_t)((rng >> 8) % n); };
	const u32 base = 0x08400000;
	int bad = 0;
	long grains = 0;
	const int savedGrain = sas->GetGrainSize();
	for (int sc = 0; sc < scenarios; ++sc) {
		const int grain = 64 * (1 + rnd(16));
		sas->SetGrainSize(grain);
		// Sample data: VAG blocks or PCM.
		const bool pcm = rnd(3) == 0;
		const int blocks = 1 + rnd(200);
		const u32 addr = base + 16 * rnd(1000);
		u8 *mem = Memory::GetPointerWriteUnchecked(addr);
		for (int b = 0; b < blocks; ++b) {
			u8 *blk = mem + b * 16;
			for (int i = 0; i < 16; ++i) blk[i] = (u8)rnd(256);
			blk[0] = (u8)((rnd(16) << 4) | rnd(16));
			const uint32_t fl = rnd(40);
			blk[1] = fl == 0 ? 6 : fl == 1 ? 3 : fl == 2 ? 7 : (u8)rnd(3) == 0 ? (u8)rnd(8) : 0;
		}
		SasVoice a, b;
		for (SasVoice *v : { &a, &b }) {
			uint32_t save = rng;
			v->type = pcm ? VOICETYPE_PCM : (rnd(10) == 0 ? VOICETYPE_NOISE : VOICETYPE_VAG);
			v->vagAddr = addr; v->vagSize = blocks * 16;
			v->pcmAddr = addr; v->pcmSize = blocks * 8; v->pcmIndex = 0;
			v->loop = rnd(2) != 0;
			v->pcmLoopPos = rnd(blocks * 8);
			v->pitch = rnd(4) == 0 ? 0x1000 : (int)rnd(0x4001);
			v->volumeLeft = (int)rnd(0x2001) - 0x1000; v->volumeRight = (int)rnd(0x2001) - 0x1000;
			v->effectLeft = (int)rnd(0x2001) - 0x1000; v->effectRight = (int)rnd(0x2001) - 0x1000;
			if (rnd(2))
				v->envelope.SetSimpleEnvelope(rnd(0x10000), rnd(0x10000));
			v->envelope.SetEnvelope(15, rnd(6), rnd(6), rnd(6), rnd(6));
			const int rates[] = { 0, 1, 0x100, 0x10000, 0x1000000, 0x7FFFFFFF, (int)rnd(0x80000000u) };
			v->envelope.SetRate(15, rates[rnd(7)], rates[rnd(7)], rates[rnd(7)], rates[rnd(7)]);
			v->envelope.SetSustainLevel(rnd(0x40000001));
			v->KeyOn();
			if (v == &b) {
				// Same draws as for a.
			} else {
				rng = save;
			}
		}
		for (int g = 0; g < 40 && (a.playing || b.playing); ++g) {
			if (rnd(15) == 0) { a.KeyOff(); b.KeyOff(); }
			std::vector<int32_t> om(grain * 2), os(grain * 2), km(grain * 2), ks(grain * 2);
			memset(sas->mixBuffer, 0, grain * 8);
			memset(sas->sendBuffer, 0, grain * 8);
			if (a.playing) sas->MixVoice(a);
			memcpy(om.data(), sas->mixBuffer, grain * 8);
			memcpy(os.data(), sas->sendBuffer, grain * 8);
			memset(sas->mixBuffer, 0, grain * 8);
			memset(sas->sendBuffer, 0, grain * 8);
			static SasKVoice k __attribute__((aligned(128)));
			if (b.playing && SasKernelBridge::Export(b, k)) {
				SasKernelMix(&k, 1, grain, km.data(), ks.data());
				SasKernelBridge::Import(k, b);
			}
			++grains;
			static SasKVoice ea __attribute__((aligned(128))), eb __attribute__((aligned(128)));
			SasKernelBridge::Export(a, ea);
			SasKernelBridge::Export(b, eb);
			if (om != km || os != ks || memcmp(&ea, &eb, sizeof(ea)) != 0 || a.playing != b.playing) {
				if (bad++ < 10)
					printf("SAS FUZZ: scenario %d grain %d type %d pitch %x differs (mix %d send %d state %d)\n", sc, g, (int)a.type, a.pitch,
						om != km, os != ks, memcmp(&ea, &eb, sizeof(ea)) != 0);
				break;
			}
		}
	}
	printf("SAS FUZZ: %d scenarios, %ld grains, %d differ\n", scenarios, grains, bad);
	sas->SetGrainSize(savedGrain);
}
#endif

bool g_sasMixOnPpu = false;

void SasInstance::MixVoices() {
#if PPSSPP_ARCH(POWERPC)
	const bool useKernel = !g_sasMixOnPpu;
#else
	const bool useKernel = false;
#endif
	const bool debug = SoundDebugOn();
	int playing = 0;
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	static bool fuzzed = false;
	if (!fuzzed && getenv("PPSSPP_SAS_FUZZ")) {
		fuzzed = true;
		SasKernelFuzz(this, atoi(getenv("PPSSPP_SAS_FUZZ")));
	}
#endif
	int kernelIndex[PSP_SAS_VOICES_MAX];
	int count = 0;
	for (int v = 0; v < PSP_SAS_VOICES_MAX; v++) {
		SasVoice &voice = voices[v];
		if (!voice.playing || voice.paused)
			continue;
		// Exactly the voices MixVoice would skip.
		if ((voice.type == VOICETYPE_VAG && !voice.vagAddr) || (voice.type == VOICETYPE_PCM && !voice.pcmAddr))
			continue;
		if (debug) {
			SoundDebugStats::Voice &d = g_soundDebug.voice[v];
			d.mixes++;
			d.type = (int)voice.type;
			d.volL = std::max(d.volL, voice.volumeLeft);
			d.volR = std::max(d.volR, voice.volumeRight);
			d.effL = std::max(d.effL, voice.effectLeft);
			d.effR = std::max(d.effR, voice.effectRight);
			d.env = std::max(d.env, voice.envelope.GetHeight());
			d.pitch = voice.pitch;
			playing++;
		}
		if (useKernel && SasKernelBridge::Export(voice, g_kernelVoices[count])) {
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
			// Test build: mix with both and compare (PPSSPP_SAS_COMPARE=1).
			static const bool compare = getenv("PPSSPP_SAS_COMPARE") != nullptr;
			if (compare) {
				static SasKVoice k __attribute__((aligned(128)));
				k = g_kernelVoices[count];
				std::vector<int32_t> km(grainSize * 2), ks(grainSize * 2), om(grainSize * 2), os(grainSize * 2);
				SasKernelMix(&k, 1, grainSize, km.data(), ks.data());
				std::vector<int32_t> savedMix(mixBuffer, mixBuffer + grainSize * 2), savedSend(sendBuffer, sendBuffer + grainSize * 2);
				memset(mixBuffer, 0, grainSize * 8);
				memset(sendBuffer, 0, grainSize * 8);
				MixVoice(voice);
				memcpy(om.data(), mixBuffer, grainSize * 8);
				memcpy(os.data(), sendBuffer, grainSize * 8);
				for (int i = 0; i < grainSize * 2; ++i) {
					mixBuffer[i] = savedMix[i] + om[i];
					sendBuffer[i] = savedSend[i] + os[i];
				}
				static SasKVoice after __attribute__((aligned(128)));
				SasKernelBridge::Export(voice, after);
				static int reports = 0;
				const bool sameState = after.sampleFrac == k.sampleFrac && after.height == k.height && after.envState == k.envState &&
					after.vagReadOff == k.vagReadOff && after.vagCurSample == k.vagCurSample && after.vagCurBlock == k.vagCurBlock &&
					after.vagEnd == k.vagEnd && after.pcmIndex == k.pcmIndex && after.playing == k.playing && after.on == k.on &&
					after.resampleHist[0] == k.resampleHist[0] && after.resampleHist[1] == k.resampleHist[1] &&
					after.vagS1 == k.vagS1 && after.vagS2 == k.vagS2;
				static long compared = 0;
				++compared;
				if ((om != km || os != ks || !sameState) && reports++ < 20)
					printf("SAS COMPARE: voice %d type %d differs (mix %d, send %d, state %d) frac %u/%u height %lld/%lld state %d/%d read %d/%d\n", v, (int)voice.type,
						om != km, os != ks, !sameState, after.sampleFrac, k.sampleFrac, (long long)after.height, (long long)k.height, after.envState, k.envState, after.vagReadOff, k.vagReadOff);
				if ((compared & 1023) == 0 || compared == 1)
					printf("SAS COMPARE: %ld voice mixes compared, %d differ\n", compared, reports);
				continue;
			}
#endif
			kernelIndex[count++] = v;
		} else {
			MixVoice(voice);
		}
	}
	if (debug)
		g_soundDebug.sasVoicesMax = std::max(g_soundDebug.sasVoicesMax, playing);
	if (count == 0)
		return;
	SasKernelMix(g_kernelVoices, count, grainSize, mixBuffer, sendBuffer);
	for (int i = 0; i < count; ++i)
		SasKernelBridge::Import(g_kernelVoices[i], voices[kernelIndex[i]]);
}

void SasInstance::Mix(u32 outAddr, u32 inAddr, int leftVol, int rightVol, bool mute) {
	PS3_PROFILE(sasMix);
	MixVoices();

	// Apply mute if needed (note: we try to keep everything else identical to the non-muted case).
	if (mute) {
		memset(mixBuffer, 0, grainSize * sizeof(int) * 2);
		memset(sendBuffer, 0, grainSize * sizeof(int) * 2);
	}

	// Then mix the send buffer in with the rest.

	// Alright, all voices mixed. Let's convert and clip, and at the same time, wipe mixBuffer for next time. Could also dither.
	s16_le *outp = (s16_le *)Memory::GetPointerWriteRangeOrException(outAddr, 4 * grainSize);
	const s16_le *inp = inAddr ? (const s16_le *)Memory::GetPointerRangeOrException(inAddr, 4 * grainSize) : 0;
	// Sound debug: the game's buffer is often the output too (mixed in place), so it is read first.
	const bool debug = SoundDebugOn() && outp && outputMode == PSP_SAS_OUTPUTMODE_MIXED;
	static std::vector<int> debugIn;
	if (debug) {
		debugIn.assign(grainSize * 2, 0);
		if (inp) {
			for (int i = 0; i < grainSize * 2; i += 2) {
				debugIn[i] = ((int)(s16)inp[i] * leftVol) >> 12;
				debugIn[i + 1] = ((int)(s16)inp[i + 1] * rightVol) >> 12;
			}
		}
	}
	if (!outp) {
		WARN_LOG_REPORT(Log::sceSas, "Bad SAS Mix output address: %08x, grain=%d", outAddr, grainSize);
	} else if (outputMode == PSP_SAS_OUTPUTMODE_MIXED) {
		// Okay, apply effects processing to the Send buffer.
		WriteMixedOutput(outp, inp, leftVol, rightVol);
		if (MemBlockInfoDetailed()) {
			if (inp)
				NotifyMemInfo(MemBlockFlags::READ, inAddr, grainSize * sizeof(u16) * 2, "SasMix");
			NotifyMemInfo(MemBlockFlags::WRITE, outAddr, grainSize * sizeof(u16) * 2, "SasMix");
		}
	} else {
		s16_le *outpL = outp + grainSize * 0;
		s16_le *outpR = outp + grainSize * 1;
		s16_le *outpSendL = outp + grainSize * 2;
		s16_le *outpSendR = outp + grainSize * 3;
		WARN_LOG_REPORT_ONCE(sasraw, Log::SasMix, "sceSasCore: raw outputMode");
		for (int i = 0; i < grainSize * 2; i += 2) {
			*outpL++ = clamp_s16(mixBuffer[i + 0]);
			*outpR++ = clamp_s16(mixBuffer[i + 1]);
			*outpSendL++ = clamp_s16(sendBuffer[i + 0]);
			*outpSendR++ = clamp_s16(sendBuffer[i + 1]);
		}
		NotifyMemInfo(MemBlockFlags::WRITE, outAddr, grainSize * sizeof(u16) * 4, "SasMix");
	}
	if (SoundDebugOn()) {
		SoundDebugStats &d = g_soundDebug;
		d.sasMixes++;
		d.sasGrain = grainSize;
		d.sasOutputMode = outputMode;
		d.sasEffectType = waveformEffect.type;
		d.sasDry = waveformEffect.isDryOn != 0;
		d.sasWet = waveformEffect.isWetOn != 0;
		d.effectVolL = waveformEffect.leftVol;
		d.effectVolR = waveformEffect.rightVol;
		const bool wet = d.sasWet && debug;
		for (int i = 0; i < grainSize * 2; ++i) {
			const int64_t dry = mixBuffer[i];
			const int64_t send = wet ? sendBufferProcessed[i] : 0;
			d.sasDryPeak = std::max(d.sasDryPeak, dry < 0 ? -dry : dry);
			d.sasWetPeak = std::max(d.sasWetPeak, send < 0 ? -send : send);
			if (debug) {
				const int in = debugIn[i];
				d.sasInPeak = std::max(d.sasInPeak, in < 0 ? -in : in);
				const int64_t out = (int64_t)in + (d.sasDry ? dry : 0) + send;
				const int64_t a = out < 0 ? -out : out;
				d.sasOutPeak = std::max(d.sasOutPeak, a);
				if (out > 32767 || out < -32768) d.sasClipped++;
			}
		}
		d.sasSamples += grainSize * 2;
		if (inp) {
			d.sasWithInput++;
			d.sasInVolL = leftVol;
			d.sasInVolR = rightVol;
		}
	}
	memset(mixBuffer, 0, grainSize * sizeof(int) * 2);
	memset(sendBuffer, 0, grainSize * sizeof(int) * 2);
}

// Note: leftVol/rightVol here are how much to scale the inp content by, not the mixBuffer.
void SasInstance::WriteMixedOutput(s16_le *outp, const s16_le *inp, int leftVol, int rightVol) {
	const bool dry = waveformEffect.isDryOn != 0;
	const bool wet = waveformEffect.isWetOn != 0;
	if (wet) {
		ApplyWaveformEffect();
	}

	if (inp) {
		for (int i = 0; i < grainSize * 2; i += 2) {
			int sampleL = ((*inp++) * leftVol >> 12);
			int sampleR = ((*inp++) * rightVol >> 12);
			if (dry) {
				sampleL += mixBuffer[i + 0];
				sampleR += mixBuffer[i + 1];
			}
			if (wet) {
				sampleL += sendBufferProcessed[i + 0];
				sampleR += sendBufferProcessed[i + 1];
			}
			*outp++ = clamp_s16(sampleL);
			*outp++ = clamp_s16(sampleR);
		}
	} else {
		// These are the optimal cases.
		if (dry && wet) {
			for (int i = 0; i < grainSize * 2; i += 2) {
				*outp++ = clamp_s16(mixBuffer[i + 0] + sendBufferProcessed[i + 0]);
				*outp++ = clamp_s16(mixBuffer[i + 1] + sendBufferProcessed[i + 1]);
			}
		} else if (dry) {
			for (int i = 0; i < grainSize * 2; i += 2) {
				*outp++ = clamp_s16(mixBuffer[i + 0]);
				*outp++ = clamp_s16(mixBuffer[i + 1]);
			}
		} else {
			// This is another uncommon case, dry must be off but let's keep it for clarity.
			for (int i = 0; i < grainSize * 2; i += 2) {
				int sampleL = 0;
				int sampleR = 0;
				if (dry) {
					sampleL += mixBuffer[i + 0];
					sampleR += mixBuffer[i + 1];
				}
				if (wet) {
					sampleL += sendBufferProcessed[i + 0];
					sampleR += sendBufferProcessed[i + 1];
				}
				*outp++ = clamp_s16(sampleL);
				*outp++ = clamp_s16(sampleR);
			}
		}
	}
}

void SasInstance::SetWaveformEffectType(int type) {
	if (type != waveformEffect.type) {
		waveformEffect.type = type;
		reverb_.SetPreset(type);
	}
}

void SasInstance::SetWaveformEffectParams(int delay, int feedback) {
	waveformEffect.delay = delay;
	waveformEffect.feedback = feedback;
	// Echo and Delay compute most of their parameters from these; the rest ignore them.
	reverb_.SetParams(delay, feedback);
}

// http://psx.rules.org/spu.txt has some information about setting up the delay time by modifying the delay preset.
// See http://report.ppsspp.org/logs/kind/772 for a list of games that use different types. Maybe can help us figure out
// which is which.
void SasInstance::ApplyWaveformEffect() {
	// First, downsample the send buffer to 22khz. We do this naively for now.
	for (int i = 0; i < grainSize / 2; i++) {
		sendBufferDownsampled[i * 2] = clamp_s16(sendBuffer[i * 4]);
		sendBufferDownsampled[i * 2 + 1] = clamp_s16(sendBuffer[i * 4 + 1]);
	}

	// Volume max is 0x1000, while our factor is up to 0x8000. Shifting left by 3 fixes that.
	reverb_.ProcessReverb(sendBufferProcessed, sendBufferDownsampled, grainSize / 2, (uint16_t)(waveformEffect.leftVol << 3), (uint16_t)(waveformEffect.rightVol << 3));
}

void SasInstance::DoState(PointerWrap &p) {
	auto s = p.Section("SasInstance", 1);
	if (!s)
		return;

	Do(p, grainSize);
	if (p.mode == p.MODE_READ) {
		if (grainSize > PSP_SAS_MAX_GRAIN) {
			ERROR_LOG(Log::SaveState, "Bad SAS grain size %d", grainSize);
			p.SetError(p.ERROR_FAILURE);
			return;
		}
		if (grainSize > 0) {
			SetGrainSize(grainSize);
		} else {
			ClearGrainSize();
		}
	}

	Do(p, maxVoices);
	Do(p, sampleRate);
	Do(p, outputMode);

	// SetGrainSize() / ClearGrainSize() should've made our buffers match.
	if (mixBuffer != NULL && grainSize > 0) {
		DoArray(p, mixBuffer, grainSize * 2);
	}
	if (sendBuffer != NULL && grainSize > 0) {
		DoArray(p, sendBuffer, grainSize * 2);
	}
	if (sendBuffer != NULL && grainSize > 0) {
		// Backwards compat
		int16_t *resampleBuf = new int16_t[grainSize * 4 + 3]();
		DoArray(p, resampleBuf, grainSize * 4 + 3);
		delete[] resampleBuf;
	}

	int n = PSP_SAS_VOICES_MAX;
	Do(p, n);
	if (n != PSP_SAS_VOICES_MAX) {
		ERROR_LOG(Log::SaveState, "Wrong number of SAS voices");
		p.SetError(p.ERROR_FAILURE);
		return;
	}
	DoArray(p, voices, ARRAY_SIZE(voices));
	Do(p, waveformEffect);
	if (p.mode == p.MODE_READ) {
		reverb_.SetPreset(waveformEffect.type);
		// SetPreset() alone would leave Echo/Delay at their defaults, since those two compute
		// their parameters from the delay/feedback we just restored.
		reverb_.SetParams(waveformEffect.delay, waveformEffect.feedback);
	}
}

void SasVoice::Reset() {
	resampleHist[0] = 0;
	resampleHist[1] = 0;
}

void SasVoice::KeyOn() {
	envelope.KeyOn();
	switch (type) {
	case VOICETYPE_VAG:
		if (Memory::IsValidAddress(vagAddr)) {
			vag.Start(vagAddr, vagSize, loop);
		} else {
			ERROR_LOG(Log::SasMix, "Invalid VAG address %08x", vagAddr);
			return;
		}
		break;
	default:
		break;
	}
	playing = true;
	on = true;
	paused = false;
	sampleFrac = 0;
}

void SasVoice::KeyOff() {
	on = false;
	envelope.KeyOff();
}

void SasVoice::DoState(PointerWrap &p) {
	auto s = p.Section("SasVoice", 1, 3);
	if (!s)
		return;

	Do(p, playing);
	Do(p, paused);
	Do(p, on);

	Do(p, type);

	Do(p, vagAddr);
	Do(p, vagSize);
	Do(p, pcmAddr);
	Do(p, pcmSize);
	Do(p, pcmIndex);
	if (s >= 2) {
		Do(p, pcmLoopPos);
	} else {
		pcmLoopPos = 0;
	}
	Do(p, sampleRate);

	Do(p, sampleFrac);
	Do(p, pitch);
	Do(p, loop);
	if (s < 2 && type == VOICETYPE_PCM) {
		// We set loop incorrectly before, and always looped.
		// Let's keep always looping, since it's usually right.
		loop = true;
	}

	Do(p, noiseFreq);

	Do(p, volumeLeft);
	Do(p, volumeRight);
	if (s < 3) {
		// There were extra variables here that were for the same purpose.
		Do(p, effectLeft);
		Do(p, effectRight);
	}
	Do(p, effectLeft);
	Do(p, effectRight);
	DoArray(p, resampleHist, ARRAY_SIZE(resampleHist));

	envelope.DoState(p);
	vag.DoState(p);
	atrac3.DoState(p);
}

void ADSREnvelope::WalkCurve(int type, int rate) {
	s64 expDelta;
	switch (type) {
	case PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE:
		height_ += rate;
		break;

	case PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE:
		height_ -= rate;
		break;

	case PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT:
		if (height_ <= (s64)PSP_SAS_ENVELOPE_HEIGHT_MAX * 3 / 4) {
			height_ += rate;
		} else {
			height_ += rate / 4;
		}
		break;

	case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE:
		expDelta = height_ - PSP_SAS_ENVELOPE_HEIGHT_MAX;
		// Flipping the sign so that we can shift in the top bits.
		expDelta += (-expDelta * rate) >> 32;
		height_ = expDelta + PSP_SAS_ENVELOPE_HEIGHT_MAX - (rate + 3UL) / 4UL;
		break;

	case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_INCREASE:
		expDelta = height_ - PSP_SAS_ENVELOPE_HEIGHT_MAX;
		// Flipping the sign so that we can shift in the top bits.
		expDelta += (-expDelta * rate) >> 32;
		height_ = expDelta + 0x4000 + PSP_SAS_ENVELOPE_HEIGHT_MAX;
		break;

	case PSP_SAS_ADSR_CURVE_MODE_DIRECT:
		height_ = rate;  // Simple :)
		break;
	}
}

void ADSREnvelope::SetState(ADSRState state) {
	if (height_ > PSP_SAS_ENVELOPE_HEIGHT_MAX) {
		height_ = PSP_SAS_ENVELOPE_HEIGHT_MAX;
	}
	// TODO: Also check for height_ < 0 and set to 0?
	state_ = state;
}

inline void ADSREnvelope::Step() {
	switch (state_) {
	case STATE_ATTACK:
		WalkCurve(attackType, attackRate);
		if (height_ >= PSP_SAS_ENVELOPE_HEIGHT_MAX || height_ < 0)
			SetState(STATE_DECAY);
		break;
	case STATE_DECAY:
		WalkCurve(decayType, decayRate);
		if (height_ < sustainLevel)
			SetState(STATE_SUSTAIN);
		break;
	case STATE_SUSTAIN:
		WalkCurve(sustainType, sustainRate);
		if (height_ <= 0) {
			height_ = 0;
			SetState(STATE_RELEASE);
		}
		break;
	case STATE_RELEASE:
		WalkCurve(releaseType, releaseRate);
		if (height_ <= 0) {
			height_ = 0;
			SetState(STATE_OFF);
		}
		break;
	case STATE_OFF:
		// Do nothing
		break;

	case STATE_KEYON:
		height_ = 0;
		SetState(STATE_KEYON_STEP);
		break;
	case STATE_KEYON_STEP:
		// This entire state is pretty much a hack to reproduce PSP behavior.
		// The STATE_KEYON state is a real state, but not sure how it switches.
		// It takes 32 steps at 0 for keyon to "kick in", 31 should shift to 0 anyway.
		height_++;
		if (height_ >= 31) {
			height_ = 0;
			SetState(STATE_ATTACK);
		}
		break;
	}
}

void ADSREnvelope::KeyOn() {
	SetState(STATE_KEYON);
}

void ADSREnvelope::KeyOff() {
	SetState(STATE_RELEASE);
}

void ADSREnvelope::End() {
	SetState(STATE_OFF);
	height_ = 0;
}

void ADSREnvelope::DoState(PointerWrap &p) {
	auto s = p.Section("ADSREnvelope", 1, 2);
	if (!s) {
		return;
	}

	Do(p, attackRate);
	Do(p, decayRate);
	Do(p, sustainRate);
	Do(p, releaseRate);
	Do(p, attackType);
	Do(p, decayType);
	Do(p, sustainType);
	Do(p, sustainLevel);
	Do(p, releaseType);
	if (s < 2) {
		Do(p, state_);
		if (state_ == 4) {
			state_ = STATE_OFF;
		}
		int stepsLegacy;
		Do(p, stepsLegacy);
	} else {
		Do(p, state_);
	}
	Do(p, height_);
}

const char *ADSRCurveModeAsString(SasADSRCurveMode mode) {
	switch (mode) {
	case PSP_SAS_ADSR_CURVE_MODE_DIRECT: return "D";
	case PSP_SAS_ADSR_CURVE_MODE_LINEAR_INCREASE: return "L+";
	case PSP_SAS_ADSR_CURVE_MODE_LINEAR_DECREASE: return "L-";
	case PSP_SAS_ADSR_CURVE_MODE_LINEAR_BENT: return "LB";
	case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_DECREASE: return "E-";
	case PSP_SAS_ADSR_CURVE_MODE_EXPONENT_INCREASE: return "E+";
	default: return "N/A";
	}
}
