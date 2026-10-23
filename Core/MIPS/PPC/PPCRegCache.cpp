// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ppsspp_config.h"
#include "Core/MIPS/PPC/PPCBuild.h"
#ifdef PPSSPP_PPC_JIT_BUILD

#include <cstddef>
#include <cstring>
#include "Common/Log.h"
#include "Core/MIPS/IR/IRInst.h"
#include "Core/MIPS/IR/IRAnalysis.h"
#include "Core/MIPS/PPC/PPCRegCache.h"
#include "Core/MIPS/JitCommon/JitState.h"

using namespace PPCGen;
using namespace PPCJitConstants;

PPCRegCache::PPCRegCache(MIPSComp::JitOptions *jo)
	: IRNativeRegCacheBase(jo) {
	config_.totalNativeRegs = NUM_PPCGPR + NUM_PPCFPR;
	config_.mapUseVRegs = false;
	config_.mapFPUSIMD = false;
	vmx_ = MIPSComp::g_ppcJitVMX;
	if (vmx_) {
		config_.totalNativeRegs = NUM_PPCGPR + NUM_PPCFPR + 32;
		config_.mapUseVRegs = true;
		config_.mapFPUSIMD = true;
		config_.mapFloatsAsVRegs = true;
	}
}

void PPCRegCache::Init(PPCEmitter *emitter) {
	emit_ = emitter;
}

const int *PPCRegCache::GetAllocationOrder(MIPSLoc type, MIPSMap flags, int &count, int &base) const {
	base = R0;
	if (type == MIPSLoc::REG) {
		// Callee-saved first (survive helper calls), then volatile ones. Excluded: r0 (reads as 0
		// in addresses), r1 (stack), r2 (TOC; a static register), r3/r4/r11/r12 (scratch, call
		// sequence), r13 (system), r28-r31 (fixed and static).
		static const int allocationOrder[] = {
			R14, R15, R16, R17, R18, R19, R20, R21, R22, R23, R24, R25, R26, R27,
			R5, R6, R7, R8, R9, R10,
		};
		// R14-R27 hold the static registers.
		static const int allocationOrderStatic[] = {
			R5, R6, R7, R8, R9, R10,
		};
		if (jo_->useStaticAlloc) {
			count = ARRAY_SIZE(allocationOrderStatic);
			return allocationOrderStatic;
		}
		count = ARRAY_SIZE(allocationOrder);
		return allocationOrder;
	} else if (type == MIPSLoc::FREG) {
		// F0 and F13 are scratch.
		static const int allocationOrder[] = {
			F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24, F25, F26, F27, F28, F29, F30, F31,
			F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
		};
		count = ARRAY_SIZE(allocationOrder);
		return allocationOrder;
	} else if (type == MIPSLoc::VREG) {
		// Non-volatile v20-v28 first (they survive calls; the JIT entry saves them), then volatile
		// v4-v19. v0-v3 are scratch, v29-v31 hold constants.
		static const int allocationOrder[] = {
			V20, V21, V22, V23, V24, V25, V26, V27, V28,
			V4, V5, V6, V7, V8, V9, V10, V11, V12, V13, V14, V15, V16, V17, V18, V19,
		};
		// v21-v28 hold static FPRs.
		static const int allocationOrderStatic[] = {
			V20, V4, V5, V6, V7, V8, V9, V10, V11, V12, V13, V14, V15, V16, V17, V18, V19,
		};
		if (jo_->useStaticAlloc && MIPSComp::g_ppcJitStaticFPR) {
			count = ARRAY_SIZE(allocationOrderStatic);
			return allocationOrderStatic;
		}
		count = ARRAY_SIZE(allocationOrder);
		return allocationOrder;
	}
	_assert_msg_(false, "Allocation order not implemented");
	count = 0;
	return nullptr;
}

