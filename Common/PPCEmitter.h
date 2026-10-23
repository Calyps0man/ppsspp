// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal 64-bit PowerPC (Cell PPU, big-endian, ELFv1 / PSL1GHT ABI) code emitter for the IR
// native JIT. Only what the backend needs: integer ALU, loads/stores (including byte-reversed
// ones for the little-endian PSP memory), branches, a few FPU ops and SPR moves.
#pragma once

#include <cstdint>
#include <cstddef>
#include "Common/CommonTypes.h"
#include "Common/CodeBlock.h"

namespace PPCGen {

enum PPCReg {
	R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, R13, R14, R15,
	R16, R17, R18, R19, R20, R21, R22, R23, R24, R25, R26, R27, R28, R29, R30, R31,
	// FPRs follow in the same numbering space (like the RISC-V backend), so the regcache can
	// treat 0..31 as GPRs and 32..63 as FPRs.
	F0 = 32, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15,
	F16, F17, F18, F19, F20, F21, F22, F23, F24, F25, F26, F27, F28, F29, F30, F31,
	// AltiVec (VMX) registers, 64..95 for the regcache.
	V0 = 64, V1, V2, V3, V4, V5, V6, V7, V8, V9, V10, V11, V12, V13, V14, V15,
	V16, V17, V18, V19, V20, V21, V22, V23, V24, V25, V26, V27, V28, V29, V30, V31,
	INVALID_REG = 0xFFFF,
};

// Condition register fields.
enum PPCCR { CR0 = 0, CR1, CR2, CR3, CR4, CR5, CR6, CR7 };

// Conditions for a conditional branch, after a compare.
enum class Cond {
	LT, GE, GT, LE, EQ, NE, SO, NS,
};

struct FixupBranch {
	u8 *ptr = nullptr;     // Writable pointer to the branch instruction.
	const u8 *code = nullptr;  // Code (exec) address of it.
	bool conditional = false;
};

class PPCEmitter {
public:
	PPCEmitter() {}
	PPCEmitter(const u8 *codePtr, u8 *writablePtr) { SetCodePointer(codePtr, writablePtr); }
	virtual ~PPCEmitter() {}

	void SetCodePointer(const u8 *ptr, u8 *writePtr) { code_ = ptr; writable_ = writePtr; }
	const u8 *GetCodePointer() const { return code_; }
	u8 *GetWritableCodePtr() { return writable_; }

	void ReserveCodeSpace(u32 bytes);  // Fills with nops.
	const u8 *AlignCode16();
	const u8 *AlignCodePage();
	void FlushIcache() {}  // Commit (PS3) does the cache maintenance.
	void FlushIcacheSection(const u8 *start, const u8 *end) {}

	void Write32(u32 value);

	// Branches.
	FixupBranch B();
	FixupBranch BC(Cond cond, PPCCR cr = CR0);
	void B(const u8 *dst);
	void BL(const u8 *dst);
	void BC(Cond cond, const u8 *dst, PPCCR cr = CR0);
	bool BCInRange(const u8 *dst) const;
	bool BInRange(const u8 *dst) const;
	void SetJumpTarget(const FixupBranch &branch);
	void SetJumpTarget(const FixupBranch &branch, const u8 *dst);
	void BLR() { Write32(0x4E800020); }
	void BCTR() { Write32(0x4E800420); }
	void BCTRL() { Write32(0x4E800421); }
	void MTCTR(PPCReg rs) { Write32(0x7C0903A6 | (rs << 21)); }
	void MFCTR(PPCReg rt) { Write32(0x7C0902A6 | (rt << 21)); }
	void MTLR(PPCReg rs) { Write32(0x7C0803A6 | (rs << 21)); }
	void MFLR(PPCReg rt) { Write32(0x7C0802A6 | (rt << 21)); }
	void NOP() { Write32(0x60000000); }
	void TRAP() { Write32(0x7FE00008); }

