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

#include "GPU/PS3Profile.h"
#include "Common/FrontendTrace.h"
#if PPSSPP_PLATFORM(PS3)
namespace PS3Frontend { void Trace(const char *fmt, ...); }
#endif
#include "ppsspp_config.h"
#include <set>
#include <new>
#include <algorithm>

#include "ext/xxhash.h"
#include "Common/Profiler/Profiler.h"

#include "Common/Log.h"
#include "Common/Serialize/Serializer.h"
#include "Common/StringUtils.h"

#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/MIPS/Interpreter.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/MIPS/IR/IRRegCache.h"
#include "Core/MIPS/IR/IRInterpreter.h"
#include "Core/MIPS/IR/IRJit.h"
#include "Core/MIPS/IR/IRNativeCommon.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/Reporting.h"
#include "Common/TimeUtil.h"
#include "Core/MIPS/MIPSTracer.h"


namespace MIPSComp {

IRJit::IRJit(MIPSState *mipsState, bool actualJit) : frontend_(mipsState->HasDefaultPrefix()), mips_(mipsState), blocks_(actualJit) {
	// u32 size = 128 * 1024;
	g_irRoundingModeUsed = false;
	InitIR();

	compileToNative_ = actualJit;

	// If this IRJit instance will be used to drive a "JIT using IR", don't optimize for interpretation.
	jo.optimizeForInterpreter = !actualJit;

	IROptions opts{};
	opts.disableFlags = g_Config.uJitDisableFlags;
#if PPSSPP_ARCH(RISCV64)
	// Assume RISC-V always has very slow unaligned memory accesses.
	opts.unalignedLoadStore = false;
	opts.unalignedLoadStoreVec4 = true;
	opts.preferVec4 = cpu_info.RiscV_V;
#elif PPSSPP_ARCH(ARM) || PPSSPP_ARCH(ARM64) || PPSSPP_ARCH(POWERPC)
	// PowerPC: svl.q/svr.q pairs become one StoreVec4/LoadVec4, which the PPC JIT does word by
	// word (any alignment); unfused, every pair went through the interpreter.
	opts.unalignedLoadStore = (opts.disableFlags & (uint32_t)JitDisable::LSU_UNALIGNED) == 0;
	opts.unalignedLoadStoreVec4 = true;
	opts.preferVec4 = true;
#else
	opts.unalignedLoadStore = (opts.disableFlags & (uint32_t)JitDisable::LSU_UNALIGNED) == 0;
	// TODO: Could allow on x86 pretty easily...
	opts.unalignedLoadStoreVec4 = false;
	opts.preferVec4 = true;
#endif
	opts.optimizeForInterpreter = jo.optimizeForInterpreter;
	frontend_.SetOptions(opts);
}

IRJit::~IRJit() {
#if PPSSPP_ARCH(POWERPC) && defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	if (getenv("JIT_PROF"))
		fprintf(stderr, "JIT_PROF blocks %llu total %llu frontend %llu native %llu\n", (unsigned long long)PS3Profile::jitBlocks,
			(unsigned long long)PS3Profile::jitCompile, (unsigned long long)PS3Profile::jitFrontend, (unsigned long long)PS3Profile::jitNative);
#endif
}

void IRJit::DoState(PointerWrap &p) {
	frontend_.DoState(p);
}

void IRJit::UpdateFCR31() {
}

void IRJit::ClearCache() {
	INFO_LOG(Log::JIT, "IRJit: Clearing the block cache!");
#if PPSSPP_ARCH(POWERPC)
	PS3Profile::jitClears++;
#endif
	blocks_.Clear();
}

void IRJit::InvalidateCacheAt(u32 em_address, int length) {
	std::vector<int> numbers = blocks_.FindInvalidatedBlockNumbers(em_address, length);
	if (numbers.empty()) {
		return;
	}

	DEBUG_LOG(Log::JIT, "Invalidating IR block cache at %08x (%d bytes): %d blocks", em_address, length, (int)numbers.size());

#if PPSSPP_ARCH(POWERPC)
	PS3Profile::jitInvalidated += numbers.size();
#endif
	for (int block_num : numbers) {
		auto block = blocks_.GetBlock(block_num);
		// TODO: We are invalidating a lot of blocks that are already invalid (yu gi oh).
		// INFO_LOG(Log::JIT, "Block at %08x invalidated: valid: %d", block->GetOriginalStart(), block->IsValid());
		// If we're a native JIT (IR->JIT, not just IR interpreter), we write native offsets into the blocks.
		int cookie = compileToNative_ ? block->GetNativeOffset() : block->GetIRArenaOffset();
		blocks_.RemoveBlockFromPageLookup(block_num);
		block->Destroy(cookie);
	}
}

void IRJit::Compile(u32 em_address) {
	_dbg_assert_(compilerEnabled_);

	PROFILE_THIS_SCOPE("jitc");
	PS3_PROFILE(jitCompile);
#if PPSSPP_ARCH(POWERPC)
	PS3Profile::JitProfExclude profExclude;
	PS3Profile::jitBlocks++;
	PS3Profile::jitCompilingPC = em_address;
	{
		// Addresses compiled since the last cache clear: a second compile of one is a recompile.
		static uint32_t seen[8192];
		static uint64_t seenClears = ~0ULL;
		if (seenClears != PS3Profile::jitClears) {
			memset(seen, 0, sizeof(seen));
			seenClears = PS3Profile::jitClears;
		}
		uint32_t h = ((em_address >> 2) * 2654435761u) >> 19;
		for (int probe = 0; probe < 8; ++probe, h = (h + 1) & 8191) {
			if (seen[h] == em_address) {
				PS3Profile::jitRecompiles++;
				PS3Profile::jitLastRecompilePC = em_address;
				break;
			}
			if (seen[h] == 0) {
				seen[h] = em_address;
				break;
			}
		}
	}
	struct ClearCompilingPC { ~ClearCompilingPC() { PS3Profile::jitCompilingPC = 0; } } clearCompilingPC;
#endif

#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	// Test build: PPC_JITTEST=file compiles code taken from a log instead of running the game:
	// lines "load <addr> <file.bin>" put code in PSP memory, "block <addr>" compiles a block (see
	// PPC_JITDUMP for the output). Then exits.
	static const char *jitTest = getenv("PPC_JITTEST");
	if (jitTest) {
		static bool done = false;
		if (!done) {
			done = true;
			FILE *f = fopen(jitTest, "r");
			char line[512];
			while (f && fgets(line, sizeof(line), f)) {
				unsigned addr;
				char path[400];
				if (sscanf(line, "load %x %399s", &addr, path) == 2) {
					FILE *b = fopen(path, "rb");
					if (!b) { fprintf(stderr, "JITTEST: can't open %s\n", path); continue; }
					size_t n = fread(Memory::GetPointerWriteUnchecked(addr), 1, 0x100000, b);
					fclose(b);
					fprintf(stderr, "JITTEST: %u bytes at %08x\n", (unsigned)n, addr);
				} else if (sscanf(line, "block %x", &addr) == 1) {
					std::vector<IRInst> insts;
					u32 bytes;
					CompileBlock(addr, insts, bytes);
					fprintf(stderr, "JITTEST: block %08x: %u MIPS bytes, %d IR\n", addr, bytes, (int)insts.size());
				}
			}
			if (f) fclose(f);
			exit(0);
		}
	}
#endif
	if (ReviveBlock(em_address))
		return;

	// Kept between blocks: no allocation per block (PS3 Frontend 109).
	static std::vector<IRInst> instructions;
	instructions.clear();
	u32 mipsBytes;
	if (!CompileBlock(em_address, instructions, mipsBytes)) {
		// Ran out of block numbers - need to reset.
		ERROR_LOG(Log::JIT, "Ran out of block numbers, clearing cache");
#if PPSSPP_PLATFORM(PS3)
		{
			// Which limit (the "JIT cache" setting, build 143): a clear means every block compiles again.
			static int clearLogs = 0;
			if (FrontendLogOn(FrontendLog::JIT) && clearLogs++ < 20)
				PS3Frontend::Trace("JIT cache full: %d blocks (limit %d), %u IR instructions (limit %u), %u KB of native code space left - cleared",
					blocks_.GetNumBlocks(), g_irPs3BlockLimit, (unsigned)blocks_.GetArenaSize(), (unsigned)g_irPs3ArenaLimit, (unsigned)(PS3Profile::jitCodeSpaceLeft / 1024));
		}
		if (g_irPs3CacheFullHook && blocks_.GetNumBlocks() >= g_irPs3BlockLimit)
			g_irPs3CacheFullHook(blocks_.GetNumBlocks());
#endif
		ClearCache();
		CompileBlock(em_address, instructions, mipsBytes);
	}

	if (frontend_.CheckRounding(em_address)) {
		// Our assumptions are all wrong so it's clean-slate time.
		frontend_.prefixTargetsToInvalidate.clear();
		ClearCache();
		CompileBlock(em_address, instructions, mipsBytes);
	} else if (!frontend_.prefixTargetsToInvalidate.empty()) {
		// Blocks compiled assuming default VFPU prefixes where this one leaves one set.
		std::vector<u32> targets;
		targets.swap(frontend_.prefixTargetsToInvalidate);
		for (u32 target : targets)
			InvalidateCacheAt(target, 4);
	}
}

// WARNING! This can be called from IRInterpret / the JIT, through the function preload stuff!
bool IRJit::CompileBlock(u32 em_address, std::vector<IRInst> &instructions, u32 &mipsBytes) {
	_dbg_assert_(compilerEnabled_);

	{
		PS3_PROFILE(jitFrontend);
		frontend_.DoJit(em_address, instructions, mipsBytes, !UnoptimizedFirst());
	}
	_dbg_assert_(!instructions.empty());

#ifdef PPSSPP_RSX_HOSTSIM
	// Host test build: PPC_XPROF=1 also runs the PS3 JIT's code generator on every block (the code
	// isn't run), so callgrind can show what compiling costs.
	{
		void PPCXProfCompile(u32 em_address, u32 mipsBytes, const std::vector<IRInst> &instructions);
		static const bool xprof = getenv("PPC_XPROF") != nullptr;
		if (xprof && !compileToNative_)
			PPCXProfCompile(em_address, mipsBytes, instructions);
		void PPCBlockTest(const char *path);
		if (const char *bt = getenv("PPC_BLOCKTEST")) {
			PPCBlockTest(bt);
			exit(0);
		}
	}
#endif
	int block_num = blocks_.AllocateBlock(em_address, mipsBytes, instructions);
	if ((block_num & ~MIPS_EMUHACK_VALUE_MASK) != 0) {
		WARN_LOG(Log::JIT, "Failed to allocate block for %08x (%d instructions)", em_address, (int)instructions.size());
		// Out of block numbers.  Caller will handle.
		return false;
	}

	IRBlock *b = blocks_.GetBlock(block_num);
	if (mipsTracer.tracing_enabled || compileToNative_) {
		// Hash, then only update page stats, don't link yet.
		// Native: the hash lets a block of rewritten code be reused when the same code comes back.
		b->UpdateHash();
	}

#if PPSSPP_ARCH(POWERPC)
	PS3Profile::Scope nativeScope(PS3Profile::jitNative);
#endif
	if (!CompileNativeBlock(&blocks_, block_num))
		return false;

	if (mipsTracer.tracing_enabled) {
		mipsTracer.prepare_block(b, blocks_);
	}

	// Updates stats, also patches the first MIPS instruction into an emuhack if 'preload == false'
	blocks_.FinalizeBlock(block_num);
	{
#if PPSSPP_ARCH(POWERPC)
		PS3_PROFILE(jitLink);
#endif
		FinalizeNativeBlock(&blocks_, block_num);
	}
	return true;
}

// Diagnostics (PS3 "nanwatch=1", IR interpreter only): after each block, float or VFPU registers
// that hold a NaN now but didn't before are reported once per block - which game code first makes
// a NaN (Dead to Rights' disarm camera). Off: one predictable branch per block.
// Diagnostics (PS3 "storewatch=1", IR interpreter only): the last 16-bit and unaligned stores, with
// the block that made them, so that the GE side can name the code that wrote vertex data it finds
// half-written (Call of Duty's rainbow walls: 16-bit colours in 8888 slots).
bool g_irStoreWatch = false;
u32 g_irStoreWatchBlock = 0;
StoreWatchRec *g_irStoreRing = nullptr;
u32 g_irStoreRingPos = 0;

void IRStoreWatchEnable(bool on) {
	if (on && !g_irStoreRing) {
		g_irStoreRing = new (std::nothrow) StoreWatchRec[STORE_WATCH_RING];
		if (g_irStoreRing)
			memset(g_irStoreRing, 0, sizeof(StoreWatchRec) * STORE_WATCH_RING);
	}
	g_irStoreWatch = on && g_irStoreRing != nullptr;
}

void IRStoreWatchNote(u32 addr, u32 value, u8 kind) {
	StoreWatchRec &r = g_irStoreRing[g_irStoreRingPos++ & (STORE_WATCH_RING - 1)];
	r.block = g_irStoreWatchBlock;
	r.addr = addr & 0x3FFFFFFF;
	r.value = value;
	r.kind = kind;
}

bool g_irNaNWatch = false;
// Also the 128 VFPU registers (nanwatch=2); otherwise only the 32 FPU ones, which is much cheaper.
bool g_irNaNWatchVfpu = false;
// Infinities count too (nanwatch=4+): 0 * inf is where Dead to Rights' NaN came from.
bool g_irNaNWatchInf = false;
void (*g_irNaNWatchLog)(u32 blockStart, u32 blockBytes, int reg, u32 before, u32 after, const u32 *fpuBefore) = nullptr;

#if !PPSSPP_PLATFORM(PS3)
// Host test hook: PPSSPP_NANWATCH=1 turns it on with output to stderr.
static void HostNaNWatchLog(u32 blockStart, u32 blockBytes, int reg, u32 before, u32 after, const u32 *) {
	fprintf(stderr, "NANWATCH block %08x (%u) reg %d %08x -> %08x\n", blockStart, blockBytes / 4, reg, before, after);
}
static struct HostNaNWatchInit {
	HostNaNWatchInit() {
		if (getenv("PPSSPP_NANWATCH")) {
			g_irNaNWatch = true;
			g_irNaNWatchLog = &HostNaNWatchLog;
		}
	}
} s_hostNaNWatchInit;
#endif

// 7f800001 isn't counted: the PSP's own NaN, which the registers hold from boot and games save and
// restore all the time (Dead to Rights' log was full of those) - not a NaN being made.
static inline bool IsNaNBits(u32 v) { return (v & 0x7FFFFFFFu) > 0x7F800000u && v != 0x7F800001u; }
static inline bool IsBadBits(u32 v) {
	return IsNaNBits(v) || (g_irNaNWatchInf && (v & 0x7FFFFFFFu) == 0x7F800000u);
}
static inline bool WasBadBits(u32 v) { return g_irNaNWatchInf ? (v & 0x7FFFFFFFu) >= 0x7F800000u : (v & 0x7FFFFFFFu) > 0x7F800000u; }

struct NaNSnapshot {
	u32 bits[32 + 128];
};

static inline void TakeNaNSnapshot(const MIPSState *mips, NaNSnapshot *snap) {
	memcpy(snap->bits, mips->fi, 32 * 4);
	if (g_irNaNWatchVfpu)
		memcpy(snap->bits + 32, mips->vi, 128 * 4);
}

static void CheckNaNSnapshot(const MIPSState *mips, const NaNSnapshot &before, IRBlockCache &blocks, u32 offset) {
	static std::set<u32> reported;
	if (reported.size() >= 400 || !g_irNaNWatchLog)
		return;
	// Quick pass: nothing in the FPU registers looks like a new NaN (the usual case).
	bool any = false;
	for (int i = 0; i < 32 && !any; ++i)
		any = IsBadBits(mips->fi[i]) && mips->fi[i] != before.bits[i];
	if (!any && g_irNaNWatchVfpu && memcmp(mips->vi, before.bits + 32, 128 * 4) != 0) {
		for (int i = 0; i < 128 && !any; ++i)
			any = IsBadBits(mips->vi[i]) && mips->vi[i] != before.bits[32 + i];
	}
	if (!any)
		return;
	const int regs = g_irNaNWatchVfpu ? 32 + 128 : 32;
	for (int i = 0; i < regs; ++i) {
		const u32 now = i < 32 ? mips->fi[i] : mips->vi[i - 32];
		if (IsBadBits(now) && !WasBadBits(before.bits[i])) {
			const IRBlock *block = blocks.GetBlock(blocks.GetBlockNumFromIRArenaOffset(offset));
			if (!block)
				return;
			u32 blockStart, blockBytes;
			block->GetRange(&blockStart, &blockBytes);
			if (!reported.insert(blockStart).second)
				return;
			g_irNaNWatchLog(blockStart, blockBytes, i, before.bits[i], now, before.bits);
			return;
		}
	}
}

void IRJit::RunLoopUntil(u64 globalticks) {
	PROFILE_THIS_SCOPE("jit");

	ApplyHostRoundingMode(mips_);
	// IR Dispatcher
	
	MIPSState *mips = mips_;
	while (true) {
		RestoreHostRoundingMode();
		CoreTiming::Advance(currentMIPS);
		ApplyHostRoundingMode(mips_);
		if (coreState != 0) {
			break;
		}

#ifdef _DEBUG
		compilerEnabled_ = false;
#endif
		while (mips->downcount >= 0) {
			u32 inst = Memory::ReadUnchecked_U32(mips->pc);
			u32 opcode = inst & 0xFF000000;
			if (opcode == MIPS_EMUHACK_OPCODE) {
				u32 offset = inst & 0x00FFFFFF; // Alternatively, inst - opcode
				const IRInst *instPtr = blocks_.GetArenaPtr() + offset;
				// First op is always, except when using breakpoints, downcount, to save one dispatch inside IRInterpret.
				// This branch is very cpu-branch-predictor-friendly so this still beats the dispatch.
				if (instPtr->op == IROp::Downcount) {
					mips->downcount -= instPtr->constant;
					instPtr++;
				}
#ifdef IR_PROFILING
				IRBlock *block = blocks_.GetBlock(blocks_.GetBlockNumFromIRArenaOffset(offset));
				Instant start = Instant::Now();
				mips->pc = IRInterpret(mips, instPtr);
				int64_t elapsedNanos = start.ElapsedNanos();
				block->profileStats_.executions += 1;
				block->profileStats_.totalNanos += elapsedNanos;
#else
				if (g_irStoreWatch) {
					const IRBlock *b = blocks_.GetBlock(blocks_.GetBlockNumFromIRArenaOffset(offset));
					g_irStoreWatchBlock = b ? b->GetOriginalStart() : 0;
				}
				if (g_irNaNWatch) {
					NaNSnapshot snap;
					TakeNaNSnapshot(mips, &snap);
					mips->pc = IRInterpret(mips, instPtr);
					CheckNaNSnapshot(mips, snap, blocks_, offset);
				} else {
					mips->pc = IRInterpret(mips, instPtr);
				}
#endif
				// Note: this will "jump to zero" on a badly constructed block missing exits.
				if (!Memory::IsValid4AlignedAddress(mips->pc)) {
					int blockNum = blocks_.GetBlockNumFromIRArenaOffset(offset);
					IRBlock *block = blocks_.GetBlockUnchecked(blockNum);
					Core_ExecException(mips->pc, block->GetOriginalStart(), ExecExceptionType::JUMP);
					break;
				}
			} else {
				RestoreHostRoundingMode();
#ifdef _DEBUG
				compilerEnabled_ = true;
#endif
				Compile(mips->pc);
#ifdef _DEBUG
				compilerEnabled_ = false;
#endif
				ApplyHostRoundingMode(mips_);
			}
		}
#ifdef _DEBUG
		compilerEnabled_ = true;
#endif
	}

	RestoreHostRoundingMode();
}

bool IRJit::DescribeCodePtr(const u8 *ptr, std::string &name) {
	// Used in native disassembly viewer.
	return false;
}

void IRJit::LinkBlock(u8 *exitPoint, const u8 *checkedEntry) {
	Crash();
}

void IRJit::UnlinkBlock(u8 *checkedEntry, u32 originalAddress) {
	Crash();
}

void IRBlockCache::Clear() {
	for (int i = 0; i < (int)blocks_.size(); ++i) {
		int cookie = compileToNative_ ? blocks_[i].GetNativeOffset() : blocks_[i].GetIRArenaOffset();
		blocks_[i].Destroy(cookie);
	}
	blocks_.clear();
	byPage_.clear();
	cookieToBlock_.clear();
	arena_.clear();
#if !PPSSPP_PLATFORM(PS3)
	arena_.shrink_to_fit();
#endif
}

#if PPSSPP_PLATFORM(PS3)
// The PS3 has ~25 MB of main memory left once a game runs. The arena (8 bytes per IR instruction,
// up to 128 MB elsewhere) grew all game long, and each time it doubled it needed the old and new
// copies at once: Dead to Rights ran out of memory after a few minutes and froze. A fixed 12 MB,
// reserved once; when it's full the cache is cleared (as when the code space is full).
// "JIT cache: Large" (build 143) raises it and the block limit, set before the core starts.
u32 g_irPs3ArenaLimit = 1536 * 1024;
#define ARENA_LIMIT g_irPs3ArenaLimit
#else
static const u32 ARENA_LIMIT = 0x1000000 - 1;  // 24 bits for offsets.
u32 g_irPs3ArenaLimit = ARENA_LIMIT;
#endif

int g_irPs3BlockLimit = 65536;
void (*g_irPs3CacheFullHook)(int blocks) = nullptr;

IRBlockCache::IRBlockCache(bool compileToNative) : compileToNative_(compileToNative) {}

int IRBlockCache::AllocateBlock(int emAddr, u32 origSize, const std::vector<IRInst> &insts) {
	int offset = (int)arena_.size();
	if (offset + insts.size() >= ARENA_LIMIT) {
		WARN_LOG(Log::JIT, "Filled JIT arena, restarting");
		return -1;
	}
#if PPSSPP_PLATFORM(PS3)
	if (arena_.capacity() < ARENA_LIMIT)
		arena_.reserve(ARENA_LIMIT);
	if ((int)blocks_.size() >= IR_PS3_BLOCK_LIMIT) {
		WARN_LOG(Log::JIT, "Too many JIT blocks, restarting");
		return -1;
	}
	if ((int)blocks_.capacity() < IR_PS3_BLOCK_LIMIT) {
		blocks_.reserve(IR_PS3_BLOCK_LIMIT);
		cookieToBlock_.reserve(IR_PS3_BLOCK_LIMIT);
	}
#endif
	arena_.insert(arena_.end(), insts.begin(), insts.end());
	int newBlockIndex = (int)blocks_.size();
	blocks_.push_back(IRBlock(emAddr, origSize, offset, (u32)insts.size()));
	return newBlockIndex;
}

bool IRBlockCache::ReplaceBlockIR(int blockNum, const std::vector<IRInst> &insts) {
	if (!compileToNative_ || blockNum < 0 || blockNum >= (int)blocks_.size())
		return false;
	IRBlock &b = blocks_[blockNum];
	if (insts.size() <= (size_t)b.GetNumIRInstructions()) {
		// Optimized IR is usually shorter than what the interpreter ran: overwrite it in place.
		std::copy(insts.begin(), insts.end(), arena_.begin() + b.GetIRArenaOffset());
		b.SetIR(b.GetIRArenaOffset(), (u32)insts.size());
		return true;
	}
	if (arena_.size() + insts.size() >= ARENA_LIMIT)
		return false;
	const u32 offset = (u32)arena_.size();
	arena_.insert(arena_.end(), insts.begin(), insts.end());
	blocks_[blockNum].SetIR(offset, (u32)insts.size());
	return true;
}

int IRBlockCache::GetBlockNumFromIRArenaOffset(int offset) const {
	// Block offsets are always in rising order (we don't go back and replace them when invalidated). So we can binary search.
	int low = 0;
	int high = (int)blocks_.size() - 1;
	int found = -1;
	while (low <= high) {
		int mid = low + (high - low) / 2;
		const int blockOffset = blocks_[mid].GetIRArenaOffset();
		if (blockOffset == offset) {
			found = mid;
			break;
		}
		if (blockOffset < offset) {
			low = mid + 1;
		} else {
			high = mid - 1;
		}
	}
	return found;
#if 1
	return found;
#else
	// Cross check the result. This is not fast so normally not enabled. Called a lot when IR_PROFILING is on.
	for (int i = 0; i < (int)blocks_.size(); i++) {
		if (blocks_[i].GetIRArenaOffset() == offset) {
			_dbg_assert_(i == found);
			return i;
		}
	}
	_dbg_assert_(found == -1);
	return -1;
#endif
}

std::vector<int> IRBlockCache::FindInvalidatedBlockNumbers(u32 address, u32 lengthInBytes) {
	u32 startPage = AddressToPage(address);
	u32 endPage = AddressToPage(address + lengthInBytes);

	std::vector<int> found;
	for (u32 page = startPage; page <= endPage; ++page) {
		const auto iter = byPage_.find(page);
		if (iter == byPage_.end())
			continue;

		const std::vector<int> &blocksInPage = iter->second;
		for (int i : blocksInPage) {
			if (blocks_[i].OverlapsRange(address, lengthInBytes)) {
				// We now try to remove these during invalidation.
				found.push_back(i);
			}
		}
	}

	return found;
}

void IRBlockCache::FinalizeBlock(int blockIndex) {
	// TODO: What's different about preload blocks?
	IRBlock &block = blocks_[blockIndex];
	int cookie = compileToNative_ ? block.GetNativeOffset() : block.GetIRArenaOffset();
	block.Finalize(cookie);
	NoteCookie(cookie, blockIndex);

	u32 startAddr, size;
	block.GetRange(&startAddr, &size);

	u32 startPage = AddressToPage(startAddr);
	u32 endPage = AddressToPage(startAddr + size);

	for (u32 page = startPage; page <= endPage; ++page) {
		byPage_[page].push_back(blockIndex);
	}
}

// Call after Destroy-ing it.
void IRBlockCache::RemoveBlockFromPageLookup(int blockIndex) {
	// We need to remove the block from the byPage lookup.
	IRBlock &block = blocks_[blockIndex];

	u32 startAddr, size;
	block.GetRange(&startAddr, &size);

	u32 startPage = AddressToPage(startAddr);
	u32 endPage = AddressToPage(startAddr + size);

	for (u32 page = startPage; page <= endPage; ++page) {
		auto iter = std::find(byPage_[page].begin(), byPage_[page].end(), blockIndex);
		if (iter != byPage_[page].end()) {
			byPage_[page].erase(iter);
		} else if (block.IsValid()) {
			// If it was previously invalidated, we don't care, hence the above check.
			WARN_LOG(Log::JIT, "RemoveBlock: Block at %08x was not found where expected in byPage table.", startAddr);
		}
	}

	// Additionally, we'd like to zap the block in the IR arena.
	// However, this breaks if calling sceKernelIcacheClearAll(), since as soon as we return, we'll be executing garbage.
	/*
	IRInst bad{ IROp::Bad };
	for (int off = block.GetIRArenaOffset(); off < (int)(block.GetIRArenaOffset() + block.GetNumIRInstructions()); off++) {
		arena_[off] = bad;
	}
	*/
}

u32 IRBlockCache::AddressToPage(u32 addr) const {
	// Use relatively small pages since basic blocks are typically small.
	return (addr & 0x3FFFFFFF) >> 10;
}

int IRBlockCache::FindPreloadBlock(u32 em_address) {
	u32 page = AddressToPage(em_address);
	auto iter = byPage_.find(page);
	if (iter == byPage_.end())
		return -1;

	const std::vector<int> &blocksInPage = iter->second;
	for (int i : blocksInPage) {
		if (blocks_[i].GetOriginalStart() == em_address) {
			if (blocks_[i].HashMatches()) {
				return i;
			}
		}
	}

	return -1;
}

int IRBlockCache::FindByCookie(int cookie) {
	if (blocks_.empty())
		return -1;

	// TODO: Maybe a flag to determine native offset mode?
	if (!compileToNative_) {
		return GetBlockNumFromIRArenaOffset(cookie);
	}

#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	{
		static int calls = 0, misses = 0;
		static const bool prof = getenv("JIT_PROF") != nullptr;
		if (prof && (++calls & 4095) == 0)
			fprintf(stderr, "JIT_PROF FindByCookie %d calls, %d map misses, %d blocks\n", calls, misses, GetNumBlocks());
		if (prof && cookieToBlock_.find(cookie) == cookieToBlock_.end())
			misses++;
	}
#endif
	auto it = cookieToBlock_.find(cookie);
	if (it != cookieToBlock_.end() && it->second < GetNumBlocks() && blocks_[it->second].GetNativeOffset() == cookie)
		return it->second;
	for (int i = 0; i < GetNumBlocks(); ++i) {
		int offset = blocks_[i].GetNativeOffset();
		if (offset == cookie) {
			cookieToBlock_[cookie] = i;
			return i;
		}
	}
	return -1;
}

