// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>

// PowerPC FPSCR RN encodes nearest, toward zero, toward +infinity and toward
// -infinity as 0..3, matching MIPS FCR31. PSL1GHT lacks the C fenv API.
// mtfsf field mask 1 changes only the low nibble; preserve its other bits.
// https://www.ibm.com/docs/en/aix/7.1.0?topic=set-mtfsf-move-fpscr-fields-instruction
// flushToZero sets FPSCR NI (non-IEEE mode): denormal results become zero, as the PSP's FPU does
// with FCR31 FS set (most games set it).
inline void SetPowerPCRoundingMode(unsigned mode, bool flushToZero = false) {
	double status;
	asm volatile ("mffs %0" : "=f" (status));
	uint64_t bits;
	memcpy(&bits, &status, sizeof(bits));
	bits = (bits & ~uint64_t{7}) | (flushToZero ? 4 : 0) | (mode & 3);
	memcpy(&status, &bits, sizeof(status));
	asm volatile ("mtfsf 1,%0" : : "f" (status) : "memory");
}
