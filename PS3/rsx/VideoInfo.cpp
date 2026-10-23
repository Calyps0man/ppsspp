// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Video output report: what the console thinks is connected (HDMI, component, composite, ...),
// what the TV/EDID says it supports, which resolutions the system will actually accept, the
// current configuration and the measured refresh rate. A console with a failing RSX ("GLOD")
// often shows up first here: modes missing, outputs stuck BUSY, no vblanks.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sys/systime.h>

#include "PS3/rsx/RSXSelfTest.h"
#include "PS3/frontend/Frontend.h"

namespace {

double Now() {
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return (double)sec + (double)nsec * 1e-9;
}

const char *PortName(int port) {
	switch (port) {
	case VIDEO_PORT_NONE: return "none";
	case VIDEO_PORT_HDMI: return "HDMI";
	case VIDEO_PORT_NETWORK: return "network (remote play)";
	case VIDEO_PORT_COMPOSITE: return "composite (AV multi)";
	case VIDEO_PORT_D: return "D-terminal (AV multi)";
	case VIDEO_PORT_COMPONENT: return "component (AV multi)";
	case VIDEO_PORT_RGB: return "RGB (AV multi)";
	case VIDEO_PORT_SCART: return "SCART (AV multi)";
	case VIDEO_PORT_DSUB: return "D-sub/VGA (AV multi)";
	default: return "unknown";
	}
}

const char *StateName(int s) {
	switch (s) {
	case VIDEO_STATE_DISABLED: return "disabled";
	case VIDEO_STATE_ENABLED: return "enabled";
	case VIDEO_STATE_BUSY: return "BUSY";
	default: return "unknown";
	}
}

std::string ColorSpaceText(int cs) {
	std::string s;
	if (cs & VIDEO_COLOR_RGB) s += "RGB ";
	if (cs & VIDEO_COLOR_YUV) s += "YUV ";
	if (cs & VIDEO_COLOR_XVYCC) s += "xvYCC ";
	if (s.empty()) s = "none ";
	char hex[16]; snprintf(hex, sizeof(hex), "(0x%02x)", cs);
	return s + hex;
}

std::string ResolutionText(int id) {
	videoResolution r{};
	char text[64];
	if (videoGetResolution(id, &r) == 0)
		snprintf(text, sizeof(text), "%ux%u%s", r.width, r.height, id >= 0x80 ? " 3D" : "");
	else
		snprintf(text, sizeof(text), "id %d", id);
	return text;
}

std::string RatesText(int rates) {
	std::string s;
	if (rates & 1) s += "59.94 ";
	if (rates & 2) s += "50 ";
	if (rates & 4) s += "60 ";
	if (rates & 8) s += "30 ";
	if (s.empty()) s = "auto ";
	s.pop_back();
	return s + " Hz";
}

std::string ModeText(const videoDisplayMode &m) {
	char text[128];
	snprintf(text, sizeof(text), "%s %s, aspect %s, %s%s", ResolutionText(m.resolution).c_str(),
		m.scanMode ? "progressive" : "interlaced",
		m.aspect == VIDEO_ASPECT_4_3 ? "4:3" : m.aspect == VIDEO_ASPECT_16_9 ? "16:9" : "auto",
		RatesText(m.refreshRates).c_str(), m.conversion ? ", converted" : "");
	return text;
}

// lv2 sys_game_get_temperature (383): 0 = Cell, 1 = RSX. Not available on every firmware.
bool Temperature(int device, unsigned *celsius) {
	u32 value = 0;
	lv2syscall2(383, (u64)device, (u64)(uintptr_t)&value);
	if ((s32)p1 != 0) return false;
	*celsius = value >> 24;
	return *celsius > 0 && *celsius < 150;
}

}  // namespace

bool RSXTemperatures(unsigned *cell, unsigned *rsx) {
	const bool c = Temperature(0, cell), r = Temperature(1, rsx);
	return c || r;
}

std::string RSXTemperatureText() {
	unsigned cell = 0, rsx = 0;
	bool c = Temperature(0, &cell), r = Temperature(1, &rsx);
	char text[96];
	if (!c && !r) return "temperatures not readable on this firmware";
	snprintf(text, sizeof(text), "Cell %s%u C, RSX %s%u C", c ? "" : "n/a ", c ? cell : 0, r ? "" : "n/a ", r ? rsx : 0);
	return text;
}