std::vector<u32> IRBlockCache::SaveAndClearEmuHackOps() {
	std::vector<u32> result;
	result.resize(blocks_.size());

	for (int number = 0; number < (int)blocks_.size(); ++number) {
		IRBlock &b = blocks_[number];
		int cookie = compileToNative_ ? b.GetNativeOffset() : b.GetIRArenaOffset();
		if (b.IsValid() && b.RestoreOriginalFirstOp(cookie)) {
			result[number] = number;
		} else {
			result[number] = 0;
		}
	}

	return result;
}

void IRBlockCache::RestoreSavedEmuHackOps(const std::vector<u32> &saved) {
	if ((int)blocks_.size() != (int)saved.size()) {
		ERROR_LOG(Log::JIT, "RestoreSavedEmuHackOps: Wrong saved block size.");
		return;
	}

	for (int number = 0; number < (int)blocks_.size(); ++number) {
		IRBlock &b = blocks_[number];
		// Only if we restored it, write it back.
		if (b.IsValid() && saved[number] != 0 && b.HasOriginalFirstOp()) {
			int cookie = compileToNative_ ? b.GetNativeOffset() : b.GetIRArenaOffset();
			b.Finalize(cookie);
			NoteCookie(cookie, number);
		}
	}
}

JitBlockDebugInfo IRBlockCache::GetBlockDebugInfo(int blockNum) const {
	const IRBlock &ir = blocks_[blockNum];
	JitBlockDebugInfo debugInfo{};
	uint32_t start, size;
	ir.GetRange(&start, &size);
	debugInfo.originalAddress = start;  // TODO
	if (!Memory::IsValid4AlignedAddress(start)) {
		return debugInfo;
	}

	debugInfo.origDisasm.reserve(((start + size) - start) / 4);
	for (u32 addr = start; addr < start + size; addr += 4) {
		char temp[256];
		MIPSDisAsm(Memory::Read_Instruction(addr), addr, temp, sizeof(temp), true);
		std::string mipsDis = temp;
		debugInfo.origDisasm.push_back(mipsDis);
	}

	debugInfo.irDisasm.reserve(ir.GetNumIRInstructions());
	const IRInst *instructions = GetBlockInstructionPtr(ir);
	for (int i = 0; i < ir.GetNumIRInstructions(); i++) {
		IRInst inst = instructions[i];
		char buffer[256];
		DisassembleIR(buffer, sizeof(buffer), inst);
		debugInfo.irDisasm.push_back(buffer);
	}
	return debugInfo;
}

