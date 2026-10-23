// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// RSX Diagnostics: a standalone PS3 app (no emulator) that runs the PPSSPP port's RSX checks -
// rendering self-test, VRAM test with a full memory sweep, a stress test, a video output report
// (HDMI / AV multi) and on-screen test patterns - to help diagnose failing RSX hardware ("GLOD").
//
// The screen is drawn by the CPU straight into our own two display buffers and shown with a plain
// flip - no SDL and no RSX drawing commands for the UI - so the UI keeps working (and shows
// progress) whatever state the RSX tests leave the 3D or 2D engines in.
#include <io/pad.h>
#include <sys/file.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <malloc.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

#include "PS3/frontend/Frontend.h"
#include "PS3/rsx/RSXCore.h"
#include "PS3/rsx/RSXSelfTest.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define DIAG_NAME "RSX Diagnostics 1.5"

static const char *base = "/dev_hdd0/game/RSXD00001/USRDIR/";
static volatile bool running = true;
static padData pad;
static std::atomic<bool> eventThreadStop{false};
static sys_ppu_thread_t eventThread;

// ---------------------------------------------------------------------------------------------
// Logging: USRDIR/diag.log, plus a copy on the first USB drive if one is plugged in.

static void RawTrace(const char *line, unsigned length, bool reset = false) {
	const char *paths[] = {"/dev_hdd0/game/RSXD00001/USRDIR/diag.log", "/dev_usb000/RSX_DIAG.log"};
	for (const char *path : paths) {
		s32 fd = -1;
		if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | (reset ? SYS_O_TRUNC : SYS_O_APPEND), &fd, 0666, nullptr, 0) == 0) {
			u64 written = 0;
			sysLv2FsWrite(fd, line, length, &written);
			sysLv2FsFsync(fd);
			sysLv2FsClose(fd);
		}
	}
}

static void EarlyTrace() __attribute__((constructor(101)));
static void EarlyTrace() {
	const char message[] = DIAG_NAME ": started\n";
	RawTrace(message, sizeof(message) - 1, true);
}

void PS3Frontend::Trace(const char *format, ...) {
	char message[512];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message) - 1, format, args);
	va_end(args);
	size_t n = strlen(message);
	message[n++] = '\n';
	RawTrace(message, n);
}

const char *PS3Frontend::ProductName() { return DIAG_NAME; }
std::string PS3Frontend::ReportPath(const char *name) { return std::string(base) + name; }

// ---------------------------------------------------------------------------------------------
// System events (XMB). Serviced on a thread so long tests never block the PS button menu.

static void SystemCallback(u64 status, u64, void *) {
	if (status == SYSUTIL_EXIT_GAME) running = false;
}

static void SystemEventThread(void *) {
	double exitAt = 0.0;
	while (!eventThreadStop) {
		sysUtilCheckCallback();
		if (!running) {
			u64 sec = 0, nsec = 0;
			sysGetCurrentTime(&sec, &nsec);
			const double now = sec + nsec * 1e-9;
			if (exitAt == 0.0) exitAt = now;
			// A test in progress can take a minute; the XMB expects us gone within seconds.
			if (now - exitAt > 3.0) {
				PS3Frontend::Trace("Exit requested during a test; exiting directly");
				sysProcessExit(0);
			}
		}
		usleep(16 * 1000);
	}
	sysThreadExit(0);
}

// ---------------------------------------------------------------------------------------------
// Screen: CPU drawing into the back buffer. Layout coordinates are for 1280x720 and scaled.

static uint8_t g_font[192 * 144];  // 1 = glyph pixel. 16x6 glyphs of 12x24 (ASCII 32..127).
static int g_w = 0, g_h = 0, g_pitch = 0;  // Display size, pitch in pixels.
static uint32_t *g_fb = nullptr;           // Current back buffer.
static float g_scale = 1.0f;               // Layout -> display pixels.

