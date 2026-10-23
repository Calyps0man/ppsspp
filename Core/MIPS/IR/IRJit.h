// Copyright (c) 2012- PPSSPP Project.

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

#include <cstring>
#include "ppsspp_config.h"
#include <unordered_map>

#include "Common/CommonTypes.h"
#include "Common/CPUDetect.h"
#include "Core/MIPS/JitCommon/JitBlockCache.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/IR/IRRegCache.h"
#include "Core/MIPS/IR/IRFrontend.h"
#include "Core/MIPS/MIPSVFPUUtils.h"

#ifndef offsetof
#include "stddef.h"
#endif

// Very expensive, time-profiles every block.
// Not to be released with this enabled.
//
// #define IR_PROFILING

// Try to catch obvious misses of be above rule.
#if defined(IR_PROFILING) && defined(GOLD)
#error
#endif

struct IRInst;

namespace MIPSComp {

// TODO : Use arena allocators. For now let's just malloc.
class IRBlock {
public:
	IRBlock() {}
	IRBlock(u32 emAddr, u32 origSize, int instOffset, u32 numInstructions)
		: origAddr_(emAddr), origSize_(origSize), arenaOffset_(instOffset), numIRInstructions_(numInstructions) {}
	IRBlock(IRBlock &&b) noexcept {
		arenaOffset_ = b.arenaOffset_;
		hash_ = b.hash_;
		origAddr_ = b.origAddr_;
		origSize_ = b.origSize_;
		origFirstOpcode_ = b.origFirstOpcode_;
		nativeOffset_ = b.nativeOffset_;
		numIRInstructions_ = b.numIRInstructions_;
		retired_ = b.retired_;
		b.arenaOffset_ = 0xFFFFFFFF;
	}

	~IRBlock() {}

	u32 GetIRArenaOffset() const { return arenaOffset_; }
	int GetNumIRInstructions() const { return numIRInstructions_; }
	MIPSOpcode GetOriginalFirstOp() const { return origFirstOpcode_; }
	bool HasOriginalFirstOp() const;
	bool RestoreOriginalFirstOp(int number);
	bool IsValid() const { return origAddr_ != 0 && !retired_ && origFirstOpcode_.encoding != 0x68FFFFFF; }
	// Code the game rewrote: kept (address, size, hash) in case the same code comes back, but not
	// in use - the PSP code no longer has its emuhack (IRNativeJit::InvalidateCacheAt).
	void Retire(int cookie) {
		RestoreOriginalFirstOp(cookie);
		retired_ = true;
	}
	bool IsRetired() const { return origAddr_ != 0 && retired_; }
	void Unretire() { retired_ = false; }
	// The block's IR moved to [offset, offset + count) of the arena (IRBlockCache::ReplaceBlockIR).
	void SetIR(u32 offset, u32 count) {
		arenaOffset_ = offset;
		numIRInstructions_ = count;
	}
	void SetNativeOffset(int offset) {
		nativeOffset_ = offset;
	}
	int GetNativeOffset() const {
		return nativeOffset_;
	}
	void UpdateHash() {
		hash_ = CalculateHash();
	}
	bool HashMatches() const {
		return origAddr_ && hash_ == CalculateHash();
	}
	bool OverlapsRange(u32 addr, u32 size) const;

	void GetRange(u32 *start, u32 *size) const {
		*start = origAddr_;
		*size = origSize_;
	}
	u32 GetOriginalStart() const {
		return origAddr_;
	}
	u64 GetHash() const {
		return hash_;
	}

	void Finalize(int number);
	void Destroy(int number);

#ifdef IR_PROFILING
	JitBlockProfileStats profileStats_{};
#endif

	// The block's PSP code as it is in memory now (other blocks' emuhacks read as their first ops).
	u64 CalculateHash() const;

private:

