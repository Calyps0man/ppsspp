// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// IR instruction compilation. MIPS values use the low 32 bits of the 64-bit registers; only
// 32-bit instruction forms (or explicit extension before 64-bit ones) are used on them.

#include "ppsspp_config.h"
#include "Core/MIPS/PPC/PPCBuild.h"
#ifdef PPSSPP_PPC_JIT_BUILD

#include <cstddef>
#include <cstring>
#include <unordered_map>
#include "Common/Log.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/IR/IRAnalysis.h"
#include "Core/MIPS/PPC/PPCJit.h"
#include "Core/MIPS/PPC/PPCRegCache.h"
#include "Core/HLE/HLE.h"
#include "Core/MIPS/MIPSVFPUUtils.h"

namespace MIPSComp {

using namespace PPCGen;
using namespace PPCJitConstants;

#define DISABLE { CompIR_Generic(inst); return; }
#define INVALIDOP { CompIR_Generic(inst); return; }

static bool FitsS16(s32 v) { return v >= -32768 && v <= 32767; }

// Float constants for SetConstF, each in all four lanes of a 16-byte slot (lane 0 for VMX, the
// first word for lfs). Kept across JIT cache clears; full means the old path through the stack.
static constexpr int FCONST_POOL_SIZE = 4096;
alignas(16) static u32 s_fconstPool[FCONST_POOL_SIZE][4];
static int s_fconstCount = 0;
static const u32 *PoolFloatConstant(u32 value) {
	// Linear search from the most recent: blocks reuse a handful of constants.
	for (int i = s_fconstCount - 1; i >= 0 && i >= s_fconstCount - 64; --i) {
		if (s_fconstPool[i][0] == value)
			return s_fconstPool[i];
	}
	static std::unordered_map<u32, int> index;
	auto it = index.find(value);
	if (it != index.end())
		return s_fconstPool[it->second];
	if (s_fconstCount >= FCONST_POOL_SIZE)
		return nullptr;
	u32 *slot = s_fconstPool[s_fconstCount];
	slot[0] = slot[1] = slot[2] = slot[3] = value;
	index[value] = s_fconstCount++;
	return slot;
}

// Stack scratch for moving values between GPRs and FPRs (the PPC has no direct move). Inside
// the parameter save area of our frame, which is free between calls.
static constexpr int FTEMP = 104;    // One word at 104(r1).
static constexpr int VTEMP = 80;     // Four words (or two doublewords) at 80..95(r1).

// ---------------------------------------------------------------------------------------------
// Basic

void PPCJitBackend::CompIR_Basic(IRInst inst) {
	switch (inst.op) {
	case IROp::SetConst:
		regs_.SetGPRImm(inst.dest, inst.constant);
		break;

	case IROp::Downcount:
		if (inst.constant <= 32767) {
			ADDI(DOWNCOUNTREG, DOWNCOUNTREG, -(s16)inst.constant);
		} else {
			LI32(SCRATCH1, inst.constant);
			SUB(DOWNCOUNTREG, DOWNCOUNTREG, SCRATCH1);
		}
		break;

	case IROp::SetPC:
		regs_.Map(inst);
		STW(regs_.R(inst.src1), CTXREG, (s16)offsetof(MIPSState, pc));
		break;

	case IROp::SetPCConst:
		LI32(SCRATCH1, inst.constant);
		STW(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, pc));
		break;

	case IROp::SetConstF: {
		regs_.Map(inst);
		// Small integers (1.0f, -1.0f, 2.0f...) without memory; anything else from the constant
		// pool (written at compile time, so no load-hit-store as through the stack).
		float f;
		memcpy(&f, &inst.constant, 4);
		const int k = (int)f;
		const bool smallInt = f >= -16.0f && f <= 15.0f && (float)k == f && (inst.constant != 0x80000000);
		const u32 *pooled = smallInt ? nullptr : PoolFloatConstant(inst.constant);
		if (regs_.UsesVMX()) {
			const PPCReg d = regs_.V(inst.dest);
			if (smallInt) {
				VSPLTISW(d, k);
				if (k != 0)
					VCFSX(d, d, 0);
			} else if (pooled) {
				LIPTR(R12, (uintptr_t)pooled);
				LVX(d, R0, R12);
			} else {
				LI32(SCRATCH1, inst.constant);
				GPRToVR(d, SCRATCH1);
			}
			break;
		}
		if (pooled) {
			LIPTR(R12, (uintptr_t)pooled);
			LFS(regs_.F(inst.dest), R12, 0);
			break;
		}
		LI32(SCRATCH1, inst.constant);
		STW(SCRATCH1, R1, FTEMP);
		LFS(regs_.F(inst.dest), R1, FTEMP);
		break;
	}

	default:
		DISABLE;
	}
}

// ---------------------------------------------------------------------------------------------
// ALU

void PPCJitBackend::CompIR_Arith(IRInst inst) {
	switch (inst.op) {
	case IROp::Add:
		regs_.Map(inst);
		ADD(regs_.R(inst.dest), regs_.R(inst.src1), regs_.R(inst.src2));
		break;
	case IROp::Sub:
		regs_.Map(inst);
		SUB(regs_.R(inst.dest), regs_.R(inst.src1), regs_.R(inst.src2));
		break;
	case IROp::AddConst:
	case IROp::SubConst: {
		const s32 c = inst.op == IROp::AddConst ? (s32)inst.constant : -(s32)inst.constant;
		regs_.Map(inst);
		if (FitsS16(c)) {
			ADDI(regs_.R(inst.dest), regs_.R(inst.src1), (s16)c);
		} else {
			LI32(SCRATCH1, (u32)c);
			ADD(regs_.R(inst.dest), regs_.R(inst.src1), SCRATCH1);
		}
		break;
	}
	case IROp::Neg:
		regs_.Map(inst);
		NEG(regs_.R(inst.dest), regs_.R(inst.src1));
		break;
	default:
		INVALIDOP;
	}
}

// For a 32-bit mask made of one run of ones (possibly wrapping), the rlwinm mb/me.
static bool MaskToMBME(u32 mask, int *mb, int *me) {
	if (mask == 0)
		return false;
	// Bit numbering: PPC bit 0 is the MSB.
	for (int b = 0; b < 32; ++b) {
		for (int e = 0; e < 32; ++e) {
			u32 m;
			if (b <= e)
				m = (0xFFFFFFFFu >> b) & (0xFFFFFFFFu << (31 - e));
			else
				m = (0xFFFFFFFFu >> b) | (0xFFFFFFFFu << (31 - e));
			if (m == mask) {
				*mb = b;
				*me = e;
				return true;
			}
		}
	}
	return false;
}