// The most used MIPS registers stay in callee-saved host registers between blocks. Otherwise each
// block stores them to MIPSState and the next one (often a few dozen cycles later, through a link)
// loads them back: a load-hit-store stall on the PS3's PPU for every register handed over.
const PPCRegCache::StaticAllocation *PPCRegCache::GetStaticAllocations(int &count) const {
	static const StaticAllocation allocs[] = {
		{ MIPS_REG_SP, R18, MIPSLoc::REG },
		{ MIPS_REG_RA, R19, MIPSLoc::REG },
		{ MIPS_REG_V0, R20, MIPSLoc::REG },
		{ MIPS_REG_V1, R21, MIPSLoc::REG },
		{ MIPS_REG_A0, R22, MIPSLoc::REG },
		{ MIPS_REG_A1, R23, MIPSLoc::REG },
		{ MIPS_REG_A2, R24, MIPSLoc::REG },
		{ MIPS_REG_A3, R25, MIPSLoc::REG },
		{ MIPS_REG_S0, R26, MIPSLoc::REG },
		{ MIPS_REG_S1, R27, MIPSLoc::REG },
		// Leaf loops (copies, searches) mostly run on the temporaries.
		{ MIPS_REG_T0, R14, MIPSLoc::REG },
		{ MIPS_REG_T1, R15, MIPSLoc::REG },
		{ MIPS_REG_T2, R16, MIPSLoc::REG },
		{ MIPS_REG_T3, R17, MIPSLoc::REG },
		{ MIPS_REG_S2, R29, MIPSLoc::REG },
		// Volatile: calls that don't save the statics (FSpecial) save this one around them.
		{ MIPS_REG_S3, R2, MIPSLoc::REG },
	};
	// AltiVec mode: the most used FPU registers too (float code hands values between blocks in
	// them; each store and reload through MIPSState was a stall). Callee-saved v21-v28.
	static const StaticAllocation allocsVMX[] = {
		{ MIPS_REG_SP, R18, MIPSLoc::REG },
		{ MIPS_REG_RA, R19, MIPSLoc::REG },
		{ MIPS_REG_V0, R20, MIPSLoc::REG },
		{ MIPS_REG_V1, R21, MIPSLoc::REG },
		{ MIPS_REG_A0, R22, MIPSLoc::REG },
		{ MIPS_REG_A1, R23, MIPSLoc::REG },
		{ MIPS_REG_A2, R24, MIPSLoc::REG },
		{ MIPS_REG_A3, R25, MIPSLoc::REG },
		{ MIPS_REG_S0, R26, MIPSLoc::REG },
		{ MIPS_REG_S1, R27, MIPSLoc::REG },
		{ MIPS_REG_T0, R14, MIPSLoc::REG },
		{ MIPS_REG_T1, R15, MIPSLoc::REG },
		{ MIPS_REG_T2, R16, MIPSLoc::REG },
		{ MIPS_REG_T3, R17, MIPSLoc::REG },
		{ MIPS_REG_S2, R29, MIPSLoc::REG },
		{ MIPS_REG_S3, R2, MIPSLoc::REG },
		{ 32 + 0, V21, MIPSLoc::VREG },
		{ 32 + 1, V22, MIPSLoc::VREG },
		{ 32 + 2, V23, MIPSLoc::VREG },
		{ 32 + 12, V24, MIPSLoc::VREG },
		{ 32 + 13, V25, MIPSLoc::VREG },
		{ 32 + 14, V26, MIPSLoc::VREG },
		{ 32 + 15, V27, MIPSLoc::VREG },
		{ 32 + 16, V28, MIPSLoc::VREG },
	};
	if (jo_->useStaticAlloc) {
		if (vmx_ && MIPSComp::g_ppcJitStaticFPR) {
			count = ARRAY_SIZE(allocsVMX);
			return allocsVMX;
		}
		count = ARRAY_SIZE(allocs);
		return allocs;
	}
	return IRNativeRegCacheBase::GetStaticAllocations(count);
}

void PPCRegCache::Save(Snapshot &s) const {
	memcpy((void *)s.nr, (const void *)nr, sizeof(s.nr));
	memcpy((void *)s.mr, (const void *)mr, sizeof(s.mr));
}

void PPCRegCache::Restore(const Snapshot &s) {
	memcpy((void *)nr, (const void *)s.nr, sizeof(s.nr));
	memcpy((void *)mr, (const void *)s.mr, sizeof(s.mr));
}

