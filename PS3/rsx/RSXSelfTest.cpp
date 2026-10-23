// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// On-console checks for every RSX building block the hardware renderer relies on: memory, fences,
// clears, coordinate mapping, vertex attribute byte order, texturing, blending, depth, alpha test,
// stencil, the shader combiners and image transfers - plus a VRAM test with bandwidth numbers.
// Each check reads pixels back and compares with what the renderer assumes, so a failure names
// the exact assumption that doesn't hold on real hardware.
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <sys/systime.h>

#include "GPU/Software/HardwareBackend.h"
#include "PS3/rsx/RSXCore.h"
#include "PS3/rsx/RSXSelfTest.h"
#include "PS3/frontend/Frontend.h"

namespace {

using namespace RSX;

double Now() {
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return (double)sec + (double)nsec * 1e-9;
}

RSXProgressCallback g_progress = nullptr;
bool g_cancelled = false;
double g_lastProgress = 0.0;

// Shows progress (rate limited). False once the user cancelled.
bool Progress(const char *label, double fraction, bool force = false) {
	if (g_cancelled) return false;
	if (!g_progress) return true;
	const double now = Now();
	if (!force && now - g_lastProgress < 0.1) return true;
	g_lastProgress = now;
	const bool keep = g_progress(label, fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction));
	EndFrame();  // The display code may have used the RSX; re-bind our vertex arrays.
	if (!keep) g_cancelled = true;
	return keep;
}

struct Report {
	std::vector<std::string> lines;
	int passed = 0, failed = 0;
	const char *title = "RSX test";
	int expected = 0;  // Checks expected, for the progress bar.
	void Line(const char *format, ...) __attribute__((format(printf, 2, 3))) {
		char text[256];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		lines.push_back(text);
		PS3Frontend::Trace("RSX test: %s", text);
	}
	bool Check(bool ok, const char *name, const char *detailFormat = "", ...) __attribute__((format(printf, 4, 5))) {
		char detail[200] = "";
		va_list args;
		va_start(args, detailFormat);
		vsnprintf(detail, sizeof(detail), detailFormat, args);
		va_end(args);
		Line("%s  %s%s%s", ok ? "PASS" : "FAIL", name, detail[0] ? " - " : "", detail);
		if (ok) passed++; else failed++;
		if (expected > 0) {
			char label[160];
			snprintf(label, sizeof(label), "%s: %s", title, name);
			Progress(label, (double)(passed + failed) / expected, true);
		}
		return ok;
	}
};

constexpr int W = 64, H = 64;
constexpr uint32_t PITCH = W * 4;

struct Fixture {
	void *color = nullptr;
	void *depth = nullptr;
	uint32_t *readback = nullptr;  // Main memory.
	Target target;
	bool ok = false;

	bool Init() {
		color = LocalAlloc(PITCH * H, 64);
		depth = LocalAlloc(PITCH * H, 64);
		readback = (uint32_t *)IoAlloc(PITCH * H, 128);
		if (!color || !depth || !readback) return false;
		target.colorOffset = LocalOffset(color);
		target.colorPitch = PITCH;
		target.depthOffset = LocalOffset(depth);
		target.depthPitch = PITCH;
		target.width = W;
		target.height = H;
		ok = true;
		return true;
	}
	~Fixture() {
		LocalFree(color);
		LocalFree(depth);
	}
	void Begin() {
		StepQuiet("test: set target and state");
		SetTarget(target);
		SetPixelViewport(W, H);
		SetDefaultState();
		ResetDrawState();
		BindVertexProgram();
		const float zero[4] = {0, 0, 0, 0}, one[4] = {1, 1, 1, 1};
		SetVertexConstants(zero, zero, one);
	}
	// Copies the color buffer to main memory and waits. False on GPU timeout.
	bool Read() {
		StepQuiet("test: read pixels back");
		GpuWaitForRendering();
		CopyRows(Mem::Local, target.colorOffset, PITCH, Mem::Main, IoOffset(readback), PITCH, PITCH, H);
		return Finish();
	}
	uint32_t Pixel(int x, int y) const { return readback[y * W + x]; }
};

// A screen-aligned rectangle in pixels, z in 0..1, through the vertex ring.
void Rect(float x0, float y0, float x1, float y1, float z, const uint8_t rgba[4],
	const uint8_t spec[4] = nullptr, float fog = 1.0f, float s0 = 0, float t0 = 0, float s1 = 1, float t1 = 1) {
	uint32_t first = 0;
	Vertex *v = AllocVertices(6, &first);
	if (!v) return;
	const float px[6] = {x0, x1, x0, x1, x1, x0}, py[6] = {y0, y0, y1, y0, y1, y1};
	const float ps[6] = {s0, s1, s0, s1, s1, s0}, pt[6] = {t0, t0, t1, t0, t1, t1};
	for (int i = 0; i < 6; ++i) {
		v[i].x = 2.0f * px[i] / W - 1.0f;
		v[i].y = 2.0f * py[i] / H - 1.0f;
		v[i].z = 2.0f * z - 1.0f;
		v[i].w = 1.0f;
		memcpy(v[i].c0, rgba, 4);
		if (spec) memcpy(v[i].c1, spec, 4); else memset(v[i].c1, 0, 4);
		v[i].s = ps[i]; v[i].t = pt[i]; v[i].r = 0; v[i].q = 1;
		v[i].fog = fog;
		v[i].pad = 0;
	}
	DrawVertices(GCM_TYPE_TRIANGLES, first, 6);
}

bool Near(uint32_t got, uint32_t want, int tolerance) {
	for (int shift = 0; shift < 32; shift += 8) {
		int a = (got >> shift) & 0xFF, b = (want >> shift) & 0xFF;
		if (std::abs(a - b) > tolerance) return false;
	}
	return true;
}

