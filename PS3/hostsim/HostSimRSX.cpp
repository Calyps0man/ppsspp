// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Desktop stand-in for the RSX layer (PS3/rsx/RSXCore.h, the PSL1GHT command functions and the
// frontend bits the renderer uses), so PS3/rsx/RSXRenderer.cpp runs in PPSSPPHeadless on a PC:
// GE dumps replayed through the real renderer code, for profiling it (callgrind) and checking it
// without a PS3. Nothing is drawn. Commands go into a dummy buffer, so their cost is still there.
// Enabled with PPSSPP_RSX_SIM=1 (=2: on the GE worker thread, like the PS3 default).
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include "PS3/rsx/RSXCore.h"
#include "PS3/rsx/RSXRenderer.h"
#include "PS3/rsx/RSXSelfTest.h"
#include "PS3/frontend/Frontend.h"
#include "GPU/Software/HardwareBackend.h"

namespace {

// Fake RSX local memory and I/O region: offsets are from their bases.
constexpr uint32_t LOCAL_SIZE = 256 * 1024 * 1024;
constexpr uint32_t IO_SIZE = RSX::IO_REGION_SIZE;
uint8_t *g_local = nullptr;
uint8_t *g_ioMem = nullptr;
uint32_t g_ioUsed = 0, g_ioMark = 0;

// First-fit list allocator for local memory (the renderer frees and reallocates textures).
struct Block { uint32_t off, size; bool used; };
std::vector<Block> g_blocks;

uint32_t g_cmd[1 << 16];
gcmContextData g_ctx;
uint32_t g_cmdWords = 0;

inline void Emit(int n, uint32_t a = 0, uint32_t b = 0) {
	uint32_t *p = g_cmd + (g_cmdWords & 0x7FF0);  // Room for the longest command (constant blocks) after it.
	p[0] = a;
	p[1] = b;
	for (int i = 2; i < n; ++i) p[i] = a ^ i;
	g_cmdWords += n;
}

uint32_t g_vertRing = 0;
const uint32_t RING_BYTES = 32 * 1024 * 1024;
uint8_t *g_ring = nullptr;
uint32_t g_ringUsed = 0;

void *RingAlloc(uint32_t bytes) {
	if (bytes > RING_BYTES) return nullptr;
	if (g_ringUsed + bytes > RING_BYTES) g_ringUsed = 0;
	void *p = g_ring + g_ringUsed;
	g_ringUsed += (bytes + 127) & ~127u;
	return p;
}

}  // namespace

extern "C" {
void rsxSetCullFaceEnable(gcmContextData *, u32 enable) { Emit(2, 0x1, enable); }
void rsxSetStencilFunc(gcmContextData *, u32 func, u32 ref, u32 mask) { Emit(4, func, ref ^ mask); }
void rsxSetStencilTestEnable(gcmContextData *, u32 enable) { Emit(2, 0x2, enable); }
void rsxSetStencilOp(gcmContextData *, u32 fail, u32 depthFail, u32 depthPass) { Emit(4, fail, depthFail ^ depthPass); }
void rsxSetStencilMask(gcmContextData *, u32 mask) { Emit(2, 0x3, mask); }
void rsxSetShadeModel(gcmContextData *, u32 shadeModel) { Emit(2, 0x4, shadeModel); }
void rsxSetPointSize(gcmContextData *, f32 size) { Emit(2, 0x5, (u32)size); }
void rsxSetLogicOpEnable(gcmContextData *, u32 enable) { Emit(2, 0x6, enable); }
void rsxSetLogicOp(gcmContextData *, u32 op) { Emit(2, 0x7, op); }
void rsxSetFrontFace(gcmContextData *, u32 dir) { Emit(2, 0x8, dir); }
void rsxSetDepthWriteEnable(gcmContextData *, u32 enable) { Emit(2, 0x9, enable); }
void rsxSetDepthTestEnable(gcmContextData *, u32 enable) { Emit(2, 0xA, enable); }
void rsxSetDepthFunc(gcmContextData *, u32 func) { Emit(2, 0xB, func); }
void rsxSetCullFace(gcmContextData *, u32 cull) { Emit(2, 0xC, cull); }
void rsxSetColorMask(gcmContextData *, u32 mask) { Emit(2, 0xD, mask); }
void rsxSetBlendFunc(gcmContextData *, u16 sfcolor, u16 dfcolor, u16 sfalpha, u16 dfalpha) { Emit(3, sfcolor | (dfcolor << 16), sfalpha | (dfalpha << 16)); }
void rsxSetBlendEquation(gcmContextData *, u16 color, u16 alpha) { Emit(2, 0xE, color | (alpha << 16)); }
void rsxSetBlendEnable(gcmContextData *, u32 enable) { Emit(2, 0xF, enable); }
void rsxSetBlendColor(gcmContextData *, u32 color0, u32 color1) { Emit(3, color0, color1); }
void rsxSetAlphaTestEnable(gcmContextData *, u32 enable) { Emit(2, 0x10, enable); }
void rsxSetAlphaFunc(gcmContextData *, u32 alphaFunc, u32 ref) { Emit(3, alphaFunc, ref); }
}

