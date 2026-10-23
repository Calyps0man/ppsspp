#pragma once

#include <unordered_set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/JitCommon/JitState.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/MIPS/IR/IRInst.h"

namespace MIPSComp {

extern bool g_irRoundingModeUsed;

class IRFrontend : public MIPSFrontendInterface {
public:
	IRFrontend(bool startDefaultPrefix);
	void Comp_Generic(MIPSOpcode op) override;

	void Comp_RunBlock(MIPSOpcode op) override;
	void Comp_ReplacementFunc(MIPSOpcode op) override;

	// Ops
	void Comp_ITypeMem(MIPSOpcode op) override;
	void Comp_StoreSync(MIPSOpcode op) override;
	void Comp_Cache(MIPSOpcode op) override;

	void Comp_RelBranch(MIPSOpcode op) override;
	void Comp_RelBranchRI(MIPSOpcode op) override;
	void Comp_FPUBranch(MIPSOpcode op) override;
	void Comp_FPULS(MIPSOpcode op) override;
	void Comp_FPUComp(MIPSOpcode op) override;
	void Comp_Jump(MIPSOpcode op) override;
	void Comp_JumpReg(MIPSOpcode op) override;
	void Comp_Syscall(MIPSOpcode op) override;
	void Comp_Break(MIPSOpcode op) override;

	void Comp_IType(MIPSOpcode op) override;
	void Comp_RType2(MIPSOpcode op) override;
	void Comp_RType3(MIPSOpcode op) override;
	void Comp_ShiftType(MIPSOpcode op) override;
	void Comp_Allegrex(MIPSOpcode op) override;
	void Comp_Allegrex2(MIPSOpcode op) override;
	void Comp_VBranch(MIPSOpcode op) override;
	void Comp_MulDivType(MIPSOpcode op) override;
	void Comp_Special3(MIPSOpcode op) override;

	void Comp_FPU3op(MIPSOpcode op) override;
	void Comp_FPU2op(MIPSOpcode op) override;
	void Comp_mxc1(MIPSOpcode op) override;

	void Comp_DoNothing(MIPSOpcode op) override;

	void Comp_SV(MIPSOpcode op) override;
	void Comp_SVQ(MIPSOpcode op) override;
	void Comp_VPFX(MIPSOpcode op) override;
	void Comp_VVectorInit(MIPSOpcode op) override;
	void Comp_VMatrixInit(MIPSOpcode op) override;
	void Comp_VDot(MIPSOpcode op) override;
	void Comp_VecDo3(MIPSOpcode op) override;
	void Comp_VV2Op(MIPSOpcode op) override;
	void Comp_Mftv(MIPSOpcode op) override;
	void Comp_Vmfvc(MIPSOpcode op) override;
	void Comp_Vmtvc(MIPSOpcode op) override;
	void Comp_Vmmov(MIPSOpcode op) override;
	void Comp_VScl(MIPSOpcode op) override;
	void Comp_Vmmul(MIPSOpcode op) override;
	void Comp_Vmscl(MIPSOpcode op) override;
	void Comp_Vtfm(MIPSOpcode op) override;
	void Comp_VHdp(MIPSOpcode op) override;
	void Comp_VCrs(MIPSOpcode op) override;
	void Comp_VDet(MIPSOpcode op) override;
	void Comp_Vi2x(MIPSOpcode op) override;
	void Comp_Vx2i(MIPSOpcode op) override;
	void Comp_Vf2i(MIPSOpcode op) override;
	void Comp_Vi2f(MIPSOpcode op) override;
	void Comp_Vh2f(MIPSOpcode op) override;
	void Comp_Vcst(MIPSOpcode op) override;
	void Comp_Vhoriz(MIPSOpcode op) override;
	void Comp_VRot(MIPSOpcode op) override;
	void Comp_VIdt(MIPSOpcode op) override;
	void Comp_Vcmp(MIPSOpcode op) override;
	void Comp_Vcmov(MIPSOpcode op) override;
	void Comp_Viim(MIPSOpcode op) override;
	void Comp_Vfim(MIPSOpcode op) override;
	void Comp_VCrossQuat(MIPSOpcode op) override;
	void Comp_Vsgn(MIPSOpcode op) override;
	void Comp_Vocp(MIPSOpcode op) override;
	void Comp_ColorConv(MIPSOpcode op) override;
	void Comp_Vbfy(MIPSOpcode op) override;

	int Replace_fabsf() override;
	void DoState(PointerWrap &p);
	bool CheckRounding(u32 blockAddress);  // returns true if we need a do-over
	// Blocks that ended with a VFPU prefix still in effect: the blocks at their exits have to be
	// compiled without assuming default prefixes. CheckRounding fills this with those addresses
	// (to invalidate) instead of dropping the assumption for every block (which sent nearly every
	// prefixed VFPU op of a game to the interpreter: Call of Duty, ~60,000 a frame).
	std::vector<u32> prefixTargetsToInvalidate;

	// optimize = false: the IR straight from the MIPS code, without the optimization passes (the PS3
	// JIT interprets new blocks a few times first, and only optimizes the ones it compiles).
	void DoJit(u32 em_address, std::vector<IRInst> &instructions, u32 &mipsBytes, bool optimize = true);

	void EatPrefix() override {
		js.EatPrefix();
	}

	void SetOptions(const IROptions &o) {
		opts = o;
	}

private:
	std::unordered_set<u32> prefixStartAddrs_;
	std::vector<u32> lastExitTargets_;
	bool lastExitsKnown_ = true;
	bool blockStartDefault_ = true;

	void RestoreRoundingMode(bool force = false);
	void ApplyRoundingMode(bool force = false);
	void UpdateRoundingMode();

	void FlushAll();
	void FlushPrefixV();

	u32 GetCompilerPC();
	void CompileDelaySlot();
	void EatInstruction(MIPSOpcode op);
	MIPSOpcode GetOffsetInstruction(int offset);

	void CheckBreakpoint(u32 addr);
	void CheckMemoryBreakpoint(int rs, int offset);

	// Utility compilation functions
	void BranchFPFlag(MIPSOpcode op, IRComparison cc, bool likely);
	void BranchVFPUFlag(MIPSOpcode op, IRComparison cc, bool likely);
	void BranchRSZeroComp(MIPSOpcode op, IRComparison cc, bool andLink, bool likely);
	void BranchRSRTComp(MIPSOpcode op, IRComparison cc, bool likely);

	// Utilities to reduce duplicated code
	void CompShiftImm(MIPSOpcode op, IROp shiftType, int sa);
	void CompShiftVar(MIPSOpcode op, IROp shiftType);

	void ApplyPrefixST(u8 *vregs, u32 prefix, VectorSize sz, int tempReg);
	void ApplyPrefixD(u8 *vregs, VectorSize sz, int vectorReg);
	void ApplyPrefixDMask(u8 *vregs, VectorSize sz, int vectorReg);
	void GetVectorRegsPrefixS(u8 *regs, VectorSize sz, int vectorReg);
	void GetVectorRegsPrefixT(u8 *regs, VectorSize sz, int vectorReg);
	void GetVectorRegsPrefixD(u8 *regs, VectorSize sz, int vectorReg);
	void GetVectorRegs(u8 regs[4], VectorSize N, int vectorReg);
	void GetMatrixRegs(u8 regs[16], MatrixSize N, int matrixReg);

	// State
	JitState js;
	IRWriter ir;
	IROptions opts{};

	int dontLogBlocks = 0;
	int logBlocks = 0;
};

}  // namespace
