// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#pragma once
#include <string>
#include <vector>

// Runs on the main thread from the menu (no emulation active). Returns report lines; also written
// to USRDIR/rsx_selftest.txt or rsx_vramtest.txt and launch.log.
// Optional progress display while a test runs (called at most ~10 times a second from the
// test's thread, between RSX work). Draw something and flip; return false to cancel the test.
typedef bool (*RSXProgressCallback)(const char *label, double fraction);
void RSXSetProgressCallback(RSXProgressCallback callback);

std::vector<std::string> RSXRunSelfTest(bool vram);

// Renders the same blended, depth-tested scene over and over for `seconds` and compares every
// frame bit-for-bit with the first one (unstable RSX = "GLOD" symptoms). rsx_stresstest.txt.
std::vector<std::string> RSXRunStressTest(double seconds);

// HDMI / AV multi out: connected port, display (EDID) modes, accepted resolutions, current mode,
// measured vblank rate, temperatures. video_report.txt.
std::vector<std::string> RSXVideoReport();
std::string RSXTemperatureText();
// Celsius; false if neither can be read.
bool RSXTemperatures(unsigned *cell, unsigned *rsx);

// Quick probe run when the hardware renderer starts: measures, instead of assuming, the screen
// orientation and the byte order of vertex colors and textures, so the renderer can compensate.
struct RSXCalibration {
	bool ok = false;        // Readback worked and the results made sense.
	bool flipY = false;     // Pixel row 0 is at the bottom in NDC terms.
	// Vertex color byte i must hold channel colorOrder[i] (0=R, 1=G, 2=B, 3=A).
	unsigned char colorOrder[4] = {0, 1, 2, 3};
	// Texture memory byte texOrder[j] lands in output byte j (A, R, G, B from the top).
	unsigned char texOrder[4] = {0, 1, 2, 3};
	std::string summary;
};
RSXCalibration RSXCalibrate();

// Startup check of the hardware T&L vertex program (lit color, position) and of which triangle
// winding the RSX culls.
struct RSXTLTestResult {
	bool ok = false;
	bool positiveCrossIsCCW = true;  // A positive PSP screen-space cross product is RSX CCW (front).
	std::string summary;
};
RSXTLTestResult RSXTestTL();