void IRBlockCache::ComputeStats(BlockCacheStats &bcStats) const {
	double totalBloat = 0.0;
	double maxBloat = 0.0;
	double minBloat = 1000000000.0;
	for (const auto &b : blocks_) {
		double codeSize = (double)b.GetNumIRInstructions() * 4;  // We count bloat in instructions, not bytes. sizeof(IRInst);
		if (codeSize == 0)
			continue;
		u32 origAddr, mipsBytes;
		b.GetRange(&origAddr, &mipsBytes);
		double origSize = (double)mipsBytes;
		double bloat = codeSize / origSize;
		if (bloat < minBloat) {
			minBloat = bloat;
			bcStats.minBloatBlock = origAddr;
		}
		if (bloat > maxBloat) {
			maxBloat = bloat;
			bcStats.maxBloatBlock = origAddr;
		}
		totalBloat += bloat;
	}
	bcStats.numBlocks = (int)blocks_.size();
	bcStats.minBloat = minBloat;
	bcStats.maxBloat = maxBloat;
	bcStats.avgBloat = totalBloat / (double)blocks_.size();
}

int IRBlockCache::GetBlockNumberFromStartAddress(u32 em_address) const {
	u32 page = AddressToPage(em_address);

	const auto iter = byPage_.find(page);
	if (iter == byPage_.end())
		return -1;

	const std::vector<int> &blocksInPage = iter->second;
	int best = -1;
	for (int i : blocksInPage) {
		if (blocks_[i].GetOriginalStart() == em_address) {
			best = i;
			if (blocks_[i].IsValid()) {
				return i;
			}
		}
	}
	return best;
}