void PPCRegCache::FlushAllFast() {
	DiscardDeadTempsAtExit();
	if (!mr[MIPS_REG_ZERO].isStatic && mr[MIPS_REG_ZERO].nReg != -1)
		DiscardNativeReg(mr[MIPS_REG_ZERO].nReg);
	for (int n = 0; n < config_.totalNativeRegs; ++n) {
		const IRReg m = nr[n].mipsReg;
		if (m != IRREG_INVALID && m != MIPS_REG_ZERO && !mr[m].isStatic)
			FlushNativeReg(n);
	}
	for (int i = 1; i < TOTAL_MAPPABLE_IRREGS; ++i) {
		// Only GPR-type registers can be immediates: MIPS GPRs, IR temps and VFPU control (192-223),
		// LO/HI and the like (240 up).
		if (i == 32)
			i = 192;
		else if (i == 224)
			i = 240;
		RegStatusMIPS &m = mr[i];
		if (m.loc == MIPSLoc::IMM) {
			if (m.isStatic) {
				SetNativeRegValue(m.nReg, m.imm);
				m.loc = MIPSLoc::REG;
				nr[m.nReg].pointerified = false;
			} else {
				FlushReg((IRReg)i);
			}
		} else if (m.loc == MIPSLoc::REG_IMM && m.isStatic) {
			m.loc = MIPSLoc::REG;
		}
	}
	int count = 0;
	const StaticAllocation *allocs = GetStaticAllocations(count);
	for (int i = 0; i < count; ++i)
		nr[allocs[i].nr].pointerified = false;
}

void PPCRegCache::EmitFlushStores() {
	for (int n = 0; n < config_.totalNativeRegs; ++n) {
		const IRReg first = nr[n].mipsReg;
		if (first == IRREG_INVALID || first == MIPS_REG_ZERO || mr[first].isStatic || !nr[n].isDirty)
			continue;
		int lanes = 0;
		for (IRReg m = first; m < IRREG_INVALID && mr[m].nReg == n; ++m)
			lanes++;
		StoreNativeReg(n, first, lanes);
	}
	for (int i = 1; i < TOTAL_MAPPABLE_IRREGS; ++i) {
		if (i == 32)
			i = 192;
		else if (i == 224)
			i = 240;
		const RegStatusMIPS &m = mr[i];
		if (m.loc != MIPSLoc::IMM)
			continue;
		if (m.isStatic)
			SetNativeRegValue(m.nReg, m.imm);
		else
			StoreRegValue((IRReg)i, m.imm);
	}
}

void PPCRegCache::FlushBeforeCall() {
	// Volatile GPRs we allocate: r5-r10. Volatile FPRs: f1-f12.
	for (int i = R5; i <= R10; ++i)
		FlushNativeReg(i);
	for (int i = F1; i <= F12; ++i)
		FlushNativeReg(i);
	if (vmx_) {
		for (int i = V4; i <= V19; ++i)
			FlushNativeReg(i);
	}
}

PPCReg PPCRegCache::GetAndLockTempGPR() {
	PPCReg reg = (PPCReg)AllocateReg(MIPSLoc::REG, MIPSMap::INIT);
	if (reg != INVALID_REG && (int)reg != -1)
		nr[reg].tempLockIRIndex = irIndex_;
	return reg;
}

PPCReg PPCRegCache::MapGPR(IRReg mipsReg, MIPSMap mapFlags) {
	_dbg_assert_(IsValidGPR(mipsReg));
	IRNativeReg nreg = MapNativeReg(MIPSLoc::REG, mipsReg, 1, mapFlags);
	return (PPCReg)nreg;
}

PPCReg PPCRegCache::MapFPR(IRReg mipsReg, MIPSMap mapFlags) {
	_dbg_assert_(IsValidFPR(mipsReg));
	IRNativeReg nreg = MapNativeReg(MIPSLoc::FREG, mipsReg + 32, 1, mapFlags);
	if (nreg != -1)
		return (PPCReg)nreg;
	return INVALID_REG;
}

void PPCRegCache::AdjustNativeRegAsPtr(IRNativeReg nreg, bool state) {
	// Pointer mapping isn't used by this backend.
	_assert_msg_(false, "PPC: pointer mapping not supported");
}

bool PPCRegCache::IsNativeRegCompatible(IRNativeReg nreg, MIPSLoc type, MIPSMap flags, int lanes) {
	if (type == MIPSLoc::VREG)
		return nreg >= V0 && nreg <= V31 && (lanes == 1 || lanes == 4);
	if (lanes != 1)
		return false;
	if (type == MIPSLoc::REG)
		return nreg >= 0 && nreg < 32;
	if (type == MIPSLoc::FREG)
		return nreg >= 32 && nreg < 64;
	return false;
}