	// Offset into the block cache's Arena
	u32 arenaOffset_ = 0;
	// Offset into the native code buffer.
	int nativeOffset_ = -1;
	u64 hash_ = 0;
	bool retired_ = false;
	u32 origAddr_ = 0;
	u32 origSize_ = 0;
	MIPSOpcode origFirstOpcode_ = MIPSOpcode(0x68FFFFFF);
	u32 numIRInstructions_ = 0;
};

#if PPSSPP_PLATFORM(PS3)
// The most blocks the PS3 build keeps before clearing the cache (Frontend 95). The block lists and
// lookup tables get room for this many once, at the first block: grown later, each needed a new,
// bigger block of memory (several MB) once main memory was in small pieces, and Silent Hill
// Origins ran out entering a new area.
// The PS3 "JIT cache" setting: 65536 (Normal) or 98304 (Large: Midnight Club L.A. filled 65536 in
// a race and the clear made every block compile again). Set before the first block.
extern int g_irPs3BlockLimit;
// PS3: called when the cache is cleared because the block limit was reached (blocks in it then), before
// the clear; the frontend may raise g_irPs3BlockLimit for the rest of the game.
extern void (*g_irPs3CacheFullHook)(int blocks);
extern u32 g_irPs3ArenaLimit;
#define IR_PS3_BLOCK_LIMIT g_irPs3BlockLimit
#endif

class IRBlockCache : public JitBlockCacheDebugInterface {
public:
	IRBlockCache(bool compileToNative);
	~IRBlockCache() {
		Clear();
	}

	void Clear();
	std::vector<int> FindInvalidatedBlockNumbers(u32 address, u32 length);
	void FinalizeBlock(int blockNum);
	int GetNumBlocks() const override { return (int)blocks_.size(); }
	int AllocateBlock(int emAddr, u32 origSize, const std::vector<IRInst> &inst);
	// New IR for an existing block, appended to the arena (native mode only: offsets stop rising).
	bool ReplaceBlockIR(int blockNum, const std::vector<IRInst> &inst);
	IRBlock *GetBlock(int blockNum) {
		if (blockNum >= 0 && blockNum < (int)blocks_.size()) {
			return &blocks_[blockNum];
		} else {
			return nullptr;
		}
	}
	void RemoveBlockFromPageLookup(int blockNum);
	int GetBlockNumFromIRArenaOffset(int offset) const;
	const IRInst *GetBlockInstructionPtr(const IRBlock &block) const {
		return arena_.data() + block.GetIRArenaOffset();
	}
	const IRInst *GetBlockInstructionPtr(int blockNum) const {
		return arena_.data() + blocks_[blockNum].GetIRArenaOffset();
	}
	size_t GetArenaSize() const { return arena_.size(); }
	const IRInst *GetArenaPtr() const {
		return arena_.data();
	}
	bool IsValidBlock(int blockNum) const override {
		return blockNum >= 0 && blockNum < (int)blocks_.size() && blocks_[blockNum].IsValid();
	}
	IRBlock *GetBlockUnchecked(int blockNum) {
		return &blocks_[blockNum];
	}
	const IRBlock *GetBlock(int blockNum) const {
		if (blockNum >= 0 && blockNum < (int)blocks_.size()) {
			return &blocks_[blockNum];
		} else {
			return nullptr;
		}
	}

	int FindPreloadBlock(u32 em_address);

	// "Cookie" means the 24 bits we inject into the first instruction of each block.
	int FindByCookie(int cookie);

	std::vector<u32> SaveAndClearEmuHackOps();
	void RestoreSavedEmuHackOps(const std::vector<u32> &saved);

	JitBlockDebugInfo GetBlockDebugInfo(int blockNum) const override;
	JitBlockMeta GetBlockMeta(int blockNum) const override {
		JitBlockMeta meta{};
		if (IsValidBlock(blockNum)) {
			meta.valid = true;
			blocks_[blockNum].GetRange(&meta.addr, &meta.sizeInBytes);
		}
		return meta;
	}
	JitBlockProfileStats GetBlockProfileStats(int blockNum) const override {
#ifdef IR_PROFILING
		return blocks_[blockNum].profileStats_;
#else
		return JitBlockProfileStats{};
#endif
	}
	void ComputeStats(BlockCacheStats &bcStats) const override;
	int GetBlockNumberFromStartAddress(u32 em_address) const override;