	// D-form immediates.
	void ADDI(PPCReg rt, PPCReg ra, s16 imm) { D(14, rt, ra, (u16)imm); }
	void ADDIS(PPCReg rt, PPCReg ra, s16 imm) { D(15, rt, ra, (u16)imm); }
	void LI(PPCReg rt, s16 imm) { ADDI(rt, R0, imm); }
	void LIS(PPCReg rt, s16 imm) { ADDIS(rt, R0, imm); }
	void MULLI(PPCReg rt, PPCReg ra, s16 imm) { D(7, rt, ra, (u16)imm); }
	void SUBFIC(PPCReg rt, PPCReg ra, s16 imm) { D(8, rt, ra, (u16)imm); }
	void ORI(PPCReg ra, PPCReg rs, u16 imm) { D(24, rs, ra, imm); }
	void ORIS(PPCReg ra, PPCReg rs, u16 imm) { D(25, rs, ra, imm); }
	void XORI(PPCReg ra, PPCReg rs, u16 imm) { D(26, rs, ra, imm); }
	void XORIS(PPCReg ra, PPCReg rs, u16 imm) { D(27, rs, ra, imm); }
	void ANDI_(PPCReg ra, PPCReg rs, u16 imm) { D(28, rs, ra, imm); }   // andi. (sets CR0)
	void ANDIS_(PPCReg ra, PPCReg rs, u16 imm) { D(29, rs, ra, imm); }  // andis.

	// Compares (32-bit, L=0).
	void CMPWI(PPCReg ra, s16 imm, PPCCR cr = CR0) { Write32((11u << 26) | ((u32)cr << 23) | (ra << 16) | (u16)imm); }
	void CMPLWI(PPCReg ra, u16 imm, PPCCR cr = CR0) { Write32((10u << 26) | ((u32)cr << 23) | (ra << 16) | imm); }
	void CMPW(PPCReg ra, PPCReg rb, PPCCR cr = CR0) { Write32((31u << 26) | ((u32)cr << 23) | (ra << 16) | (rb << 11) | (0 << 1)); }
	void CMPLW(PPCReg ra, PPCReg rb, PPCCR cr = CR0) { Write32((31u << 26) | ((u32)cr << 23) | (ra << 16) | (rb << 11) | (32 << 1)); }
	void CMPD(PPCReg ra, PPCReg rb, PPCCR cr = CR0) { Write32((31u << 26) | ((u32)cr << 23) | (1 << 21) | (ra << 16) | (rb << 11) | (0 << 1)); }
	void CMPLD(PPCReg ra, PPCReg rb, PPCCR cr = CR0) { Write32((31u << 26) | ((u32)cr << 23) | (1 << 21) | (ra << 16) | (rb << 11) | (32 << 1)); }

	// XO-form arithmetic.
	void ADD(PPCReg rt, PPCReg ra, PPCReg rb) { XO(266, rt, ra, rb); }
	void SUBF(PPCReg rt, PPCReg ra, PPCReg rb) { XO(40, rt, ra, rb); }  // rt = rb - ra
	void SUB(PPCReg rt, PPCReg ra, PPCReg rb) { SUBF(rt, rb, ra); }    // rt = ra - rb
	void NEG(PPCReg rt, PPCReg ra) { XO(104, rt, ra, R0); }
	void MULLW(PPCReg rt, PPCReg ra, PPCReg rb) { XO(235, rt, ra, rb); }
	void MULHW(PPCReg rt, PPCReg ra, PPCReg rb) { XO(75, rt, ra, rb); }
	void MULHWU(PPCReg rt, PPCReg ra, PPCReg rb) { XO(11, rt, ra, rb); }
	void MULLD(PPCReg rt, PPCReg ra, PPCReg rb) { XO(233, rt, ra, rb); }
	void DIVW(PPCReg rt, PPCReg ra, PPCReg rb) { XO(491, rt, ra, rb); }
	void DIVWU(PPCReg rt, PPCReg ra, PPCReg rb) { XO(459, rt, ra, rb); }
	void DIVD(PPCReg rt, PPCReg ra, PPCReg rb) { XO(489, rt, ra, rb); }
	void DIVDU(PPCReg rt, PPCReg ra, PPCReg rb) { XO(457, rt, ra, rb); }