void RunRenderingTests(Report &r) {
	Fixture f;
	if (!r.Check(f.Init(), "Allocate 64x64 test target in RSX memory and readback buffer in main memory"))
		return;
	gcmContextData *ctx = Context();
	const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};

	// 0. One command group at a time, each followed by a fence, to find a command the RSX rejects.
	// The step name goes to launch.log before the commands are sent.
	{
		struct Stage {
			const char *name;
			void (*run)(Fixture &f);
		};
		static const Stage stages[] = {
			{"S01 fence only", [](Fixture &) {}},
			{"S02 DMA copy RSX memory -> main memory (M2MF)", [](Fixture &f) {
				CopyRows(Mem::Local, f.target.colorOffset, PITCH, Mem::Main, IoOffset(f.readback), PITCH, PITCH, 4);
			}},
			{"S03 set render target", [](Fixture &f) { SetTarget(f.target); }},
			{"S04 viewport and scissor", [](Fixture &) { SetPixelViewport(W, H); }},
			{"S05 clear color+depth", [](Fixture &) { ClearTarget(0x11223344, true, true); }},
			{"S06 color mask", [](Fixture &) { rsxSetColorMask(Context(), GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A); rsxSetColorMaskMrt(Context(), 0); }},
			{"S07 blend enable off", [](Fixture &) { rsxSetBlendEnable(Context(), GCM_FALSE); }},
			{"S08 depth state", [](Fixture &) { rsxSetDepthTestEnable(Context(), GCM_FALSE); rsxSetDepthWriteEnable(Context(), GCM_FALSE); }},
			{"S09 alpha test off", [](Fixture &) { rsxSetAlphaTestEnable(Context(), GCM_FALSE); }},
			{"S10 stencil off", [](Fixture &) { rsxSetStencilTestEnable(Context(), GCM_FALSE); }},
			{"S11 cull off", [](Fixture &) { rsxSetCullFaceEnable(Context(), GCM_FALSE); }},
			{"S12 logic op off", [](Fixture &) { rsxSetLogicOpEnable(Context(), GCM_FALSE); }},
			{"S13 dither off", [](Fixture &) { rsxSetDitherEnable(Context(), GCM_FALSE); }},
			{"S14 shade model, front face", [](Fixture &) { rsxSetShadeModel(Context(), GCM_SHADE_MODEL_SMOOTH); rsxSetFrontFace(Context(), GCM_FRONTFACE_CCW); }},
			{"S15 z min/max control", [](Fixture &) { rsxSetZMinMaxControl(Context(), GCM_FALSE, GCM_TRUE, GCM_FALSE); }},
			{"S16 user clip planes off", [](Fixture &) {
				rsxSetUserClipPlaneControl(Context(), GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
					GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
			}},
			{"S17 load vertex program", [](Fixture &) { BindVertexProgram(); }},
			{"S18 vertex program constants", [](Fixture &) {
				const float zero[4] = {0, 0, 0, 0}, one[4] = {1, 1, 1, 1};
				SetVertexConstants(zero, zero, one);
			}},
			{"S19 load fragment program", [](Fixture &) { BindFragmentProgram(FRAGMENT_CLEAR); }},
			{"S20 GPU waits for rendering (backend label + wait label + idle)", [](Fixture &) { GpuWaitForRendering(); }},
			{"S21 draw one rectangle", [](Fixture &) {
				const uint8_t white[4] = {255, 255, 255, 255};
				Rect(0, 0, 8, 8, 0.5f, white);
			}},
			{"S22 scaled image transfer RSX -> main", [](Fixture &f) {
				ScaleImage(Mem::Local, f.target.colorOffset, PITCH, 8, 8, Mem::Main, IoOffset(f.readback), PITCH, 0, 0, 8, 8, false);
			}},
		};
		for (const Stage &stage : stages) {
			Step(stage.name);
			const double t0 = Now();
			stage.run(f);
			const bool ok = Finish(2.0);
			char detail[200];
			if (ok) snprintf(detail, sizeof(detail), "%.2f ms", (Now() - t0) * 1000.0);
			else snprintf(detail, sizeof(detail), "GPU did not finish: %s", GpuStateText().c_str());
			r.Check(ok, stage.name, "%s", detail);
			if (!ok) {
				Step("idle");
				r.Line("Stopping: later tests depend on this step.");
				return;
			}
		}
		Step("idle");
	}

	// 1. Clear and DMA readback: byte order of A8R8G8B8 in memory as seen by the PPU.
	f.Begin();
	ClearTarget(0x11223344, true, true);
	bool done = f.Read();
	r.Check(done, "RSX finishes work (fence label written)");
	if (!done) return;
	const uint8_t *bytes = (const uint8_t *)f.readback;
	r.Check(f.Pixel(0, 0) == 0x11223344, "Clear color reads back as native 0xAARRGGBB",
		"got %08x (bytes %02x %02x %02x %02x)", f.Pixel(0, 0), bytes[0], bytes[1], bytes[2], bytes[3]);

	// 3. Coordinate mapping: a rectangle in the top-left corner.
	f.Begin();
	ClearTarget(0xFF000000, true, true);
	BindFragmentProgram(FRAGMENT_CLEAR);
	Rect(0, 0, 32, 16, 0.5f, red);
	f.Read();
	const uint32_t inside = f.Pixel(4, 4), below = f.Pixel(4, 40), right = f.Pixel(40, 4), mirrored = f.Pixel(4, H - 5);
	bool mapped = inside == 0xFFFF0000 && below == 0xFF000000 && right == 0xFF000000;
	r.Check(mapped, "Pixel coordinates: (0,0) is top-left, y grows down",
		"inside %08x below %08x right %08x bottom-left %08x", inside, below, right, mirrored);
	r.Check(f.Pixel(31, 15) == 0xFFFF0000 && f.Pixel(32, 15) == 0xFF000000 && f.Pixel(31, 16) == 0xFF000000,
		"Rectangle edges cover exactly x<32, y<16", "(31,15) %08x (32,15) %08x (31,16) %08x",
		f.Pixel(31, 15), f.Pixel(32, 15), f.Pixel(31, 16));

	// 4. Vertex color byte order.
	f.Begin();
	ClearTarget(0, true, true);
	BindFragmentProgram(FRAGMENT_CLEAR);
	const uint8_t pattern[4] = {0x10, 0x20, 0x30, 0x40};
	Rect(0, 0, W, H, 0.5f, pattern);
	f.Read();
	r.Check(Near(f.Pixel(10, 10), 0x40102030, 1), "Vertex color bytes R,G,B,A arrive in that order", "got %08x want 40102030", f.Pixel(10, 10));

	// 5. Texturing: 4x4 texture, nearest, REPLACE.
	{
		const uint32_t texPitch = 64;  // Bytes; padded.
		uint32_t *tex = (uint32_t *)LocalAlloc(texPitch * 4, 128);
		if (tex) {
			for (int y = 0; y < 4; ++y)
				for (int x = 0; x < 4; ++x)
					tex[y * (texPitch / 4) + x] = 0x80000000 | ((x * 0x40) << 16) | ((y * 0x40) << 8) | 0x55;
			f.Begin();
			ClearTarget(0, true, true);
			InvalidateTextureCache();
			BindTexture(0, LocalOffset(tex), 4, 4, texPitch, false, true, true);
			BindFragmentProgram(FragmentIndex(true, 3, true, false, false, false));
			const uint8_t white[4] = {255, 255, 255, 255};
			Rect(0, 0, W, H, 0.5f, white);
			f.Read();
			bool texOk = true;
			char detail[200] = "";
			for (int ty = 0; ty < 4 && texOk; ++ty) {
				for (int tx = 0; tx < 4; ++tx) {
					uint32_t want = tex[ty * (texPitch / 4) + tx];
					uint32_t got = f.Pixel(tx * 16 + 8, ty * 16 + 8);
					if (got != want) {
						snprintf(detail, sizeof(detail), "texel %d,%d: got %08x want %08x", tx, ty, got, want);
						texOk = false;
						break;
					}
				}
			}
			r.Check(texOk, "Texture A8R8G8B8 layout, orientation and nearest sampling", "%s", detail);

			// Projective coordinate: q = 2 halves s,t, so the whole quad samples the top-left 2x2 texels.
			f.Begin();
			ClearTarget(0, true, true);
			BindTexture(0, LocalOffset(tex), 4, 4, texPitch, false, true, true);
			BindFragmentProgram(FragmentIndex(true, 3, true, false, false, false));
			{
				uint32_t first = 0;
				Vertex *v = AllocVertices(6, &first);
				const float px[6] = {0, W, 0, W, W, 0}, py[6] = {0, 0, H, 0, H, H};
				for (int i = 0; i < 6; ++i) {
					v[i].x = 2.0f * px[i] / W - 1.0f; v[i].y = 2.0f * py[i] / H - 1.0f; v[i].z = 0; v[i].w = 1;
					memset(v[i].c0, 255, 4); memset(v[i].c1, 0, 4);
					v[i].s = px[i] / W; v[i].t = py[i] / H; v[i].r = 0; v[i].q = 2.0f; v[i].fog = 1;
				}
				DrawVertices(GCM_TYPE_TRIANGLES, first, 6);
			}
			f.Read();
			r.Check(f.Pixel(60, 60) == tex[1 * (texPitch / 4) + 1], "Projective texture coordinates (TXP divides by q)",
				"got %08x want %08x", f.Pixel(60, 60), tex[1 * (texPitch / 4) + 1]);
			LocalFree(tex);
		}
	}

	// 6. Blending SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
	f.Begin();
	ClearTarget(0xFF0000FF, true, true);
	BindFragmentProgram(FRAGMENT_CLEAR);
	rsxSetBlendEnable(ctx, GCM_TRUE);
	rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ZERO);
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	const uint8_t halfRed[4] = {255, 0, 0, 128};
	Rect(0, 0, W, H, 0.5f, halfRed);
	f.Read();
	r.Check(Near(f.Pixel(5, 5), 0x80800080, 2), "Alpha blending", "got %08x want ~80800080", f.Pixel(5, 5));

	// Constant color factor (used for the GE's fixed blend colors).
	f.Begin();
	ClearTarget(0xFF404040, true, true);
	BindFragmentProgram(FRAGMENT_CLEAR);
	rsxSetBlendEnable(ctx, GCM_TRUE);
	rsxSetBlendColor(ctx, 0x00800000, 0);
	rsxSetBlendFunc(ctx, GCM_ONE, GCM_CONSTANT_COLOR, GCM_ONE, GCM_ZERO);
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	const uint8_t dark[4] = {0x10, 0x10, 0x10, 255};
	Rect(0, 0, W, H, 0.5f, dark);
	f.Read();
	r.Check(Near(f.Pixel(5, 5), 0xFF301010, 2), "Blend constant color (R=0x80, others 0)", "got %08x want ~ff301010", f.Pixel(5, 5));

	// 7. Depth test.
	f.Begin();
	ClearTarget(0xFF000000, true, true, 0xFFFFFF00);
	BindFragmentProgram(FRAGMENT_CLEAR);
	rsxSetDepthTestEnable(ctx, GCM_TRUE);
	rsxSetDepthWriteEnable(ctx, GCM_TRUE);
	rsxSetDepthFunc(ctx, GCM_LESS);
	Rect(0, 0, W, H, 0.5f, red);
	Rect(0, 0, W, H, 0.7f, green);
	Rect(0, 0, W / 2, H, 0.3f, blue);
	f.Read();
	r.Check(f.Pixel(50, 10) == 0xFFFF0000 && f.Pixel(10, 10) == 0xFF0000FF, "Depth test LESS with depth writes",
		"right %08x (want red) left %08x (want blue)", f.Pixel(50, 10), f.Pixel(10, 10));

	// 8. Alpha test.
	f.Begin();
	ClearTarget(0xFF000000, true, true);
	BindFragmentProgram(FRAGMENT_CLEAR);
	rsxSetAlphaTestEnable(ctx, GCM_TRUE);
	rsxSetAlphaFunc(ctx, GCM_GREATER, 0x80);
	const uint8_t lowAlpha[4] = {0, 255, 0, 0x40}, highAlpha[4] = {0, 0, 255, 0xC0};
	Rect(0, 0, W / 2, H, 0.5f, lowAlpha);
	Rect(W / 2, 0, W, H, 0.5f, highAlpha);
	f.Read();
	r.Check(f.Pixel(10, 10) == 0xFF000000 && (f.Pixel(50, 10) & 0x00FFFFFF) == 0x0000FF, "Alpha test GREATER 0x80",
		"low %08x (want untouched) high %08x (want blue)", f.Pixel(10, 10), f.Pixel(50, 10));

	// 9. Stencil.
	f.Begin();
	ClearTarget(0xFF000000, true, true, 0xFFFFFF00);
	BindFragmentProgram(FRAGMENT_CLEAR);
	rsxSetStencilTestEnable(ctx, GCM_TRUE);
	rsxSetStencilMask(ctx, 0xFF);
	rsxSetStencilFunc(ctx, GCM_ALWAYS, 5, 0xFF);
	rsxSetStencilOp(ctx, GCM_KEEP, GCM_KEEP, GCM_REPLACE);
	rsxSetColorMask(ctx, 0);
	Rect(0, 0, W / 2, H, 0.5f, red);
	rsxSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetStencilOp(ctx, GCM_KEEP, GCM_KEEP, GCM_KEEP);
	rsxSetStencilFunc(ctx, GCM_EQUAL, 5, 0xFF);
	Rect(0, 0, W, H, 0.5f, green);
	f.Read();
	r.Check(f.Pixel(10, 10) == 0xFF00FF00 && f.Pixel(50, 10) == 0xFF000000, "Stencil REPLACE then EQUAL",
		"marked %08x (want green) unmarked %08x (want black)", f.Pixel(10, 10), f.Pixel(50, 10));

	// 10. Combiner: specular add, color doubling, fog.
	{
		f.Begin();
		ClearTarget(0, true, true);
		const float zero[4] = {0, 0, 0, 0}, fogBlue[4] = {0, 0, 1, 1}, one[4] = {1, 1, 1, 1};
		SetVertexConstants(zero, fogBlue, one);
		BindFragmentProgram(FragmentIndex(false, 0, false, true, false, false));
		const uint8_t grey[4] = {100, 100, 100, 255}, spec[4] = {20, 20, 20, 0};
		Rect(0, 0, W / 2, H, 0.5f, grey, spec);
		BindFragmentProgram(FragmentIndex(false, 0, false, false, true, false));
		Rect(W / 2, 0, W, H, 0.5f, red, nullptr, 0.25f);
		f.Read();
		r.Check(Near(f.Pixel(10, 10), 0xFFF0F0F0, 2), "Combiner: specular add then color doubling", "got %08x want ~fff0f0f0", f.Pixel(10, 10));
		r.Check(Near(f.Pixel(50, 10), 0xFF4000BF, 2), "Combiner: fog 25%% color, 75%% fog color", "got %08x want ~ff4000bf", f.Pixel(50, 10));
	}

	// 11. Texture env color (BLEND function) and the blend multiplier varying.
	{
		const uint32_t texPitch = 64;
		uint32_t *tex = (uint32_t *)LocalAlloc(texPitch * 4, 128);
		if (tex) {
			for (int i = 0; i < 16 * 4; ++i) tex[i] = 0xFF808080;
			f.Begin();
			ClearTarget(0, true, true);
			const float env[4] = {0, 1, 0, 1}, zero[4] = {0, 0, 0, 0}, half[4] = {0.5f, 0.5f, 0.5f, 1};
			SetVertexConstants(env, zero, half);
			InvalidateTextureCache();
			BindTexture(0, LocalOffset(tex), 4, 4, texPitch, false, true, true);
			BindFragmentProgram(FragmentIndex(true, 2, false, false, false, true));
			const uint8_t white[4] = {255, 0, 0, 255};
			Rect(0, 0, W, H, 0.5f, white);
			f.Read();
			// prim (1,0,0) blended toward env (0,1,0) by texel 0.5 -> (0.5,0.5,0); times multiplier 0.5.
			r.Check(Near(f.Pixel(10, 10), 0xFF404000, 3), "Texture BLEND with env color, source multiplier", "got %08x want ~ff404000", f.Pixel(10, 10));
			LocalFree(tex);
		}
	}

	// 12. Scaled transfer (render target -> main memory, 2x nearest).
	{
		uint32_t *src = (uint32_t *)LocalAlloc(64 * 8, 128);
		uint32_t *dst = (uint32_t *)IoAlloc(64 * 16, 128);
		if (src && dst) {
			for (int y = 0; y < 8; ++y)
				for (int x = 0; x < 8; ++x)
					src[y * 16 + x] = 0xFF000000 | (x << 20) | (y << 12);
			memset(dst, 0, 64 * 16);
			ScaleImage(Mem::Local, LocalOffset(src), 64, 8, 8, Mem::Main, IoOffset(dst), 64, 0, 0, 16, 16, false);
			Finish();
			bool ok = dst[0] == src[0] && dst[15 * 16 + 15] == src[7 * 16 + 7] && dst[3 * 16 + 9] == src[1 * 16 + 4];
			r.Check(ok, "Scaled image transfer 8x8 -> 16x16 (local to main)", "corners %08x/%08x want %08x/%08x",
				dst[0], dst[15 * 16 + 15], src[0], src[7 * 16 + 7]);
		}
		LocalFree(src);
	}

	// 13. Throughput.
	{
		f.Begin();
		ClearTarget(0, true, true);
		BindFragmentProgram(FRAGMENT_CLEAR);
		const int batches = 100, perBatch = 600;
		double start = Now();
		for (int b = 0; b < batches; ++b) {
			uint32_t first = 0;
			Vertex *v = AllocVertices(perBatch, &first);
			if (!v) break;
			for (int i = 0; i < perBatch; ++i) {
				float x = (float)((i * 7 + b) % W), y = (float)((i * 13) % H);
				v[i].x = 2.0f * (x + (i % 3 == 1 ? 4 : 0)) / W - 1.0f;
				v[i].y = 2.0f * (y + (i % 3 == 2 ? 4 : 0)) / H - 1.0f;
				v[i].z = 0; v[i].w = 1;
				v[i].c0[0] = (uint8_t)i; v[i].c0[1] = (uint8_t)b; v[i].c0[2] = 128; v[i].c0[3] = 255;
				memset(v[i].c1, 0, 4);
				v[i].s = v[i].t = v[i].r = 0; v[i].q = 1; v[i].fog = 1;
			}
			DrawVertices(GCM_TYPE_TRIANGLES, first, perBatch);
		}
		bool ok = Finish(5.0);
		double seconds = Now() - start;
		r.Check(ok, "20000 triangles through the vertex ring", "%.1f ms (%.2f M triangles/s incl. CPU vertex writes)",
			seconds * 1000.0, 20000.0 / seconds / 1e6);
	}
	// Render target in main memory - last, and followed by a draw, because it seemed to break
	// later draws (Frontend 08 report).
	{
		uint32_t *mainColor = (uint32_t *)IoAlloc(PITCH * H, 128);
		if (mainColor) {
			Target t = f.target;
			t.colorInMain = true;
			t.colorOffset = IoOffset(mainColor);
			t.hasDepth = false;
			SetTarget(t);
			ClearTarget(0x55667788, true, false);
			Finish();
			r.Check(mainColor[W * H / 2] == 0x55667788, "Render target in main memory", "got %08x", mainColor[W * H / 2]);
		}
	}

	// Does drawing still work after a main-memory target?
	{
		const uint8_t green[4] = {0, 255, 0, 255};
		f.Begin();
		ClearTarget(0xFF000000, true, true);
		BindFragmentProgram(FRAGMENT_CLEAR);
		Rect(0, 0, W, H, 0.5f, green);
		f.Read();
		r.Check(f.Pixel(8, 8) == 0xFF00FF00, "Drawing after a main-memory render target", "got %08x want ff00ff00", f.Pixel(8, 8));
	}
	EndFrame();
}

