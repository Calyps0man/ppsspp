// Executes PPSSPP's actual IR interpreter, not a reimplementation.
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "Common/Swap.h"
#ifndef COMMON_LITTLE_ENDIAN
#define COMMON_LITTLE_ENDIAN 0
#endif
#include "Core/MIPS/IR/IRInst.h"
#include "Core/MemMap.h"
static int checks = 0, failures = 0;
static void check(bool ok, const char *name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}
static void run(MIPSState &s, IROp op, u8 dest = 0, u8 a = 1, u8 b = 2, u32 offset = 0) {
    const IRInst block[] = {{op, {dest}, a, b, offset}, {IROp::ExitToConst, {0}, 0, 0, 0x12345678}};
    check(IRInterpret(&s, block) == 0x12345678, "IR block exit");
}
static u64 hilo(const MIPSState &s) { return ((u64)s.hi << 32) | s.lo; }
static void arithmetic() {
    MIPSState s;
    for (u32 a : {0U, 1U, 0xFFFFFFFFU, 0x80000000U, 0x7FFFFFFFU, 0x12345678U}) {
        for (u32 b : {0U, 3U, 0xFFFFFFFFU, 0x80000000U, 0xCAFEBABEU}) {
            s.r[1] = a; s.r[2] = b;
            u64 signedProduct = (u64)((s64)(s32)a * (s64)(s32)b);
            u64 unsignedProduct = (u64)a * b;
            run(s, IROp::Mult); check(hilo(s) == signedProduct, "Mult HI/LO");
            run(s, IROp::MultU); check(hilo(s) == unsignedProduct, "MultU HI/LO");
            for (u64 initial : {0ULL, 0xFFFFFFFFFFFFFFFFULL, 0x89ABCDEF01234567ULL}) {
                for (IROp op : {IROp::Madd, IROp::MaddU, IROp::Msub, IROp::MsubU}) {
                    s.lo = (u32)initial; s.hi = (u32)(initial >> 32);
                    u64 product = op == IROp::Madd || op == IROp::Msub ? signedProduct : unsignedProduct;
                    u64 expected = op == IROp::Msub || op == IROp::MsubU ? initial - product : initial + product;
                    run(s, op); check(hilo(s) == expected, "accumulate modulo 64 bits");
                }
            }
        }
    }
}
static void vectors() {
    alignas(16) u8 ram[256] = {};
    Memory::base = ram;
    MIPSState s;
    // Distinct lanes, including signed zero and a NaN payload: preserve bits.
    const u32 bits[4] = {0x3F800000, 0x80000000, 0x7FC12345, 0xC0200000};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) ram[32 + i * 4 + j] = (u8)(bits[i] >> (j * 8));
    s.r[1] = 32;
    run(s, IROp::LoadVec4, 8);
    for (int i = 0; i < 4; ++i) check(s.fi[8 + i] == bits[i], "vector load lane bits");
    s.r[1] = 64;
    run(s, IROp::StoreVec4, 8);
    check(std::memcmp(ram + 32, ram + 64, 16) == 0, "vector store byte order");
    s.r[1] = 32;
    run(s, IROp::Load32, 3); check(s.r[3] == bits[0], "scalar LE read");
    s.r[1] = 96; s.r[3] = 0x89ABCDEF;
    run(s, IROp::Store32, 3);
    const u8 expected[] = {0xEF, 0xCD, 0xAB, 0x89};
    check(std::memcmp(ram + 96, expected, 4) == 0, "scalar LE write");
    Memory::base = nullptr;
}
int RunIRTests(int *checksOut, int *failuresOut) {
    checks=0; failures=0;
    arithmetic(); vectors();
    std::printf("host=%s checks=%d failures=%d\n", COMMON_LITTLE_ENDIAN ? "little-endian" : "big-endian", checks, failures);
    if(checksOut) *checksOut=checks;
    if(failuresOut) *failuresOut=failures;
    return failures ? 1 : 0;
}
#ifndef PPSSPP_PS3_TEST_HARNESS
int main() { return RunIRTests(nullptr,nullptr); }
#endif
