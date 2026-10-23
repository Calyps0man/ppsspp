// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "Common/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/PPCEmitter.h"

namespace PPCGen {

void PPCEmitter::Write32(u32 value) {
	// Big-endian host and target: store the word as is.
	memcpy(writable_, &value, 4);
	writable_ += 4;
	code_ += 4;
}

void PPCEmitter::ReserveCodeSpace(u32 bytes) {
	_assert_msg_((bytes & 3) == 0, "Code space should be aligned");
	for (u32 i = 0; i < bytes / 4; i++)
		NOP();
}

const u8 *PPCEmitter::AlignCode16() {
	int c = int((uintptr_t)code_ & 15);
	if (c)
		ReserveCodeSpace(16 - c);
	return code_;
}

const u8 *PPCEmitter::AlignCodePage() {
	// 4 KB is plenty for our purposes (no page protection changes on PS3).
	int c = int((uintptr_t)code_ & 4095);
	if (c)
		ReserveCodeSpace(4096 - c);
	return code_;
}

bool PPCEmitter::BInRange(const u8 *dst) const {
	ptrdiff_t distance = dst - code_;
	return distance >= -0x2000000 && distance < 0x2000000;
}

bool PPCEmitter::BCInRange(const u8 *dst) const {
	ptrdiff_t distance = dst - code_;
	return distance >= -0x8000 && distance < 0x8000;
}

// BO/BI for a condition on a CR field. BO 12 = branch if bit set, 4 = branch if clear.
static void CondBits(Cond cond, PPCCR cr, u32 *bo, u32 *bi) {
	const u32 base = (u32)cr * 4;
	switch (cond) {
	case Cond::LT: *bo = 12; *bi = base + 0; break;
	case Cond::GE: *bo = 4;  *bi = base + 0; break;
	case Cond::GT: *bo = 12; *bi = base + 1; break;
	case Cond::LE: *bo = 4;  *bi = base + 1; break;
	case Cond::EQ: *bo = 12; *bi = base + 2; break;
	case Cond::NE: *bo = 4;  *bi = base + 2; break;
	case Cond::SO: *bo = 12; *bi = base + 3; break;
	case Cond::NS: *bo = 4;  *bi = base + 3; break;
	}
}

FixupBranch PPCEmitter::B() {
	FixupBranch b;
	b.ptr = writable_;
	b.code = code_;
	b.conditional = false;
	Write32(18u << 26);
	return b;
}

FixupBranch PPCEmitter::BC(Cond cond, PPCCR cr) {
	u32 bo, bi;
	CondBits(cond, cr, &bo, &bi);
	FixupBranch b;
	b.ptr = writable_;
	b.code = code_;
	b.conditional = true;
	Write32((16u << 26) | (bo << 21) | (bi << 16));
	return b;
}

void PPCEmitter::B(const u8 *dst) {
	_assert_msg_(BInRange(dst), "B out of range");
	ptrdiff_t distance = dst - code_;
	Write32((18u << 26) | ((u32)distance & 0x03FFFFFC));
}

void PPCEmitter::BL(const u8 *dst) {
	_assert_msg_(BInRange(dst), "BL out of range");
	ptrdiff_t distance = dst - code_;
	Write32((18u << 26) | ((u32)distance & 0x03FFFFFC) | 1);
}

void PPCEmitter::BC(Cond cond, const u8 *dst, PPCCR cr) {
	if (BCInRange(dst)) {
		u32 bo, bi;
		CondBits(cond, cr, &bo, &bi);
		ptrdiff_t distance = dst - code_;
		Write32((16u << 26) | (bo << 21) | (bi << 16) | ((u32)distance & 0xFFFC));
	} else {
		// Inverted short branch over a long one.
		static const Cond inverse[] = {Cond::GE, Cond::LT, Cond::LE, Cond::GT, Cond::NE, Cond::EQ, Cond::NS, Cond::SO};
		FixupBranch skip = BC(inverse[(int)cond], cr);
		B(dst);
		SetJumpTarget(skip);
	}
}

void PPCEmitter::SetJumpTarget(const FixupBranch &branch) {
	SetJumpTarget(branch, code_);
}

void PPCEmitter::SetJumpTarget(const FixupBranch &branch, const u8 *dst) {
	ptrdiff_t distance = dst - branch.code;
	u32 inst;
	memcpy(&inst, branch.ptr, 4);
	if (branch.conditional) {
		_assert_msg_(distance >= -0x8000 && distance < 0x8000, "Conditional branch out of range");
		inst = (inst & ~0xFFFCu) | ((u32)distance & 0xFFFC);
	} else {
		_assert_msg_(distance >= -0x2000000 && distance < 0x2000000, "Branch out of range");
		inst = (inst & ~0x03FFFFFCu) | ((u32)distance & 0x03FFFFFC);
	}
	memcpy(branch.ptr, &inst, 4);
}

void PPCEmitter::LI32(PPCReg rt, u32 value) {
	const s32 sv = (s32)value;
	if (sv >= -32768 && sv <= 32767) {
		LI(rt, (s16)sv);
		return;
	}
	LIS(rt, (s16)(value >> 16));
	if (value & 0xFFFF)
		ORI(rt, rt, (u16)(value & 0xFFFF));
}

void PPCEmitter::LIPTR(PPCReg rt, uintptr_t value) {
	if ((u64)value > 0xFFFFFFFFull) {
		LI64(rt, (u64)value);
		return;
	}
	const u32 v = (u32)value;
	if (v < 0x8000) {
		LI(rt, (s16)v);
		return;
	}
	LIS(rt, (s16)(v >> 16));
	if (v & 0xFFFF)
		ORI(rt, rt, (u16)(v & 0xFFFF));
	if (v & 0x80000000)
		CLRLDI(rt, rt, 32);
}

void PPCEmitter::LI64(PPCReg rt, u64 value) {
	if ((value >> 32) == 0) {
		const u32 v = (u32)value;
		if (v < 0x8000) {
			LI(rt, (s16)v);
		} else {
			LIS(rt, (s16)(v >> 16));
			if (v & 0xFFFF)
				ORI(rt, rt, (u16)(v & 0xFFFF));
			if (v & 0x80000000)
				CLRLDI(rt, rt, 32);
		}
		return;
	}
	LI32(rt, (u32)(value >> 32));
	SLDI(rt, rt, 32);
	if (value & 0xFFFF0000)
		ORIS(rt, rt, (u16)((value >> 16) & 0xFFFF));
	if (value & 0xFFFF)
		ORI(rt, rt, (u16)(value & 0xFFFF));
}

void PPCEmitter::CallDescriptor(const void *functionPointer) {
	// ELFv1: a function pointer points to {entry, toc, environment}.
	LIPTR(R12, (uintptr_t)functionPointer);
	LD(R0, R12, 0);
	LD(R2, R12, 8);
	MTCTR(R0);
	BCTRL();
}

void PPCCodeBlock::PoisonMemory(int offset) {
	// Traps, so running stale code stops instead of doing something random.
	u8 *writable = GetWritablePtrFromCodePtr(region) + offset;
	const u32 trap = 0x7FE00008;
	for (size_t i = offset; i + 4 <= region_size; i += 4, writable += 4)
		memcpy(writable, &trap, 4);
}

}  // namespace PPCGen
