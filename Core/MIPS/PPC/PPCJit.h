// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// IR -> 64-bit PowerPC (PS3 PPU) native JIT. The PSP's MIPS code is first translated to
// PPSSPP's IR, as for the IR interpreter; this backend turns each IR block into PPC code.
// Integer ALU ops, loads/stores (byte-reversed, the PSP is little-endian), branches and exits
// are compiled natively; everything else (FPU, VFPU, syscalls...) calls the IR interpreter for
// that one instruction, so behaviour matches the interpreter exactly.
#pragma once

#include <array>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>
#include "Common/PPCEmitter.h"
#include "Core/MIPS/IR/IRJit.h"
#include "Core/MIPS/IR/IRNativeCommon.h"
#include "Core/MIPS/JitCommon/JitState.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/PPC/PPCRegCache.h"

namespace MIPSComp {

class PPCJitBackend : public PPCGen::PPCCodeBlock, public IRNativeBackend {
public:
	PPCJitBackend(JitOptions &jo, IRBlockCache &blocks);
	~PPCJitBackend();

	bool DescribeCodePtr(const u8 *ptr, std::string &name) const override;

	void GenerateFixedCode(MIPSState *mipsState) override;
	bool CompileBlock(IRBlockCache *irBlockCache, int block_num) override;
	void ClearAllBlocks() override;
	void InvalidateBlock(IRBlockCache *irBlockCache, int block_num) override;
	bool CanReviveBlocks() const override { return true; }
	bool ReviveBlock(IRBlockCache *irBlockCache, int block_num) override;
	void ForgetBlock(int block_num) override;
	void FinalizeBlockDone() override;

protected:
	const CodeBlockCommon &CodeBlock() const override {
		return *this;
	}

private:
	void SaveStaticRegisters();
	void LoadStaticRegisters();
	void SaveVolatileStatics();
	void LoadVolatileStatics();
	void FlushAll();
	// Copies generated code [start, end) into the executable area.
	void Commit(const u8 *start, const u8 *end);

	void WriteConstExit(uint32_t pc);
	// Rarely run code (the exact-rounding FPU paths) goes after the block, so the usual path has
	// no taken branch around it: emit() starts at the target of `from` and ends with a branch back.
	void AddFarCode(PPCGen::FixupBranch from, std::function<void()> emit);
	void FlushFarCode();
	void MoveCacheBefore(const IRInst &inst);
	bool MoveCacheShortcut(const IRInst &inst);
	void MoveCacheAfter(const IRInst &inst);
	int16_t gprHoldsFpr_[32];
	// AltiVec mode: the IR register holding the 0/1 result of the FCmp just compiled (fpcond, then
	// the GPR FpCondToReg copied it to), whose mask v2 still holds; -1 when v2 may have changed.
	// FMovFromGPR of it builds the value from the mask (no store and reload through memory).
	int cmpMaskReg_ = -1;
	bool cmpMaskInverted_ = false;
	void TrackCompareMask(const IRInst &inst);
	bool CompareMaskShortcut(const IRInst &inst);
	// AltiVec mode: FPRs loaded from memory (LoadFloat) since the last instruction that could
	// change memory or the address registers. FMovToGPR of one loads the GPR from the same address.
	struct FprLoad { IRReg fpr; IRReg base; u32 offset; };
	FprLoad fprLoads_[8];
	int numFprLoads_ = 0;
	void TrackFprLoads(const IRInst &inst);
	bool FprLoadShortcut(const IRInst &inst);
	std::vector<std::pair<PPCGen::FixupBranch, std::function<void()>>> farCode_;
	int farFirstOffset_ = -1;
	void FarExitIf(PPCGen::FixupBranch taken, uint32_t pc);
	void ExitPathFlushAndExit(uint32_t pc);
	void OverwriteExit(int srcOffset, int len, int block_num) override;
	// Address (masked PSP address) of src1 + constant into SCRATCH2.
	void AddressToScratch2(IRInst inst);
	void IntBitsToFloat(PPCGen::PPCReg dest, PPCGen::PPCReg src);
	void EmitFMinMax(bool isMin, PPCGen::PPCReg d, PPCGen::PPCReg a, PPCGen::PPCReg b);

