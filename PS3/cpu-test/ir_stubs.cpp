// Isolated IR test harness: unsupported subsystem calls fail immediately.
// Used only by isolated tests, including the PS3 CPU test app.
// NEVER link these stubs into the full emulator.
#include <cstdlib>
#include <cstring>
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/Breakpoints.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/MIPS/MIPSVFPUUtils.h"
#include "Core/MIPS/MIPSTracer.h"
#include "Core/MemMap.h"
[[noreturn]] static void unsupported() { std::abort(); }
namespace Memory { u8 *base = nullptr; u32 g_MemorySize = 32 * 1024 * 1024; }
volatile CoreState coreState = CORE_RUNNING_CPU;
MIPSState *currentMIPS = nullptr;
BreakpointManager g_breakpoints;
MIPSTracer mipsTracer;
MIPSState::MIPSState() { std::memset(this, 0, sizeof(*this)); }
MIPSState::~MIPSState() = default;
void CallSyscall(MIPSOpcode) { unsupported(); }
void CallSyscallUnresolvedAtPC(u32) { unsupported(); }
void MIPSInterpret(MIPSState *, MIPSOpcode) { unsupported(); }
const ReplacementTableEntry *GetReplacementFunc(size_t) { unsupported(); }
void Core_BreakException(u32) { unsupported(); }
void Core_MemoryException(u32, u32, u32, MemoryExceptionType, std::string_view) { unsupported(); }
void ApplyHostRoundingMode(const MIPSState *) { unsupported(); }
void RestoreHostRoundingMode() { unsupported(); }
namespace CoreTiming { void ForceCheck(MIPSState *) { unsupported(); } }
BreakAction BreakpointManager::ExecBreakPoint(u32) { unsupported(); }
BreakAction BreakpointManager::ExecOpMemCheck(u32, u32) { unsupported(); }
u32 BreakpointManager::CheckSkipFirst() const { unsupported(); }
#define VFPU_STUB(name) float name(float) { unsupported(); }
VFPU_STUB(vfpu_rsqrt)
VFPU_STUB(vfpu_cos)
VFPU_STUB(vfpu_rcp)
VFPU_STUB(vfpu_sin)
VFPU_STUB(vfpu_asin)
VFPU_STUB(vfpu_exp2)
VFPU_STUB(vfpu_log2)
VFPU_STUB(vfpu_sqrt)
void vfpu_sincos(float, float&, float&) { unsupported(); }
u32 vfpu_h2f(u16) { unsupported(); }
