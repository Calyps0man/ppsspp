// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Experimental hardware renderer: the software GPU still decodes display lists, transforms, lights
// and clips; every resulting primitive is drawn by the RSX into its own copies of the PSP's
// framebuffers, optionally at 2x or 3x resolution. PSP VRAM stays the source of truth between
// sync points: framebuffers are uploaded from VRAM when the CPU changed them, and written back
// to VRAM when something reads them (block transfers, render-to-texture, CLUT loads, and every
// frame unless "VRAM sync: minimal" is chosen).
#pragma once

#include <cstdint>
#include <string>

#include "GPU/Software/HardwareBackend.h"

class RSXRenderer;

struct RSXRendererOptions {
	int scale = 1;              // Internal resolution multiplier, 1..3.
	bool syncEveryFrame = true; // Write rendered framebuffers back to PSP VRAM every frame.
	bool bilinear = true;       // Output scaling filter.
	int screenMode = 0;         // PS3Frontend::ScreenMode.
	bool batched = true;        // Fast batched path for whole draws (skips the per-triangle software path).
	bool hardwareTL = true;     // Batched 3D draws transformed and lit by an RSX vertex program.
	// Diagnostics switches (frontend settings menu), each back to an older behavior:
	bool textureCheckFull = false; // Check texture contents on every use (before Frontend 42: no address-range shortcut).
	bool writeBackAllRows = false; // Write back all rows of a render target (before Frontend 41: not just the drawn rows).
	bool rttCopyReuse = true;      // Reuse the copy of a render target a draw samples across TEXFLUSH (Frontend 40).
	bool tiled = true;             // Render targets in tiled regions (Frontend 56).
	bool tiledDepth = true;        // Depth buffers too (with tiled; Frontend 236).
	// A depth buffer read as a texture gets the previous read's depth (copied without waiting for
	// the RSX) instead of stopping the RSX to fetch the current one.
	bool lateDepth = false;
	// A buffer address drawn in another layout (stride/format) keeps its old surface on the RSX,
	// not written back to PSP memory, and gets it again when the game switches back (Midnight Club
	// L.A. switches one buffer between two layouts several times a frame; each switch waited for
	// the RSX to finish everything). Pixels drawn in one layout aren't seen in the other's memory.
	bool keepLayouts = false;
	// "Lens flare occlusion" off: a depth buffer read as a texture is not written back from the
	// RSX (the game reads whatever PSP memory has there). Midnight Club reads its depth once a
	// frame, to hide the sun's lens flare behind buildings.
	bool skipDepthReads = false;
	// "Edge smoothing": FXAA over the finished picture at the internal resolution, before it is
	// scaled to the TV (a pass on the RSX in Present).
	int fxaa = 0;  // 0 off, 1 Full, 2 Light (shorter search, half strength).
	// A framebuffer the RSX drew into and the CPU wrote to afterwards gets the CPU's rows before
	// it is read as a texture or shown (Silent Hill Origins' loading screen showed only its top part).
	bool cpuRows = false;
};

// Null if the RSX could not be set up (the reason is in launch.log).
RSXRenderer *RSXRendererCreate(const RSXRendererOptions &options);
void RSXRendererDestroy(RSXRenderer *renderer);
// Shows the PSP's current display framebuffer, with an optional text line on top.
void RSXRendererPresent(RSXRenderer *renderer, const std::string &overlay);
std::string RSXRendererStats(RSXRenderer *renderer);
Rasterizer::HardwareBackend *RSXRendererBackend(RSXRenderer *renderer);
// Only while the GE worker is idle (in-game options).
void RSXRendererSetLiveOptions(RSXRenderer *renderer, bool bilinear, bool keepLayouts, bool skipDepthReads, int fxaa);
// Writes every buffer the RSX drew into back to PSP memory. Call with the GE worker idle (GESync).
void RSXRendererWriteBackAll(RSXRenderer *renderer);
// "CPU writes to drawn buffers" (RSXRendererOptions::cpuRows), changed while a game runs.
void RSXRendererSetCpuRows(RSXRenderer *renderer, bool on);
// 192x144 font atlas (12x24 glyphs for ASCII 32..126, 16 per row), one byte per pixel (0 = clear).
void RSXRendererSetFont(const uint8_t *atlas);