void RunVramTests(Report &r) {
	// Largest single block in RSX local memory (approximate, 1 MB steps then 64 KB).
	uint32_t lo = 0, hi = 256u * 1024 * 1024;
	while (hi - lo > 1024 * 1024) {
		uint32_t mid = lo + (hi - lo) / 2;
		void *p = LocalAlloc(mid, 1024);
		if (p) { LocalFree(p); lo = mid; } else hi = mid;
	}
	r.Line("INFO  Largest free block of RSX local memory: %u MB", lo / (1024 * 1024));

	const uint32_t size = 32u * 1024 * 1024;
	uint8_t *local = (uint8_t *)LocalAlloc(size, 1024 * 1024);
	if (!r.Check(local != nullptr, "Allocate 32 MB of RSX local memory"))
		return;

	// CPU write bandwidth into local memory.
	double t0 = Now();
	uint64_t *q = (uint64_t *)local;
	for (uint32_t i = 0; i < size / 8; ++i)
		q[i] = ((uint64_t)i * 0x9E3779B97F4A7C15ull) ^ 0xA5A5A5A5A5A5A5A5ull;
	__sync_synchronize();
	double cpuWrite = Now() - t0;
	r.Line("INFO  CPU writes to RSX memory: %.0f MB/s", size / cpuWrite / 1e6);

	// CPU read (slow path on PS3; only 1 MB).
	t0 = Now();
	volatile uint64_t sink = 0;
	for (uint32_t i = 0; i < (1024 * 1024) / 8; ++i) sink += q[i];
	double cpuRead = Now() - t0;
	r.Line("INFO  CPU reads from RSX memory: %.1f MB/s (expected to be slow)", 1.0 / cpuRead);

	// Pattern verify via RSX DMA to main memory in 4 MB chunks.
	const uint32_t chunk = 4u * 1024 * 1024;
	uint64_t *staging = (uint64_t *)IoAlloc(chunk, 1024);
	if (!r.Check(staging != nullptr, "Allocate 4 MB staging in main memory"))
		goto done;
	{
		bool allOk = true;
		uint32_t badIndex = 0;
		double dmaTime = 0;
		for (uint32_t base = 0; base < size && allOk; base += chunk) {
			t0 = Now();
			CopyRows(Mem::Local, LocalOffset(local) + base, 64 * 1024, Mem::Main, IoOffset(staging), 64 * 1024, 64 * 1024, chunk / (64 * 1024));
			if (!Finish(3.0)) { allOk = false; break; }
			dmaTime += Now() - t0;
			for (uint32_t i = 0; i < chunk / 8; ++i) {
				uint32_t index = base / 8 + i;
				uint64_t want = ((uint64_t)index * 0x9E3779B97F4A7C15ull) ^ 0xA5A5A5A5A5A5A5A5ull;
				if (staging[i] != want) { allOk = false; badIndex = index; break; }
			}
		}
		r.Check(allOk, "32 MB pattern written by CPU, read back by RSX DMA, verified", allOk ? "" : "first mismatch at byte %u", badIndex * 8);
		if (dmaTime > 0) r.Line("INFO  RSX DMA RSX memory -> main memory: %.0f MB/s", size / dmaTime / 1e6);

		// Main -> local, then local -> main round trip.
		for (uint32_t i = 0; i < chunk / 8; ++i) staging[i] = ~((uint64_t)i * 0x2545F4914F6CDD1Dull);
		__sync_synchronize();
		t0 = Now();
		CopyRows(Mem::Main, IoOffset(staging), 64 * 1024, Mem::Local, LocalOffset(local), 64 * 1024, 64 * 1024, chunk / (64 * 1024));
		bool ok = Finish(3.0);
		double up = Now() - t0;
		memset(staging, 0, chunk);
		__sync_synchronize();
		CopyRows(Mem::Local, LocalOffset(local), 64 * 1024, Mem::Main, IoOffset(staging), 64 * 1024, 64 * 1024, chunk / (64 * 1024));
		ok = ok && Finish(3.0);
		uint32_t bad = 0;
		for (uint32_t i = 0; i < chunk / 8 && ok; ++i)
			if (staging[i] != ~((uint64_t)i * 0x2545F4914F6CDD1Dull)) { ok = false; bad = i; }
		r.Check(ok, "4 MB main -> RSX -> main round trip", ok ? "" : "mismatch at byte %u", bad * 8);
		if (up > 0) r.Line("INFO  RSX DMA main memory -> RSX memory: %.0f MB/s", chunk / up / 1e6);
	}

	// Fill rate: clear a 1920x1080 surface repeatedly.
	{
		const int fw = 1920, fh = 1080;
		void *surf = LocalAlloc(fw * 4 * fh, 64);
		if (surf) {
			Target t;
			t.colorOffset = LocalOffset(surf);
			t.colorPitch = fw * 4;
			t.width = fw;
			t.height = fh;
			t.hasDepth = false;
			SetTarget(t);
			SetPixelViewport(fw, fh);
			SetDefaultState();
			Finish();
			t0 = Now();
			for (int i = 0; i < 60; ++i) ClearTarget(0xFF000000 | i, true, false);
			bool ok = Finish(5.0);
			double s = Now() - t0;
			r.Check(ok, "60 full clears of a 1920x1080 target", "%.0f Mpixels/s", 60.0 * fw * fh / s / 1e6);
			LocalFree(surf);
		}
	}
done:
	LocalFree(local);
}