bool IRBlock::HasOriginalFirstOp() const {
	return Memory::ReadUnchecked_U32(origAddr_) == origFirstOpcode_.encoding;
}

bool IRBlock::RestoreOriginalFirstOp(int cookie) {
	const u32 emuhack = MIPS_EMUHACK_OPCODE | cookie;
	if (Memory::ReadUnchecked_U32(origAddr_) == emuhack) {
		Memory::Write_Opcode_JIT(origAddr_, origFirstOpcode_);
		return true;
	}
	return false;
}

void IRBlock::Finalize(int cookie) {
	// Check it wasn't invalidated, in case this is after preload.
	// TODO: Allow reusing blocks when the code matches hash_ again, instead.
	if (origAddr_ && Memory::IsValid4AlignedAddress(origAddr_)) {
		origFirstOpcode_ = Memory::Read_Opcode_JIT(origAddr_);
		MIPSOpcode opcode = MIPSOpcode(MIPS_EMUHACK_OPCODE | cookie);
		Memory::Write_Opcode_JIT(origAddr_, opcode);
	} else {
		WARN_LOG(Log::JIT, "Finalizing invalid block (cookie: %d)", cookie);
	}
}

void IRBlock::Destroy(int cookie) {
	if (origAddr_ && Memory::IsValid4AlignedAddress(origAddr_)) {
		MIPSOpcode opcode = MIPSOpcode(MIPS_EMUHACK_OPCODE | cookie);
		u32 memOp = Memory::ReadUnchecked_U32(origAddr_);
		if (memOp == opcode.encoding) {
			Memory::Write_Opcode_JIT(origAddr_, origFirstOpcode_);
		} else {
			// NOTE: This is not an error. Just interesting to log.
			DEBUG_LOG(Log::JIT, "IRBlock::Destroy: Note: Block at %08x was overwritten - checked for %08x, got %08x when restoring the MIPS op to %08x", origAddr_, opcode.encoding, memOp, origFirstOpcode_.encoding);
		}
		// TODO: Also wipe the block in the IR opcode arena.
		// Let's mark this invalid so we don't try to clear it again.
		origAddr_ = 0;
	}
}

