// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#include <algorithm>
#include "Common/FrontendTrace.h"
#include <chrono>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <ppu-types.h>
#if defined(__ALTIVEC__)
#include <altivec.h>
#undef vector
#undef pixel
#undef bool
#endif
#include <rsx/rsx.h>

#include "ext/xxhash.h"
#include "Common/Math/math_util.h"
#include "Core/MemMap.h"
#include "Core/HLE/sceDisplay.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Math3D.h"
#include "GPU/Software/FuncId.h"
#include "GPU/Software/Rasterizer.h"
#include "GPU/Software/RasterizerRegCache.h"
#include "GPU/Software/Sampler.h"
#include "GPU/Software/TransformUnit.h"

#include "PS3/frontend/Frontend.h"
#include "PS3/rsx/RSXCore.h"
#include "PS3/rsx/RSXRenderer.h"
#include "PS3/rsx/RSXSelfTest.h"
#include "PS3/rsx/TexSpu.h"

// GE registers: a queued draw's own copy while the GE worker thread draws it, otherwise gstate.
static inline const GEState &CurrentGEState() {
	return Rasterizer::g_drawStateOverride ? *Rasterizer::g_drawStateOverride : gstate;
}
#define GS CurrentGEState()

using namespace Rasterizer;


// Renderer events (not setup or errors): the Debug tab's "RSX renderer events" (rsxlog).
#define RSXEventLog(...) do { if (FrontendLogOn(FrontendLog::RSX)) PS3Frontend::Trace(__VA_ARGS__); } while (0)

namespace {

constexpr int PSP_W = 480, PSP_H = 272;
constexpr uint32_t VRAM_MASK = 0x041FFFFF;
constexpr int MAX_BATCH = 6000;  // Vertices; must stay below one vertex ring segment.
constexpr uint32_t TEXTURE_BUDGET = 96u * 1024 * 1024;

static const uint8_t *g_font = nullptr;

inline int BytesPerPixel(GEBufferFormat fmt) { return fmt == GE_FORMAT_8888 ? 4 : 2; }

uint32_t NormalizeAddr(uint32_t addr) {
	if (Memory::IsVRAMAddress(addr)) return addr & VRAM_MASK;
	return addr & 0x3FFFFFFF;
}

// A write-back row may cover a display list the GE runs from VRAM (GeVramListOverlap): its bytes
// are saved before the row is written and put back after. Midnight Club L.A. keeps a list right
// after a small effect buffer whose scissor reaches past it; written back over, the list turned
// to pixels and the GE ran them (brown screen).
struct ListGuard {
	uint32_t addr = 0, len = 0;
	uint8_t saved[4096];
	void Begin(uint32_t rowAddr, const uint8_t *row, uint32_t b0, uint32_t b1) {
		len = 0;
		uint32_t ls, le;
		if (!GeVramListOverlap(rowAddr + b0, rowAddr + b1, &ls, &le))
			return;
		addr = std::max(ls, rowAddr + b0);
		const uint32_t end = std::min(le, rowAddr + b1);
		if (end <= addr || end - addr > sizeof(saved))
			return;
		len = end - addr;
		memcpy(saved, row + (addr - rowAddr), len);
	}
	void End(uint32_t rowAddr, uint8_t *row, const char *what, uint32_t surfAddr, int y) {
		if (!len)
			return;
		uint8_t *p = row + (addr - rowAddr);
		if (memcmp(p, saved, len) != 0) {
			static int logs = 0;
			if (logs < 8) {
				logs++;
				uint32_t ls = 0, le = 0;
				GeVramListOverlap(addr, addr + len, &ls, &le);
				RSXEventLog("RSX renderer: %s write-back of %08x row %d would have changed the display list at %08x-%08x (bytes %08x-%08x) - left as it was",
					what, surfAddr, y, ls, le, addr, addr + len);
			}
			memcpy(p, saved, len);
		}
	}
};

uint16_t CompareFunc(int ge) {
	static const uint16_t table[8] = {GCM_NEVER, GCM_ALWAYS, GCM_EQUAL, GCM_NOTEQUAL, GCM_LESS, GCM_LEQUAL, GCM_GREATER, GCM_GEQUAL};
	return table[ge & 7];
}

uint16_t StencilOp(int ge) {
	switch (ge) {
	case GE_STENCILOP_ZERO: return GCM_ZERO;
	case GE_STENCILOP_REPLACE: return GCM_REPLACE;
	case GE_STENCILOP_INVERT: return GCM_INVERT;
	case GE_STENCILOP_INCR: return GCM_INCR;
	case GE_STENCILOP_DECR: return GCM_DECR;
	default: return GCM_KEEP;
	}
}

// 0xBBGGRR (GE register layout) -> floats.
void ColorToFloats(uint32_t bgr, float alpha, float out[4]) {
	out[0] = (bgr & 0xFF) / 255.0f;
	out[1] = ((bgr >> 8) & 0xFF) / 255.0f;
	out[2] = ((bgr >> 16) & 0xFF) / 255.0f;
	out[3] = alpha;
}

uint32_t BgrToArgb(uint32_t bgr) {
	return 0xFF000000 | ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF);
}

// Cheap timing with the PPU time base (79.8 MHz on the PS3).
static inline uint64_t TimeBase() {
#if defined(__powerpc__) || defined(__PPU__)
	uint64_t tb;
	__asm__ volatile("mftb %0" : "=r"(tb));
	return tb;
#else
	// Host simulation (PS3/hostsim): the same 79.8 MHz units.
	return (uint64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 79800000.0);
#endif
}
struct TbScope {
	uint64_t &acc;
	uint64_t start;
	explicit TbScope(uint64_t &a) : acc(a), start(TimeBase()) {}
	~TbScope() { acc += TimeBase() - start; }
};

struct Surface {
	uint32_t addr = 0;
	uint16_t stride = 0;
	GEBufferFormat fmt = GE_FORMAT_565;
	void *mem = nullptr;
	uint32_t offset = 0, pitch = 0;
	uint16_t w = 0, h = 0;     // Scaled size of the RSX buffer.
	int rows = PSP_H;          // PSP rows in use (for hashing/sync).
	bool gpuDirty = false;     // Rendered on the RSX since the last write-back to VRAM.
	bool synced = false;       // syncHash valid.
	// Left on the RSX without a write-back at a layout switch (keepLayouts): its pixels are newer
	// than VRAM's, so a few rows the CPU changed since (Midnight Club's display lists) don't make
	// it upload VRAM over them.
	bool keptOnRsx = false;
	uint64_t syncHash = 0;     // Hash of VRAM right after the last upload/write-back.
	// Each row's hash at that point. A write-back skips rows whose VRAM changed since (the CPU or a
	// transfer put a texture there): rows past the displayed area that the game never drew still
	// hold what VRAM had when we last synced, and writing those back wiped textures (Killzone's
	// menu text came out as white boxes).
	std::vector<uint32_t> rowHash;
	// PSP rows the RSX has drawn into (scissor of each draw) since the last sync. Only these are
	// written back: the rest of our copy is just what we uploaded, and VRAM may hold textures the
	// game put there since (Gran Turismo keeps its DXT sky right after an 8888 render target, and
	// writing back all 272 rows turned the sky black).
	int drawnY0 = 0x7FFFFFFF, drawnY1 = 0;
	// PSP columns the RSX has ever drawn into (draw scissors). Only these are written back and
	// watched for CPU changes: a buffer laid over another's spare columns (Silent Hill Origins puts
	// a 16-bit blur strip at x 960-1023 of a 1024-wide buffer, the bytes of its 8888 screen's
	// margin) had its rows rewritten by the other's whole-row write-backs and vice versa.
	int ownX0 = 0x7FFFFFFF, ownX1 = 0;
	// Columns drawn since the last write-back (with drawnY0/Y1), and version at the last small
	// write-back at a sync point (RSXRenderer::Sync).
	int drawnX0 = 0x7FFFFFFF, drawnX1 = 0;
	uint32_t syncWbVersion = 0xFFFFFFFF;
	uint32_t dlSerial = 0;     // Counts full write-backs (Download).
	int hashX0 = 0, hashX1 = 0;  // Columns the row hashes cover (0, 0: whole rows).
	uint32_t keptRows = 0;
	uint32_t lastFrame = 0;
	uint32_t validatedFrame = 0xFFFFFFFF, validatedGen = 0xFFFFFFFF;  // VRAM checked once per frame.
	uint32_t cpuCheckFrame = 0xFFFFFFFF;  // MergeCpuRows: checked once per frame.
	uint32_t version = 0;      // Bumped whenever its contents may change (draws, uploads).
	uint32_t drawSeq = 0;      // When last drawn into (RSXRenderer::drawSeq_), 0: never.
	uint32_t rttWaitVersion = 0xFFFFFFFF;  // version when a texture read last waited for its drawing.
	// Sampled in place by draws to another buffer, which the RSX may still be running: drawing
	// into it must wait for them (EnsureTarget).
	bool sampledPending = false;
	int tile = -1;             // Tiled region (RSX::BindTileRegion), -1: plain memory.
};

struct DepthSurface {
	uint32_t addr = 0;
	void *mem = nullptr;
	uint32_t offset = 0, pitch = 0;
	uint16_t w = 0, h = 0;
	bool fresh = false;  // Not cleared yet.
	int tile = -1;
	// Drawn on the RSX since the last write-back to PSP memory (rows drawnY0..drawnY1).
	bool gpuDirty = false;
	int drawnY0 = 0x7FFFFFFF, drawnY1 = 0;
	uint16_t stride = 0;  // The GE's depth buffer stride (pixels) when last bound.
	uint64_t vramProbe = 0;  // ProbeHash of its PSP memory when it became dirty.
};


struct TexEntry {
	uint32_t addr = 0;
	uint8_t fmt = 0;
	uint16_t w = 0, h = 0, bufw = 0;
	uint64_t clutHash = 0;
	uint64_t dataHash = 0;
	uint32_t validatedFrame = 0xFFFFFFFF;
	uint32_t validatedGen = 0xFFFFFFFF;
	void *mem = nullptr;
	uint32_t offset = 0, pitch = 0, bytes = 0;
	uint32_t lastFrame = 0;
	uint16_t uvW = 0, uvH = 0;  // PSP texture size for through-mode UVs (0 = w/h).
	uint16_t stable = 0;  // Hash checks in a row that found the data unchanged.
	uint64_t probeHash = 0;  // ProbeHash() of the data at the last full hash.
	uint32_t validatedEpoch = 0;  // Rasterizer::g_geCpuEpoch of the last check.
};

struct HwState {
	int fragment;
	uint8_t tex, texLinMin, texLinMag, clampS, clampT, pad0;
	uint32_t texOffset, texW, texH, texPitch;
	uint32_t uvW, uvH;
	uint8_t blend, depthTest, depthWrite, alphaTest, stencil, logic, flat, pad1;
	uint16_t bsrc, bdst, beq, depthFunc, alphaFunc, sfunc, sfail, szfail, szpass, logicOp;
	uint32_t blendColor, colorMask;
	uint8_t alphaRef, sref, smask, swmask;
	int16_t sx, sy, sw, sh;
	float env[4], fog[4], mul[4];
};

}  // namespace

static uint64_t ProbeHash(const u8 *p, uint32_t bytes);
static inline uint32_t DepthProbeBytes(const DepthSurface &d) { return (uint32_t)(d.stride ? d.stride : 512) * 2 * PSP_H; }

class RSXRenderer : public HardwareBackend {
public:
	explicit RSXRenderer(const RSXRendererOptions &o) : opt_(o) {}
	bool Init();
	void Shutdown();

	void Triangle(const RasterizerState &state, const VertexData &v0, const VertexData &v1, const VertexData &v2) override;
	void Rect(const RasterizerState &state, const VertexData &v0, const VertexData &v1, bool clear) override;
	void Line(const RasterizerState &state, const VertexData &v0, const VertexData &v1) override;
	void Point(const RasterizerState &state, const VertexData &v0) override;
	void Flush(const char *reason) override;
	bool DrawTriangles(const RasterizerState &state, const FastVertex *verts, int count, bool through) override;
	bool DrawIndexed(const RasterizerState &state, const FastVertex *verts, int nverts, const uint16_t *indices, int nidx, bool through) override;
	bool SupportsTL() const override { return opt_.batched && opt_.hardwareTL && tlOk_; }
	bool DrawTL(const RasterizerState &state, const TLParams &p, const TLVertex *verts, int nverts, const uint16_t *indices, int nidx, int cull) override;
	bool HasPendingWrite(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) override;
	// Asked from the emulation thread while the GE worker may be drawing: no renderer state here
	// (the worker checks the address against what it queued). TakeCpuRows runs in order.
	bool CanTakeCpuRows(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) override { return true; }
	void TakeCpuRows(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) override;
	void WriteBackRange(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) override;
	void Sync() override;

	void Present(const std::string &overlay);
	std::string Stats();
	// Every buffer the RSX drew into, written back to PSP memory now (GE dump: the PS3's own result).
	void WriteBackAll() { FlushBatch(); DownloadAll(); }
	void SetLiveOptions(bool bilinear, bool keepLayouts, bool skipDepthReads, int fxaa) {
		opt_.bilinear = bilinear;
		opt_.keepLayouts = keepLayouts;
		opt_.skipDepthReads = skipDepthReads;
		opt_.fxaa = fxaa;
	}
	void SetCpuRows(bool on) { opt_.cpuRows = on; }

private:
	bool EnsureTarget();
	Surface *FindSurface(uint32_t addr, uint16_t stride, GEBufferFormat fmt, int rows, bool create, bool forDrawing = false);
	DepthSurface *FindDepth(uint32_t addr, uint16_t minW, uint16_t minH);
	void Upload(Surface &s);
	void Download(Surface &s);
	bool DownloadPart(Surface &s, int y0, int y1, int x0, int x1);
	void KeepLayoutsOverWrite(const Surface &s, uint32_t start, uint32_t end);
	void WriteBackSpan(uint32_t start, uint32_t end, uint32_t stride, uint32_t width, bool queued);
	void DownloadAll();
	void DownloadDepth(DepthSurface &d);
	uint64_t HashVram(const Surface &s) const;
	bool Validate(Surface &s);
	void MergeCpuRows(Surface &s);
	void ApplyState(const RasterizerState &state, bool clear);
	bool LookupTexture(const RasterizerState &state, TexEntry **out);
	void FlushBatch();
	void PushVertex(const VertexData &v, bool through, float sScale, float tScale, bool proj);
	void BeginPrim(uint32_t type);
	void DeferFree(void *mem, int tile = -1);
	// Render target memory: in a tiled region when the option is on and the pitch allows.
	void *AllocTarget(uint32_t size, uint32_t pitch, int bank, int *tile);
	void ProcessDeferredFrees(bool force);
	void *AllocLocal(uint32_t size);
	void EvictTextures(uint32_t frameAge);
	void DrawOverlay(const std::string &text, int x, int y);
	void ComputeGameRect();

	RSXRendererOptions opt_;
	int scale_ = 1;
	// Owned through pointers so Surface* stays valid while the lists change.
	std::vector<std::unique_ptr<Surface>> surfaces_;
	std::vector<std::unique_ptr<DepthSurface>> depths_;
	std::unordered_map<uint64_t, TexEntry> textures_;
	uint32_t textureBytes_ = 0;
	Surface *target_ = nullptr;
	DepthSurface *depth_ = nullptr;
	bool targetValid_ = false;
	uint32_t targetKey_[4]{};
	uint16_t targetW_ = 0, targetH_ = 0;
	int targetReqRows_ = 0;  // Rows asked for when the target was bound (before clamping).
	HwState cur_{};
	bool stateValid_ = false;
	// ApplyState memo: the same RasterizerState (the binner makes a new one whenever anything in it
	// changes) with the same scissor in the same frame needs no work at all.
	const RasterizerState *memoState_ = nullptr;
	// Draws queued to the GE worker each carry their own copy of the state, so the pointer alone
	// rarely matches: compare the contents too (DOA Paradise: ~35,000 queued triangles a frame).
	RasterizerState memoCopy_;
	alignas(16) uint8_t memoClut_[1024];
	bool memoHasClut_ = false;
	uint32_t memoHits_ = 0, memoMisses_ = 0;
	uint64_t tbTLTarget_ = 0, tbTLApply_ = 0;
	bool SameAsMemo(const RasterizerState &state) const;
	bool memoClear_ = false;
	uint32_t memoFrame_ = 0, memoGen_ = 0, memoScissor_[3]{};

	std::vector<RSX::Vertex> batch_;
	uint32_t batchType_ = GCM_TYPE_TRIANGLES;
	uint32_t *staging_ = nullptr;   // Main memory, 1024x512 ARGB.
	// Late depth reads (opt_.lateDepth): the copy started at the last read, finished by now.
	uint32_t *depthStaging_ = nullptr;
	// Sync: a few pixels of a buffer copied off the RSX without waiting, written to VRAM at a later
	// sync once the copy is done (into smallStaging_, SMALL_WB_W x SMALL_WB_H ARGB words).
	static constexpr int SMALL_WB_W = 64, SMALL_WB_H = 64;
	uint32_t *smallStaging_ = nullptr;
	struct {
		uint32_t fence = 0;  // 0: none pending.
		uint32_t addr = 0;
		uint16_t stride = 0;
		GEBufferFormat fmt = GE_FORMAT_565;
		int x0 = 0, x1 = 0, y0 = 0, y1 = 0;  // What goes back to VRAM.
		int cx0 = 0, cy0 = 0;                // Where the copy in smallStaging_ starts (it may be bigger).
		uint32_t dlSerial = 0;
	} smallWb_;
	struct LateDepth {
		uint32_t fence = 0;
		uint32_t addr = 0;
		int stride = 0, width = 0, y0 = 0, y1 = 0;
	} lateDepth_;
	unsigned lateDepthLogs_ = 0;
	void WriteDepthRows(uint32_t addr, int stride, int width, int y0, int y1, const uint32_t *src);
	uint32_t *overlayBuf_ = nullptr;
	struct Deferred { uint32_t fence; void *mem; int tile; };
	std::vector<Deferred> deferred_;
	int tileLogs_ = 0;
	uint32_t frame_ = 1;
	uint32_t vramGen_ = 1;
	int clearFrames_ = 2;
	int gameX_ = 0, gameY_ = 0, gameW_ = 0, gameH_ = 0;
	RSXCalibration cal_;
	bool identityTex_ = true;
	// Render to texture straight from our RSX copy of a framebuffer.
	TexEntry rtt_;
	RSX::Target boundTarget_;   // What EnsureTarget last bound.
	// Hardware T&L: self-test result, whether its vertex program is bound, last constants sent.
	bool tlOk_ = false;
	bool tlPositiveIsCCW_ = true;
	bool vpTL_ = false;
	int tlCull_ = 0;  // Culling mode currently set on the RSX for T&L draws (0 = off).
	bool tlConstValid_ = false;
	float tlConst_[Rasterizer::TL_CONST_COUNT][4];
	unsigned tlDraws_ = 0;
	void UseMainVertexProgram();
	// Palette lookups from framebuffers (LookupDepal): output texture, palette ring, last result.
	TexEntry depal_;
	void *depalMem_ = nullptr;
	uint32_t depalBytes_ = 0;
	// Edge smoothing (opt_.fxaa): the smoothed picture, scaled to the TV from here.
	void *fxaaMem_ = nullptr;
	uint32_t fxaaBytes_ = 0;
	uint64_t tbFxaa_ = 0;
	unsigned fxaaFrames_ = 0;
	bool FxaaPass(const Surface &s, uint32_t srcX, uint32_t srcY, int srcW, int srcH, uint32_t *outOffset, uint32_t *outPitch);
	uint32_t *clutRing_ = nullptr;
	int clutSlot_ = -1;
	uint64_t clutHash_ = 0;
	uint64_t depalKey_ = 0;
	unsigned depalPasses_ = 0, depal16Logs_ = 0, depalxLogs_ = 0, xferFlushLogs_ = 0, partDownloads_ = 0, queuedXfers_ = 0, syncWbLogs_ = 0;
	// The small copies for the game to read (Sync, build 259 diagnostics): started, rows written to
	// VRAM, rows kept because the CPU changed them since, copies dropped (a full write-back since),
	// syncs where a newer drawing waited because one was still in progress, syncs waited in all.
	unsigned swbIssued_ = 0, swbRowsWritten_ = 0, swbRowsKept_ = 0, swbDropped_ = 0, swbBusy_ = 0, swbPendingSyncs_ = 0, swbKeptLogs_ = 0;
	uint32_t drawSeq_ = 0;  // Counts draws into framebuffers (Surface::drawSeq).
	void *rttCopy_ = nullptr;   // Copy of the current target when a draw samples it.
	uint32_t rttCopyBytes_ = 0;
	// What rttCopy_ holds. A run of draws reading the buffer they draw to (DOA Paradise: ~16,000 per
	// frame) shares the copy made at the first read, until the game flushes textures, draws there
	// without reading it, or changes target. A full-screen texture doesn't fit the GE's 8 KB texture
	// cache, so a later pass must see what was drawn in between (Killzone menu).
	const Surface *rttCopySrc_ = nullptr;
	uint32_t rttCopyAddr_ = 0, rttCopyGen_ = 0, rttCopyFrame_ = 0;
	int rttCopyW_ = 0, rttCopyH_ = 0;
	unsigned rttCopies_ = 0, rttLogs_ = 0, rttShortLogs_ = 0;
	unsigned readWaits_ = 0;  // EnsureTarget waits for draws sampling the new target (perf line).
	unsigned depthDownloads_ = 0;
	// Texel rows the draw being set up samples, when its texture coordinates are known (2D draws):
	// only those rows of the texture are checked against framebuffers waiting for a write-back.
	// Midnight Club L.A. reads its depth buffer as a 512x512 CLUT16 texture but only 272 rows of
	// it; the rows past them are its small effect buffers, which were then written back for every
	// strip drawn into them (~7 write-backs a frame, the biggest cost in its GE work).
	int texRowsHint_ = 0;
	ListGuard listGuard_;
	// The draw being set up samples rttCopy_. A draw to the copied buffer that doesn't (new content
	// the game will want to see) makes the next read copy again.
	bool drawReadsRttCopy_ = false;
	// Rect sets this around ApplyState: a fixed-colour subtract blend may be done in the fragment
	// program (it fills in each vertex's destination coordinates).
	bool fixSubAllowed_ = false;
	unsigned fixSubLogs_ = 0;
	// bx0/bx1, by0/by1: PSP columns and rows the draw's vertices span, where known (rectangles,
	// points, lines, through-mode triangles); the scissor alone otherwise. Rows past what was drawn
	// can hold the game's data: Midnight Club L.A. draws 256x160 into a buffer whose scissor
	// reaches row 272, and keeps its VRAM heap (block headers, display lists) from row 162 on.
	void MarkTargetDrawn(int bx0 = -0x40000000, int bx1 = 0x40000000, int by0 = -0x40000000, int by1 = 0x40000000) {
		target_->gpuDirty = true;
		target_->version++;
		target_->drawSeq = ++drawSeq_;
		const int y0 = std::max(cur_.sy / scale_, by0), y1 = std::min((cur_.sy + cur_.sh + scale_ - 1) / scale_, by1);
		if (y1 <= y0)
			return;
		if (y0 < target_->drawnY0) target_->drawnY0 = y0;
		if (y1 > target_->drawnY1) target_->drawnY1 = y1;
		const int x0 = std::max(cur_.sx / scale_, bx0), x1 = std::min((cur_.sx + cur_.sw + scale_ - 1) / scale_, bx1);
		if (x0 < target_->ownX0) target_->ownX0 = x0;
		if (x1 > target_->ownX1) target_->ownX1 = x1;
		if (x0 < target_->drawnX0) target_->drawnX0 = x0;
		if (x1 > target_->drawnX1) target_->drawnX1 = x1;
		if (depth_ && cur_.depthWrite) {
			if (!depth_->gpuDirty && Memory::IsValidRange(depth_->addr, DepthProbeBytes(*depth_)))
				depth_->vramProbe = ProbeHash(Memory::GetPointerUnchecked(depth_->addr), DepthProbeBytes(*depth_));
			depth_->gpuDirty = true;
			if (y0 < depth_->drawnY0) depth_->drawnY0 = y0;
			if (y1 > depth_->drawnY1) depth_->drawnY1 = y1;
		}
		if (rttCopySrc_ == target_) {
			if (!drawReadsRttCopy_) {
				rttCopySrc_ = nullptr;
			} else if (GS.isModeThrough() && x0 < rttCopyX1_ && x1 > rttCopyX0_ && y0 < rttCopyY1_ && y1 > rttCopyY0_) {
				// A 2D pass drew over what the copy holds: the next pass must see that (Silent
				// Hill Origins builds its glow in steps in one buffer, each reading the last; with
				// the copy reused every step read the full-bright scene and the screen went
				// white). Fresh copies for up to 8 such draws a frame; past that the copy is
				// reused, as for Killzone's ~150 screen-reading draws (Frontend 101).
				if (freshCopyFrame_ != frame_) {
					freshCopyFrame_ = frame_;
					freshCopies_ = 0;
				}
				if (freshCopies_ < 8) {
					freshCopies_++;
					rttCopySrc_ = nullptr;
				}
#ifdef PPSSPP_RSX_HOSTSIM
				else if (getenv("PPSSPP_FRESHLOG")) {
					fprintf(stderr, "FRESH limit: copy of %08x reused (frame %u)\n", target_->addr, frame_);
				}
#endif
			}
		}
	}
	int rttCopyX0_ = 0, rttCopyY0_ = 0, rttCopyX1_ = 0, rttCopyY1_ = 0;
	uint32_t freshCopyFrame_ = ~0u;
	int freshCopies_ = 0;
	unsigned rttHits_ = 0, slowDecodes_ = 0, spuDecodes_ = 0, batchedDraws_ = 0;
	bool LookupRenderTarget(uint32_t addr, GETextureFormat fmt, int w, int h, int bufw, bool swizzle, uint32_t texFlushGen, TexEntry **out);
	bool LookupDepal(const SamplerID &id, uint32_t addr, GETextureFormat fmt, int w, int h, int bufw, TexEntry **out);
	bool FastDecode(const SamplerID &id, const u8 *texptr, int bufw, int w, int h, uint32_t bytes, uint32_t *dst, uint32_t dstPitch);
	// Stats since the last Stats() call.
	unsigned cpuRowsMerged_ = 0, cpuMergeLogs_ = 0, cpuRectLogs_ = 0;
	unsigned rttOffsetHits_ = 0, overlapLogs_ = 0, wideSwizzleLogs_ = 0, dstAlphaLogs_ = 0, newFramebuffers_ = 0;
	unsigned uploads_ = 0, downloads_ = 0, texDecodes_ = 0, draws_ = 0, prims_ = 0;
	uint64_t tbBackend_ = 0, tbDecode_ = 0, tbDownload_ = 0, tbUpload_ = 0, tbHash_ = 0, tbLookup_ = 0;
	// Parts of a texture lookup (Frontend 137): framebuffer checks, palette hash and key, the table.
	uint64_t tbLkFb_ = 0, tbLkKey_ = 0, tbLkMap_ = 0;
	unsigned lookups_ = 0;
	// Write-backs by cause (for the perf line): 0 other, 1 more rows, 2 new layout at the address,
	// 3 a read of PSP memory (block transfer, CLUT, output), 4 a texture over it, 5 depth.
	enum { DL_OTHER, DL_ROWS, DL_LAYOUT, DL_READ, DL_TEXTURE, DL_DEPTH, DL_CAUSES };
	int dlCause_ = DL_OTHER;
	unsigned dlCount_[DL_CAUSES]{};
	uint64_t dlTb_[DL_CAUSES]{};
	unsigned layoutLogs_ = 0;
	unsigned keptLayouts_ = 0;  // Layout switches with keepLayouts (perf line).
	unsigned keptRowSkips_ = 0;  // Uploads skipped for a kept surface with a few CPU-changed rows.
	// Hardware T&L draws, by step: state setup, vertex constants, vertex/index copies, submitting.
	uint64_t tbTLState_ = 0, tbTLConst_ = 0, tbTLCopy_ = 0, tbTLSubmit_ = 0;
	unsigned tlConstUploads_ = 0;
};