void PPCJitBackend::CompIR_Logic(IRInst inst) {
	switch (inst.op) {
	case IROp::And:
		regs_.Map(inst);
		AND(regs_.R(inst.dest), regs_.R(inst.src1), regs_.R(inst.src2));
		break;
	case IROp::Or:
		regs_.Map(inst);
		OR(regs_.R(inst.dest), regs_.R(inst.src1), regs_.R(inst.src2));
		break;
	case IROp::Xor:
		regs_.Map(inst);
		XOR(regs_.R(inst.dest), regs_.R(inst.src1), regs_.R(inst.src2));
		break;
	case IROp::AndConst: {
		regs_.Map(inst);
		const u32 c = inst.constant;
		int mb, me;
		if (c <= 0xFFFF) {
			ANDI_(regs_.R(inst.dest), regs_.R(inst.src1), (u16)c);
		} else if ((c & 0xFFFF) == 0) {
			ANDIS_(regs_.R(inst.dest), regs_.R(inst.src1), (u16)(c >> 16));
		} else if (MaskToMBME(c, &mb, &me)) {
			RLWINM(regs_.R(inst.dest), regs_.R(inst.src1), 0, mb, me);
		} else {
			LI32(SCRATCH1, c);
			AND(regs_.R(inst.dest), regs_.R(inst.src1), SCRATCH1);
		}
		break;
	}
	case IROp::OrConst:
	case IROp::XorConst: {
		regs_.Map(inst);
		const u32 c = inst.constant;
		const PPCReg d = regs_.R(inst.dest), s = regs_.R(inst.src1);
		const bool isOr = inst.op == IROp::OrConst;
		if (c == 0) {
			MR(d, s);
		} else if ((c & 0xFFFF0000) == 0) {
			if (isOr) ORI(d, s, (u16)c); else XORI(d, s, (u16)c);
		} else if ((c & 0xFFFF) == 0) {
			if (isOr) ORIS(d, s, (u16)(c >> 16)); else XORIS(d, s, (u16)(c >> 16));
		} else {
			if (isOr) { ORIS(d, s, (u16)(c >> 16)); ORI(d, d, (u16)c); }
			else { XORIS(d, s, (u16)(c >> 16)); XORI(d, d, (u16)c); }
		}
		break;
	}
	case IROp::Not:
		regs_.Map(inst);
		NOT(regs_.R(inst.dest), regs_.R(inst.src1));
		break;
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_Assign(IRInst inst) {
	switch (inst.op) {
	case IROp::Mov:
		if (inst.dest != inst.src1) {
			regs_.Map(inst);
			MR(regs_.R(inst.dest), regs_.R(inst.src1));
		}
		break;
	case IROp::Ext8to32:
		regs_.Map(inst);
		EXTSB(regs_.R(inst.dest), regs_.R(inst.src1));
		break;
	case IROp::Ext16to32:
		regs_.Map(inst);
		EXTSH(regs_.R(inst.dest), regs_.R(inst.src1));
		break;
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_Bits(IRInst inst) {
	switch (inst.op) {
	case IROp::Clz:
		regs_.Map(inst);
		CNTLZW(regs_.R(inst.dest), regs_.R(inst.src1));
		break;
	case IROp::BSwap32: {
		regs_.Map(inst);
		// Rotate left 8, then insert the two bytes that need the other rotation.
		const PPCReg d = regs_.R(inst.dest), s = regs_.R(inst.src1);
		RLWINM(SCRATCH1, s, 8, 0, 31);          // b2 b1 b0 b3 -> (s rotl 8)
		RLWIMI(SCRATCH1, s, 24, 0, 7);          // top byte from rotl 24
		RLWIMI(SCRATCH1, s, 24, 16, 23);        // third byte from rotl 24
		MR(d, SCRATCH1);
		break;
	}
	case IROp::BSwap16: {
		// Swaps the bytes within each halfword.
		regs_.Map(inst);
		const PPCReg d = regs_.R(inst.dest), s = regs_.R(inst.src1);
		RLWINM(SCRATCH1, s, 8, 0, 31);          // rotl 8: bytes b2 b1 b0 b3 (b3 = top)
		LI32(SCRATCH2, 0xFF00FF00);
		AND(SCRATCH1, SCRATCH1, SCRATCH2);      // keep (b2 at 31..24? ) see below
		RLWINM(SCRATCH2, s, 24, 0, 31);         // rotl 24
		LI32(R12, 0x00FF00FF);
		AND(SCRATCH2, SCRATCH2, R12);
		OR(d, SCRATCH1, SCRATCH2);
		break;
	}
	case IROp::ReverseBits: {
		// Swap adjacent bits, pairs and nibbles, then reverse the bytes (DOA Paradise runs millions).
		regs_.Map(inst);
		const PPCReg d = regs_.R(inst.dest), s = regs_.R(inst.src1);
		static const u32 masks[3] = { 0x55555555, 0x33333333, 0x0F0F0F0F };
		PPCReg t = s;
		for (int i = 0; i < 3; i++) {
			const int k = 1 << i;
			LI32(R12, masks[i]);
			SRWI(SCRATCH2, t, k);
			AND(SCRATCH2, SCRATCH2, R12);
			AND(SCRATCH1, t, R12);
			SLWI(SCRATCH1, SCRATCH1, k);
			OR(SCRATCH1, SCRATCH1, SCRATCH2);
			t = SCRATCH1;
		}
		RLWINM(SCRATCH2, SCRATCH1, 8, 0, 31);
		RLWIMI(SCRATCH2, SCRATCH1, 24, 0, 7);
		RLWIMI(SCRATCH2, SCRATCH1, 24, 16, 23);
		MR(d, SCRATCH2);
		break;
	}
	default:
		DISABLE;
	}
}

void PPCJitBackend::CompIR_Shift(IRInst inst) {
	switch (inst.op) {
	case IROp::Shl:
	case IROp::Shr:
	case IROp::Sar:
		regs_.Map(inst);
		// MIPS uses the low 5 bits of the amount; PPC 32-bit shifts use 6 (32..63 give 0).
		CLRLWI(SCRATCH1, regs_.R(inst.src2), 27);
		if (inst.op == IROp::Shl) SLW(regs_.R(inst.dest), regs_.R(inst.src1), SCRATCH1);
		else if (inst.op == IROp::Shr) SRW(regs_.R(inst.dest), regs_.R(inst.src1), SCRATCH1);
		else SRAW(regs_.R(inst.dest), regs_.R(inst.src1), SCRATCH1);
		break;
	case IROp::Ror:
		regs_.Map(inst);
		// rotr n == rotl (32 - n); rlwnm uses the low 5 bits.
		NEG(SCRATCH1, regs_.R(inst.src2));
		RLWNM(regs_.R(inst.dest), regs_.R(inst.src1), SCRATCH1, 0, 31);
		break;
	case IROp::ShlImm:
	case IROp::ShrImm:
	case IROp::SarImm:
	case IROp::RorImm: {
		regs_.Map(inst);
		const int n = inst.src2 & 31;
		const PPCReg d = regs_.R(inst.dest), s = regs_.R(inst.src1);
		if (n == 0) {
			MR(d, s);
		} else if (inst.op == IROp::ShlImm) {
			SLWI(d, s, n);
		} else if (inst.op == IROp::ShrImm) {
			SRWI(d, s, n);
		} else if (inst.op == IROp::SarImm) {
			SRAWI(d, s, n);
		} else {
			ROTLWI(d, s, 32 - n);
		}
		break;
	}
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_Compare(IRInst inst) {
	switch (inst.op) {
	case IROp::Slt:
	case IROp::SltU:
		regs_.Map(inst);
		// 64-bit difference of the extended values: its sign bit is the answer.
		if (inst.op == IROp::Slt) {
			EXTSW(SCRATCH1, regs_.R(inst.src1));
			EXTSW(SCRATCH2, regs_.R(inst.src2));
		} else {
			CLRLDI(SCRATCH1, regs_.R(inst.src1), 32);
			CLRLDI(SCRATCH2, regs_.R(inst.src2), 32);
		}
		SUB(SCRATCH1, SCRATCH1, SCRATCH2);
		SRDI(regs_.R(inst.dest), SCRATCH1, 63);
		break;
	case IROp::SltConst:
	case IROp::SltUConst:
		regs_.Map(inst);
		if (inst.op == IROp::SltConst) {
			EXTSW(SCRATCH1, regs_.R(inst.src1));
			LI32(SCRATCH2, inst.constant);  // Sign-extended 64-bit value.
		} else {
			CLRLDI(SCRATCH1, regs_.R(inst.src1), 32);
			LI32(SCRATCH2, inst.constant);
			CLRLDI(SCRATCH2, SCRATCH2, 32);
		}
		SUB(SCRATCH1, SCRATCH1, SCRATCH2);
		SRDI(regs_.R(inst.dest), SCRATCH1, 63);
		break;
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_CondAssign(IRInst inst) {
	switch (inst.op) {
	case IROp::MovZ:
	case IROp::MovNZ: {
		if (inst.dest == inst.src2)
			return;
		regs_.Map(inst);
		CMPWI(regs_.R(inst.src1), 0);
		FixupBranch skip = BC(inst.op == IROp::MovZ ? Cond::NE : Cond::EQ);
		MR(regs_.R(inst.dest), regs_.R(inst.src2));
		SetJumpTarget(skip);
		break;
	}
	case IROp::Max:
	case IROp::Min: {
		regs_.Map(inst);
		const PPCReg d = regs_.R(inst.dest), a = regs_.R(inst.src1), b = regs_.R(inst.src2);
		if (inst.src1 == inst.src2) {
			MR(d, a);
			break;
		}
		CMPW(a, b);
		// Max: a > b ? a : b. Min: a < b ? a : b. Each path moves once (d may be a or b).
		FixupBranch useA = BC(inst.op == IROp::Max ? Cond::GT : Cond::LT);
		MR(d, b);
		FixupBranch done = B();
		SetJumpTarget(useA);
		MR(d, a);
		SetJumpTarget(done);
		break;
	}
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_HiLo(IRInst inst) {
	switch (inst.op) {
	case IROp::MtLo:
		regs_.MapWithExtra(inst, { { 'G', IRREG_LO, 1, MIPSMap::NOINIT } });
		MR(regs_.R(IRREG_LO), regs_.R(inst.src1));
		break;
	case IROp::MtHi:
		regs_.MapWithExtra(inst, { { 'G', IRREG_HI, 1, MIPSMap::NOINIT } });
		MR(regs_.R(IRREG_HI), regs_.R(inst.src1));
		break;
	case IROp::MfLo:
		regs_.MapWithExtra(inst, { { 'G', IRREG_LO, 1, MIPSMap::INIT } });
		MR(regs_.R(inst.dest), regs_.R(IRREG_LO));
		break;
	case IROp::MfHi:
		regs_.MapWithExtra(inst, { { 'G', IRREG_HI, 1, MIPSMap::INIT } });
		MR(regs_.R(inst.dest), regs_.R(IRREG_HI));
		break;
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_Mult(IRInst inst) {
	switch (inst.op) {
	case IROp::Mult:
	case IROp::MultU: {
		regs_.MapWithExtra(inst, { { 'G', IRREG_LO, 1, MIPSMap::NOINIT }, { 'G', IRREG_HI, 1, MIPSMap::NOINIT } });
		const PPCReg a = regs_.R(inst.src1), b = regs_.R(inst.src2);
		// Compute both before writing (LO/HI can't alias the sources: different IR regs).
		MULLW(SCRATCH1, a, b);
		if (inst.op == IROp::Mult) MULHW(SCRATCH2, a, b); else MULHWU(SCRATCH2, a, b);
		MR(regs_.R(IRREG_LO), SCRATCH1);
		MR(regs_.R(IRREG_HI), SCRATCH2);
		break;
	}
	case IROp::Madd:
	case IROp::MaddU:
	case IROp::Msub:
	case IROp::MsubU: {
		// HI:LO +/-= rs * rt as one 64-bit operation.
		regs_.MapWithExtra(inst, { { 'G', IRREG_LO, 1, MIPSMap::DIRTY }, { 'G', IRREG_HI, 1, MIPSMap::DIRTY } });
		const PPCReg a = regs_.R(inst.src1), b = regs_.R(inst.src2);
		const PPCReg lo = regs_.R(IRREG_LO), hi = regs_.R(IRREG_HI);
		const bool isSigned = inst.op == IROp::Madd || inst.op == IROp::Msub;
		if (isSigned) {
			EXTSW(SCRATCH1, a);
			EXTSW(SCRATCH2, b);
		} else {
			CLRLDI(SCRATCH1, a, 32);
			CLRLDI(SCRATCH2, b, 32);
		}
		MULLD(SCRATCH1, SCRATCH1, SCRATCH2);
		SLDI(SCRATCH2, hi, 32);
		CLRLDI(R12, lo, 32);
		OR(SCRATCH2, SCRATCH2, R12);
		if (inst.op == IROp::Madd || inst.op == IROp::MaddU)
			ADD(SCRATCH2, SCRATCH2, SCRATCH1);
		else
			SUB(SCRATCH2, SCRATCH2, SCRATCH1);
		MR(lo, SCRATCH2);
		SRDI(hi, SCRATCH2, 32);
		break;
	}
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_Div(IRInst inst) {
	// Same results as the interpreter for division by zero and the one signed overflow.
	regs_.MapWithExtra(inst, { { 'G', IRREG_LO, 1, MIPSMap::NOINIT }, { 'G', IRREG_HI, 1, MIPSMap::NOINIT } });
	const PPCReg a = regs_.R(inst.src1), b = regs_.R(inst.src2);
	const PPCReg lo = regs_.R(IRREG_LO), hi = regs_.R(IRREG_HI);
	switch (inst.op) {
	case IROp::Div: {
		CMPWI(b, 0);
		FixupBranch zero = BC(Cond::EQ);
		CMPWI(b, -1);
		FixupBranch normal1 = BC(Cond::NE);
		LIS(SCRATCH1, (s16)0x8000);
		CMPW(a, SCRATCH1);
		FixupBranch normal2 = BC(Cond::NE);
		// 0x80000000 / -1: lo = 0x80000000, hi = 0.
		MR(lo, SCRATCH1);
		LI(hi, 0);
		FixupBranch done1 = B();
		SetJumpTarget(normal1);
		SetJumpTarget(normal2);
		DIVW(SCRATCH1, a, b);
		MULLW(SCRATCH2, SCRATCH1, b);
		SUB(SCRATCH2, a, SCRATCH2);
		MR(lo, SCRATCH1);
		MR(hi, SCRATCH2);
		FixupBranch done2 = B();
		SetJumpTarget(zero);
		// lo = a < 0 ? 1 : -1, hi = a.
		SRAWI(SCRATCH1, a, 31);
		ADD(SCRATCH1, SCRATCH1, SCRATCH1);
		SUBFIC(SCRATCH1, SCRATCH1, -1);
		MR(hi, a);
		MR(lo, SCRATCH1);
		SetJumpTarget(done1);
		SetJumpTarget(done2);
		break;
	}
	case IROp::DivU: {
		CMPWI(b, 0);
		FixupBranch zero = BC(Cond::EQ);
		DIVWU(SCRATCH1, a, b);
		MULLW(SCRATCH2, SCRATCH1, b);
		SUB(SCRATCH2, a, SCRATCH2);
		MR(lo, SCRATCH1);
		MR(hi, SCRATCH2);
		FixupBranch done = B();
		SetJumpTarget(zero);
		// lo = a <= 0xFFFF ? 0xFFFF : -1, hi = a.
		LI(SCRATCH1, -1);
		CMPLWI(a, 0xFFFF);
		FixupBranch big = BC(Cond::GT);
		LI32(SCRATCH1, 0xFFFF);
		SetJumpTarget(big);
		MR(hi, a);
		MR(lo, SCRATCH1);
		SetJumpTarget(done);
		break;
	}
	default:
		INVALIDOP;
	}
}

// ---------------------------------------------------------------------------------------------
// Memory

void PPCJitBackend::AddressToScratch2(IRInst inst) {
	const PPCReg base = regs_.R(inst.src1);
	const s32 offset = (s32)inst.constant;
	// The stack pointer always points into user RAM (0x08000000-0x0A000000, no mirror bits), and
	// stack offsets are small: no masking. (A quarter of the loads and stores are sp-relative.)
	if (inst.src1 == MIPS_REG_SP && offset >= -0x8000 && offset < 0x8000) {
		ADDI(SCRATCH2, base, (s16)offset);
		return;
	}
	if (offset == 0) {
		CLRLWI(SCRATCH2, base, 2);
	} else if (FitsS16(offset)) {
		ADDI(SCRATCH2, base, (s16)offset);
		CLRLWI(SCRATCH2, SCRATCH2, 2);
	} else {
		LI32(SCRATCH2, (u32)offset);
		ADD(SCRATCH2, SCRATCH2, base);
		CLRLWI(SCRATCH2, SCRATCH2, 2);
	}
}

void PPCJitBackend::CompIR_Load(IRInst inst) {
	switch (inst.op) {
	case IROp::Load8:
	case IROp::Load8Ext:
	case IROp::Load16:
	case IROp::Load16Ext:
	case IROp::Load32: {
		regs_.Map(inst);
		AddressToScratch2(inst);
		const PPCReg d = regs_.R(inst.dest);
		switch (inst.op) {
		case IROp::Load8: LBZX(d, MEMBASEREG, SCRATCH2); break;
		case IROp::Load8Ext: LBZX(d, MEMBASEREG, SCRATCH2); EXTSB(d, d); break;
		case IROp::Load16: LHBRX(d, MEMBASEREG, SCRATCH2); break;
		case IROp::Load16Ext: LHBRX(d, MEMBASEREG, SCRATCH2); EXTSH(d, d); break;
		default: LWBRX(d, MEMBASEREG, SCRATCH2); break;
		}
		break;
	}
	case IROp::Load32Linked:
		if (inst.dest != MIPS_REG_ZERO) {
			regs_.Map(inst);
			AddressToScratch2(inst);
			LWBRX(regs_.R(inst.dest), MEMBASEREG, SCRATCH2);
		}
		LI(SCRATCH1, 1);
		STW(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, llBit));
		break;
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_Store(IRInst inst) {
	switch (inst.op) {
	case IROp::Store8:
	case IROp::Store16:
	case IROp::Store32: {
		regs_.Map(inst);
		AddressToScratch2(inst);
		const PPCReg v = regs_.R(inst.src3);
		if (inst.op == IROp::Store8) STBX(v, MEMBASEREG, SCRATCH2);
		else if (inst.op == IROp::Store16) STHBRX(v, MEMBASEREG, SCRATCH2);
		else STWBRX(v, MEMBASEREG, SCRATCH2);
		break;
	}
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_LoadShift(IRInst inst) { DISABLE; }
void PPCJitBackend::CompIR_StoreShift(IRInst inst) { DISABLE; }
void PPCJitBackend::CompIR_CondStore(IRInst inst) {
	if (inst.op != IROp::Store32Conditional) {
		INVALIDOP;
	}
	// Store only while the ll bit is set; dest = whether it stored.
	regs_.Map(inst);
	LWZ(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, llBit));
	CMPWI(SCRATCH1, 0);
	FixupBranch skip = BC(Cond::EQ);
	AddressToScratch2(inst);
	STWBRX(regs_.R(inst.src3), MEMBASEREG, SCRATCH2);
	SetJumpTarget(skip);
	if (inst.dest != MIPS_REG_ZERO) {
		// 1 if llBit != 0, else 0.
		CNTLZW(SCRATCH1, SCRATCH1);
		SRWI(SCRATCH1, SCRATCH1, 5);
		XORI(regs_.R(inst.dest), SCRATCH1, 1);
	}
}
void PPCJitBackend::CompIR_ValidateAddress(IRInst inst) { DISABLE; }

// ---------------------------------------------------------------------------------------------
// Exits

void PPCJitBackend::CompIR_Exit(IRInst inst) {
	switch (inst.op) {
	case IROp::ExitToConst:
		FlushAll();
		WriteConstExit(inst.constant);
		break;
	case IROp::ExitToReg: {
		const PPCReg exitReg = regs_.MapGPR(inst.src1);
		FlushAll();
		MR(SCRATCH1, exitReg);
		if (!jo.enableBlocklink) {
			B(dispatcherPCInSCRATCH1_);
			break;
		}
		// Inline cache (PatchReturnIC fills in the PC and the branch): most returns go back to
		// the same place as last time, and a direct branch avoids the dispatcher's lookup and its
		// indirect branch (mtctr/bctr, which the PPU can't predict).
		const int index = (int)ics_.size();
		ics_.push_back({ (int)GetOffset(GetCodePointer()), 0 });
		LIS(R12, 0);
		ORI(R12, R12, 0);
		CMPLW(SCRATCH1, R12);
		FixupBranch miss1 = BC(Cond::NE);
		FixupBranch miss2 = B();
		SetJumpTarget(miss1);
		SetJumpTarget(miss2);
		LIS(SCRATCH2, (s16)(index >> 16));
		ORI(SCRATCH2, SCRATCH2, (u16)(index & 0xFFFF));
		B(dispatcherICMiss_);
		break;
	}
	case IROp::ExitToPC:
		FlushAll();
		B(dispatcherCheckCoreState_);
		break;
	default:
		INVALIDOP;
	}
}

// The taken side of a conditional exit: store the registers there only. The code after the exit
// keeps them in host registers (dirty), instead of storing them on both paths and loading them
// back right after (a load-hit-store stall on the PPU for each).
void PPCJitBackend::ExitPathFlushAndExit(uint32_t pc) {
	regs_.EmitFlushStores();
	WriteConstExit(pc);
}

// A conditional exit's stores and jump go to the far code: the usual not-taken case falls through
// with no taken branch and no exit code in between (the PPU fetches and caches only the hot path;
// the inline exit stubs made blocks several cache lines longer). The stores are captured now, as
// the mapping is at this point, and copied there (they don't depend on where they are).
void PPCJitBackend::FarExitIf(FixupBranch taken, uint32_t pc) {
	static u32 scratch[2048];
	const u8 *savedCode = GetCodePointer();
	u8 *savedWritable = GetWritableCodePtr();
	SetCodePointer((const u8 *)scratch, (u8 *)scratch);
	regs_.EmitFlushStores();
	const size_t words = (GetCodePointer() - (const u8 *)scratch) / 4;
	SetCodePointer(savedCode, savedWritable);
	std::vector<u32> stores(scratch, scratch + words);
	AddFarCode(taken, [this, stores, pc]() {
		for (u32 w : stores)
			Write32(w);
		WriteConstExit(pc);
	});
}

void PPCJitBackend::CompIR_ExitIf(IRInst inst) {
	switch (inst.op) {
	case IROp::ExitToConstIfEq:
	case IROp::ExitToConstIfNeq:
		if (inst.src2 == MIPS_REG_ZERO || inst.src1 == MIPS_REG_ZERO) {
			// Against zero (most branches): no register for it.
			const IRReg other = inst.src2 == MIPS_REG_ZERO ? inst.src1 : inst.src2;
			CMPWI(regs_.MapGPR(other), 0);
		} else {
			regs_.Map(inst);
			CMPW(regs_.R(inst.src1), regs_.R(inst.src2));
		}
		FarExitIf(BC(inst.op == IROp::ExitToConstIfEq ? Cond::EQ : Cond::NE), inst.constant);
		break;

	case IROp::ExitToConstIfGtZ:
	case IROp::ExitToConstIfGeZ:
	case IROp::ExitToConstIfLtZ:
	case IROp::ExitToConstIfLeZ: {
		regs_.Map(inst);
		CMPWI(regs_.R(inst.src1), 0);
		Cond taken = Cond::GT;
		switch (inst.op) {
		case IROp::ExitToConstIfGtZ: taken = Cond::GT; break;
		case IROp::ExitToConstIfGeZ: taken = Cond::GE; break;
		case IROp::ExitToConstIfLtZ: taken = Cond::LT; break;
		default: taken = Cond::LE; break;
		}
		FarExitIf(BC(taken), inst.constant);
		break;
	}

	default:
		// FP condition exits.
		DISABLE;
	}
}

// ---------------------------------------------------------------------------------------------
// Everything else goes through the IR interpreter for now.

void PPCJitBackend::CompIR_Breakpoint(IRInst inst) { DISABLE; }
static void SyscallHelper(u32 op) {
	CallSyscall(MIPSOpcode(op));
}

void PPCJitBackend::CompIR_System(IRInst inst) {
	switch (inst.op) {
	case IROp::Syscall:
		FlushAll();
		SaveStaticRegisters();
		LIPTR(SCRATCH1, inst.constant);  // Zero-extended u32 argument.
		QuickCallFunction(&SyscallHelper);
		LoadStaticRegisters();
		// Always followed by an ExitToPC, which checks coreState.
		break;
	default:
		DISABLE;
	}
}
void PPCJitBackend::CompIR_RoundingMode(IRInst inst) {
	// FPSCR RN uses the same encoding as MIPS FCR31 (nearest, zero, +inf, -inf).
	switch (inst.op) {
	case IROp::RestoreRoundingMode:
		// Field 7: XE, NI and RN - all zero is round to nearest, IEEE mode.
		MTFSFI(7, 0);
		break;
	case IROp::ApplyRoundingMode: {
		regs_.MapWithExtra(inst, { { 'G', IRREG_FCR31, 1, MIPSMap::INIT } });
		const PPCReg fcr31 = regs_.R(IRREG_FCR31);
		MFFS(FSCRATCH1);
		STFD(FSCRATCH1, R1, VTEMP);
		LWZ(SCRATCH1, R1, VTEMP + 4);
		RLWIMI(SCRATCH1, fcr31, 0, 30, 31);
		// FCR31 FS (bit 24, flush denormals to zero) into FPSCR NI (0x4): the PSP flushes them,
		// IEEE mode keeps them (a value the game expects to be zero isn't, 1/x overflows...).
		if (HostFlushDenormals())
			RLWIMI(SCRATCH1, fcr31, 10, 29, 29);
		STW(SCRATCH1, R1, VTEMP + 4);
		LFD(FSCRATCH1, R1, VTEMP);
		MTFSF(1, FSCRATCH1);
		break;
	}
	default:
		// UpdateRoundingMode.
		DISABLE;
	}
}

// After FCMPU in CR0: extracts one of its bits (0 LT, 1 GT, 2 EQ, 3 unordered) from a MFCR value.
static void ExtractCR0Bit(PPCEmitter *e, PPCReg dest, PPCReg cr, int bit) {
	e->RLWINM(dest, cr, bit + 1, 31, 31);
}

void PPCJitBackend::CompIR_FCompare(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_FCompare(inst);
		return;
	}
	switch (inst.op) {
	case IROp::FCmp: {
		if (inst.dest == IRFpCompareMode::False) {
			regs_.SetGPRImm(IRREG_FPCOND, 0);
			break;
		}
		regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
		const PPCReg d = regs_.R(IRREG_FPCOND);
		FCMPU(regs_.F(inst.src1), regs_.F(inst.src2));
		MFOCRF(SCRATCH1, 0);
		enum { LT = 0, GT = 1, EQ = 2, UN = 3 };
		auto either = [&](int b1, int b2) {
			ExtractCR0Bit(this, SCRATCH2, SCRATCH1, b1);
			ExtractCR0Bit(this, d, SCRATCH1, b2);
			OR(d, d, SCRATCH2);
		};
		switch (inst.dest) {
		case IRFpCompareMode::EitherUnordered: ExtractCR0Bit(this, d, SCRATCH1, UN); break;
		case IRFpCompareMode::EqualOrdered: ExtractCR0Bit(this, d, SCRATCH1, EQ); break;
		case IRFpCompareMode::EqualUnordered: either(EQ, UN); break;
		case IRFpCompareMode::LessEqualOrdered: either(LT, EQ); break;
		// !(a > b)
		case IRFpCompareMode::LessEqualUnordered: ExtractCR0Bit(this, d, SCRATCH1, GT); XORI(d, d, 1); break;
		case IRFpCompareMode::LessOrdered: ExtractCR0Bit(this, d, SCRATCH1, LT); break;
		// !(a >= b)
		case IRFpCompareMode::LessUnordered: either(LT, UN); break;
		default: ExtractCR0Bit(this, d, SCRATCH1, UN); break;
		}
		break;
	}
	case IROp::FCmpVfpuBit: {
		const int op = inst.dest & 0xF;
		const int bit = inst.dest >> 4;
		const bool twoOperands = op == VC_EQ || op == VC_LT || op == VC_LE || op == VC_NE || op == VC_GE || op == VC_GT;
		const bool zeroCompare = op == VC_EZ || op == VC_NZ;
		if (!(twoOperands || zeroCompare || op == VC_TR || op == VC_FL)) {
			// NaN / infinity tests.
			DISABLE;
		}
		regs_.MapWithExtra(inst, { { 'G', IRREG_VFPU_CC, 1, MIPSMap::DIRTY } });
		const PPCReg cc = regs_.R(IRREG_VFPU_CC);
		enum { LT = 0, GT = 1, EQ = 2, UN = 3 };
		if (op == VC_TR || op == VC_FL) {
			LI(SCRATCH1, op == VC_TR ? 1 : 0);
		} else {
			if (twoOperands) {
				FCMPU(regs_.F(inst.src1), regs_.F(inst.src2));
			} else {
				LI(SCRATCH1, 0);
				STW(SCRATCH1, R1, FTEMP);
				LFS(FSCRATCH2, R1, FTEMP);
				FCMPU(regs_.F(inst.src1), FSCRATCH2);
			}
			MFOCRF(SCRATCH2, 0);
			switch (op) {
			case VC_EQ: case VC_EZ: ExtractCR0Bit(this, SCRATCH1, SCRATCH2, EQ); break;
			case VC_NE: case VC_NZ: ExtractCR0Bit(this, SCRATCH1, SCRATCH2, EQ); XORI(SCRATCH1, SCRATCH1, 1); break;
			case VC_LT: ExtractCR0Bit(this, SCRATCH1, SCRATCH2, LT); break;
			case VC_GT: ExtractCR0Bit(this, SCRATCH1, SCRATCH2, GT); break;
			case VC_LE:
				ExtractCR0Bit(this, SCRATCH1, SCRATCH2, LT);
				ExtractCR0Bit(this, SCRATCH2, SCRATCH2, EQ);
				OR(SCRATCH1, SCRATCH1, SCRATCH2);
				break;
			default:  // VC_GE
				ExtractCR0Bit(this, SCRATCH1, SCRATCH2, GT);
				ExtractCR0Bit(this, SCRATCH2, SCRATCH2, EQ);
				OR(SCRATCH1, SCRATCH1, SCRATCH2);
				break;
			}
		}
		// Insert as bit `bit` of CC.
		RLWIMI(cc, SCRATCH1, bit, 31 - bit, 31 - bit);
		break;
	}
	case IROp::FCmovVfpuCC: {
		regs_.MapWithExtra(inst, { { 'G', IRREG_VFPU_CC, 1, MIPSMap::INIT } });
		const int bit = inst.src2 & 0xF;
		RLWINM(SCRATCH1, regs_.R(IRREG_VFPU_CC), (32 - bit) & 31, 31, 31);
		CMPWI(SCRATCH1, 0);
		FixupBranch skip = BC((inst.src2 >> 7) & 1 ? Cond::EQ : Cond::NE);
		FMR(regs_.F(inst.dest), regs_.F(inst.src1));
		SetJumpTarget(skip);
		break;
	}
	case IROp::FCmpVfpuAggregate: {
		regs_.MapWithExtra(inst, { { 'G', IRREG_VFPU_CC, 1, MIPSMap::DIRTY } });
		const PPCReg cc = regs_.R(IRREG_VFPU_CC);
		const u32 mask = inst.dest;
		// any = (cc & mask) != 0 -> 0x10, all = (cc & mask) == mask -> 0x20.
		ANDI_(SCRATCH1, cc, (u16)mask);
		LI(SCRATCH2, 0);
		FixupBranch none = BC(Cond::EQ);
		LI(SCRATCH2, 0x10);
		CMPLWI(SCRATCH1, (u16)mask);
		FixupBranch notAll = BC(Cond::NE);
		LI(SCRATCH2, 0x30);
		SetJumpTarget(notAll);
		SetJumpTarget(none);
		LI(SCRATCH1, 0x30);
		ANDC(cc, cc, SCRATCH1);
		OR(cc, cc, SCRATCH2);
		break;
	}
	default:
		DISABLE;
	}
}

void PPCJitBackend::CompIR_FCondAssign(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_FCondAssign(inst);
		return;
	}
	if (inst.op != IROp::FMin && inst.op != IROp::FMax)
		DISABLE;
	regs_.Map(inst);
	EmitFMinMax(inst.op == IROp::FMin, regs_.F(inst.dest), regs_.F(inst.src1), regs_.F(inst.src2));
}

void PPCJitBackend::EmitFMinMax(bool isMin, PPCReg d, PPCReg a, PPCReg b) {
	FCMPU(a, b);
	FixupBranch nan = BC(Cond::SO);
	// std::min(a, b) = b < a ? b : a; std::max(a, b) = a < b ? b : a.
	FixupBranch takeB = BC(isMin ? Cond::GT : Cond::LT);
	FMR(d, a);
	FixupBranch done1 = B();
	SetJumpTarget(takeB);
	FMR(d, b);
	FixupBranch done2 = B();
	SetJumpTarget(nan);
	// A NaN: compare the bits as signed integers (the interpreter's vmin/vmax rule): both
	// negative -> the other way around.
	STFS(a, R1, VTEMP);
	STFS(b, R1, VTEMP + 4);
	LWZ(SCRATCH1, R1, VTEMP);
	LWZ(SCRATCH2, R1, VTEMP + 4);
	AND(SCRATCH3, SCRATCH1, SCRATCH2);
	CMPWI(SCRATCH3, 0, CR1);
	CMPW(SCRATCH1, SCRATCH2);
	// Want the smaller one when (isMin and not both negative) or (!isMin and both negative).
	FixupBranch bothNeg = BC(Cond::LT, CR1);
	FixupBranch keepA1 = BC(isMin ? Cond::LE : Cond::GE);
	MR(SCRATCH1, SCRATCH2);
	SetJumpTarget(keepA1);
	FixupBranch store1 = B();
	SetJumpTarget(bothNeg);
	FixupBranch keepA2 = BC(isMin ? Cond::GE : Cond::LE);
	MR(SCRATCH1, SCRATCH2);
	SetJumpTarget(keepA2);
	SetJumpTarget(store1);
	STW(SCRATCH1, R1, FTEMP);
	LFS(d, R1, FTEMP);
	SetJumpTarget(done1);
	SetJumpTarget(done2);
}

// int32 bits held in an FPR -> single precision value, into dest.
void PPCJitBackend::IntBitsToFloat(PPCReg dest, PPCReg src) {
	STFS(src, R1, FTEMP);
	LWZ(SCRATCH1, R1, FTEMP);
	EXTSW(SCRATCH1, SCRATCH1);
	STD(SCRATCH1, R1, VTEMP);
	LFD(FSCRATCH1, R1, VTEMP);
	FCFID(FSCRATCH1, FSCRATCH1);
	FRSP(dest, FSCRATCH1);
}

// FPR value -> saturated int32 bits in dest (an FPR), like SaturatedFloatToInt: NaN gives
// 0x7FFFFFFF. roundMode < 0 truncates, otherwise rounds with that FPSCR mode.
void PPCJitBackend::FloatToIntBits(PPCReg dest, PPCReg src, int roundMode) {
	if (roundMode < 0) {
		FCTIWZ(FSCRATCH1, src);
	} else {
		MFFS(FSCRATCH2);
		MTFSFI(7, roundMode);
		FCTIW(FSCRATCH1, src);
		MTFSF(1, FSCRATCH2);
	}
	STFD(FSCRATCH1, R1, VTEMP);
	LWZ(SCRATCH1, R1, VTEMP + 4);
	// fctiw gives 0x80000000 for NaN: subtract 1 then.
	FCMPU(src, src);
	MFOCRF(SCRATCH2, 0);
	ExtractCR0Bit(this, SCRATCH2, SCRATCH2, 3);
	SUB(SCRATCH1, SCRATCH1, SCRATCH2);
	STW(SCRATCH1, R1, FTEMP);
	LFS(dest, R1, FTEMP);
}

void PPCJitBackend::CompIR_FCvt(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_FCvt(inst);
		return;
	}
	switch (inst.op) {
	case IROp::FCvtSW:
		regs_.Map(inst);
		IntBitsToFloat(regs_.F(inst.dest), regs_.F(inst.src1));
		break;
	case IROp::FCvtScaledSW: {
		regs_.Map(inst);
		IntBitsToFloat(FSCRATCH1, regs_.F(inst.src1));
		const float scale = 1.0f / (float)(1UL << (inst.src2 & 0x1F));
		u32 bits;
		memcpy(&bits, &scale, 4);
		LI32(SCRATCH1, bits);
		STW(SCRATCH1, R1, FTEMP);
		LFS(FSCRATCH2, R1, FTEMP);
		FMULS(regs_.F(inst.dest), FSCRATCH1, FSCRATCH2);
		break;
	}
	default:
		// FCvtWS (rounding mode from FCR31), FCvtScaledWS.
		DISABLE;
	}
}

void PPCJitBackend::CompIR_FRound(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_FRound(inst);
		return;
	}
	regs_.Map(inst);
	switch (inst.op) {
	case IROp::FTrunc: FloatToIntBits(regs_.F(inst.dest), regs_.F(inst.src1), -1); break;
	case IROp::FRound: FloatToIntBits(regs_.F(inst.dest), regs_.F(inst.src1), 0); break;
	case IROp::FCeil: FloatToIntBits(regs_.F(inst.dest), regs_.F(inst.src1), 2); break;
	case IROp::FFloor: FloatToIntBits(regs_.F(inst.dest), regs_.F(inst.src1), 3); break;
	default: INVALIDOP;
	}
}
void PPCJitBackend::CompIR_FSat(IRInst inst) {
	if (!regs_.UsesVMX())
		DISABLE;
	// v >= max ? max : (v <= min ? min : v): NaN stays, -0 becomes +0 when min is +0.
	regs_.Map(inst);
	const PPCReg s = regs_.V(inst.src1), d = regs_.V(inst.dest);
	VSPLTISW(V0, inst.op == IROp::FSat0_1 ? 0 : -1);
	VCFSX(V0, V0, 0);
	VSPLTISW(V1, 1);
	VCFSX(V1, V1, 0);
	VCMPGEFP(V2, V0, s);
	VSEL(V0, s, V0, V2);
	VCMPGEFP(V2, s, V1);
	VSEL(d, V0, V1, V2);
}
void PPCJitBackend::CompIR_FSpecial(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_FSpecial(inst);
		return;
	}
	float (*func)(float) = nullptr;
	switch (inst.op) {
	case IROp::FSin: func = &vfpu_sin; break;
	case IROp::FCos: func = &vfpu_cos; break;
	case IROp::FRSqrt: func = &vfpu_rsqrt; break;
	case IROp::FRecip: func = &vfpu_rcp; break;
	case IROp::FVSqrt: func = &vfpu_sqrt; break;
	case IROp::FAsin: func = &vfpu_asin; break;
	// vexp2/vlog2: the interpreter's own bit-exact helpers (Midnight Club L.A. runs them ~13,000
	// times a frame; the generic fallback saved and reloaded every static register for each).
	case IROp::FExp2: func = &vfpu_exp2; break;
	case IROp::FLog2: func = &vfpu_log2; break;
	default: DISABLE;
	}
	// Straight call to the interpreter's own helper (float in f1, result in f1); only the
	// volatile registers are flushed, unlike the generic fallback.
	regs_.FlushBeforeCall();
	if (regs_.IsFPRMapped(inst.src1))
		FMR(F1, regs_.F(inst.src1));
	else
		LFS(F1, CTXREG, (s16)(offsetof(MIPSState, f) + inst.src1 * 4));
	SaveVolatileStatics();
	QuickCallFunction(func);
	LoadVolatileStatics();
	regs_.MapFPR(inst.dest, MIPSMap::NOINIT);
	FMR(regs_.F(inst.dest), F1);
}
void PPCJitBackend::CompIR_VecPack(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_VecPack(inst);
		return;
	}
	switch (inst.op) {
	case IROp::Vec2Unpack16To32:
	case IROp::Vec2Unpack16To31: {
		regs_.Map(inst);
		STFS(regs_.F(inst.src1), R1, FTEMP);
		LWZ(SCRATCH1, R1, FTEMP);
		SLWI(SCRATCH2, SCRATCH1, 16);
		RLWINM(SCRATCH1, SCRATCH1, 0, 0, 15);
		if (inst.op == IROp::Vec2Unpack16To31) {
			SRWI(SCRATCH2, SCRATCH2, 1);
			SRWI(SCRATCH1, SCRATCH1, 1);
		}
		STW(SCRATCH2, R1, VTEMP);
		STW(SCRATCH1, R1, VTEMP + 4);
		LFS(regs_.F(inst.dest), R1, VTEMP);
		LFS(regs_.F(inst.dest + 1), R1, VTEMP + 4);
		break;
	}
	default:
		DISABLE;
	}
}

// ---------------------------------------------------------------------------------------------
// FPU / VFPU (scalar FPRs; the PPC has no direct GPR<->FPR move, so values go through a
// scratch slot in our stack frame - inside the parameter save area, free between calls).


void PPCJitBackend::CompIR_FArith(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_FArith(inst);
		return;
	}
	switch (inst.op) {
	case IROp::FAdd:
		regs_.Map(inst);
		FADDS(regs_.F(inst.dest), regs_.F(inst.src1), regs_.F(inst.src2));
		break;
	case IROp::FSub:
		regs_.Map(inst);
		FSUBS(regs_.F(inst.dest), regs_.F(inst.src1), regs_.F(inst.src2));
		break;
	case IROp::FMul:
		regs_.Map(inst);
		FMULS(regs_.F(inst.dest), regs_.F(inst.src1), regs_.F(inst.src2));
		break;
	case IROp::FDiv:
		regs_.Map(inst);
		FDIVS(regs_.F(inst.dest), regs_.F(inst.src1), regs_.F(inst.src2));
		break;
	case IROp::FNeg:
		regs_.Map(inst);
		FNEG(regs_.F(inst.dest), regs_.F(inst.src1));
		break;
	case IROp::FSqrt:
		// The Cell PPU has fsqrts (GCC emits it for sqrtf with -mcpu=cell, as the interpreter uses).
		regs_.Map(inst);
		FSQRTS(regs_.F(inst.dest), regs_.F(inst.src1));
		break;
	default:
		DISABLE;
	}
}

void PPCJitBackend::CompIR_FAssign(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_FAssign(inst);
		return;
	}
	switch (inst.op) {
	case IROp::FMov:
		if (inst.dest != inst.src1) {
			regs_.Map(inst);
			FMR(regs_.F(inst.dest), regs_.F(inst.src1));
		}
		break;
	case IROp::FAbs:
		regs_.Map(inst);
		FABS(regs_.F(inst.dest), regs_.F(inst.src1));
		break;
	case IROp::FSign: {
		// +1 / -1 by the sign bit; 0 for zero and denormals (as the interpreter).
		regs_.Map(inst);
		STFS(regs_.F(inst.src1), R1, FTEMP);
		LWZ(SCRATCH1, R1, FTEMP);
		RLWINM(SCRATCH2, SCRATCH1, 0, 0, 0);
		ORIS(SCRATCH2, SCRATCH2, 0x3F80);
		ANDIS_(SCRATCH3, SCRATCH1, 0x7F80);
		FixupBranch nonZero = BC(Cond::NE);
		LI(SCRATCH2, 0);
		SetJumpTarget(nonZero);
		STW(SCRATCH2, R1, FTEMP);
		LFS(regs_.F(inst.dest), R1, FTEMP);
		break;
	}
	default:
		DISABLE;
	}
}

void PPCJitBackend::CompIR_FLoad(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_FLoad(inst);
		return;
	}
	if (inst.op != IROp::LoadFloat)
		DISABLE;
	regs_.Map(inst);
	AddressToScratch2(inst);
	LWBRX(SCRATCH1, MEMBASEREG, SCRATCH2);
	STW(SCRATCH1, R1, FTEMP);
	LFS(regs_.F(inst.dest), R1, FTEMP);
}

void PPCJitBackend::CompIR_FStore(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_FStore(inst);
		return;
	}
	if (inst.op != IROp::StoreFloat)
		DISABLE;
	regs_.Map(inst);
	AddressToScratch2(inst);
	STFS(regs_.F(inst.src3), R1, FTEMP);
	LWZ(SCRATCH1, R1, FTEMP);
	STWBRX(SCRATCH1, MEMBASEREG, SCRATCH2);
}

void PPCJitBackend::CompIR_VecLoad(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_VecLoad(inst);
		return;
	}
	if (inst.op != IROp::LoadVec4)
		DISABLE;
	regs_.Map(inst);
	AddressToScratch2(inst);
	for (int i = 0; i < 4; ++i) {
		if (i > 0)
			ADDI(SCRATCH2, SCRATCH2, 4);
		LWBRX(SCRATCH1, MEMBASEREG, SCRATCH2);
		STW(SCRATCH1, R1, (s16)(VTEMP + i * 4));
	}
	for (int i = 0; i < 4; ++i)
		LFS(regs_.F(inst.dest + i), R1, (s16)(VTEMP + i * 4));
}

void PPCJitBackend::CompIR_VecStore(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_VecStore(inst);
		return;
	}
	if (inst.op != IROp::StoreVec4)
		DISABLE;
	regs_.Map(inst);
	AddressToScratch2(inst);
	for (int i = 0; i < 4; ++i)
		STFS(regs_.F(inst.src3 + i), R1, (s16)(VTEMP + i * 4));
	for (int i = 0; i < 4; ++i) {
		if (i > 0)
			ADDI(SCRATCH2, SCRATCH2, 4);
		LWZ(SCRATCH1, R1, (s16)(VTEMP + i * 4));
		STWBRX(SCRATCH1, MEMBASEREG, SCRATCH2);
	}
}

void PPCJitBackend::CompIR_Transfer(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_Transfer(inst);
		return;
	}
	switch (inst.op) {
	case IROp::FMovFromGPR:
		regs_.Map(inst);
		STW(regs_.R(inst.src1), R1, FTEMP);
		LFS(regs_.F(inst.dest), R1, FTEMP);
		break;
	case IROp::FMovToGPR:
		regs_.Map(inst);
		STFS(regs_.F(inst.src1), R1, FTEMP);
		LWZ(regs_.R(inst.dest), R1, FTEMP);
		break;
	case IROp::FpCondFromReg:
		regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
		MR(regs_.R(IRREG_FPCOND), regs_.R(inst.src1));
		break;
	case IROp::FpCondToReg:
		regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::INIT } });
		MR(regs_.R(inst.dest), regs_.R(IRREG_FPCOND));
		break;
	case IROp::SetCtrlVFPU:
		regs_.SetGPRImm(IRREG_VFPU_CTRL_BASE + inst.dest, inst.constant);
		break;
	case IROp::SetCtrlVFPUReg:
		regs_.MapWithExtra(inst, { { 'G', (IRReg)(IRREG_VFPU_CTRL_BASE + inst.dest), 1, MIPSMap::NOINIT } });
		MR(regs_.R(IRREG_VFPU_CTRL_BASE + inst.dest), regs_.R(inst.src1));
		break;
	case IROp::VfpuCtrlToReg:
		regs_.MapWithExtra(inst, { { 'G', (IRReg)(IRREG_VFPU_CTRL_BASE + inst.src1), 1, MIPSMap::INIT } });
		MR(regs_.R(inst.dest), regs_.R(IRREG_VFPU_CTRL_BASE + inst.src1));
		break;
	default:
		DISABLE;
	}
}