	bool SupportsProfiling() const override {
#ifdef IR_PROFILING
		return true;
#else
		return false;
#endif
	}

private:
	u32 AddressToPage(u32 addr) const;
	bool compileToNative_;
	std::vector<IRBlock> blocks_;
	std::vector<IRInst> arena_;
	std::unordered_map<u32, std::vector<int>> byPage_;
	// Native mode: which block an emuhack cookie (native offset) belongs to. GetOriginalOp needs it
	// for every emuhack met while compiling; a scan of all blocks each time made compiling slower
	// the longer a game ran (Gran Turismo: tens of thousands of blocks, seconds of compiling).
	std::unordered_map<int, int> cookieToBlock_;
public:
	void NoteCookie(int cookie, int blockNum) { if (compileToNative_) cookieToBlock_[cookie] = blockNum; }
private:
};

class IRJit : public JitInterface {
public:
	IRJit(MIPSState *mipsState, bool actualJit);
	~IRJit();

	void DoState(PointerWrap &p) override;

	const JitOptions &GetJitOptions() { return jo; }

	void RunLoopUntil(u64 globalticks) override;

	void Compile(u32 em_address) override;	// Compiles a block at current MIPS PC

	bool DescribeCodePtr(const u8 *ptr, std::string &name) override;
	// Not using a regular block cache.
	JitBlockCache *GetBlockCache() override { return nullptr; }
	JitBlockCacheDebugInterface *GetBlockCacheDebugInterface() override { return &blocks_; }
	// The IR blocks themselves (test tools: PPCBlockVerify).
	IRBlockCache &GetIRBlocks() { return blocks_; }
	MIPSOpcode GetOriginalOp(MIPSOpcode op) override;

	std::vector<u32> SaveAndClearEmuHackOps() override { return blocks_.SaveAndClearEmuHackOps(); }
	void RestoreSavedEmuHackOps(std::vector<u32> saved) override { blocks_.RestoreSavedEmuHackOps(saved); }

	void ClearCache() override;
	void InvalidateCacheAt(u32 em_address, int length = 4) override;
	void UpdateFCR31() override;

	bool CodeInRange(const u8 *ptr) const override {
		return false;
	}

	const u8 *GetDispatcher() const override { return nullptr; }
	const u8 *GetCrashHandler() const override { return nullptr; }

	void LinkBlock(u8 *exitPoint, const u8 *checkedEntry) override;
	void UnlinkBlock(u8 *checkedEntry, u32 originalAddress) override;

	// This gets overridden by the native-backed IR jits.
	const u8 *GetCodeBase() const override { return nullptr; }

protected:
	bool CompileBlock(u32 em_address, std::vector<IRInst> &instructions, u32 &mipsBytes);
	virtual bool CompileNativeBlock(IRBlockCache *irBlockCache, int block_num) { return true; }
	// New blocks without the IR optimization passes (a backend that optimizes them later).
	virtual bool UnoptimizedFirst() const { return false; }
	virtual void FinalizeNativeBlock(IRBlockCache *irBlockCache, int block_num) {}
	// A block for this address compiled before and retired, whose code is back unchanged: in use again.
	virtual bool ReviveBlock(u32 em_address) { return false; }

	bool compileToNative_;

	JitOptions jo;

	IRFrontend frontend_;
	IRBlockCache blocks_;

	MIPSState *mips_;

	bool compilerEnabled_ = true;

	// where to write branch-likely trampolines. not used atm
	// u32 blTrampolines_;
	// int blTrampolineCount_;
};

}	// namespace MIPSComp
