// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fixed code: entry/exit, the dispatcher (PSP pc -> compiled block via the emuhack opcode
// written into PSP memory), the outer loop calling CoreTiming, and the crash handler.

#include "ppsspp_config.h"
#include "Core/MIPS/PPC/PPCBuild.h"
#ifdef PPSSPP_PPC_JIT_BUILD

#include <cstddef>
#include "Common/Log.h"
#include "Core/Core.h"
#include "Core/MemMap.h"
#include "Core/CoreTiming.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/PPC/PPCJit.h"
#include "Core/MIPS/PPC/PPCRegCache.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "Core/MIPS/JitCommon/JitState.h"

namespace MIPSComp {

using namespace PPCGen;
using namespace PPCJitConstants;

static_assert(offsetof(MIPSState, pc) < 32768, "pc offset");
static_assert(offsetof(MIPSState, downcount) < 32768, "downcount offset");

// ELFv1 function descriptor for the generated entry point, so C++ can call it.
static u64 s_enterDescriptor[3];

static void RestoreRoundingHelper() {
	RestoreHostRoundingMode();
}
static void ApplyRoundingHelper() {
	ApplyHostRoundingMode(currentMIPS);
}
static void AdvanceHelper() {
	CoreTiming::Advance(currentMIPS);
}
// Outside main RAM: 1 if the PC is valid code memory (scratchpad etc.), otherwise raises the
// PSP exception (as the IR interpreter does) and returns 0.
static u32 CheckPCHelper() {
	const u32 pc = currentMIPS->pc;
	if (Memory::IsValid4AlignedAddress(pc))
		return 1;
	Core_ExecException(pc, pc, ExecExceptionType::JUMP);
	return 0;
}
static void JitAtHelper() {
	MIPSComp::JitAt(currentMIPS);
}
static PPCJitBackend *s_backend = nullptr;
const void *PPCJitVmxSwapConstants();
static void ReturnICMissHelper(u32 index) {
	s_backend->PatchReturnIC((int)index, currentMIPS->pc);
}
static u32 TierRunHelper(u32 block_num) {
	return s_backend->TierRun((int)block_num);
}

// Stack frame: 112 bytes ABI header/parameter area, r14-r31, f14-f31, saved r2, VRSAVE, and
// (16-aligned) v20-v31 for the AltiVec mode.
static constexpr int FRAME_GPR_OFFSET = 112;
static constexpr int FRAME_FPR_OFFSET = FRAME_GPR_OFFSET + 18 * 8;
static constexpr int FRAME_TOC_OFFSET = FRAME_FPR_OFFSET + 18 * 8;
static constexpr int FRAME_VRSAVE_OFFSET = FRAME_TOC_OFFSET + 8;
static constexpr int FRAME_VR_OFFSET = (FRAME_VRSAVE_OFFSET + 8 + 15) & ~15;
static constexpr int FRAME_SIZE = FRAME_VR_OFFSET + 12 * 16;

void PPCJitBackend::GenerateFixedCode(MIPSState *mipsState) {
	const u8 *start = AlignCodePage();
	BeginWrite(4096);

	const u8 *enter = AlignCode16();
	MFLR(R0);
	STD(R0, R1, 16);
	STDU(R1, R1, -FRAME_SIZE);
	STD(R2, R1, FRAME_TOC_OFFSET);
	for (int i = 0; i < 18; ++i)
		STD((PPCReg)(R14 + i), R1, (s16)(FRAME_GPR_OFFSET + i * 8));
	for (int i = 0; i < 18; ++i)
		STFD((PPCReg)(F14 + i), R1, (s16)(FRAME_FPR_OFFSET + i * 8));
	if (g_ppcJitVMX) {
		// v20-v31 are callee-saved. VRSAVE: all registers in use (in case the OS saves only
		// those on a switch). VSCR: non-Java mode (NJ, 0x10000): denormal inputs and results are
		// zero, as on the PSP's VFPU and its FPU with FCR31 FS set. (IEEE denormals here made
		// values the game expects to be zero tiny instead: 1/x then overflows, inf * 0 is NaN -
		// Dead to Rights' characters and camera ended up at NaN positions.)
		for (int i = 0; i < 12; ++i) {
			LI(R0, (s16)(FRAME_VR_OFFSET + i * 16));
			STVX((PPCReg)(V20 + i), R1, R0);
		}
		MFSPR(R0, 256);
		STD(R0, R1, FRAME_VRSAVE_OFFSET);
		LI(R0, -1);
		MTSPR(256, R0);
		if (HostFlushDenormals()) {
			VSPLTISW(V0, 1);
			VSPLTISW(V1, 8);
			VADDUWM(V1, V1, V1);
			VSLW(V0, V0, V1);
		} else {
			VSPLTISW(V0, 0);  // Java (IEEE) mode: denormals kept (setting ftz=0).
		}
		MTVSCR(V0);
		LIPTR(R12, (uintptr_t)PPCJitVmxSwapConstants());
		LVX(VSWAPWORDS, R0, R12);
		LI(R0, 16);
		LVX(VLANE0SWAP, R12, R0);
		VSPLTISW(VNEGZERO, -1);
		VSLW(VNEGZERO, VNEGZERO, VNEGZERO);
	}

	LIPTR(MEMBASEREG, (uintptr_t)Memory::base);
	LIPTR(CTXREG, (uintptr_t)mipsState);

	QuickCallFunction(&ApplyRoundingHelper);
	LoadStaticRegisters();
	LWZ(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, pc));