// Every free byte of RSX local memory, in blocks: CPU-written address patterns (and inverses),
// then RSX-written clear patterns, each read back by RSX DMA. Bad memory chips or a failing
// memory controller show up as specific offsets and bit positions.
struct SweepErrors {
	uint32_t count = 0, bitsOr = 0;
	uint32_t firstOffsets[8];
	uint32_t firstBits[8];
	int recorded = 0;
	uint32_t bitCounts[32] = {};
	void Add(uint32_t offset, uint32_t xorBits) {
		count++;
		bitsOr |= xorBits;
		for (int b = 0; b < 32; ++b) if (xorBits & (1u << b)) bitCounts[b]++;
		if (recorded < 8) { firstOffsets[recorded] = offset; firstBits[recorded] = xorBits; recorded++; }
	}
};

bool SweepVerify(void *block, uint32_t size, uint32_t *staging, uint32_t chunk, uint32_t (*expect)(uint32_t offset, uint32_t seed), uint32_t seed, SweepErrors &e) {
	const uint32_t base = LocalOffset(block);
	for (uint32_t pos = 0; pos < size; pos += chunk) {
		const uint32_t n = std::min(chunk, size - pos);
		CopyRows(Mem::Local, base + pos, 64 * 1024, Mem::Main, IoOffset(staging), 64 * 1024, 64 * 1024, n / (64 * 1024));
		if (!Finish(3.0)) return false;
		for (uint32_t i = 0; i < n / 4; ++i) {
			const uint32_t offset = base + pos + i * 4;
			const uint32_t want = expect(offset, seed);
			if (staging[i] != want) e.Add(offset, staging[i] ^ want);
		}
	}
	return true;
}