static bool Overlap(IRReg r1, int l1, IRReg r2, int l2) {
	return r1 < r2 + l2 && r1 + l1 > r2;
}

void PPCJitBackend::CompIR_VecAssign(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_VecAssign(inst);
		return;
	}
	switch (inst.op) {
	case IROp::Vec4Mov:
		if (inst.dest != inst.src1) {
			regs_.Map(inst);
			for (int i = 0; i < 4; ++i)
				FMR(regs_.F(inst.dest + i), regs_.F(inst.src1 + i));
		}
		break;
	case IROp::Vec4Blend:
		regs_.Map(inst);
		for (int i = 0; i < 4; ++i) {
			const IRReg srcReg = ((inst.constant >> i) & 1) ? inst.src2 : inst.src1;
			if (inst.dest != srcReg)
				FMR(regs_.F(inst.dest + i), regs_.F(srcReg + i));
		}
		break;
	case IROp::Vec4Shuffle:
		if (inst.dest == inst.src1) {
			// In place: go through the scratch slots.
			regs_.Map(inst);
			for (int i = 0; i < 4; ++i)
				STFS(regs_.F(inst.src1 + i), R1, (s16)(VTEMP + i * 4));
			for (int i = 0; i < 4; ++i)
				LFS(regs_.F(inst.dest + i), R1, (s16)(VTEMP + ((inst.src2 >> (i * 2)) & 3) * 4));
		} else if (!Overlap(inst.dest, 4, inst.src1, 4)) {
			regs_.Map(inst);
			for (int i = 0; i < 4; ++i)
				FMR(regs_.F(inst.dest + i), regs_.F(inst.src1 + ((inst.src2 >> (i * 2)) & 3)));
		} else {
			DISABLE;
		}
		break;
	case IROp::Vec4Init: {
		// Same table as the interpreter.
		static const float values[7][4] = {
			{ 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { -1.0f, -1.0f, -1.0f, -1.0f },
			{ 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f },
		};
		if (inst.src1 >= 7)
			DISABLE;
		regs_.Map(inst);
		for (int i = 0; i < 4; ++i) {
			u32 bits;
			memcpy(&bits, &values[inst.src1][i], 4);
			LI32(SCRATCH1, bits);
			STW(SCRATCH1, R1, (s16)(VTEMP + i * 4));
		}
		for (int i = 0; i < 4; ++i)
			LFS(regs_.F(inst.dest + i), R1, (s16)(VTEMP + i * 4));
		break;
	}
	default:
		DISABLE;
	}
}

