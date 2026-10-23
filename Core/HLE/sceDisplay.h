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

#pragma once

#include "Core/MemMap.h"

enum GEBufferFormat : uint8_t;

extern int g_displayMinVblanksPerFlip;
extern int g_displayVideoCapFps;
// Frame cap counters for the PS3 frontend's perf log (reset by the reader).
struct DisplayCapStats {
	unsigned frames;       // New frames set while the cap is on.
	unsigned inInterrupt;  // Of those, set from an interrupt handler (can't be held there).
	unsigned held;         // Held in sceDisplaySetFrameBuf.
	unsigned long long heldUs;
	unsigned waitsExtended;  // Vblank waits made longer.
};
extern DisplayCapStats g_displayCapStats;
void __DisplayInit();
void __DisplayDoState(PointerWrap &p);
void __DisplayShutdown();

void Register_sceDisplay();

// Get information about the current framebuffer.
bool __DisplayGetFramebuf(PSPPointer<u8> *topaddr, u32 *linesize, GEBufferFormat *pixelFormat, int mode);
void __DisplaySetFramebuf(u32 topaddr, int linesize, int pixelformat, int sync);

// Call this when resuming to avoid a small speedup burst
void __DisplaySetWasPaused();

// The frame rate throttling is actually aiming for right now, after fast-forward, the alternative
// speeds, and the netplay/achievement restrictions have all had their say. 0 means unlimited.
int __DisplayGetFrameTimingLimit();

void Register_sceDisplay_driver();
void __DisplayWaitForVblanks(const char* reason, int vblanks, bool callbacks = false);
// A video frame was written (frame cap off while videos play).
// videoPts: the picture's timestamp (90 kHz units), -1 if unknown.
void __DisplayNoteVideoFrame(s64 videoPts = -1);
bool __DisplayVideoPlaying();

// Kinda hacky, but what can you do... we can't pass it through the jit etc.
struct DisplayLayoutConfig;
void __DisplaySetDisplayLayoutConfig(const DisplayLayoutConfig &config);