uint32_t AddressPattern(uint32_t offset, uint32_t seed) { return (offset * 0x9E3779B1u) ^ seed; }
uint32_t ConstantPattern(uint32_t, uint32_t seed) { return seed; }

void RunFullVramSweep(Report &r) {
	StepQuiet("test: full VRAM sweep");
	std::vector<std::pair<void *, uint32_t>> blocks;
	uint32_t total = 0;
	for (uint32_t size : {16u << 20, 1u << 20}) {
		while (blocks.size() < 300) {
			void *p = LocalAlloc(size, 1024 * 1024);
			if (!p) break;
			blocks.push_back({p, size});
			total += size;
		}
	}
	const uint32_t chunk = 4u * 1024 * 1024;
	uint32_t *staging = (uint32_t *)IoAlloc(chunk, 1024);
	if (!r.Check(!blocks.empty() && staging, "Full sweep: allocate all free RSX memory", "%u MB in %u blocks", total >> 20, (unsigned)blocks.size())) {
		for (auto &b : blocks) LocalFree(b.first);
		return;
	}

	struct Pass {
		const char *name;
		bool rsxWrites;
		uint32_t seed;
	};
	static const Pass passes[] = {
		{"CPU-written address pattern", false, 0x00000000u},
		{"CPU-written inverted address pattern", false, 0xFFFFFFFFu},
		{"RSX-written fill 0x5AA5C33C", true, 0x5AA5C33Cu},
		{"RSX-written fill 0xA55A3CC3", true, 0xA55A3CC3u},
	};
	bool anyBad = false;
	int passIndex = 0;
	for (const Pass &pass : passes) {
		SweepErrors e;
		bool gpuOk = true;
		const double t0 = Now();
		size_t blockIndex = 0;
		for (auto &b : blocks) {
			char label[128];
			snprintf(label, sizeof(label), "VRAM sweep pass %d/4: %s (%u MB)", passIndex + 1, pass.name, total >> 20);
			if (!Progress(label, (passIndex + (double)blockIndex / blocks.size()) / 4.0)) break;
			blockIndex++;
			if (!pass.rsxWrites) {
				uint32_t *w = (uint32_t *)b.first;
				const uint32_t base = LocalOffset(b.first);
				for (uint32_t i = 0; i < b.second / 4; ++i) w[i] = AddressPattern(base + i * 4, pass.seed);
				__sync_synchronize();
			} else {
				// Render target: 1024 pixels wide (4 KB pitch), as many rows as the block holds.
				Target t;
				t.colorOffset = LocalOffset(b.first);
				t.colorPitch = 4096;
				t.width = 1024;
				t.height = (uint16_t)(b.second / 4096);
				t.hasDepth = false;
				SetTarget(t);
				SetPixelViewport(t.width, t.height);
				SetDefaultState();
				ClearTarget(pass.seed, true, false);
				if (!Finish(3.0)) { gpuOk = false; break; }
			}
			if (!SweepVerify(b.first, b.second, staging, chunk, pass.rsxWrites ? ConstantPattern : AddressPattern, pass.seed, e)) {
				gpuOk = false;
				break;
			}
		}
		passIndex++;
		if (g_cancelled) {
			r.Line("INFO  Cancelled during %s", pass.name);
			break;
		}
		if (!gpuOk) {
			r.Check(false, pass.name, "GPU did not finish: %s", GpuStateText().c_str());
			anyBad = true;
			break;
		}
		char detail[200];
		if (e.count == 0) {
			snprintf(detail, sizeof(detail), "%u MB clean, %.1f s", total >> 20, Now() - t0);
		} else {
			snprintf(detail, sizeof(detail), "%u bad words, failing bits mask %08x", e.count, e.bitsOr);
			anyBad = true;
		}
		r.Check(e.count == 0, pass.name, "%s", detail);
		for (int i = 0; i < e.recorded; ++i)
			r.Line("      bad word at RSX offset 0x%08x (%u MB), flipped bits %08x", e.firstOffsets[i], e.firstOffsets[i] >> 20, e.firstBits[i]);
		if (e.count) {
			std::string bits;
			for (int b = 31; b >= 0; --b) if (e.bitCounts[b]) { char s[32]; snprintf(s, sizeof(s), " b%d:%u", b, e.bitCounts[b]); bits += s; }
			r.Line("      errors per bit:%s", bits.c_str());
		}
	}
	if (anyBad)
		r.Line("NOTE  Errors only in RSX-written passes point at the RSX/memory interface; errors in all passes at the memory chips.");
	for (auto &b : blocks) LocalFree(b.first);
}

// FNV-1a over the read-back pixels.
uint32_t Checksum(const uint32_t *p, size_t n) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
	return h;
}

// One fixed scene: overlapping, blended, depth-tested rectangles. Same input every time, so the
// output must be bit-identical every time.
void StressScene(Fixture &f, int density) {
	f.Begin();
	ClearTarget(0xFF102030, true, true);
	gcmContextData *ctx = Context();
	BindFragmentProgram(FRAGMENT_CLEAR);
	rsxSetDepthTestEnable(ctx, GCM_TRUE);
	rsxSetDepthFunc(ctx, GCM_LESS);
	rsxSetDepthWriteEnable(ctx, GCM_TRUE);
	rsxSetBlendEnable(ctx, GCM_TRUE);
	rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ZERO);
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	uint32_t seed = 12345;
	auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFF; };
	for (int i = 0; i < density; ++i) {
		const float x0 = (float)(rnd() % W), y0 = (float)(rnd() % H);
		const float x1 = std::min((float)W, x0 + 1 + rnd() % 40), y1 = std::min((float)H, y0 + 1 + rnd() % 40);
		const uint8_t c[4] = {(uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)rnd(), (uint8_t)(64 + rnd() % 192)};
		Rect(x0, y0, x1, y1, (rnd() % 1000) / 1000.0f, c);
	}
	rsxSetBlendEnable(ctx, GCM_FALSE);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
}