void PPCJitBackend::CompIR_VecArith(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_VecArith(inst);
		return;
	}
	switch (inst.op) {
	case IROp::Vec4Add:
	case IROp::Vec4Sub:
	case IROp::Vec4Mul:
	case IROp::Vec4Div:
		regs_.Map(inst);
		for (int i = 0; i < 4; ++i) {
			const PPCReg d = regs_.F(inst.dest + i), a = regs_.F(inst.src1 + i), b = regs_.F(inst.src2 + i);
			switch (inst.op) {
			case IROp::Vec4Add: FADDS(d, a, b); break;
			case IROp::Vec4Sub: FSUBS(d, a, b); break;
			case IROp::Vec4Mul: FMULS(d, a, b); break;
			default: FDIVS(d, a, b); break;
			}
		}
		break;
	case IROp::Vec4Scale:
		regs_.Map(inst);
		// dest may contain src2: do that lane last.
		for (int pass = 0; pass < 2; ++pass) {
			for (int i = 0; i < 4; ++i) {
				const bool isScaleLane = inst.src2 == inst.dest + i;
				if (isScaleLane == (pass == 1))
					FMULS(regs_.F(inst.dest + i), regs_.F(inst.src1 + i), regs_.F(inst.src2));
			}
		}
		break;
	case IROp::Vec4Neg:
		regs_.Map(inst);
		for (int i = 0; i < 4; ++i)
			FNEG(regs_.F(inst.dest + i), regs_.F(inst.src1 + i));
		break;
	case IROp::Vec4Abs:
		regs_.Map(inst);
		for (int i = 0; i < 4; ++i)
			FABS(regs_.F(inst.dest + i), regs_.F(inst.src1 + i));
		break;
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::CompIR_VecHoriz(IRInst inst) {
	if (regs_.UsesVMX()) {
		VMX_VecHoriz(inst);
		return;
	}
	if (inst.op != IROp::Vec4Dot)
		DISABLE;
	regs_.Map(inst);
	// Same evaluation order as the interpreter: ((a0*b0 + a1*b1) + a2*b2) + a3*b3.
	FMULS(FSCRATCH1, regs_.F(inst.src1), regs_.F(inst.src2));
	for (int i = 1; i < 4; ++i) {
		FMULS(FSCRATCH2, regs_.F(inst.src1 + i), regs_.F(inst.src2 + i));
		FADDS(FSCRATCH1, FSCRATCH1, FSCRATCH2);
	}
	FMR(regs_.F(inst.dest), FSCRATCH1);
}


// ---------------------------------------------------------------------------------------------
// AltiVec mode (g_ppcJitVMX). Every FPU/VFPU register lives in a VMX register: a scalar in lane 0,
// a Vec4 group in all four lanes. Values move to and from PSP memory with lvx/lvewx/stvx/stvewx
// plus a byte swap (vperm), so nothing passes through a GPR or FPR and back through memory (a
// load-hit-store stall on the PPU). Ops VMX can't do exactly (divide, square root, conversions
// with rounding modes, min/max with the interpreter's NaN rules, the special functions) copy their
// inputs to FPRs and back through the stack slot, as the FPR mode does for GPR transfers.
// v0-v2 are scratch for the ops here, v3 for the regcache.

struct alignas(16) VmxConstants {
	u8 swapWords[16];   // vperm pattern: byte-reverse each word.
	u8 lane0Swap[16];   // vperm pattern: lane 0 byte-reversed, in every lane.
	u32 blend[16][4];   // vsel masks: lane i from the second source when bit i is set.
	u8 shuffle[256][16];
	u32 laneMask[4][4];
	float init[7][4];   // Vec4Init values (as the interpreter).
	u8 unpack8[16];     // vperm pattern: byte i of lane 0's word (little-endian) to the top of lane i, zeros below.
	u32 denormLimit[4]; // 0x01000000: float bits shifted left one (no sign) below this are zero or denormal.
	u32 fltMin[4];      // 0x00800000: the smallest normal float.
};

static const VmxConstants &Vmx() {
	static VmxConstants c;
	static bool ready = false;
	if (!ready) {
		for (int j = 0; j < 16; ++j) {
			c.swapWords[j] = (u8)(j ^ 3);
			c.lane0Swap[j] = (u8)(3 - (j & 3));
		}
		for (int m = 0; m < 16; ++m)
			for (int i = 0; i < 4; ++i)
				c.blend[m][i] = (m >> i) & 1 ? 0xFFFFFFFF : 0;
		for (int sh = 0; sh < 256; ++sh)
			for (int i = 0; i < 4; ++i)
				for (int b = 0; b < 4; ++b)
					c.shuffle[sh][i * 4 + b] = (u8)(((sh >> (i * 2)) & 3) * 4 + b);
		for (int l = 0; l < 4; ++l)
			for (int i = 0; i < 4; ++i)
				c.laneMask[l][i] = l == i ? 0xFFFFFFFF : 0;
		static const float values[7][4] = {
			{ 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f }, { -1.0f, -1.0f, -1.0f, -1.0f },
			{ 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f },
		};
		memcpy(c.init, values, sizeof(values));
		for (int i = 0; i < 4; ++i) {
			c.unpack8[i * 4] = (u8)(3 - i);
			c.unpack8[i * 4 + 1] = c.unpack8[i * 4 + 2] = c.unpack8[i * 4 + 3] = 16;
		}
		for (int i = 0; i < 4; ++i) {
			c.denormLimit[i] = 0x01000000;
			c.fltMin[i] = 0x00800000;
		}
		ready = true;
	}
	return c;
}

// swapWords then lane0Swap, for the JIT entry to load into VSWAPWORDS/VLANE0SWAP.
const void *PPCJitVmxSwapConstants() {
	return Vmx().swapWords;
}

void PPCJitBackend::VConst(PPCReg vt, const void *ptr) {
	LIPTR(R12, (uintptr_t)ptr);
	LVX(vt, R0, R12);
}

// -0.0f in every lane: the addend that makes vmaddfp a plain multiply (keeps the product's sign).
void PPCJitBackend::VNegZero(PPCReg vt) {
	VSPLTISW(vt, -1);
	VSLW(vt, vt, vt);
}

void PPCJitBackend::VRToFPR(PPCReg f, PPCReg v, int lane) {
	VSPLTW(V2, v, lane);
	LI(R0, VTEMP);
	STVEWX(V2, R1, R0);
	LFS(f, R1, VTEMP);
}

void PPCJitBackend::FPRToVR(PPCReg v, PPCReg f) {
	STFS(f, R1, VTEMP);
	LI(R0, VTEMP);
	LVEWX(v, R1, R0);  // VTEMP is 16-aligned: lane 0.
}

void PPCJitBackend::GPRToVR(PPCReg v, PPCReg r) {
	STW(r, R1, VTEMP);
	LI(R0, VTEMP);
	LVEWX(v, R1, R0);
}

void PPCJitBackend::VRToGPR(PPCReg r, PPCReg v, int lane) {
	VSPLTW(V2, v, lane);
	LI(R0, VTEMP);
	STVEWX(V2, R1, R0);
	LWZ(r, R1, VTEMP);
}

// VMX always rounds to nearest. Once the game has changed the FPU rounding mode, FPU (COP1) ops
// that round go through the FPU, which follows FPSCR (the VFPU always rounds to nearest).
// Set while compiling a block that checks the rounding mode once at its entry (see CompileBlock):
// then the ops in it use VMX directly.
bool g_ppcBlockRMChecked = false;

static bool ExactFPU(IRReg dest) {
	return g_irRoundingModeUsed && dest < 32 && !g_ppcBlockRMChecked;
}

// FPU ops that follow the guest rounding mode (the ones ExactFPU handles).
bool PPCJitRoundingOp(const IRInst &inst) {
	switch (inst.op) {
	case IROp::FAdd: case IROp::FSub: case IROp::FMul: case IROp::FCvtSW: case IROp::FCvtScaledSW:
		return inst.dest < 32;
	default:
		return false;
	}
}

// Branch taken when the guest FPU rounding mode (fcr31 bits 0-1) isn't round-to-nearest. Most games
// write fcr31 once (which makes ExactFPU true for the rest of the run) but keep rounding to
// nearest; then the VMX op is exact. The FPU path costs four load-hit-store stalls (~160 cycles)
// moving the values between AltiVec and FPU registers through memory.
FixupBranch PPCJitBackend::BranchIfNotNearest() {
	LWZ(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, fcr31));
	ANDI_(SCRATCH1, SCRATCH1, 3);
	return BC(Cond::NE);
}

void PPCJitBackend::VMX_FArith(IRInst inst) {
	if ((inst.op == IROp::FAdd || inst.op == IROp::FSub || inst.op == IROp::FMul) && ExactFPU(inst.dest)) {
		regs_.Map(inst);
		const PPCReg d = regs_.V(inst.dest), a = regs_.V(inst.src1), b = regs_.V(inst.src2);
		FixupBranch slow = BranchIfNotNearest();
		switch (inst.op) {
		case IROp::FAdd: VADDFP(V1, a, b); break;
		case IROp::FSub: VSUBFP(V1, a, b); break;
		default: VMADDFP(V1, a, b, VNEGZERO); break;
		}
		const u8 *done = GetCodePointer();
		VMR(d, V1);
		const IROp op = inst.op;
		AddFarCode(slow, [this, a, b, op, done]() {
			VRToFPR(F1, a);
			VRToFPR(F2, b);
			switch (op) {
			case IROp::FAdd: FADDS(F1, F1, F2); break;
			case IROp::FSub: FSUBS(F1, F1, F2); break;
			default: FMULS(F1, F1, F2); break;
			}
			FPRToVR(V1, F1);
			B(done);
		});
		return;
	}
	switch (inst.op) {
	case IROp::FAdd:
		regs_.Map(inst);
		VADDFP(regs_.V(inst.dest), regs_.V(inst.src1), regs_.V(inst.src2));
		break;
	case IROp::FSub:
		regs_.Map(inst);
		VSUBFP(regs_.V(inst.dest), regs_.V(inst.src1), regs_.V(inst.src2));
		break;
	case IROp::FMul:
		regs_.Map(inst);
		VMADDFP(regs_.V(inst.dest), regs_.V(inst.src1), regs_.V(inst.src2), VNEGZERO);
		break;
	case IROp::FNeg:
		regs_.Map(inst);
		VXOR(regs_.V(inst.dest), regs_.V(inst.src1), VNEGZERO);
		break;
	case IROp::FDiv:
		regs_.Map(inst);
		VRToFPR(F1, regs_.V(inst.src1));
		VRToFPR(F2, regs_.V(inst.src2));
		FDIVS(F1, F1, F2);
		FPRToVR(regs_.V(inst.dest), F1);
		break;
	case IROp::FSqrt:
		regs_.Map(inst);
		VRToFPR(F1, regs_.V(inst.src1));
		FSQRTS(F1, F1);
		FPRToVR(regs_.V(inst.dest), F1);
		break;
	default:
		DISABLE;
	}
}

void PPCJitBackend::VMX_FAssign(IRInst inst) {
	switch (inst.op) {
	case IROp::FMov:
		if (inst.dest != inst.src1) {
			regs_.Map(inst);
			VMR(regs_.V(inst.dest), regs_.V(inst.src1));
		}
		break;
	case IROp::FAbs:
		regs_.Map(inst);
		VANDC(regs_.V(inst.dest), regs_.V(inst.src1), VNEGZERO);
		break;
	case IROp::FSign:
		// +1 / -1 by the sign bit; 0 for zero and denormals (as the interpreter).
		regs_.Map(inst);
		VRToGPR(SCRATCH1, regs_.V(inst.src1));
		RLWINM(SCRATCH2, SCRATCH1, 0, 0, 0);
		ORIS(SCRATCH2, SCRATCH2, 0x3F80);
		ANDIS_(SCRATCH3, SCRATCH1, 0x7F80);
		{
			FixupBranch nonZero = BC(Cond::NE);
			LI(SCRATCH2, 0);
			SetJumpTarget(nonZero);
		}
		GPRToVR(regs_.V(inst.dest), SCRATCH2);
		break;
	default:
		DISABLE;
	}
}

void PPCJitBackend::VMX_FLoad(IRInst inst) {
	if (inst.op != IROp::LoadFloat)
		DISABLE;
	regs_.Map(inst);
	AddressToScratch2(inst);
	// lvewx puts the word in lane (ea & 15) / 4; lvsl gives sh .. sh + 15 (sh = ea & 15), and
	// permuting that by 3,2,1,0 makes a pattern that brings it, byte-reversed, to lane 0.
	LVEWX(V1, MEMBASEREG, SCRATCH2);
	LVSL(V0, MEMBASEREG, SCRATCH2);
	VPERM(V0, V0, V0, VLANE0SWAP);
	VPERM(regs_.V(inst.dest), V1, V1, V0);
}

void PPCJitBackend::VMX_FStore(IRInst inst) {
	if (inst.op != IROp::StoreFloat)
		DISABLE;
	regs_.Map(inst);
	AddressToScratch2(inst);
	// Lane 0 byte-reversed in every lane; stvewx stores the lane the address selects.
	VPERM(V0, regs_.V(inst.src3), regs_.V(inst.src3), VLANE0SWAP);
	STVEWX(V0, MEMBASEREG, SCRATCH2);
}

void PPCJitBackend::VMX_VecLoad(IRInst inst) {
	if (inst.op != IROp::LoadVec4)
		DISABLE;
	regs_.Map(inst);
	const PPCReg d = regs_.V(inst.dest);
	AddressToScratch2(inst);
	ADDI(SCRATCH3, SCRATCH2, 15);
	// Two aligned loads cover any (word-aligned) address; the permute picks the 16 bytes from
	// ea and reverses each word.
	LVX(V1, MEMBASEREG, SCRATCH2);
	LVX(V2, MEMBASEREG, SCRATCH3);
	LVSL(V0, MEMBASEREG, SCRATCH2);
	VPERM(V0, V0, V0, VSWAPWORDS);
	VPERM(d, V1, V2, V0);
}

void PPCJitBackend::VMX_VecStore(IRInst inst) {
	if (inst.op != IROp::StoreVec4)
		DISABLE;
	regs_.Map(inst);
	AddressToScratch2(inst);
	ADD(SCRATCH1, MEMBASEREG, SCRATCH2);
	VPERM(V0, regs_.V(inst.src3), regs_.V(inst.src3), VSWAPWORDS);
	ANDI_(SCRATCH3, SCRATCH1, 15);
	FixupBranch unaligned = BC(Cond::NE);
	STVX(V0, R0, SCRATCH1);
	FixupBranch done = B();
	SetJumpTarget(unaligned);
	// Not 16-aligned: through the stack, a word at a time (already byte-swapped).
	LI(R0, VTEMP);
	STVX(V0, R1, R0);
	for (int i = 0; i < 4; ++i) {
		LWZ(SCRATCH2, R1, (s16)(VTEMP + i * 4));
		STW(SCRATCH2, SCRATCH1, (s16)(i * 4));
	}
	SetJumpTarget(done);
}

// lv.q / sv.q of a whole matrix by rows (R000..R003: the IR's 16 LoadFloat/StoreFloat of rows
// m+r, m+4+r, m+8+r, m+12+r, since a row's lanes are four different column groups): four
// 16-byte loads/stores and a 4x4 transpose in registers, instead of 16 single-float accesses
// (7 instructions each, and 16 AltiVec registers for the scalars). Returns the IR instructions
// it handled (0: not this pattern).
// vdot.t of two aligned 3-element vectors comes out of the IR as FMul, FMul, FAdd, FMul, FAdd on
// single lanes: each matrix element loaded on its own and a chain of dependent adds, which stalls
// the in-order PPU at every step. Silent Hill Origins' culling routine does ten in a row against
// the same vector (~75,000 times a frame). Here: one multiply of the whole vectors, then the same
// two adds in the same order ((p0 + p1) + p2), on lanes shifted down - the same results. Two dots
// in a row (as the game has them) are interleaved so neither waits on its own adds.
// Returns the number of IR instructions done (0: not this pattern).
static bool MatchDot3(const IRInst *insts, int i, int n, IRReg *a, IRReg *b, IRReg *d, IRReg *t0, IRReg *t1) {
	if (i + 5 > n)
		return false;
	const IRInst &m0 = insts[i], &m1 = insts[i + 1], &s0 = insts[i + 2], &m2 = insts[i + 3], &s1 = insts[i + 4];
	if (m0.op != IROp::FMul || m1.op != IROp::FMul || s0.op != IROp::FAdd || m2.op != IROp::FMul || s1.op != IROp::FAdd)
		return false;
	const IRReg A = m0.src1, B = m0.src2, T0 = m0.dest, T1 = m1.dest;
	if (A < 32 || B < 32 || (A & 3) != 0 || (B & 3) != 0 || A + 4 > 160 || B + 4 > 160)
		return false;
	if (m1.src1 != A + 1 || m1.src2 != B + 1 || m2.src1 != A + 2 || m2.src2 != B + 2)
		return false;
	if (s0.dest != T0 || s0.src1 != T0 || s0.src2 != T1 || m2.dest != T1 || s1.src1 != T0 || s1.src2 != T1)
		return false;
	const IRReg D = s1.dest;
	if (T0 == T1 || T0 < 32 || T1 < 32 || D < 32)
		return false;
	auto inGroup = [](IRReg r, IRReg g) { return r >= g && r < g + 4; };
	if (inGroup(T0, A) || inGroup(T0, B) || inGroup(T1, A) || inGroup(T1, B) || inGroup(D, A) || inGroup(D, B) || D == T1)
		return false;
	*a = A; *b = B; *d = D; *t0 = T0; *t1 = T1;
	return true;
}