	// X-form logical (ra = rs OP rb).
	void AND(PPCReg ra, PPCReg rs, PPCReg rb) { X(28, rs, ra, rb); }
	void ANDC(PPCReg ra, PPCReg rs, PPCReg rb) { X(60, rs, ra, rb); }
	void OR(PPCReg ra, PPCReg rs, PPCReg rb) { X(444, rs, ra, rb); }
	void ORC(PPCReg ra, PPCReg rs, PPCReg rb) { X(412, rs, ra, rb); }
	void XOR(PPCReg ra, PPCReg rs, PPCReg rb) { X(316, rs, ra, rb); }
	void NOR(PPCReg ra, PPCReg rs, PPCReg rb) { X(124, rs, ra, rb); }
	void MR(PPCReg ra, PPCReg rs) { if (ra != rs) OR(ra, rs, rs); }
	void NOT(PPCReg ra, PPCReg rs) { NOR(ra, rs, rs); }
	void SLW(PPCReg ra, PPCReg rs, PPCReg rb) { X(24, rs, ra, rb); }
	void SRW(PPCReg ra, PPCReg rs, PPCReg rb) { X(536, rs, ra, rb); }
	void SRAW(PPCReg ra, PPCReg rs, PPCReg rb) { X(792, rs, ra, rb); }
	void SRAWI(PPCReg ra, PPCReg rs, int sh) { X(824, rs, ra, (PPCReg)(sh & 31)); }
	void SLD(PPCReg ra, PPCReg rs, PPCReg rb) { X(27, rs, ra, rb); }
	void SRD(PPCReg ra, PPCReg rs, PPCReg rb) { X(539, rs, ra, rb); }
	void SRAD(PPCReg ra, PPCReg rs, PPCReg rb) { X(794, rs, ra, rb); }
	void CNTLZW(PPCReg ra, PPCReg rs) { X(26, rs, ra, R0); }
	void EXTSB(PPCReg ra, PPCReg rs) { X(954, rs, ra, R0); }
	void EXTSH(PPCReg ra, PPCReg rs) { X(922, rs, ra, R0); }
	void EXTSW(PPCReg ra, PPCReg rs) { X(986, rs, ra, R0); }