void PPCRegCache::LoadNativeReg(IRNativeReg nreg, IRReg first, int lanes) {
	const int offset = GetMipsRegOffset(first);
	if (nreg >= V0) {
		// r0 as the index (rb is a real register; only ra = 0 means zero).
		emit_->LI(R0, (s16)offset);
		if (lanes == 4) {
			_assert_msg_((offset & 15) == 0, "PPC: unaligned Vec4 group");
			emit_->LVX((PPCReg)nreg, CTXREG, R0);
		} else {
			// lvewx loads the word into lane (address & 15) / 4; move it to lane 0.
			emit_->LVEWX((PPCReg)nreg, CTXREG, R0);
			const int lane = (offset >> 2) & 3;
			if (lane != 0)
				emit_->VSLDOI((PPCReg)nreg, (PPCReg)nreg, (PPCReg)nreg, lane * 4);
		}
		return;
	}
	_assert_msg_(lanes == 1, "PPC: multi-lane loads not supported");
	if (nreg < 32) {
		emit_->LWZ((PPCReg)nreg, CTXREG, (s16)offset);
		nr[nreg].normalized32 = true;
	} else {
		emit_->LFS((PPCReg)nreg, CTXREG, (s16)offset);
	}
}

void PPCRegCache::StoreNativeReg(IRNativeReg nreg, IRReg first, int lanes) {
	const int offset = GetMipsRegOffset(first);
	if (nreg >= V0) {
		emit_->LI(R0, (s16)offset);
		if (lanes == 4) {
			_assert_msg_((offset & 15) == 0, "PPC: unaligned Vec4 group");
			emit_->STVX((PPCReg)nreg, CTXREG, R0);
		} else {
			// stvewx stores lane (address & 15) / 4: splat lane 0 there (v3 is the regcache's own scratch).
			emit_->VSPLTW(V3, (PPCReg)nreg, 0);
			emit_->STVEWX(V3, CTXREG, R0);
		}
		return;
	}
	_assert_msg_(lanes == 1, "PPC: multi-lane stores not supported");
	if (nreg < 32)
		emit_->STW((PPCReg)nreg, CTXREG, (s16)offset);
	else
		emit_->STFS((PPCReg)nreg, CTXREG, (s16)offset);
}

void PPCRegCache::SetNativeRegValue(IRNativeReg nreg, uint32_t imm) {
	_dbg_assert_(nreg < 32);
	emit_->LI32((PPCReg)nreg, imm);
}

void PPCRegCache::StoreRegValue(IRReg mreg, uint32_t imm) {
	_assert_(IsValidGPRNoZero(mreg));
	PPCReg storeReg = INVALID_REG;
	for (int i = 0; i < TOTAL_MAPPABLE_IRREGS; ++i) {
		if (mr[i].loc == MIPSLoc::REG_IMM && mr[i].imm == imm) {
			storeReg = (PPCReg)mr[i].nReg;
			break;
		}
	}
	if (storeReg == INVALID_REG) {
		emit_->LI32(SCRATCH3, imm);
		storeReg = SCRATCH3;
	}
	emit_->STW(storeReg, CTXREG, (s16)GetMipsRegOffset(mreg));
}

PPCReg PPCRegCache::R(IRReg mipsReg) {
	_dbg_assert_(IsValidGPR(mipsReg));
	if (mr[mipsReg].loc == MIPSLoc::REG || mr[mipsReg].loc == MIPSLoc::REG_IMM)
		return (PPCReg)mr[mipsReg].nReg;
	ERROR_LOG(Log::JIT, "PPC: reg %i not in a native register", mipsReg);
	_assert_msg_(false, "PPC: reg not mapped");
	return INVALID_REG;
}