static bool LoadFont() {
	FILE *f = fopen((std::string(base) + "font.bmp").c_str(), "rb");
	if (!f) return false;
	uint8_t header[54];
	bool ok = fread(header, 1, 54, f) == 54 && header[0] == 'B' && header[1] == 'M';
	if (ok) {
		const uint32_t offset = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
		const int w = header[18] | (header[19] << 8), h = header[22] | (header[23] << 8);
		const int bpp = header[28];
		ok = w == 192 && h == 144 && bpp == 24;
		if (ok) {
			const int rowBytes = (w * 3 + 3) & ~3;
			std::vector<uint8_t> row(rowBytes);
			fseek(f, offset, SEEK_SET);
			for (int y = h - 1; y >= 0 && ok; --y) {  // Bottom-up.
				ok = fread(row.data(), 1, rowBytes, f) == (size_t)rowBytes;
				for (int x = 0; x < w; ++x)
					g_font[y * 192 + x] = (row[x * 3] + row[x * 3 + 1] + row[x * 3 + 2]) > 384 ? 1 : 0;
			}
		}
	}
	fclose(f);
	return ok;
}

static inline int S(float v) { return (int)(v * g_scale + 0.5f); }

static void BeginFrame() {
	RSX::WaitFlip(0.25);
	g_fb = (uint32_t *)RSX::BackBufferPtr();
}

static void FillRect(int x, int y, int w, int h, uint32_t color) {  // Display pixels.
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > g_w) w = g_w - x;
	if (y + h > g_h) h = g_h - y;
	if (w <= 0 || h <= 0) return;
	for (int yy = y; yy < y + h; ++yy) {
		uint32_t *row = g_fb + yy * g_pitch + x;
		for (int xx = 0; xx < w; ++xx) row[xx] = color;
	}
}

static void Rect(float x, float y, float w, float h, uint32_t color) {  // Layout units.
	FillRect(S(x), S(y), S(x + w) - S(x), S(y + h) - S(y), color);
}

static void Clear(uint32_t color = 0xFF0C121C) {
	FillRect(0, 0, g_w, g_h, color);
}

static void Text(float x, float y, const char *value, float size = 1.0f, uint32_t color = 0xFFFFFFFF) {
	const float gw = 12.0f * size * g_scale, gh = 24.0f * size * g_scale;
	const int iw = (int)(gw + 0.5f), ih = (int)(gh + 0.5f);
	float px = x * g_scale;
	const int py = S(y);
	for (; *value; ++value, px += gw) {
		unsigned c = (unsigned char)*value;
		if (c < 32 || c > 127) c = '?';
		c -= 32;
		const int sx0 = (c % 16) * 12, sy0 = (c / 16) * 24;
		const int ox = (int)(px + 0.5f);
		for (int yy = 0; yy < ih; ++yy) {
			const int dy = py + yy;
			if (dy < 0 || dy >= g_h) continue;
			const int sy = sy0 + yy * 24 / ih;
			uint32_t *row = g_fb + dy * g_pitch;
			for (int xx = 0; xx < iw; ++xx) {
				const int dx = ox + xx;
				if (dx < 0 || dx >= g_w) continue;
				if (g_font[sy * 192 + sx0 + xx * 12 / iw])
					row[dx] = color;
			}
		}
	}
}

static void EndFrame() {
	RSX::Present();
}

static bool InitScreen() {
	gcmContextData *context = nullptr;
	// Our wrapper (RSXCore) enlarges these and sets up the I/O region.
	if (rsxInit(&context, 0x10000, 1024 * 1024, nullptr) != 0) {
		PS3Frontend::Trace("rsxInit failed");
		return false;
	}
	videoState state{};
	videoResolution res{};
	if (videoGetState(0, 0, &state) != 0 || videoGetResolution(state.displayMode.resolution, &res) != 0) {
		PS3Frontend::Trace("Could not read the video mode");
		return false;
	}
	videoConfiguration config{};
	config.resolution = state.displayMode.resolution;
	config.format = VIDEO_BUFFER_FORMAT_XRGB;
	config.pitch = res.width * 4;
	if (videoConfigure(0, &config, nullptr, 0) != 0)
		PS3Frontend::Trace("videoConfigure failed (continuing with the current mode)");
	for (int i = 0; i < 100; ++i) {
		if (videoGetState(0, 0, &state) == 0 && state.state != VIDEO_STATE_BUSY) break;
		usleep(10 * 1000);
	}
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	if (!RSX::InitDisplay()) return false;
	g_w = RSX::DisplayWidth();
	g_h = RSX::DisplayHeight();
	g_pitch = (int)(RSX::DisplayPitch() / 4);
	g_scale = (float)g_h / 720.0f;
	PS3Frontend::Trace("Screen: %dx%d (layout scale %.2f)", g_w, g_h, g_scale);
	return true;
}