	// Rotates.
	void RLWINM(PPCReg ra, PPCReg rs, int sh, int mb, int me) {
		Write32((21u << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((mb & 31) << 6) | ((me & 31) << 1));
	}
	void RLWNM(PPCReg ra, PPCReg rs, PPCReg rb, int mb, int me) {
		Write32((23u << 26) | (rs << 21) | (ra << 16) | (rb << 11) | ((mb & 31) << 6) | ((me & 31) << 1));
	}
	void RLWIMI(PPCReg ra, PPCReg rs, int sh, int mb, int me) {
		Write32((20u << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((mb & 31) << 6) | ((me & 31) << 1));
	}
	void SLWI(PPCReg ra, PPCReg rs, int n) { RLWINM(ra, rs, n, 0, 31 - n); }
	void SRWI(PPCReg ra, PPCReg rs, int n) { RLWINM(ra, rs, (32 - n) & 31, n, 31); }
	void ROTLWI(PPCReg ra, PPCReg rs, int n) { RLWINM(ra, rs, n, 0, 31); }
	void CLRLWI(PPCReg ra, PPCReg rs, int n) { RLWINM(ra, rs, 0, n, 31); }  // Clear the high n bits (of 32).
	void RLDICL(PPCReg ra, PPCReg rs, int sh, int mb) { MD(0, ra, rs, sh, mb); }
	void RLDICR(PPCReg ra, PPCReg rs, int sh, int me) { MD(1, ra, rs, sh, me); }
	void CLRLDI(PPCReg ra, PPCReg rs, int n) { RLDICL(ra, rs, 0, n); }
	void SLDI(PPCReg ra, PPCReg rs, int n) { RLDICR(ra, rs, n, 63 - n); }
	void SRDI(PPCReg ra, PPCReg rs, int n) { RLDICL(ra, rs, (64 - n) & 63, n); }

	// Loads and stores (D-form: ra + imm; ra = R0 means 0).
	void LWZ(PPCReg rt, PPCReg ra, s16 d) { D(32, rt, ra, (u16)d); }
	void LBZ(PPCReg rt, PPCReg ra, s16 d) { D(34, rt, ra, (u16)d); }
	void LHZ(PPCReg rt, PPCReg ra, s16 d) { D(40, rt, ra, (u16)d); }
	void LHA(PPCReg rt, PPCReg ra, s16 d) { D(42, rt, ra, (u16)d); }
	void STW(PPCReg rs, PPCReg ra, s16 d) { D(36, rs, ra, (u16)d); }
	void STB(PPCReg rs, PPCReg ra, s16 d) { D(38, rs, ra, (u16)d); }
	void STH(PPCReg rs, PPCReg ra, s16 d) { D(44, rs, ra, (u16)d); }
	void LD(PPCReg rt, PPCReg ra, s16 d) { Write32((58u << 26) | (rt << 21) | (ra << 16) | ((u16)d & 0xFFFC)); }
	void STD(PPCReg rs, PPCReg ra, s16 d) { Write32((62u << 26) | (rs << 21) | (ra << 16) | ((u16)d & 0xFFFC)); }
	void STDU(PPCReg rs, PPCReg ra, s16 d) { Write32((62u << 26) | (rs << 21) | (ra << 16) | ((u16)d & 0xFFFC) | 1); }
	void LFS(PPCReg ft, PPCReg ra, s16 d) { D(48, (PPCReg)(ft & 31), ra, (u16)d); }
	void LFD(PPCReg ft, PPCReg ra, s16 d) { D(50, (PPCReg)(ft & 31), ra, (u16)d); }
	void STFS(PPCReg fs, PPCReg ra, s16 d) { D(52, (PPCReg)(fs & 31), ra, (u16)d); }
	void STFD(PPCReg fs, PPCReg ra, s16 d) { D(54, (PPCReg)(fs & 31), ra, (u16)d); }

	// Indexed (X-form, ra may be R0 = 0).
	void LWZX(PPCReg rt, PPCReg ra, PPCReg rb) { X(23, rt, ra, rb); }
	void LBZX(PPCReg rt, PPCReg ra, PPCReg rb) { X(87, rt, ra, rb); }
	void LHZX(PPCReg rt, PPCReg ra, PPCReg rb) { X(279, rt, ra, rb); }
	void LHAX(PPCReg rt, PPCReg ra, PPCReg rb) { X(343, rt, ra, rb); }
	void LWBRX(PPCReg rt, PPCReg ra, PPCReg rb) { X(534, rt, ra, rb); }
	void LHBRX(PPCReg rt, PPCReg ra, PPCReg rb) { X(790, rt, ra, rb); }
	void STWX(PPCReg rs, PPCReg ra, PPCReg rb) { X(151, rs, ra, rb); }
	void STBX(PPCReg rs, PPCReg ra, PPCReg rb) { X(215, rs, ra, rb); }
	void STHX(PPCReg rs, PPCReg ra, PPCReg rb) { X(407, rs, ra, rb); }
	void STWBRX(PPCReg rs, PPCReg ra, PPCReg rb) { X(662, rs, ra, rb); }
	void STHBRX(PPCReg rs, PPCReg ra, PPCReg rb) { X(918, rs, ra, rb); }
	void LFSX(PPCReg ft, PPCReg ra, PPCReg rb) { X(535, (PPCReg)(ft & 31), ra, rb); }
	void STFSX(PPCReg fs, PPCReg ra, PPCReg rb) { X(663, (PPCReg)(fs & 31), ra, rb); }

	// FPU (single precision arithmetic; registers are F0..F31).
	void FADDS(PPCReg ft, PPCReg fa, PPCReg fb) { A(59, 21, ft, fa, fb, F0); }
	void FSUBS(PPCReg ft, PPCReg fa, PPCReg fb) { A(59, 20, ft, fa, fb, F0); }
	void FMULS(PPCReg ft, PPCReg fa, PPCReg fc) { A(59, 25, ft, fa, F0, fc); }
	void FDIVS(PPCReg ft, PPCReg fa, PPCReg fb) { A(59, 18, ft, fa, fb, F0); }
	void FSQRTS(PPCReg ft, PPCReg fb) { A(59, 22, ft, F0, fb, F0); }
	void FMR(PPCReg ft, PPCReg fb) { if (ft != fb) FX(72, ft, F0, fb); }
	void FNEG(PPCReg ft, PPCReg fb) { FX(40, ft, F0, fb); }
	void FABS(PPCReg ft, PPCReg fb) { FX(264, ft, F0, fb); }
	void FRSP(PPCReg ft, PPCReg fb) { FX(12, ft, F0, fb); }
	void FCMPU(PPCReg fa, PPCReg fb, PPCCR cr = CR0) {
		Write32((63u << 26) | ((u32)cr << 23) | ((fa & 31) << 16) | ((fb & 31) << 11));
	}
	void FCFID(PPCReg ft, PPCReg fb) { FX(846, ft, F0, fb); }   // Double from 64-bit integer.
	void FCTIW(PPCReg ft, PPCReg fb) { FX(14, ft, F0, fb); }    // To int32, current rounding mode.
	void FCTIWZ(PPCReg ft, PPCReg fb) { FX(15, ft, F0, fb); }   // To int32, toward zero.
	void MFCR(PPCReg rt) { Write32(0x7C000026 | ((rt & 31) << 21)); }
	// One CR field (the other bits are undefined). Unlike mfcr, not microcoded on the Cell PPU.
	void MFOCRF(PPCReg rt, int crField) { Write32(0x7C100026 | ((rt & 31) << 21) | ((0x80u >> crField) << 12)); }
	// Sets FPSCR field bf (0..7) to imm (4 bits). Field 7 holds XE, NI and the rounding mode.
	void MTFSFI(int bf, int imm) { Write32((63u << 26) | ((u32)(bf & 7) << 23) | ((u32)(imm & 15) << 12) | (134 << 1)); }
	void MFFS(PPCReg ft) { FX(583, ft, F0, F0); }
	void MTFSF(u8 fieldMask, PPCReg fb) { Write32((63u << 26) | ((u32)fieldMask << 17) | ((fb & 31) << 11) | (711 << 1)); }

	// AltiVec (VMX). Loads/stores are X-form (ra may be R0 = 0); EA is ra + rb, low bits ignored
	// (lvx/stvx: 16 bytes at EA & ~15; lvewx/stvewx: the word lane (EA & 15) / 4).
	void LVX(PPCReg vt, PPCReg ra, PPCReg rb) { X(103, vt, ra, rb); }
	void STVX(PPCReg vs, PPCReg ra, PPCReg rb) { X(231, vs, ra, rb); }
	void LVEWX(PPCReg vt, PPCReg ra, PPCReg rb) { X(71, vt, ra, rb); }
	void STVEWX(PPCReg vs, PPCReg ra, PPCReg rb) { X(199, vs, ra, rb); }
	void LVSL(PPCReg vt, PPCReg ra, PPCReg rb) { X(6, vt, ra, rb); }
	void VADDFP(PPCReg vd, PPCReg va, PPCReg vb) { VX(10, vd, va, vb); }
	void VSUBFP(PPCReg vd, PPCReg va, PPCReg vb) { VX(74, vd, va, vb); }
	// vd = va * vc + vb (one rounding).
	void VMADDFP(PPCReg vd, PPCReg va, PPCReg vc, PPCReg vb) { VA(46, vd, va, vb, vc); }
	void VMAXFP(PPCReg vd, PPCReg va, PPCReg vb) { VX(1034, vd, va, vb); }
	void VMINFP(PPCReg vd, PPCReg va, PPCReg vb) { VX(1098, vd, va, vb); }
	void VAND(PPCReg vd, PPCReg va, PPCReg vb) { VX(1028, vd, va, vb); }
	void VANDC(PPCReg vd, PPCReg va, PPCReg vb) { VX(1092, vd, va, vb); }
	void VOR(PPCReg vd, PPCReg va, PPCReg vb) { VX(1156, vd, va, vb); }
	void VXOR(PPCReg vd, PPCReg va, PPCReg vb) { VX(1220, vd, va, vb); }
	void VNOR(PPCReg vd, PPCReg va, PPCReg vb) { VX(1284, vd, va, vb); }
	void VMR(PPCReg vd, PPCReg va) { if (vd != va) VOR(vd, va, va); }
	void VSLW(PPCReg vd, PPCReg va, PPCReg vb) { VX(388, vd, va, vb); }
	void VADDUWM(PPCReg vd, PPCReg va, PPCReg vb) { VX(128, vd, va, vb); }
	void VSRW(PPCReg vd, PPCReg va, PPCReg vb) { VX(644, vd, va, vb); }
	void VMAXSW(PPCReg vd, PPCReg va, PPCReg vb) { VX(386, vd, va, vb); }
	void VMINSW(PPCReg vd, PPCReg va, PPCReg vb) { VX(898, vd, va, vb); }
	void VSRAW(PPCReg vd, PPCReg va, PPCReg vb) { VX(900, vd, va, vb); }
	// Packs: words -> halfwords, halfwords -> bytes, unsigned saturate (va's elements first).
	void VPKUWUS(PPCReg vd, PPCReg va, PPCReg vb) { VX(206, vd, va, vb); }
	void VPKUHUS(PPCReg vd, PPCReg va, PPCReg vb) { VX(142, vd, va, vb); }
	// Float rounding: to nearest even, toward +inf, toward -inf.
	void VRFIN(PPCReg vd, PPCReg vb) { VX(522, vd, V0, vb); }
	void VRFIP(PPCReg vd, PPCReg vb) { VX(650, vd, V0, vb); }
	void VRFIM(PPCReg vd, PPCReg vb) { VX(714, vd, V0, vb); }
	void VMRGHW(PPCReg vd, PPCReg va, PPCReg vb) { VX(140, vd, va, vb); }
	void VMRGLW(PPCReg vd, PPCReg va, PPCReg vb) { VX(396, vd, va, vb); }
	// Byte i of vd = byte vc[i] & 31 of va || vb.
	void VPERM(PPCReg vd, PPCReg va, PPCReg vb, PPCReg vc) { VA(43, vd, va, vb, vc); }
	// vd = (va & ~vc) | (vb & vc).
	void VSEL(PPCReg vd, PPCReg va, PPCReg vb, PPCReg vc) { VA(42, vd, va, vb, vc); }
	// vd = bytes shb .. shb + 15 of va || vb.
	void VSLDOI(PPCReg vd, PPCReg va, PPCReg vb, int shb) { VA(44, vd, va, vb, (PPCReg)(shb & 15)); }
	void VSPLTW(PPCReg vd, PPCReg vb, int lane) { VX(652, vd, (PPCReg)(lane & 3), vb); }
	void VSPLTISW(PPCReg vd, int simm) { VX(908, vd, (PPCReg)(simm & 31), R0); }
	// Compares; the record forms set CR6 (bit 0 all lanes true, bit 2 all false).
	void VCMPEQFP(PPCReg vd, PPCReg va, PPCReg vb, bool rc = false) { VX(198 | (rc ? 0x400 : 0), vd, va, vb); }
	void VCMPGEFP(PPCReg vd, PPCReg va, PPCReg vb, bool rc = false) { VX(454 | (rc ? 0x400 : 0), vd, va, vb); }
	void VCMPGTFP(PPCReg vd, PPCReg va, PPCReg vb, bool rc = false) { VX(710 | (rc ? 0x400 : 0), vd, va, vb); }
	void VCMPEQUW(PPCReg vd, PPCReg va, PPCReg vb, bool rc = false) { VX(134 | (rc ? 0x400 : 0), vd, va, vb); }
	void VCMPGTUW(PPCReg vd, PPCReg va, PPCReg vb, bool rc = false) { VX(646 | (rc ? 0x400 : 0), vd, va, vb); }
	// Signed int32 lanes -> float, divided by 2^uimm.
	void VCFSX(PPCReg vd, PPCReg vb, int uimm) { VX(842, vd, (PPCReg)(uimm & 31), vb); }
	// Unsigned int32 lanes -> float, divided by 2^uimm.
	void VCFUX(PPCReg vd, PPCReg vb, int uimm) { VX(778, vd, (PPCReg)(uimm & 31), vb); }
	// Float * 2^uimm -> signed int32, truncated, saturated.
	void VCTSXS(PPCReg vd, PPCReg vb, int uimm) { VX(970, vd, (PPCReg)(uimm & 31), vb); }
	void MTVSCR(PPCReg vb) { VX(1604, V0, V0, vb); }
	void MTSPR(int spr, PPCReg rs) { Write32((31u << 26) | ((rs & 31) << 21) | ((u32)(spr & 31) << 16) | ((u32)(spr >> 5) << 11) | (467 << 1)); }
	void MFSPR(PPCReg rt, int spr) { Write32((31u << 26) | ((rt & 31) << 21) | ((u32)(spr & 31) << 16) | ((u32)(spr >> 5) << 11) | (339 << 1)); }

	// Pseudo-instructions.
	// 32-bit value; the upper 32 bits of the register may be anything (sign extension).
	void LI32(PPCReg rt, u32 value);
	// Zero-extended 32-bit value (pointers - user addresses are below 4 GB).
	void LIPTR(PPCReg rt, uintptr_t value);
	void LI64(PPCReg rt, u64 value);
	// Calls a C function through its ELFv1 descriptor (loads r2). Clobbers r0, r12, CTR, LR,
	// and all volatile registers per the ABI.
	void CallDescriptor(const void *functionPointer);
	template <typename T>
	void QuickCallFunction(T *func) { CallDescriptor((const void *)func); }

protected:
	void D(u32 op, PPCReg rt, PPCReg ra, u16 imm) { Write32((op << 26) | ((rt & 31) << 21) | ((ra & 31) << 16) | imm); }
	void X(u32 xo, PPCReg rt, PPCReg ra, PPCReg rb) { Write32((31u << 26) | ((rt & 31) << 21) | ((ra & 31) << 16) | ((rb & 31) << 11) | (xo << 1)); }
	void XO(u32 xo, PPCReg rt, PPCReg ra, PPCReg rb) { Write32((31u << 26) | ((rt & 31) << 21) | ((ra & 31) << 16) | ((rb & 31) << 11) | (xo << 1)); }
	void MD(u32 xo, PPCReg ra, PPCReg rs, int sh, int mb) {
		const u32 mbField = (((u32)mb & 0x1F) << 1) | (((u32)mb >> 5) & 1);
		Write32((30u << 26) | ((rs & 31) << 21) | ((ra & 31) << 16) | (((u32)sh & 31) << 11) | (mbField << 5) | (xo << 2) | ((((u32)sh >> 5) & 1) << 1));
	}
	void A(u32 op, u32 xo, PPCReg ft, PPCReg fa, PPCReg fb, PPCReg fc) {
		Write32((op << 26) | ((ft & 31) << 21) | ((fa & 31) << 16) | ((fb & 31) << 11) | ((fc & 31) << 6) | (xo << 1));
	}
	void VX(u32 xo, PPCReg vd, PPCReg va, PPCReg vb) { Write32((4u << 26) | ((vd & 31) << 21) | ((va & 31) << 16) | ((vb & 31) << 11) | xo); }
	void VA(u32 xo, PPCReg vd, PPCReg va, PPCReg vb, PPCReg vc) {
		Write32((4u << 26) | ((vd & 31) << 21) | ((va & 31) << 16) | ((vb & 31) << 11) | ((vc & 31) << 6) | xo);
	}
	void FX(u32 xo, PPCReg ft, PPCReg fa, PPCReg fb) {
		Write32((63u << 26) | ((ft & 31) << 21) | ((fa & 31) << 16) | ((fb & 31) << 11) | (xo << 1));
	}

	const u8 *code_ = nullptr;
	u8 *writable_ = nullptr;
};

class PPCCodeBlock : public CodeBlock<PPCEmitter> {
private:
	void PoisonMemory(int offset) override;
};

}  // namespace PPCGen
