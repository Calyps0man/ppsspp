#pragma once

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/MemMap.h"

class MIPSState;
struct IRInst;

u32 IRRunBreakpoint(u32 pc);
u32 IRRunMemCheck(u32 pc, u32 addr);
u32 IRInterpret(MIPSState *ms, const IRInst *inst);

template <uint32_t alignment>
u32 RunValidateAddress(u32 pc, u32 addr, u32 isWrite) {
	static_assert(alignment <= 16);
	const auto toss = [&](MemoryExceptionType t) {
		Core_MemoryException(addr, alignment, pc, t);
		return coreState != CORE_RUNNING_CPU ? 1 : 0;
	};

	if (!Memory::IsValidNAlignedAddress<alignment>(addr)) {
		MemoryExceptionType t = isWrite == 1 ? MemoryExceptionType::WRITE_WORD : MemoryExceptionType::READ_WORD;
		if constexpr (alignment > 4)
			t = isWrite ? MemoryExceptionType::WRITE_BLOCK : MemoryExceptionType::READ_BLOCK;
		return toss(t);
	}

	if constexpr (alignment > 1)
		if ((addr & (alignment - 1)) != 0)
			return toss(MemoryExceptionType::ALIGNMENT);
	return 0;
}

namespace MIPSComp {
// Store watch (diagnostics): see IRJit.cpp.
struct StoreWatchRec {
	u32 block;
	u32 addr;
	u32 value;
	u32 kind;  // 2 = 16-bit store, 'L'/'R' = swl/swr.
};
static constexpr u32 STORE_WATCH_RING = 32768;
extern bool g_irStoreWatch;
extern StoreWatchRec *g_irStoreRing;
extern u32 g_irStoreRingPos;
void IRStoreWatchEnable(bool on);
void IRStoreWatchNote(u32 addr, u32 value, u8 kind);
}