// Whether reg is written before anything reads it after index i (or the block ends with it unread
// - IR temps aren't kept past a block).
static bool DeadAfter(const IRInst *insts, int i, int n, IRReg reg) {
	auto lanesOf = [](char t) { return t == 'F' ? 1 : (t == '2' ? 2 : (t == 'V' ? 4 : 0)); };
	for (int j = i; j < n; ++j) {
		const IRInst &x = insts[j];
		const IRMeta *m = GetIRMeta(x.op);
		if (!m)
			return false;
		const int l1 = lanesOf(m->types[1]), l2 = lanesOf(m->types[2]), ld = lanesOf(m->types[0]);
		if ((l1 && reg >= x.src1 && reg < x.src1 + l1) || (l2 && reg >= x.src2 && reg < x.src2 + l2))
			return false;
		if (m->flags & IRFLAG_SRC3) {
			// Stores: types[0] is the stored value (src3), nothing is written.
			if (ld && reg >= x.src3 && reg < x.src3 + ld)
				return false;
			if (!ld && m->types[0] != 'G' && m->types[0] != 'C')
				return false;
			continue;
		}
		if (ld == 1 && x.dest == reg)
			return true;
		if (m->flags & IRFLAG_BARRIER)
			return reg >= IRVTEMP_PFX_S;  // Exits keep the real registers, not temps.
	}
	return reg >= IRVTEMP_PFX_S;
}

// Whether GPR reg is written before anything reads it after index i (IR temps die at the block's
// end and at exits; other registers are kept).
static bool GprDeadAfter(const IRInst *insts, int i, int n, IRReg reg) {
	const bool temp = reg >= IRTEMP_0 && reg <= IRTEMP_LR_SHIFT;
	for (int j = i; j < n; ++j) {
		const IRInstMeta m = GetIRMeta(insts[j]);
		bool directly = false;
		if (IRReadsFromGPR(m, reg, &directly)) {
			if (directly || !temp)
				return false;
			return true;  // An exit or barrier: temps aren't kept.
		}
		if (IRWritesToGPR(m, reg))
			return true;
	}
	return temp;
}

// Whether fpcond is set again before anything reads it after index i.
static bool FpCondDeadAfter(const IRInst *insts, int i, int n) {
	for (int j = i; j < n; ++j) {
		const IRInst &x = insts[j];
		switch (x.op) {
		case IROp::FpCondToReg:
		case IROp::FpCtrlToReg:
			return false;
		case IROp::FCmp:
		case IROp::FpCondFromReg:
		case IROp::FpCtrlFromReg:
			return true;
		default:
			break;
		}
		const IRMeta *m = GetIRMeta(x.op);
		if (!m || (m->flags & (IRFLAG_EXIT | IRFLAG_BARRIER)))
			return false;
	}
	return false;
}

// AltiVec mode: FCmp, FpCondToReg t, [XorConst t, t, 1,] FMovFromGPR f, t - how vslt/vsge/vcmp-like
// VFPU code comes out of the IR, one lane at a time - with t and fpcond both set again before
// anything reads them (vslt saves fpcond first and puts it back after). The 0/1 goes from the
// compare mask straight into f; the dead fpcond and t (a vector compare, a CR6 read and two
// integer ops, a chain of stalls on the in-order PPU) are left out. Frontend 94.
int s_cmpToFloatFused = 0;
int PPCJitBackend::VMX_CompareToFloat(const IRInst *insts, int i, int n) {
	const IRInst &cmp = insts[i];
	if (cmp.dest == IRFpCompareMode::False || i + 2 >= n || insts[i + 1].op != IROp::FpCondToReg)
		return 0;
	const IRReg t = insts[i + 1].dest;
	if (t == MIPS_REG_ZERO)
		return 0;
	int k = i + 2;
	bool inverted = false;
	if (insts[k].op == IROp::XorConst && insts[k].dest == t && insts[k].src1 == t && insts[k].constant == 1) {
		inverted = true;
		k++;
	}
	if (k >= n || insts[k].op != IROp::FMovFromGPR || insts[k].src1 != t)
		return 0;
	const IRInst &mov = insts[k];
	if (!GprDeadAfter(insts, k + 1, n, t) || !FpCondDeadAfter(insts, k + 1, n))
		return 0;
	regs_.Map(cmp);
	VCmpMask(cmp.dest, regs_.V(cmp.src1), regs_.V(cmp.src2));
	// The regcache uses v3, not v0-v2: the mask in v2 survives mapping the destination.
	const PPCReg d = regs_.MapVR(mov.dest, 1, MIPSMap::NOINIT);
	VSPLTISW(V0, 1);
	if (inverted)
		VANDC(d, V0, V2);
	else
		VAND(d, V2, V0);
	regs_.MarkVRDirty(mov.dest);
	s_cmpToFloatFused++;
#if !PPSSPP_PLATFORM(PS3)
	if (getenv("PPC_FUSELOG"))
		fprintf(stderr, "PPC fused compare to float (mode %d, %s)\n", (int)cmp.dest, inverted ? "inverted" : "plain");
#endif
	return k - i + 1;
}

int s_dot3Fused = 0;
int PPCJitBackend::VMX_Dot3(const IRInst *insts, int i, int n) {
	IRReg a1, b1, d1, ta, tb;
	if (!MatchDot3(insts, i, n, &a1, &b1, &d1, &ta, &tb))
		return 0;
	IRReg a2 = 0, b2 = 0, d2 = 0, tc = 0, td = 0;
	const bool pair = MatchDot3(insts, i + 5, n, &a2, &b2, &d2, &tc, &td) && a2 == a1 && tc == ta && td == tb &&
		d2 != d1 && !(d1 >= b2 && d1 < b2 + 4) && !(d1 >= a2 && d1 < a2 + 4);
	const int count = pair ? 10 : 5;
	if (!DeadAfter(insts, i + count, n, ta) || !DeadAfter(insts, i + count, n, tb))
		return 0;
	regs_.SetIRIndex(i + count - 1);
	auto inGroupOf = [](IRReg r, IRReg g) { return r >= g && r < g + 4; };
	if (pair && (d1 & 3) == 0 && d2 == d1 + 1 && !inGroupOf(a1, d1) && !inGroupOf(b1, d1) && !inGroupOf(b2, d1) &&
		!inGroupOf(ta, d1) && !inGroupOf(tb, d1)) {
		// Frontend 103: two dots into lanes 0 and 1 of a group (vdot.t pairs feeding a vector op
		// of that group): the results go into the group's register, the other lanes kept,
		// instead of each into a register of its own to be merged later.
		regs_.SpillLockFPR(a1, b1, b2, d1);
		const PPCReg A = regs_.MapVR(a1, 4, MIPSMap::INIT);
		const PPCReg B1 = regs_.MapVR(b1, 4, MIPSMap::INIT);
		const PPCReg B2 = regs_.MapVR(b2, 4, MIPSMap::INIT);
		const PPCReg D = regs_.MapVR(d1, 4, MIPSMap::INIT);
		VMADDFP(V0, A, B1, VNEGZERO);
		VMADDFP(V1, A, B2, VNEGZERO);
		VMRGHW(V2, V0, V1);        // x1 x2 y1 y2
		VMRGLW(V3, V0, V1);        // z1 z2 w1 w2
		VSLDOI(V0, V2, V2, 8);     // y1 y2
		VADDFP(V2, V2, V0);
		VADDFP(V2, V2, V3);
		VSPLTISW(V0, -1);
		VSPLTISW(V1, 0);
		VSLDOI(V0, V0, V1, 8);     // lanes 0-1
		VSEL(D, D, V2, V0);
		regs_.MarkVRDirty(d1);
		regs_.ReleaseSpillLockFPR(a1, b1, b2, d1);
		s_dot3Fused += 2;
#if !PPSSPP_PLATFORM(PS3)
		if (getenv("PPC_FUSELOG"))
			fprintf(stderr, "PPC fused dot3 x2 into group (a=%d b=%d d=%d)\n", a1, b1, d1);
#endif
		return count;
	}
	if (pair)
		regs_.SpillLockFPR(a1, b1, b2);
	else
		regs_.SpillLockFPR(a1, b1);
	const PPCReg A = regs_.MapVR(a1, 4, MIPSMap::INIT);
	const PPCReg B1 = regs_.MapVR(b1, 4, MIPSMap::INIT);
	const PPCReg B2 = pair ? regs_.MapVR(b2, 4, MIPSMap::INIT) : INVALID_REG;
	regs_.SpillLockFPR(d1);
	if (pair)
		regs_.SpillLockFPR(d2);
	const PPCReg D1 = regs_.MapVR(d1, 1, MIPSMap::NOINIT);
	const PPCReg D2 = pair ? regs_.MapVR(d2, 1, MIPSMap::NOINIT) : INVALID_REG;
	VMADDFP(D1, A, B1, VNEGZERO);
	if (pair)
		VMADDFP(D2, A, B2, VNEGZERO);
	VSLDOI(V0, D1, D1, 4);
	if (pair)
		VSLDOI(V1, D2, D2, 4);
	VADDFP(V0, D1, V0);
	if (pair)
		VADDFP(V1, D2, V1);
	VSLDOI(V2, D1, D1, 8);
	VADDFP(D1, V0, V2);
	regs_.MarkVRDirty(d1);
	if (pair) {
		VSLDOI(V2, D2, D2, 8);
		VADDFP(D2, V1, V2);
		regs_.MarkVRDirty(d2);
		regs_.ReleaseSpillLockFPR(a1, b1, b2, d1);
		regs_.ReleaseSpillLockFPR(d2);
	} else {
		regs_.ReleaseSpillLockFPR(a1, b1, d1);
	}
	s_dot3Fused += pair ? 2 : 1;
#if !PPSSPP_PLATFORM(PS3)
	if (getenv("PPC_FUSELOG"))
		fprintf(stderr, "PPC fused dot3 x%d (a=%d b=%d d=%d)\n", pair ? 2 : 1, a1, b1, d1);
#endif
	return count;
}

// AltiVec mode: vfad.q (and the sum of a vmul) comes out of the IR as SetConstF t, 0 then four
// FAdds of a group's lanes in order. With the group in a vector register, each lane read wrote the
// group to memory and loaded the lane back (a load-hit-store stall each, Silent Hill Origins'
// busiest routine has three such sums). Sum the lanes in registers instead, in the same order
// (0 + l0, + l1, + l2, + l3: the same results). Frontend 97.
int s_hsumFused = 0;
// SetConstF t, 0; FAdd t, t, x0; FAdd t, t, x1; FAdd t, t, x2; FAdd d, t, x3 at i.
static bool MatchHSum(const IRInst *insts, int i, int n, IRReg x[4], IRReg *t, IRReg *d) {
	if (i + 5 > n || insts[i].op != IROp::SetConstF || insts[i].constant != 0)
		return false;
	const IRReg T = insts[i].dest;
	for (int k = 0; k < 4; ++k) {
		const IRInst &a = insts[i + 1 + k];
		if (a.op != IROp::FAdd || a.src1 != T || a.src2 == T)
			return false;
		if (k < 3 && a.dest != T)
			return false;
		x[k] = a.src2;
	}
	*t = T;
	*d = insts[i + 4].dest;
	return true;
}

// Frontend 103: the four lanes may be anywhere in vector registers (a group's register, or
// scalars), not only one group's register; and two sums in a row that don't depend on each
// other are interleaved (each is a chain of four dependent adds).
int PPCJitBackend::VMX_HSum4(const IRInst *insts, int i, int n) {
	IRReg xa[4], xb[4], ta, tb, da, db;
	if (!MatchHSum(insts, i, n, xa, &ta, &da))
		return 0;
	auto allInVR = [&](const IRReg x[4]) {
		for (int k = 0; k < 4; ++k)
			if (!regs_.InVR(x[k]))
				return false;
		return true;
	};
	if (!allInVR(xa))
		return 0;
	bool two = MatchHSum(insts, i + 5, n, xb, &tb, &db) && allInVR(xb) && tb == ta;
	if (two) {
		for (int k = 0; k < 4; ++k)
			if (xb[k] == da || xb[k] == ta)
				two = false;
	}
	const int count = two ? 10 : 5;
	if (!two && da != ta && !DeadAfter(insts, i + 5, n, ta))
		return 0;
	if (two && db != tb && !DeadAfter(insts, i + 10, n, tb))
		return 0;
	if (two && da == ta)
		return 0;  // The second sum would overwrite the first's result register (it's t).
	// Where each lane is now, read before anything moves (no regcache calls until the sums are done).
	PPCReg ra[4], rb[4];
	int la[4], lb[4];
	for (int k = 0; k < 4; ++k) {
		ra[k] = regs_.V(xa[k]);
		la[k] = regs_.VRLane(xa[k]);
		if (two) {
			rb[k] = regs_.V(xb[k]);
			lb[k] = regs_.VRLane(xb[k]);
		}
	}
	auto lane0 = [&](PPCReg r, int lane, PPCReg tmp) -> PPCReg {
		if (lane == 0)
			return r;
		VSLDOI(tmp, r, r, lane * 4);
		return tmp;
	};
	VSPLTISW(V0, 0);
	if (two)
		VSPLTISW(V1, 0);
	for (int k = 0; k < 4; ++k) {
		VADDFP(V0, V0, lane0(ra[k], la[k], V2));
		if (two)
			VADDFP(V1, V1, lane0(rb[k], lb[k], V3));
	}
	regs_.SetIRIndex(i + count - 1);
	// A result that is a lane of a group held in one register goes into that lane (mapping it
	// alone would write the group out and split it, and the next sum would read lanes from
	// memory). Otherwise it gets a register of its own; the regcache doesn't touch v0/v1 when
	// mapping one lane.
	auto put = [&](IRReg d, PPCReg sum) {
		const IRReg group = d & ~3;
		if (d >= 32 && regs_.InVR(d) && regs_.VRLanes(group) == 4 && regs_.V(group) == regs_.V(d) && regs_.VRLane(group) == 0) {
			const PPCReg G = regs_.V(d);
			const int k = regs_.VRLane(d);
			VSPLTISW(V2, -1);
			VSPLTISW(V3, 0);
			VSLDOI(V2, V3, V2, 4);                 // lane 3 only
			if (k != 3)
				VSLDOI(V2, V2, V2, 4 * (3 - k));   // lane k only
			VSPLTW(V3, sum, 0);
			VSEL(G, G, V3, V2);
			regs_.MarkVRDirty(group);
			return;
		}
		const PPCReg D = regs_.MapVR(d, 1, MIPSMap::NOINIT);
		VOR(D, sum, sum);
		regs_.MarkVRDirty(d);
	};
	put(da, V0);
	if (two) {
		regs_.SpillLockFPR(da);
		put(db, V1);
		regs_.ReleaseSpillLockFPR(da);
	}
	s_hsumFused += two ? 2 : 1;
#if !PPSSPP_PLATFORM(PS3)
	if (getenv("PPC_FUSELOG"))
		fprintf(stderr, "PPC fused hsum4 x%d (d=%d%s)\n", two ? 2 : 1, da, two ? " and one more" : "");
#endif
	return count;
}

// ---------------------------------------------------------------------------------------------
// Frontend 102: VFPU code the IR does one lane at a time, done four lanes at a time again.
// vdot.t into the four lanes of a group, a vector op of a group with a matrix row (a lane taken
// from four groups), and vslt-like compares of a whole group: each lane was its own chain of
// dependent vector instructions, a stall after each on the in-order PPU. Same arithmetic in the
// same order per lane, so the same results. Silent Hill Origins' light routine (the biggest cost
// in its scenes with many lights) is built of exactly these.

// Four vdot.t (MatchDot3) of one vector A with the four groups B0..B3 into the lanes of group D.
bool PPCJitBackend::MatchDotQuad(const IRInst *insts, int i, int n, IRReg *a, IRReg b[4], IRReg *d, IRReg *t0, IRReg *t1) const {
	IRReg A = 0, D0 = 0, T0 = 0, T1 = 0;
	for (int k = 0; k < 4; ++k) {
		IRReg ak, bk, dk, tk0, tk1;
		if (!MatchDot3(insts, i + 5 * k, n, &ak, &bk, &dk, &tk0, &tk1))
			return false;
		if (k == 0) {
			A = ak; D0 = dk; T0 = tk0; T1 = tk1;
			if ((D0 & 3) != 0)
				return false;
		} else if (ak != A || dk != D0 + k || tk0 != T0 || tk1 != T1) {
			return false;
		}
		b[k] = bk;
	}
	auto inGroup = [](IRReg r, IRReg g) { return r >= g && r < g + 4; };
	if (inGroup(A, D0) || inGroup(T0, D0) || inGroup(T1, D0))
		return false;
	for (int k = 0; k < 4; ++k)
		if (b[k] == D0)
			return false;
	*a = A; *d = D0; *t0 = T0; *t1 = T1;
	return true;
}

