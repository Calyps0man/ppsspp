// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ppsspp_config.h"
#include "Core/MIPS/PPC/PPCBuild.h"
#include "Common/FrontendTrace.h"
#ifdef PPSSPP_PPC_JIT_BUILD

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "Common/Log.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/MIPS/IR/IRInterpreter.h"
#include "GPU/PS3Profile.h"
#include "Core/MIPS/PPC/PPCJit.h"
#if !PPSSPP_PLATFORM(PS3)
static bool s_fusedInBlock_ = false;
#endif
#if PPSSPP_PLATFORM(PS3)
namespace PS3Frontend {
void Trace(const char *format, ...);
void TraceRaw(const char *text, unsigned length);
}
#endif
#include "Core/MIPS/PPC/PPCRegCache.h"
#include "PS3/jit/PS3JitMemory.h"

namespace MIPSComp {
extern bool g_ppcBlockRMChecked;
bool PPCJitRoundingOp(const IRInst &inst);

using namespace PPCGen;
using namespace PPCJitConstants;

#if (defined(PPSSPP_NO_GL) || defined(PPSSPP_RSX_HOSTSIM)) && !PPSSPP_PLATFORM(PS3)
int g_ppcJitTierRuns = getenv("PPC_TIER") ? atoi(getenv("PPC_TIER")) : 0;  // Test build.
#else
int g_ppcJitTierRuns = 20;
#endif
#if (defined(PPSSPP_NO_GL) || defined(PPSSPP_RSX_HOSTSIM)) && !PPSSPP_PLATFORM(PS3)
bool g_ppcJitVMX = getenv("PPC_VMX") != nullptr;  // Test build.
bool g_ppcJitStaticFPR = getenv("PPC_NOSFPR") == nullptr;  // Test build.
#else
bool g_ppcJitVMX = false;
bool g_ppcJitStaticFPR = true;
#endif

// A block must be able to hold "li r3,pc (2 instrs) + b dispatcher" when invalidated.
static constexpr int MIN_BLOCK_NORMAL_LEN = 16;
static_assert(MIN_BLOCK_NORMAL_LEN == 16, "savedEntries_ holds 16 bytes a block");
// An exit must be able to hold the unlinked form (li r3,pc + b) - patched to a single b.
static constexpr int MIN_BLOCK_EXIT_LEN = 12;

bool PPCJitAvailable() {
	return PS3Jit::Available();
}

PPCJitBackend::PPCJitBackend(JitOptions &jitopt, IRBlockCache &blocks)
	: IRNativeBackend(blocks), jo(jitopt), regs_(&jo) {
	jo.enablePointerify = false;
	jo.useStaticAlloc = true;
	jo.useBackJump = false;
	jo.optimizeForInterpreter = false;

	// Emuhack offsets are 24 bits, so 16 MB max; the PS3 area is 8 MB.
	AllocCodeSpace(8 * 1024 * 1024);
	regs_.Init(this);
}

static int s_moveShortcuts = 0;
static int s_fprLoadShortcuts = 0;
static int s_promoted = 0, s_stubs = 0;

PPCJitBackend::~PPCJitBackend() {
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	if (getenv("JIT_PROF"))
	{
		int patched = 0, mega = 0;
		for (const auto &ic : ics_) {
			patched += ic.patches > 0;
			mega += ic.patches >= 4;
		}
		fprintf(stderr, "JIT_PROF code bytes %llu, return caches %d (%d patched, %d gave up), float/GPR moves shortened %d, blocks interpreted first %d, compiled after %d\n", (unsigned long long)std::max(highWater_, (size_t)GetOffset(GetCodePtr())), (int)ics_.size(), patched, mega, s_moveShortcuts, s_stubs, s_promoted);
	}
#endif
}

void PPCJitBackend::Commit(const u8 *start, const u8 *end) {
	if (end <= start)
		return;
	if (deferCommits_) {
		pendingCommits_.emplace_back((int)(start - GetBasePtr()), (int)(end - GetBasePtr()));
		return;
	}
	CommitNow(start, end);
}

void PPCJitBackend::FinalizeBlockDone() {
	deferCommits_ = false;
	if (pendingCommits_.empty())
		return;
	std::sort(pendingCommits_.begin(), pendingCommits_.end());
	// Ranges a little apart are cheaper as one call than two: on the PS3 a commit costs ~33 us a
	// call plus ~4.5 ns a byte (Killzone and Gran Turismo logs), so gaps up to ~7 KB pay for
	// themselves. The gap is code already committed, identical in the writable copy.
	int s = pendingCommits_[0].first, e = pendingCommits_[0].second;
	for (size_t i = 1; i < pendingCommits_.size(); ++i) {
		const auto &r = pendingCommits_[i];
		if (r.first <= e + 6 * 1024) {
			e = std::max(e, r.second);
		} else {
			CommitNow(GetBasePtr() + s, GetBasePtr() + e);
			s = r.first;
			e = r.second;
		}
	}
	CommitNow(GetBasePtr() + s, GetBasePtr() + e);
	pendingCommits_.clear();
}

void PPCJitBackend::CommitNow(const u8 *start, const u8 *end) {
	size_t used = (size_t)(end - GetBasePtr());
	if (used > highWater_)
		highWater_ = used;
	u8 *writable = GetWritablePtrFromCodePtr(start);
	if (!PS3Jit::Commit(start, writable, end - start)) {
		if (!commitFailed_)
			ERROR_LOG(Log::JIT, "PPC JIT: committing code failed");
		commitFailed_ = true;
	}
}

void PPCJitBackend::EmitTierStub(IRBlockCache *irBlockCache, int block_num) {
	IRBlock *block = irBlockCache->GetBlock(block_num);
	const u8 *writeStart = GetCodePtr();
	BeginWrite(64);
	const u32 startPC = block->GetOriginalStart();
	// The same checked entry as a compiled block (links and inline caches go there), then a call
	// into the interpreter.
	const u8 *bailOut = GetCodePointer();
	LI32(SCRATCH1, startPC);
	B(outerLoopPCInSCRATCH1_);
	SetBlockCheckedOffset(block_num, (int)GetOffset(GetCodePointer()));
	CMPWI(DOWNCOUNTREG, 0);
	Write32((16u << 26) | (14u << 21) | (0u << 16) | ((u32)(bailOut - GetCodePointer()) & 0xFFFC));
	block->SetNativeOffset((int)GetOffset(GetCodePointer()));
	LIS(SCRATCH2, (s16)(block_num >> 16));
	ORI(SCRATCH2, SCRATCH2, (u16)(block_num & 0xFFFF));
	B(tierInterp_);
	// Room for "b <compiled code>" (and for InvalidateBlock's "li r3, pc; b dispatcher").
	ReserveCodeSpace(MIN_BLOCK_NORMAL_LEN - 12);
	EndWrite();
	Commit(writeStart, GetCodePtr());
	if ((int)tierRuns_.size() <= block_num)
		tierRuns_.resize(block_num + 1024);
	tierRuns_[block_num] = 0;
	s_stubs++;
}

bool PPCJit::UnoptimizedFirst() const {
	return g_ppcJitTierRuns > 0;
}

u32 PPCJitBackend::TierRun(int block_num) {
	if ((u32)block_num & 0x80000000u) {
		// A compiled block whose entry found a rounding mode other than nearest: interpret it.
		IRBlock *b = blocks_.GetBlock(block_num & 0x7FFFFFFF);
		return b ? IRInterpret(currentMIPS, blocks_.GetBlockInstructionPtr(*b)) : currentMIPS->pc;
	}
	IRBlock *block = blocks_.GetBlock(block_num);
	if (!block)
		return currentMIPS->pc;
	const int generation = clearGeneration_;
#if PPSSPP_PLATFORM(PS3)
	PS3Profile::JitProfExclude profExclude;  // The run and the compile after it.
	PS3Profile::jitInterpRuns++;
	const uint64_t interpStart = PS3Profile::Now();
#endif
	const u32 next = IRInterpret(currentMIPS, blocks_.GetBlockInstructionPtr(*block));
#if PPSSPP_PLATFORM(PS3)
	PS3Profile::jitInterp += PS3Profile::Now() - interpStart;
#endif
	// The run may have cleared the cache or invalidated this block (code loading, writes).
	if (generation == clearGeneration_ && block_num < (int)tierRuns_.size() && ++tierRuns_[block_num] >= g_ppcJitTierRuns) {
		block = blocks_.GetBlock(block_num);
		if (block && block->IsValid())
			PromoteBlock(block_num);
	}
	return next;
}

void PPCJitBackend::PromoteBlock(int block_num) {
	IRBlock *block = blocks_.GetBlock(block_num);
	const int stubOffset = block->GetNativeOffset();
	const IRNativeBlock *nativeBlock = GetNativeBlock(block_num);
	const int stubChecked = nativeBlock ? nativeBlock->checkedOffset : 0;
	tierRuns_[block_num] = 0xFFFF;  // Not again, whatever happens.
	if (GetSpaceLeft() < 0x10000)
		return;
#if PPSSPP_PLATFORM(PS3)
	PS3Profile::Scope compileScope(PS3Profile::jitCompile);
	PS3Profile::jitPromoted++;
#endif
	if (reoptimize_) {
		// It ran unoptimized in the interpreter: compile from the optimized IR.
#if PPSSPP_PLATFORM(PS3)
		PS3Profile::Scope frontScope(PS3Profile::jitFrontend);
#endif
		std::vector<IRInst> insts;
		u32 bytes = 0;
		u32 start = 0, size = 0;
		block->GetRange(&start, &size);
		reoptimize_(start, insts, bytes);
		if (!insts.empty() && bytes == size)
			blocks_.ReplaceBlockIR(block_num, insts);
		block = blocks_.GetBlock(block_num);
		block->UpdateHash();  // What was compiled now (ReviveBlock compares against it).
	}
#if PPSSPP_PLATFORM(PS3)
	PS3Profile::Scope nativeScope(PS3Profile::jitNative);
#endif
	promoting_ = true;
	bool ok;
	{
#if PPSSPP_PLATFORM(PS3)
		PS3Profile::Scope genScope(PS3Profile::jitCodeGen);
#endif
		ok = CompileBlock(&blocks_, block_num);
	}
	promoting_ = false;
	if (!ok) {
		block->SetNativeOffset(stubOffset);
		SetBlockCheckedOffset(block_num, stubChecked);
		return;
	}
	s_promoted++;
	const int nativeOffset = block->GetNativeOffset();
	// The PSP code's emuhack word now names the compiled code.
	block->RestoreOriginalFirstOp(stubOffset);
	block->Finalize(nativeOffset);
	blocks_.NoteCookie(nativeOffset, block_num);
	// Anything still arriving at the stub (old links, a cached return) goes on to the compiled code.
	const u8 *stub = GetBasePtr() + stubOffset;
	u8 *writable = GetWritablePtrFromCodePtr(stub);
	PPCEmitter e(stub, writable);
	e.B(GetBasePtr() + nativeOffset);
	CommitNow(stub, stub + 4);
	// Links in and out of the compiled code (and the deferred commits).
#if PPSSPP_PLATFORM(PS3)
	PS3Profile::Scope linkScope(PS3Profile::jitLink);
#endif
	FinalizeBlock(&blocks_, block_num, jo);
}

bool PPCJitBackend::CompileBlock(IRBlockCache *irBlockCache, int block_num) {
#if PPSSPP_ARCH(POWERPC)
	PS3Profile::jitCodeSpaceLeft = GetSpaceLeft();
#endif
	if (GetSpaceLeft() < 0x800)
		return false;
	if (g_ppcJitTierRuns > 0 && !promoting_ && tierInterp_) {
		EmitTierStub(irBlockCache, block_num);
		return true;
	}
	deferCommits_ = true;
	pendingCommits_.clear();

	IRBlock *block = irBlockCache->GetBlock(block_num);
	const u8 *writeStart = GetCodePtr();
	BeginWrite(std::min(GetSpaceLeft(), (size_t)block->GetNumIRInstructions() * 128));

	u32 startPC = block->GetOriginalStart();
	// Checked entry (used by links): bail to the outer loop when out of cycles. The bail code goes
	// before it, so the usual case falls straight through (a branch marked unlikely; a taken
	// branch on every block entry costs the PPU a fetch bubble).
	const u8 *bailOut = GetCodePointer();
	LI32(SCRATCH1, startPC);
	B(outerLoopPCInSCRATCH1_);
	SetBlockCheckedOffset(block_num, (int)GetOffset(GetCodePointer()));
	CMPWI(DOWNCOUNTREG, 0);
	// bc 14 (branch if CR0[LT], hint: not taken), 0, bailOut.
	Write32((16u << 26) | (14u << 21) | (0u << 16) | ((u32)(bailOut - GetCodePointer()) & 0xFFFC));

	const u8 *blockStart = GetCodePointer();
	block->SetNativeOffset((int)GetOffset(blockStart));
	compilingBlockNum_ = block_num;
#if PPSSPP_ARCH(POWERPC)
	if (PS3Profile::jitBlockProfile && block_num < PS3Profile::JIT_BLOCK_COUNTS) {
		// Entry counter, and the time since the previous block entry charged to that block (so
		// a block's time includes the helpers and HLE calls it makes). PS3 pointers are 32-bit;
		// r0, r3, r4, r11 and r12 are free at a block entry.
		LI32(SCRATCH1, (u32)(uintptr_t)&PS3Profile::jitBlockCounts[block_num]);
		LWZ(SCRATCH2, SCRATCH1, 0);
		ADDI(SCRATCH2, SCRATCH2, 1);
		STW(SCRATCH2, SCRATCH1, 0);
		MFSPR(SCRATCH1, 268);  // Time base.
		LI32(SCRATCH2, (u32)(uintptr_t)&PS3Profile::jitProfState);
		LD(R11, SCRATCH2, 0);
		STD(SCRATCH1, SCRATCH2, 0);
		SUBF(R11, R11, SCRATCH1);
		LWZ(R12, SCRATCH2, 8);
		LI32(SCRATCH1, (u32)(uintptr_t)&PS3Profile::jitBlockTicks[0]);
		LWZX(R0, SCRATCH1, R12);
		ADD(R0, R0, R11);
		STWX(R0, SCRATCH1, R12);
		LI32(R12, (u32)block_num * 4);
		STW(R12, SCRATCH2, 8);
	}
#endif

	regs_.staticMismatch = false;
#if !PPSSPP_PLATFORM(PS3)
	s_fusedInBlock_ = false;
#endif
	regs_.Start(irBlockCache, block_num);
	memset(gprHoldsFpr_, -1, sizeof(gprHoldsFpr_));

	const IRInst *instructions = irBlockCache->GetBlockInstructionPtr(*block);
	// AltiVec mode, after the game has written fcr31: one rounding-mode check at the block entry
	// instead of one per FPU op (most games keep rounding to nearest; the per-op check cost a load,
	// a compare and a branch per add/multiply). Not round-to-nearest: the IR interpreter runs the
	// block. Blocks that write fcr31 themselves keep the per-op checks.
	g_ppcBlockRMChecked = false;
	if (regs_.UsesVMX() && g_irRoundingModeUsed && tierInterp_) {
		bool rounding = false, writesFcr31 = false;
		for (int i = 0; i < block->GetNumIRInstructions(); ++i) {
			const IRInst &inst = instructions[i];
			rounding = rounding || PPCJitRoundingOp(inst);
			switch (inst.op) {
			case IROp::FpCtrlFromReg: case IROp::UpdateRoundingMode: case IROp::ApplyRoundingMode: case IROp::RestoreRoundingMode:
				writesFcr31 = true;
				break;
			default: {
				const IRMeta *m = GetIRMeta(inst.op);
				if (m && m->types[0] == 'G' && inst.dest == IRREG_FCR31)
					writesFcr31 = true;
				break;
			}
			}
		}
		if (rounding && !writesFcr31) {
			LWZ(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, fcr31));
			ANDI_(SCRATCH1, SCRATCH1, 3);
			FixupBranch notNearest = BC(Cond::NE);
			const u32 interpOnly = 0x80000000u | (u32)block_num;
			AddFarCode(notNearest, [this, interpOnly]() {
				LIS(SCRATCH2, (s16)(interpOnly >> 16));
				ORI(SCRATCH2, SCRATCH2, (u16)(interpOnly & 0xFFFF));
				B(tierInterp_);
			});
			g_ppcBlockRMChecked = true;
		}
	}
	cmpMaskReg_ = -1;
	numFprLoads_ = 0;
#if PPSSPP_PLATFORM(PS3)
	const uint64_t loopStart = PS3Profile::Now();
#endif
	for (int i = 0; i < block->GetNumIRInstructions(); ++i) {
#if !PPSSPP_PLATFORM(PS3)
		{
			// Test builds: PPC_FAKE_STATIC=1 fails each block that fused something (compiled again
			// without), =2 every block (interpreted), to exercise the fallback.
			static const int fake = getenv("PPC_FAKE_STATIC") ? atoi(getenv("PPC_FAKE_STATIC")) : 0;
			if ((fake == 2 && i == 1) || (fake == 1 && !noFuse_ && s_fusedInBlock_))
				regs_.staticMismatch = true;
		}
#endif
		if (regs_.staticMismatch)
			break;
		const IRInst &inst = instructions[i];
		regs_.SetIRIndex(i);
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
		static const char *genericOps = getenv("PPC_GENERIC");
		if (genericOps && GetIRMeta(inst.op) && strstr(genericOps, GetIRMeta(inst.op)->name)) {
			CompIR_Generic(inst);
			continue;
		}
#endif
		if (g_irNativeGenericOp[(uint8_t)inst.op]) {
			// jitops (diagnostics): this op in the IR interpreter. It may write any register: what the
			// shortcuts track starts over.
			g_irNativeGenericOpCount++;
			CompIR_Generic(inst);
			numFprLoads_ = 0;
			cmpMaskReg_ = -1;
			memset(gprHoldsFpr_, -1, sizeof(gprHoldsFpr_));
			continue;
		}
		int handled = 0;
		// jitgeneric (diagnostics): the fused patterns are float/vector code, the shortcuts GPR<->FPR
		// moves; 64 turns off both, the float/vector groups the patterns.
		const bool fuse = !noFuse_ && regs_.UsesVMX() && !(g_irNativeGenericMask & (4 | 8 | 16 | 32 | 64));
		const bool shortcuts = !(g_irNativeGenericMask & (16 | 64));
		if (inst.op == IROp::LoadFloat && fuse)
			handled = VMX_FloatCopy(instructions, i, block->GetNumIRInstructions());
		if (!handled && (inst.op == IROp::LoadFloat || inst.op == IROp::StoreFloat) && fuse)
			handled = VMX_MatrixRows(instructions, i, block->GetNumIRInstructions());
		else if (inst.op == IROp::Vec4Init && fuse)
			handled = VMX_Vec4InitScalars(instructions, i, block->GetNumIRInstructions());
		else if (inst.op == IROp::FMul && fuse) {
			handled = VMX_DotQuad(instructions, i, block->GetNumIRInstructions());
			if (!handled)
				handled = VMX_Dot3(instructions, i, block->GetNumIRInstructions());
			if (!handled)
				handled = VMX_LaneOp4(instructions, i, block->GetNumIRInstructions());
		} else if ((inst.op == IROp::FAdd || inst.op == IROp::FSub || inst.op == IROp::FMin || inst.op == IROp::FMax) && fuse) {
			handled = VMX_LaneOp4(instructions, i, block->GetNumIRInstructions());
		} else if (inst.op == IROp::FCmp && fuse) {
			handled = VMX_Compare4(instructions, i, block->GetNumIRInstructions());
			if (!handled)
				handled = VMX_CompareToFloat(instructions, i, block->GetNumIRInstructions());
		}
		else if (inst.op == IROp::SetConstF && fuse)
			handled = VMX_HSum4(instructions, i, block->GetNumIRInstructions());
		if (handled) {
#if !PPSSPP_PLATFORM(PS3)
			s_fusedInBlock_ = true;
#endif
			for (int k = 0; k < handled; ++k)
				MoveCacheBefore(instructions[i + k]);
			i += handled - 1;
		} else {
			MoveCacheBefore(inst);
			if (!shortcuts || (!CompareMaskShortcut(inst) && !FprLoadShortcut(inst) && !MoveCacheShortcut(inst)))
				CompileIRInst(inst);
			MoveCacheAfter(inst);
		}
		// A fused group: the state after its last instruction (no compare mask left to track).
		TrackCompareMask(handled ? instructions[i] : inst);
		TrackFprLoads(inst);
		// Conditional branches reach +-32 KB: don't let far code drift too far from its users.
		if (!farCode_.empty() && (int)GetOffset(GetCodePointer()) - farFirstOffset_ > 16 * 1024)
			FlushFarCode();

		if (GetSpaceLeft() < 0x800) {
			farCode_.clear();
			farFirstOffset_ = -1;
			compilingBlockNum_ = -1;
			deferCommits_ = false;
			pendingCommits_.clear();
			EndWrite();
			return false;
		}
	}

#if PPSSPP_PLATFORM(PS3)
	PS3Profile::jitCodeLoop += PS3Profile::Now() - loopStart;
#endif
	if (regs_.staticMismatch) {
		// A group mapping needed a static register (Midnight Club L.A., Frontend 105: the assert, then
		// a hang). Throw this code away; again without the fused patterns, else the interpreter.
		farCode_.clear();
		farFirstOffset_ = -1;
		compilingBlockNum_ = -1;
		deferCommits_ = false;
		pendingCommits_.clear();
		EndWrite();
		static int logged = 0;
		if (logged < 4 && FrontendLogOn(FrontendLog::JIT)) {
			logged++;
#if PPSSPP_PLATFORM(PS3)
			PS3Frontend::Trace("JIT: block %08x mapped a static register in a group (%s); IR follows", startPC, noFuse_ ? "without fused patterns too: interpreted" : "compiling it again without fused patterns");
			if (!noFuse_) {
				std::string text;
				for (int k = 0; k < block->GetNumIRInstructions() && k < 80; ++k) {
					char buf[256];
					DisassembleIR(buf, sizeof(buf), instructions[k]);
					text += "  ir   ";
					text += buf;
					text += "\n";
				}
				PS3Frontend::TraceRaw(text.c_str(), (unsigned)text.size());
			}
#else
			fprintf(stderr, "PPC JIT: block %08x mapped a static register in a group (%s)\n", startPC, noFuse_ ? "interpreted" : "again without fusing");
#endif
		}
		if (noFuse_) {
			// Promoting: the caller keeps the interpreter stub. A first compile (no tiering): a stub.
			if (!promoting_ && tierInterp_) {
				EmitTierStub(irBlockCache, block_num);
				if (block_num < (int)tierRuns_.size())
					tierRuns_[block_num] = 0xFFFF;
				return true;
			}
			return false;
		}
		noFuse_ = true;
		const bool ok = CompileBlock(irBlockCache, block_num);
		noFuse_ = false;
		return ok;
	}