namespace RSX {

bool Ready() { return true; }
void SetVertexRingInMain(bool) {}
bool VertexRingInMain() { return true; }
uint32_t RingOffset(const void *ptr) { return (uint32_t)((const uint8_t *)ptr - g_ring); }
void InitDiagnostics() {}
void Step(const char *) {}
void StepQuiet(const char *) {}
std::string GpuStateText() { return "host simulation"; }
_gcmCtxData *Context() { return &g_ctx; }
std::string Describe() { return "host simulation"; }

void *LocalAlloc(uint32_t size, uint32_t align) {
	if (!g_local) {
		g_local = (uint8_t *)aligned_alloc(1024 * 1024, LOCAL_SIZE);
		g_blocks.push_back({0, LOCAL_SIZE, false});
	}
	size = (size + 1023) & ~1023u;
	(void)align;  // Everything is 1 KB aligned.
	for (size_t i = 0; i < g_blocks.size(); ++i) {
		Block b = g_blocks[i];
		if (b.used || b.size < size) continue;
		g_blocks[i] = {b.off, size, true};
		if (b.size > size) g_blocks.insert(g_blocks.begin() + i + 1, {b.off + size, b.size - size, false});
		return g_local + b.off;
	}
	return nullptr;
}

void LocalFree(void *ptr) {
	if (!ptr) return;
	const uint32_t off = (uint32_t)((uint8_t *)ptr - g_local);
	for (size_t i = 0; i < g_blocks.size(); ++i) {
		if (g_blocks[i].off != off) continue;
		g_blocks[i].used = false;
		if (i + 1 < g_blocks.size() && !g_blocks[i + 1].used) {
			g_blocks[i].size += g_blocks[i + 1].size;
			g_blocks.erase(g_blocks.begin() + i + 1);
		}
		if (i > 0 && !g_blocks[i - 1].used) {
			g_blocks[i - 1].size += g_blocks[i].size;
			g_blocks.erase(g_blocks.begin() + i);
		}
		return;
	}
}

uint32_t LargestFreeLocal() {
	uint32_t m = 0;
	for (const Block &b : g_blocks)
		if (!b.used && b.size > m) m = b.size;
	return m & ~(1024u * 1024 - 1);
}
uint32_t LocalOffset(const void *ptr) { return (uint32_t)((const uint8_t *)ptr - g_local); }

void *IoAlloc(uint32_t size, uint32_t align) {
	if (!g_ioMem) g_ioMem = (uint8_t *)aligned_alloc(1024 * 1024, IO_SIZE);
	g_ioUsed = (g_ioUsed + align - 1) & ~(align - 1);
	if (g_ioUsed + size > IO_SIZE) return nullptr;
	void *p = g_ioMem + g_ioUsed;
	g_ioUsed += size;
	return p;
}
uint32_t IoOffset(const void *ptr) { return (uint32_t)((const uint8_t *)ptr - g_ioMem); }
void IoMark() { g_ioMark = g_ioUsed; }
void IoRelease() { g_ioUsed = g_ioMark; }
static bool g_simTiles[15];
bool TilePitchSupported(uint32_t pitch) { return pitch == 0x800 || pitch == 0x1000 || pitch == 0x400 || pitch == 0x2000; }
int BindTileRegion(uint32_t offset, uint32_t size, uint32_t pitch, int bank) {
	if (!TilePitchSupported(pitch) || (offset & 0xFFFF) || (size & 0xFFFF)) return -1;
	for (int i = 2; i < 15; ++i) if (!g_simTiles[i]) { g_simTiles[i] = true; return i; }
	return -1;
}
double TakeFlipWait() { return 0.0; }
void UnbindTileRegion(int region) { if (region >= 2 && region < 15) g_simTiles[region] = false; }

static uint32_t g_fence = 0;
uint32_t Fence() { Emit(2, 0x20, ++g_fence); return g_fence; }
bool FenceDone(uint32_t) { return true; }
bool WaitFence(uint32_t, double) { return true; }
bool Finish(double) { Fence(); return true; }
void GpuWaitForRendering() { Emit(4, 0x21); }
void Flush() {}

bool LoadPrograms() { return true; }
void BindVertexProgram() { Emit(40, 0x30); }
bool BindVertexProgramTL() { Emit(200, 0x31); return true; }
bool BindVertexProgramTLMin() { return true; }
bool BindTestVertexProgram(int) { return true; }
void SetVertexConstantBlock(int first, int count, const float *values) {
	Emit(2 + count * 4, (uint32_t)first, (uint32_t)values[0]);
}
int FragmentIndex(bool textured, int texFunc, bool rgba, bool doubling, bool fog, bool blendMul) {
	return 1 + ((textured ? 1 : 0) | (texFunc << 1) | (rgba ? 16 : 0) | (doubling ? 32 : 0) | (fog ? 64 : 0) | (blendMul ? 128 : 0)) % 160;
}
int FragmentFixSub(bool rgba, bool doubling, bool reverse) { return 163 + (rgba ? 4 : 0) + (doubling ? 2 : 0) + (reverse ? 1 : 0); }
void BindFragmentProgram(int index) { Emit(8, 0x32, (uint32_t)index); }
void SetVertexConstants(const float env[4], const float fog[4], const float mul[4]) {
	Emit(14, (uint32_t)env[0], (uint32_t)(fog[0] + mul[0]));
}

void SetTarget(const Target &t) { Emit(20, t.colorOffset, t.depthOffset); }
void SetPixelViewport(uint16_t width, uint16_t height) { Emit(16, width, height); }
void SetScissor(int x, int y, int w, int h) { Emit(3, (uint32_t)(x | (w << 16)), (uint32_t)(y | (h << 16))); }
void ClearTarget(uint32_t argb, bool, bool, uint32_t v) { Emit(6, argb, v); }
void SetDefaultState() { Emit(40, 0x40); }
void ResetDrawState() {}

Vertex *AllocVertices(int count, uint32_t *firstIndex) {
	*firstIndex = g_vertRing;
	g_vertRing += count;
	return (Vertex *)RingAlloc(count * sizeof(Vertex));
}
void DrawVertices(uint32_t primType, uint32_t firstIndex, uint32_t count) { Emit(8, primType, firstIndex + count); }
uint32_t *AllocIndices(int count, uint32_t *offset) {
	uint32_t *p = (uint32_t *)RingAlloc(count * 4);
	*offset = p ? (uint32_t)((uint8_t *)p - g_ring) : 0;
	return p;
}
void DrawIndexed(uint32_t primType, uint32_t indexOffset, uint32_t count) { Emit(12, primType, indexOffset + count); }
void BindTLAttribs(uint32_t offset) { Emit(30, 0x50, offset); }
TLVertex *AllocTLVertices(int count, uint32_t *baseIndex) {
	*baseIndex = g_vertRing;
	g_vertRing += count;
	return (TLVertex *)RingAlloc(count * sizeof(TLVertex));
}
void BindTLAttribsRing() { Emit(30, 0x51); }
void DrawIndexedBound(uint32_t primType, uint32_t indexOffset, uint32_t count) { Emit(16, primType, indexOffset + count); }
void EndFrame() { g_vertRing = 0; }

void BindTexture(int unit, uint32_t offset, uint32_t width, uint32_t height, uint32_t pitch, bool, bool, bool, bool, bool) {
	Emit(24, offset, (uint32_t)unit ^ width ^ height ^ pitch);
}
void InvalidateTextureCache() { Emit(2, 0x60); }
void SetMaxAnisotropy(int) {}
void ScaleImage(Mem, uint32_t srcOffset, uint32_t, int, int, Mem dstMem, uint32_t dstOffset, uint32_t dstPitch, int dstX, int dstY, int dstW, int dstH, bool) {
	Emit(30, srcOffset, dstOffset);
	// PPSSPP_RSX_SIM_FILL: a copy to main memory (a write-back) brings a marker colour, as if the
	// RSX had drawn every pixel, so tests see which bytes a write-back would change.
	static const bool fill = getenv("PPSSPP_RSX_SIM_FILL") != nullptr;
	if (fill && dstMem == Mem::Main) {
		for (int y = dstY; y < dstY + dstH; ++y) {
			uint32_t *row = (uint32_t *)(g_ioMem + dstOffset + y * dstPitch);
			for (int x = dstX; x < dstX + dstW; ++x)
				row[x] = 0xFF3366CC;
		}
	}
}
void CopyRows(Mem, uint32_t srcOffset, uint32_t, Mem, uint32_t dstOffset, uint32_t, uint32_t, uint32_t) { Emit(20, srcOffset, dstOffset); }

bool InitDisplay() { return true; }
int DisplayWidth() { return 1280; }
int DisplayHeight() { return 720; }
static void *g_back = nullptr;
uint32_t BackBufferOffset() {
	if (!g_back) g_back = LocalAlloc(1280 * 4 * 720);
	return LocalOffset(g_back);
}
void *BackBufferPtr() {
	BackBufferOffset();
	return g_back;
}
bool WaitFlip(double) { return true; }
uint32_t DisplayPitch() { return 1280 * 4; }
void Present() { Emit(4, 0x70); }
void GpuWaitFlip() {}
void ResetDisplay() {}

}  // namespace RSX