int s_dotQuadFused = 0;
int PPCJitBackend::VMX_DotQuad(const IRInst *insts, int i, int n) {
	IRReg a, b[4], d, t0, t1;
	if (!MatchDotQuad(insts, i, n, &a, b, &d, &t0, &t1))
		return 0;
	if (!DeadAfter(insts, i + 20, n, t0) || !DeadAfter(insts, i + 20, n, t1))
		return 0;
	regs_.SetIRIndex(i + 19);
	regs_.SpillLockFPR(a, b[0], b[1], b[2]);
	regs_.SpillLockFPR(b[3], d);
	const PPCReg A = regs_.MapVR(a, 4, MIPSMap::INIT);
	PPCReg B[4];
	for (int k = 0; k < 4; ++k)
		B[k] = regs_.MapVR(b[k], 4, MIPSMap::INIT);
	const PPCReg D = regs_.MapVR(d, 4, MIPSMap::NOINIT);
	// No regcache calls from here on: v0-v3 are ours.
	// Products P0..P3 (exact, as FMul), transposed into the x, y, z lanes of the four dots, then
	// (x + y) + z per lane as the IR adds them.
	VMADDFP(V0, A, B[0], VNEGZERO);
	VMADDFP(V1, A, B[2], VNEGZERO);
	VMADDFP(V2, A, B[1], VNEGZERO);
	VMADDFP(D, A, B[3], VNEGZERO);
	VMRGLW(V3, V0, V1);       // P0z P2z P0w P2w
	VMRGHW(V0, V0, V1);       // P0x P2x P0y P2y
	VMRGLW(V1, V2, D);        // P1z P3z P1w P3w
	VMRGHW(V2, V2, D);        // P1x P3x P1y P3y
	VMRGHW(D, V0, V2);        // x
	VMRGLW(V0, V0, V2);       // y
	VADDFP(D, D, V0);
	VMRGHW(V0, V3, V1);       // z
	VADDFP(D, D, V0);
	regs_.MarkVRDirty(d);
	regs_.ReleaseSpillLockFPR(a, b[0], b[1], b[2]);
	regs_.ReleaseSpillLockFPR(b[3], d);
	s_dotQuadFused++;
#if !PPSSPP_PLATFORM(PS3)
	if (getenv("PPC_FUSELOG"))
		fprintf(stderr, "PPC fused dot quad (a=%d b=%d,%d,%d,%d d=%d)\n", a, b[0], b[1], b[2], b[3], d);
#endif
	return 20;
}

// Four FAdd/FSub/FMul of the lanes of group S with the lanes of group R (R + k), or with a matrix
// row: lane k of the groups G, G+4, G+8, G+12 (src2 = G + k + 4i), into the lanes of group D.
int s_rowOpFused = 0;
// Frontend 103: also three lanes (vsub.t and the like), the fourth lane of D kept.
int PPCJitBackend::VMX_LaneOp4(const IRInst *insts, int i, int n) {
	if (i + 3 > n)
		return 0;
	const IRInst &first = insts[i];
	const IROp op = first.op;
	const bool minMax = op == IROp::FMin || op == IROp::FMax;
	if (op != IROp::FAdd && op != IROp::FSub && op != IROp::FMul && !minMax)
		return 0;
	const IRReg D = first.dest, S = first.src1, R0 = first.src2;
	if ((D & 3) != 0 || (S & 3) != 0 || D < 32 || S < 32 || R0 < 32)
		return 0;
	// Row form: src2 steps by 4; contiguous form: src2 is an aligned group, steps by 1.
	const bool contiguous = (R0 & 3) == 0 && insts[i + 1].src2 == R0 + 1;
	// Frontend 105: FMin/FMax (vmin/vmax, Killzone), group form only (the min/max sequence
	// needs all of v0-v3).
	if (minMax && !contiguous)
		return 0;
	const int step = contiguous ? 1 : 4;
	int lanes = 0;
	while (lanes < 4 && i + lanes < n) {
		const IRInst &x = insts[i + lanes];
		if (x.op != op || x.dest != D + lanes || x.src1 != S + lanes || x.src2 != R0 + step * lanes)
			break;
		lanes++;
	}
	if (lanes < 3)
		return 0;
	const IRReg G = R0 & ~3;  // Row form: the first of the groups.
	const int lane = R0 & 3;
	// Lane by lane, a later lane must not read what an earlier lane wrote (all lanes read before
	// any is written here). Lane j reading its own lane of D (in place) is fine.
	for (int j = 1; j < lanes; ++j) {
		for (int k = 0; k < j; ++k) {
			if (S + j == D + k || R0 + step * j == D + k)
				return 0;
		}
	}
	// The groups read must not partly overlap D, except being D itself.
	auto overlaps = [](IRReg a, IRReg b) { return a != b && a < b + 4 && a + 4 > b; };
	if (overlaps(S, D) || (contiguous && overlaps(R0, D)) || (contiguous && overlaps(R0, S)))
		return 0;
	if (!contiguous) {
		for (int k = 0; k < lanes; ++k)
			if ((G + 4 * k) == D || overlaps(G + 4 * k, D))
				return 0;
	}
	regs_.SetIRIndex(i + lanes - 1);
	IRReg groups[4];
	int numGroups = 0;
	if (contiguous) {
		groups[numGroups++] = R0;
	} else {
		for (int k = 0; k < lanes; ++k)
			groups[numGroups++] = G + 4 * k;
	}
	regs_.SpillLockFPR(S, D);
	for (int k = 0; k < numGroups; ++k)
		regs_.SpillLockFPR(groups[k]);
	const PPCReg SR = regs_.MapVR(S, 4, MIPSMap::INIT);
	PPCReg GR[4];
	for (int k = 0; k < numGroups; ++k)
		GR[k] = regs_.MapVR(groups[k], 4, MIPSMap::INIT);
	if (numGroups == 3)
		GR[3] = GR[2];  // The fourth lane isn't used.
	// Three lanes: the fourth lane of D is kept, so D is loaded.
	PPCReg DR;
	if (S == D)
		DR = SR;
	else if (contiguous && R0 == D)
		DR = GR[0];
	else
		DR = regs_.MapVR(D, 4, lanes == 3 ? MIPSMap::INIT : MIPSMap::NOINIT);
	// No regcache calls from here on: v0-v3 are ours.
	PPCReg src2 = GR[0];
	if (!contiguous) {
		// [g0 g1 g2 g3] lane `lane` of each: merge pairs (0,2) and (1,3), then the halves.
		if (lane < 2) {
			VMRGHW(V0, GR[0], GR[2]);
			VMRGHW(V1, GR[1], GR[3]);
		} else {
			VMRGLW(V0, GR[0], GR[2]);
			VMRGLW(V1, GR[1], GR[3]);
		}
		if ((lane & 1) == 0)
			VMRGHW(V2, V0, V1);
		else
			VMRGLW(V2, V0, V1);
		src2 = V2;
	}
	const PPCReg out = lanes == 4 ? DR : V3;
	switch (op) {
	case IROp::FAdd: VADDFP(out, SR, src2); break;
	case IROp::FSub: VSUBFP(out, SR, src2); break;
	case IROp::FMul: VMADDFP(out, SR, src2, VNEGZERO); break;
	default: {
		// As VMX_FCondAssign, all lanes at once (that sequence is lane by lane anyway).
		const bool isMin = op == IROp::FMin;
		const PPCReg a = SR, b = src2;
		if (isMin)
			VCMPGTFP(V0, a, b);
		else
			VCMPGTFP(V0, b, a);
		VSEL(V1, a, b, V0);
		VAND(V0, a, b);
		VSPLTISW(V2, -1);
		VSRAW(V0, V0, V2);  // Both negative.
		VMINSW(V2, a, b);
		VMAXSW(V3, a, b);
		if (isMin)
			VSEL(V2, V2, V3, V0);
		else
			VSEL(V2, V3, V2, V0);
		VCMPEQFP(V0, a, a);
		VCMPEQFP(V3, b, b);
		VAND(V0, V0, V3);  // Neither is a NaN.
		VSEL(out, V2, V1, V0);
		break;
	}
	}
	if (lanes == 3) {
		VSPLTISW(V0, -1);
		VSPLTISW(V1, 0);
		VSLDOI(V0, V0, V1, 4);  // lanes 0-2
		VSEL(DR, DR, V3, V0);
	}
	regs_.MarkVRDirty(D);
	regs_.ReleaseSpillLockFPR(S, D);
	for (int k = 0; k < numGroups; ++k)
		regs_.ReleaseSpillLockFPR(groups[k]);
	s_rowOpFused++;
#if !PPSSPP_PLATFORM(PS3)
	if (getenv("PPC_FUSELOG"))
		fprintf(stderr, "PPC fused lane op x%d (%s, d=%d s=%d r=%d %s)\n", lanes, GetIRMeta(op)->name, D, S, R0, contiguous ? "group" : "row");
#endif
	return lanes;
}

// Lanes of FCmp mode, A+k, B+k; FpCondToReg t; FMovFromGPR f, t; FCvtSW D+k, f (vslt.q and the
// like with a prefix): the 1.0/0.0 results of a whole group at once, when fpcond, t and the f
// temps aren't read afterwards (as VMX_CompareToFloat does for one lane). Four lanes, or three
// (vslt.t: in place, the fourth lane kept). B is a group, or lanes just set to 0 (SetConstF).
static bool KnownZeroFPR(const IRInst *insts, int i, IRReg reg) {
	for (int j = i - 1; j >= 0 && j >= i - 32; --j) {
		const IRInstMeta m = GetIRMeta(insts[j]);
		if (IRWritesToFPR(m, reg))
			return insts[j].op == IROp::SetConstF && insts[j].constant == 0;
		if (m.m.flags & (IRFLAG_BARRIER | IRFLAG_EXIT))
			return false;
	}
	return false;
}

bool PPCJitBackend::MatchCompareN(const IRInst *insts, int i, int n, IRReg *a, IRReg *b, IRReg *d, int *mode, int *lanesOut, bool *bZero) const {
	if (i + 12 > n)
		return false;
	const IRInst &c0 = insts[i];
	if (c0.op != IROp::FCmp || c0.dest == IRFpCompareMode::False)
		return false;
	const IRReg A = c0.src1, B = c0.src2, t = insts[i + 1].dest;
	const IRReg D = insts[i + 3].dest;
	if ((A & 3) != 0 || (D & 3) != 0 || A < 32 || B < 32 || D < 32 || t == MIPS_REG_ZERO)
		return false;
	int lanes = 0;
	while (lanes < 4 && i + 4 * lanes + 4 <= n) {
		const int k = lanes;
		const IRInst &cmp = insts[i + 4 * k], &toReg = insts[i + 4 * k + 1], &mov = insts[i + 4 * k + 2], &cvt = insts[i + 4 * k + 3];
		if (cmp.op != IROp::FCmp || cmp.dest != c0.dest || cmp.src1 != A + k || cmp.src2 != B + k)
			break;
		if (toReg.op != IROp::FpCondToReg || toReg.dest != t)
			break;
		if (mov.op != IROp::FMovFromGPR || mov.src1 != t || cvt.op != IROp::FCvtSW || cvt.src1 != mov.dest || cvt.dest != D + k)
			break;
		// The temp f must not be one of the groups.
		if ((mov.dest >= A && mov.dest < A + 4) || (mov.dest >= B && mov.dest < B + 4) || (mov.dest >= D && mov.dest < D + 4))
			break;
		lanes++;
	}
	if (lanes < 3)
		return false;
	// Three lanes: in place only.
	if (lanes == 3 && D != A)
		return false;
	bool zero = true;
	for (int k = 0; k < lanes; ++k)
		if (!KnownZeroFPR(insts, i, B + k))
			zero = false;
	if (!zero && ((B & 3) != 0 || lanes != 4))
		return false;
	// A later lane must not compare a lane already written.
	if (!zero && B == D)
		return false;
	if (D != A && D < A + 4 && D + 4 > A)
		return false;
	*a = A; *b = B; *d = D; *mode = c0.dest; *lanesOut = lanes; *bZero = zero;
	return true;
}

bool PPCJitBackend::MatchCompare4(const IRInst *insts, int i, int n, IRReg *a, IRReg *b, IRReg *d, int *mode) const {
	int lanes;
	bool zero;
	return MatchCompareN(insts, i, n, a, b, d, mode, &lanes, &zero) && lanes == 4 && !zero;
}

int s_compare4Fused = 0;
int PPCJitBackend::VMX_Compare4(const IRInst *insts, int i, int n) {
	IRReg a, b, d;
	int mode, lanes;
	bool bZero;
	if (!MatchCompareN(insts, i, n, &a, &b, &d, &mode, &lanes, &bZero))
		return 0;
	const int count = 4 * lanes;
	const IRReg t = insts[i + 1].dest;
	if (!GprDeadAfter(insts, i + count, n, t) || !FpCondDeadAfter(insts, i + count, n))
		return 0;
	for (int k = 0; k < lanes; ++k)
		if (!DeadAfter(insts, i + count, n, insts[i + 4 * k + 2].dest))
			return 0;
	regs_.SetIRIndex(i + count - 1);
	if (bZero)
		regs_.SpillLockFPR(a, d);
	else
		regs_.SpillLockFPR(a, b, d);
	const PPCReg A = regs_.MapVR(a, 4, MIPSMap::INIT);
	const PPCReg B = bZero ? INVALID_REG : regs_.MapVR(b, 4, MIPSMap::INIT);
	const PPCReg D = d == a ? A : regs_.MapVR(d, 4, MIPSMap::NOINIT);
	// No regcache calls from here on: v0-v3 are ours.
	VOR(V0, A, A);
	if (bZero)
		VSPLTISW(V1, 0);
	else
		VOR(V1, B, B);
	VCmpMaskV01(mode);
	VSPLTISW(V0, 1);
	VAND(V1, V2, V0);
	if (lanes == 4) {
		VCFSX(D, V1, 0);
	} else {
		VCFSX(V3, V1, 0);
		VSPLTISW(V0, -1);
		VSPLTISW(V1, 0);
		VSLDOI(V0, V0, V1, 4);  // lanes 0-2
		VSEL(D, D, V3, V0);
	}
	regs_.MarkVRDirty(d);
	if (bZero)
		regs_.ReleaseSpillLockFPR(a, d);
	else
		regs_.ReleaseSpillLockFPR(a, b, d);
	s_compare4Fused++;
#if !PPSSPP_PLATFORM(PS3)
	if (getenv("PPC_FUSELOG"))
		fprintf(stderr, "PPC fused compare x%d (mode %d, a=%d b=%d%s d=%d)\n", lanes, mode, a, b, bZero ? " (zeros)" : "", d);
#endif
	return count;
}

// lwc1 then swc1 of the same register (a float copied from one place in memory to another,
// common in compiled PSP code): through a vector register that's two byte swaps and a lane
// shuffle. When the register isn't read afterwards (written again first), the word is copied
// through an integer register as it is. Frontend 103.
int s_floatCopyFused = 0;
int PPCJitBackend::VMX_FloatCopy(const IRInst *insts, int i, int n) {
	if (i + 2 > n)
		return 0;
	const IRInst &ld = insts[i], &st = insts[i + 1];
	if (ld.op != IROp::LoadFloat || st.op != IROp::StoreFloat || st.src3 != ld.dest)
		return 0;
	if (ld.src1 == MIPS_REG_ZERO || st.src1 == MIPS_REG_ZERO)
		return 0;
	if (!DeadAfter(insts, i + 2, n, ld.dest))
		return 0;
	regs_.SetIRIndex(i + 1);
	regs_.SpillLockGPR(ld.src1, st.src1);
	regs_.MapGPR(ld.src1);
	regs_.MapGPR(st.src1);
	AddressToScratch2(ld);
	LWZX(SCRATCH1, MEMBASEREG, SCRATCH2);
	AddressToScratch2(st);
	STWX(SCRATCH1, MEMBASEREG, SCRATCH2);
	regs_.ReleaseSpillLockGPR(ld.src1, st.src1);
	s_floatCopyFused++;
	return 2;
}

// A Vec4Init whose lanes are next read one at a time (a constant prefix on a VFPU op the IR does
// per lane, like vslt with vpfxt 0,0,0,0): as a group, each lane read wrote the group back and
// loaded the lane from memory right after (a load-hit-store stall per lane). Set the four lanes as
// scalar constants instead; a later Vec4 op still merges them in registers. Returns 1 if done.
int PPCJitBackend::VMX_Vec4InitScalars(const IRInst *insts, int i, int n) {
	const IRInst &init = insts[i];
	const IRReg first = init.dest;
	auto inGroup = [&](IRReg r, int lanes) { return r < first + 4 && r + lanes > first; };
	for (int j = i + 1; j < n && j < i + 16; ++j) {
		const IRInst &next = insts[j];
		const IRMeta *m = GetIRMeta(next.op);
		if (!m || (m->flags & (IRFLAG_BARRIER | IRFLAG_SRC3)))
			return 0;
		auto lanesOf = [](char t) { return t == 'F' ? 1 : (t == '2' ? 2 : (t == 'V' ? 4 : 0)); };
		const int l1 = lanesOf(m->types[1]), l2 = lanesOf(m->types[2]), ld = lanesOf(m->types[0]);
		const bool reads1 = l1 && inGroup(next.src1, l1), reads2 = l2 && inGroup(next.src2, l2);
		if (reads1 || reads2) {
			// The first use: split only for single-lane reads.
			if ((reads1 && l1 != 1) || (reads2 && l2 != 1))
				return 0;
			// Not when a whole-group compare (VMX_Compare4) reads it as one vector.
			IRReg ca, cb, cd;
			int cmode;
			if (next.op == IROp::FCmp && MatchCompare4(insts, j, n, &ca, &cb, &cd, &cmode))
				return 0;
			break;
		}
		if (ld && inGroup(next.dest, ld))
			return 0;
		if (j == n - 1 || j == i + 15)
			return 0;
	}
	static const u32 values[7][4] = {
		{ 0, 0, 0, 0 }, { 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000 }, { 0xBF800000, 0xBF800000, 0xBF800000, 0xBF800000 },
		{ 0x3F800000, 0, 0, 0 }, { 0, 0x3F800000, 0, 0 }, { 0, 0, 0x3F800000, 0 }, { 0, 0, 0, 0x3F800000 },
	};
	if (init.src1 > 6)
		return 0;
	for (int lane = 0; lane < 4; ++lane) {
		IRInst c{};
		c.op = IROp::SetConstF;
		c.dest = first + lane;
		c.constant = values[init.src1][lane];
		CompileIRInst(c);
	}
	return 1;
}