// ---------------------------------------------------------------------------------------------

bool RSXRenderer::Init() {
	RSX::Step("renderer: init");
	scale_ = std::max(1, std::min(opt_.scale, 3));
	if (!RSX::Ready()) {
		PS3Frontend::Trace("RSX renderer: no RSX context");
		return false;
	}
	if (!RSX::LoadPrograms() || !RSX::InitDisplay()) return false;
	RSX::Step("renderer: calibration");
	cal_ = RSXCalibrate();
	RSX::StepQuiet("idle");
	PS3Frontend::Trace("RSX renderer calibration %s", cal_.summary.c_str());
	if (!cal_.ok) {
		// Keep the defaults the self-test expects; the log line says what didn't check out.
		RSXCalibration defaults;
		defaults.summary = cal_.summary;
		cal_ = defaults;
	}
	identityTex_ = cal_.texOrder[0] == 0 && cal_.texOrder[1] == 1 && cal_.texOrder[2] == 2 && cal_.texOrder[3] == 3;
	if (opt_.batched && opt_.hardwareTL) {
		const bool rgbaColors = cal_.colorOrder[0] == 0 && cal_.colorOrder[1] == 1 && cal_.colorOrder[2] == 2 && cal_.colorOrder[3] == 3;
		const RSXTLTestResult tl = RSXTestTL();
		tlOk_ = tl.ok && rgbaColors && !cal_.flipY;
		tlPositiveIsCCW_ = tl.positiveCrossIsCCW;
		PS3Frontend::Trace("RSX renderer hardware T&L: %s%s", tl.summary.c_str(), tlOk_ ? "" : " - using the CPU transform");
	}
	RSX::IoMark();
	staging_ = (uint32_t *)RSX::IoAlloc(1024 * 512 * 4, 1024);
	if (opt_.lateDepth) {
		depthStaging_ = (uint32_t *)RSX::IoAlloc(1024 * 512 * 4, 1024);
		if (!depthStaging_)
			PS3Frontend::Trace("RSX renderer: no I/O memory for late depth reads - exact reads instead");
	}
	overlayBuf_ = (uint32_t *)RSX::IoAlloc(1024 * 24 * 4, 1024);
	smallStaging_ = (uint32_t *)RSX::IoAlloc(SMALL_WB_W * SMALL_WB_H * 4, 1024);
	if (!staging_ || !overlayBuf_) {
		PS3Frontend::Trace("RSX renderer: no main-memory staging (I/O region not mapped?)");
		return false;
	}
	batch_.reserve(MAX_BATCH + 8);
	ComputeGameRect();
	RSX::Finish();
	PS3Frontend::Trace("RSX renderer ready: %dx internal resolution, VRAM sync %s, output %dx%d at %d,%d",
		scale_, opt_.syncEveryFrame ? "every frame" : "minimal", gameW_, gameH_, gameX_, gameY_);
	return true;
}

void RSXRenderer::Shutdown() {
	FlushBatch();
	if (opt_.syncEveryFrame) DownloadAll();
	RSX::ResetDisplay();
	ProcessDeferredFrees(true);
	for (auto &s : surfaces_) { RSX::UnbindTileRegion(s->tile); RSX::LocalFree(s->mem); }
	for (auto &d : depths_) { RSX::UnbindTileRegion(d->tile); RSX::LocalFree(d->mem); }
	for (auto &t : textures_) RSX::LocalFree(t.second.mem);
	surfaces_.clear();
	rttCopySrc_ = nullptr;
	depths_.clear();
	textures_.clear();
	RSX::IoRelease();
}

void RSXRenderer::ComputeGameRect() {
	const int w = RSX::DisplayWidth(), h = RSX::DisplayHeight();
	int rw = w, rh = h;
	switch ((PS3Frontend::ScreenMode)opt_.screenMode) {
	case PS3Frontend::ScreenMode::STRETCH:
		break;
	case PS3Frontend::ScreenMode::PIXEL_2X:
		rw = std::min(w, PSP_W * 2); rh = std::min(h, PSP_H * 2);
		break;
	default:
		rh = h; rw = (h * PSP_W + PSP_H / 2) / PSP_H;
		if (rw > w) { rw = w; rh = (w * PSP_H + PSP_W / 2) / PSP_W; }
		if (opt_.screenMode == (int)PS3Frontend::ScreenMode::FIT_90) { rw = rw * 9 / 10; rh = rh * 9 / 10; }
		break;
	}
	gameX_ = (w - rw) / 2; gameY_ = (h - rh) / 2; gameW_ = rw; gameH_ = rh;
}

void *RSXRenderer::AllocLocal(uint32_t size) {
	void *mem = RSX::LocalAlloc(size, 128);
	if (!mem) {
		// Free what we can: old textures, then everything already retired.
		EvictTextures(0);
		ProcessDeferredFrees(true);
		mem = RSX::LocalAlloc(size, 128);
		if (!mem) PS3Frontend::Trace("RSX renderer: out of RSX memory (%u bytes)", size);
	}
	return mem;
}

void RSXRenderer::DeferFree(void *mem, int tile) {
	if (mem) deferred_.push_back({RSX::Fence(), mem, tile});
}

void RSXRenderer::ProcessDeferredFrees(bool force) {
	if (force) RSX::Finish();
	size_t kept = 0;
	for (auto &item : deferred_) {
		if (force || RSX::FenceDone(item.fence)) {
			// The region goes before the memory can be handed out again.
			RSX::UnbindTileRegion(item.tile);
			RSX::LocalFree(item.mem);
		} else {
			deferred_[kept++] = item;
		}
	}
	deferred_.resize(kept);
}

// The RSX draws faster into tiled memory (its own layout, bank-interleaved), and nothing else
// notices: texture reads, transfers and the CPU see the same linear bytes.
void *RSXRenderer::AllocTarget(uint32_t size, uint32_t pitch, int bank, int *tile) {
	*tile = -1;
	if (!opt_.tiled || !RSX::TilePitchSupported(pitch))
		return AllocLocal(size);
	const uint32_t rounded = (size + 0xFFFF) & ~0xFFFFu;
	void *mem = RSX::LocalAlloc(rounded, 0x10000);
	if (!mem) {
		EvictTextures(0);
		ProcessDeferredFrees(true);
		mem = RSX::LocalAlloc(rounded, 0x10000);
	}
	if (!mem)
		return AllocLocal(size);
	*tile = RSX::BindTileRegion(RSX::LocalOffset(mem), rounded, pitch, bank);
	if (tileLogs_ < 8) {
		tileLogs_++;
		RSXEventLog("RSX renderer: render target %08x (%u bytes, pitch %u) %s", RSX::LocalOffset(mem), rounded, pitch,
			*tile >= 0 ? "in a tiled region" : "untiled (no region free)");
	}
	return mem;
}