	FlushFarCode();
	int len = (int)GetOffset(GetCodePointer()) - block->GetNativeOffset();
#if PPSSPP_PLATFORM(PS3)
	if (block_num < PS3Profile::JIT_BLOCK_COUNTS)
	{
		PS3Profile::jitBlockNativeBytes[block_num] = (uint16_t)std::min(len, 65535);
		PS3Profile::jitBlockNativeOffset[block_num] = (uint32_t)block->GetNativeOffset();
	}
#endif
	if (len < MIN_BLOCK_NORMAL_LEN)
		ReserveCodeSpace(MIN_BLOCK_NORMAL_LEN - len);

	EndWrite();
	Commit(writeStart, GetCodePtr());
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	// Test build: PPC_JITDUMP=dir writes each block's code (raw, for objdump) and its IR.
	static const char *dumpDir = getenv("PPC_JITDUMP");
	if (dumpDir) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%08x.bin", dumpDir, startPC);
		if (FILE *f = fopen(path, "wb")) {
			fwrite(GetWritablePtrFromCodePtr(GetBasePtr() + block->GetNativeOffset()), 1, len, f);
			fclose(f);
		}
		snprintf(path, sizeof(path), "%s/%08x.ir", dumpDir, startPC);
		if (FILE *f = fopen(path, "w")) {
			for (int i = 0; i < block->GetNumIRInstructions(); ++i) {
				char buf[256];
				DisassembleIR(buf, sizeof(buf), instructions[i]);
				fprintf(f, "%s\n", buf);
			}
			fclose(f);
		}
	}
