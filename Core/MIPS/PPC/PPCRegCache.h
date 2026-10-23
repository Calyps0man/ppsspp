// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Common/PPCEmitter.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/IR/IRJit.h"
#include "Core/MIPS/IR/IRRegCache.h"

namespace MIPSComp {
// Keep FPU/VFPU values in AltiVec registers (set before the JIT is created).
extern bool g_ppcJitVMX;
// New blocks run in the IR interpreter until they've run this many times, then get compiled to
// PPC code (0: compile right away). Most code a new scene runs runs only a few times; compiling all
// of it made the scene start freeze.
extern int g_ppcJitTierRuns;
// AltiVec mode: keep the most used FPU registers in AltiVec registers across blocks.
extern bool g_ppcJitStaticFPR;
}

namespace PPCJitConstants {

// Fixed registers while in JIT code (all callee-saved in the ELFv1 ABI). r29 and r2 hold static
// MIPS registers too: r2 (the TOC) is loaded by every call through a function descriptor, saved on
// entry and restored on exit, so the JIT code can use it in between.
const PPCGen::PPCReg CTXREG = PPCGen::R31;       // MIPSState *
const PPCGen::PPCReg MEMBASEREG = PPCGen::R30;   // Memory::base
const PPCGen::PPCReg DOWNCOUNTREG = PPCGen::R28; // mips->downcount, kept in a register
// Scratch registers (volatile). R3 doubles as the first argument / return value.
const PPCGen::PPCReg SCRATCH1 = PPCGen::R3;
const PPCGen::PPCReg SCRATCH2 = PPCGen::R4;
const PPCGen::PPCReg SCRATCH3 = PPCGen::R11;
const PPCGen::PPCReg FSCRATCH1 = PPCGen::F0;
const PPCGen::PPCReg FSCRATCH2 = PPCGen::F13;
// AltiVec mode: the byte-swap permute patterns, loaded at the JIT entry (callee-saved).
const PPCGen::PPCReg VNEGZERO = PPCGen::V29;    // -0.0f in every lane.
const PPCGen::PPCReg VSWAPWORDS = PPCGen::V30;
const PPCGen::PPCReg VLANE0SWAP = PPCGen::V31;

}  // namespace PPCJitConstants

// MIPS GPR values live in the low 32 bits of 64-bit registers. The upper 32 bits are not kept
// in any particular form: every operation that cares uses a 32-bit instruction form (cmpw,
// slw, rlwinm, mullw, lwbrx, stw...).
class PPCRegCache : public IRNativeRegCacheBase {
public:
	PPCRegCache(MIPSComp::JitOptions *jo);

	void Init(PPCGen::PPCEmitter *emitter);

	// Returns a native register holding the MIPS register.
	PPCGen::PPCReg MapGPR(IRReg reg, MIPSMap mapFlags = MIPSMap::INIT);
	PPCGen::PPCReg MapFPR(IRReg reg, MIPSMap mapFlags = MIPSMap::INIT);

	void FlushBeforeCall();

	PPCGen::PPCReg GetAndLockTempGPR();

	PPCGen::PPCReg R(IRReg preg);
	PPCGen::PPCReg F(IRReg preg);

	// AltiVec mode (g_ppcJitVMX): every FPR lives in a VMX register, a scalar in lane 0 or a Vec4
	// group in all four lanes. V() gives the register holding the FPR (or its group).
	PPCGen::PPCReg V(IRReg fpr);
	PPCGen::PPCReg MapVR(IRReg fpr, int lanes, MIPSMap mapFlags);
	// Lane of the VMX register the FPR is in (0 for a scalar), and the lane count.
	int VRLane(IRReg fpr) const { return mr[fpr + 32].lane < 0 ? 0 : mr[fpr + 32].lane; }
	bool InVR(IRReg fpr) const { return mr[fpr + 32].loc == MIPSLoc::VREG; }
	int VRLanes(IRReg fpr) const;
	void MarkVRDirty(IRReg fpr);
	bool UsesVMX() const { return vmx_; }

	// MIPS registers kept in host registers across blocks (see GetStaticAllocations).
	const StaticAllocation *GetStaticAllocations(int &count) const override;

	// The mapping state, so a flush can be emitted on one side of a branch only (a conditional
	// exit stores its registers on the exit path, and the code after it keeps them mapped).
	struct Snapshot {
		RegStatusNative nr[TOTAL_POSSIBLE_NATIVEREGS];
		RegStatusMIPS mr[TOTAL_MAPPABLE_IRREGS];
	};
	void Save(Snapshot &s) const;
	void Restore(const Snapshot &s);
	// FlushAll, walking the mapped native registers instead of every IR register (compile time:
	// FlushAll was half of what compiling a block cost).
	void FlushAllFast();
	// The stores FlushAll would emit, leaving the mapping as it is (the taken side of an exit).
	void EmitFlushStores();
	// Writes an FPR (and the rest of its group) back to MIPSState if it's in a register.
	void FlushFPR(IRReg fpr) { FlushReg(fpr + 32); }

protected:
	const int *GetAllocationOrder(MIPSLoc type, MIPSMap flags, int &count, int &base) const override;
	void AdjustNativeRegAsPtr(IRNativeReg nreg, bool state) override;

	bool IsNativeRegCompatible(IRNativeReg nreg, MIPSLoc type, MIPSMap flags, int lanes) override;
	void LoadNativeReg(IRNativeReg nreg, IRReg first, int lanes) override;
	void StoreNativeReg(IRNativeReg nreg, IRReg first, int lanes) override;
	void SetNativeRegValue(IRNativeReg nreg, uint32_t imm) override;
	bool TransferNativeReg(IRNativeReg nreg, IRNativeReg dest, MIPSLoc type, IRReg first, int lanes, MIPSMap flags) override;
	bool TransferVecTo1(IRNativeReg nreg, IRNativeReg dest, IRReg first);
	bool Transfer1ToVec(IRNativeReg dest, IRReg first);
	void StoreRegValue(IRReg mreg, uint32_t imm) override;

private:
	PPCGen::PPCEmitter *emit_ = nullptr;
	bool vmx_ = false;

	enum {
		NUM_PPCGPR = 32,
		NUM_PPCFPR = 32,
	};
};