// Lane moves between a Vec4 group and scalars, in registers (the base class would store the
// register and load it back, a load-hit-store stall on the PPU). v2 and v3 are free here: ops map
// their registers before using their scratch registers.
bool PPCRegCache::TransferNativeReg(IRNativeReg nreg, IRNativeReg dest, MIPSLoc type, IRReg first, int lanes, MIPSMap flags) {
	if (vmx_ && type == MIPSLoc::VREG && !mr[nr[nreg].mipsReg].isStatic && (flags == MIPSMap::INIT || flags == MIPSMap::DIRTY)) {
		if (dest == -1)
			dest = nreg;
		IRReg oldfirst = nr[nreg].mipsReg;
		int oldlanes = 0;
		while (oldfirst + oldlanes < TOTAL_MAPPABLE_IRREGS && mr[oldfirst + oldlanes].nReg == nreg)
			oldlanes++;
		if (lanes == 1 && oldlanes == 4 && TransferVecTo1(nreg, dest, first))
			return true;
		if (lanes == 4 && oldlanes == 1 && Transfer1ToVec(dest, first))
			return true;
	}
	return IRNativeRegCacheBase::TransferNativeReg(nreg, dest, type, first, lanes, flags);
}

bool PPCRegCache::TransferVecTo1(IRNativeReg nreg, IRNativeReg dest, IRReg first) {
	const IRReg oldfirst = nr[nreg].mipsReg;
	const int k = first - oldfirst;
	if (k < 0 || k > 3)
		return false;
	// Write the group back if it changed, so the other lanes can simply be dropped.
	if (nr[nreg].isDirty)
		StoreNativeReg(nreg, oldfirst, 4);
	// The other lanes go to free registers too when there are some (clean, memory has them): read
	// back from memory right after the store above, they'd each be a load-hit-store stall (Killzone's
	// min/max box tests use all lanes of a loaded vector one by one).
	IRNativeReg laneReg[4] = { -1, -1, -1, -1 };
	for (int i = 0; i < 4; ++i) {
		if (i == k)
			continue;
		const IRNativeReg free = FindFreeReg(MIPSLoc::VREG, MIPSMap::INIT);
		if (free == -1 || free == dest)
			break;
		nr[free].mipsReg = oldfirst + i;  // Taken, so FindFreeReg moves on.
		laneReg[i] = free;
		if (i != 0)
			emit_->VSLDOI((PPCReg)free, (PPCReg)nreg, (PPCReg)nreg, i * 4);
		else
			emit_->VOR((PPCReg)free, (PPCReg)nreg, (PPCReg)nreg);
	}
	if (k != 0)
		emit_->VSLDOI((PPCReg)dest, (PPCReg)nreg, (PPCReg)nreg, k * 4);
	else if (dest != nreg)
		emit_->VOR((PPCReg)dest, (PPCReg)nreg, (PPCReg)nreg);
	for (int i = 0; i < 4; ++i) {
		RegStatusMIPS &m = mr[oldfirst + i];
		if (oldfirst + i == first) {
			m.nReg = dest;
			m.lane = -1;
			m.loc = MIPSLoc::VREG;
		} else if (laneReg[i] != -1) {
			m.nReg = laneReg[i];
			m.lane = -1;
			m.loc = MIPSLoc::VREG;
			nr[laneReg[i]].mipsReg = oldfirst + i;
			nr[laneReg[i]].isDirty = false;
		} else {
			m.nReg = -1;
			m.lane = -1;
			m.loc = MIPSLoc::MEM;
		}
	}
	if (dest != nreg) {
		nr[nreg].mipsReg = IRREG_INVALID;
		nr[nreg].isDirty = false;
	}
	nr[dest].mipsReg = first;
	nr[dest].isDirty = false;
	return true;
}