	// AltiVec mode (g_ppcJitVMX): FPU/VFPU values live in VMX registers.
	void VConst(PPCGen::PPCReg vt, const void *ptr);
	void VNegZero(PPCGen::PPCReg vt);
	void VRToFPR(PPCGen::PPCReg f, PPCGen::PPCReg v, int lane = 0);
	void FPRToVR(PPCGen::PPCReg v, PPCGen::PPCReg f);
	void GPRToVR(PPCGen::PPCReg v, PPCGen::PPCReg r);
	void VRToGPR(PPCGen::PPCReg r, PPCGen::PPCReg v, int lane = 0);
	void VCmpMask(int mode, PPCGen::PPCReg a, PPCGen::PPCReg b);
	void VMaskToBool(PPCGen::PPCReg dest);
	PPCGen::FixupBranch BranchIfNotNearest();
	void VMX_FArith(IRInst inst);
	void VMX_FAssign(IRInst inst);
	void VMX_FLoad(IRInst inst);
	void VMX_FStore(IRInst inst);
	void VMX_VecLoad(IRInst inst);
	void VMX_VecStore(IRInst inst);
	int VMX_MatrixRows(const IRInst *insts, int i, int n);
	int VMX_Vec4InitScalars(const IRInst *insts, int i, int n);
	int VMX_Dot3(const IRInst *insts, int i, int n);
	int VMX_CompareToFloat(const IRInst *insts, int i, int n);
	int VMX_HSum4(const IRInst *insts, int i, int n);
	int VMX_FloatCopy(const IRInst *insts, int i, int n);
	bool MatchDotQuad(const IRInst *insts, int i, int n, IRReg *a, IRReg b[4], IRReg *d, IRReg *t0, IRReg *t1) const;
	int VMX_DotQuad(const IRInst *insts, int i, int n);
	int VMX_LaneOp4(const IRInst *insts, int i, int n);
	bool MatchCompare4(const IRInst *insts, int i, int n, IRReg *a, IRReg *b, IRReg *d, int *mode) const;
	bool MatchCompareN(const IRInst *insts, int i, int n, IRReg *a, IRReg *b, IRReg *d, int *mode, int *lanes, bool *bZero) const;
	int VMX_Compare4(const IRInst *insts, int i, int n);
	void VCmpMaskV01(int mode);
	void VMX_VecAssign(IRInst inst);
	void VMX_VecArith(IRInst inst);
	void VMX_VecHoriz(IRInst inst);
	void VMX_FCompare(IRInst inst);
	void VMX_FCondAssign(IRInst inst);
	void VMX_FCvt(IRInst inst);
	void VMX_FRound(IRInst inst);
	void VMX_FSpecial(IRInst inst);
	void VMX_Transfer(IRInst inst);
	void VMX_VecPack(IRInst inst);
	void FloatToIntBits(PPCGen::PPCReg dest, PPCGen::PPCReg src, int roundMode);

	void CompIR_Arith(IRInst inst) override;
	void CompIR_Assign(IRInst inst) override;
	void CompIR_Basic(IRInst inst) override;
	void CompIR_Bits(IRInst inst) override;
	void CompIR_Breakpoint(IRInst inst) override;
	void CompIR_Compare(IRInst inst) override;
	void CompIR_CondAssign(IRInst inst) override;
	void CompIR_CondStore(IRInst inst) override;
	void CompIR_Div(IRInst inst) override;
	void CompIR_Exit(IRInst inst) override;
	void CompIR_ExitIf(IRInst inst) override;
	void CompIR_FArith(IRInst inst) override;
	void CompIR_FAssign(IRInst inst) override;
	void CompIR_FCompare(IRInst inst) override;
	void CompIR_FCondAssign(IRInst inst) override;
	void CompIR_FCvt(IRInst inst) override;
	void CompIR_FLoad(IRInst inst) override;
	void CompIR_FRound(IRInst inst) override;
	void CompIR_FSat(IRInst inst) override;
	void CompIR_FSpecial(IRInst inst) override;
	void CompIR_FStore(IRInst inst) override;
	void CompIR_Generic(IRInst inst) override;
	void CompIR_HiLo(IRInst inst) override;
	void CompIR_Interpret(IRInst inst) override;
	void CompIR_Load(IRInst inst) override;
	void CompIR_LoadShift(IRInst inst) override;
	void CompIR_Logic(IRInst inst) override;
	void CompIR_Mult(IRInst inst) override;
	void CompIR_RoundingMode(IRInst inst) override;
	void CompIR_Shift(IRInst inst) override;
	void CompIR_Store(IRInst inst) override;
	void CompIR_StoreShift(IRInst inst) override;
	void CompIR_System(IRInst inst) override;
	void CompIR_Transfer(IRInst inst) override;
	void CompIR_VecArith(IRInst inst) override;
	void CompIR_VecAssign(IRInst inst) override;
	void CompIR_VecHoriz(IRInst inst) override;
	void CompIR_VecLoad(IRInst inst) override;
	void CompIR_VecPack(IRInst inst) override;
	void CompIR_VecStore(IRInst inst) override;
	void CompIR_ValidateAddress(IRInst inst) override;

