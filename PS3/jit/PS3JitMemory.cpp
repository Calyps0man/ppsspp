// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <string>
#include <ppu-lv2.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <unistd.h>

#include "PS3/jit/PS3JitMemory.h"
#include "PS3/frontend/Frontend.h"

// 8 MB of nops in the code segment (the JIT's executable area). 2M instructions.
__asm__(
	".section \".text\"\n"
	".align 12\n"
	".globl ppsspp_jit_code_area\n"
	"ppsspp_jit_code_area:\n"
	".rept 2097152\n"
	"nop\n"
	".endr\n"
	".previous\n");
extern "C" char ppsspp_jit_code_area[];

namespace PS3Jit {

static constexpr size_t AREA_BYTES = 8 * 1024 * 1024;
static constexpr uint64_t SYSCALL8_OPCODE_PS3MAPI = 0x7777;
static constexpr uint64_t OP_GET_CORE_VERSION = 0x0011;
static constexpr uint64_t OP_GET_PROC_MEM = 0x0031;
static constexpr uint64_t OP_SET_PROC_MEM = 0x0032;

static int s_available = -1;
static char s_status[160] = "not checked";
static uint64_t s_bytes = 0, s_calls = 0, s_ticks = 0;
static inline uint64_t Tb() {
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return t;
}

static int MapiCoreVersion() {
	lv2syscall2(8, SYSCALL8_OPCODE_PS3MAPI, OP_GET_CORE_VERSION);
	return_to_user_prog(int);
}
static int MapiSetMem(uint32_t pid, uint64_t addr, const void *src, uint64_t size) {
	lv2syscall6(8, SYSCALL8_OPCODE_PS3MAPI, OP_SET_PROC_MEM, (uint64_t)pid, addr, (uint64_t)(uintptr_t)src, size);
	return_to_user_prog(int);
}
static int MapiGetMem(uint32_t pid, uint64_t addr, void *dst, uint64_t size) {
	lv2syscall6(8, SYSCALL8_OPCODE_PS3MAPI, OP_GET_PROC_MEM, (uint64_t)pid, addr, (uint64_t)(uintptr_t)dst, size);
	return_to_user_prog(int);
}

static void FlushIcache(uint64_t addr, size_t bytes) {
	const uint64_t start = addr & ~127ull, end = addr + bytes;
	for (uint64_t p = start; p < end; p += 128)
		__asm__ volatile("dcbst 0,%0" : : "r"(p) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (uint64_t p = start; p < end; p += 128)
		__asm__ volatile("icbi 0,%0" : : "r"(p) : "memory");
	__asm__ volatile("sync\n isync" : : : "memory");
}

uint8_t *CodeArea() { return (uint8_t *)ppsspp_jit_code_area; }
size_t CodeAreaSize() { return AREA_BYTES; }

// Plain stores into the code area: lv2 doesn't seem to enforce W^X on the program's own code
// segment (found with CFW while porting flycast's SH4 dynarec). If so, a commit is a memcpy and
// a cache flush instead of a PS3MAPI system call (~33 us each). Tried on a thread of its own: a
// faulting store kills that thread, not the app. A marker file covers the case where it takes
// the whole app down instead: the next start then skips the test and uses PS3MAPI.
static bool s_direct = false;
static volatile int s_probeDone = 0;

static void DirectProbeThread(void *) {
	volatile uint32_t *p = (volatile uint32_t *)(ppsspp_jit_code_area + AREA_BYTES - 128);
	const uint32_t old = p[0];
	p[0] = old;  // The same word (a nop): harmless if it works.
	FlushIcache((uint64_t)(uintptr_t)p, 4);
	s_probeDone = 1;
	sysThreadExit(0);
}

static bool ProbeDirectWrites() {
	const std::string marker = PS3Frontend::ReportPath("jitprobe.running");
	if (access(marker.c_str(), F_OK) == 0) {
		PS3Frontend::Trace("JIT: the last direct code write test didn't finish; using PS3MAPI (delete %s to test again)", marker.c_str());
		return false;
	}
	if (FILE *f = fopen(marker.c_str(), "w")) {
		fputs("PPSSPP JIT direct write test in progress\n", f);
		fclose(f);
	}
	sys_ppu_thread_t thread;
	s_probeDone = 0;
	if (sysThreadCreate(&thread, DirectProbeThread, nullptr, 1000, 0x4000, 0, (char *)"jit_probe") != 0) {
		remove(marker.c_str());
		return false;
	}
	for (int i = 0; i < 100 && !s_probeDone; ++i)
		usleep(5000);
	remove(marker.c_str());
	return s_probeDone != 0;
}

bool Available() {
	if (s_available >= 0)
		return s_available == 1;
	s_available = 0;
	s_direct = ProbeDirectWrites();
	if (s_direct) {
		snprintf(s_status, sizeof(s_status), "direct writes, %u MB code area at %p", (unsigned)(AREA_BYTES >> 20), ppsspp_jit_code_area);
		s_available = 1;
		return true;
	}
	const int core = MapiCoreVersion();
	if (core < 0x0120) {
		snprintf(s_status, sizeof(s_status), "PS3MAPI not available (reply %d) - needs Cobra/webMAN CFW", core);
		return false;
	}
	// Write and read back the last word of the area (a nop either way).
	const uint32_t pid = sysProcessGetPid();
	const uint64_t addr = (uint64_t)(uintptr_t)(ppsspp_jit_code_area + AREA_BYTES - 4);
	const uint32_t nop = 0x60000000;
	uint32_t back = 0;
	if (MapiSetMem(pid, addr, &nop, 4) != 0 || MapiGetMem(pid, addr, &back, 4) != 0 || back != nop) {
		snprintf(s_status, sizeof(s_status), "PS3MAPI %X present, but writing the code area failed", (unsigned)core);
		return false;
	}
	snprintf(s_status, sizeof(s_status), "PS3MAPI %X, %u MB code area at %p (direct writes not possible)", (unsigned)core, (unsigned)(AREA_BYTES >> 20), ppsspp_jit_code_area);
	s_available = 1;
	return true;
}

const char *StatusText() { return s_status; }

bool Commit(const uint8_t *code, const uint8_t *src, size_t len) {
	if (!len)
		return true;
	const uint8_t *area = (const uint8_t *)ppsspp_jit_code_area;
	if (code < area || code + len > area + AREA_BYTES) {
		PS3Frontend::Trace("JIT commit outside the code area: %p + %u", code, (unsigned)len);
		return false;
	}
	const uint64_t t0 = Tb();
	if (s_direct) {
		memcpy((void *)code, src, len);
		s_calls++;
		s_bytes += len;
		FlushIcache((uint64_t)(uintptr_t)code, len);
		s_ticks += Tb() - t0;
		return true;
	}
	const uint32_t pid = sysProcessGetPid();
	// Moderate chunks per call.
	const size_t chunk = 64 * 1024;
	for (size_t done = 0; done < len; done += chunk) {
		const size_t n = len - done < chunk ? len - done : chunk;
		if (MapiSetMem(pid, (uint64_t)(uintptr_t)(code + done), src + done, n) != 0) {
			PS3Frontend::Trace("JIT commit failed at %p", code + done);
			return false;
		}
		s_calls++;
	}
	s_bytes += len;
	FlushIcache((uint64_t)(uintptr_t)code, len);
	s_ticks += Tb() - t0;
	return true;
}

uint64_t CommittedBytes() { return s_bytes; }
uint64_t CommitCalls() { return s_calls; }
uint64_t CommitTicks() { return s_ticks; }

}  // namespace PS3Jit

// CodeBlock hook (Common/CodeBlock.h).
typedef unsigned char u8;
u8 *PS3JitCodeArea(size_t *size, size_t requested) {
	*size = requested < PS3Jit::CodeAreaSize() ? requested : PS3Jit::CodeAreaSize();
	return PS3Jit::CodeArea();
}