u64 IRBlock::CalculateHash() const {
	if (origAddr_) {
		// This is unfortunate. In case there are emuhacks, we have to make a copy.
		// If we could hash while reading we could avoid this.
		static std::vector<u32> buffer;  // Emulation thread only; no allocation per call.
		buffer.resize(origSize_ / 4);
		size_t pos = 0;
		for (u32 off = 0; off < origSize_; off += 4) {
			// Let's actually hash the replacement, if any.
			MIPSOpcode instr = Memory::ReadUnchecked_Instruction(origAddr_ + off, false);
			buffer[pos++] = instr.encoding;
		}
		return XXH3_64bits(&buffer[0], origSize_);
	}
	return 0;
}

bool IRBlock::OverlapsRange(u32 addr, u32 size) const {
	addr &= 0x3FFFFFFF;
	u32 origAddr = origAddr_ & 0x3FFFFFFF;
	return addr + size > origAddr && addr < origAddr + origSize_;
}

MIPSOpcode IRJit::GetOriginalOp(MIPSOpcode op) {
	IRBlock *b = blocks_.GetBlock(blocks_.FindByCookie(op.encoding & 0xFFFFFF));
	if (b) {
		return b->GetOriginalFirstOp();
	}
	return op;
}

}  // namespace MIPSComp
