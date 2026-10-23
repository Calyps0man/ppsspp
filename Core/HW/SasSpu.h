// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#pragma once

#include "Core/HW/SasMixKernel.h"

// Mixes count voices (SasKVoice records, updated in place) and adds the result into mix/send
// (int32[grainSize * 2] each). On the PS3 this runs on an SPU; elsewhere, or if the SPU can't be
// used, on the calling thread. voices must be 128-byte aligned.
void SasKernelMix(SasKVoice *voices, int count, int grainSize, int32_t *mix, int32_t *send);

// For the PS3 perf log: where sceSas voices are mixed.
const char *SasMixerDescription();
// Stops the SPU (end of a game; the SPU threads must be joined before the app exits).
void SasSpuShutdown();

// PS3: wait for an SPU mix by polling its done word (no lv2 sleep and wake-up).
extern bool g_sasSpuPoll;