#endif
	compilingBlockNum_ = -1;
	return true;
}

// Float bits moved to a GPR (mfc1/mfv) and back to a float register, or on to another GPR, go
// through memory each way on the PPU (no direct GPR<->FPR/VR moves), a ~40-cycle load-hit-store
// stall each. gprHoldsFpr_[g] remembers which float register's bits GPR g still holds (until either
// changes), so those become plain register moves.
void PPCJitBackend::MoveCacheBefore(const IRInst &inst) {
	const IRMeta *meta = GetIRMeta(inst.op);
	if (!meta || (meta->flags & IRFLAG_BARRIER)) {
		memset(gprHoldsFpr_, -1, sizeof(gprHoldsFpr_));
		return;
	}
	if (meta->flags & IRFLAG_SRC3)
		return;
	auto dropFpr = [&](int first, int count) {
		for (int g = 0; g < 32; ++g) {
			if (gprHoldsFpr_[g] >= first && gprHoldsFpr_[g] < first + count)
				gprHoldsFpr_[g] = -1;
		}
	};
	switch (meta->types[0]) {
	case 'G':
		if (inst.dest < 32)
			gprHoldsFpr_[inst.dest] = -1;
		break;
	case 'F': dropFpr(inst.dest, 1); break;
	case '2': dropFpr(inst.dest, 2); break;
	case 'V': dropFpr(inst.dest, 4); break;
	default: break;
	}
}