void PS3Frontend::Status(const char *message) {
	Trace("Status: %s", message);
	BeginFrame();
	Clear();
	Text(50, 50, DIAG_NAME, 2);
	Text(50, 155, std::string(message).substr(0, 94).c_str());
	Text(50, 625, "Please wait...");
	EndFrame();
}

static void ReadPad() {
	padInfo2 info{};
	int port = -1;
	if (ioPadGetInfo2(&info) == 0)
		for (int i = 0; i < 7; ++i) if (info.port_status[i] & 1) { port = i; break; }
	if (port < 0) { memset(&pad, 0, sizeof(pad)); return; }
	padData data{};
	if (ioPadGetData(port, &data) == 0 && data.len > 0) pad = data;
}

struct MenuKeys {
	bool up = false, down = false, left = false, right = false, cross = false, circle = false;
	padData last{};
	void Update() {
		ReadPad();
		up = pad.BTN_UP && !last.BTN_UP; down = pad.BTN_DOWN && !last.BTN_DOWN;
		left = pad.BTN_LEFT && !last.BTN_LEFT; right = pad.BTN_RIGHT && !last.BTN_RIGHT;
		cross = pad.BTN_CROSS && !last.BTN_CROSS; circle = pad.BTN_CIRCLE && !last.BTN_CIRCLE;
		last = pad;
	}
};

static void ShowReport(MenuKeys &keys, const char *title, const std::vector<std::string> &lines) {
	size_t top = 0;
	const size_t visible = 17;
	while (running) {
		keys.Update();
		if (keys.circle || keys.cross) break;
		if (keys.down && top + visible < lines.size()) top++;
		if (keys.up && top > 0) top--;
		if (keys.right) top = std::min(top + visible, lines.size() > visible ? lines.size() - visible : 0);
		if (keys.left) top = top > visible ? top - visible : 0;
		BeginFrame();
		Clear();
		Text(50, 30, title, 2);
		for (size_t i = top; i < lines.size() && i < top + visible; ++i) {
			const std::string &line = lines[i];
			const bool fail = line.compare(0, 4, "FAIL") == 0 || line.find("BUSY") != std::string::npos || line.find("NO VBLANKS") != std::string::npos;
			const bool pass = line.compare(0, 4, "PASS") == 0;
			Text(40, 100 + (float)(i - top) * 30, line.substr(0, 100).c_str(), 1.0f, fail ? 0xFFFF5A5A : (pass ? 0xFF78F08C : 0xFFC8F0FF));
		}
		Text(40, 640, "Up/Down: scroll   Left/Right: page   X/Circle: back   (saved in USRDIR)");
		EndFrame();
	}
}

// Progress screen, drawn between test steps. Also keeps the display flipping (the PS button
// menu needs that). Circle cancels.
static double g_testStarted = 0.0;
static double NowSeconds() {
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec + nsec * 1e-9;
}
static bool TestProgress(const char *label, double fraction) {
	const double now = NowSeconds();
	if (g_testStarted == 0.0) g_testStarted = now;
	ReadPad();
	BeginFrame();
	Clear();
	Text(50, 40, DIAG_NAME, 2);
	const std::string text(label);
	for (size_t i = 0, row = 0; i < text.size() && row < 3; i += 94, ++row)
		Text(50, 150 + (float)row * 34, text.substr(i, 94).c_str());
	Rect(50, 290, 1180, 40, 0xFF5A6478);
	Rect(54, 294, 1172, 32, 0xFF0C121C);
	Rect(54, 294, (float)(1172 * fraction), 32, 0xFF5ADC8C);
	char status[96];
	snprintf(status, sizeof(status), "%3.0f%%   %.0f s elapsed", fraction * 100.0, now - g_testStarted);
	Text(50, 350, status);
	Text(50, 625, "Circle: cancel");
	EndFrame();
	return running && !pad.BTN_CIRCLE;
}