	JitOptions &jo;
	PPCRegCache regs_;

	const u8 *outerLoop_ = nullptr;
	const u8 *outerLoopPCInSCRATCH1_ = nullptr;
	const u8 *dispatcherCheckCoreState_ = nullptr;
	const u8 *dispatcherPCInSCRATCH1_ = nullptr;
	const u8 *dispatcherNoCheck_ = nullptr;
	// Jumps through a register (jr ra) first compare with the last target they went to and branch
	// straight to its block (an inline cache); a miss comes here with the PC in SCRATCH1 and the
	// cache's number in SCRATCH2.
	const u8 *dispatcherICMiss_ = nullptr;
	struct ReturnIC {
		int offset;
		int patches;
	};
	std::vector<ReturnIC> ics_;
	// Native entries of invalidated blocks as they were (ReviveBlock), by block number.
	std::unordered_map<int, std::array<u8, 16>> savedEntries_;
public:
	void PatchReturnIC(int index, u32 pc);
	// A block still running in the interpreter (see g_ppcJitTierRuns): runs it once, returns the
	// next PC, compiles it once it's run enough.
	u32 TierRun(int block_num);
private:
	const u8 *tierInterp_ = nullptr;
public:
	// Optimized IR for a block about to be compiled after its warm-up (set by PPCJit).
	std::function<void(u32, std::vector<IRInst> &, u32 &)> reoptimize_;
private:
	std::vector<uint16_t> tierRuns_;
	bool promoting_ = false;
	bool noFuse_ = false;  // Frontend 106: compiling again without the fused patterns.
	int clearGeneration_ = 0;
	void EmitTierStub(IRBlockCache *irBlockCache, int block_num);
	void PromoteBlock(int block_num);

	int jitStartOffset_ = 0;
	int compilingBlockNum_ = -1;
	size_t highWater_ = 0;
	bool commitFailed_ = false;
	// While compiling a block, commits are collected and made in FinalizeBlockDone (on the PS3 each
	// commit is a PS3MAPI system call): the block and the exits it links become one call.
	bool deferCommits_ = false;
	std::vector<std::pair<int, int>> pendingCommits_;
	void CommitNow(const u8 *start, const u8 *end);
};

class PPCJit : public IRNativeJit {
public:
	PPCJit(MIPSState *mipsState)
		: IRNativeJit(mipsState), ppcBackend_(jo, blocks_) {
		Init(ppcBackend_);
		ppcBackend_.reoptimize_ = [this](u32 addr, std::vector<IRInst> &out, u32 &bytes) {
			frontend_.DoJit(addr, out, bytes, true);
		};
	}

protected:
	bool UnoptimizedFirst() const override;

private:
	PPCJitBackend ppcBackend_;
};

// True when the PS3 JIT can run (PS3MAPI available). CreateNativeJit falls back to the IR
// interpreter otherwise.
bool PPCJitAvailable();
// Interpreter fallbacks since the last call (top ops), for the perf log.
std::string PPCJitStats();

}  // namespace MIPSComp