void RunStress(Report &r, double seconds) {
	Fixture f;
	if (!r.Check(f.Init(), "Allocate stress target")) return;
	r.Line("INFO  Before: %s", RSXTemperatureText().c_str());
	const int density = 3000;
	auto countDrawn = [&f]() {
		int drawn = 0;
		for (int i = 0; i < W * H; ++i) if (f.readback[i] != 0xFF102030) drawn++;
		return drawn;
	};
	StressScene(f, density);
	if (!r.Check(f.Read(), "Reference frame rendered")) return;
	int drawn = countDrawn();
	if (drawn <= W * H / 2) {
		// Find out what is wrong: one plain opaque rectangle, no depth or blending.
		r.Line("INFO  Scene drew only %d of %d pixels; checking a plain draw", drawn, W * H);
		f.Begin();
		ClearTarget(0xFF102030, true, true);
		BindFragmentProgram(FRAGMENT_CLEAR);
		const uint8_t green[4] = {0, 255, 0, 255};
		Rect(0, 0, W, H, 0.5f, green);
		if (f.Read())
			r.Line("INFO  Plain draw: %d of %d pixels, center %08x", countDrawn(), W * H, f.Pixel(W / 2, H / 2));
		StressScene(f, density);
		if (f.Read()) drawn = countDrawn();
	}
	if (!r.Check(drawn > W * H / 2, "Scene actually draws (not just the clear color)", "%d of %d pixels drawn", drawn, W * H))
		return;
	const uint32_t reference = Checksum(f.readback, W * H);
	static uint32_t refPixels[W * H];
	memcpy(refPixels, f.readback, sizeof(refPixels));

	// Full load: a 1280x720 target covered by full-screen layers of a ~200-instruction fragment
	// program, blended and depth-tested, so shaders, texture units and memory all work flat out.
	// Every second the small scene above is drawn again and must come out bit-identical.
	constexpr int LW = 1280, LH = 720, TEX = 256;
	void *loadColor = LocalAlloc(LW * 4 * LH, 64);
	void *loadDepth = LocalAlloc(LW * 4 * LH, 64);
	uint32_t *loadTex = (uint32_t *)LocalAlloc(TEX * TEX * 4, 128);
	const bool heavy = loadColor && loadDepth && loadTex;
	if (heavy) {
		uint32_t seed = 777;
		for (int i = 0; i < TEX * TEX; ++i) { seed = seed * 1103515245u + 12345u; loadTex[i] = seed | 0xFF000000; }
		__sync_synchronize();
	} else {
		r.Line("INFO  Not enough RSX memory for the full-load target; running the small scene only");
	}
	Target lt;
	if (heavy) {
		lt.colorOffset = LocalOffset(loadColor);
		lt.colorPitch = LW * 4;
		lt.depthOffset = LocalOffset(loadDepth);
		lt.depthPitch = LW * 4;
		lt.width = LW;
		lt.height = LH;
	}
	int layers = 4;
	auto loadFrame = [&](int frameNo) {
		SetTarget(lt);
		SetPixelViewport(LW, LH);
		SetDefaultState();
		BindVertexProgram();
		const float zero[4] = {0, 0, 0, 0}, one[4] = {1, 1, 1, 1};
		SetVertexConstants(zero, zero, one);
		gcmContextData *ctx = Context();
		if ((frameNo & 7) == 0)
			ClearTarget(0xFF000000, true, true);
		BindTexture(0, LocalOffset(loadTex), TEX, TEX, TEX * 4, true, false, false, false, true);
		BindFragmentProgram(FRAGMENT_BURN);
		rsxSetBlendEnable(ctx, GCM_TRUE);
		rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ZERO);
		rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
		rsxSetDepthTestEnable(ctx, GCM_TRUE);
		rsxSetDepthFunc(ctx, GCM_ALWAYS);
		rsxSetDepthWriteEnable(ctx, GCM_TRUE);
		for (int l = 0; l < layers; ++l) {
			uint32_t first = 0;
			Vertex *v = AllocVertices(6, &first);
			if (!v) break;
			const float px[6] = {-1, 1, -1, 1, 1, -1}, py[6] = {-1, -1, 1, -1, 1, 1};
			const float shift = (float)((frameNo * 7 + l * 13) & 63) / 64.0f;
			for (int i = 0; i < 6; ++i) {
				v[i].x = px[i]; v[i].y = py[i]; v[i].z = 0.1f * l - 0.5f; v[i].w = 1.0f;
				v[i].c0[0] = (uint8_t)(40 * l + 30); v[i].c0[1] = (uint8_t)(200 - 30 * l); v[i].c0[2] = (uint8_t)(90 + 20 * i); v[i].c0[3] = 160;
				memset(v[i].c1, 0, 4);
				v[i].s = (px[i] + 1.0f) * 2.0f + shift; v[i].t = (py[i] + 1.0f) * 2.0f - shift; v[i].r = 0; v[i].q = 1;
				v[i].fog = 1.0f; v[i].pad = 0;
			}
			DrawVertices(GCM_TYPE_TRIANGLES, first, 6);
		}
		rsxSetBlendEnable(ctx, GCM_FALSE);
		rsxSetDepthTestEnable(ctx, GCM_FALSE);
		rsxSetDepthWriteEnable(ctx, GCM_FALSE);
		Flush();
	};

	unsigned startCell = 0, startRsx = 0, maxCell = 0, maxRsx = 0;
	RSXTemperatures(&startCell, &startRsx);
	maxCell = startCell;
	maxRsx = startRsx;
	int frames = 0, mismatches = 0, timeouts = 0;
	uint64_t loadFrames = 0;
	const double start = Now();
	double lastProgress = start, lastCheck = start;
	std::string temperature = RSXTemperatureText();
	double lastTemperature = start;
	uint32_t inFlight[2] = {0, 0};

	if (heavy) {
		// Size the load so one frame takes ~40 ms: layers until it does (max 32).
		for (int attempt = 0; attempt < 4; ++attempt) {
			const double t0 = Now();
			loadFrame(0);
			if (!Finish(5.0)) { timeouts++; break; }
			const double ms = (Now() - t0) * 1000.0;
			if (ms > 25.0 || layers >= 32) break;
			layers = std::min(32, layers * 2);
		}
		r.Line("INFO  Load: %dx%d target, %d full-screen layers per frame of a ~200-instruction shader", LW, LH, layers);
	}

	while (timeouts == 0 && Now() - start < seconds) {
		if (Now() - lastTemperature > 2.0) {
			unsigned c = 0, x = 0;
			RSXTemperatures(&c, &x);
			maxCell = std::max(maxCell, c);
			maxRsx = std::max(maxRsx, x);
			temperature = RSXTemperatureText();
			lastTemperature = Now();
		}
		char label[192];
		snprintf(label, sizeof(label), "Stress test: %.0f of %.0f s, %llu load frames, %d checks, %d mismatched - %s",
			Now() - start, seconds, (unsigned long long)loadFrames, frames, mismatches, temperature.c_str());
		if (!Progress(label, (Now() - start) / seconds)) {
			r.Line("INFO  Cancelled after %d checks", frames);
			break;
		}
		if (heavy) {
			// Keep two load frames queued so the RSX never waits for the CPU.
			for (int k = 0; k < 2 && Now() - lastCheck < 1.0; ++k) {
				const int slot = (int)(loadFrames & 1);
				if (inFlight[slot] && !WaitFence(inFlight[slot], 5.0)) {
					timeouts++;
					r.Line("FAIL  load frame %llu: GPU did not finish: %s", (unsigned long long)loadFrames, GpuStateText().c_str());
					break;
				}
				loadFrame((int)loadFrames);
				inFlight[slot] = Fence();
				loadFrames++;
			}
			if (timeouts || Now() - lastCheck < 1.0)
				continue;
		}
		lastCheck = Now();
		StressScene(f, density);
		if (!f.Read()) {
			timeouts++;
			r.Line("FAIL  check %d: GPU did not finish: %s", frames, GpuStateText().c_str());
			break;
		}
		const uint32_t sum = Checksum(f.readback, W * H);
		if (sum != reference) {
			mismatches++;
			if (mismatches <= 5) {
				int bad = 0, firstX = -1, firstY = -1;
				for (int i = 0; i < W * H; ++i) if (f.readback[i] != refPixels[i]) { if (!bad) { firstX = i % W; firstY = i / W; } bad++; }
				r.Line("FAIL  check %d differs from reference: %d pixels, first at (%d,%d) got %08x want %08x (%s)", frames, bad,
					firstX, firstY, f.readback[firstY * W + firstX], refPixels[firstY * W + firstX], temperature.c_str());
			}
		}
		frames++;
		if (Now() - lastProgress > 5.0) {
			lastProgress = Now();
			PS3Frontend::Trace("RSX stress: %llu load frames, %d checks, %d mismatches, %s", (unsigned long long)loadFrames, frames, mismatches, temperature.c_str());
		}
	}
	Finish(5.0);
	const double elapsed = Now() - start;
	r.Check(mismatches == 0 && timeouts == 0, "Identical output for every check under load",
		"%d checks in %.0f s, %d mismatched", frames, elapsed, mismatches);
	if (heavy && elapsed > 0)
		r.Line("INFO  Load: %llu frames (%.1f frames/s), %.0f megapixels/s shaded", (unsigned long long)loadFrames, loadFrames / elapsed,
			(double)loadFrames * layers * LW * LH / elapsed / 1e6);
	unsigned endCell = 0, endRsx = 0;
	RSXTemperatures(&endCell, &endRsx);
	maxCell = std::max(maxCell, endCell);
	maxRsx = std::max(maxRsx, endRsx);
	if (startRsx || maxRsx)
		r.Line("INFO  Temperatures: RSX %u C at start, %u C highest, %u C at the end; Cell %u / %u / %u C", startRsx, maxRsx, endRsx, startCell, maxCell, endCell);
	r.Line("INFO  After: %s", RSXTemperatureText().c_str());
	LocalFree(loadColor);
	LocalFree(loadDepth);
	LocalFree(loadTex);
}

}  // namespace