int PPCJitBackend::VMX_MatrixRows(const IRInst *insts, int i, int n) {
	if (i + 16 > n)
		return 0;
	const IRInst &first = insts[i];
	const bool load = first.op == IROp::LoadFloat;
	if (!load && first.op != IROp::StoreFloat)
		return 0;
	const IRReg m = load ? first.dest : first.src3;
	if (m < 32 || (m & 3) != 0 || m + 16 > 32 + 128)
		return 0;
	for (int k = 0; k < 16; ++k) {
		const IRInst &x = insts[i + k];
		const int r = k >> 2, c = k & 3;
		if (x.op != first.op || x.src1 != first.src1 || (load ? x.dest : x.src3) != m + 4 * c + r ||
			x.constant != first.constant + 16 * r + 4 * c)
			return 0;
	}

#if !PPSSPP_PLATFORM(PS3)
	if (getenv("PPC_MATRIXLOG"))
		fprintf(stderr, "matrix rows: %s m=%d base=%d off=%08x\n", load ? "load" : "store", m, first.src1, first.constant);
#endif
	regs_.SetIRIndex(i + 15);
	const IRReg baseReg = first.src1;
	if (baseReg != MIPS_REG_ZERO)
		regs_.SpillLockGPR(baseReg);
	regs_.SpillLockFPR(m, m + 4, m + 8, m + 12);
	PPCReg g[4];
	for (int c = 0; c < 4; ++c)
		g[c] = regs_.MapVR(m + 4 * c, 4, load ? MIPSMap::NOINIT : MIPSMap::INIT);
	const s32 offset = (s32)first.constant;
	if (baseReg == MIPS_REG_ZERO) {
		LI32(SCRATCH2, (u32)offset & 0x3FFFFFFF);
	} else {
		const PPCReg base = regs_.MapGPR(baseReg);
		if (offset == 0) {
			CLRLWI(SCRATCH2, base, 2);
		} else if (FitsS16(offset)) {
			ADDI(SCRATCH2, base, (s16)offset);
			CLRLWI(SCRATCH2, SCRATCH2, 2);
		} else {
			LI32(SCRATCH2, (u32)offset);
			ADD(SCRATCH2, SCRATCH2, base);
			CLRLWI(SCRATCH2, SCRATCH2, 2);
		}
	}
	ADD(SCRATCH1, MEMBASEREG, SCRATCH2);

	if (load) {
		// All four rows share the alignment: one permute pattern (pick 16 bytes, reverse each word).
		LVSL(V0, R0, SCRATCH1);
		VPERM(V0, V0, V0, VSWAPWORDS);
		for (int r = 0; r < 4; ++r) {
			if (r == 0) {
				LVX(V1, R0, SCRATCH1);
			} else {
				LI(R0, (s16)(16 * r));
				LVX(V1, SCRATCH1, R0);
			}
			LI(R0, (s16)(16 * r + 15));
			LVX(V2, SCRATCH1, R0);
			VPERM(g[r], V1, V2, V0);
		}
		// Rows A B C D (in g[0..3]) to columns.
		VMRGHW(V0, g[0], g[2]);  // a0 c0 a1 c1
		VMRGLW(V1, g[0], g[2]);  // a2 c2 a3 c3
		VMRGHW(V2, g[1], g[3]);  // b0 d0 b1 d1
		VMRGLW(g[2], g[1], g[3]);  // b2 d2 b3 d3
		VMRGHW(g[0], V0, V2);
		VMRGLW(g[1], V0, V2);
		VMRGLW(g[3], V1, g[2]);
		VMRGHW(g[2], V1, g[2]);
		for (int c = 0; c < 4; ++c)
			regs_.MarkVRDirty(m + 4 * c);
	} else {
		ANDI_(SCRATCH3, SCRATCH1, 15);
		FixupBranch unaligned = BC(Cond::NE);
		for (int half = 0; half < 2; ++half) {
			if (half == 0) {
				VMRGHW(V0, g[0], g[2]);
				VMRGHW(V1, g[1], g[3]);
			} else {
				VMRGLW(V0, g[0], g[2]);
				VMRGLW(V1, g[1], g[3]);
			}
			for (int lo = 0; lo < 2; ++lo) {
				const int r = half * 2 + lo;
				if (lo == 0)
					VMRGHW(V2, V0, V1);
				else
					VMRGLW(V2, V0, V1);
				VPERM(V2, V2, V2, VSWAPWORDS);
				if (r == 0) {
					STVX(V2, R0, SCRATCH1);
				} else {
					LI(R0, (s16)(16 * r));
					STVX(V2, SCRATCH1, R0);
				}
			}
		}
		const u8 *done = GetCodePointer();
		const PPCReg g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3];
		AddFarCode(unaligned, [this, g0, g1, g2, g3, done]() {
			// Not 16-aligned (a fused svl/svr pair): a word at a time.
			const PPCReg cols[4] = { g0, g1, g2, g3 };
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 4; ++c) {
					VSPLTW(V0, cols[c], r);
					VPERM(V0, V0, V0, VSWAPWORDS);
					LI(R0, (s16)(16 * r + 4 * c));
					STVEWX(V0, SCRATCH1, R0);
				}
			}
			B(done);
		});
	}
	regs_.ReleaseSpillLockFPR(m, m + 4, m + 8, m + 12);
	if (baseReg != MIPS_REG_ZERO)
		regs_.ReleaseSpillLockGPR(baseReg);
	return 16;
}

void PPCJitBackend::VMX_VecAssign(IRInst inst) {
	switch (inst.op) {
	case IROp::Vec4Mov:
		if (inst.dest != inst.src1) {
			regs_.Map(inst);
			VMR(regs_.V(inst.dest), regs_.V(inst.src1));
		}
		break;
	case IROp::Vec4Blend:
		// Frontend 108: an IR pass can blend FPU registers into a vector (Midnight Club L.A.:
		// mtv f12 into S000 became Vec4Blend vf0..vf3, vf0..vf3, f12..f15, 1). The PS3 build keeps
		// f12-f16 as single values in their own registers, which can't be mapped as a group: copy
		// the lanes one at a time instead.
		if (inst.dest < 32 || inst.src1 < 32 || inst.src2 < 32) {
			bool ok = true;
			for (int j = 1; j < 4 && ok; ++j) {
				const IRReg srcJ = (((inst.constant >> j) & 1) ? inst.src2 : inst.src1) + j;
				for (int k = 0; k < j; ++k)
					if (srcJ == inst.dest + k)
						ok = false;
			}
			if (ok) {
				for (int k = 0; k < 4; ++k) {
					const IRReg src = (((inst.constant >> k) & 1) ? inst.src2 : inst.src1) + k;
					if (src == inst.dest + k)
						continue;
					IRInst mov{};
					mov.op = IROp::FMov;
					mov.dest = inst.dest + k;
					mov.src1 = src;
					CompileIRInst(mov);
				}
				break;
			}
		}
		regs_.Map(inst);
		VConst(V0, Vmx().blend[inst.constant & 15]);
		VSEL(regs_.V(inst.dest), regs_.V(inst.src1), regs_.V(inst.src2), V0);
		break;
	case IROp::Vec4Shuffle:
		regs_.Map(inst);
		VConst(V0, Vmx().shuffle[inst.src2 & 0xFF]);
		VPERM(regs_.V(inst.dest), regs_.V(inst.src1), regs_.V(inst.src1), V0);
		break;
	case IROp::Vec4Init:
		if (inst.src1 >= 7)
			DISABLE;
		regs_.Map(inst);
		if (inst.src1 == (int)Vec4Init::AllZERO)
			VSPLTISW(regs_.V(inst.dest), 0);  // No constant to load (Frontend 103).
		else
			VConst(regs_.V(inst.dest), Vmx().init[inst.src1]);
		break;
	default:
		DISABLE;
	}
}

void PPCJitBackend::VMX_VecArith(IRInst inst) {
	switch (inst.op) {
	case IROp::Vec4Add:
		regs_.Map(inst);
		VADDFP(regs_.V(inst.dest), regs_.V(inst.src1), regs_.V(inst.src2));
		break;
	case IROp::Vec4Sub:
		regs_.Map(inst);
		VSUBFP(regs_.V(inst.dest), regs_.V(inst.src1), regs_.V(inst.src2));
		break;
	case IROp::Vec4Mul:
		regs_.Map(inst);
		VMADDFP(regs_.V(inst.dest), regs_.V(inst.src1), regs_.V(inst.src2), VNEGZERO);
		break;
	case IROp::Vec4Neg:
		regs_.Map(inst);
		VXOR(regs_.V(inst.dest), regs_.V(inst.src1), VNEGZERO);
		break;
	case IROp::Vec4Abs:
		regs_.Map(inst);
		VANDC(regs_.V(inst.dest), regs_.V(inst.src1), VNEGZERO);
		break;
	case IROp::Vec4Div: {
		// No exact VMX divide: four FPU divides through the stack (48..95 of our frame).
		regs_.Map(inst);
		LI(R0, 48);
		STVX(regs_.V(inst.src1), R1, R0);
		LI(R0, 64);
		STVX(regs_.V(inst.src2), R1, R0);
		for (int i = 0; i < 4; ++i) {
			LFS((PPCReg)(F1 + i), R1, (s16)(48 + i * 4));
			LFS((PPCReg)(F5 + i), R1, (s16)(64 + i * 4));
		}
		for (int i = 0; i < 4; ++i)
			FDIVS((PPCReg)(F1 + i), (PPCReg)(F1 + i), (PPCReg)(F5 + i));
		for (int i = 0; i < 4; ++i)
			STFS((PPCReg)(F1 + i), R1, (s16)(VTEMP + i * 4));
		LI(R0, VTEMP);
		LVX(regs_.V(inst.dest), R1, R0);
		break;
	}
	case IROp::Vec4Scale: {
		// The scale may be a lane of the source or destination group: then take it from there
		// (mapping it as its own scalar would split the group).
		const IRReg sgroup = inst.src2 & ~3;
		if (sgroup != inst.src1 && sgroup != inst.dest) {
			// A lane of a third group (a matrix multiply: each column times a lane of another
			// column). Unless the lane is already a scalar of its own, take it from the whole group
			// in a register: mapping it as a scalar stored the group and reloaded the lane from
			// memory right away (a load-hit-store stall, ~12 in each vmmul.q).
			if (inst.src2 >= 32 && sgroup >= 32 && (regs_.VRLanes(sgroup) == 4 || !regs_.InVR(inst.src2)) &&
				(inst.dest & 3) == 0 && (inst.src1 & 3) == 0 && (sgroup & 3) == 0 &&
				!(inst.dest < sgroup + 4 && inst.dest + 4 > sgroup) && !(inst.src1 < sgroup + 4 && inst.src1 + 4 > sgroup)) {
				regs_.SpillLockFPR(inst.src1, inst.dest, sgroup);
				const PPCReg sv = regs_.MapVR(sgroup, 4, MIPSMap::INIT);
				const PPCReg a = regs_.MapVR(inst.src1, 4, MIPSMap::INIT);
				const PPCReg d = regs_.MapVR(inst.dest, 4, inst.dest == inst.src1 ? MIPSMap::DIRTY : MIPSMap::NOINIT);
				VSPLTW(V1, sv, inst.src2 & 3);
				VMADDFP(d, a, V1, VNEGZERO);
				regs_.MarkVRDirty(inst.dest);
				regs_.ReleaseSpillLockFPR(inst.src1, inst.dest, sgroup);
				break;
			}
			regs_.Map(inst);
			VSPLTW(V1, regs_.V(inst.src2), 0);
			VMADDFP(regs_.V(inst.dest), regs_.V(inst.src1), V1, VNEGZERO);
			break;
		}
		regs_.SpillLockFPR(inst.src1, inst.dest);
		const PPCReg a = regs_.MapVR(inst.src1, 4, MIPSMap::INIT);
		const PPCReg d = regs_.MapVR(inst.dest, 4, inst.dest == inst.src1 || sgroup == inst.dest ? MIPSMap::DIRTY : MIPSMap::NOINIT);
		VSPLTW(V1, sgroup == inst.src1 ? a : d, inst.src2 & 3);
		VMADDFP(d, a, V1, VNEGZERO);
		regs_.MarkVRDirty(inst.dest);
		regs_.ReleaseSpillLockFPR(inst.src1, inst.dest);
		break;
	}
	default:
		INVALIDOP;
	}
}

void PPCJitBackend::VMX_VecHoriz(IRInst inst) {
	if (inst.op != IROp::Vec4Dot)
		DISABLE;
	regs_.SpillLockFPR(inst.src1, inst.src2);
	const PPCReg a = regs_.MapVR(inst.src1, 4, MIPSMap::INIT);
	const PPCReg b = regs_.MapVR(inst.src2, 4, MIPSMap::INIT);
	// Same order as the interpreter: ((a0*b0 + a1*b1) + a2*b2) + a3*b3.
	VMADDFP(V0, a, b, VNEGZERO);
	VSLDOI(V1, V0, V0, 4);
	VADDFP(V2, V0, V1);
	VSLDOI(V1, V0, V0, 8);
	VADDFP(V2, V2, V1);
	VSLDOI(V1, V0, V0, 12);
	VADDFP(V2, V2, V1);
	const IRReg dgroup = inst.dest & ~3;
	if (dgroup == inst.src1 || dgroup == inst.src2) {
		// The result goes into a lane of a source group: insert it there.
		const PPCReg g = dgroup == inst.src1 ? a : b;
		VSPLTW(V2, V2, 0);
		VConst(V1, Vmx().laneMask[inst.dest & 3]);
		VSEL(g, g, V2, V1);
		regs_.MarkVRDirty(inst.dest);
	} else {
		// v0 survives mapping a single lane (the regcache moves values through v2/v3 only).
		VMR(V0, V2);
		const PPCReg d = regs_.MapVR(inst.dest, 1, MIPSMap::NOINIT);
		VMR(d, V0);
	}
	regs_.ReleaseSpillLockFPR(inst.src1, inst.src2);
}

// A lane-0 compare mask (all ones = true) for an IRFpCompareMode, into v2. Clobbers v0, v1.
void PPCJitBackend::VCmpMask(int mode, PPCReg a, PPCReg b) {
	VSPLTW(V0, a, 0);
	VSPLTW(V1, b, 0);
	VCmpMaskV01(mode);
}

// The compare of v0 with v1, lane by lane, into v2 (all ones where true). Clobbers v0, v1.
void PPCJitBackend::VCmpMaskV01(int mode) {
	switch (mode) {
	case (int)IRFpCompareMode::EqualOrdered:
		VCMPEQFP(V2, V0, V1);
		break;
	case (int)IRFpCompareMode::EqualUnordered:
		VCMPEQFP(V2, V0, V1);
		VCMPEQFP(V0, V0, V0);
		VCMPEQFP(V1, V1, V1);
		VAND(V0, V0, V1);
		VNOR(V0, V0, V0);
		VOR(V2, V2, V0);
		break;
	case (int)IRFpCompareMode::LessEqualOrdered:
		VCMPGEFP(V2, V1, V0);
		break;
	case (int)IRFpCompareMode::LessEqualUnordered:
		VCMPGTFP(V2, V0, V1);
		VNOR(V2, V2, V2);
		break;
	case (int)IRFpCompareMode::LessOrdered:
		VCMPGTFP(V2, V1, V0);
		break;
	case (int)IRFpCompareMode::LessUnordered:
		VCMPGEFP(V2, V0, V1);
		VNOR(V2, V2, V2);
		break;
	default:  // EitherUnordered
		VCMPEQFP(V2, V0, V0);
		VCMPEQFP(V1, V1, V1);
		VAND(V2, V2, V1);
		VNOR(V2, V2, V2);
		break;
	}
}

// v2 lane 0 (a compare mask, the same in every lane) -> 0 or 1 in dest. Clobbers v0, CR6.
void PPCJitBackend::VMaskToBool(PPCReg dest) {
	VSPLTISW(V0, 0);
	VCMPEQUW(V0, V2, V0, true);  // CR6 bit 0: all lanes zero, i.e. false.
	MFOCRF(SCRATCH1, 6);
	RLWINM(dest, SCRATCH1, 25, 31, 31);
	XORI(dest, dest, 1);
}

void PPCJitBackend::VMX_FCompare(IRInst inst) {
	switch (inst.op) {
	case IROp::FCmp:
		if (inst.dest == IRFpCompareMode::False) {
			regs_.SetGPRImm(IRREG_FPCOND, 0);
			break;
		}
		regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
		{
			// One compare whose own CR6 gives the result (operands in every lane: bit 0 true in
			// all lanes, bit 2 in none), instead of a mask compared again against zero.
			int crBit = -1;
			VSPLTW(V0, regs_.V(inst.src1), 0);
			VSPLTW(V1, regs_.V(inst.src2), 0);
			switch (inst.dest) {
			case (int)IRFpCompareMode::EqualOrdered: VCMPEQFP(V2, V0, V1, true); crBit = 24; break;
			case (int)IRFpCompareMode::LessEqualOrdered: VCMPGEFP(V2, V1, V0, true); crBit = 24; break;
			case (int)IRFpCompareMode::LessOrdered: VCMPGTFP(V2, V1, V0, true); crBit = 24; break;
			case (int)IRFpCompareMode::LessEqualUnordered: VCMPGTFP(V2, V0, V1, true); crBit = 26; break;  // !(a > b)
			case (int)IRFpCompareMode::LessUnordered: VCMPGEFP(V2, V0, V1, true); crBit = 26; break;  // !(a >= b)
			default: break;
			}
			if (crBit >= 0) {
				MFOCRF(SCRATCH1, 6);
				RLWINM(regs_.R(IRREG_FPCOND), SCRATCH1, crBit + 1, 31, 31);
			} else {
				VCmpMaskV01(inst.dest);
				VMaskToBool(regs_.R(IRREG_FPCOND));
			}
		}
		break;
	case IROp::FCmpVfpuBit: {
		const int op = inst.dest & 0xF;
		const int bit = inst.dest >> 4;
		const bool twoOperands = op == VC_EQ || op == VC_LT || op == VC_LE || op == VC_NE || op == VC_GE || op == VC_GT;
		const bool zeroCompare = op == VC_EZ || op == VC_NZ;
		if (!(twoOperands || zeroCompare || op == VC_TR || op == VC_FL))
			DISABLE;
		regs_.MapWithExtra(inst, { { 'G', IRREG_VFPU_CC, 1, MIPSMap::DIRTY } });
		const PPCReg cc = regs_.R(IRREG_VFPU_CC);
		if (op == VC_TR || op == VC_FL) {
			LI(SCRATCH2, op == VC_TR ? 1 : 0);
			RLWIMI(cc, SCRATCH2, bit, 31 - bit, 31 - bit);
			break;
		}
		VSPLTW(V0, regs_.V(inst.src1), 0);
		if (twoOperands)
			VSPLTW(V1, regs_.V(inst.src2), 0);
		else
			VSPLTISW(V1, 0);
		// Both operands in every lane: the compare's own CR6 says the result (bit 0: true in all
		// lanes, bit 2: in none), straight into the CC bit. Before: a second compare of the mask
		// against zero, a move, an extract and an xor for each lane (Midnight Club's vcmp.q).
		int crBit = 24;  // CR6 "all lanes true", big-endian bit number in the CR.
		switch (op) {
		case VC_EQ: case VC_EZ: VCMPEQFP(V2, V0, V1, true); break;
		case VC_NE: case VC_NZ: VCMPEQFP(V2, V0, V1, true); crBit = 26; break;
		case VC_LT: VCMPGTFP(V2, V1, V0, true); break;
		case VC_GT: VCMPGTFP(V2, V0, V1, true); break;
		case VC_LE: VCMPGEFP(V2, V1, V0, true); break;
		default: VCMPGEFP(V2, V0, V1, true); break;  // VC_GE
		}
		MFOCRF(SCRATCH1, 6);
		RLWIMI(cc, SCRATCH1, (crBit + 1 + bit) & 31, 31 - bit, 31 - bit);
		break;
	}
	case IROp::FCmovVfpuCC: {
		regs_.MapWithExtra(inst, { { 'G', IRREG_VFPU_CC, 1, MIPSMap::INIT } });
		const int bit = inst.src2 & 0xF;
		RLWINM(SCRATCH1, regs_.R(IRREG_VFPU_CC), (32 - bit) & 31, 31, 31);
		CMPWI(SCRATCH1, 0);
		FixupBranch skip = BC((inst.src2 >> 7) & 1 ? Cond::EQ : Cond::NE);
		VMR(regs_.V(inst.dest), regs_.V(inst.src1));
		SetJumpTarget(skip);
		break;
	}
	case IROp::FCmpVfpuAggregate: {
		regs_.MapWithExtra(inst, { { 'G', IRREG_VFPU_CC, 1, MIPSMap::DIRTY } });
		const PPCReg cc = regs_.R(IRREG_VFPU_CC);
		const u32 mask = inst.dest;
		ANDI_(SCRATCH1, cc, (u16)mask);
		LI(SCRATCH2, 0);
		FixupBranch none = BC(Cond::EQ);
		LI(SCRATCH2, 0x10);
		CMPLWI(SCRATCH1, (u16)mask);
		FixupBranch notAll = BC(Cond::NE);
		LI(SCRATCH2, 0x30);
		SetJumpTarget(notAll);
		SetJumpTarget(none);
		LI(SCRATCH1, 0x30);
		ANDC(cc, cc, SCRATCH1);
		OR(cc, cc, SCRATCH2);
		break;
	}
	default:
		DISABLE;
	}
}