bool PPCJitBackend::MoveCacheShortcut(const IRInst &inst) {
	if (inst.op == IROp::FMovFromGPR && inst.src1 < 32 && inst.src1 != 0 && gprHoldsFpr_[inst.src1] >= 0) {
		IRInst mov = inst;
		mov.op = IROp::FMov;
		mov.src1 = (IRReg)gprHoldsFpr_[inst.src1];
		if (mov.src1 != inst.dest)
			CompileIRInst(mov);
		s_moveShortcuts++;
		return true;
	}
	if (inst.op == IROp::FMovToGPR && inst.dest < 32 && inst.dest != 0) {
		for (int g = 1; g < 32; ++g) {
			if (g != inst.dest && gprHoldsFpr_[g] == inst.src1) {
				IRInst mov = inst;
				mov.op = IROp::Mov;
				mov.src1 = (IRReg)g;
				CompileIRInst(mov);
				s_moveShortcuts++;
				return true;
			}
		}
	}
	return false;
}

// AltiVec mode: vslt/vsge (and c.cond.s followed by a move of the condition into an FPR) compare,
// copy fpcond to a GPR, maybe flip it, then move that GPR into an FPR - a store and reload through
// the stack, a load-hit-store stall on the PPU (11 of them in a Silent Hill Origins routine run
// ~50,000 times a frame). The FCmp's mask is still in v2: build the 0 or 1 from it instead.
void PPCJitBackend::TrackCompareMask(const IRInst &inst) {
	if (!regs_.UsesVMX()) {
		cmpMaskReg_ = -1;
		return;
	}
	switch (inst.op) {
	case IROp::FCmp:
		// VMX_FCompare leaves the compare mask in v2 (not for "false", which sets no mask).
		cmpMaskReg_ = inst.dest == IRFpCompareMode::False ? -1 : IRREG_FPCOND;
		cmpMaskInverted_ = false;
		return;
	case IROp::FpCondToReg:
		if (cmpMaskReg_ == IRREG_FPCOND) {
			cmpMaskReg_ = inst.dest;
			return;
		}
		break;
	case IROp::XorConst:
		if (cmpMaskReg_ >= 0 && cmpMaskReg_ != IRREG_FPCOND && inst.dest == cmpMaskReg_ && inst.src1 == cmpMaskReg_ && inst.constant == 1) {
			cmpMaskInverted_ = !cmpMaskInverted_;
			return;
		}
		break;
	default:
		break;
	}
	cmpMaskReg_ = -1;
}

bool PPCJitBackend::CompareMaskShortcut(const IRInst &inst) {
	if (inst.op != IROp::FMovFromGPR || cmpMaskReg_ < 0 || cmpMaskReg_ == IRREG_FPCOND || inst.src1 != cmpMaskReg_)
		return false;
	// Only the FPR is mapped (the regcache uses v3, not v0-v2); the GPR keeps its value.
	const PPCReg d = regs_.MapVR(inst.dest, 1, MIPSMap::NOINIT);
	VSPLTISW(V0, 1);
	if (cmpMaskInverted_)
		VANDC(d, V0, V2);
	else
		VAND(d, V2, V0);
	regs_.MarkVRDirty(inst.dest);
	return true;
}

// AltiVec mode: lwc1 then mfc1 of the same register (copies through FPU registers, common in
// compiled PSP code) went memory -> VMX register -> stack -> GPR, a load-hit-store stall. The GPR
// can load the word from memory itself, as long as nothing could have changed it since.
void PPCJitBackend::TrackFprLoads(const IRInst &inst) {
	if (!regs_.UsesVMX()) {
		numFprLoads_ = 0;
		return;
	}
	auto dropFpr = [&](IRReg f) {
		int kept = 0;
		for (int i = 0; i < numFprLoads_; ++i)
			if (fprLoads_[i].fpr != f)
				fprLoads_[kept++] = fprLoads_[i];
		numFprLoads_ = kept;
	};
	switch (inst.op) {
	case IROp::LoadFloat:
		dropFpr(inst.dest);
		if (numFprLoads_ == 8)
			dropFpr(fprLoads_[0].fpr);
		fprLoads_[numFprLoads_++] = { inst.dest, inst.src1, inst.constant };
		return;
	case IROp::FMovToGPR: {
		// Writes a GPR: loads addressed through it are no longer known.
		int kept = 0;
		for (int i = 0; i < numFprLoads_; ++i)
			if (fprLoads_[i].base != inst.dest)
				fprLoads_[kept++] = fprLoads_[i];
		numFprLoads_ = kept;
		return;
	}
	default:
		// Anything else might store, write an address register or an FPR: start over.
		numFprLoads_ = 0;
		return;
	}
}

bool PPCJitBackend::FprLoadShortcut(const IRInst &inst) {
	if (inst.op != IROp::FMovToGPR || inst.dest == MIPS_REG_ZERO || inst.dest >= 32)
		return false;
	for (int i = 0; i < numFprLoads_; ++i) {
		if (fprLoads_[i].fpr != inst.src1)
			continue;
		IRInst load{};
		load.op = IROp::Load32;
		load.dest = inst.dest;
		load.src1 = fprLoads_[i].base;
		load.constant = fprLoads_[i].offset;
		CompileIRInst(load);
		s_fprLoadShortcuts++;
		return true;
	}
	return false;
}