// Hardware T&L check at startup: one directional light and two quads of opposite winding, drawn
// through the T&L vertex program. Lit color and position must come out as computed here; the
// second pass with culling tells which winding the RSX treats as front.
RSXTLTestResult RSXTestTL() {
	RSXTLTestResult res;
	if (!Ready() || !LoadPrograms()) {
		res.summary = "RSX not ready";
		return res;
	}
	IoMark();
	{
		Fixture f;
		if (!f.Init()) {
			IoRelease();
			res.summary = "no memory";
			return res;
		}
		float c[Rasterizer::TL_CONST_COUNT][4];
		memset(c, 0, sizeof(c));
		auto C = [&](int k) -> float * { return c[k - 3]; };
		auto set4 = [](float *o, float a, float b, float x, float d) { o[0] = a; o[1] = b; o[2] = x; o[3] = d; };
		set4(C(3), 1, 0, 0, 0); set4(C(4), 0, 1, 0, 0); set4(C(5), 0, 0, 1, 0); set4(C(6), 0, 0, 0, 1);
		set4(C(7), 1, 0, 0, 0); set4(C(8), 0, 1, 0, 0); set4(C(9), 0, 0, 1, 0);
		set4(C(11), 0, 0, 0, 1);
		set4(C(12), 1, 1, 0, 0);
		set4(C(13), 0.1f, 0, 0, 0);
		set4(C(14), 0.2f, 0.2f, 0.2f, 1);
		set4(C(16), 1, 1, 1, 1); set4(C(17), 1, 1, 1, 0);
		set4(C(19), 1, 0, 0, 0);
		set4(C(20), 0, 0, 1, 0);
		set4(C(21), 0, 1, 0.5f, 1e-20f);
		for (int l = 0; l < 4; ++l) {
			set4(C(23 + 6 * l), 1, 0, 0, 0);
			set4(C(24 + 6 * l), 0, 0, 0, -2);
		}
		set4(C(22), 0, 0, 1, 1);
		set4(C(26), 0.5f, 0.5f, 0.5f, 0);
		// Expected: emissive 0.1 + ambient 0.2 + diffuse 0.5 (N.L = 1) in red, 0.7 in green/blue.
		const uint32_t want = 0xFFCCB3B3;

		// Left quad: triangles with a positive screen cross product (y down); right: negative.
		TLVertex quad[12];
		auto vtx = [](TLVertex &v, float x, float y) {
			v.x = x; v.y = y; v.z = 0.0f;
			v.nx = 0.0f; v.ny = 0.0f; v.nz = 1.0f;
			v.c[0] = v.c[1] = v.c[2] = v.c[3] = 255;
			v.u = v.v = 0.0f;
		};
		// NDC y grows downwards on screen here (see SetPixelViewport), like PSP screen y.
		const float L[6][2] = {{-1, -1}, {0, -1}, {-1, 1}, {0, -1}, {0, 1}, {-1, 1}};
		for (int i = 0; i < 6; ++i) vtx(quad[i], L[i][0], L[i][1]);
		const float R[6][2] = {{0, -1}, {0, 1}, {1, -1}, {1, -1}, {0, 1}, {1, 1}};
		for (int i = 0; i < 6; ++i) vtx(quad[6 + i], R[i][0], R[i][1]);
		const float cross = (L[1][0] - L[0][0]) * (L[2][1] - L[0][1]) - (L[1][1] - L[0][1]) * (L[2][0] - L[0][0]);

		// stage 0: the main vertex program on the T&L vertex layout (attribute binding, indexed
		// draw); 1: a pass-through program on that layout (loading another program); 2: the T&L
		// program with lighting off (vertex color); 3: lit.
		// stages 4-6: the cut-down programs (BindTestVertexProgram 0-2), lighting off.
		auto draw = [&](bool cull, int stage = 3) {
			f.Begin();
			ClearTarget(0xFF000000, true, true);
			bool bound = stage == 0 ? true : stage == 1 ? BindVertexProgramTLMin() : stage >= 4 ? BindTestVertexProgram(stage - 4) : BindVertexProgramTL();
			if (stage == 0) BindVertexProgram();
			if (!bound) return false;
			BindFragmentProgram(FRAGMENT_CLEAR);
			float cs[Rasterizer::TL_CONST_COUNT][4];
			memcpy(cs, c, sizeof(cs));
			if (stage == 2 || stage >= 4) cs[11 - 3][3] = 0.0f;
			SetVertexConstantBlock(3, Rasterizer::TL_CONST_COUNT, &cs[0][0]);
			gcmContextData *ctx = Context();
			if (cull) {
				rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW);
				rsxSetCullFace(ctx, GCM_CULL_BACK);
				rsxSetCullFaceEnable(ctx, GCM_TRUE);
			}
			uint32_t first = 0;
			Vertex *mem = AllocVertices((int)((sizeof(quad) + sizeof(Vertex) - 1) / sizeof(Vertex)), &first);
			uint32_t ioff = 0;
			uint32_t *ib = AllocIndices(12, &ioff);
			if (!mem || !ib) return false;
			memcpy(mem, quad, sizeof(quad));
			for (int i = 0; i < 12; ++i) ib[i] = i;
			__sync_synchronize();
			BindTLAttribs(RingOffset(mem));
			DrawIndexedBound(GCM_TYPE_TRIANGLES, ioff, 12);
			rsxSetCullFaceEnable(ctx, GCM_FALSE);
			const bool ok = f.Read();
			BindVertexProgram();
			EndFrame();
			return ok;
		};
		char text[320];
		// Stages 0-2 for the log (white quads expected).
		std::string stages;
		static const char *names[7] = {"main program", "pass-through", "T&L unlit", "", "position via temp", "position per component", "T&L without lights"};
		static const int order[6] = {0, 1, 4, 5, 6, 2};
		for (int k = 0; k < 6; ++k) {
			const int stage = order[k];
			char part[96];
			if (draw(false, stage))
				snprintf(part, sizeof(part), "%s%s %08x/%08x", stage ? ", " : "stages (want ffffffff): ", names[stage], f.Pixel(W / 4, H / 2), f.Pixel(3 * W / 4, H / 2));
			else
				snprintf(part, sizeof(part), "%s%s did not finish", stage ? ", " : "stages: ", names[stage]);
			stages += part;
		}
		PS3Frontend::Trace("RSX T&L self-test %s", stages.c_str());
		if (!draw(false)) {
			res.summary = "GPU did not finish";
		} else {
			const uint32_t l = f.Pixel(W / 4, H / 2), r = f.Pixel(3 * W / 4, H / 2);
			if (!Near(l, want, 6) || !Near(r, want, 6)) {
				snprintf(text, sizeof(text), "lit color wrong: left %08x right %08x want %08x", l, r, want);
				res.summary = text;
			} else if (!draw(true)) {
				res.summary = "GPU did not finish (cull pass)";
			} else {
				const uint32_t l2 = f.Pixel(W / 4, H / 2), r2 = f.Pixel(3 * W / 4, H / 2);
				const bool lDrawn = Near(l2, want, 6), rDrawn = Near(r2, want, 6);
				if (lDrawn == rDrawn) {
					snprintf(text, sizeof(text), "culling did not pick one winding: left %08x right %08x", l2, r2);
					res.summary = text;
				} else {
					// The surviving quad is the RSX's front (CCW) one.
					res.positiveCrossIsCCW = lDrawn == (cross > 0.0f);
					res.ok = true;
					snprintf(text, sizeof(text), "ok: color %08x, positive cross = %s", l, res.positiveCrossIsCCW ? "CCW" : "CW");
					res.summary = text;
				}
			}
		}
	}
	Finish();
	IoRelease();
	StepQuiet("idle");
	return res;
}