// ---------------------------------------------------------------------------------------------
// Test patterns, drawn at the display's native resolution.

static void DrawPattern(int pattern) {
	static const uint32_t bars[8] = {0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00, 0xFFFF00FF, 0xFFFF0000, 0xFF0000FF, 0xFF000000};
	for (int y = 0; y < g_h; ++y) {
		uint32_t *row = g_fb + y * g_pitch;
		for (int x = 0; x < g_w; ++x) {
			uint32_t c;
			switch (pattern) {
			case 0: c = bars[x * 8 / g_w]; break;
			case 1: {
				const uint32_t v = (uint32_t)(x * 32 / g_w) * 255 / 31;
				const int band = y * 4 / g_h;
				c = 0xFF000000 | (band == 0 ? (v << 16 | v << 8 | v) : band == 1 ? v << 16 : band == 2 ? v << 8 : v);
				break;
			}
			case 2: c = ((x ^ y) & 1) ? 0xFFFFFFFF : 0xFF000000; break;
			case 3: c = (x % 64 == 0 || y % 64 == 0 || x == g_w - 1 || y == g_h - 1) ? 0xFFFFFFFF : 0xFF000000; break;
			case 4: c = 0xFFFF0000; break;
			case 5: c = 0xFF00FF00; break;
			case 6: c = 0xFF0000FF; break;
			case 7: c = 0xFFFFFFFF; break;
			default: c = 0xFF000000; break;
			}
			row[x] = c;
		}
	}
}

static void TestPatterns(MenuKeys &keys) {
	static const char *names[] = {"Color bars", "Ramps (grey, red, green, blue)", "1-pixel checkerboard",
		"Grid (64 px) with border", "Solid red", "Solid green", "Solid blue", "Solid white", "Solid black"};
	const int count = 9;
	int pattern = 0;
	int labelFrames = 150;
	while (running) {
		keys.Update();
		if (keys.circle) break;
		if (keys.right || keys.cross) { pattern = (pattern + 1) % count; labelFrames = 150; }
		if (keys.left) { pattern = (pattern + count - 1) % count; labelFrames = 150; }
		if (keys.up || keys.down) labelFrames = 150;
		BeginFrame();
		DrawPattern(pattern);
		if (labelFrames > 0) {
			labelFrames--;
			Rect(30, 630, 1220, 60, 0xFF000000);
			Text(40, 648, (std::string(names[pattern]) + "   Left/Right/X: next   Circle: back   Up: show this").c_str());
		}
		EndFrame();
	}
}

// ---------------------------------------------------------------------------------------------
// Menu

static std::vector<std::string> RunAll() {
	std::vector<std::string> all;
	auto append = [&all](const std::vector<std::string> &lines) {
		all.insert(all.end(), lines.begin(), lines.end());
		all.push_back("");
	};
	g_testStarted = 0.0;
	PS3Frontend::Status("1/4 Video output report...");
	append(RSXVideoReport());
	append(RSXRunSelfTest(false));
	append(RSXRunSelfTest(true));
	append(RSXRunStressTest(60.0));
	int failed = 0;
	for (const auto &l : all) if (l.compare(0, 4, "FAIL") == 0) failed++;
	char summary[128];
	snprintf(summary, sizeof(summary), "Summary: %d failed check(s). %s", failed,
		failed ? "See the FAIL lines below." : "No faults found.");
	all.insert(all.begin(), summary);
	all.insert(all.begin(), DIAG_NAME " - full report");
	FILE *file = fopen(PS3Frontend::ReportPath("rsx_diag_report.txt").c_str(), "w");
	if (file) {
		for (const auto &line : all) fprintf(file, "%s\n", line.c_str());
		fclose(file);
	}
	return all;
}