void RSXRenderer::EvictTextures(uint32_t frameAge) {
	for (auto it = textures_.begin(); it != textures_.end();) {
		if (frame_ - it->second.lastFrame > frameAge) {
			DeferFree(it->second.mem);
			textureBytes_ -= it->second.bytes;
			it = textures_.erase(it);
		} else {
			++it;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Framebuffers

// The bytes of row y the row hashes cover: the columns the RSX draws, once it has (whole rows before).
static void RowHashSpan(const Surface &s, uint32_t *start, uint32_t *len) {
	const uint32_t bpp = BytesPerPixel(s.fmt);
	const uint32_t rowBytes = s.stride * bpp;
	if (s.hashX1 > s.hashX0) {
		*start = (uint32_t)s.hashX0 * bpp;
		*len = std::min(rowBytes, (uint32_t)s.hashX1 * bpp) - *start;
	} else {
		*start = 0;
		*len = rowBytes;
	}
}

static void ComputeRowHashes(Surface &s) {
	const uint32_t rowBytes = s.stride * BytesPerPixel(s.fmt);
	s.rowHash.assign(s.rows, 0);
	if (s.ownX1 > s.ownX0 && s.ownX0 < (int)s.stride) {
		s.hashX0 = s.ownX0;
		s.hashX1 = std::min(s.ownX1, (int)s.stride);
	} else {
		s.hashX0 = s.hashX1 = 0;
	}
	if (!Memory::IsValidRange(s.addr, rowBytes * s.rows)) return;
	const uint8_t *p = Memory::GetPointerUnchecked(s.addr);
	uint32_t start, len;
	RowHashSpan(s, &start, &len);
	for (int y = 0; y < s.rows; ++y)
		s.rowHash[y] = (uint32_t)XXH3_64bits(p + y * rowBytes + start, len);
}

uint64_t RSXRenderer::HashVram(const Surface &s) const {
	const uint32_t bytes = s.stride * BytesPerPixel(s.fmt) * s.rows;
	if (!Memory::IsValidRange(s.addr, bytes)) return 0;
	return XXH3_64bits(Memory::GetPointerUnchecked(s.addr), bytes);
}

Surface *RSXRenderer::FindSurface(uint32_t addr, uint16_t stride, GEBufferFormat fmt, int rows, bool create, bool forDrawing) {
	if (opt_.keepLayouts && forDrawing) {
		// Drawing in this layout now: other layouts at the address keep their copies on the RSX,
		// no longer to be written back (only the newest drawing may reach VRAM).
		for (auto &ptr : surfaces_) {
			Surface &o = *ptr;
			if (o.addr != addr || (o.stride == stride && o.fmt == fmt) || !o.gpuDirty) continue;
			if (target_ == &o) {
				FlushBatch();
				target_ = nullptr;
				targetValid_ = false;
			}
			if (layoutLogs_ < 16) {
				layoutLogs_++;
				RSXEventLog("RSX renderer: %08x drawn as stride %d format %d, now stride %d format %d - kept on the RSX, not written back",
					addr, o.stride, (int)o.fmt, stride, (int)fmt);
			}
			o.keptOnRsx = true;
			o.gpuDirty = false;
			keptLayouts_++;
		}
	}
	for (size_t i = 0; i < surfaces_.size(); ++i) {
		Surface &s = *surfaces_[i];
		if (s.addr != addr) continue;
		if (s.stride == stride && s.fmt == fmt) {
			if (rows > s.rows) {
				// The new rows were never uploaded: write back what was drawn first, so the
				// re-upload (synced = false) brings in VRAM for all of them.
				if (s.gpuDirty) {
					dlCause_ = DL_ROWS;
					Download(s);
				}
				s.rows = std::min(rows, (int)(s.h / scale_));
				s.synced = false;
				s.rowHash.clear();
			}
			s.lastFrame = frame_;
			return &s;
		}
		// Same address, different layout: write back what we have, then replace it. With
		// keepLayouts, keep it (up to 4 layouts at an address) without a write-back.
		if (opt_.keepLayouts) {
			int atAddr = 0;
			for (const auto &o : surfaces_)
				if (o->addr == addr) atAddr++;
			if (atAddr < 4)
				continue;
		}
		if (s.gpuDirty) {
			if (layoutLogs_ < 16) {
				layoutLogs_++;
				RSXEventLog("RSX renderer: %08x drawn as stride %d format %d (%d rows), now stride %d format %d (%d rows) - written back and replaced",
					addr, s.stride, (int)s.fmt, s.rows, stride, (int)fmt, rows);
			}
			dlCause_ = DL_LAYOUT;
			Download(s);
		}
		FlushBatch();
		if (target_ == &s) {
			target_ = nullptr;
			targetValid_ = false;
		}
		if (rttCopySrc_ == &s) rttCopySrc_ = nullptr;
		DeferFree(s.mem, s.tile);
		surfaces_[i] = std::move(surfaces_.back());
		surfaces_.pop_back();
		// Others at this address may still match (kept layouts, also after keepLayouts was turned
		// off in a game).
		--i;
	}
	if (!create) return nullptr;
	std::unique_ptr<Surface> owned(new Surface());
	Surface &s = *owned;
	s.addr = addr;
	s.stride = stride;
	s.fmt = fmt;
	const int width = std::max<int>(1, std::min<int>(stride, 1024));
	s.w = (uint16_t)(width * scale_);
	s.h = (uint16_t)(512 * scale_);
	s.pitch = (s.w * 4 + 63) & ~63;
	s.rows = std::max(1, std::min(rows, 512));
	// In use now: Present frees framebuffers unused for 1200 frames by this. Left at 0, a layout
	// that was never the target at the end of a frame or shown was freed at every Present once the
	// game had run 1200 frames - and made again, loaded from VRAM, the next frame (Silent Hill
	// Origins' 16-bit light buffers, 2x: two new framebuffers and uploads every frame).
	s.lastFrame = frame_;
	s.mem = AllocTarget(s.pitch * s.h, s.pitch, 0, &s.tile);
	if (!s.mem) return nullptr;
	s.offset = RSX::LocalOffset(s.mem);
	surfaces_.push_back(std::move(owned));
	// Only the first few, then now and then with the count: GTA Chinatown Wars switches one
	// buffer between two widths twice a frame, and a line to launch.log takes ~40 ms on the PS3's
	// disk - a new framebuffer each time cost ~80 ms a frame on the GE worker (Frontend 104).
	newFramebuffers_++;
	if (newFramebuffers_ <= 24 || (newFramebuffers_ & 1023) == 0)
		RSXEventLog("RSX renderer: new framebuffer %08x stride %d format %d (%dx%d)%s", addr, stride, (int)fmt, s.w, s.h,
			newFramebuffers_ == 24 ? " - more are counted, not listed" : (newFramebuffers_ > 24 ? " (one of many)" : ""));
	return surfaces_.back().get();
}

DepthSurface *RSXRenderer::FindDepth(uint32_t addr, uint16_t minW, uint16_t minH) {
	for (size_t i = 0; i < depths_.size(); ++i) {
		DepthSurface &d = *depths_[i];
		if (d.addr != addr) continue;
		if (d.w >= minW && d.h >= minH) return &d;
		if (depth_ == &d) depth_ = nullptr;
		DeferFree(d.mem, d.tile);
		depths_[i] = std::move(depths_.back());
		depths_.pop_back();
		break;
	}
	std::unique_ptr<DepthSurface> owned(new DepthSurface());
	DepthSurface &d = *owned;
	d.addr = addr;
	d.w = std::max<uint16_t>(minW, 512 * scale_);
	d.h = std::max<uint16_t>(minH, 512 * scale_);
	d.pitch = (d.w * 4 + 63) & ~63;
	if (opt_.tiledDepth) {
		d.mem = AllocTarget(d.pitch * d.h, d.pitch, 1, &d.tile);
	} else {
		// "RSX tiling: Screens only": depth buffers in plain memory (Killzone's ground, drawn
		// twice where the second pass needs the same depth, flickered less with tiling off).
		d.tile = -1;
		d.mem = AllocLocal(d.pitch * d.h);
	}
	if (!d.mem) return nullptr;
	d.offset = RSX::LocalOffset(d.mem);
	d.fresh = true;
	depths_.push_back(std::move(owned));
	return depths_.back().get();
}

// PSP rows y0..y1 of a framebuffer into the staging buffer (1024 ARGB words a row).
static void VramRowsToStaging(const Surface &s, int y0, int y1, uint32_t *staging) {
	const int bpp = BytesPerPixel(s.fmt);
	const int width = std::min<int>(s.stride, 1024);
	const uint8_t *src = Memory::GetPointerUnchecked(s.addr);
	for (int y = y0; y < y1; ++y) {
		const uint8_t *row = src + y * s.stride * bpp;
		uint32_t *out = staging + y * 1024;
		switch (s.fmt) {
		case GE_FORMAT_565:
			for (int x = 0; x < width; ++x) {
				uint32_t v = row[x * 2] | (row[x * 2 + 1] << 8);
				uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
				out[x] = 0xFF000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
			}
			break;
		case GE_FORMAT_5551:
			for (int x = 0; x < width; ++x) {
				uint32_t v = row[x * 2] | (row[x * 2 + 1] << 8);
				uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
				out[x] = ((v & 0x8000) ? 0xFF000000 : 0) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
			}
			break;
		case GE_FORMAT_4444:
			for (int x = 0; x < width; ++x) {
				uint32_t v = row[x * 2] | (row[x * 2 + 1] << 8);
				out[x] = (((v >> 12) & 15) * 17 << 24) | ((v & 15) * 17 << 16) | (((v >> 4) & 15) * 17 << 8) | ((v >> 8) & 15) * 17;
			}
			break;
		default:
			for (int x = 0; x < width; ++x)
				out[x] = (row[x * 4 + 3] << 24) | (row[x * 4] << 16) | (row[x * 4 + 1] << 8) | row[x * 4 + 2];
			break;
		}
	}
}

void RSXRenderer::Upload(Surface &s) {
	TbScope tb(tbUpload_);
	s.version++;
	RSX::StepQuiet("renderer: upload VRAM to RSX");
	const int bpp = BytesPerPixel(s.fmt);
	const int width = std::min<int>(s.stride, 1024);
	if (!Memory::IsValidRange(s.addr, s.stride * bpp * s.rows)) return;
	FlushBatch();
	// The RSX may still be reading the staging buffer for an earlier transfer.
	RSX::Finish();
	VramRowsToStaging(s, 0, s.rows, staging_);
	__sync_synchronize();
	RSX::ScaleImage(RSX::Mem::Main, RSX::IoOffset(staging_), 1024 * 4, width, s.rows,
		RSX::Mem::Local, s.offset, s.pitch, 0, 0, width * scale_, s.rows * scale_, false);
	RSX::GpuWaitForRendering();
	s.gpuDirty = false;
	uploads_++;
}

// "CPU writes to drawn buffers": a buffer the RSX has drawn into and that the CPU has written
// since (rows whose VRAM no longer matches the last sync) gets those rows from VRAM, so draws
// that read it and the screen show them. Otherwise only the next write-back brought them in
// (Silent Hill Origins writes its loading screen into the buffer it composes the screen from;
// with VRAM sync minimal only the top part showed). Once a frame per buffer.
void RSXRenderer::MergeCpuRows(Surface &s) {
	if (!opt_.cpuRows || !s.gpuDirty || s.cpuCheckFrame == frame_ || (int)s.rowHash.size() != s.rows)
		return;
	s.cpuCheckFrame = frame_;
	const uint32_t bpp = BytesPerPixel(s.fmt);
	const uint32_t rowBytes = s.stride * bpp;
	if (!Memory::IsValidRange(s.addr, rowBytes * s.rows))
		return;
	const uint8_t *p = Memory::GetPointerUnchecked(s.addr);
	uint32_t start, len;
	RowHashSpan(s, &start, &len);
	int y = 0;
	bool waited = false;
	uint32_t merged = 0;
	const int width = std::min<int>(s.stride, 1024);
	while (y < s.rows) {
		if ((uint32_t)XXH3_64bits(p + y * rowBytes + start, len) == s.rowHash[y]) {
			y++;
			continue;
		}
		int y1 = y + 1;
		while (y1 < s.rows && (uint32_t)XXH3_64bits(p + y1 * rowBytes + start, len) != s.rowHash[y1])
			y1++;
		if (!waited) {
			FlushBatch();
			// The RSX may still be reading the staging buffer for an earlier transfer.
			RSX::Finish();
			waited = true;
		}
		VramRowsToStaging(s, y, y1, staging_);
		__sync_synchronize();
		RSX::ScaleImage(RSX::Mem::Main, RSX::IoOffset(staging_ + y * 1024), 1024 * 4, width, y1 - y,
			RSX::Mem::Local, s.offset + (uint32_t)(y * scale_) * s.pitch, s.pitch, 0, 0, width * scale_, (y1 - y) * scale_, false);
		for (int r = y; r < y1; ++r)
			s.rowHash[r] = (uint32_t)XXH3_64bits(p + r * rowBytes + start, len);
		merged += y1 - y;
		y = y1;
	}
	if (!merged)
		return;
	RSX::GpuWaitForRendering();
	s.version++;
	cpuRowsMerged_ += merged;
	if (cpuMergeLogs_ < 8) {
		cpuMergeLogs_++;
		RSXEventLog("RSX renderer: %u rows of %08x the CPU wrote after drawing were taken from VRAM", merged, s.addr);
	}
}

// A block transfer from memory wrote this rectangle of VRAM (stride and w in bytes) after the draws
// queued before it: buffers drawn on the RSX since their last write-back take it from VRAM into
// their copy, so they need no write-back for it (HardwareBackend::CanTakeCpuRows). The others
// are checked against VRAM when they're next used (Validate).
void RSXRenderer::TakeCpuRows(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) {
#ifdef PPSSPP_RSX_HOSTSIM
	if (getenv("PPSSPP_DOWNLOADLOG"))
		fprintf(stderr, "TAKEROWS %08x stride %u w %u h %u\n", start, stride, w, h);
#endif
	start = NormalizeAddr(start);
	const uint32_t end = start + stride * (h ? h - 1 : 0) + w;
	FlushBatch();
	bool waited = false;
	for (auto &ptr : surfaces_) {
		Surface &s = *ptr;
		if (!s.gpuDirty)
			continue;
		const uint32_t bpp = BytesPerPixel(s.fmt);
		const uint32_t rowBytes = s.stride * bpp;
		const uint32_t sEnd = s.addr + rowBytes * s.rows;
		if (start >= sEnd || end <= s.addr || !Memory::IsValidRange(s.addr, rowBytes * s.rows))
			continue;
		const int width = std::min<int>(s.stride, 1024);
		int y0, y1, x0 = 0, x1 = width;
		if (rowBytes == stride && start >= s.addr && (start - s.addr) % rowBytes + w <= rowBytes) {
			const uint32_t off = start - s.addr;
			y0 = (int)(off / rowBytes);
			y1 = std::min<int>(s.rows, y0 + (int)h);
			// The transfer source starts on a 64-byte boundary of the staging buffer.
			x0 = (int)((off % rowBytes) / bpp) & ~15;
			x1 = std::min<int>(width, (int)((off % rowBytes + w + bpp - 1) / bpp));
		} else {
			// Another layout over the same memory: whole rows.
			y0 = start > s.addr ? (int)((start - s.addr) / rowBytes) : 0;
			y1 = std::min<int>(s.rows, (int)((end - s.addr + rowBytes - 1) / rowBytes));
		}
		if (y1 <= y0 || x1 <= x0)
			continue;
		// The RSX may still be reading the staging buffer for an earlier transfer, also for the
		// buffer before this one in this loop: two buffers over the same memory (Silent Hill Origins'
		// menu screen and a 16-bit buffer from the game that starts 88 rows above it) had the second
		// one's rows overwrite the first one's before the RSX had copied them.
		RSX::Finish();
		waited = true;
		VramRowsToStaging(s, y0, y1, staging_);
		__sync_synchronize();
		RSX::ScaleImage(RSX::Mem::Main, RSX::IoOffset(staging_ + y0 * 1024 + x0), 1024 * 4, x1 - x0, y1 - y0,
			RSX::Mem::Local, s.offset + (uint32_t)(y0 * scale_) * s.pitch, s.pitch, x0 * scale_, 0, (x1 - x0) * scale_, (y1 - y0) * scale_, false);
		// What the RSX has in these rows matches VRAM again (a later write-back isn't to keep them).
		if ((int)s.rowHash.size() == s.rows) {
			const uint8_t *p = Memory::GetPointerUnchecked(s.addr);
			uint32_t hs, hl;
			RowHashSpan(s, &hs, &hl);
			for (int r = y0; r < y1; ++r)
				s.rowHash[r] = (uint32_t)XXH3_64bits(p + r * rowBytes + hs, hl);
		}
		s.version++;
		uploads_++;
		if (cpuRectLogs_ < 4) {
			cpuRectLogs_++;
			RSXEventLog("RSX renderer: a block transfer's rows %d-%d (columns %d-%d) of %08x taken from VRAM, no write-back", y0, y1, x0, x1, s.addr);
		}
	}
	if (waited)
		RSX::GpuWaitForRendering();
}

// Writes back part of a buffer: rows y0-y1, columns x0-x1 (PSP pixels; only what the RSX drew there).
// For a block transfer out of a corner of a buffer (Silent Hill Origins copies its light mask out of
// VRAM every frame): the whole buffer each time, and the uploads that one buffer's write-back
// caused in others over the same memory, cost ~30 ms a frame at 2x. The buffer stays marked drawn
// (the rest is still only on the RSX); the rows' hashes are taken again, so a later write-back
// doesn't take them for rows the CPU changed. False: not done (the caller writes back all of it).
static void StagingRowToVram(GEBufferFormat fmt, const uint32_t *in, uint8_t *row, int xs, int xe);
bool RSXRenderer::DownloadPart(Surface &s, int y0, int y1, int x0, int x1) {
	const int bpp = BytesPerPixel(s.fmt);
	const int width = std::min<int>(s.stride, 1024);
	if ((int)s.rowHash.size() != s.rows || opt_.writeBackAllRows || !Memory::IsValidRange(s.addr, s.stride * bpp * s.rows)) {
#ifdef PPSSPP_RSX_HOSTSIM
		if (getenv("PPSSPP_DOWNLOADLOG"))
			fprintf(stderr, "DOWNLOADPART %08x: no row hashes (%d of %d rows)\n", s.addr, (int)s.rowHash.size(), s.rows);
#endif
		return false;
	}
	y0 = std::max(y0, std::max(s.drawnY0, 0));
	y1 = std::min(y1, std::min(s.drawnY1, s.rows));
	x0 = std::max(x0, std::max(s.ownX0, 0));
	x1 = std::min(x1, std::min(s.ownX1, width));
#ifdef PPSSPP_RSX_HOSTSIM
	if (getenv("PPSSPP_DOWNLOADLOG"))
		fprintf(stderr, "DOWNLOADPART %08x stride %d fmt %d rows %d-%d columns %d-%d\n", s.addr, s.stride, (int)s.fmt, y0, y1, x0, x1);
#endif
	if (y1 <= y0 || x1 <= x0)
		return true;  // Not drawn there: VRAM has those pixels.
	uint8_t *dst = Memory::GetPointerWriteUnchecked(s.addr);
	uint32_t hashStart, hashLen;
	RowHashSpan(s, &hashStart, &hashLen);
	for (int y = y0; y < y1; ++y) {
		// The CPU changed this row since the last sync: a full write-back sorts that out.
		if ((uint32_t)XXH3_64bits(dst + y * s.stride * bpp + hashStart, hashLen) != s.rowHash[y]) {
#ifdef PPSSPP_RSX_HOSTSIM
			if (getenv("PPSSPP_DOWNLOADLOG"))
				fprintf(stderr, "DOWNLOADPART %08x: row %d changed in VRAM\n", s.addr, y);
#endif
			return false;
		}
	}
	TbScope tb(tbDownload_);
	TbScope tbCause(dlTb_[DL_READ]);
	dlCount_[DL_READ]++;
	RSX::StepQuiet("renderer: write back part of a buffer");
	FlushBatch();
	RSX::GpuWaitForRendering();
	RSX::ScaleImage(RSX::Mem::Local, s.offset + (uint32_t)(y0 * scale_) * s.pitch, s.pitch, width * scale_, (y1 - y0) * scale_,
		RSX::Mem::Main, RSX::IoOffset(staging_), 1024 * 4, 0, 0, width, y1 - y0, false);
	if (!RSX::Finish()) {
		PS3Frontend::Trace("RSX renderer: part write-back of %08x timed out", s.addr);
		return false;
	}
	for (int y = y0; y < y1; ++y) {
		uint8_t *row = dst + y * s.stride * bpp;
		const uint32_t rowAddr = s.addr + y * s.stride * bpp;
		listGuard_.Begin(rowAddr, row, x0 * bpp, x1 * bpp);
		StagingRowToVram(s.fmt, staging_ + (y - y0) * 1024, row, x0, x1);
		listGuard_.End(rowAddr, row, "framebuffer", s.addr, y);
		s.rowHash[y] = (uint32_t)XXH3_64bits(row + hashStart, hashLen);
	}
	vramGen_++;
	partDownloads_++;
	KeepLayoutsOverWrite(s, s.addr + (uint32_t)y0 * s.stride * bpp, s.addr + (uint32_t)y1 * s.stride * bpp);
	return true;
}

// One row of the staging buffer (ARGB words) into PSP memory in the framebuffer's format.
static void StagingRowToVram(GEBufferFormat fmt, const uint32_t *in, uint8_t *row, int xs, int xe) {
	switch (fmt) {
	case GE_FORMAT_565:
		for (int x = xs; x < xe; ++x) {
			uint32_t c = in[x];
			uint32_t v = ((c >> 19) & 31) | (((c >> 10) & 63) << 5) | (((c >> 3) & 31) << 11);
			row[x * 2] = (uint8_t)v; row[x * 2 + 1] = (uint8_t)(v >> 8);
		}
		break;
	case GE_FORMAT_5551:
		for (int x = xs; x < xe; ++x) {
			uint32_t c = in[x];
			uint32_t v = ((c >> 19) & 31) | (((c >> 11) & 31) << 5) | (((c >> 3) & 31) << 10) | ((c >> 31) << 15);
			row[x * 2] = (uint8_t)v; row[x * 2 + 1] = (uint8_t)(v >> 8);
		}
		break;
	case GE_FORMAT_4444:
		for (int x = xs; x < xe; ++x) {
			uint32_t c = in[x];
			uint32_t v = ((c >> 20) & 15) | (((c >> 12) & 15) << 4) | (((c >> 4) & 15) << 8) | ((c >> 28) << 12);
			row[x * 2] = (uint8_t)v; row[x * 2 + 1] = (uint8_t)(v >> 8);
		}
		break;
	default:
		for (int x = xs; x < xe; ++x) {
			uint32_t c = in[x];
			row[x * 4] = (uint8_t)(c >> 16); row[x * 4 + 1] = (uint8_t)(c >> 8);
			row[x * 4 + 2] = (uint8_t)c; row[x * 4 + 3] = (uint8_t)(c >> 24);
		}
		break;
	}
}

void RSXRenderer::Download(Surface &s) {
	TbScope tb(tbDownload_);
	const int cause = dlCause_;
	dlCause_ = DL_OTHER;
	TbScope tbCause(dlTb_[cause]);
	dlCount_[cause]++;
	RSX::StepQuiet("renderer: write back RSX to VRAM");
	const int bpp = BytesPerPixel(s.fmt);
	const int width = std::min<int>(s.stride, 1024);
	if (!Memory::IsValidRange(s.addr, s.stride * bpp * s.rows)) {
		s.gpuDirty = false;
		return;
	}
	FlushBatch();
	RSX::GpuWaitForRendering();
	RSX::ScaleImage(RSX::Mem::Local, s.offset, s.pitch, width * scale_, s.rows * scale_,
		RSX::Mem::Main, RSX::IoOffset(staging_), 1024 * 4, 0, 0, width, s.rows, false);
	if (!RSX::Finish()) {
		PS3Frontend::Trace("RSX renderer: write-back of %08x timed out", s.addr);
		return;
	}
	uint8_t *dst = Memory::GetPointerWriteUnchecked(s.addr);
	const bool haveRowHashes = (int)s.rowHash.size() == s.rows;
	uint32_t hashStart, hashLen;
	RowHashSpan(s, &hashStart, &hashLen);
	// Only the columns the RSX drew go back (all of them with the write-back-all-rows option).
	int xs = 0, xe = width;
	if (!opt_.writeBackAllRows && s.ownX1 > s.ownX0) {
		xs = std::min(s.ownX0, width);
		xe = std::min(s.ownX1, width);
	}
	uint32_t kept = 0;
	for (int y = 0; y < s.rows; ++y) {
		const uint32_t *in = staging_ + y * 1024;
		uint8_t *row = dst + y * s.stride * bpp;
		const bool drawn = y >= s.drawnY0 && y < s.drawnY1;
		if (haveRowHashes && (uint32_t)XXH3_64bits(row + hashStart, hashLen) != s.rowHash[y]) {
			// Changed in VRAM since we synced: the CPU's data wins over our (stale) copy.
			kept++;
			continue;
		}
		if (!drawn && !opt_.writeBackAllRows)
			continue;  // Never drawn by the RSX: VRAM already has these pixels.
		const uint32_t rowAddr = s.addr + y * s.stride * bpp;
		listGuard_.Begin(rowAddr, row, xs * bpp, xe * bpp);
		StagingRowToVram(s.fmt, in, row, xs, xe);
		listGuard_.End(rowAddr, row, "framebuffer", s.addr, y);
	}
#ifdef PPSSPP_RSX_HOSTSIM
	if (getenv("PPSSPP_DOWNLOADLOG"))
		fprintf(stderr, "DOWNLOAD %08x stride %d fmt %d rows %d drawn %d-%d columns %d-%d (bytes %08x-%08x) kept %u\n", s.addr, s.stride, (int)s.fmt, s.rows,
			s.drawnY0, s.drawnY1, xs, xe, s.addr + s.drawnY0 * s.stride * bpp, s.addr + s.drawnY1 * s.stride * bpp, kept);
#endif
	s.gpuDirty = false;
	s.dlSerial++;
	s.drawnY0 = 0x7FFFFFFF;
	s.drawnY1 = 0;
	s.drawnX0 = 0x7FFFFFFF;
	s.drawnX1 = 0;
	if (kept) {
		// Our copy is stale in those rows: upload VRAM again now. Waiting for the next Validate
		// left a gap: drawing on to the same buffer in the same frame skips Validate, and the next
		// write-back then had no row hashes to protect the CPU's rows with (textures the game put
		// right after a render target turned to garbage).
		Upload(s);
		s.syncHash = HashVram(s);
		s.synced = true;
		ComputeRowHashes(s);
		if (s.keptRows < 8) {
			RSXEventLog("RSX renderer: write-back of %08x kept %u rows the CPU had changed", s.addr, kept);
		}
		s.keptRows++;
	} else {
		s.syncHash = HashVram(s);
		s.synced = true;
		ComputeRowHashes(s);
	}
	s.keptOnRsx = false;
	vramGen_++;
	downloads_++;
	KeepLayoutsOverWrite(s, s.addr, s.addr + (uint32_t)s.stride * bpp * s.rows);
}

// A write-back of s changed PSP memory start-end: layouts kept on the RSX over that memory (Keep
// buffer layouts) take it as their own state, not as the CPU's (which would make them load VRAM
// again: Silent Hill Origins' light buffer, 2x, every frame, for a corner its light-mask copy wrote
// back - most of the GE worker's time in the flashlight area). Checked with its GE dumps: what
// the copy leaves in that corner never reaches the picture.
void RSXRenderer::KeepLayoutsOverWrite(const Surface &s, uint32_t start, uint32_t end) {
	for (auto &ptr : surfaces_) {
		Surface &o = *ptr;
		if (&o == &s || !o.keptOnRsx || !o.synced || (int)o.rowHash.size() != o.rows)
			continue;
		const uint32_t rowBytes = o.stride * BytesPerPixel(o.fmt);
		const uint32_t oEnd = o.addr + rowBytes * o.rows;
		if (start >= oEnd || end <= o.addr || !Memory::IsValidRange(o.addr, rowBytes * o.rows))
			continue;
		const uint8_t *p = Memory::GetPointerUnchecked(o.addr);
		uint32_t hs, hl;
		RowHashSpan(o, &hs, &hl);
		const int y0 = start > o.addr ? (int)((start - o.addr) / rowBytes) : 0;
		const int y1 = std::min(o.rows, (int)((std::min(end, oEnd) - o.addr + rowBytes - 1) / rowBytes));
		for (int y = y0; y < y1; ++y)
			o.rowHash[y] = (uint32_t)XXH3_64bits(p + y * rowBytes + hs, hl);
		o.syncHash = HashVram(o);
	}
}

// The depth buffer read as a texture (Midnight Club LA reads it through a VRAM mirror as CLUT16 for
// an effect): put the RSX's depth into PSP memory as the GE's 16-bit depth, like the software
// renderer has it. Depth is otherwise never written back.
void RSXRenderer::WriteDepthRows(uint32_t addr, int stride, int width, int y0, int y1, const uint32_t *src) {
	if (!Memory::IsValidRange(addr, (uint32_t)stride * 2 * y1))
		return;
	uint8_t *dst = Memory::GetPointerWriteUnchecked(addr);
	for (int y = y0; y < y1; ++y) {
		const uint32_t *in = src + y * 1024;
		uint8_t *row = dst + y * stride * 2;
		const uint32_t rowAddr = addr + y * stride * 2;
		listGuard_.Begin(rowAddr, row, 0, width * 2);
		for (int x = 0; x < width; ++x) {
			// Window depth z/65535 (see PushVertex) stored as 24 bits.
			const uint32_t z24 = in[x] >> 8;
			// (z24 * 65535 + 0x7FFFFF) / 0xFFFFFF without the divide (a 64-bit divide per pixel,
			// ~140,000 a frame, cost ~3.5 ms on the PPU): dividing by 2^24 - 1 as (v + (v >> 24) +
			// 1) >> 24, the same for all 2^24 inputs (checked).
			const uint64_t v = (uint64_t)z24 * 65535 + 0x7FFFFF;
			const uint32_t z = (uint32_t)((v + (v >> 24) + 1) >> 24);
			row[x * 2] = (uint8_t)z;
			row[x * 2 + 1] = (uint8_t)(z >> 8);
		}
		listGuard_.End(rowAddr, row, "depth", addr, y);
	}
}

void RSXRenderer::DownloadDepth(DepthSurface &d) {
	TbScope tb(tbDownload_);
	TbScope tbCause(dlTb_[DL_DEPTH]);
	dlCount_[DL_DEPTH]++;
	RSX::StepQuiet("renderer: write back RSX depth to VRAM");
	const int stride = d.stride ? d.stride : 512;
	const int width = std::min<int>(std::min<int>(stride, 1024), d.w / scale_);
	const int y0 = std::max(d.drawnY0, 0);
	// Rows past the screen were drawn only as far as a scissor says; written back they could
	// cover textures kept right after the buffer (Killzone keeps some there).
	const int y1 = std::min<int>(std::min(d.drawnY1, PSP_H), d.h / scale_);
	d.gpuDirty = false;
	d.drawnY0 = 0x7FFFFFFF;
	d.drawnY1 = 0;
	if (y1 <= y0 || width <= 0 || !Memory::IsValidRange(d.addr, (uint32_t)stride * 2 * y1))
		return;
	if (Memory::IsValidRange(d.addr, DepthProbeBytes(d)) && ProbeHash(Memory::GetPointerUnchecked(d.addr), DepthProbeBytes(d)) != d.vramProbe) {
		// The CPU put something else there since (a texture in memory that was a depth buffer).
		if (depthDownloads_++ < 8)
			RSXEventLog("RSX renderer: depth %08x changed in VRAM, not written back", d.addr);
		return;
	}
	if (depthStaging_ && lateDepth_.fence && lateDepth_.addr == d.addr && lateDepth_.stride == stride) {
		// The copy started at the last read: into PSP memory, and the next copy starts without
		// waiting. Midnight Club L.A. reads its depth once a frame; stopping the RSX for it made
		// the GE thread wait for the whole scene to be drawn every frame.
		if (!RSX::FenceDone(lateDepth_.fence))
			RSX::WaitFence(lateDepth_.fence);
		WriteDepthRows(lateDepth_.addr, lateDepth_.stride, lateDepth_.width, lateDepth_.y0, lateDepth_.y1, depthStaging_);
		FlushBatch();
		RSX::GpuWaitForRendering();
		RSX::ScaleImage(RSX::Mem::Local, d.offset, d.pitch, width * scale_, y1 * scale_,
			RSX::Mem::Main, RSX::IoOffset(depthStaging_), 1024 * 4, 0, 0, width, y1, false);
		lateDepth_.fence = RSX::Fence();
		lateDepth_.width = width;
		lateDepth_.y0 = y0;
		lateDepth_.y1 = y1;
		if (lateDepthLogs_++ < 1)
			RSXEventLog("RSX renderer: depth %08x read one frame late (no RSX wait)", d.addr);
		vramGen_++;
		return;
	}
	FlushBatch();
	RSX::GpuWaitForRendering();
	// Z24S8 words copied as colour (no filtering): depth in the top 24 bits.
	RSX::ScaleImage(RSX::Mem::Local, d.offset, d.pitch, width * scale_, y1 * scale_,
		RSX::Mem::Main, RSX::IoOffset(staging_), 1024 * 4, 0, 0, width, y1, false);
	if (!RSX::Finish()) {
		PS3Frontend::Trace("RSX renderer: depth write-back of %08x timed out", d.addr);
		return;
	}
	WriteDepthRows(d.addr, stride, width, y0, y1, staging_);
	if (depthStaging_) {
		// Late reads from now on: the next read gets this copy, started now without waiting.
		RSX::ScaleImage(RSX::Mem::Local, d.offset, d.pitch, width * scale_, y1 * scale_,
			RSX::Mem::Main, RSX::IoOffset(depthStaging_), 1024 * 4, 0, 0, width, y1, false);
		lateDepth_.fence = RSX::Fence();
		lateDepth_.addr = d.addr;
		lateDepth_.stride = stride;
		lateDepth_.width = width;
		lateDepth_.y0 = y0;
		lateDepth_.y1 = y1;
	}
	if (depthDownloads_++ < 8)
		RSXEventLog("RSX renderer: depth %08x (stride %d, rows %d-%d) written back for a texture read", d.addr, stride, y0, y1);
	vramGen_++;
}

void RSXRenderer::DownloadAll() {
	for (auto &s : surfaces_)
		if (s->gpuDirty) {
			dlCause_ = DL_READ;
			Download(*s);
		}
}

// Makes the surface's RSX copy match VRAM if the CPU (or a transfer) changed VRAM since we last
// synced. Returns false if the surface couldn't be used.
bool RSXRenderer::Validate(Surface &s) {
	if (s.gpuDirty) return true;  // Our copy is newer than VRAM.
	// Hashing a framebuffer costs ~0.3 ms on the PPU; once per frame (or after a write-back) is enough.
	if (s.synced && s.validatedFrame == frame_ && s.validatedGen == vramGen_) return true;
	s.validatedFrame = frame_;
	s.validatedGen = vramGen_;
	const uint64_t hash = HashVram(s);
	if (s.keptOnRsx && s.synced && hash != s.syncHash && (int)s.rowHash.size() == s.rows) {
		// Kept at a layout switch: only a few rows changed in VRAM (the CPU wrote there, not a
		// picture for this buffer)? Then our copy stays.
		const uint8_t *p = Memory::GetPointerUnchecked(s.addr);
		const uint32_t rowBytes = s.stride * BytesPerPixel(s.fmt);
		uint32_t start, len;
		RowHashSpan(s, &start, &len);
		int changed = 0;
		for (int y = 0; y < s.rows; ++y)
			if ((uint32_t)XXH3_64bits(p + y * rowBytes + start, len) != s.rowHash[y]) changed++;
		if (changed * 4 < s.rows) {
			s.syncHash = hash;
			ComputeRowHashes(s);
			keptRowSkips_++;
			return true;
		}
	}
	if (!s.synced || hash != s.syncHash) {
		s.keptOnRsx = false;
		Upload(s);
		s.syncHash = hash;
		s.synced = true;
		ComputeRowHashes(s);
	}
	return true;
}

// Texel rows a 2D draw samples (texture coordinates are texels in through mode): 0 when unknown.
static int TexRowsSampled(float tMin, float tMax) {
	if (tMin < 0.0f || !(tMax < 4096.0f))
		return 0;
	// The last row only blends into the edge pixels; its source is VRAM from the last write-back.
	return (int)ceilf(tMax);
}

bool RSXRenderer::EnsureTarget() {
	RSX::StepQuiet("renderer: bind framebuffer");
	const uint32_t key[4] = {NormalizeAddr(GS.getFrameBufAddress()), (uint32_t)GS.FrameBufStride(),
		(uint32_t)GS.FrameBufFormat(), NormalizeAddr(GS.getDepthBufAddress())};
	int rows = std::max(PSP_H, std::min(511, std::max(GS.getScissorY2(), std::min(GS.getRegionY2(), 511))) + 1);
	if (targetValid_ && memcmp(key, targetKey_, sizeof(key)) == 0 && (!target_ || rows <= target_->rows || rows == targetReqRows_))
		return target_ != nullptr && depth_ != nullptr;
	targetReqRows_ = rows;
	// A scissor reaching row 511 doesn't mean the game draws there: stop at the depth buffer or the
	// next framebuffer, which is where games put their textures too. Otherwise those rows are
	// written back over them (Killzone's menu text turned into blocks).
	{
		const uint32_t rowBytes = key[1] * (key[2] == GE_FORMAT_8888 ? 4 : 2);
		// The nearest buffer start that still leaves a full screen (a buffer inside the first 272
		// rows is an older layout - Killzone has an 8888 buffer half way into another one).
		const uint32_t minEnd = key[0] + rowBytes * PSP_H;
		uint32_t limit = 0xFFFFFFFF;
		if (key[3] >= minEnd) limit = key[3];
		for (const auto &ptr : surfaces_)
			if (ptr->addr >= minEnd && ptr->addr < limit) limit = ptr->addr;
		if (rowBytes && limit != 0xFFFFFFFF) {
			const int maxRows = (int)((limit - key[0]) / rowBytes);
			if (rows > maxRows) rows = maxRows;
		}
	}

	FlushBatch();
	memcpy(targetKey_, key, sizeof(key));
	targetValid_ = true;
	stateValid_ = false;
	target_ = FindSurface(key[0], (uint16_t)key[1], (GEBufferFormat)key[2], rows, true, true);
	rttCopySrc_ = nullptr;  // A new pass: the next read of a buffer copies it again.
	if (!target_) return false;
	Validate(*target_);
	if (target_->sampledPending) {
		// Draws already sent may still be sampling this buffer: let them finish before drawing
		// over it. Silent Hill Origins copies its screen out of its scene buffer, then at once
		// draws a light mask into that buffer's top-left 128x128; the copy sometimes picked up
		// the mask (a black square with a white blob flashing in the corner for a frame).
		RSX::GpuWaitForRendering();
		RSX::InvalidateTextureCache();
		target_->sampledPending = false;
		readWaits_++;
	}
	depth_ = FindDepth(key[3], target_->w, target_->h);
	if (!depth_) { target_ = nullptr; return false; }
	depth_->stride = (uint16_t)GS.DepthBufStride();
	RSX::Target t;
	t.colorOffset = target_->offset;
	t.colorPitch = target_->pitch;
	t.depthOffset = depth_->offset;
	t.depthPitch = depth_->pitch;
	t.width = targetW_ = target_->w;
	t.height = targetH_ = target_->h;
	RSX::SetTarget(t);
	boundTarget_ = t;
	RSX::SetPixelViewport(targetW_, targetH_);
	RSX::SetDefaultState();
	tlCull_ = 0;
	RSX::BindVertexProgram();
	vpTL_ = false;
	if (depth_->fresh) {
		// Zeroed like fresh PSP VRAM; games clear depth themselves before relying on it.
		RSX::ClearTarget(0, false, true, 0);
		depth_->fresh = false;
	}
	return true;
}

bool RSXRenderer::HasPendingWrite(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) {
	start = NormalizeAddr(start);
	const uint32_t end = start + stride * (h ? h - 1 : 0) + w;
	for (const auto &ptr : surfaces_) {
		const Surface &s = *ptr;
		if (!s.gpuDirty && !(target_ == &s && !batch_.empty())) continue;
		const uint32_t sEnd = s.addr + s.stride * BytesPerPixel(s.fmt) * s.rows;
		if (start < sEnd && end > s.addr) return true;
	}
	return false;
}

// Writes back what the RSX drew over PSP memory start-end, which a block transfer reads or writes
// (stride: its row pitch, width: bytes a row; both in bytes): only the rows and, with the same row
// pitch, the columns it covers (DownloadPart), else the whole buffer. queued: for a transfer the GE
// worker makes in order with the draws (QueueTransferToRam).
void RSXRenderer::WriteBackSpan(uint32_t rangeStart, uint32_t rangeEnd, uint32_t stride, uint32_t width, bool queued) {
	const uint32_t start = NormalizeAddr(rangeStart);
	const uint32_t end = start + (rangeEnd - rangeStart);
	FlushBatch();
	for (auto &ptr : surfaces_) {
		Surface &s = *ptr;
		if (!s.gpuDirty)
			continue;
		const uint32_t sEnd = s.addr + s.stride * BytesPerPixel(s.fmt) * s.rows;
		if (start >= sEnd || end <= s.addr)
			continue;
		const uint32_t bpp = BytesPerPixel(s.fmt);
		const uint32_t rowBytes = s.stride * bpp;
		const uint32_t lo = std::max(start, s.addr) - s.addr, hi = std::min(end, sEnd) - s.addr;
		const int y0 = (int)(lo / rowBytes), y1 = (int)((hi + rowBytes - 1) / rowBytes);
		int x0 = 0, x1 = 0x7FFFFFFF;
		if (stride == rowBytes && width < rowBytes) {
			const uint32_t col = start >= s.addr ? (start - s.addr) % rowBytes : rowBytes - (s.addr - start) % rowBytes;
			if (col % rowBytes + width <= rowBytes) {
				x0 = (int)((col % rowBytes) / bpp);
				x1 = (int)((col % rowBytes + width + bpp - 1) / bpp);
			}
		}
		const bool part = DownloadPart(s, y0, y1, x0, x1);
		if (xferFlushLogs_ < 8) {
			xferFlushLogs_++;
			RSXEventLog("RSX renderer: block transfer%s over %08x-%08x (row pitch %u, %u bytes a row): %08x (stride %d format %d) %s",
				queued ? " to main memory, in order with the drawing," : "", rangeStart, rangeEnd, stride, width, s.addr, s.stride, (int)s.fmt,
				part ? "rows/columns it covers written back" : "written back");
		}
		if (!part) {
			dlCause_ = DL_READ;
			Download(s);
		}
	}
}

void RSXRenderer::WriteBackRange(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) {
	queuedXfers_++;
	WriteBackSpan(start, start + stride * (h ? h - 1 : 0) + w, stride, w, true);
}

// At a GE sync point the game may read VRAM with its CPU. What it drew only a few pixels of since
// the last write-back is copied off the RSX here, and written to VRAM at a later sync once the RSX
// has made the copy (no wait; for a test the game reads a frame later, a frame late is fine).
// Silent Hill Origins draws one pixel each frame (480,0 of 04000000, from its scene) to see whether
// its lamp is hidden, and reads it to decide on the lamp's glow. That pixel used to reach VRAM only
// with the write-back of everything at its light-mask copy; once that copy wrote back only what it
// reads (build 243), the glow came and went. Waiting for the RSX at the sync instead cost ~7 ms a
// frame (build 246).
void RSXRenderer::Sync() {
	if (!smallStaging_)
		return;
	// Writes a finished small copy into PSP memory.
	auto complete = [this]() {
		smallWb_.fence = 0;
		bool found = false;
		for (auto &ptr : surfaces_) {
			Surface &s = *ptr;
			// The same buffer, with no full write-back since (that has newer pixels in VRAM).
			if (s.addr != smallWb_.addr || s.stride != smallWb_.stride || s.fmt != smallWb_.fmt || !s.gpuDirty || s.dlSerial != smallWb_.dlSerial)
				continue;
			found = true;
			const int bpp = BytesPerPixel(s.fmt);
			if ((int)s.rowHash.size() != s.rows || smallWb_.y1 > s.rows || !Memory::IsValidRange(s.addr, s.stride * bpp * s.rows))
				break;
			uint8_t *dst = Memory::GetPointerWriteUnchecked(s.addr);
			uint32_t hashStart, hashLen;
			RowHashSpan(s, &hashStart, &hashLen);
			for (int y = smallWb_.y0; y < smallWb_.y1; ++y) {
				uint8_t *row = dst + y * s.stride * bpp;
				// The CPU changed this row since: its bytes win.
				if ((uint32_t)XXH3_64bits(row + hashStart, hashLen) != s.rowHash[y]) {
					swbRowsKept_++;
					if (swbKeptLogs_ < 8) {
						swbKeptLogs_++;
						RSXEventLog("RSX renderer: small copy of %08x row %d not written: the CPU changed that row since (pixel %d now %08x)", s.addr, y, smallWb_.x0,
							bpp == 4 ? *(const uint32_t *)(row + smallWb_.x0 * 4) : (uint32_t)*(const uint16_t *)(row + smallWb_.x0 * 2));
					}
					continue;
				}
				swbRowsWritten_++;
				const uint32_t rowAddr = s.addr + y * s.stride * bpp;
				listGuard_.Begin(rowAddr, row, smallWb_.x0 * bpp, smallWb_.x1 * bpp);
				StagingRowToVram(s.fmt, smallStaging_ + (y - smallWb_.cy0) * SMALL_WB_W - smallWb_.cx0, row, smallWb_.x0, smallWb_.x1);
				listGuard_.End(rowAddr, row, "framebuffer", s.addr, y);
				s.rowHash[y] = (uint32_t)XXH3_64bits(row + hashStart, hashLen);
			}
			vramGen_++;
			break;
		}
		if (!found)
			swbDropped_++;
	};
	// A finished copy is written at the next GE sync after the RSX did it (waiting for it at the
	// sync, build 260 diagnostics, cost speed and didn't steady Silent Hill Origins' glow).
	if (smallWb_.fence) {
		if (!RSX::FenceDone(smallWb_.fence)) {
			swbPendingSyncs_++;
			return;  // The last copy isn't done yet.
		}
		complete();
	}
	for (auto &ptr : surfaces_) {
		Surface &s = *ptr;
		if (!s.gpuDirty || s.syncWbVersion == s.version || s.drawnY1 <= s.drawnY0 || s.drawnX1 <= s.drawnX0)
			continue;
		const int x0 = std::max(s.drawnX0, std::max(s.ownX0, 0)), x1 = std::min(s.drawnX1, std::min(s.ownX1, std::min<int>(s.stride, 1024)));
		const int y0 = std::max(s.drawnY0, 0), y1 = std::min(s.drawnY1, s.rows);
		if (x1 <= x0 || y1 <= y0 || x1 - x0 > SMALL_WB_W || y1 - y0 > SMALL_WB_H || (x1 - x0) * (y1 - y0) > 256)
			continue;
		s.syncWbVersion = s.version;
		if (syncWbLogs_ < 2) {
			syncWbLogs_++;
			RSXEventLog("RSX renderer: %08x drawn only at %d,%d-%d,%d: copied for the game to read (written to VRAM at a later GE sync)", s.addr, x0, y0, x1, y1);
		}
		// The RSX's copy is at least 2x2 pixels: a single pixel (at 1x: Killzone's 0,0-1,1, build 253)
		// stopped the RSX with a graphics exception. Only the drawn pixels go back to VRAM.
		int cx0 = x0, cx1 = x1, cy0 = y0, cy1 = y1;
		const int need = scale_ >= 2 ? 1 : 2;
		const int maxX = std::min<int>(s.stride, 1024), maxY = s.h / scale_;
		if (cx1 - cx0 < need) {
			if (cx0 + need <= maxX) cx1 = cx0 + need;
			else cx0 = std::max(0, cx1 - need);
		}
		if (cy1 - cy0 < need) {
			if (cy0 + need <= maxY) cy1 = cy0 + need;
			else cy0 = std::max(0, cy1 - need);
		}
		if ((cx1 - cx0) * scale_ < 2 || (cy1 - cy0) * scale_ < 2)
			continue;
		FlushBatch();
		RSX::GpuWaitForRendering();
		RSX::ScaleImage(RSX::Mem::Local, s.offset + (uint32_t)(cy0 * scale_) * s.pitch + (uint32_t)(cx0 * scale_) * 4, s.pitch, (cx1 - cx0) * scale_, (cy1 - cy0) * scale_,
			RSX::Mem::Main, RSX::IoOffset(smallStaging_), SMALL_WB_W * 4, 0, 0, cx1 - cx0, cy1 - cy0, false);
		smallWb_.cx0 = cx0;
		smallWb_.cy0 = cy0;
		swbIssued_++;
		smallWb_.fence = RSX::Fence();
		RSX::Flush();
		smallWb_.addr = s.addr;
		smallWb_.stride = s.stride;
		smallWb_.fmt = s.fmt;
		smallWb_.x0 = x0;
		smallWb_.x1 = x1;
		smallWb_.y0 = y0;
		smallWb_.y1 = y1;
		smallWb_.dlSerial = s.dlSerial;
		break;
	}
}

// Edge smoothing ("Edge smoothing" setting): the displayed part of s through the FXAA fragment
// program (make-shaders.py fxaa()) into fxaaMem_, at the internal resolution. False: not done (no
// memory), the picture is shown as it is.
bool RSXRenderer::FxaaPass(const Surface &s, uint32_t srcX, uint32_t srcY, int srcW, int srcH, uint32_t *outOffset, uint32_t *outPitch) {
	if (srcW <= 0 || srcH <= 0)
		return false;
	const uint64_t t0 = TimeBase();
	const uint32_t pitch = ((uint32_t)srcW * 4 + 63) & ~63u;
	if (!fxaaMem_ || fxaaBytes_ < pitch * srcH) {
		if (fxaaMem_) DeferFree(fxaaMem_);
		fxaaMem_ = AllocLocal(pitch * srcH);
		fxaaBytes_ = fxaaMem_ ? pitch * srcH : 0;
		if (!fxaaMem_)
			return false;
	}
	RSX::Target ft;
	ft.colorOffset = RSX::LocalOffset(fxaaMem_);
	ft.colorPitch = pitch;
	ft.width = (uint16_t)srcW;
	ft.height = (uint16_t)srcH;
	ft.hasDepth = false;
	RSX::SetTarget(ft);
	RSX::SetPixelViewport(ft.width, ft.height);
	RSX::SetDefaultState();
	tlCull_ = 0;
	RSX::BindVertexProgram();
	vpTL_ = false;
	const float zero[4] = {0, 0, 0, 0};
	const float rcp[4] = {1.0f / s.w, 1.0f / s.h, 0, 0};
	RSX::SetVertexConstants(zero, zero, rcp);
	RSX::InvalidateTextureCache();
	RSX::BindTexture(0, s.offset, s.w, s.h, s.pitch, true, true, true, false, true);
	RSX::BindFragmentProgram(opt_.fxaa == 2 ? RSX::FRAGMENT_FXAA_LIGHT : RSX::FRAGMENT_FXAA);
	uint32_t first = 0;
	RSX::Vertex *v = RSX::AllocVertices(6, &first);
	if (!v)
		return false;
	const float px[6] = {-1, 1, -1, 1, 1, -1}, py[6] = {-1, -1, 1, -1, 1, 1};
	const float u0 = (float)(srcX * scale_) / s.w, u1 = (float)(srcX * scale_ + srcW) / s.w;
	const float v0 = (float)(srcY * scale_) / s.h, v1 = (float)(srcY * scale_ + srcH) / s.h;
	for (int i = 0; i < 6; ++i) {
		RSX::Vertex o;
		o.x = px[i];
		o.y = cal_.flipY ? -py[i] : py[i];
		o.z = 0.0f;
		o.w = 1.0f;
		memset(o.c0, 255, 4);
		memset(o.c1, 0, 4);
		o.s = px[i] < 0 ? u0 : u1;
		o.t = py[i] < 0 ? v0 : v1;
		o.r = 0.0f;
		o.q = 1.0f;
		o.fog = 1.0f;
		o.pad = 0.0f;
		memcpy(&v[i], &o, sizeof(o));
	}
	__sync_synchronize();
	RSX::DrawVertices(GCM_TYPE_TRIANGLES, first, 6);
	// The transfer engine reads it next (ScaleImage).
	RSX::GpuWaitForRendering();
	RSX::InvalidateTextureCache();
	*outOffset = RSX::LocalOffset(fxaaMem_);
	*outPitch = pitch;
	tbFxaa_ += TimeBase() - t0;
	fxaaFrames_++;
	return true;
}

void RSXRenderer::Flush(const char *reason) {
	FlushBatch();
	// Reasons where the software GPU is about to read PSP memory the RSX may have drawn into.
	// Texture reads ("tex", "selfrender") are handled by LookupTexture: framebuffers are sampled
	// on the RSX directly, and only other overlapping reads download.
	static const char *const readers[] = {"blockxfer", "blockxfer_wrap", "loadclut", "debug", "output"};
	const Rasterizer::FlushRange range = Rasterizer::g_flushRange;
	if (strcmp(reason, "blockxfer") == 0 && range.end > range.start) {
		// A block transfer: only the buffers over the memory it reads or writes. Writing back all of
		// them cost Silent Hill Origins ~50 ms a frame at 2x in an area that copies out of one buffer
		// every frame (and the write-backs of overlapping buffers made each other upload again).
		WriteBackSpan(range.start, range.end, range.stride, range.width, false);
		return;
	}
	for (const char *r : readers) {
		if (strcmp(reason, r) == 0) {
			DownloadAll();
			return;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Textures

// Textures up to this size are hashed in full on every lookup (a 32x32 CLUT4 glyph is 1 KB).
static constexpr uint32_t TEX_SMALL_BYTES = 16 * 1024;

// A hash of 16 spread-out 64-byte pieces: catches a texture the CPU rewrote (a video frame, a
// rendered font page) without hashing all of it.
static uint64_t ProbeHash(const u8 *p, uint32_t bytes) {
	if (bytes <= 1024)
		return XXH3_64bits(p, bytes);
	uint64_t buf[16 * 8];
	const uint32_t step = (bytes - 64) / 15;
	for (int i = 0; i < 16; ++i)
		memcpy(&buf[i * 8], p + (uint32_t)i * step, 64);
	return XXH3_64bits(buf, sizeof(buf));
}

static bool SpuDecode(const SamplerID &id, const u8 *src, int bufw, int w, int h, uint32_t bytes, uint32_t *dst, uint32_t dstPitch);

// Whole-texture hash: on the SPUs for big ones (TexSpu; its own hash function, which is fine since
// a texture's hash is only compared with its earlier ones).
static uint64_t TextureHash(const u8 *p, uint32_t bytes) {
	uint64_t h;
	const uint32_t body = bytes & ~15u;
	if (bytes >= 32 * 1024 && ((uintptr_t)p & 15) == 0 && TexSpu::Hash(p, body, &h))
		return body == bytes ? h : h ^ XXH3_64bits(p + body, bytes - body);
	return XXH3_64bits(p, bytes);
}

// The palette bytes a level-0 decode can read (Sampler's TransformClutIndex and LookupColor):
// a CLUT4 texture with a 16-bit palette reaches 16 entries, 32 bytes, not the whole 1 KB that was
// hashed for every lookup. Hashing just those is the same key for the same picture.
// (ClutBytesUsed: GPU/Software/FuncId.h.)

bool RSXRenderer::LookupTexture(const RasterizerState &state, TexEntry **out) {
	RSX::StepQuiet("renderer: texture");
	const SamplerID &id = state.samplerID;
	const GETextureFormat fmt = id.TexFmt();
	const uint32_t addr = NormalizeAddr(state.texaddr[0]);
	const int w = id.cached.sizes[0].w, h = id.cached.sizes[0].h;
	const int bufw = state.texbufw[0];
	const u8 *texptr = state.texptr[0];
	if (!texptr || w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
	uint32_t bytes = (uint32_t)(textureBitsPerPixel[fmt] * (uint64_t)bufw * h / 8);
	// Wider than its buffer: the GE reads past the buffer width (texel x of row y at y * bufw + x;
	// swizzled, block column x/16 bytes of block row y/8 at (y/8 * blocks per row + x/16 bytes) * 128),
	// so the data reaches further. Silent Hill Origins' film grain is 128x128 CLUT4 with a buffer
	// width of 32: real noise all the way across, laid out for that addressing.
	const bool wide = bufw > 0 && w > bufw;
	if (wide) {
		const uint32_t bitsPP = textureBitsPerPixel[fmt];
		if (id.swizzle) {
			const uint32_t blocksPerRow = std::max<uint32_t>(1, (uint32_t)bufw * bitsPP / 128);
			const uint32_t lastBlock = (uint32_t)((h - 1) / 8) * blocksPerRow + ((uint32_t)w * bitsPP / 8 - 1) / 16;
			bytes = (lastBlock + 1) * 128;
		} else {
			bytes = (uint32_t)((bitsPP * ((uint64_t)bufw * (h - 1) + w) + 7) / 8);
		}
	}
	// The system dialogs' font atlas (PPGe) isn't in PSP memory: it has a fake address, and
	// texptr points at the host copy. No framebuffer can be there.
	u32 atlasOffset = 0;
	const bool atlas = IsPPGEAtlasFakeAddress(addr, &atlasOffset);
	if (atlas) {
		bytes = std::min(bytes, g_ppgeAtlasFakeSize - atlasOffset);
		if (bytes == 0) return false;
	} else if (bytes == 0 || !Memory::IsValidRange(addr, bytes)) {
		bytes = Memory::ClampValidSizeAt(addr, bytes);
		if (bytes == 0) return false;
	}

#ifdef PPSSPP_RSX_HOSTSIM
	static const bool texLog = getenv("PPSSPP_TEXLOG") != nullptr;  // Host sim: each lookup's path.
#else
	constexpr bool texLog = false;
#endif
	lookups_++;
	const uint64_t tLk0 = TimeBase();
	if (!atlas && LookupRenderTarget(addr, fmt, w, h, bufw, id.swizzle, state.texFlushGen, out)) {
		if (texLog) fprintf(stderr, "TEXLOOK %08x fmt %d %dx%d bufw %d: framebuffer\n", addr, (int)fmt, w, h, bufw);
		return true;
	}
	if (!atlas && !id.swizzle && LookupDepal(id, addr, fmt, w, h, bufw, out)) {
		if (texLog) fprintf(stderr, "TEXLOOK %08x fmt %d %dx%d bufw %d: palette lookup of a framebuffer\n", addr, (int)fmt, w, h, bufw);
		return true;
	}
	if (texLog) fprintf(stderr, "TEXLOOK %08x fmt %d %dx%d bufw %d cfmt %06x: texture\n", addr, (int)fmt, w, h, bufw, id.cached.clutFormat & 0xFFFFFF);

	// Render to texture in a format we can't sample directly: make sure pixels the RSX drew are in
	// PSP memory before reading them.
	uint32_t readBytes = bytes;
	if (texRowsHint_ > 0 && !id.swizzle && !atlas && texRowsHint_ < h)
		readBytes = std::min(bytes, (uint32_t)(textureBitsPerPixel[fmt] * (uint64_t)bufw * texRowsHint_ / 8));
	for (auto &ptr : surfaces_) {
		if (atlas) break;
		Surface &s = *ptr;
		if (!s.gpuDirty) continue;
		const uint32_t sEnd = s.addr + s.stride * BytesPerPixel(s.fmt) * s.rows;
		if (addr < sEnd && addr + readBytes > s.addr) {
			if (overlapLogs_ < 24) {
				overlapLogs_++;
				RSXEventLog("RSX renderer: texture %08x fmt %d %dx%d bufw %d swz %d overlaps framebuffer %08x fmt %d stride %d rows %d - written back",
					addr, (int)fmt, w, h, bufw, id.swizzle ? 1 : 0, s.addr, (int)s.fmt, s.stride, s.rows);
			}
			dlCause_ = DL_TEXTURE;
			Download(s);
		}
	}
	for (auto &ptr : depths_) {
		if (atlas || opt_.skipDepthReads) break;
		DepthSurface &d = *ptr;
		if (!d.gpuDirty) continue;
		const uint32_t dEnd = d.addr + (uint32_t)(d.stride ? d.stride : 512) * 2 * (uint32_t)std::max(d.drawnY1, 0);
		if (addr < dEnd && addr + readBytes > d.addr)
			DownloadDepth(d);
	}

	const uint64_t tLk1 = TimeBase();
	tbLkFb_ += tLk1 - tLk0;
	uint64_t clutHash = 0;
	const bool isClut = fmt >= GE_TFMT_CLUT4 && fmt <= GE_TFMT_CLUT32;
	if (isClut && id.cached.clut)
		clutHash = XXH3_64bits(id.cached.clut, ClutBytesUsed(id));
	// Frontend 112: the palette format word too (colour format, shift, mask, start index): the
	// decode bakes it in, so the same texture and palette data read with other palette settings
	// is a different picture (it got the first one's decode).
	const uint64_t clutFmtKey = isClut ? (uint64_t)(id.cached.clutFormat & 0xFFFFFF) * 0xC2B2AE3D27D4EB4Full : 0;
	const uint64_t key = XXH3_64bits(&addr, 4) ^ ((uint64_t)fmt << 56) ^ ((uint64_t)w << 44) ^ ((uint64_t)h << 32) ^
		((uint64_t)bufw << 16) ^ (clutHash * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)id.swizzle << 63) ^ clutFmtKey;
	const uint64_t tLk2 = TimeBase();
	TexEntry &e = textures_[key];
	e.lastFrame = frame_;
	const uint64_t tLk3 = TimeBase();
	tbLkKey_ += tLk2 - tLk1;
	tbLkMap_ += tLk3 - tLk2;
	// Small textures are hashed in full whenever the game may have run since the last check: games
	// rewrite glyph and sprite buffers between draws, even within a frame (Gran Turismo's menu
	// text). Bigger ones are fully rehashed once a frame (every 4th frame once stable: hashing
	// everything every frame cost ~8 ms a frame in Dead to Rights), with a cheap probe in between.
	const bool small = bytes <= TEX_SMALL_BYTES;
	if (e.mem && e.validatedGen == vramGen_ && e.addr == addr && !opt_.textureCheckFull &&
		(e.validatedEpoch == state.cpuEpoch || Rasterizer::GeRangeUnchanged(addr, bytes, e.validatedEpoch, state.cpuEpoch))) {
		// Nothing but drawing (tracked by vramGen_) changed this memory since the last check.
		e.validatedEpoch = state.cpuEpoch;
		*out = &e;
		return true;
	}
	e.validatedEpoch = state.cpuEpoch;
	if (e.mem && e.validatedGen == vramGen_ && e.addr == addr && !small) {
		const uint32_t age = frame_ - e.validatedFrame;
		if (age == 0 || (e.stable >= 8 && age < 4)) {
			uint64_t probe;
			{
				TbScope th(tbHash_);
				probe = ProbeHash(texptr, bytes);
			}
			if (probe == e.probeHash) {
				*out = &e;
				return true;
			}
		}
	}
	uint64_t dataHash, probeHash;
	{
		TbScope th(tbHash_);
		dataHash = TextureHash(texptr, bytes);
		probeHash = small ? dataHash : ProbeHash(texptr, bytes);
	}
	e.probeHash = probeHash;
	e.validatedFrame = frame_;
	e.validatedGen = vramGen_;
	if (e.mem && dataHash == e.dataHash && e.addr == addr) {
		if (e.stable < 0xFFFF) e.stable++;
		*out = &e;
		return true;
	}
	e.stable = 0;

	// (Re)decode with the software sampler's own fetch function: every PSP format, swizzle and
	// CLUT mode comes out exactly as the software renderer would read it.
	Sampler::FetchFunc fetch = Sampler::GetFetchFunc(id, nullptr);
	if (!fetch) {
		Sampler::FlushJit();
		fetch = Sampler::GetFetchFunc(id, nullptr);
		if (!fetch) return false;
	}
	TbScope td(tbDecode_);
	const uint32_t pitch = ((uint32_t)w * 4 + 63) & ~63u;
	void *mem = AllocLocal(pitch * h);
	if (!mem) return false;
	uint32_t *dst = (uint32_t *)mem;
	// The SPU kernel reads 8-row bands of bufw texels; a texture wider than its buffer is decoded
	// on the PPU (FastDecode addresses every texel as the GE does).
	bool fast = !wide && identityTex_ && SpuDecode(id, texptr, bufw, w, h, bytes, dst, pitch / 4);
	if (fast) {
		spuDecodes_++;
#ifdef PPSSPP_RSX_HOSTSIM
		if (getenv("PPSSPP_TEXK_CHECK")) {
			std::vector<uint32_t> ref((size_t)(pitch / 4) * h);
			FastDecode(id, texptr, bufw, w, h, bytes, ref.data(), pitch / 4);
			for (int y = 0; y < h; ++y) {
				if (memcmp(ref.data() + y * (pitch / 4), dst + y * (pitch / 4), w * 4) != 0) {
					fprintf(stderr, "TEXK MISMATCH fmt %d %dx%d bufw %d swz %d row %d\n", (int)fmt, w, h, bufw, id.swizzle ? 1 : 0, y);
					break;
				}
			}
		}
#endif
	} else {
		fast = identityTex_ && FastDecode(id, texptr, bufw, w, h, bytes, dst, pitch / 4);
	}
	if (!fast)
		slowDecodes_++;
#ifdef PPSSPP_RSX_HOSTSIM
	// Host sim: PPSSPP_TEXDUMP=<hex addr> writes that texture as decoded (after the column repeat
	// below is applied the dump would differ; this is the raw decode) to texdump_<addr>.ppm.
	if (const char *td = getenv("PPSSPP_TEXDUMP")) {
		if (strtoul(td, nullptr, 16) == addr) {
			char name[64];
			snprintf(name, sizeof(name), "texdump_%08x.ppm", addr);
			if (FILE *f = fopen(name, "wb")) {
				fprintf(f, "P6 %d %d 255\n", w, h);
				for (int y = 0; y < h; ++y)
					for (int x = 0; x < w; ++x) {
						const uint32_t v = dst[y * (pitch / 4) + x];
						const uint8_t px[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
						fwrite(px, 1, 3, f);
					}
				fclose(f);
			}
		}
	}
	// Host sim: PPSSPP_TEXREF_CHECK=1 compares the fast decode with the sampler's own fetch.
	if (fast && getenv("PPSSPP_TEXREF_CHECK")) {
		int bad = 0, firstX = -1, firstY = -1;
		uint32_t got = 0, want = 0;
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				const uint32_t rgba = Math3D::Vec4<int>(fetch(x, y, texptr, bufw, 0, id)).ToRGBA();
				const uint32_t argb = (rgba & 0xFF000000) | ((rgba & 0xFF) << 16) | (rgba & 0xFF00) | ((rgba >> 16) & 0xFF);
				const uint32_t have = dst[y * (pitch / 4) + x];
				if (have != argb) {
					if (!bad) { firstX = x; firstY = y; got = have; want = argb; }
					bad++;
				}
			}
		}
		if (bad)
			fprintf(stderr, "TEXREF MISMATCH %08x fmt %d %dx%d bufw %d swz %d cfmt %06x: %d texels, first %d,%d got %08x want %08x\n",
				addr, (int)fmt, w, h, bufw, id.swizzle ? 1 : 0, id.cached.clutFormat & 0xFFFFFF, bad, firstX, firstY, got, want);
	}
#endif
	for (int y = 0; y < h && !fast; ++y) {
		uint32_t *row = dst + y * (pitch / 4);
		for (int x = 0; x < w; ++x) {
			const uint32_t rgba = Math3D::Vec4<int>(fetch(x, y, texptr, bufw, 0, id)).ToRGBA();
			const uint32_t argb = (rgba & 0xFF000000) | ((rgba & 0xFF) << 16) | (rgba & 0xFF00) | ((rgba >> 16) & 0xFF);
			if (identityTex_) {
				row[x] = argb;
			} else {
				// Output byte j (A,R,G,B from the top) is read from memory byte texOrder[j].
				uint8_t *bytes = (uint8_t *)&row[x];
				for (int j = 0; j < 4; ++j) bytes[cal_.texOrder[j]] = (uint8_t)(argb >> (24 - 8 * j));
			}
		}
	}
	__sync_synchronize();
	if (e.mem) {
		DeferFree(e.mem);
		textureBytes_ -= e.bytes;
	}
	e.mem = mem;
	e.offset = RSX::LocalOffset(mem);
	e.pitch = pitch;
	e.bytes = pitch * h;
	e.addr = addr;
	e.fmt = fmt;
	e.w = (uint16_t)w;
	e.h = (uint16_t)h;
	e.bufw = (uint16_t)bufw;
	e.clutHash = clutHash;
	e.dataHash = dataHash;
	textureBytes_ += e.bytes;
	RSX::InvalidateTextureCache();
	texDecodes_++;
	if (textureBytes_ > TEXTURE_BUDGET) EvictTextures(60);
	*out = &textures_[key];
	return true;
}

// A texture at (or inside) a framebuffer we hold, in the same format and stride: sample our RSX
// copy. A texture starting inside the framebuffer (a sub-rectangle, common for post effects) or
// the framebuffer being drawn to is first copied by the RSX into a separate buffer.
bool RSXRenderer::LookupRenderTarget(uint32_t addr, GETextureFormat fmt, int w, int h, int bufw, bool swizzle, uint32_t texFlushGen, TexEntry **out) {
	if (swizzle || fmt > GE_TFMT_8888)
		return false;
	// A framebuffer that starts exactly at the texture wins over one it lies inside of: Silent Hill
	// Origins draws its screen at 0404c000, right below the one at 04000000 (same stride), and
	// taking it for rows 152+ of that one showed only its top 120 rows, from a stale copy.
	Surface *exact = nullptr;
	for (auto &ptr : surfaces_) {
		if (ptr->addr == addr && (int)ptr->fmt == (int)fmt && ptr->stride == bufw) {
			exact = ptr.get();
			break;
		}
	}
	for (auto &ptr : surfaces_) {
		Surface &s = *ptr;
		if ((int)s.fmt != (int)fmt || s.stride != bufw)
			continue;
		if (exact && &s != exact)
			continue;
		const uint32_t bpp = BytesPerPixel(s.fmt);
		const uint32_t rowBytes = s.stride * bpp;
		if (addr < s.addr || addr >= s.addr + rowBytes * s.rows)
			continue;
		const uint32_t byteOff = addr - s.addr;
		if (byteOff % bpp)
			continue;
		const int y0 = (int)(byteOff / rowBytes);
		const int x0 = (int)((byteOff % rowBytes) / bpp);
		const int surfW = s.w / scale_, surfH = s.h / scale_;
		const int copyW = std::min(w, surfW - x0), copyH = std::min(h, std::min(surfH, (int)s.rows) - y0);
		if (copyW <= 0 || copyH <= 0)
			continue;
		if (texRowsHint_ > copyH + 2) {
			// The draw samples rows past the end of this buffer, which our copy doesn't have: read
			// the texture from memory. Silent Hill Origins' loading screen (a picture at 0404c000,
			// copied to the screen) sits in rows 152+ of the buffer at 04000000, which holds only
			// 272 rows: just the top 120 rows of the picture showed.
			if (rttShortLogs_ < 4) {
				rttShortLogs_++;
				RSXEventLog("RSX renderer: texture %08x samples %d rows, framebuffer %08x has %d from there - read from memory",
					addr, texRowsHint_, s.addr, copyH);
			}
			continue;
		}
		Validate(s);
		MergeCpuRows(s);
		if (rttLogs_ < 16) {
			rttLogs_++;
			RSXEventLog("RSX renderer: texture %08x fmt %d %dx%d read from framebuffer %08x%s, texflush %u, frame %u",
				addr, (int)fmt, w, h, s.addr, &s == target_ ? " (the one being drawn to)" : "", texFlushGen, frame_);
		}
		uint32_t offset = s.offset;
		uint32_t pitch = s.pitch;
		if (&s == target_ || byteOff != 0) {
			// Copy the part the texture covers, unless the copy we hold is still what the GE's
			// texture cache would show.
			// Draws to the buffer that read the copy themselves keep it (MarkTargetDrawn drops it on
			// any other draw), even across TEXFLUSH: a chain of draws that each sample the screen
			// they draw to (Killzone: ~150 a frame, Dead or Alive Paradise's outline pass) otherwise
			// copied the whole screen, with two GPU pipeline drains, for every draw.
			const bool reuse = rttCopy_ && rttCopySrc_ == &s && rttCopyAddr_ == addr && rttCopyW_ == w && rttCopyH_ == h &&
				rttCopyFrame_ == frame_ && (&s == target_ && opt_.rttCopyReuse ? true : (rttCopyGen_ == texFlushGen && s.rttWaitVersion == s.version));
			if (!reuse) {
				const uint32_t bytes = s.pitch * (uint32_t)(h * scale_);
				if (!rttCopy_ || rttCopyBytes_ < bytes) {
					if (rttCopy_) DeferFree(rttCopy_);
					rttCopy_ = AllocLocal(bytes);
					rttCopyBytes_ = rttCopy_ ? bytes : 0;
				}
				if (!rttCopy_) {
					rttCopySrc_ = nullptr;
					return false;
				}
				FlushBatch();
				RSX::GpuWaitForRendering();
				const uint32_t src = s.offset + (uint32_t)(y0 * scale_) * s.pitch + (uint32_t)(x0 * scale_) * 4;
				RSX::ScaleImage(RSX::Mem::Local, src, s.pitch, copyW * scale_, copyH * scale_, RSX::Mem::Local,
					RSX::LocalOffset(rttCopy_), s.pitch, 0, 0, copyW * scale_, copyH * scale_, false);
				RSX::GpuWaitForRendering();
				RSX::InvalidateTextureCache();
				rttCopySrc_ = &s;
				rttCopyAddr_ = addr;
				rttCopyW_ = w;
				rttCopyH_ = h;
				rttCopyGen_ = texFlushGen;
				rttCopyFrame_ = frame_;
				rttCopyX0_ = x0;
				rttCopyY0_ = y0;
				rttCopyX1_ = x0 + copyW;
				rttCopyY1_ = y0 + copyH;
				s.rttWaitVersion = s.version;
				rttCopies_++;
			}
			offset = RSX::LocalOffset(rttCopy_);
			drawReadsRttCopy_ = true;
			if (byteOff != 0) rttOffsetHits_++;
		} else {
			if (s.rttWaitVersion != s.version) {
				// Changed since the last read: the texture unit must see the finished pixels.
				if (s.gpuDirty) {
					FlushBatch();
					RSX::GpuWaitForRendering();
				}
				RSX::InvalidateTextureCache();
				s.rttWaitVersion = s.version;
			}
			s.sampledPending = true;
		}
		rtt_.offset = offset;
		rtt_.pitch = pitch;
		rtt_.w = (uint16_t)(w * scale_);
		rtt_.h = (uint16_t)(h * scale_);
		rtt_.uvW = (uint16_t)w;
		rtt_.uvH = (uint16_t)h;
		rtt_.addr = addr;
		rttHits_++;
		*out = &rtt_;
		return true;
	}
	return false;
}

// Direct decoders for the common formats (the per-texel sampler call is far too slow on the PPU).
// Output: native 0xAARRGGBB words.
static inline uint32_t TexelOffset(uint32_t xbytes, uint32_t y, uint32_t rowBytes, bool swizzle) {
	if (!swizzle)
		return y * rowBytes + xbytes;
	return ((y >> 3) * (rowBytes >> 4) + (xbytes >> 4)) * 128 + (y & 7) * 16 + (xbytes & 15);
}

static inline uint32_t Conv565(uint32_t v) {
	const uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
	return 0xFF000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}
static inline uint32_t Conv5551(uint32_t v) {
	const uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
	return ((v & 0x8000) ? 0xFF000000 : 0) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}
static inline uint32_t Conv4444(uint32_t v) {
	return (((v >> 12) & 15) * 17 << 24) | ((v & 15) * 17 << 16) | (((v >> 4) & 15) * 17 << 8) | ((v >> 8) & 15) * 17;
}
static inline uint32_t Conv8888(const u8 *p) {
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

// A CLUT32 texture over a 32-bit framebuffer: the game uses its rendered pixels as palette indices
// (color grading and similar effects; Killzone). Done on the RSX in one pass into a separate
// texture, instead of writing the framebuffer back to PSP memory and decoding it on the CPU.
// Handles an index taken from one 8-bit channel with a power-of-two mask.
bool RSXRenderer::LookupDepal(const SamplerID &id, uint32_t addr, GETextureFormat fmt, int w, int h, int bufw, TexEntry **out) {
	const bool clut16Tex = fmt == GE_TFMT_CLUT16;
	if ((fmt != GE_TFMT_CLUT32 && !clut16Tex) || !id.cached.clut)
		return false;
	const int shift = GS.getClutIndexShift();
	const int mask = GS.getClutIndexMask();
	const bool clut32 = GS.getClutPaletteFormat() == GE_CMODE_32BIT_ABGR8888;
	const int base = GS.getClutIndexStartPos() & (clut32 ? 0xFF : 0x1FF);
	if ((mask & (mask + 1)) != 0 || (base & mask) != 0)
		return false;
	// Build 264: texel = pixel, any shift (FRAGMENT_DEPALX): a CLUT16 texture of a 16-bit
	// framebuffer, or a CLUT32 texture of an 8888 one whose index sits inside one byte at a shift
	// that isn't a whole byte (3rd Birthday's glow: its screen with shift 12 and mask 15, then a
	// 4444 buffer as CLUT16). Byte-aligned CLUT32 lookups go the way they always did.
	const bool anyShift = clut16Tex || (shift & 7) != 0;
	if (anyShift) {
		int maskBits = 0;
		while ((1 << maskBits) <= mask)
			maskBits++;
		if (clut16Tex ? shift > 15 : (shift > 31 || (shift & 7) + maskBits > 8))
			return false;
	} else if (shift > 24) {
		return false;
	}
	// The source: a 32-bit framebuffer of the texture's width, or a 16-bit one twice as wide (each
	// texel is two of its pixels; Silent Hill Origins reads its 565 distance buffer this way for
	// its fog). A framebuffer that starts exactly at the texture wins over one it lies inside of
	// (as in LookupRenderTarget): Killzone grades its screen at 04044000; with Keep buffer layouts,
	// the menu's 8888 layout of 04000000 stayed on the RSX, and its rows 136+ (where 04044000
	// starts) were taken instead, a stale picture. Of several layouts at the address, the one drawn
	// last (with Keep buffer layouts, Silent Hill's 8888 screen stays at its 565 buffer's address).
	auto fits = [&](const Surface &s) {
		const bool wide = s.fmt == GE_FORMAT_8888;
		if (anyShift) {
			// Texel = pixel: CLUT16 reads a 16-bit framebuffer, CLUT32 an 8888 one, same width.
			if (wide == clut16Tex || s.stride != bufw)
				return false;
			const uint32_t rowBytes = s.stride * (wide ? 4 : 2);
			return addr >= s.addr && addr < s.addr + rowBytes * s.rows && ((addr - s.addr) & (wide ? 3 : 1)) == 0;
		}
		if (wide ? s.stride != bufw : s.stride != bufw * 2)
			return false;
		const uint32_t rowBytes = s.stride * (wide ? 4 : 2);
		return addr >= s.addr && addr < s.addr + rowBytes * s.rows && ((addr - s.addr) & 3) == 0;
	};
	Surface *src = nullptr;
	for (int pass = 0; pass < 2 && !src; ++pass) {
		for (auto &ptr : surfaces_) {
			Surface &s = *ptr;
			if ((pass == 0 && s.addr != addr) || !fits(s))
				continue;
			if (!src || s.drawSeq > src->drawSeq)
				src = &s;
		}
	}
	if (!src)
		return false;
	const bool src16 = src->fmt != GE_FORMAT_8888;
	const uint32_t srcRowBytes = src->stride * (src16 ? 2 : 4);
	const int y0 = (int)((addr - src->addr) / srcRowBytes);
	// In the source's pixels.
	const int x0 = (int)(((addr - src->addr) % srcRowBytes) / (src16 ? 2 : 4));
	// 16-bit: the texel's first or second pixel, and its low or high byte.
	const int pixel = shift >= 16 ? 1 : 0;
	if (anyShift ? x0 + w > std::min<int>(src->stride, 1024) : (src16 && x0 + 2 * w > std::min<int>(src->stride, 1024)))
		return false;
#ifdef PPSSPP_RSX_HOSTSIM
	if (getenv("PPSSPP_DEPALLOG"))
		fprintf(stderr, "DEPAL %08x from framebuffer %08x fmt %d row %d%s\n", addr, src->addr, (int)src->fmt, y0, anyShift ? " (new: any shift)" : "");
#endif
	if (anyShift && depalxLogs_ < 2) {
		depalxLogs_++;
		RSXEventLog("RSX renderer: palette lookup %08x (CLUT%d, shift %d, mask %x) from framebuffer %08x (format %d) done on the RSX",
			addr, clut16Tex ? 16 : 32, shift, mask, src->addr, (int)src->fmt);
	}
	if (!anyShift && src16 && depal16Logs_ < 2) {
		depal16Logs_++;
		RSXEventLog("RSX renderer: palette lookup %08x from 16-bit framebuffer %08x (format %d) done on the RSX", addr, src->addr, (int)src->fmt);
	}
	Validate(*src);

	// Palette: a 256 (32-bit entries) or 512 entry row, in a ring of slots so a palette the RSX may
	// still be reading is never overwritten.
	const int entries = clut32 ? 256 : 512;
	constexpr int CLUT_SLOTS = 128;
	if (!clutRing_) {
		clutRing_ = (uint32_t *)AllocLocal(CLUT_SLOTS * 512 * 4);
		if (!clutRing_)
			return false;
	}
	const u8 *clut = (const u8 *)id.cached.clut;
	const uint64_t clutHash = XXH3_64bits(clut, clut32 ? 1024 : 1024) ^ (uint64_t)GS.getClutPaletteFormat();
	if (clutSlot_ < 0 || clutHash != clutHash_) {
		clutSlot_ = (clutSlot_ + 1) % CLUT_SLOTS;
		uint32_t *dst = clutRing_ + clutSlot_ * 512;
		for (int i = 0; i < entries; ++i) {
			uint32_t c;
			if (clut32) {
				c = Conv8888(clut + i * 4);
			} else {
				const uint32_t v = clut[i * 2] | (clut[i * 2 + 1] << 8);
				switch (GS.getClutPaletteFormat()) {
				case GE_CMODE_16BIT_BGR5650: c = Conv565(v); break;
				case GE_CMODE_16BIT_ABGR5551: c = Conv5551(v); break;
				default: c = Conv4444(v); break;
				}
			}
			dst[i] = c;
		}
		__sync_synchronize();
		clutHash_ = clutHash;
		depalKey_ = 0;
	}

	const int tw = w * scale_, th = h * scale_;
	const uint32_t pitch = ((uint32_t)tw * 4 + 63) & ~63u;
	// Same source contents, palette and parameters as last time: reuse the result.
	const uint64_t key = XXH3_64bits(&addr, 4) ^ ((uint64_t)(uintptr_t)src << 1) ^ ((uint64_t)src->version << 32) ^
		((uint64_t)w << 52) ^ ((uint64_t)h << 40) ^ ((uint64_t)clutSlot_ << 24) ^ ((uint64_t)shift << 16) ^ ((uint64_t)mask << 8) ^ (uint64_t)base ^
		((uint64_t)fmt * 0x9E3779B97F4A7C15ull);
	if (key != depalKey_ || src == target_) {
		if (!depalMem_ || depalBytes_ < pitch * th) {
			if (depalMem_) DeferFree(depalMem_);
			depalMem_ = AllocLocal(pitch * th);
			depalBytes_ = depalMem_ ? pitch * th : 0;
			if (!depalMem_)
				return false;
		}
		FlushBatch();
		RSX::GpuWaitForRendering();
		RSX::Target t;
		t.colorOffset = RSX::LocalOffset(depalMem_);
		t.colorPitch = pitch;
		t.width = (uint16_t)tw;
		t.height = (uint16_t)th;
		t.hasDepth = false;
		RSX::SetTarget(t);
		RSX::SetPixelViewport(t.width, t.height);
		RSX::SetDefaultState();
	tlCull_ = 0;
		RSX::BindVertexProgram();
		const float zero[4] = {0, 0, 0, 0}, one[4] = {1, 1, 1, 1};
		if (anyShift) {
			// DEPALX: per channel the divisor to the format's bits and the weight in the value the
			// index comes from; TEX4.x = 2^shift within that value.
			float step[4], weight[4];
			if (src16) {
				static const float divisors[3][4] = {
					{8.0f, 4.0f, 8.0f, 256.0f},
					{8.0f, 8.0f, 8.0f, 128.0f},
					{16.0f, 16.0f, 16.0f, 16.0f},
				};
				static const float weights[3][4] = {
					{1.0f, 32.0f, 2048.0f, 0.0f},
					{1.0f, 32.0f, 1024.0f, 32768.0f},
					{1.0f, 16.0f, 256.0f, 4096.0f},
				};
				const int f = src->fmt == GE_FORMAT_565 ? 0 : src->fmt == GE_FORMAT_5551 ? 1 : 2;
				for (int c = 0; c < 4; ++c) {
					step[cal_.colorOrder[c]] = divisors[f][c];
					weight[cal_.colorOrder[c]] = weights[f][c];
				}
			} else {
				// 8888: the byte holding the index.
				for (int c = 0; c < 4; ++c) {
					step[c] = 1.0f;
					weight[c] = 0.0f;
				}
				weight[cal_.colorOrder[shift / 8]] = 1.0f;
			}
			const float params[4] = {(float)(1 << (src16 ? shift : (shift & 7))), 0.0f, 0.0f, 0.0f};
			RSX::SetVertexConstants(step, weight, params);
		} else if (src16) {
			// Per PSP channel (R, G, B, A): the divisor from 8 bits down to the format's bits (256:
			// unused), and the channel's weight in the 16-bit value. Texel component colorOrder[c]
			// holds channel c.
			static const float divisors[3][4] = {
				{8.0f, 4.0f, 8.0f, 256.0f},      // 565
				{8.0f, 8.0f, 8.0f, 128.0f},      // 5551
				{16.0f, 16.0f, 16.0f, 16.0f},    // 4444
			};
			static const float weights[3][4] = {
				{1.0f, 32.0f, 2048.0f, 0.0f},
				{1.0f, 32.0f, 1024.0f, 32768.0f},
				{1.0f, 16.0f, 256.0f, 4096.0f},
			};
			const int f = src->fmt == GE_FORMAT_565 ? 0 : src->fmt == GE_FORMAT_5551 ? 1 : 2;
			float step[4], weight[4];
			for (int c = 0; c < 4; ++c) {
				step[cal_.colorOrder[c]] = divisors[f][c];
				weight[cal_.colorOrder[c]] = weights[f][c];
			}
			const float params[4] = {(float)scale_, (shift & 8) ? 256.0f : 1.0f,
				(float)((x0 + pixel) * scale_) + 0.5f, 1.0f / src->w};
			RSX::SetVertexConstants(step, weight, params);
		} else {
			RSX::SetVertexConstants(zero, zero, one);
		}
		RSX::BindTexture(0, src->offset, src->w, src->h, src->pitch, false, true, true, false, false);
		RSX::BindTexture(1, RSX::LocalOffset(clutRing_ + clutSlot_ * 512), entries, 1, entries * 4, false, false, true, false, false);
		RSX::BindFragmentProgram(anyShift ? RSX::FRAGMENT_DEPALX : src16 ? RSX::FRAGMENT_DEPAL16 : RSX::FRAGMENT_DEPAL);
		RSX::InvalidateTextureCache();
		uint32_t first = 0;
		RSX::Vertex *v = RSX::AllocVertices(6, &first);
		if (v) {
			const float px[6] = {-1, 1, -1, 1, 1, -1}, py[6] = {-1, -1, 1, -1, 1, 1};
			const float u0 = (float)(x0 * scale_) / src->w, u1 = (float)((x0 + w) * scale_) / src->w;
			const float v0 = (float)(y0 * scale_) / src->h, v1 = (float)((y0 + h) * scale_) / src->h;
			const int channel = anyShift ? 0 : shift / 8;  // 0 = R ... 3 = A
			for (int i = 0; i < 6; ++i) {
				RSX::Vertex o;
				o.x = px[i];
				o.y = cal_.flipY ? -py[i] : py[i];
				o.z = 0.0f;
				o.w = 1.0f;
				for (int k = 0; k < 4; ++k)
					o.c0[k] = 0;
				o.c0[cal_.colorOrder[channel]] = 255;
				memset(o.c1, 0, 4);
				// 16-bit: x in output pixels (the program finds the source pixel).
				o.s = (src16 && !anyShift) ? (px[i] < 0 ? 0.0f : (float)tw) : (px[i] < 0 ? u0 : u1);
				o.t = py[i] < 0 ? v0 : v1;
				o.r = (float)base;
				o.q = 1.0f / entries;
				o.fog = (float)(mask + 1);
				o.pad = 0.0f;
				memcpy(&v[i], &o, sizeof(o));
			}
			__sync_synchronize();
			RSX::DrawVertices(GCM_TYPE_TRIANGLES, first, 6);
		}
		RSX::GpuWaitForRendering();
		RSX::InvalidateTextureCache();
		// Back to the game's target; ApplyState then re-sends the whole draw state.
		RSX::SetTarget(boundTarget_);
		RSX::SetPixelViewport(targetW_, targetH_);
		RSX::SetDefaultState();
	tlCull_ = 0;
		RSX::BindVertexProgram();
		vpTL_ = false;
		stateValid_ = false;
		depalKey_ = key;
		depalPasses_++;
	}
	depal_.offset = RSX::LocalOffset(depalMem_);
	depal_.pitch = pitch;
	depal_.w = (uint16_t)tw;
	depal_.h = (uint16_t)th;
	depal_.uvW = (uint16_t)w;
	depal_.uvH = (uint16_t)h;
	depal_.addr = addr;
	*out = &depal_;
	return true;
}

// The converted palette (512 entries, a 256-entry one repeated) and, for CLUT4/8, the 256 colors
// each index gives after the shift/mask/offset (as the software sampler's TransformClutIndex).
struct ClutTransform {
	bool on = false;
	uint32_t shift = 0, mask = 0xFF, offset = 0;
};
static bool BuildClut(const SamplerID &id, uint32_t pal[512], uint32_t small[256], ClutTransform *ct) {
	if (!id.cached.clut)
		return false;
	const int entries = id.ClutFmt() == GE_CMODE_32BIT_ABGR8888 ? 256 : 512;
	for (int i = 0; i < entries; ++i) {
		switch (id.ClutFmt()) {
		case GE_CMODE_16BIT_BGR5650: pal[i] = Conv565(id.cached.clut16[i]); break;
		case GE_CMODE_16BIT_ABGR5551: pal[i] = Conv5551(id.cached.clut16[i]); break;
		case GE_CMODE_16BIT_ABGR4444: pal[i] = Conv4444(id.cached.clut16[i]); break;
		default: pal[i] = Conv8888((const u8 *)&id.cached.clut32[i]); break;
		}
	}
	if (entries == 256)
		memcpy(pal + 256, pal, 256 * 4);
	ct->on = id.hasClutShift || id.hasClutMask || id.hasClutOffset;
	if (ct->on) {
		ct->shift = (id.cached.clutFormat >> 2) & 0x1F;
		ct->mask = (id.cached.clutFormat >> 8) & 0xFF;
		const uint32_t offsetMask = id.ClutFmt() == GE_CMODE_32BIT_ABGR8888 ? 0xFF : 0x1FF;
		ct->offset = (((id.cached.clutFormat >> 16) & 0x1F) << 4) & offsetMask;
	}
	for (int i = 0; i < 256; ++i)
		small[i] = pal[ct->on ? ((((uint32_t)i >> ct->shift) & ct->mask) | ct->offset) & 511 : (uint32_t)i];
	return true;
}

// The SPU decoder (TexSpu): the common formats with whole 8-row bands in memory.
static bool SpuDecode(const SamplerID &id, const u8 *src, int bufw, int w, int h, uint32_t bytes, uint32_t *dst, uint32_t dstPitch) {
	const GETextureFormat fmt = id.TexFmt();
	static const int bitsPerTexel[8] = {16, 16, 16, 32, 4, 8, 16, 32};
	if (fmt > GE_TFMT_CLUT32 || (w & 3) || w > 1024)
		return false;
	const uint32_t rowBytes = (uint32_t)bufw * bitsPerTexel[fmt] / 8;
	const uint32_t rows = id.swizzle ? ((uint32_t)h + 7) & ~7u : (uint32_t)h;
	if ((rowBytes & 15) || rowBytes > 4096 || (uint64_t)rowBytes * rows > bytes || (uint32_t)w * bitsPerTexel[fmt] / 8 > rowBytes)
		return false;
	if (!TexSpu::Available())
		return false;
	alignas(16) uint32_t pal[512];
	alignas(16) uint32_t small[256];
	ClutTransform ct;
	if (fmt >= GE_TFMT_CLUT4 && !BuildClut(id, pal, small, &ct))
		return false;
	TexSpu::DecodeRequest r;
	r.src = src;
	r.rowBytes = rowBytes;
	r.fmt = (uint32_t)fmt;
	r.swizzle = id.swizzle;
	r.w = (uint32_t)w;
	r.h = (uint32_t)h;
	r.dst = dst;
	r.dstPitch = dstPitch * 4;
	r.lut = (fmt == GE_TFMT_CLUT4 || fmt == GE_TFMT_CLUT8) ? small : pal;
	r.clutTransform = ct.on;
	r.clutShift = ct.shift;
	r.clutMask = ct.mask;
	r.clutOffset = ct.offset;
	return TexSpu::Decode(r);
}

bool RSXRenderer::FastDecode(const SamplerID &id, const u8 *src, int bufw, int w, int h, uint32_t bytes, uint32_t *dst, uint32_t dstPitch) {
	const GETextureFormat fmt = id.TexFmt();
	static const int bitsPerTexel[8] = {16, 16, 16, 32, 4, 8, 16, 32};
	if (fmt > GE_TFMT_CLUT32)
		return false;
	const uint32_t rowBytes = (uint32_t)bufw * bitsPerTexel[fmt] / 8;
	if (id.swizzle && (rowBytes & 15))
		return false;
	const bool swz = id.swizzle;

	// Converted palette for the CLUT formats, indexed after the clut shift/mask/offset.
	uint32_t pal[512];
	uint32_t small[256];
	ClutTransform ct;
	if (fmt >= GE_TFMT_CLUT4 && !BuildClut(id, pal, small, &ct))
		return false;
	auto clutIndex = [&](uint32_t index) -> uint32_t {
		return ct.on ? (((index >> ct.shift) & ct.mask) | ct.offset) & 511 : index & 0xFF;
	};

	for (int y = 0; y < h; ++y) {
		uint32_t *row = dst + y * dstPitch;
		for (int x = 0; x < w; ++x) {
			uint32_t c = 0;
			switch (fmt) {
			case GE_TFMT_5650: case GE_TFMT_5551: case GE_TFMT_4444: {
				const uint32_t o = TexelOffset(x * 2, y, rowBytes, swz);
				if (o + 2 > bytes) break;
				const uint32_t v = src[o] | (src[o + 1] << 8);
				c = fmt == GE_TFMT_5650 ? Conv565(v) : (fmt == GE_TFMT_5551 ? Conv5551(v) : Conv4444(v));
				break;
			}
			case GE_TFMT_8888: {
				const uint32_t o = TexelOffset(x * 4, y, rowBytes, swz);
				if (o + 4 > bytes) break;
				c = Conv8888(src + o);
				break;
			}
			case GE_TFMT_CLUT4: {
				const uint32_t o = TexelOffset(x >> 1, y, rowBytes, swz);
				if (o >= bytes) break;
				c = small[(src[o] >> ((x & 1) * 4)) & 15];
				break;
			}
			case GE_TFMT_CLUT8: {
				const uint32_t o = TexelOffset(x, y, rowBytes, swz);
				if (o >= bytes) break;
				c = small[src[o]];
				break;
			}
			case GE_TFMT_CLUT16: {
				const uint32_t o = TexelOffset(x * 2, y, rowBytes, swz);
				if (o + 2 > bytes) break;
				c = pal[clutIndex(src[o] | (src[o + 1] << 8))];
				break;
			}
			default: {
				const uint32_t o = TexelOffset(x * 4, y, rowBytes, swz);
				if (o + 4 > bytes) break;
				c = pal[clutIndex(src[o] | (src[o + 1] << 8) | (src[o + 2] << 16) | ((uint32_t)src[o + 3] << 24))];
				break;
			}
			}
			row[x] = c;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// State

bool RSXRenderer::SameAsMemo(const RasterizerState &state) const {
	alignas(16) RasterizerState n;
	memcpy((void *)&n, (const void *)&state, sizeof(n));
	NormalizeForHardware(n);
	n.cpuEpoch = memoCopy_.cpuEpoch;  // Texture memory is rechecked in ApplyState instead.
	if (memcmp(&n, &memoCopy_, sizeof(n)) != 0)
		return false;
	const uint8_t *clut = state.samplerID.cached.clut;
	if ((clut != nullptr) != memoHasClut_)
		return false;
	return !clut || memcmp(clut, memoClut_, sizeof(memoClut_)) == 0;
}

void RSXRenderer::ApplyState(const RasterizerState &state, bool clear) {
	clear = clear || state.pixelID.clearMode;
	const uint32_t scissorRegs[3] = {GS.scissor1, GS.scissor2, GS.region2};
	// (A fixed-colour subtract state needs Rect's destination coordinates: not for other primitives.)
	if (stateValid_ && memoClear_ == clear && memoFrame_ == frame_ && memoGen_ == vramGen_ && (!cur_.pad0 || fixSubAllowed_) &&
		memcmp(scissorRegs, memoScissor_, sizeof(scissorRegs)) == 0 && SameAsMemo(state)) {
		// The game may have changed texture memory since (a new CPU epoch): the same RSX copy?
		bool reuse = true;
		if ((state.cpuEpoch != memoCopy_.cpuEpoch || opt_.textureCheckFull) && cur_.tex) {
			TexEntry *tex = nullptr;
			reuse = state.enableTextures && LookupTexture(state, &tex) && tex && tex->offset == cur_.texOffset;
		}
		if (reuse) {
			memoCopy_.cpuEpoch = state.cpuEpoch;
			memoState_ = &state;
			memoHits_++;
			return;
		}
	}
	memoMisses_++;
	memoState_ = &state;
	memcpy((void *)&memoCopy_, (const void *)&state, sizeof(RasterizerState));
	NormalizeForHardware(memoCopy_);
	memoHasClut_ = state.samplerID.cached.clut != nullptr;
	if (memoHasClut_)
		memcpy(memoClut_, state.samplerID.cached.clut, sizeof(memoClut_));
	memoClear_ = clear;
	memoFrame_ = frame_;
	memoGen_ = vramGen_;
	memcpy(memoScissor_, scissorRegs, sizeof(scissorRegs));
	drawReadsRttCopy_ = false;
	const PixelFuncID &p = state.pixelID;
	const SamplerID &sid = state.samplerID;
	HwState s;
	memset(&s, 0, sizeof(s));

	const int scissorX1 = GS.getScissorX1(), scissorY1 = GS.getScissorY1();
	const int scissorX2 = std::min(GS.getScissorX2(), GS.getRegionX2());
	const int scissorY2 = std::min(GS.getScissorY2(), GS.getRegionY2());
	s.sx = (int16_t)(scissorX1 * scale_);
	s.sy = (int16_t)(scissorY1 * scale_);
	s.sw = (int16_t)std::max(0, (scissorX2 - scissorX1 + 1) * scale_);
	s.sh = (int16_t)std::max(0, (scissorY2 - scissorY1 + 1) * scale_);
	s.mul[0] = s.mul[1] = s.mul[2] = s.mul[3] = 1.0f;

	if (clear) {
		s.fragment = RSX::FRAGMENT_CLEAR;
		s.depthTest = 1;
		s.depthFunc = GCM_ALWAYS;
		s.depthWrite = p.DepthClear();
		s.colorMask = (p.ColorClear() ? (GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B) : 0) |
			(p.StencilClear() ? GCM_COLOR_MASK_A : 0);
		if (p.StencilClear()) {
			s.stencil = 1;
			s.sfunc = GCM_ALWAYS;
			s.sref = 0;  // Replaced per draw from the vertex alpha below.
			s.smask = 0xFF;
			s.swmask = 0xFF;
			s.sfail = s.szfail = s.szpass = GCM_REPLACE;
		}
	} else {
		// Texture.
		TexEntry *tex = nullptr;
		bool textured;
		{
			TbScope tl(tbLookup_);
			textured = state.enableTextures && LookupTexture(state, &tex);
		}
		if (textured) {
			s.tex = 1;
			s.texOffset = tex->offset;
			s.texW = tex->w;
			s.texH = tex->h;
			s.texPitch = tex->pitch;
			s.uvW = tex->uvW ? tex->uvW : tex->w;
			s.uvH = tex->uvH ? tex->uvH : tex->h;
			s.texLinMin = state.minFilt;
			s.texLinMag = state.magFilt;
			s.clampS = sid.clampS;
			s.clampT = sid.clampT;
			ColorToFloats(sid.cached.texBlendColor, 1.0f, s.env);
		}

		// Blending.
		bool blendMul = false;
		if (p.alphaBlend) {
			s.blend = 1;
			switch (p.AlphaBlendEq()) {
			case GE_BLENDMODE_MUL_AND_SUBTRACT: s.beq = GCM_FUNC_SUBTRACT; break;
			case GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE: s.beq = GCM_FUNC_REVERSE_SUBTRACT; break;
			case GE_BLENDMODE_MIN: s.beq = GCM_FUNC_MIN; break;
			case GE_BLENDMODE_MAX: s.beq = GCM_FUNC_MAX; break;
			case GE_BLENDMODE_ABSDIFF: s.beq = GCM_FUNC_SUBTRACT; break;  // No RSX equivalent; closest.
			default: s.beq = GCM_FUNC_ADD; break;
			}
			auto factor = [&](PixelBlendFactor f, bool isSrc) -> uint16_t {
				switch (f) {
				case PixelBlendFactor::OTHERCOLOR: return isSrc ? GCM_DST_COLOR : GCM_SRC_COLOR;
				case PixelBlendFactor::INVOTHERCOLOR: return isSrc ? GCM_ONE_MINUS_DST_COLOR : GCM_ONE_MINUS_SRC_COLOR;
				case PixelBlendFactor::SRCALPHA: case PixelBlendFactor::DOUBLESRCALPHA: return GCM_SRC_ALPHA;
				case PixelBlendFactor::INVSRCALPHA: case PixelBlendFactor::DOUBLEINVSRCALPHA: return GCM_ONE_MINUS_SRC_ALPHA;
				case PixelBlendFactor::DSTALPHA: case PixelBlendFactor::DOUBLEDSTALPHA: return GCM_DST_ALPHA;
				case PixelBlendFactor::INVDSTALPHA: case PixelBlendFactor::DOUBLEINVDSTALPHA: return GCM_ONE_MINUS_DST_ALPHA;
				case PixelBlendFactor::ZERO: return GCM_ZERO;
				case PixelBlendFactor::ONE: return GCM_ONE;
				case PixelBlendFactor::FIX:
				default:
					return GCM_CONSTANT_COLOR;
				}
			};
			const bool minmax = p.AlphaBlendEq() == GE_BLENDMODE_MIN || p.AlphaBlendEq() == GE_BLENDMODE_MAX;
			if (minmax) {
				s.bsrc = GCM_ONE;
				s.bdst = GCM_ONE;
			} else {
				s.bsrc = factor(p.AlphaBlendSrc(), true);
				s.bdst = factor(p.AlphaBlendDst(), false);
				// A 16-bit 565 framebuffer has no alpha: the PSP blends with a destination alpha of 0
				// (as PPSSPP's other backends and its software renderer do). Our copy of it is a 32-bit
				// RSX surface whose alpha is whatever was uploaded or cleared (often 255), so a blend
				// by destination alpha went far too bright. Frontend 96, for Silent Hill Origins'
				// white screen in the burning house (a guess: no dump of it yet).
				auto isDstA = [](PixelBlendFactor f) { return f == PixelBlendFactor::DSTALPHA || f == PixelBlendFactor::DOUBLEDSTALPHA; };
				auto isInvDstA = [](PixelBlendFactor f) { return f == PixelBlendFactor::INVDSTALPHA || f == PixelBlendFactor::DOUBLEINVDSTALPHA; };
				const bool usesDstA = isDstA(p.AlphaBlendSrc()) || isInvDstA(p.AlphaBlendSrc()) || isDstA(p.AlphaBlendDst()) || isInvDstA(p.AlphaBlendDst());
				if (usesDstA && dstAlphaLogs_ < 8) {
					dstAlphaLogs_++;
					RSXEventLog("RSX renderer: blend by destination alpha (src %d, dst %d) into framebuffer %08x format %d%s",
						(int)p.AlphaBlendSrc(), (int)p.AlphaBlendDst(), NormalizeAddr(GS.getFrameBufAddress()), (int)GS.FrameBufFormat(),
						GS.FrameBufFormat() == GE_FORMAT_565 ? " (16-bit, no alpha: blended as alpha 0)" : "");
				}
				if (GS.FrameBufFormat() == GE_FORMAT_565) {
					if (isDstA(p.AlphaBlendSrc())) s.bsrc = GCM_ZERO;
					else if (isInvDstA(p.AlphaBlendSrc())) s.bsrc = GCM_ONE;
					if (isDstA(p.AlphaBlendDst())) s.bdst = GCM_ZERO;
					else if (isInvDstA(p.AlphaBlendDst())) s.bdst = GCM_ONE;
				}
				if (p.AlphaBlendSrc() == PixelBlendFactor::FIX) {
					// Exact: multiply the source in the shader, keep the constant color for the other side.
					s.bsrc = GCM_ONE;
					blendMul = true;
					ColorToFloats(GS.getFixA(), 1.0f, s.mul);
				}
				if (p.AlphaBlendDst() == PixelBlendFactor::FIX)
					s.blendColor = BgrToArgb(GS.getFixB());
			}
		}

		const bool fog = p.applyFog;
		if (fog) ColorToFloats(p.cached.fogColor, 1.0f, s.fog);
		s.fragment = RSX::FragmentIndex(textured, sid.TexFunc(), sid.useTextureAlpha, sid.useColorDoubling, fog, blendMul);
		// Both blend factors fixed and a subtracting equation, drawing into the buffer the draw also
		// samples (Silent Hill Origins' bright-pass): the whole pixel in the fragment program, as
		// the GE computes it, with blending off. The RSX blender's rounding, a colour channel at a
		// time, turned results of 0-4 into coloured specks that the later glow passes multiplied
		// into colour blotches over the fog. The destination comes from the copy the draw samples.
		const GEBlendMode beqGE = p.AlphaBlendEq();
		if (fixSubAllowed_ && textured && p.alphaBlend && !fog && !p.applyLogicOp && !p.dithering &&
			p.AlphaBlendSrc() == PixelBlendFactor::FIX && p.AlphaBlendDst() == PixelBlendFactor::FIX &&
			(beqGE == GE_BLENDMODE_MUL_AND_SUBTRACT || beqGE == GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE) &&
			sid.TexFunc() == GE_TEXFUNC_MODULATE && drawReadsRttCopy_ && rttCopySrc_ == target_ &&
			rttCopyX0_ == 0 && rttCopyY0_ == 0) {
			s.fragment = RSX::FragmentFixSub(sid.useTextureAlpha, sid.useColorDoubling, beqGE == GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE);
			s.blend = 0;
			s.pad0 = 1;  // Rect fills in the destination coordinates.
			const uint32_t fa = GS.getFixA(), fb = GS.getFixB();
			for (int i = 0; i < 3; ++i) {
				s.mul[i] = (float)(2 * ((fa >> (8 * i)) & 0xFF) + 1);
				s.fog[i] = (float)(2 * ((fb >> (8 * i)) & 0xFF) + 1);
			}
			s.mul[3] = s.fog[3] = 1.0f;
			if (fixSubLogs_ < 2) {
				fixSubLogs_++;
				RSXEventLog("RSX renderer: fixed-colour subtract blend (fix %06x %06x) into %08x done in the fragment program",
					fa, fb, target_ ? target_->addr : 0);
			}
		}

		// Depth.
		s.depthTest = 1;
		s.depthFunc = CompareFunc(p.DepthTestFunc());
		s.depthWrite = p.depthWrite;

		// Alpha test.
		if (p.AlphaTestFunc() != GE_COMP_ALWAYS) {
			s.alphaTest = 1;
			s.alphaFunc = CompareFunc(p.AlphaTestFunc());
			s.alphaRef = p.alphaTestRef;
		}

		// Stencil (the PSP keeps it in framebuffer alpha; the RSX copy keeps it in the depth buffer).
		if (p.stencilTest) {
			s.stencil = 1;
			s.sfunc = CompareFunc(p.StencilTestFunc());
			s.sref = (uint8_t)GS.getStencilTestRef();
			s.smask = (uint8_t)GS.getStencilTestMask();
			s.swmask = (uint8_t)~GS.getStencilWriteMask();
			s.sfail = StencilOp(p.SFail());
			s.szfail = StencilOp(p.ZFail());
			s.szpass = StencilOp(p.ZPass());
		}

		// Color write mask: a channel is written unless all of its bits are masked.
		// Framebuffer alpha is the PSP's stencil: drawing leaves it alone unless the stencil test is
		// on (then the RSX writes the fragment's alpha, the closest it can do). Writing it anyway
		// broke games that later read it back: Dead or Alive Paradise draws every character mesh
		// again as lines textured from the framebuffer, which only stay invisible because its
		// alpha (stencil) is 0 there - with alpha written they showed up as a black/white lattice.
		const uint32_t mask = GS.getColorMask();
		s.colorMask = ((mask & 0x0000FF) != 0x0000FF ? GCM_COLOR_MASK_R : 0) |
			((mask & 0x00FF00) != 0x00FF00 ? GCM_COLOR_MASK_G : 0) |
			((mask & 0xFF0000) != 0xFF0000 ? GCM_COLOR_MASK_B : 0) |
			((p.stencilTest && (mask >> 24) != 0xFF && GS.FrameBufFormat() != GE_FORMAT_565) ? GCM_COLOR_MASK_A : 0);

		if (p.applyLogicOp) {
			s.logic = 1;
			s.logicOp = (uint16_t)(0x1500 + (GS.getLogicOp() & 15));
		}
		s.flat = !state.shadeGouraud;
	}

	if (stateValid_ && memcmp(&s, &cur_, sizeof(s)) == 0) return;
	FlushBatch();
	gcmContextData *ctx = RSX::Context();
	const bool full = !stateValid_;
	auto changed = [&](const void *a, const void *b, size_t n) { return full || memcmp(a, b, n) != 0; };

	if (changed(&s.fragment, &cur_.fragment, sizeof(s.fragment)))
		RSX::BindFragmentProgram(s.fragment);
	if (s.tex && (full || !cur_.tex || s.texOffset != cur_.texOffset || s.texW != cur_.texW || s.texH != cur_.texH ||
			s.texPitch != cur_.texPitch || s.texLinMin != cur_.texLinMin || s.texLinMag != cur_.texLinMag ||
			s.clampS != cur_.clampS || s.clampT != cur_.clampT))
		RSX::BindTexture(0, s.texOffset, s.texW, s.texH, s.texPitch, s.texLinMin, s.clampS, s.clampT, false, s.texLinMag);
	if (changed(s.env, cur_.env, sizeof(float) * 12))
		RSX::SetVertexConstants(s.env, s.fog, s.mul);
	if (changed(&s.blend, &cur_.blend, 1) || changed(&s.bsrc, &cur_.bsrc, 6) || changed(&s.blendColor, &cur_.blendColor, 4)) {
		rsxSetBlendEnable(ctx, s.blend);
		if (s.blend) {
			rsxSetBlendFunc(ctx, s.bsrc, s.bdst, GCM_ONE, GCM_ZERO);
			rsxSetBlendEquation(ctx, s.beq, GCM_FUNC_ADD);
			rsxSetBlendColor(ctx, s.blendColor, 0);
		}
	}
	if (changed(&s.depthTest, &cur_.depthTest, 1) || changed(&s.depthFunc, &cur_.depthFunc, 2) || changed(&s.depthWrite, &cur_.depthWrite, 1)) {
		rsxSetDepthTestEnable(ctx, s.depthTest);
		rsxSetDepthFunc(ctx, s.depthFunc);
		rsxSetDepthWriteEnable(ctx, s.depthWrite);
	}
	if (changed(&s.alphaTest, &cur_.alphaTest, 1) || changed(&s.alphaFunc, &cur_.alphaFunc, 2) || changed(&s.alphaRef, &cur_.alphaRef, 1)) {
		rsxSetAlphaTestEnable(ctx, s.alphaTest);
		if (s.alphaTest) rsxSetAlphaFunc(ctx, s.alphaFunc, s.alphaRef);
	}
	if (changed(&s.stencil, &cur_.stencil, 1) || changed(&s.sfunc, &cur_.sfunc, 2) || changed(&s.sfail, &cur_.sfail, 6) ||
		changed(&s.sref, &cur_.sref, 3)) {
		rsxSetStencilTestEnable(ctx, s.stencil);
		if (s.stencil) {
			rsxSetStencilFunc(ctx, s.sfunc, s.sref, s.smask);
			rsxSetStencilMask(ctx, s.swmask);
			rsxSetStencilOp(ctx, s.sfail, s.szfail, s.szpass);
		}
	}
	if (changed(&s.colorMask, &cur_.colorMask, 4))
		rsxSetColorMask(ctx, s.colorMask);
	if (changed(&s.logic, &cur_.logic, 1) || changed(&s.logicOp, &cur_.logicOp, 2)) {
		rsxSetLogicOpEnable(ctx, s.logic);
		if (s.logic) rsxSetLogicOp(ctx, s.logicOp);
	}
	if (changed(&s.flat, &cur_.flat, 1))
		rsxSetShadeModel(ctx, s.flat ? GCM_SHADE_MODEL_FLAT : GCM_SHADE_MODEL_SMOOTH);
	if (changed(&s.sx, &cur_.sx, 8))
		RSX::SetScissor(s.sx, s.sy, s.sw, s.sh);
	cur_ = s;
	stateValid_ = true;
}

// ---------------------------------------------------------------------------------------------
// Primitives

void RSXRenderer::BeginPrim(uint32_t type) {
	if (type != batchType_ || batch_.size() + 6 > MAX_BATCH) {
		FlushBatch();
		batchType_ = type;
	}
}

void RSXRenderer::FlushBatch() {
	RSX::StepQuiet("renderer: draw batch");
	if (batch_.empty()) return;
	UseMainVertexProgram();
	uint32_t first = 0;
	RSX::Vertex *dst = RSX::AllocVertices((int)batch_.size(), &first);
	if (dst) {
		memcpy(dst, batch_.data(), batch_.size() * sizeof(RSX::Vertex));
		__sync_synchronize();
		RSX::DrawVertices(batchType_, first, (uint32_t)batch_.size());
		draws_++;
	}
	batch_.clear();
}

void RSXRenderer::PushVertex(const VertexData &v, bool through, float sScale, float tScale, bool proj) {
	RSX::Vertex o;
	const float w = through || !(v.clipw > 0.0f) ? 1.0f : v.clipw;
	const float px = v.screenpos.x * (1.0f / SCREEN_SCALE_FACTOR) * scale_;
	const float py = v.screenpos.y * (1.0f / SCREEN_SCALE_FACTOR) * scale_;
	o.x = (2.0f * px / targetW_ - 1.0f) * w;
	o.y = (cal_.flipY ? 1.0f - 2.0f * py / targetH_ : 2.0f * py / targetH_ - 1.0f) * w;
	o.z = (2.0f * (v.screenpos.z * (1.0f / 65535.0f)) - 1.0f) * w;
	o.w = w;
	const uint32_t c0 = v.color0, c1 = v.color1 & 0x00FFFFFF;
	for (int i = 0; i < 4; ++i) {
		// color0/color1 hold R in the low byte; colorOrder says which channel each byte must carry.
		o.c0[i] = (uint8_t)(c0 >> (8 * cal_.colorOrder[i]));
		o.c1[i] = (uint8_t)(c1 >> (8 * cal_.colorOrder[i]));
	}
	o.s = v.texturecoords.s() * sScale;
	o.t = v.texturecoords.t() * tScale;
	o.r = 0.0f;
	o.q = proj ? v.texturecoords.q() : 1.0f;
	o.fog = v.fogdepth;
	o.pad = 0.0f;
	batch_.push_back(o);
}

void RSXRenderer::Triangle(const RasterizerState &state, const VertexData &v0, const VertexData &v1, const VertexData &v2) {
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return;
	if (state.throughMode)
		texRowsHint_ = TexRowsSampled(std::min(v0.texturecoords.t(), std::min(v1.texturecoords.t(), v2.texturecoords.t())),
			std::max(v0.texturecoords.t(), std::max(v1.texturecoords.t(), v2.texturecoords.t())));
	ApplyState(state, false);
	texRowsHint_ = 0;
	BeginPrim(GCM_TYPE_TRIANGLES);
	const bool through = state.throughMode;
	const float ss = through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	const bool proj = state.textureProj && !through;
	PushVertex(v0, through, ss, ts, proj);
	PushVertex(v1, through, ss, ts, proj);
	PushVertex(v2, through, ss, ts, proj);
	{
		const float k = 1.0f / SCREEN_SCALE_FACTOR;
		const float xa = std::min(v0.screenpos.x, std::min(v1.screenpos.x, v2.screenpos.x)) * k;
		const float xb = std::max(v0.screenpos.x, std::max(v1.screenpos.x, v2.screenpos.x)) * k;
		const float ya = std::min(v0.screenpos.y, std::min(v1.screenpos.y, v2.screenpos.y)) * k;
		const float yb = std::max(v0.screenpos.y, std::max(v1.screenpos.y, v2.screenpos.y)) * k;
		MarkTargetDrawn((int)floorf(xa), (int)ceilf(xb) + 1, (int)floorf(ya), (int)ceilf(yb));
	}
	prims_++;
}

// Batched path: PSP screen space (homogeneous) -> our NDC, same mapping as PushVertex.
bool RSXRenderer::DrawTriangles(const RasterizerState &state, const FastVertex *verts, int count, bool through) {
	if (!opt_.batched) return false;
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return true;  // Nothing to draw into; like the regular path, drop it.
	if (through && count > 0) {
		float tMin = verts[0].t, tMax = verts[0].t;
		for (int i = 1; i < count; ++i) {
			tMin = std::min(tMin, verts[i].t);
			tMax = std::max(tMax, verts[i].t);
		}
		texRowsHint_ = TexRowsSampled(tMin, tMax);
	}
	ApplyState(state, false);
	texRowsHint_ = 0;
	const float ss = through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	const bool proj = state.textureProj && !through;
	const float kx = 2.0f * scale_ / targetW_, ky = 2.0f * scale_ / targetH_, kz = 2.0f / 65535.0f;
	for (int i = 0; i + 2 < count; i += 3) {
		BeginPrim(GCM_TYPE_TRIANGLES);
		for (int j = 0; j < 3; ++j) {
			const FastVertex &v = verts[i + j];
			RSX::Vertex o;
			o.x = kx * v.x - v.w;
			o.y = cal_.flipY ? v.w - ky * v.y : ky * v.y - v.w;
			o.z = kz * v.z - v.w;
			o.w = v.w;
			const uint32_t c0 = v.color0, c1 = v.color1 & 0x00FFFFFF;
			for (int k = 0; k < 4; ++k) {
				o.c0[k] = (uint8_t)(c0 >> (8 * cal_.colorOrder[k]));
				o.c1[k] = (uint8_t)(c1 >> (8 * cal_.colorOrder[k]));
			}
			o.s = v.s * ss;
			o.t = v.t * ts;
			o.r = 0.0f;
			o.q = proj ? v.q : 1.0f;
			o.fog = v.fog;
			o.pad = 0.0f;
			batch_.push_back(o);
		}
		prims_++;
	}
	if (through && count >= 3) {
		float xa = verts[0].x, xb = verts[0].x, ya = verts[0].y, yb = verts[0].y;
		for (int i = 1; i < count; ++i) {
			xa = std::min(xa, verts[i].x);
			xb = std::max(xb, verts[i].x);
			ya = std::min(ya, verts[i].y);
			yb = std::max(yb, verts[i].y);
		}
		MarkTargetDrawn((int)floorf(xa), (int)ceilf(xb) + 1, (int)floorf(ya), (int)ceilf(yb));
	} else {
		MarkTargetDrawn();
	}
	batchedDraws_++;
	return true;
}

bool RSXRenderer::DrawIndexed(const RasterizerState &state, const FastVertex *verts, int nverts, const uint16_t *indices, int nidx, bool through) {
	if (!opt_.batched || nverts > 20000) return false;
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return true;
	ApplyState(state, false);
	FlushBatch();
	UseMainVertexProgram();
	uint32_t first = 0;
	RSX::Vertex *dst = RSX::AllocVertices(nverts, &first);
	if (!dst) return false;
	const float ss = through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	const bool proj = state.textureProj && !through;
	const float kx = 2.0f * scale_ / targetW_, ky = 2.0f * scale_ / targetH_, kz = 2.0f / 65535.0f;
	const bool flipY = cal_.flipY;
	const unsigned char *order = cal_.colorOrder;
	const bool rgbaOrder = order[0] == 0 && order[1] == 1 && order[2] == 2 && order[3] == 3;
	for (int i = 0; i < nverts; ++i) {
		const FastVertex &v = verts[i];
		RSX::Vertex o;
		o.x = kx * v.x - v.w;
		o.y = flipY ? v.w - ky * v.y : ky * v.y - v.w;
		o.z = kz * v.z - v.w;
		o.w = v.w;
		const uint32_t c0 = v.color0, c1 = v.color1 & 0x00FFFFFF;
		if (rgbaOrder) {
			// color0 holds R in the low byte; the vertex wants R, G, B, A in memory order.
			const uint32_t s0 = __builtin_bswap32(c0), s1 = __builtin_bswap32(c1);
			memcpy(o.c0, &s0, 4);
			memcpy(o.c1, &s1, 4);
		} else {
			for (int k = 0; k < 4; ++k) {
				o.c0[k] = (uint8_t)(c0 >> (8 * order[k]));
				o.c1[k] = (uint8_t)(c1 >> (8 * order[k]));
			}
		}
		o.s = v.s * ss;
		o.t = v.t * ts;
		o.r = 0.0f;
		o.q = proj ? v.q : 1.0f;
		o.fog = v.fog;
		o.pad = 0.0f;
		memcpy(&dst[i], &o, sizeof(o));
	}
	uint32_t indexOffset = 0;
	uint32_t *ib = RSX::AllocIndices(nidx, &indexOffset);
	if (!ib) return true;
	for (int i = 0; i < nidx; ++i)
		ib[i] = first + indices[i];
	__sync_synchronize();
	RSX::DrawIndexed(GCM_TYPE_TRIANGLES, indexOffset, (uint32_t)nidx);
	draws_++;
	prims_ += nidx / 3;
	MarkTargetDrawn();
	batchedDraws_++;
	return true;
}

void RSXRenderer::UseMainVertexProgram() {
	if (!vpTL_) return;
	if (tlCull_) {
		rsxSetCullFaceEnable(RSX::Context(), GCM_FALSE);
		tlCull_ = 0;
	}
	RSX::BindVertexProgram();
	RSX::BindFragmentProgram(cur_.fragment);
	vpTL_ = false;
}

// Vertices into the ring. The source is usually fresh SPU output (not in the PPU's cache) and the
// ring lines aren't either: plain memcpy waited on both, a load for every destination line it was
// about to overwrite whole. Here: whole 128-byte destination lines are zeroed in the cache first
// (dcbz, no memory read) and the source is prefetched ahead.
static void CopyToRing(void *dst, const void *src, uint32_t bytes) {
#if defined(__ALTIVEC__)
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *sp = (const uint8_t *)src;
	// dcbz only on cacheable memory: the ring in RSX memory (Vertex data: RSX memory) is mapped
	// cache-inhibited, and dcbz there is an alignment exception (the game crashed).
	if ((((uintptr_t)d | (uintptr_t)sp) & 15) != 0 || bytes < 256 || !RSX::VertexRingInMain()) {
		memcpy(dst, src, bytes);
		return;
	}
	uint8_t *const end = d + bytes;
	// Up to the first whole line.
	while (((uintptr_t)d & 127) != 0 && d + 16 <= end) {
		__builtin_memcpy(d, sp, 16);
		d += 16;
		sp += 16;
	}
	while (d + 128 <= end) {
		__asm__ volatile("dcbt 0,%0" : : "r"(sp + 512));
		__asm__ volatile("dcbz 0,%0" : : "r"(d) : "memory");
		const __vector unsigned char *vs = (const __vector unsigned char *)sp;
		__vector unsigned char *vd = (__vector unsigned char *)d;
		const __vector unsigned char a0 = vs[0], a1 = vs[1], a2 = vs[2], a3 = vs[3];
		const __vector unsigned char a4 = vs[4], a5 = vs[5], a6 = vs[6], a7 = vs[7];
		vd[0] = a0; vd[1] = a1; vd[2] = a2; vd[3] = a3;
		vd[4] = a4; vd[5] = a5; vd[6] = a6; vd[7] = a7;
		d += 128;
		sp += 128;
	}
	if (d < end)
		memcpy(d, sp, end - d);
#else
	memcpy(dst, src, bytes);
#endif
}

bool RSXRenderer::DrawTL(const RasterizerState &state, const TLParams &p, const TLVertex *verts, int nverts, const uint16_t *indices, int nidx, int cull) {
	if (!SupportsTL() || nverts > 20000) return false;
#ifdef PPSSPP_RSX_HOSTSIM
	if (getenv("PPSSPP_TLDRAWLOG")) {
		// Each triangle as its three vertices (independent of how vertices and indices are packed).
		uint64_t h = 1469598103934665603ull;
		auto mix = [&](const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull; };
		for (int i = 0; i < nidx; ++i) mix(&verts[indices[i]], sizeof(TLVertex));
		uint64_t hp = h; h = 1469598103934665603ull; mix(&p, sizeof(p));
		uint64_t hs = h; h = 1469598103934665603ull;
		RasterizerState n = state; NormalizeForHardware(n); n.cpuEpoch = 0; n.texptr[0] = nullptr; mix(&n, sizeof(n));  // (host pointer: differs per run)
		if (const char *all = getenv("PPSSPP_TLSTATEALL")) {
			static FILE *fa = fopen(all, "wb");
			if (fa) fwrite(&n, sizeof(n), 1, fa);
		}
		static int dumped;
		if (getenv("PPSSPP_TLSTATEDUMP") && dumped++ < 3) {
			char name[64]; snprintf(name, sizeof(name), "%s.%d", getenv("PPSSPP_TLSTATEDUMP"), dumped);
			FILE *f = fopen(name, "wb"); if (f) { fwrite(&n, sizeof(n), 1, f); fclose(f); }
		}
		fprintf(stderr, "TLDRAW tex %08x nidx %d cull %d verts %016llx params %016llx state %016llx\n", state.texaddr[0], nidx, cull,
			(unsigned long long)hp, (unsigned long long)hs, (unsigned long long)h);
	}
#endif
	const bool lines = cull == 3;  // A line list: no culling.
	if (lines) cull = 0;
	TbScope tb(tbBackend_);
	uint64_t t0 = TimeBase();
	if (!EnsureTarget()) return true;
	const uint64_t tA = TimeBase();
	ApplyState(state, false);
	const uint64_t tB = TimeBase();
	tbTLTarget_ += tA - t0;
	tbTLApply_ += tB - tA;
	FlushBatch();
	if (!vpTL_) {
		if (!RSX::BindVertexProgramTL()) return false;
		RSX::BindFragmentProgram(cur_.fragment);
		vpTL_ = true;
	}
	uint64_t t1 = TimeBase();
	tbTLState_ += t1 - t0;
	float rows[4][4];
	FoldTLMatrix(p, 2.0f * scale_ / targetW_, 2.0f * scale_ / targetH_, 2.0f / 65535.0f, false, rows);
	float block[Rasterizer::TL_CONST_COUNT][4];
	memcpy(block, p.c, sizeof(block));
	memcpy(block, rows, sizeof(rows));
	if (!tlConstValid_) {
		RSX::SetVertexConstantBlock(3, Rasterizer::TL_CONST_COUNT, &block[0][0]);
		memcpy(tlConst_, block, sizeof(block));
		tlConstValid_ = true;
		tlConstUploads_++;
	} else {
		// Only the registers that changed (between objects, usually just the 4 matrix rows):
		// 688 bytes of constants per draw was a lot of FIFO traffic for small draws.
		int lo = 0, hi = Rasterizer::TL_CONST_COUNT - 1;
		while (lo <= hi && memcmp(block[lo], tlConst_[lo], 16) == 0) lo++;
		while (hi >= lo && memcmp(block[hi], tlConst_[hi], 16) == 0) hi--;
		if (lo <= hi) {
			RSX::SetVertexConstantBlock(3 + lo, hi - lo + 1, &block[lo][0]);
			memcpy(tlConst_[lo], block[lo], (hi - lo + 1) * 16);
			tlConstUploads_++;
		}
	}
	uint64_t t2 = TimeBase();
	tbTLConst_ += t2 - t1;
	const uint32_t bytes = (uint32_t)nverts * sizeof(TLVertex);
	uint32_t base = 0;
	RSX::TLVertex *mem = RSX::AllocTLVertices(nverts, &base);
	if (!mem) return false;
	CopyToRing(mem, verts, bytes);
	uint32_t indexOffset = 0;
	uint32_t *ib = RSX::AllocIndices(nidx, &indexOffset);
	if (!ib) return true;
	for (int i = 0; i < nidx; ++i)
		ib[i] = base + indices[i];
	__sync_synchronize();
	uint64_t t3 = TimeBase();
	tbTLCopy_ += t3 - t2;
	gcmContextData *ctx = RSX::Context();
	if (cull != tlCull_) {
		// cull 1 keeps a positive PSP screen cross product, 2 a negative one. Left set for the
		// next T&L draw; the other paths turn it off (UseMainVertexProgram, SetDefaultState).
		if (cull) {
			const bool keepCCW = (cull == 1) == tlPositiveIsCCW_;
			rsxSetFrontFace(ctx, keepCCW ? GCM_FRONTFACE_CCW : GCM_FRONTFACE_CW);
			rsxSetCullFace(ctx, GCM_CULL_BACK);
			if (!tlCull_)
				rsxSetCullFaceEnable(ctx, GCM_TRUE);
		} else {
			rsxSetCullFaceEnable(ctx, GCM_FALSE);
		}
		tlCull_ = cull;
	}
	RSX::BindTLAttribsRing();
	RSX::DrawIndexedBound(lines ? GCM_TYPE_LINES : GCM_TYPE_TRIANGLES, indexOffset, (uint32_t)nidx);
	draws_++;
	prims_ += lines ? nidx / 2 : nidx / 3;
	MarkTargetDrawn();
	batchedDraws_++;
	tlDraws_++;
	tbTLSubmit_ += TimeBase() - t3;
	return true;
}

void RSXRenderer::Rect(const RasterizerState &state, const VertexData &v0, const VertexData &v1, bool clear) {
	TbScope tb(tbBackend_);
	// A clear-mode rectangle with dithering on comes as a plain rectangle (the software renderer's
	// fast clear can't dither), but it's still a clear: its pixel state has no stencil test or
	// depth test of its own (as a normal draw, stencil "never" made it draw nothing - Gran Turismo's
	// depth clear, so the sky failed the depth test and stayed black).
	clear = clear || state.pixelID.clearMode;
	if (!EnsureTarget()) return;
	if (state.throughMode)
		texRowsHint_ = TexRowsSampled(std::min(v0.texturecoords.t(), v1.texturecoords.t()), std::max(v0.texturecoords.t(), v1.texturecoords.t()));
	fixSubAllowed_ = state.throughMode && !clear;
	ApplyState(state, clear);
	fixSubAllowed_ = false;
	texRowsHint_ = 0;
	if (clear && cur_.stencil) {
		// Clear mode writes the clear color's alpha as the stencil value.
		const uint8_t ref = (uint8_t)(v1.color0 >> 24);
		if (ref != cur_.sref) {
			FlushBatch();
			rsxSetStencilFunc(RSX::Context(), GCM_ALWAYS, ref, 0xFF);
			cur_.sref = ref;
		}
	}
	BeginPrim(GCM_TYPE_TRIANGLES);
	const bool through = state.throughMode;
	const float ss = !clear && through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = !clear && through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	// Corners take position and texture coordinates from both vertices; everything else from v1.
	VertexData c[4] = {v1, v1, v1, v1};
	c[0].screenpos.x = v0.screenpos.x; c[0].screenpos.y = v0.screenpos.y;
	c[0].texturecoords = Vec3Packedf(v0.texturecoords.s(), v0.texturecoords.t(), 1.0f);
	c[1].screenpos.x = v1.screenpos.x; c[1].screenpos.y = v0.screenpos.y;
	c[1].texturecoords = Vec3Packedf(v1.texturecoords.s(), v0.texturecoords.t(), 1.0f);
	c[2].screenpos.x = v0.screenpos.x; c[2].screenpos.y = v1.screenpos.y;
	c[2].texturecoords = Vec3Packedf(v0.texturecoords.s(), v1.texturecoords.t(), 1.0f);
	c[3].texturecoords = Vec3Packedf(v1.texturecoords.s(), v1.texturecoords.t(), 1.0f);
	const int order[6] = {0, 1, 2, 1, 3, 2};
	for (int i : order)
		PushVertex(c[i], true, ss, ts, false);
	if (cur_.pad0 && cur_.tex) {
		// Fixed-colour subtract blend in the fragment program: the destination pixel, read from the
		// target's copy (texture 0), at this vertex's screen position (normalized like through-mode
		// texture coordinates).
		const float du = 1.0f / (SCREEN_SCALE_FACTOR * cur_.uvW), dv = 1.0f / (SCREEN_SCALE_FACTOR * cur_.uvH);
		for (int k = 0; k < 6; ++k) {
			RSX::Vertex &o = batch_[batch_.size() - 6 + k];
			o.r = c[order[k]].screenpos.x * du;
			o.fog = c[order[k]].screenpos.y * dv;
		}
	}
	{
		const float xa = std::min(v0.screenpos.x, v1.screenpos.x) * (1.0f / SCREEN_SCALE_FACTOR);
		const float xb = std::max(v0.screenpos.x, v1.screenpos.x) * (1.0f / SCREEN_SCALE_FACTOR);
		const float ya = std::min(v0.screenpos.y, v1.screenpos.y) * (1.0f / SCREEN_SCALE_FACTOR);
		const float yb = std::max(v0.screenpos.y, v1.screenpos.y) * (1.0f / SCREEN_SCALE_FACTOR);
		MarkTargetDrawn((int)floorf(xa), (int)ceilf(xb) + 1, (int)floorf(ya), (int)ceilf(yb));
	}
	prims_++;
}

void RSXRenderer::Line(const RasterizerState &state, const VertexData &v0, const VertexData &v1) {
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return;
	ApplyState(state, false);
	BeginPrim(GCM_TYPE_LINES);
	const bool through = state.throughMode;
	const float ss = through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	PushVertex(v0, through, ss, ts, false);
	PushVertex(v1, through, ss, ts, false);
	if (through) {
		const float xa = std::min(v0.screenpos.x, v1.screenpos.x) * (1.0f / SCREEN_SCALE_FACTOR);
		const float xb = std::max(v0.screenpos.x, v1.screenpos.x) * (1.0f / SCREEN_SCALE_FACTOR);
		const float ya = std::min(v0.screenpos.y, v1.screenpos.y) * (1.0f / SCREEN_SCALE_FACTOR);
		const float yb = std::max(v0.screenpos.y, v1.screenpos.y) * (1.0f / SCREEN_SCALE_FACTOR);
		MarkTargetDrawn((int)floorf(xa), (int)ceilf(xb) + 1, (int)floorf(ya), (int)ceilf(yb) + 1);
	} else {
		MarkTargetDrawn();
	}
	prims_++;
}

void RSXRenderer::Point(const RasterizerState &state, const VertexData &v0) {
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return;
	ApplyState(state, false);
	if (batchType_ != GCM_TYPE_POINTS) {
		FlushBatch();
		rsxSetPointSize(RSX::Context(), (float)scale_);
	}
	BeginPrim(GCM_TYPE_POINTS);
	const bool through = state.throughMode;
	const float ss = through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	PushVertex(v0, through, ss, ts, false);
	if (through) {
		const int x = (int)floorf(v0.screenpos.x * (1.0f / SCREEN_SCALE_FACTOR));
		const int y = (int)floorf(v0.screenpos.y * (1.0f / SCREEN_SCALE_FACTOR));
		MarkTargetDrawn(x, x + 2, y, y + 1);
	} else {
		MarkTargetDrawn();
	}
	prims_++;
}

// ---------------------------------------------------------------------------------------------
// Presentation

static void DrawGlyphs(uint32_t *buf, const std::string &line, int chars) {
	for (int i = 0; i < chars; ++i) {
		unsigned c = (unsigned char)line[i];
		uint32_t color = 0xFFFFFFFF;
		if (c == '>') color = 0xFF40E0FF;  // The in-game options cursor.
		if (c < 32 || c > 126) c = '?';
		c -= 32;
		const int gx = (c % 16) * 12, gy = (c / 16) * 24;
		for (int row = 0; row < 24; ++row) {
			const uint8_t *glyph = g_font + (gy + row) * 192 + gx;
			uint32_t *out = buf + row * 1024 + 4 + i * 12;
			for (int col = 0; col < 12; ++col)
				if (glyph[col]) out[col] = color;
		}
	}
}

void RSXRenderer::DrawOverlay(const std::string &text, int x, int y) {
	if (!g_font || text.empty()) return;
	if (text.find('\n') != std::string::npos) {
		// Several lines (the in-game options): drawn through staging_, which holds 21 lines.
		// Only while the game is paused, so the wait for the RSX costs nothing.
		std::vector<std::string> lines;
		size_t start = 0;
		while (start <= text.size() && lines.size() < 512 / 24) {
			const size_t end = text.find('\n', start);
			lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (end == std::string::npos) break;
			start = end + 1;
		}
		int chars = 0;
		for (const auto &l : lines) chars = std::max(chars, (int)l.size());
		chars = std::min(chars, 1024 / 12 - 1);
		const int w = chars * 12 + 8, h = (int)lines.size() * 24;
		RSX::Finish();
		for (int row = 0; row < h; ++row) {
			uint32_t *out = staging_ + row * 1024;
			for (int i = 0; i < w; ++i) out[i] = 0xE0101820;
		}
		for (size_t i = 0; i < lines.size(); ++i)
			DrawGlyphs(staging_ + i * 24 * 1024, lines[i], std::min<int>((int)lines[i].size(), chars));
		__sync_synchronize();
		const int maxW = std::max(0, (int)RSX::DisplayWidth() - x), maxH = std::max(0, (int)RSX::DisplayHeight() - y);
		const int dw = std::min(w, maxW), dh = std::min(h, maxH);
		if (dw > 0 && dh > 0)
			RSX::ScaleImage(RSX::Mem::Main, RSX::IoOffset(staging_), 1024 * 4, dw, dh,
				RSX::Mem::Local, RSX::BackBufferOffset(), RSX::DisplayPitch(), x, y, dw, dh, false);
		return;
	}
	const int chars = std::min<int>((int)text.size(), 1024 / 12 - 1);
	const int w = chars * 12 + 8, h = 24;
	// The RSX may still be copying the previous frame's overlay; one frame of latency is plenty.
	for (int row = 0; row < h; ++row) {
		uint32_t *out = overlayBuf_ + row * 1024;
		for (int i = 0; i < w; ++i) out[i] = 0xC0101820;
	}
	for (int i = 0; i < chars; ++i) {
		unsigned c = (unsigned char)text[i];
		if (c < 32 || c > 126) c = '?';
		c -= 32;
		const int gx = (c % 16) * 12, gy = (c / 16) * 24;
		for (int row = 0; row < 24; ++row) {
			const uint8_t *glyph = g_font + (gy + row) * 192 + gx;
			uint32_t *out = overlayBuf_ + row * 1024 + 4 + i * 12;
			for (int col = 0; col < 12; ++col)
				if (glyph[col]) out[col] = 0xFFFFFFFF;
		}
	}
	__sync_synchronize();
	RSX::ScaleImage(RSX::Mem::Main, RSX::IoOffset(overlayBuf_), 1024 * 4, w, h,
		RSX::Mem::Local, RSX::BackBufferOffset(), RSX::DisplayPitch(), x, y, w, h, false);
}

void RSXRenderer::Present(const std::string &overlay) {
	RSX::StepQuiet("renderer: present");
	FlushBatch();
	PSPPointer<u8> topaddr;
	u32 linesize = 512;
	GEBufferFormat fmt = GE_FORMAT_565;
	__DisplayGetFramebuf(&topaddr, &linesize, &fmt, 0);
	Surface *s = nullptr;
	uint32_t srcX = 0, srcY = 0;  // Where the PSP screen starts in s (PSP pixels).
	if (topaddr.ptr && linesize > 0 && linesize <= 1024) {
		const uint32_t addr = NormalizeAddr(topaddr.ptr);
		// The screen may lie inside a framebuffer the game drew at an offset: Silent Hill Origins
		// shows its two 16-bit screens side by side in one 1024-wide buffer (040d4000 and
		// 040d43c0) but draws both through 040d4000, the right one 480 pixels in. Presenting a
		// copy of its own made for 040d43c0 showed that buffer as VRAM had it - black or an old
		// frame, every other frame. One the RSX drew into wins over an undrawn exact match.
		Surface *exact = nullptr, *inside = nullptr;
		for (auto &ptr : surfaces_) {
			Surface &c = *ptr;
			if (c.stride != linesize || c.fmt != fmt)
				continue;
			if (c.addr == addr) {
				exact = &c;
				continue;
			}
			const uint32_t bpp = BytesPerPixel(c.fmt);
			const uint32_t rowBytes = c.stride * bpp;
			if (addr < c.addr || !c.gpuDirty)
				continue;
			const uint32_t off = addr - c.addr;
			const uint32_t y0 = off / rowBytes, x0 = (off % rowBytes) / bpp;
			if (off % bpp || x0 + PSP_W > c.stride || y0 + PSP_H > (uint32_t)c.rows)
				continue;
			if (!inside) {
				inside = &c;
				srcX = x0;
				srcY = y0;
			}
		}
		if (inside && !(exact && exact->gpuDirty)) {
			s = inside;
		} else {
			srcX = srcY = 0;
			s = exact ? exact : FindSurface(addr, (uint16_t)linesize, fmt, PSP_H, true);
		}
	}
#ifdef PPSSPP_RSX_HOSTSIM
	if (getenv("PPSSPP_PRESENTLOG"))
		fprintf(stderr, "PRESENT %08x stride %d fmt %d from %08x at %u,%u\n", topaddr.ptr ? NormalizeAddr(topaddr.ptr) : 0, (int)linesize, (int)fmt, s ? s->addr : 0, srcX, srcY);
#endif
	if (s) {
		Validate(*s);
		MergeCpuRows(*s);
		s->lastFrame = frame_;
	}

	RSX::GpuWaitForRendering();
	// The back buffer is on screen until the previous frame's flip; the RSX waits for it here,
	// after the frame's drawing, not before.
	RSX::GpuWaitFlip();
	RSX::Target t;
	t.colorOffset = RSX::BackBufferOffset();
	t.colorPitch = RSX::DisplayPitch();
	t.width = (uint16_t)RSX::DisplayWidth();
	t.height = (uint16_t)RSX::DisplayHeight();
	t.hasDepth = false;
	RSX::SetTarget(t);
	RSX::SetPixelViewport(t.width, t.height);
	RSX::SetDefaultState();
	tlCull_ = 0;
	if (clearFrames_ > 0) {
		RSX::ClearTarget(0xFF000000, true, false);
		clearFrames_--;
		RSX::GpuWaitForRendering();
	}
	if (s) {
		const int srcW = std::min(PSP_W, (int)s->stride - (int)srcX) * scale_;
		uint32_t srcOffset = s->offset + srcY * scale_ * s->pitch + srcX * scale_ * 4;
		uint32_t srcPitch = s->pitch;
		// Edge smoothing: the picture through FXAA first (its own buffer), then scaled from there.
		if (opt_.fxaa && FxaaPass(*s, srcX, srcY, srcW, PSP_H * scale_, &srcOffset, &srcPitch)) {
			// FxaaPass set its own target: the back buffer's again.
			RSX::SetTarget(t);
			RSX::SetPixelViewport(t.width, t.height);
			RSX::SetDefaultState();
			tlCull_ = 0;
			RSX::BindVertexProgram();
			vpTL_ = false;
		}
		RSX::ScaleImage(RSX::Mem::Local, srcOffset, srcPitch, srcW, PSP_H * scale_,
			RSX::Mem::Local, RSX::BackBufferOffset(), RSX::DisplayPitch(), gameX_, gameY_, gameW_, gameH_, opt_.bilinear);
	}
	DrawOverlay(overlay, gameX_ + 8, gameY_ + 6);
	RSX::StepQuiet("renderer: flip");
	RSX::Present();
	RSX::EndFrame();

	if (opt_.syncEveryFrame) DownloadAll();

	ProcessDeferredFrees(false);
	if ((frame_ & 255) == 0) EvictTextures(600);
	// Retired framebuffers nobody drew into or displayed for a long time.
	for (size_t i = 0; i < surfaces_.size();) {
		Surface &old = *surfaces_[i];
		if (!old.gpuDirty && &old != target_ && frame_ - old.lastFrame > 1200) {
			DeferFree(old.mem, old.tile);
			surfaces_[i] = std::move(surfaces_.back());
			surfaces_.pop_back();
		} else {
			++i;
		}
	}
	if (target_) target_->lastFrame = frame_;
	frame_++;
	// Next frame starts by checking VRAM against our copies again.
	targetValid_ = false;
	stateValid_ = false;
	RSX::StepQuiet("idle");
}

std::string RSXRenderer::Stats() {
	char text[400];
	snprintf(text, sizeof(text), "RSX %ux: %u prims, %u draws, %u tex decodes (%u slow, %u on SPUs), %u fb-as-texture (%u inside, %u copies, %u read waits), %u palette lookups, %u batched draws (%u hardware T&L), %u up, %u down, %u fb, %u tex (%u MB)",
		scale_, prims_, draws_, texDecodes_, slowDecodes_, spuDecodes_, rttHits_, rttOffsetHits_, rttCopies_, readWaits_, depalPasses_, batchedDraws_, tlDraws_, uploads_, downloads_, (unsigned)surfaces_.size(), (unsigned)textures_.size(),
		textureBytes_ / (1024 * 1024));
	const double ms = 1000.0 / 79800000.0;
	char times[240];
	snprintf(times, sizeof(times), "; time in renderer %.0f ms (texture lookups %.0f: decode %.0f, hashing %.0f; write-back %.0f, upload %.0f; vertex ring in %s memory)",
		tbBackend_ * ms, tbLookup_ * ms, tbDecode_ * ms, tbHash_ * ms, tbDownload_ * ms, tbUpload_ * ms, RSX::VertexRingInMain() ? "main" : "local");
	char lk[160];
	snprintf(lk, sizeof(lk), "; texture lookups %u (framebuffer checks %.0f ms, palette hash and key %.0f ms, table %.0f ms)",
		lookups_, tbLkFb_ * ms, tbLkKey_ * ms, tbLkMap_ * ms);
	lookups_ = 0;
	tbLkFb_ = tbLkKey_ = tbLkMap_ = 0;
	char dl[1024];
	snprintf(dl, sizeof(dl), "; write-backs by cause: more rows %u (%.0f ms), new layout %u (%.0f ms), memory read %u (%.0f ms), texture %u (%.0f ms), depth %u (%.0f ms), other %u (%.0f ms)",
		dlCount_[DL_ROWS], dlTb_[DL_ROWS] * ms, dlCount_[DL_LAYOUT], dlTb_[DL_LAYOUT] * ms, dlCount_[DL_READ], dlTb_[DL_READ] * ms,
		dlCount_[DL_TEXTURE], dlTb_[DL_TEXTURE] * ms, dlCount_[DL_DEPTH], dlTb_[DL_DEPTH] * ms, dlCount_[DL_OTHER], dlTb_[DL_OTHER] * ms);
	if (partDownloads_) {
		const size_t len = strlen(dl);
		snprintf(dl + len, sizeof(dl) - len, " (%u of them only the part a block transfer reads)", partDownloads_);
		partDownloads_ = 0;
	}
	if (queuedXfers_) {
		const size_t len = strlen(dl);
		snprintf(dl + len, sizeof(dl) - len, "; %u block transfers to main memory made in order with the drawing", queuedXfers_);
		queuedXfers_ = 0;
	}
	if (opt_.keepLayouts) {
		const size_t len = strlen(dl);
		snprintf(dl + len, sizeof(dl) - len, "; layout switches kept on the RSX %u (%u uploads skipped)", keptLayouts_, keptRowSkips_);
		keptLayouts_ = keptRowSkips_ = 0;
	}
	if (fxaaFrames_) {
		const size_t len = strlen(dl);
		snprintf(dl + len, sizeof(dl) - len, "; edge smoothing (%s) on %u frames (%.0f ms to queue)", opt_.fxaa == 2 ? "Light" : "Full", fxaaFrames_, tbFxaa_ * ms);
		fxaaFrames_ = 0;
		tbFxaa_ = 0;
	}
	if (swbIssued_ || swbPendingSyncs_) {
		const size_t len = strlen(dl);
		snprintf(dl + len, sizeof(dl) - len, "; small copies for the game to read: %u made, %u rows written to VRAM, %u rows kept (the CPU had changed them), %u dropped (written back in full since), %u syncs waiting for one in all",
			swbIssued_, swbRowsWritten_, swbRowsKept_, swbDropped_, swbPendingSyncs_);
		swbIssued_ = swbRowsWritten_ = swbRowsKept_ = swbDropped_ = swbBusy_ = swbPendingSyncs_ = 0;
	}
	memset(dlCount_, 0, sizeof(dlCount_));
	memset(dlTb_, 0, sizeof(dlTb_));
	char tl[320];
	snprintf(tl, sizeof(tl), "; hardware T&L draws %.0f ms (state %.0f: target %.0f, apply %.0f, memo %u hits %u misses; constants %.0f in %u uploads, vertex copy %.0f, submit %.0f)",
		(tbTLState_ + tbTLConst_ + tbTLCopy_ + tbTLSubmit_) * ms, tbTLState_ * ms, tbTLTarget_ * ms, tbTLApply_ * ms, memoHits_, memoMisses_,
		tbTLConst_ * ms, tlConstUploads_, tbTLCopy_ * ms, tbTLSubmit_ * ms);
	tbTLState_ = tbTLConst_ = tbTLCopy_ = tbTLSubmit_ = tbTLTarget_ = tbTLApply_ = 0;
	memoHits_ = memoMisses_ = 0;
	tlConstUploads_ = 0;
	tbBackend_ = tbDecode_ = tbDownload_ = tbUpload_ = tbHash_ = tbLookup_ = 0;
	prims_ = draws_ = texDecodes_ = uploads_ = downloads_ = rttHits_ = rttOffsetHits_ = rttCopies_ = readWaits_ = depalPasses_ = slowDecodes_ = spuDecodes_ = batchedDraws_ = tlDraws_ = 0;
	return std::string(text) + times + tl + dl + lk;
	return text;
}

// ---------------------------------------------------------------------------------------------

RSXRenderer *RSXRendererCreate(const RSXRendererOptions &options) {
	RSXRenderer *r = new RSXRenderer(options);
	if (!r->Init()) {
		delete r;
		return nullptr;
	}
	return r;
}

void RSXRendererDestroy(RSXRenderer *renderer) {
	if (!renderer) return;
	renderer->Shutdown();
	delete renderer;
}

void RSXRendererPresent(RSXRenderer *renderer, const std::string &overlay) {
	if (renderer) renderer->Present(overlay);
}

std::string RSXRendererStats(RSXRenderer *renderer) {
	return renderer ? renderer->Stats() : std::string();
}

Rasterizer::HardwareBackend *RSXRendererBackend(RSXRenderer *renderer) {
	return renderer;
}

void RSXRendererSetLiveOptions(RSXRenderer *renderer, bool bilinear, bool keepLayouts, bool skipDepthReads, int fxaa) {
	if (renderer) renderer->SetLiveOptions(bilinear, keepLayouts, skipDepthReads, fxaa);
}

void RSXRendererWriteBackAll(RSXRenderer *renderer) {
	if (renderer) renderer->WriteBackAll();
}

void RSXRendererSetCpuRows(RSXRenderer *renderer, bool on) {
	if (renderer) renderer->SetCpuRows(on);
}

void RSXRendererSetFont(const uint8_t *atlas) {
	g_font = atlas;
}