void PPCJitBackend::MoveCacheAfter(const IRInst &inst) {
	// Not the IR's temporaries (they may be dropped once dead).
	if (inst.op == IROp::FMovToGPR && inst.dest < 32 && inst.dest != 0 && inst.src1 < IRVTEMP_PFX_S)
		gprHoldsFpr_[inst.dest] = (int16_t)inst.src1;
	else if (inst.op == IROp::FMovFromGPR && inst.src1 < 32 && inst.src1 != 0 && inst.dest < IRVTEMP_PFX_S && gprHoldsFpr_[inst.src1] < 0)
		gprHoldsFpr_[inst.src1] = (int16_t)inst.dest;
}

void PPCJitBackend::AddFarCode(FixupBranch from, std::function<void()> emit) {
	if (farCode_.empty())
		farFirstOffset_ = (int)GetOffset(GetCodePointer());
	farCode_.emplace_back(from, std::move(emit));
}

void PPCJitBackend::FlushFarCode() {
	if (farCode_.empty())
		return;
	// Unreachable when the block just ended with an exit; otherwise jumps over the far code.
	FixupBranch over = B();
	for (auto &f : farCode_) {
		SetJumpTarget(f.first);
		f.second();
	}
	farCode_.clear();
	farFirstOffset_ = -1;
	SetJumpTarget(over);
}

void PPCJitBackend::WriteConstExit(uint32_t pc) {
	int block_num = blocks_.GetBlockNumberFromStartAddress(pc);
	const IRNativeBlock *nativeBlock = GetNativeBlock(block_num);

	int exitStart = (int)GetOffset(GetCodePointer());
	if (block_num >= 0 && jo.enableBlocklink && nativeBlock && nativeBlock->checkedOffset != 0) {
		B(GetBasePtr() + nativeBlock->checkedOffset);
	} else {
		LI32(SCRATCH1, pc);
		B(dispatcherPCInSCRATCH1_);
	}

	if (jo.enableBlocklink) {
		int len = (int)GetOffset(GetCodePointer()) - exitStart;
		if (len < MIN_BLOCK_EXIT_LEN) {
			ReserveCodeSpace(MIN_BLOCK_EXIT_LEN - len);
			len = MIN_BLOCK_EXIT_LEN;
		}
		AddLinkableExit(compilingBlockNum_, pc, exitStart, len);
	}
}

void PPCJitBackend::OverwriteExit(int srcOffset, int len, int block_num) {
	_dbg_assert_(len >= MIN_BLOCK_EXIT_LEN);
	const IRNativeBlock *nativeBlock = GetNativeBlock(block_num);
	if (nativeBlock) {
		u8 *writable = GetWritablePtrFromCodePtr(GetBasePtr()) + srcOffset;
		PPCEmitter emitter(GetBasePtr() + srcOffset, writable);
		emitter.B(GetBasePtr() + nativeBlock->checkedOffset);
		int bytesWritten = (int)(emitter.GetWritableCodePtr() - writable);
		if (bytesWritten < len)
			emitter.ReserveCodeSpace(len - bytesWritten);
		Commit(GetBasePtr() + srcOffset, GetBasePtr() + srcOffset + len);
	}
}

// Helpers called from generated code (through their descriptors).
// How often each IR op had to go through the interpreter (for the perf log).
static uint32_t s_genericCounts[256];

// Which MIPS instructions the IR couldn't translate (IROp::Interpret), by name.
struct InterpretCount {
	const char *name;
	uint32_t count;
};
static InterpretCount s_interpretCounts[32];

static void CountInterpret(u32 opcode) {
	const char *name = MIPSGetName(MIPSOpcode(opcode));
	for (auto &c : s_interpretCounts) {
		if (c.name == name || !c.name) {
			c.name = name;
			c.count++;
			return;
		}
	}
}

static u32 DoIRInstHelper(u64 value) {
	IRInst inst[2]{};
	memcpy(&inst[0], &value, sizeof(value));
	s_genericCounts[(uint8_t)inst[0].op]++;
	if (inst[0].op == IROp::Interpret)
		CountInterpret(inst[0].constant);
	// Falling through returns 0 (= keep running the block), unlike the stock DoIRInst whose
	// ExitToPC would return the stale mips->pc for a conditional exit that isn't taken.
	inst[1].op = IROp::ExitToConst;
	inst[1].constant = 0;
	return IRInterpret(currentMIPS, &inst[0]);
}

void PPCJitBackend::CompIR_Generic(IRInst inst) {
	// Run this one instruction in the IR interpreter.
	uint64_t value;
	memcpy(&value, &inst, sizeof(inst));

	FlushAll();
	SaveStaticRegisters();
	LI64(SCRATCH1, value);
	QuickCallFunction(&DoIRInstHelper);
	LoadStaticRegisters();

	if ((GetIRMeta(inst.op)->flags & IRFLAG_EXIT) != 0) {
		// Nonzero result: the PC to continue at. Go through the coreState check.
		CMPWI(SCRATCH1, 0);
		FixupBranch noExit = BC(Cond::EQ);
		STW(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, pc));
		B(dispatcherCheckCoreState_);
		SetJumpTarget(noExit);
	}
}

// svl.q/svr.q (store the left/right part of a vector to an unaligned address): when the IR
// can't pair them into one 16-byte store, a direct helper instead of the interpreter fallback
// (which writes back every register; Gran Turismo runs thousands of these a frame).
static void SvlrHelper(u32 addr, u32 vtAndRight) {
	float d[4];
	ReadVector(currentMIPS, d, V_Quad, (int)(vtAndRight >> 1));
	if (!Memory::IsValid4AlignedAddress(addr))
		return;
	const int offset = (addr >> 2) & 3;
	if ((vtAndRight & 1) == 0) {
		for (int i = 0; i < offset + 1; i++)
			Memory::WriteUnchecked_Float(d[3 - i], addr - i * 4);
	} else {
		for (int i = 0; i < (3 - offset) + 1; i++)
			Memory::WriteUnchecked_Float(d[i], addr + 4 * i);
	}
}

static void VfpuInterpretHelper(u32 op) {
	s_genericCounts[(uint8_t)IROp::Interpret]++;
	CountInterpret(op);
	MIPSInterpret(currentMIPS, MIPSOpcode(op));
}

// VFPU instructions that only read and write VFPU registers and the VFPU control registers.
static bool IsVfpuOnlyOp(u32 op) {
	static const char *const names[] = { "vscmp", "vsrt1", "vsrt2", "vsrt3", "vsrt4", "vuc2ifs", "vbfy1", "vbfy2" };
	const char *name = MIPSGetName(MIPSOpcode(op));
	if (!name)
		return false;
	for (const char *n : names) {
		if (strcmp(name, n) == 0)
			return true;
	}
	return false;
}

void PPCJitBackend::CompIR_Interpret(IRInst inst) {
	const u32 op = inst.constant;
	if (IsVfpuOnlyOp(op)) {
		// Straight into the interpreter's function: no other register is touched, so only the
		// mapped ones go back to memory (not all the static ones there and back, as for any op).
		FlushAll();
		SaveVolatileStatics();
		LI32(SCRATCH1, op);
		QuickCallFunction(&VfpuInterpretHelper);
		LoadVolatileStatics();
		return;
	}
	if ((op >> 26) == 61) {
		const IRReg rs = (IRReg)((op >> 21) & 31);
		const s16 imm = (s16)(op & 0xFFFC);
		const int vt = (int)(((op >> 16) & 0x1F) | ((op & 1) << 5));
		u8 vr[4];
		::GetVectorRegs(vr, V_Quad, vt);
		for (int i = 0; i < 4; ++i)
			regs_.FlushFPR((IRReg)(32 + voffset[vr[i]]));
		if (rs == MIPS_REG_ZERO) {
			LI(SCRATCH1, imm);
		} else {
			ADDI(SCRATCH1, regs_.MapGPR(rs), imm);
		}
		regs_.FlushBeforeCall();
		LI(SCRATCH2, (s16)((vt << 1) | ((op >> 1) & 1)));
		SaveVolatileStatics();
		QuickCallFunction(&SvlrHelper);
		LoadVolatileStatics();
		return;
	}
	CompIR_Generic(inst);
}