static void MainMenu() {
	MenuKeys keys;
	ReadPad();
	keys.last = pad;
	int selection = 0;
	const char *items[] = {
		"Run all tests (about 2 minutes) -> rsx_diag_report.txt",
		"Video output report (HDMI / AV multi, modes, refresh)",
		"RSX self-test (rendering building blocks)",
		"VRAM test (bandwidth + full sweep of free RSX memory)",
		"RSX stress test - 60 seconds",
		"RSX stress test - 10 minutes (heat soak)",
		"Screen test patterns",
		"Exit",
	};
	const int count = 8;
	std::string temperature = RSXTemperatureText();
	double lastTemperature = NowSeconds();
	const std::string describe = "RSX: " + RSX::Describe();
	while (running) {
		keys.Update();
		if (keys.up) selection = (selection + count - 1) % count;
		if (keys.down) selection = (selection + 1) % count;
		if (keys.cross) {
			std::vector<std::string> lines;
			const char *title = items[selection];
			g_testStarted = 0.0;
			PS3Frontend::Trace("Menu: %s", items[selection]);
			switch (selection) {
			case 0: lines = RunAll(); title = "Full report"; break;
			case 1: PS3Frontend::Status("Reading video output information..."); lines = RSXVideoReport(); title = "Video output"; break;
			case 2: lines = RSXRunSelfTest(false); title = "RSX self-test"; break;
			case 3: lines = RSXRunSelfTest(true); title = "VRAM test"; break;
			case 4: lines = RSXRunStressTest(60.0); title = "RSX stress test"; break;
			case 5: lines = RSXRunStressTest(600.0); title = "RSX stress test (10 min)"; break;
			case 6: TestPatterns(keys); continue;
			default: running = false; continue;
			}
			if (running) ShowReport(keys, title, lines);
			continue;
		}
		if (NowSeconds() - lastTemperature > 2.0) {
			temperature = RSXTemperatureText();
			lastTemperature = NowSeconds();
		}
		BeginFrame();
		Clear();
		Text(50, 40, DIAG_NAME, 2);
		for (int i = 0; i < count; ++i)
			Text(50, 130 + (float)i * 44, ((i == selection ? "> " : "  ") + std::string(items[i])).c_str(), 1.0f,
				i == selection ? 0xFF5AF08C : 0xFFFFFFFF);
		Text(50, 500, describe.substr(0, 100).c_str());
		Text(50, 535, ("Temperature: " + temperature).c_str());
		Text(50, 580, "Reports are saved to /dev_hdd0/game/RSXD00001/USRDIR (diag.log, *.txt)");
		Text(50, 625, "Up/Down: select   X: run");
		EndFrame();
	}
}

int main(int, char **) {
	PS3Frontend::Trace("Main entered");
	if (chdir(base) != 0) PS3Frontend::Trace("Cannot access installed USRDIR");
	if (!LoadFont()) PS3Frontend::Trace("Font load failed (text will be invisible)");
	if (!InitScreen()) return 10;
	RSX::InitDiagnostics();
	PS3Frontend::Trace("RSX: %s", RSX::Describe().c_str());
	if (sysUtilRegisterCallback(1, SystemCallback, nullptr) != 0 || ioPadInit(7) != 0) { PS3Frontend::Trace("Pad/callback setup failed"); return 15; }
	RSXSetProgressCallback(TestProgress);
	if (sysThreadCreate(&eventThread, SystemEventThread, nullptr, 900, 0x4000, THREAD_JOINABLE, (char *)"sysutil_events") != 0)
		PS3Frontend::Trace("Event thread failed");

	MainMenu();

	PS3Frontend::Trace("Exiting");
	eventThreadStop = true;
	u64 ret;
	sysThreadJoin(eventThread, &ret);
	sysUtilUnregisterCallback(1);
	ioPadEnd();
	RSX::Finish(0.5);
	return 0;
}
