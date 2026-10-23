// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace PS3Audio {

bool Start();
void Stop();
// From the emulator thread: stereo frames (PSP mixer output, 44.1 kHz).
void Push(const int32_t *samples, int frames, float volume);
void SetEnabled(bool enabled);
// When the emulator is slower than real time: 0 gaps (fades), 1 wait and refill, 2 play slower.
void SetSlowMode(int mode);
std::string StatsText();

}  // namespace PS3Audio