void PPCJitBackend::FlushAll() {
	regs_.FlushAllFast();
}

bool PPCJitBackend::DescribeCodePtr(const u8 *ptr, std::string &name) const {
	if (ptr == dispatcherPCInSCRATCH1_) {
		name = "dispatcherPCInSCRATCH1";
	} else if (ptr == outerLoopPCInSCRATCH1_) {
		name = "outerLoopPCInSCRATCH1";
	} else if (ptr == dispatcherNoCheck_) {
		name = "dispatcherNoCheck";
	} else if (ptr >= GetBasePtr() && ptr < GetBasePtr() + jitStartOffset_) {
		name = "fixedCode";
	} else {
		return IRNativeBackend::DescribeCodePtr(ptr, name);
	}
	return true;
}

// How often a return inline cache follows its target to a new place before it gives up and always
// goes through the dispatcher (a function returning to several callers in turn).
static constexpr int RETURN_IC_MAX_PATCHES = 4;

void PPCJitBackend::PatchReturnIC(int index, u32 pc) {
	if (index < 0 || index >= (int)ics_.size())
		return;
	ReturnIC &ic = ics_[index];
	if (ic.patches >= RETURN_IC_MAX_PATCHES)
		return;
	const int block_num = blocks_.GetBlockNumberFromStartAddress(pc);
	const IRNativeBlock *nativeBlock = GetNativeBlock(block_num);
	if (block_num < 0 || !nativeBlock || nativeBlock->checkedOffset == 0)
		return;
	ic.patches++;
	// Layout (CompIR_Exit): +0 lis r12, +4 ori r12, +8 cmplw, +12 bne miss, +16 b target,
	// +20 lis/ori r4 (index), +28 b dispatcherICMiss.
	const u8 *start = GetBasePtr() + ic.offset;
	u8 *writable = GetWritablePtrFromCodePtr(start);
	PPCEmitter e(start, writable);
	e.LIS(R12, (s16)(pc >> 16));
	e.ORI(R12, R12, (u16)(pc & 0xFFFF));
	PPCEmitter b(start + 16, writable + 16);
	b.B(GetBasePtr() + nativeBlock->checkedOffset);
	if (ic.patches >= RETURN_IC_MAX_PATCHES) {
		PPCEmitter m(start + 28, writable + 28);
		m.B(dispatcherPCInSCRATCH1_);
	}
	CommitNow(start, start + 32);
}

void PPCJitBackend::ClearAllBlocks() {
	ics_.clear();
	savedEntries_.clear();
	tierRuns_.clear();
	clearGeneration_++;
	const size_t used = std::max(highWater_, (size_t)GetOffset(GetCodePtr()));
	ClearCodeSpace(jitStartOffset_);
	if (used > (size_t)jitStartOffset_)
		Commit(GetBasePtr() + jitStartOffset_, GetBasePtr() + used);
	EraseAllLinks(-1);
}

void PPCJitBackend::InvalidateBlock(IRBlockCache *irBlockCache, int block_num) {
	IRBlock *block = irBlockCache->GetBlock(block_num);
	int offset = block->GetNativeOffset();
	u8 *writable = GetWritablePtrFromCodePtr(GetBasePtr()) + offset;

	u32 pc = block->GetOriginalStart();
	if (pc != 0) {
		// What's patched over, for ReviveBlock (the same code coming back).
		std::array<u8, MIN_BLOCK_NORMAL_LEN> saved;
		memcpy(saved.data(), GetBasePtr() + offset, MIN_BLOCK_NORMAL_LEN);
		savedEntries_[block_num] = saved;
		PPCEmitter emitter(GetBasePtr() + offset, writable);
		emitter.LI32(SCRATCH1, pc);
		emitter.B(dispatcherPCInSCRATCH1_);
		int bytesWritten = (int)(emitter.GetWritableCodePtr() - writable);
		if (bytesWritten < MIN_BLOCK_NORMAL_LEN)
			emitter.ReserveCodeSpace(MIN_BLOCK_NORMAL_LEN - bytesWritten);
		// Now, not deferred: this runs between blocks, not while one is being written.
		CommitNow(GetBasePtr() + offset, GetBasePtr() + offset + MIN_BLOCK_NORMAL_LEN);
	}
	EraseAllLinks(block_num);
}

bool PPCJitBackend::ReviveBlock(IRBlockCache *irBlockCache, int block_num) {
	auto it = savedEntries_.find(block_num);
	if (it == savedEntries_.end())
		return false;
	IRBlock *block = irBlockCache->GetBlock(block_num);
	const int offset = block->GetNativeOffset();
	u8 *writable = GetWritablePtrFromCodePtr(GetBasePtr()) + offset;
	memcpy(writable, it->second.data(), MIN_BLOCK_NORMAL_LEN);
	CommitNow(GetBasePtr() + offset, GetBasePtr() + offset + MIN_BLOCK_NORMAL_LEN);
	savedEntries_.erase(it);
	return true;
}

void PPCJitBackend::ForgetBlock(int block_num) {
	savedEntries_.erase(block_num);
}

void PPCJitBackend::SaveStaticRegisters() {
	STW(DOWNCOUNTREG, CTXREG, (s16)offsetof(MIPSState, downcount));
	int count = 0;
	const auto *allocs = regs_.GetStaticAllocations(count);
	for (int i = 0; i < count; ++i) {
		const s16 offset = (s16)(offsetof(MIPSState, r) + allocs[i].mr * 4);
		if (allocs[i].nr >= V0) {
			// stvewx stores lane (address & 15) / 4: splat lane 0 there first.
			LI(R0, offset);
			VSPLTW(V3, (PPCReg)allocs[i].nr, 0);
			STVEWX(V3, CTXREG, R0);
		} else {
			STW((PPCReg)allocs[i].nr, CTXREG, offset);
		}
	}
}

// Static registers in volatile host registers (r2), around calls that leave the others alone.
void PPCJitBackend::SaveVolatileStatics() {
	int count = 0;
	const auto *allocs = regs_.GetStaticAllocations(count);
	for (int i = 0; i < count; ++i) {
		if (allocs[i].nr == R2)
			STW((PPCReg)allocs[i].nr, CTXREG, (s16)(offsetof(MIPSState, r) + allocs[i].mr * 4));
	}
}

void PPCJitBackend::LoadVolatileStatics() {
	int count = 0;
	const auto *allocs = regs_.GetStaticAllocations(count);
	for (int i = 0; i < count; ++i) {
		if (allocs[i].nr == R2)
			LWZ((PPCReg)allocs[i].nr, CTXREG, (s16)(offsetof(MIPSState, r) + allocs[i].mr * 4));
	}
}

void PPCJitBackend::LoadStaticRegisters() {
	LWZ(DOWNCOUNTREG, CTXREG, (s16)offsetof(MIPSState, downcount));
	int count = 0;
	const auto *allocs = regs_.GetStaticAllocations(count);
	for (int i = 0; i < count; ++i) {
		const s16 offset = (s16)(offsetof(MIPSState, r) + allocs[i].mr * 4);
		if (allocs[i].nr >= V0) {
			// lvewx loads into lane (address & 15) / 4; move it to lane 0.
			const PPCReg v = (PPCReg)allocs[i].nr;
			LI(R0, offset);
			LVEWX(v, CTXREG, R0);
			const int lane = (offset >> 2) & 3;
			if (lane != 0)
				VSLDOI(v, v, v, lane * 4);
		} else {
			LWZ((PPCReg)allocs[i].nr, CTXREG, offset);
		}
	}
}