bool PPCRegCache::Transfer1ToVec(IRNativeReg dest, IRReg first) {
	if ((first & 3) != 0)
		return false;
	IRNativeReg src[4];
	bool dirty = false;
	// Frontend 103: one lane may be in memory (not in any register): it's loaded on the way
	// (lvewx puts it at its own lane, a rotate brings it to lane 0) instead of all four going
	// through memory, a store and a load right after it - a load-hit-store stall.
	int memLane = -1;
	for (int i = 0; i < 4; ++i) {
		const RegStatusMIPS &m = mr[first + i];
		if (m.isStatic)
			return false;
		if (m.loc == MIPSLoc::MEM && m.nReg == -1 && memLane < 0) {
			memLane = i;
			src[i] = -1;
			continue;
		}
		// The others must be scalars in VMX registers, and not also in use by this instruction.
		if (m.loc != MIPSLoc::VREG || m.nReg == -1 || m.lane != -1)
			return false;
		if (i != 0 && m.spillLockIRIndex >= irIndex_)
			return false;
		src[i] = m.nReg;
		dirty = dirty || nr[src[i]].isDirty;
	}
	for (int i = 0; i < 4; ++i)
		for (int j = i + 1; j < 4; ++j)
			if (src[i] != -1 && src[i] == src[j])
				return false;
	auto loadLane = [&](PPCReg tmp) {
		const int offset = GetMipsRegOffset(first + memLane);
		emit_->LI(R0, (s16)offset);
		emit_->LVEWX(tmp, CTXREG, R0);
		if ((offset & 15) != 0)
			emit_->VSLDOI(tmp, tmp, tmp, offset & 15);
	};
	// [a0 c0 a1 c1], [b0 d0 b1 d1] -> [a0 b0 c0 d0].
	if (memLane == 0 || memLane == 2) {
		emit_->VMRGHW(V2, (PPCReg)src[1], (PPCReg)src[3]);
		loadLane(V3);
		emit_->VMRGHW(V3, memLane == 0 ? V3 : (PPCReg)src[0], memLane == 2 ? V3 : (PPCReg)src[2]);
	} else if (memLane == 1 || memLane == 3) {
		emit_->VMRGHW(V3, (PPCReg)src[0], (PPCReg)src[2]);
		loadLane(V2);
		emit_->VMRGHW(V2, memLane == 1 ? V2 : (PPCReg)src[1], memLane == 3 ? V2 : (PPCReg)src[3]);
	} else {
		emit_->VMRGHW(V3, (PPCReg)src[0], (PPCReg)src[2]);
		emit_->VMRGHW(V2, (PPCReg)src[1], (PPCReg)src[3]);
	}
	emit_->VMRGHW((PPCReg)dest, V3, V2);
	for (int i = 0; i < 4; ++i) {
		if (src[i] != -1 && src[i] != dest) {
			nr[src[i]].mipsReg = IRREG_INVALID;
			nr[src[i]].isDirty = false;
		}
		mr[first + i].nReg = dest;
		mr[first + i].lane = i;
		mr[first + i].loc = MIPSLoc::VREG;
	}
	nr[dest].mipsReg = first;
	nr[dest].isDirty = dirty;
	return true;
}

PPCReg PPCRegCache::V(IRReg fpr) {
	_dbg_assert_(IsValidFPR(fpr));
	if (mr[fpr + 32].loc == MIPSLoc::VREG)
		return (PPCReg)mr[fpr + 32].nReg;
	ERROR_LOG(Log::JIT, "PPC: fpr %i not in a VMX register", fpr);
	_assert_msg_(false, "PPC: fpr not in a VMX register");
	return INVALID_REG;
}

PPCReg PPCRegCache::MapVR(IRReg fpr, int lanes, MIPSMap mapFlags) {
	_dbg_assert_(IsValidFPR(fpr));
	if (lanes > 1) {
		for (int k = 0; k < lanes; ++k) {
			if (mr[fpr + 32 + k].isStatic) {
				staticMismatch = true;
				return V0;  // The block's code is thrown away.
			}
		}
	}
	IRNativeReg nreg = MapNativeReg(MIPSLoc::VREG, fpr + 32, lanes, mapFlags);
	return nreg == -1 ? INVALID_REG : (PPCReg)nreg;
}

int PPCRegCache::VRLanes(IRReg fpr) const {
	const IRNativeReg nreg = mr[fpr + 32].nReg;
	if (mr[fpr + 32].loc != MIPSLoc::VREG || nreg < 0)
		return 0;
	int lanes = 0;
	for (IRReg m = nr[nreg].mipsReg; m < IRREG_INVALID && mr[m].nReg == nreg; ++m)
		lanes++;
	return lanes;
}

void PPCRegCache::MarkVRDirty(IRReg fpr) {
	const IRNativeReg nreg = mr[fpr + 32].nReg;
	if (nreg >= 0)
		nr[nreg].isDirty = true;
}

PPCReg PPCRegCache::F(IRReg mipsReg) {
	_dbg_assert_(IsValidFPR(mipsReg));
	if (mr[mipsReg + 32].loc == MIPSLoc::FREG)
		return (PPCReg)mr[mipsReg + 32].nReg;
	ERROR_LOG(Log::JIT, "PPC: fpr %i not in a native register", mipsReg);
	_assert_msg_(false, "PPC: fpr not mapped");
	return INVALID_REG;
}

#endif