	outerLoopPCInSCRATCH1_ = GetCodePtr();
	STW(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, pc));
	outerLoop_ = GetCodePtr();
	// Advance can change the downcount (or thread), so save/reload around it.
	SaveStaticRegisters();
	QuickCallFunction(&RestoreRoundingHelper);
	QuickCallFunction(&AdvanceHelper);
	QuickCallFunction(&ApplyRoundingHelper);
	LoadStaticRegisters();

	dispatcherCheckCoreState_ = GetCodePtr();
	LIPTR(SCRATCH1, (uintptr_t)&coreState);
	LWZ(SCRATCH1, SCRATCH1, 0);
	CMPWI(SCRATCH1, 0);
	FixupBranch badCoreState = BC(Cond::NE);
	// Just checked coreState: go to Advance if the downcount ran out.
	CMPWI(DOWNCOUNTREG, 0);
	BC(Cond::LT, outerLoop_);
	FixupBranch skipToRealDispatch = B();

	// Every jump through a register (jr ra) comes here with the PC in SCRATCH1. It's stored for
	// the slow paths but not loaded back: a load right after a store to the same word stalls the
	// Cell PPU for ~40 cycles (load-hit-store).
	dispatcherPCInSCRATCH1_ = GetCodePtr();
	STW(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, pc));
	CMPWI(DOWNCOUNTREG, 0);
	FixupBranch bailWithPC = BC(Cond::LT);
	FixupBranch dispatchWithPC = B();

	// A return inline cache missed (PC in SCRATCH1, cache number in SCRATCH2): point it at the
	// new target if that's compiled, then dispatch.
	s_backend = this;
	dispatcherICMiss_ = GetCodePtr();
	STW(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, pc));
	CMPWI(DOWNCOUNTREG, 0);
	FixupBranch bailIC = BC(Cond::LT);
	SaveStaticRegisters();
	MR(SCRATCH1, SCRATCH2);
	QuickCallFunction(&ReturnICMissHelper);
	LoadStaticRegisters();
	FixupBranch icDispatch = B();

	// A block not compiled yet (block number in SCRATCH2): the IR interpreter runs it.
	tierInterp_ = GetCodePtr();
	SaveStaticRegisters();
	MR(SCRATCH1, SCRATCH2);
	QuickCallFunction(&TierRunHelper);
	STW(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, pc));
	LoadStaticRegisters();
	B(dispatcherCheckCoreState_);

	hooks_.dispatcher = GetCodePtr();
	CMPWI(DOWNCOUNTREG, 0);
	FixupBranch bail = BC(Cond::LT);
	SetJumpTarget(skipToRealDispatch);

	dispatcherNoCheck_ = GetCodePtr();
	SetJumpTarget(icDispatch);
	LWZ(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, pc));
	SetJumpTarget(dispatchWithPC);
	CLRLWI(SCRATCH2, SCRATCH1, 2);  // & 0x3FFFFFFF (masked PSP memory)
	// Only main RAM (0x08000000-0x0BFFFFFF masked) is read here directly; anything else (bad
	// jump targets, scratchpad code) goes through JitAt, which checks the address.
	ADDIS(R12, SCRATCH2, -0x0800);
	SRWI(R12, R12, 26);
	CMPLWI(R12, 0);
	FixupBranch notRam = BC(Cond::NE);
	hooks_.dispatchFetch = GetCodePtr();
	LWBRX(SCRATCH1, MEMBASEREG, SCRATCH2);  // The opcode (PSP memory is little-endian).
	SRWI(SCRATCH2, SCRATCH1, 24);
	CMPLWI(SCRATCH2, MIPS_EMUHACK_OPCODE >> 24);
	FixupBranch needsCompile = BC(Cond::NE);
	CLRLWI(SCRATCH1, SCRATCH1, 8);  // Low 24 bits: offset of the block.
	LIPTR(R12, (uintptr_t)GetBasePtr());
	ADD(SCRATCH1, SCRATCH1, R12);
	MTCTR(SCRATCH1);
	BCTR();
	SetJumpTarget(needsCompile);

	// No block yet: compile one.
	SaveStaticRegisters();
	QuickCallFunction(&RestoreRoundingHelper);
	QuickCallFunction(&JitAtHelper);
	QuickCallFunction(&ApplyRoundingHelper);
	LoadStaticRegisters();
	B(dispatcherNoCheck_);

	// PC outside main RAM.
	SetJumpTarget(notRam);
	SaveStaticRegisters();
	QuickCallFunction(&CheckPCHelper);
	LoadStaticRegisters();
	CMPWI(SCRATCH1, 0);
	BC(Cond::EQ, dispatcherCheckCoreState_);
	LWZ(SCRATCH1, CTXREG, (s16)offsetof(MIPSState, pc));
	CLRLWI(SCRATCH2, SCRATCH1, 2);
	B(hooks_.dispatchFetch);

	SetJumpTarget(bail);
	SetJumpTarget(bailWithPC);
	SetJumpTarget(bailIC);
	LIPTR(SCRATCH1, (uintptr_t)&coreState);
	LWZ(SCRATCH1, SCRATCH1, 0);
	CMPWI(SCRATCH1, 0);
	BC(Cond::EQ, outerLoop_);

	const u8 *quitLoop = GetCodePtr();
	SetJumpTarget(badCoreState);
	SaveStaticRegisters();
	QuickCallFunction(&RestoreRoundingHelper);
	for (int i = 0; i < 18; ++i)
		LD((PPCReg)(R14 + i), R1, (s16)(FRAME_GPR_OFFSET + i * 8));
	for (int i = 0; i < 18; ++i)
		LFD((PPCReg)(F14 + i), R1, (s16)(FRAME_FPR_OFFSET + i * 8));
	if (g_ppcJitVMX) {
		for (int i = 0; i < 12; ++i) {
			LI(R0, (s16)(FRAME_VR_OFFSET + i * 16));
			LVX((PPCReg)(V20 + i), R1, R0);
		}
		LD(R0, R1, FRAME_VRSAVE_OFFSET);
		MTSPR(256, R0);
	}
	LD(R2, R1, FRAME_TOC_OFFSET);
	ADDI(R1, R1, FRAME_SIZE);
	LD(R0, R1, 16);
	MTLR(R0);
	BLR();

	hooks_.crashHandler = GetCodePtr();
	LIPTR(SCRATCH1, (uintptr_t)&coreState);
	LI(SCRATCH2, (s16)CORE_RUNTIME_ERROR);
	STW(SCRATCH2, SCRATCH1, 0);
	B(quitLoop);

	// Descriptor for C++ callers: entry, TOC (ours), environment.
	const u64 *own = (const u64 *)(const void *)&RestoreRoundingHelper;
	s_enterDescriptor[0] = (u64)(uintptr_t)enter;
	s_enterDescriptor[1] = own[1];
	s_enterDescriptor[2] = 0;
	hooks_.enterDispatcher = (IRNativeFuncNoArg)(void *)s_enterDescriptor;

	AlignCodePage();
	jitStartOffset_ = (int)(GetCodePtr() - start);
	EndWrite();
	Commit(start, GetCodePtr());
	INFO_LOG(Log::JIT, "PPC JIT: fixed code %d bytes at %p", jitStartOffset_, start);
}

}  // namespace MIPSComp

#endif