#ifdef PPSSPP_RSX_HOSTSIM
// See IRJit::CompileBlock.
void PPCXProfCompile(u32 em_address, u32 mipsBytesInterp, const std::vector<IRInst> &instructionsInterp) {
	// The IR as the PS3 makes it (IRJit's options for a native backend).
	static IRFrontend frontend(true);
	static bool optionsSet = false;
	if (!optionsSet) {
		IROptions opts{};
		opts.unalignedLoadStore = true;
		opts.unalignedLoadStoreVec4 = true;
		opts.preferVec4 = true;
		opts.optimizeForInterpreter = false;
		frontend.SetOptions(opts);
		optionsSet = true;
	}
	std::vector<IRInst> instructions;
	u32 mipsBytes = 0;
	frontend.DoJit(em_address, instructions, mipsBytes);
	static JitOptions jo;
	static IRBlockCache cache(true);
	static PPCJitBackend *backend = nullptr;
	if (!backend) {
		jo.enableBlocklink = true;
		backend = new PPCJitBackend(jo, cache);
		backend->GenerateFixedCode(currentMIPS);
	}
	if (backend->GetSpaceLeft() < 0x10000 || cache.GetNumBlocks() > 60000) {
		cache.Clear();
		backend->ClearAllBlocks();
	}
	int num = cache.AllocateBlock((int)em_address, mipsBytes, instructions);
	if (backend->CompileBlock(&cache, num))
		backend->FinalizeBlock(&cache, num, jo);
}
#endif