std::vector<std::string> RSXVideoReport() {
	std::vector<std::string> lines;
	char text[300];
#define OUT(...) do { snprintf(text, sizeof(text), __VA_ARGS__); lines.push_back(text); PS3Frontend::Trace("Video: %s", text); } while (0)

	OUT("%s - video output report", PS3Frontend::ProductName());
	const char *names[1] = {"HDMI / AV multi out"};
	for (u32 out = VIDEO_PRIMARY; out <= VIDEO_PRIMARY; ++out) {
		s32 count = videoGetNumberOfDevice(out);
		OUT("== Output %s: %d device(s)%s", names[out], count, count < 0 ? " (error)" : "");
		for (s32 dev = 0; dev < count && dev < 4; ++dev) {
			videoState st{};
			s32 rs = videoGetState(out, dev, &st);
			if (rs == 0)
				OUT("Device %d state: %s, color space %s, mode %s", dev, StateName(st.state),
					ColorSpaceText(st.colorSpace).c_str(), ModeText(st.displayMode).c_str());
			else
				OUT("Device %d state: FAILED (0x%08x)", dev, (unsigned)rs);

			static videoDeviceInfo info;  // ~1 KB; static keeps it off the small stack.
			memset(&info, 0, sizeof(info));
			s32 ri = videoGetDeviceInfo(out, dev, &info);
			if (ri != 0) { OUT("Device %d info: FAILED (0x%08x)", dev, (unsigned)ri); continue; }
			OUT("Device %d: port %s (0x%02x), state %s, color space %s, latency %u ms, RGB range %s",
				dev, PortName(info.portType), info.portType, StateName(info.state),
				ColorSpaceText(info.colorSpace).c_str(), info.latency,
				info.rgbOutputRange == 0 ? "limited" : info.rgbOutputRange == 1 ? "full" : "?");
			const videoColorInfo &ci = info.colorInfo;
			if (ci.whiteX || ci.whiteY || ci.gamma)
				OUT("  EDID chromaticity R(%u,%u) G(%u,%u) B(%u,%u) W(%u,%u), gamma %u",
					ci.redX, ci.redY, ci.greenX, ci.greenY, ci.blueX, ci.blueY, ci.whiteX, ci.whiteY, ci.gamma);
			else
				OUT("  EDID chromaticity: none reported%s", info.portType == VIDEO_PORT_HDMI ? " (TV EDID not read?)" : "");
			OUT("  %u mode(s) reported by the display:", info.availableModeCount);
			for (int m = 0; m < info.availableModeCount && m < 32; ++m)
				OUT("    %s", ModeText(info.availableModes[m]).c_str());
			if (info.portType == VIDEO_PORT_HDMI)
				OUT("  HDCP receiver keys (KSV): %u", info.ksvList.count);
		}

		videoConfiguration config{};
		s32 rc = videoGetConfiguration(out, &config, nullptr);
		if (rc == 0)
			OUT("Current configuration: %s, format %s, aspect %s, pitch %u", ResolutionText(config.resolution).c_str(),
				config.format == VIDEO_BUFFER_FORMAT_XRGB ? "XRGB" : config.format == VIDEO_BUFFER_FORMAT_XBGR ? "XBGR" : config.format == VIDEO_BUFFER_FORMAT_FLOAT ? "float" : "?",
				config.aspect == VIDEO_ASPECT_4_3 ? "4:3" : config.aspect == VIDEO_ASPECT_16_9 ? "16:9" : "auto", config.pitch);
		else
			OUT("Current configuration: FAILED (0x%08x)", (unsigned)rc);
		if (out == VIDEO_SECONDARY) break;

		// Which resolutions the system would accept for this output.
		static const int ids[] = {VIDEO_RESOLUTION_1080, VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576,
			VIDEO_RESOLUTION_1600x1080, VIDEO_RESOLUTION_1440x1080, VIDEO_RESOLUTION_1280x1080, VIDEO_RESOLUTION_960x1080,
			VIDEO_RESOLUTION_720_3D_FRAME_PACKING};
		static const int aspects[] = {VIDEO_ASPECT_AUTO, VIDEO_ASPECT_4_3, VIDEO_ASPECT_16_9};
		static const char *aspectNames[] = {"auto", "4:3", "16:9"};
		OUT("Resolution availability (system check per aspect: auto / 4:3 / 16:9):");
		for (int id : ids) {
			std::string row;
			for (int a = 0; a < 3; ++a) {
				s32 ok = videoGetResolutionAvailability(out, id, aspects[a], 0);
				row += std::string(aspectNames[a]) + (ok > 0 ? " yes" : ok == 0 ? " no" : " err") + (a < 2 ? ", " : "");
			}
			OUT("  %-14s %s", ResolutionText(id).c_str(), row.c_str());
		}
	}

	// Refresh: count vblanks for half a second. Zero means the display controller is not running.
	{
		u64 v0 = gcmGetVBlankCount();
		double t0 = Now();
		usleep(500 * 1000);
		u64 v1 = gcmGetVBlankCount();
		double dt = Now() - t0;
		if (v1 > v0)
			OUT("Measured vblank rate: %.2f Hz (%llu vblanks in %.2f s), current field %u", (v1 - v0) / dt,
				(unsigned long long)(v1 - v0), dt, gcmGetCurrentField());
		else
			OUT("Measured vblank rate: NO VBLANKS in %.2f s - display controller not running", dt);
	}
	OUT("Temperature: %s", RSXTemperatureText().c_str());
#undef OUT

	FILE *file = fopen(PS3Frontend::ReportPath("video_report.txt").c_str(), "w");
	if (file) {
		for (const auto &l : lines) fprintf(file, "%s\n", l.c_str());
		fclose(file);
	}
	return lines;
}