RSXCalibration RSXCalibrate() {
	RSXCalibration c;
	c.ok = true;
	c.summary = "host simulation";
	return c;
}

RSXTLTestResult RSXTestTL() {
	RSXTLTestResult r;
	r.ok = true;
	r.summary = "host simulation";
	return r;
}

namespace PS3Frontend {
void Trace(const char *format, ...) {
	if (!getenv("PPSSPP_RSX_SIM_TRACE"))
		return;
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputc('\n', stderr);
}
}  // namespace PS3Frontend

namespace {
RSXRenderer *g_renderer = nullptr;
}

// Called from InstallBenchBackendFromEnv (TransformUnit.cpp) and once per replayed frame.
Rasterizer::HardwareBackend *HostSimRSXCreate(bool worker) {
	g_ring = (uint8_t *)aligned_alloc(4096, RING_BYTES);
	g_ctx.begin = g_cmd;
	g_ctx.end = g_cmd + (1 << 16);
	g_ctx.current = g_cmd;
	RSXRendererOptions o;
	o.lateDepth = getenv("PPSSPP_RSX_SIM_LATEDEPTH") != nullptr;
	o.keepLayouts = getenv("PPSSPP_RSX_SIM_KEEPLAYOUTS") != nullptr;
	o.fxaa = getenv("PPSSPP_RSX_SIM_FXAA") ? atoi(getenv("PPSSPP_RSX_SIM_FXAA")) : 0;
	o.cpuRows = getenv("PPSSPP_RSX_SIM_CPUROWS") != nullptr;
	g_renderer = RSXRendererCreate(o);
	if (!g_renderer) {
		fprintf(stderr, "RSX sim: renderer create failed\n");
		return nullptr;
	}
	Rasterizer::HardwareBackend *b = RSXRendererBackend(g_renderer);
	return worker ? Rasterizer::StartGEWorker(b) : b;
}

void HostSimRSXFrame() {
	if (!g_renderer) return;
	if (Rasterizer::g_hardwareBackend) Rasterizer::g_hardwareBackend->Sync();
	RSXRendererPresent(g_renderer, std::string());
	// PPSSPP_RSX_SIM_WRITEBACK: everything the RSX drew written back to VRAM after each frame, so
	// a screenshot shows the RSX's picture (otherwise only what reached VRAM on its own).
	if (getenv("PPSSPP_RSX_SIM_WRITEBACK"))
		RSXRendererWriteBackAll(g_renderer);
	static int frames = 0;
	if (++frames % (getenv("PPSSPP_RSX_SIM_EVERY") ? atoi(getenv("PPSSPP_RSX_SIM_EVERY")) : 50) == 0)
		fprintf(stderr, "RSX sim: %s (%u command words)\n", RSXRendererStats(g_renderer).c_str(), g_cmdWords);
}
