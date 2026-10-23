// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Optional hook for platforms that rasterize the software GPU's output on real graphics hardware
// (the PS3 RSX renderer). When set, the BinManager hands every primitive - already transformed,
// lit, clipped and in screen space - to the backend instead of binning it for the software
// rasterizer. Everything before rasterization stays the software GPU's own code.
#pragma once

#include <string>

#include <cstdint>

struct VertexData;
struct GEState;

namespace Rasterizer {

struct RasterizerState;

// Batched hardware path: a whole draw's triangles, transformed (but not clipped, lit or binned)
// by a light-weight loop in the transform unit. Position is PSP screen space in homogeneous
// form: pixel x = x / w, y = y / w, depth (0..65535) = z / w. Colors are RGBA with R in the low
// byte, like VertexData. s/t are as the GE uses them (texels in through mode).
struct FastVertex {
	float x, y, z, w;
	uint32_t color0, color1;
	float s, t, q;
	float fog;
};

// Hardware transform & lighting: the backend's vertex program does the GE's vertex stage.
// Model-space vertices (the decoder's output, skinned and morphed) plus the state below.
struct TLVertex {
	float x, y, z;
	float nx, ny, nz;
	uint8_t c[4];  // R, G, B, A.
	float u, v;
};
static_assert(sizeof(TLVertex) == 36, "T&L vertex layout");

constexpr int TL_CONST_COUNT = 52;  // c[3] .. c[54].

struct TLParams {
	float mvp[16];  // World-view-projection, column-major (PSP clip space).
	float screenScale[3], screenAdd[3];
	float offX, offY;
	// Vertex program constants c[3] .. c[54] (layout: PS3/rsx/make-shaders.py, tl_program).
	// c[3] .. c[6], the final matrix, are left for the backend (FoldTLMatrix).
	float c[TL_CONST_COUNT][4];
};

// The final matrix rows c[3] .. c[6]: PSP clip space -> PSP screen space (viewport and offset)
// -> backend clip space, where x = kx * screen_x - w, y = ky * screen_y - w (or w - ky * screen_y
// when flipped), z = kz * depth - w. All linear in the clip coordinates, so one matrix.
inline void FoldTLMatrix(const TLParams &p, float kx, float ky, float kz, bool flipY, float rows[4][4]) {
	const float *m = p.mvp;
	float r[4][4];
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			r[i][j] = m[j * 4 + i];
	for (int j = 0; j < 4; ++j) {
		rows[0][j] = kx * p.screenScale[0] * r[0][j] + (kx * (p.screenAdd[0] - p.offX) - 1.0f) * r[3][j];
		const float y = ky * p.screenScale[1] * r[1][j] + (ky * (p.screenAdd[1] - p.offY) - 1.0f) * r[3][j];
		rows[1][j] = flipY ? -y : y;
		rows[2][j] = kz * p.screenScale[2] * r[2][j] + (kz * p.screenAdd[2] - 1.0f) * r[3][j];
		rows[3][j] = r[3][j];
	}
}

class HardwareBackend {
public:
	// Hardware transform & lighting. cull 3: the indices are a line list (pairs), not triangles.
	// Hardware transform & lighting. cull: 0 none, 1 keep triangles whose screen-space cross
	// product (v1 - v0) x (v2 - v0), y down, is positive (the GE's CCW mode), 2 negative.
	virtual bool SupportsTL() const { return false; }
	virtual bool DrawTL(const RasterizerState &state, const TLParams &p, const TLVertex *verts, int nverts, const uint16_t *indices, int nidx, int cull) { return false; }
	// Draws count / 3 triangles (already culled) in one go. False if the backend can't (the
	// caller then takes the regular per-primitive path).
	virtual bool DrawTriangles(const RasterizerState &state, const FastVertex *verts, int count, bool through) { return false; }
	// Indexed triangle list: nidx indices into verts (each triple one triangle). Returns false if
	// the backend can't take it (the caller then uses DrawTriangles or the regular path).
	virtual bool DrawIndexed(const RasterizerState &state, const FastVertex *verts, int nverts, const uint16_t *indices, int nidx, bool through) { return false; }
	virtual ~HardwareBackend() {}
	virtual void Triangle(const RasterizerState &state, const VertexData &v0, const VertexData &v1, const VertexData &v2) = 0;
	// Two corners; clear is the GE's clear mode.
	virtual void Rect(const RasterizerState &state, const VertexData &v0, const VertexData &v1, bool clear) = 0;
	virtual void Line(const RasterizerState &state, const VertexData &v0, const VertexData &v1) = 0;
	virtual void Point(const RasterizerState &state, const VertexData &v0) = 0;
	// The software GPU is about to read or write emulated memory that drawing may have changed:
	// pixels rendered on the hardware must be in PSP memory when this returns, where needed.
	virtual void Flush(const char *reason) = 0;
	// Whether rendered-but-not-yet-written-back pixels overlap this range of PSP memory.
	virtual bool HasPendingWrite(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) = 0;
	// Finish drawing queued on another thread (the GE worker). Called at the PSP's GE sync points.
	virtual void Sync() {}
	// Whether drawing not done yet may still read this range of PSP memory (a texture), which is
	// about to be overwritten. True when unknown.
	virtual bool MayRead(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) { return true; }
	// A block transfer from memory the hardware didn't draw is about to overwrite part of a buffer it
	// draws into (stride and w in bytes, h rows). True: it doesn't need that buffer written back
	// first. The caller then copies into PSP memory and calls TakeCpuRows, and the hardware takes
	// that rectangle from there into its copy, in order with the draws queued before (they still
	// land first, what is drawn after goes on top). Queued draws that read the buffer as a texture
	// read the hardware's copy, so they aren't affected.
	virtual bool CanTakeCpuRows(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) { return false; }
	virtual void TakeCpuRows(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) {}
	// A block transfer from VRAM into main memory (strides and width in bytes, h rows). True: the
	// hardware makes it later, in order with the draws queued before it, as the GE does; the caller
	// neither flushes nor copies, and notes the destination (NoteAsyncRamWrite) so its own reads of it
	// wait. It is done by the time the game can see the GE finish (Sync).
	virtual bool QueueTransferToRam(uint32_t src, uint32_t srcStride, uint32_t dst, uint32_t dstStride, uint32_t w, uint32_t h) { return false; }
	// Pixels drawn over this range of VRAM (stride and w in bytes, h rows) written back to PSP memory.
	virtual void WriteBackRange(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) {}
};

// Null unless a platform installs one before the game starts.
extern HardwareBackend *g_hardwareBackend;

// The memory a block transfer's flush is for (TransformUnit::FlushIfOverlap sets it while it
// flushes): a backend need only write back what overlaps it. end == 0: not known, everything.
// Main memory that queued block transfers (QueueTransferToRam) are still to write. Used on the GE
// command thread only; cleared by the backend once its queue has run (Sync, or a flush that drains it).
void NoteAsyncRamWrite(uint32_t start, uint32_t size);
bool AsyncRamWritePending(uint32_t start, uint32_t size);
bool AsyncRamWritesQueued();
void ClearAsyncRamWrites();

struct FlushRange {
	uint32_t start = 0, end = 0;
	uint32_t stride = 0, width = 0;  // Bytes: the transfer's row pitch and row length.
};
extern FlushRange g_flushRange;
// Bumped whenever PSP memory may have changed behind the GE's back: the CPU ran (any GE call from
// the game) or a block transfer. A hardware backend can trust its texture copies within one value.
extern uint32_t g_geCpuEpoch;
// Bumped by BinManager::UpdateState, i.e. once per draw: primitives with the same value come from one draw.
extern uint32_t g_drawSerial;
// Merge consecutive direct T&L draws with the same state into one job (PS3 settings menu).
extern bool g_tlMergeEnabled;
// Diag builds: draws whose vertices the SPUs prepared are decoded on the PPU too and compared (the
// PPU's are drawn when they differ). TakeSpuVertexCheckReport: the report lines since the last call.
extern bool g_spuVertexCheck;
void TakeSpuVertexCheckReport(std::string &out);
// Bumped whenever GE state may have changed (any command other than drawing and vertex/index
// addresses, and each start of command processing): two draws with the same value have exactly
// the same GE state, so a draw can join the previous one's job without comparing it.
extern uint32_t g_geStateSerial;
extern int g_lastSerialCmd;  // Test builds: last command that changed the serial.
// Each change also goes into a ring of the last 256 ranges (by epoch), so a backend can tell
// whether its copy of a texture is affected without rehashing it.
struct GeDirtyRange { uint32_t start, end, epoch; };
extern GeDirtyRange g_geDirty[256];
inline uint32_t GeNormalizeAddr(uint32_t a) { return (a & 0x0F000000) == 0x04000000 ? (a & 0x041FFFFF) : (a & 0x3FFFFFFF); }
// Whether a draw has taken the current epoch (BinManager::UpdateState). Until one has, a change
// can widen the current epoch's range instead of starting a new epoch: a run of small block
// transfers (Gran Turismo's menus: thousands a frame) would otherwise push the ring past what
// GeRangeUnchanged looks at, and every texture check would rehash.
extern bool g_geEpochSeen;
inline void NoteGeDirtyRange(uint32_t start, uint32_t size) {
	uint32_t s0 = 0, s1 = 0xFFFFFFFF;
	if (size != 0xFFFFFFFF) {
		s0 = GeNormalizeAddr(start);
		s1 = s0 + size;
	}
	if (!g_geEpochSeen) {
		// No reader looks at this entry before a draw takes the epoch (on this thread, after this).
		GeDirtyRange &cur = g_geDirty[g_geCpuEpoch & 255];
		if (cur.epoch == g_geCpuEpoch) {
			if (s0 < cur.start) cur.start = s0;
			if (s1 > cur.end) cur.end = s1;
			return;
		}
	}
	const uint32_t e = ++g_geCpuEpoch;
	GeDirtyRange &r = g_geDirty[e & 255];
	r.start = s0;
	r.end = s1;
	__sync_synchronize();
	r.epoch = e;
	g_geEpochSeen = false;
}
inline void NoteGeMemoryMayHaveChanged() { NoteGeDirtyRange(0, 0xFFFFFFFF); }
// True if nothing recorded after epoch `since` up to `until` touches [start, start + size).
inline bool GeRangeUnchanged(uint32_t start, uint32_t size, uint32_t since, uint32_t until) {
	if (until - since > 200)
		return false;
	start = GeNormalizeAddr(start);
	const uint32_t end = start + size;
	for (uint32_t e = since + 1; e != until + 1; ++e) {
		const GeDirtyRange &r = g_geDirty[e & 255];
		if (r.epoch != e)
			return false;
		if (start < r.end && end > r.start)
			return false;
	}
	return true;
}
// While the GE worker draws a queued draw: the GE registers as they were when it was queued. A
// backend reads GE registers through this when it's set, not from gstate (which has moved on).
extern const GEState *g_drawStateOverride;

// The GE worker thread (TransformUnit.cpp): direct T&L draws are decoded and drawn on it while the
// emulation thread carries on. Returns the backend to install as g_hardwareBackend.
HardwareBackend *StartGEWorker(HardwareBackend *real);
void StopGEWorker();
// The emulated GE stops processing commands and the game's code runs next.
void GEBurstEnd();

}  // namespace Rasterizer