RSXCalibration RSXCalibrate() {
	RSXCalibration c;
	if (!Ready() || !LoadPrograms()) {
		c.summary = "RSX not ready";
		return c;
	}
	IoMark();
	{
		Fixture f;
		if (!f.Init()) {
			IoRelease();
			c.summary = "no memory for calibration";
			return c;
		}
		// Readback itself: a clear must come back as written.
		f.Begin();
		ClearTarget(0x11223344, true, true);
		if (!f.Read() || f.Pixel(0, 0) != 0x11223344) {
			char text[96];
			snprintf(text, sizeof(text), "readback check failed (%08x)", f.Pixel(0, 0));
			c.summary = text;
			IoRelease();
			return c;
		}
		// Orientation.
		const uint8_t red[4] = {255, 0, 0, 255};
		f.Begin();
		ClearTarget(0xFF000000, true, true);
		BindFragmentProgram(FRAGMENT_CLEAR);
		Rect(0, 0, W, 16, 0.5f, red);
		f.Read();
		const bool top = (f.Pixel(4, 4) & 0xFFFFFF) == 0xFF0000, bottom = (f.Pixel(4, H - 5) & 0xFFFFFF) == 0xFF0000;
		c.flipY = !top && bottom;
		bool ok = top || bottom;

		// Vertex color bytes.
		const uint8_t pattern[4] = {0x10, 0x20, 0x30, 0x40};
		f.Begin();
		ClearTarget(0, true, true);
		BindFragmentProgram(FRAGMENT_CLEAR);
		Rect(0, 0, W, H, 0.5f, pattern);
		f.Read();
		const uint32_t px = f.Pixel(8, 8);
		const int got[4] = {(int)((px >> 16) & 0xFF), (int)((px >> 8) & 0xFF), (int)(px & 0xFF), (int)(px >> 24)};  // R, G, B, A
		unsigned char order[4] = {255, 255, 255, 255};
		bool perm = true;
		for (int channel = 0; channel < 4 && perm; ++channel) {
			const int index = (got[channel] + 8) / 0x10 - 1;  // Which vertex byte reached this channel.
			if (index < 0 || index > 3 || order[index] != 255) perm = false;
			else order[index] = (unsigned char)channel;
		}
		if (perm) memcpy(c.colorOrder, order, 4);
		ok = ok && perm;

		// Texture bytes.
		uint32_t *tex = (uint32_t *)LocalAlloc(64 * 4, 128);
		if (tex) {
			uint8_t *b = (uint8_t *)tex;
			for (int i = 0; i < 64 * 4; i += 4) { b[i] = 0x11; b[i + 1] = 0x22; b[i + 2] = 0x33; b[i + 3] = 0x44; }
			f.Begin();
			ClearTarget(0, true, true);
			InvalidateTextureCache();
			BindTexture(0, LocalOffset(tex), 4, 4, 64, false, true, true);
			BindFragmentProgram(FragmentIndex(true, 3, true, false, false, false));
			const uint8_t white[4] = {255, 255, 255, 255};
			Rect(0, 0, W, H, 0.5f, white);
			f.Read();
			const uint32_t tp = f.Pixel(8, 8);
			unsigned char texOrder[4];
			bool tperm = true;
			int seen = 0;
			for (int j = 0; j < 4 && tperm; ++j) {
				const int value = (tp >> (24 - 8 * j)) & 0xFF;
				const int index = value / 0x11 - 1;
				if (value % 0x11 != 0 || index < 0 || index > 3 || (seen & (1 << index))) tperm = false;
				else { texOrder[j] = (unsigned char)index; seen |= 1 << index; }
			}
			if (tperm) memcpy(c.texOrder, texOrder, 4);
			ok = ok && tperm;
			LocalFree(tex);
		}
		EndFrame();
		c.ok = ok;
		char text[160];
		snprintf(text, sizeof(text), "%s: flipY=%d colorOrder=%d%d%d%d texOrder=%d%d%d%d", ok ? "ok" : "incomplete", c.flipY ? 1 : 0,
			c.colorOrder[0], c.colorOrder[1], c.colorOrder[2], c.colorOrder[3], c.texOrder[0], c.texOrder[1], c.texOrder[2], c.texOrder[3]);
		c.summary = text;
	}
	Finish();
	IoRelease();
	StepQuiet("idle");
	return c;
}

void RSXSetProgressCallback(RSXProgressCallback callback) {
	g_progress = callback;
}

std::vector<std::string> RSXRunSelfTest(bool vram) {
	Report r;
	g_cancelled = false;
	r.title = vram ? "VRAM test" : "Self-test";
	r.expected = vram ? 11 : 44;
	Progress(r.title, 0.0, true);
	r.Line("%s - RSX %s", PS3Frontend::ProductName(), vram ? "VRAM test" : "self-test");
	if (!r.Check(Ready(), "RSX context available")) return r.lines;
	r.Line("INFO  %s", Describe().c_str());
	IoMark();
	if (vram) {
		RunVramTests(r);
		RunFullVramSweep(r);
	} else {
		if (r.Check(LoadPrograms(), "Load embedded vertex program and fragment programs into RSX memory"))
			RunRenderingTests(r);
	}
	Finish();
	IoRelease();
	StepQuiet("idle");
	r.Line("%d passed, %d failed", r.passed, r.failed);
	FILE *file = fopen(PS3Frontend::ReportPath(vram ? "rsx_vramtest.txt" : "rsx_selftest.txt").c_str(), "w");
	if (file) {
		for (const auto &line : r.lines) fprintf(file, "%s\n", line.c_str());
		fclose(file);
	}
	return r.lines;
}

std::vector<std::string> RSXRunStressTest(double seconds) {
	Report r;
	g_cancelled = false;
	r.title = "Stress test";
	r.Line("%s - RSX stress test (%.0f s)", PS3Frontend::ProductName(), seconds);
	if (!r.Check(Ready(), "RSX context available")) return r.lines;
	IoMark();
	if (r.Check(LoadPrograms(), "Load programs"))
		RunStress(r, seconds);
	Finish();
	IoRelease();
	StepQuiet("idle");
	r.Line("%d passed, %d failed", r.passed, r.failed);
	FILE *file = fopen(PS3Frontend::ReportPath("rsx_stresstest.txt").c_str(), "w");
	if (file) {
		for (const auto &line : r.lines) fprintf(file, "%s\n", line.c_str());
		fclose(file);
	}
	return r.lines;
}