void PPCJitBackend::VMX_FCondAssign(IRInst inst) {
	if (inst.op != IROp::FMin && inst.op != IROp::FMax)
		DISABLE;
	regs_.Map(inst);
	// In AltiVec registers (the FPU version moved both values through memory and back, three
	// load-hit-store stalls). No NaN: std::min(a, b) = b < a ? b : a, std::max(a, b) = a < b ? b : a.
	// A NaN: the bits compared as signed ints, the other way around when both are negative.
	const bool isMin = inst.op == IROp::FMin;
	const PPCReg a = regs_.V(inst.src1), b = regs_.V(inst.src2);
	if (isMin)
		VCMPGTFP(V0, a, b);
	else
		VCMPGTFP(V0, b, a);
	VSEL(V1, a, b, V0);
	VAND(V0, a, b);
	VSPLTISW(V2, -1);
	VSRAW(V0, V0, V2);  // Both negative.
	VMINSW(V2, a, b);
	VMAXSW(V3, a, b);
	if (isMin)
		VSEL(V2, V2, V3, V0);
	else
		VSEL(V2, V3, V2, V0);
	VCMPEQFP(V0, a, a);
	VCMPEQFP(V3, b, b);
	VAND(V0, V0, V3);  // Neither is a NaN.
	VSEL(regs_.V(inst.dest), V2, V1, V0);
}

void PPCJitBackend::VMX_FCvt(IRInst inst) {
	if ((inst.op == IROp::FCvtSW || inst.op == IROp::FCvtScaledSW) && ExactFPU(inst.dest)) {
		regs_.Map(inst);
		// As in VMX_FArith: the VMX conversion while the game rounds to nearest.
		FixupBranch slow = BranchIfNotNearest();
		const PPCReg d = regs_.V(inst.dest), src = regs_.V(inst.src1);
		VCFSX(d, src, inst.op == IROp::FCvtScaledSW ? (inst.src2 & 0x1F) : 0);
		const u8 *done = GetCodePointer();
		const bool scaled = inst.op == IROp::FCvtScaledSW;
		const int shift = inst.src2 & 0x1F;
		AddFarCode(slow, [this, d, src, scaled, shift, done]() {
			VRToFPR(F1, src);
			IntBitsToFloat(F3, F1);
			if (scaled) {
				const float scale = 1.0f / (float)(1UL << shift);
				u32 bits;
				memcpy(&bits, &scale, 4);
				LI32(SCRATCH1, bits);
				STW(SCRATCH1, R1, FTEMP);
				LFS(FSCRATCH2, R1, FTEMP);
				FMULS(F3, F3, FSCRATCH2);
			}
			FPRToVR(d, F3);
			B(done);
		});
		return;
	}
	switch (inst.op) {
	case IROp::FCvtSW:
		regs_.Map(inst);
		VCFSX(regs_.V(inst.dest), regs_.V(inst.src1), 0);
		break;
	case IROp::FCvtScaledSW:
		// int / 2^n with one rounding: the same as converting then scaling by the exact 2^-n.
		regs_.Map(inst);
		VCFSX(regs_.V(inst.dest), regs_.V(inst.src1), inst.src2 & 0x1F);
		break;
	case IROp::FCvtScaledWS: {
		// Saturated int(round(v * 2^n)); NaN gives 0x7FFFFFFF (as the interpreter).
		regs_.Map(inst);
		const PPCReg s = regs_.V(inst.src1), d = regs_.V(inst.dest);
		const int n = inst.src2 & 0x1F;
		VSPLTISW(V1, 1);
		VSPLTISW(V2, n >= 16 ? n - 32 : n);
		VSLW(V1, V1, V2);
		VCFUX(V1, V1, 0);
		VMADDFP(V0, s, V1, VNEGZERO);
		switch (IRRoundMode(inst.src2 >> 6)) {
		case IRRoundMode::RINT_0: VRFIN(V0, V0); break;
		case IRRoundMode::CEIL_2: VRFIP(V0, V0); break;
		case IRRoundMode::FLOOR_3: VRFIM(V0, V0); break;
		default: break;  // CAST_1 truncates, as vctsxs does.
		}
		VCTSXS(V0, V0, 0);
		VCMPEQFP(V2, s, s);
		VNegZero(V1);
		VNOR(V1, V1, V1);
		VSEL(d, V1, V0, V2);
		break;
	}
	default:
		DISABLE;
	}
}

void PPCJitBackend::VMX_FRound(IRInst inst) {
	regs_.Map(inst);
	{
		// In VMX, as FCvtScaledWS does: round to an integral float, convert with saturation, and
		// 0x7FFFFFFF for NaN (the interpreter's SaturatedFloatToInt). The FPU route below went
		// through memory four times (load-hit-store stalls) and changed FPSCR (mtfsf serializes).
		// vrfin rounds to nearest even, as round_ieee_754 does.
		const PPCReg s = regs_.V(inst.src1), d = regs_.V(inst.dest);
		PPCReg in = s;
		if (inst.op == IROp::FCeil || inst.op == IROp::FFloor) {
			// VMX reads denormals as zero, but ceil of a positive one is 1 and floor of a negative
			// one -1 (cpu/fpu/roundmode): round +-FLT_MIN instead, which gives the same.
			VSPLTISW(V1, 1);
			VSLW(V2, s, V1);  // The bits without the sign.
			VConst(V0, Vmx().denormLimit);
			VCMPGTUW(V0, V0, V2);
			VSPLTISW(V1, 0);
			VCMPEQUW(V1, V2, V1, false);
			VANDC(V0, V0, V1);  // Denormal, not zero.
			VNegZero(V1);
			VAND(V1, s, V1);
			VConst(V2, Vmx().fltMin);
			VOR(V1, V1, V2);
			VSEL(V3, s, V1, V0);  // v3: free once the registers are mapped.
			in = V3;
		}
		switch (inst.op) {
		case IROp::FTrunc: VOR(V0, s, s); break;  // vctsxs truncates (a denormal gives 0, as it should).
		case IROp::FRound: VRFIN(V0, s); break;
		case IROp::FCeil: VRFIP(V0, in); break;
		case IROp::FFloor: VRFIM(V0, in); break;
		default: INVALIDOP;
		}
		VCTSXS(V0, V0, 0);
		VCMPEQFP(V2, s, s);
		VNegZero(V1);
		VNOR(V1, V1, V1);
		VSEL(d, V1, V0, V2);
		return;
	}
	VRToFPR(F1, regs_.V(inst.src1));
	switch (inst.op) {
	case IROp::FTrunc: FloatToIntBits(F3, F1, -1); break;
	case IROp::FRound: FloatToIntBits(F3, F1, 0); break;
	case IROp::FCeil: FloatToIntBits(F3, F1, 2); break;
	case IROp::FFloor: FloatToIntBits(F3, F1, 3); break;
	default: INVALIDOP;
	}
	// FloatToIntBits left the result word in SCRATCH1 as well.
	GPRToVR(regs_.V(inst.dest), SCRATCH1);
}

// vsincos: sine in f1, cosine left here (read right after the call).
static float s_sinCosCos;
static float SinCosHelper(float a) {
	float s, c;
	vfpu_sincos(a, s, c);
	s_sinCosCos = c;
	return s;
}

void PPCJitBackend::VMX_FSpecial(IRInst inst) {
	if (inst.op == IROp::FSinCos) {
		regs_.FlushBeforeCall();
		if (regs_.InVR(inst.src1))
			VRToFPR(F1, regs_.V(inst.src1), regs_.VRLane(inst.src1));
		else
			LFS(F1, CTXREG, (s16)(offsetof(MIPSState, f) + inst.src1 * 4));
		SaveVolatileStatics();
		QuickCallFunction(&SinCosHelper);
		LoadVolatileStatics();
		// The pair may overlap the source; it's been read.
		FPRToVR(regs_.MapVR(inst.dest, 1, MIPSMap::NOINIT), F1);
		LIPTR(R12, (uintptr_t)&s_sinCosCos);
		LFS(F1, R12, 0);
		FPRToVR(regs_.MapVR(inst.dest + 1, 1, MIPSMap::NOINIT), F1);
		return;
	}
	float (*func)(float) = nullptr;
	switch (inst.op) {
	case IROp::FSin: func = &vfpu_sin; break;
	case IROp::FCos: func = &vfpu_cos; break;
	case IROp::FRSqrt: func = &vfpu_rsqrt; break;
	case IROp::FRecip: func = &vfpu_rcp; break;
	case IROp::FVSqrt: func = &vfpu_sqrt; break;
	case IROp::FAsin: func = &vfpu_asin; break;
	// vexp2/vlog2: the interpreter's own bit-exact helpers (Midnight Club L.A. runs them ~13,000
	// times a frame; the generic fallback saved and reloaded every static register for each).
	case IROp::FExp2: func = &vfpu_exp2; break;
	case IROp::FLog2: func = &vfpu_log2; break;
	default: DISABLE;
	}
	regs_.FlushBeforeCall();
	if (regs_.InVR(inst.src1))
		VRToFPR(F1, regs_.V(inst.src1), regs_.VRLane(inst.src1));
	else
		LFS(F1, CTXREG, (s16)(offsetof(MIPSState, f) + inst.src1 * 4));
	SaveVolatileStatics();
	QuickCallFunction(func);
	LoadVolatileStatics();
	const PPCReg d = regs_.MapVR(inst.dest, 1, MIPSMap::NOINIT);
	FPRToVR(d, F1);
}

void PPCJitBackend::VMX_Transfer(IRInst inst) {
	switch (inst.op) {
	case IROp::FMovFromGPR:
		regs_.Map(inst);
		GPRToVR(regs_.V(inst.dest), regs_.R(inst.src1));
		break;
	case IROp::FMovToGPR:
		regs_.Map(inst);
		VRToGPR(regs_.R(inst.dest), regs_.V(inst.src1));
		break;
	case IROp::FpCondFromReg:
		regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::NOINIT } });
		MR(regs_.R(IRREG_FPCOND), regs_.R(inst.src1));
		break;
	case IROp::FpCondToReg:
		regs_.MapWithExtra(inst, { { 'G', IRREG_FPCOND, 1, MIPSMap::INIT } });
		MR(regs_.R(inst.dest), regs_.R(IRREG_FPCOND));
		break;
	case IROp::SetCtrlVFPU:
		regs_.SetGPRImm(IRREG_VFPU_CTRL_BASE + inst.dest, inst.constant);
		break;
	case IROp::SetCtrlVFPUReg:
		regs_.MapWithExtra(inst, { { 'G', (IRReg)(IRREG_VFPU_CTRL_BASE + inst.dest), 1, MIPSMap::NOINIT } });
		MR(regs_.R(IRREG_VFPU_CTRL_BASE + inst.dest), regs_.R(inst.src1));
		break;
	case IROp::VfpuCtrlToReg:
		regs_.MapWithExtra(inst, { { 'G', (IRReg)(IRREG_VFPU_CTRL_BASE + inst.src1), 1, MIPSMap::INIT } });
		MR(regs_.R(inst.dest), regs_.R(IRREG_VFPU_CTRL_BASE + inst.src1));
		break;
	default:
		DISABLE;
	}
}

void PPCJitBackend::VMX_VecPack(IRInst inst) {
	switch (inst.op) {
	case IROp::Vec2Unpack16To32:
	case IROp::Vec2Unpack16To31: {
		// In AltiVec registers (through a GPR it was three load-hit-store stalls each). The results
		// go to v0/v1 first: the destination pair may overlap the source.
		const PPCReg s = regs_.MapVR(inst.src1, 1, MIPSMap::INIT);
		VSPLTISW(V2, -16);  // Shift counts of 16.
		VSLW(V0, s, V2);
		VSPLTISW(V3, -1);
		VSLW(V3, V3, V2);  // 0xFFFF0000.
		VAND(V1, s, V3);
		if (inst.op == IROp::Vec2Unpack16To31) {
			VSPLTISW(V3, 1);
			VSRW(V0, V0, V3);
			VSRW(V1, V1, V3);
		}
		VMR(regs_.MapVR(inst.dest, 1, MIPSMap::NOINIT), V0);
		VMR(regs_.MapVR(inst.dest + 1, 1, MIPSMap::NOINIT), V1);
		break;
	}
	case IROp::Vec2Pack32To16: {
		// (s0 >> 16) | (s1 & 0xFFFF0000). Frontend 109: all mapped before v0-v3 are used - mapping
		// with many registers live moves values through v2/v3, which wiped the mask (wrong low
		// halves from vi2s, Midnight Club L.A.).
		const IRReg d = inst.dest, a = inst.src1, b = inst.src1 + 1;
		regs_.SpillLockFPR(a, b, d);
		const PPCReg ra = regs_.MapVR(a, 1, MIPSMap::INIT);
		const PPCReg rb = regs_.MapVR(b, 1, MIPSMap::INIT);
		const PPCReg rd = regs_.MapVR(d, 1, (d == a || d == b) ? MIPSMap::DIRTY : MIPSMap::NOINIT);
		VSPLTISW(V2, -16);
		VSRW(V0, ra, V2);
		VSPLTISW(V3, -1);
		VSLW(V3, V3, V2);
		VAND(V1, rb, V3);
		VOR(rd, V0, V1);
		regs_.MarkVRDirty(d);
		regs_.ReleaseSpillLockFPR(a, b, d);
		break;
	}
	case IROp::Vec4Pack31To8: {
		// Bits 30-23 of each lane (negative lanes 0), lane i into byte i (little-endian).
		// The scalar result can be a lane of the source group (vi2uc.q S000, C000): mapped by
		// hand, like Vec4Dot, so the other three lanes survive.
		regs_.SpillLockFPR(inst.src1);
		const PPCReg s = regs_.MapVR(inst.src1, 4, MIPSMap::INIT);
		VSPLTISW(V1, 0);
		VMAXSW(V0, s, V1);
		VSPLTISW(V1, 23 - 32);
		VSRW(V0, V0, V1);
		VPKUWUS(V0, V0, V0);
		VPKUHUS(V0, V0, V0);
		VPERM(V0, V0, V0, VLANE0SWAP);  // The packed word, byte-swapped, in every lane.
		if ((inst.dest & ~3) == inst.src1) {
			VConst(V1, Vmx().laneMask[inst.dest & 3]);
			VSEL(s, s, V0, V1);
			regs_.MarkVRDirty(inst.dest);
		} else {
			VMR(regs_.MapVR(inst.dest, 1, MIPSMap::NOINIT), V0);
		}
		regs_.ReleaseSpillLockFPR(inst.src1);
		break;
	}
	case IROp::Vec4Pack32To8: {
		// The top byte of each lane, lane i into byte i (little-endian). Mapped as Vec4Pack31To8.
		regs_.SpillLockFPR(inst.src1);
		const PPCReg s = regs_.MapVR(inst.src1, 4, MIPSMap::INIT);
		VSPLTISW(V1, 24 - 32);
		VSRW(V0, s, V1);
		VPKUWUS(V0, V0, V0);
		VPKUHUS(V0, V0, V0);
		VPERM(V0, V0, V0, VLANE0SWAP);
		if ((inst.dest & ~3) == inst.src1) {
			VConst(V1, Vmx().laneMask[inst.dest & 3]);
			VSEL(s, s, V0, V1);
			regs_.MarkVRDirty(inst.dest);
		} else {
			VMR(regs_.MapVR(inst.dest, 1, MIPSMap::NOINIT), V0);
		}
		regs_.ReleaseSpillLockFPR(inst.src1);
		break;
	}
	case IROp::Vec4Unpack8To32: {
		// The source can be a lane of the destination group (vuc2i.q C000, S000): copy it out
		// before the group is mapped.
		VMR(V0, regs_.MapVR(inst.src1, 1, MIPSMap::INIT));
		const PPCReg d = regs_.MapVR(inst.dest, 4, MIPSMap::NOINIT);
		VSPLTISW(V1, 0);
		VConst(V2, Vmx().unpack8);
		VPERM(d, V0, V1, V2);
		break;
	}
	case IROp::Vec4DuplicateUpperBitsAndShift1: {
		// v |= v >> 8; v |= v >> 16; v >>= 1 in each lane.
		regs_.Map(inst);
		const PPCReg s = regs_.V(inst.src1), d = regs_.V(inst.dest);
		VSPLTISW(V1, 8);
		VSRW(V0, s, V1);
		VOR(V0, s, V0);
		VSPLTISW(V1, 16 - 32);
		VSRW(V2, V0, V1);
		VOR(V0, V0, V2);
		VSPLTISW(V1, 1);
		VSRW(d, V0, V1);
		break;
	}
	default:
		DISABLE;
	}
}

}  // namespace MIPSComp

#endif