#ifdef PPSSPP_RSX_HOSTSIM
}  // namespace MIPSComp
#include "Core/MIPS/MIPSAsm.h"
#include "Core/MIPS/MIPSDebugInterface.h"
namespace MIPSComp {
// Host test build: PPC_BLOCKTEST=<file> assembles the MIPS lines in <file> (the first line is the
// address) and prints the IR and writes the PPC code the PS3 JIT makes for that block to
// <file>.ppc (for objdump). Run with --ir on any test PRX.
void PPCBlockTest(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[256];
	u32 addr = 0;
	if (fgets(line, sizeof(line), f))
		addr = strtoul(line, nullptr, 16);
	const u32 start = addr;
	while (fgets(line, sizeof(line), f)) {
		std::string l(line);
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
		if (l.empty()) continue;
		std::string err;
		if (!MipsAssembleOpcode(l, currentDebugMIPS, addr, &err))
			fprintf(stderr, "BLOCKTEST asm error at %08x '%s': %s\n", addr, l.c_str(), err.c_str());
		addr += 4;
	}
	fclose(f);
	IRFrontend frontend(true);
	IROptions opts{};
	opts.unalignedLoadStore = true;
	opts.unalignedLoadStoreVec4 = true;
	opts.preferVec4 = true;
	opts.optimizeForInterpreter = false;
	frontend.SetOptions(opts);
	std::vector<IRInst> instructions;
	u32 mipsBytes = 0;
	frontend.DoJit(start, instructions, mipsBytes);
	for (const IRInst &inst : instructions) {
		char buf[256];
		DisassembleIR(buf, sizeof(buf), inst);
		fprintf(stderr, "BLOCKTEST ir %s\n", buf);
	}
	JitOptions jo;
	jo.enableBlocklink = true;
	IRBlockCache cache(true);
	PPCJitBackend *backend = new PPCJitBackend(jo, cache);
	backend->GenerateFixedCode(currentMIPS);
	int num = cache.AllocateBlock((int)start, mipsBytes, instructions);
	const u8 *begin = backend->GetCodePtr();
	if (backend->CompileBlock(&cache, num)) {
		const u8 *end = backend->GetCodePtr();
		std::string out = std::string(path) + ".ppc";
		if (FILE *o = fopen(out.c_str(), "wb")) {
			fwrite(begin, 1, end - begin, o);
			fclose(o);
		}
		fprintf(stderr, "BLOCKTEST %d MIPS, %d IR, %d PPC instructions -> %s\n", (int)(mipsBytes / 4), (int)instructions.size(), (int)((end - begin) / 4), out.c_str());
	}
}
#endif

std::string PPCJitStats() {
	// Top interpreted IR ops since the last call.
	std::string out;
	uint64_t total = 0;
	for (int i = 0; i < 256; ++i) total += s_genericCounts[i];
	char buf[96];
	snprintf(buf, sizeof(buf), "JIT: %llu IR ops ran in the interpreter%s", (unsigned long long)total,
		g_irNativeGenericOpCount ? " (jitops in use)" : "");
	out = buf;
	for (int n = 0; n < 8 && total; ++n) {
		int best = -1;
		for (int i = 0; i < 256; ++i)
			if (s_genericCounts[i] && (best < 0 || s_genericCounts[i] > s_genericCounts[best])) best = i;
		if (best < 0) break;
		const IRMeta *meta = GetIRMeta((IROp)best);
		snprintf(buf, sizeof(buf), "%s %s %u", n == 0 ? ":" : ",", meta ? meta->name : "?", s_genericCounts[best]);
		out += buf;
		s_genericCounts[best] = 0;
	}
	memset(s_genericCounts, 0, sizeof(s_genericCounts));
	// The untranslated MIPS instructions behind "Interpret", most frequent first.
	bool first = true;
	for (int n = 0; n < 8; ++n) {
		int best = -1;
		for (int i = 0; i < 32; ++i)
			if (s_interpretCounts[i].name && s_interpretCounts[i].count && (best < 0 || s_interpretCounts[i].count > s_interpretCounts[best].count)) best = i;
		if (best < 0) break;
		snprintf(buf, sizeof(buf), "%s %s %u", first ? "; interpreted MIPS instructions:" : ",", s_interpretCounts[best].name, s_interpretCounts[best].count);
		out += buf;
		first = false;
		s_interpretCounts[best].count = 0;
	}
	memset(s_interpretCounts, 0, sizeof(s_interpretCounts));
	static uint64_t lastCalls = 0, lastTicks = 0, lastBytes = 0;
	const uint64_t calls = PS3Jit::CommitCalls(), ticks = PS3Jit::CommitTicks(), bytes = PS3Jit::CommittedBytes();
	snprintf(buf, sizeof(buf), "; code commits: %llu calls, %llu KB, %.0f ms", (unsigned long long)(calls - lastCalls),
		(unsigned long long)((bytes - lastBytes) / 1024), (ticks - lastTicks) * (1000.0 / 79800000.0));
	out += buf;
	lastCalls = calls; lastTicks = ticks; lastBytes = bytes;
	return out;
}


#if PPSSPP_ARCH(POWERPC) && !PPSSPP_PLATFORM(PS3)
}  // namespace MIPSComp
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/MIPS/MIPSAsm.h"
#include "Core/MIPS/MIPSDebugInterface.h"
#include "Core/MIPS/IR/IRJit.h"
namespace MIPSComp {
// Test build on PPC (qemu): PPC_BLOCKVERIFY=<file> (a PPC_BLOCKTEST file) runs that MIPS block
// with random register and memory contents, once through the IR interpreter and once as the PPC
// JIT compiled it, and compares every register and the memory around sp. Returns the number of
// trials that differ. Values are kept normal (no NaN, infinity or denormals: VMX and the FPU
// treat those differently by design), so what it checks is the lane plumbing of the fusions.
int PPCBlockVerify(const char *path, int trials) {
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char line[256];
	u32 addr = 0;
	if (fgets(line, sizeof(line), f))
		addr = strtoul(line, nullptr, 16);
	const u32 start = addr;
	while (fgets(line, sizeof(line), f)) {
		std::string l(line);
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
		if (l.empty()) continue;
		std::string err;
		if (!MipsAssembleOpcode(l, currentDebugMIPS, addr, &err))
			fprintf(stderr, "BLOCKVERIFY asm error at %08x '%s': %s\n", addr, l.c_str(), err.c_str());
		addr += 4;
	}
	fclose(f);
	fprintf(stderr, "BLOCKVERIFY: assembled %08x-%08x\n", start, addr);
	g_ppcJitTierRuns = 0;
	// Break at every exit target outside the block, so the native run stops there.
	IRFrontend frontend(true);
	IROptions opts{};
	opts.unalignedLoadStore = true;
	opts.unalignedLoadStoreVec4 = true;
	opts.preferVec4 = true;
	frontend.SetOptions(opts);
	std::vector<IRInst> probe;
	u32 probeBytes = 0;
	frontend.DoJit(start, probe, probeBytes);
	for (const IRInst &inst : probe) {
		const IRMeta *m = GetIRMeta(inst.op);
		if (m && (m->flags & IRFLAG_EXIT) && (inst.op == IROp::ExitToConst || inst.op == IROp::ExitToConstIfEq || inst.op == IROp::ExitToConstIfNeq ||
				inst.op == IROp::ExitToConstIfGtZ || inst.op == IROp::ExitToConstIfGeZ || inst.op == IROp::ExitToConstIfLtZ || inst.op == IROp::ExitToConstIfLeZ ||
				inst.op == IROp::ExitToConstIfFpTrue || inst.op == IROp::ExitToConstIfFpFalse)) {
			const u32 target = inst.constant;
			if (target < start || target >= addr)
				Memory::WriteUnchecked_U32(0x0000000D, target);  // break
		}
	}
	Memory::WriteUnchecked_U32(0x0000000D, 0x08A00000);
	const u32 sp = 0x09F00000;
	const u32 memLo = sp - 0x800, memHi = sp + 0x800;
	uint32_t rng = 12345;
	auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return rng; };
	auto rndFloat = [&]() -> float {
		const uint32_t r = rnd();
		if ((r & 15) == 0) return 0.0f;
		const float v = (float)((int)(rnd() % 20001) - 10000) / 997.0f;
		return v == 0.0f ? 1.0f : v;
	};
	struct Snap {
		u32 r[32]; float f[32]; float v[128]; u32 fpcond; u32 vcc; u32 hi, lo;
		std::vector<u8> mem;
	};
	auto take = [&](Snap &sn) {
		memcpy(sn.r, currentMIPS->r, sizeof(sn.r));
		memcpy(sn.f, currentMIPS->f, sizeof(sn.f));
		memcpy(sn.v, currentMIPS->v, sizeof(sn.v));
		sn.fpcond = currentMIPS->fpcond;
		sn.vcc = currentMIPS->vfpuCtrl[VFPU_CTRL_CC];
		sn.hi = currentMIPS->hi; sn.lo = currentMIPS->lo;
		sn.mem.assign(Memory::GetPointerUnchecked(memLo), Memory::GetPointerUnchecked(memLo) + (memHi - memLo));
	};
	auto put = [&](const Snap &sn) {
		memcpy(currentMIPS->r, sn.r, sizeof(sn.r));
		memcpy(currentMIPS->f, sn.f, sizeof(sn.f));
		memcpy(currentMIPS->v, sn.v, sizeof(sn.v));
		currentMIPS->fpcond = sn.fpcond;
		currentMIPS->vfpuCtrl[VFPU_CTRL_CC] = sn.vcc;
		currentMIPS->hi = sn.hi; currentMIPS->lo = sn.lo;
		memcpy(Memory::GetPointerWriteUnchecked(memLo), sn.mem.data(), sn.mem.size());
	};
	auto runNative = [&]() {
		currentMIPS->pc = start;
		coreState = CORE_RUNNING_CPU;
		MIPSComp::jit->RunLoopUntil(CoreTiming::GetTicks(currentMIPS) + 20000);
		Core_ResetException();
		coreState = CORE_RUNNING_CPU;
	};
	int bad = 0;
	const IRInst *irInsts = nullptr;
	int irCount = 0;
	for (int t = 0; t < trials; ++t) {
		Snap s0, s1, s2;
		for (int i = 1; i < 32; ++i) currentMIPS->r[i] = rnd();
		currentMIPS->r[MIPS_REG_SP] = sp;
		currentMIPS->r[MIPS_REG_RA] = 0x08A00000;  // jr ra ends there (a break).
		for (int i = 0; i < 32; ++i) currentMIPS->f[i] = rndFloat();
		for (int i = 0; i < 128; ++i) currentMIPS->v[i] = rndFloat();
		currentMIPS->fpcond = rnd() & 1;
		currentMIPS->vfpuCtrl[VFPU_CTRL_CC] = rnd() & 0x3F;
		for (u32 a = memLo; a < memHi; a += 4) {
			const float v = rndFloat();
			u32 bits;
			memcpy(&bits, &v, 4);
			Memory::WriteUnchecked_U32(bits, a);
		}
		take(s0);
		if (!irInsts) {
			fprintf(stderr, "BLOCKVERIFY: first native run\n");
			runNative();  // Compiles it.
			fprintf(stderr, "BLOCKVERIFY: first native run done, pc %08x\n", currentMIPS->pc);
			IRBlockCache *cache = &((IRJit *)MIPSComp::jit)->GetIRBlocks();
			const int num = cache->GetBlockNumberFromStartAddress(start);
			if (num < 0) {
				fprintf(stderr, "BLOCKVERIFY: block not compiled\n");
				return -1;
			}
			irInsts = cache->GetBlockInstructionPtr(num);
			irCount = cache->GetBlock(num)->GetNumIRInstructions();
			put(s0);
		}
		currentMIPS->pc = start;
		const u32 irPC = IRInterpret(currentMIPS, irInsts);
		take(s1);
		put(s0);
		runNative();
		const u32 nativePC = currentMIPS->pc;
		take(s2);
		int diffs = 0;
		auto report = [&](const char *what, int i, u32 a, u32 b) {
			if (diffs++ < 8)
				fprintf(stderr, "BLOCKVERIFY trial %d: %s[%d] IR %08x native %08x\n", t, what, i, a, b);
		};
		for (int i = 0; i < 32; ++i) if (s1.r[i] != s2.r[i]) report("r", i, s1.r[i], s2.r[i]);
		for (int i = 0; i < 32; ++i) { u32 a, b; memcpy(&a, &s1.f[i], 4); memcpy(&b, &s2.f[i], 4); if (a != b) report("f", i, a, b); }
		for (int i = 0; i < 128; ++i) { u32 a, b; memcpy(&a, &s1.v[i], 4); memcpy(&b, &s2.v[i], 4); if (a != b) report("v", i, a, b); }
		if (s1.fpcond != s2.fpcond) report("fpcond", 0, s1.fpcond, s2.fpcond);
		if (s1.vcc != s2.vcc) report("vfpucc", 0, s1.vcc, s2.vcc);
		for (size_t i = 0; i < s1.mem.size(); i += 4) {
			u32 a, b;
			memcpy(&a, &s1.mem[i], 4); memcpy(&b, &s2.mem[i], 4);
			if (a != b) report("mem", (int)(memLo + i), a, b);
		}
		if (irPC != nativePC && irPC + 4 != nativePC && irPC != nativePC + 4) report("pc", 0, irPC, nativePC);
		if (diffs) bad++;
	}
	fprintf(stderr, "BLOCKVERIFY %s: %d IR instructions, %d trials, %d differ\n", path, irCount, trials, bad);
	return bad;
}
#endif

}  // namespace MIPSComp

#endif
