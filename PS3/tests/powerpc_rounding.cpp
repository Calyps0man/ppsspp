// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
// Execute the production FPSCR helper on big-endian Linux PowerPC under QEMU.
// This checks instruction semantics, not native PS3 execution or flush-to-zero.
#include <cassert>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include "Common/Math/PowerPCRounding.h"

static uint64_t ReadFPSCR() {
	double status;
	asm volatile ("mffs %0" : "=f" (status));
	uint64_t bits;
	memcpy(&bits, &status, sizeof(bits));
	return bits;
}

static double Add(double a, double b) {
	double result;
	asm volatile ("fadd %0,%1,%2" : "=f" (result) : "f" (a), "f" (b));
	return result;
}

int main() {
	const int expected[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
	const double nextPositive = std::nextafter(1.0, 2.0);
	const double nextNegative = std::nextafter(-1.0, -2.0);
	const double halfUlp = std::ldexp(1.0, -53);
	for (unsigned mode = 0; mode < 4; ++mode) {
		std::feraiseexcept(FE_INVALID | FE_INEXACT);
		uint64_t before = ReadFPSCR();
		SetPowerPCRoundingMode(mode);
		uint64_t after = ReadFPSCR();
		assert((after & 3) == mode);
		assert((before & ~uint64_t{3}) == (after & ~uint64_t{3}));
		assert(std::fegetround() == expected[mode]);
		assert(Add(1.0, halfUlp) == (mode == 2 ? nextPositive : 1.0));
		assert(Add(-1.0, -halfUlp) == (mode == 3 ? nextNegative : -1.0));
	}
	SetPowerPCRoundingMode(0);
	assert(std::fegetround() == FE_TONEAREST);
	puts("PowerPC FPSCR: four MIPS rounding modes, signed arithmetic, preserved status and restore PASS (QEMU, not PS3)");
}
