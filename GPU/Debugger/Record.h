// Copyright (c) 2017- PPSSPP Project.

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

#include <functional>
#include <atomic>
#include <mutex>
#include <vector>
#include <set>
#include <unordered_map>
#include <deque>
#include <memory>

#include "Common/CommonTypes.h"
#include "GPU/Debugger/RecordFormat.h"

class Path;

namespace GPURecord {

// A byte buffer kept in 64 KB pieces: a dump of a heavy frame is 10-20 MB, and on the PS3 a game
// like Silent Hill Origins leaves plenty of memory free, but not in pieces of more than ~512 KB
// (a std::vector of that size, doubling as it grew, couldn't be had: "GE dump skipped (memory)").
// Most bytes a dump's data may take (0 = no limit). The PS3 frontend sets it from the memory left
// when a dump is asked for: a dump that would need more is dropped instead of running the emulator
// out of memory.
extern size_t g_dumpByteLimit;
// Called (if set) when a dump was dropped for that reason, with the bytes it had reached.
extern void (*g_dumpDroppedHook)(u32 bytes);
// A frame taking longer than this to record is given up (0: no limit). With the PS3's RSX renderer
// every texture the game takes from a buffer it drew is copied back from the RSX for the dump: one
// frame of Midnight Club took 20+ s and drained memory until the PS3 hung (build 257).
extern double g_dumpTimeLimit;
// Test builds (headless): allow recording while a GE dump plays, to check the recorder on a PC.
extern bool g_dumpRecordReplays;
extern void (*g_dumpAbandonedHook)(double seconds, u32 bytes);

class ChunkedBuffer {
public:
	static constexpr u32 CHUNK = 64 * 1024;
	// Wanted to grow past the limit (or memory ran out): the contents are incomplete.
	bool failed() const { return failed_; }
	u32 size() const { return size_; }
	bool empty() const { return size_ == 0; }
	void clear() { chunks_.clear(); size_ = 0; failed_ = false; }
	// Grows (new bytes zeroed) or shrinks.
	void resize(u32 n);
	void write(u32 off, const void *p, u32 n);
	void fill(u32 off, u8 c, u32 n);
	void read(u32 off, void *p, u32 n) const;
	// memcmp of n bytes at off with p (0 when equal).
	int compare(u32 off, const void *p, u32 n) const;
	// Bytes from off to the end of its piece, and a pointer to them.
	const u8 *span(u32 off, u32 *len) const;
	u8 *span(u32 off, u32 *len);
private:
	std::vector<std::unique_ptr<u8[]>> chunks_;
	u32 size_ = 0;
	bool failed_ = false;
	size_t limit_ = 0;
public:
	void setLimit(size_t limit) { limit_ = limit; }
};


constexpr uint32_t DIRTY_VRAM_SHIFT = 8;
constexpr uint32_t DIRTY_VRAM_ROUND = (1 << DIRTY_VRAM_SHIFT) - 1;
constexpr uint32_t DIRTY_VRAM_SIZE = (2 * 1024 * 1024) >> DIRTY_VRAM_SHIFT;
constexpr uint32_t DIRTY_VRAM_MASK = (2 * 1024 * 1024 - 1) >> DIRTY_VRAM_SHIFT;
enum class DirtyVRAMFlag : uint8_t {
	CLEAN = 0,
	UNKNOWN = 1,
	DIRTY = 2,
	DRAWN = 3,
};

class Recorder {
public:
	bool IsActive() const {
		return active;
	}
	bool IsActivePending() const {
		return nextFrame || active;
	}
	bool RecordNextFrame(const std::function<void(const Path &)> callback);
	void ClearCallback() {
		std::lock_guard<std::mutex> guard(callbackLock_);
		writeCallback = nullptr;
	}

	void NotifyCommand(u32 pc);
	void NotifyMemcpy(u32 dest, u32 src, u32 sz);
	void NotifyMemset(u32 dest, int v, u32 sz);
	void NotifyUpload(u32 dest, u32 sz);
	void NotifyDisplay(u32 addr, int stride, int fmt);
	void NotifyBeginFrame();
	void NotifyCPU();
private:
	void FlushRegisters();
	void DirtyAllVRAM(DirtyVRAMFlag flag);
	void DirtyVRAM(u32 start, u32 sz, DirtyVRAMFlag flag);
	void DirtyDrawnVRAM();

	bool BeginRecording();
	Path WriteRecording();

	bool HasDrawCommands() const;
	void CheckEdramTrans();
	void FinishRecording();

	Command EmitCommandWithRAM(CommandType t, const void *p, u32 sz, u32 align);

	void UpdateLastVRAM(u32 addr, u32 bytes);
	void ClearLastVRAM(u32 addr, u8 c, u32 bytes);
	int CompareLastVRAM(u32 addr, u32 bytes) const;

	u32 GetTargetFlags(u32 addr, u32 sizeInRAM);

	void FlushPrimState(int vcount);
	void EmitTextureData(int level, u32 texaddr);
	void EmitTransfer(u32 op);
	void EmitClut(u32 op);
	void EmitPrim(u32 op);
	void EmitBezierSpline(u32 op);

	double recordStart_ = 0.0;
	u32 commandsSinceCheck_ = 0;
	void AbandonRecording(double seconds);
	bool active = false;
	std::atomic<bool> nextFrame = false;
	int flipLastAction = -1;
	int flipFinishAt = -1;
	uint32_t lastEdramTrans = 0x400;
	std::function<void(const Path &)> writeCallback;
	// RecordNextFrame is called from other threads. Guards writeCallback, nextFrame and the writes to active.
	std::mutex callbackLock_;

	ChunkedBuffer pushbuf;
	std::deque<Command> commands;
	std::vector<u32_le> lastRegisters;  // GE commands, little-endian like PSP memory (the dump format).
	std::vector<u32> lastTextures;
	std::set<u32> lastRenderTargets;
	// Data emitted this recording: hash of its bytes (seeded with its size) -> its offset.
	std::unordered_map<uint64_t, u32> emitted_;
	ChunkedBuffer lastVRAM;

	DirtyVRAMFlag dirtyVRAM[DIRTY_VRAM_SIZE];
};

}  // namespace GPURecord
