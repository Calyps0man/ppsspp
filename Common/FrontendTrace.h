// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Notable events the frontend's own log should have (PS3: launch.log). The hook may be null.
extern void (*g_frontendTraceHook)(const char *text);
void FrontendTrace(const char *format, ...)
#ifdef __GNUC__
	__attribute__((format(printf, 1, 2)))
#endif
	;

// Groups of frontend log lines that can be switched off (PS3: the Debug tab). The frontend sets the
// mask; all on by default.
enum class FrontendLog : unsigned {
	VIDEO = 1 << 0,     // Video playback statistics, once a second during a cutscene.
	SAVEDATA = 1 << 1,  // Save dialog steps.
	GE_WATCH = 1 << 2,  // Display list memory overwritten while queued, sub-lists in VRAM.
	RSX = 1 << 3,       // RSX renderer events (framebuffer layouts, write-backs, depth reads...).
	JIT = 1 << 4,       // JIT cache clears and recompiled blocks.
	SOUND = 1 << 5,     // Sound debug: where samples reach full scale (clipping), every 2 s.
};
extern unsigned g_frontendLogMask;
inline bool FrontendLogOn(FrontendLog group) {
	return g_frontendTraceHook && (g_frontendLogMask & (unsigned)group) != 0;
}
