// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Can this console run JIT-compiled code? A normal LV2 process can't make memory both writable
// and executable, which is why the port uses the IR interpreter. CFW with PS3MAPI (Cobra, webMAN)
// can write into a process's code (and allocate executable pages), the approach of crystalct's
// PS3 MAPI Dynarec Lib (https://github.com/crystalct/ps3mapidynarec) and webMAN's payload loader.
// This probe checks both routes on the real console: it writes tiny generated functions, calls
// them and checks their results, and measures how fast code can be written.
//
// Every step is logged before it runs, so if a step crashes the app, launch.log names it.
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <ppu-lv2.h>
#include <sys/process.h>
#include <sys/systime.h>

#include "PS3/frontend/Frontend.h"
#include "PS3/jit/MapiJitProbe.h"

// A 64 KB block of nops in the program's own code segment: generated code is written over it.
__asm__(
	".section \".text\"\n"
	".align 7\n"
	".globl ppsspp_mapi_code_area\n"
	"ppsspp_mapi_code_area:\n"
	".rept 16384\n"
	"nop\n"
	".endr\n"
	"blr\n"
	".previous\n");
extern "C" char ppsspp_mapi_code_area[];

namespace {

constexpr uint64_t SYSCALL8_OPCODE_PS3MAPI = 0x7777;
constexpr uint64_t OP_GET_CORE_VERSION = 0x0011;
constexpr uint64_t OP_GET_FW_VERSION = 0x0014;
constexpr uint64_t OP_GET_PROC_MEM = 0x0031;
constexpr uint64_t OP_SET_PROC_MEM = 0x0032;
constexpr uint64_t OP_PROC_PAGE_ALLOCATE = 0x0033;
constexpr uint64_t OP_PROC_PAGE_FREE = 0x0034;
constexpr uint64_t PAGE_FLAGS = 0x2F;  // As webMAN's payload loader uses.
constexpr uint32_t CODE_AREA_BYTES = 16384 * 4;

constexpr uint32_t BLR = 0x4E800020;
inline uint32_t LiR3(int16_t value) { return 0x38600000u | (uint16_t)value; }
constexpr uint32_t ADDI_R3_1 = 0x38630001;

int MapiCoreVersion() {
	lv2syscall2(8, SYSCALL8_OPCODE_PS3MAPI, OP_GET_CORE_VERSION);
	return_to_user_prog(int);
}
int MapiFwVersion() {
	lv2syscall2(8, SYSCALL8_OPCODE_PS3MAPI, OP_GET_FW_VERSION);
	return_to_user_prog(int);
}
int MapiSetMem(uint32_t pid, uint64_t addr, const void *src, uint64_t size) {
	lv2syscall6(8, SYSCALL8_OPCODE_PS3MAPI, OP_SET_PROC_MEM, (uint64_t)pid, addr, (uint64_t)(uintptr_t)src, size);
	return_to_user_prog(int);
}
int MapiGetMem(uint32_t pid, uint64_t addr, void *dst, uint64_t size) {
	lv2syscall6(8, SYSCALL8_OPCODE_PS3MAPI, OP_GET_PROC_MEM, (uint64_t)pid, addr, (uint64_t)(uintptr_t)dst, size);
	return_to_user_prog(int);
}
int MapiPageAllocate(uint32_t pid, uint64_t size, uint64_t pageSize, uint64_t flags, uint64_t exec, uint64_t *table) {
	lv2syscall8(8, SYSCALL8_OPCODE_PS3MAPI, OP_PROC_PAGE_ALLOCATE, (uint64_t)pid, size, pageSize, flags, exec, (uint64_t)(uintptr_t)table);
	return_to_user_prog(int);
}
int MapiPageFree(uint32_t pid, uint64_t flags, uint64_t *table) {
	lv2syscall5(8, SYSCALL8_OPCODE_PS3MAPI, OP_PROC_PAGE_FREE, (uint64_t)pid, flags, (uint64_t)(uintptr_t)table);
	return_to_user_prog(int);
}

double Now() {
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return (double)sec + (double)nsec * 1e-9;
}

// New instructions must reach the instruction cache: write data back, then invalidate (128-byte lines).
void FlushIcache(uint64_t addr, uint32_t bytes) {
	const uint64_t start = addr & ~127ull, end = addr + bytes;
	for (uint64_t p = start; p < end; p += 128)
		__asm__ volatile("dcbst 0,%0" : : "r"(p) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (uint64_t p = start; p < end; p += 128)
		__asm__ volatile("icbi 0,%0" : : "r"(p) : "memory");
	__asm__ volatile("sync\n isync" : : : "memory");
}

// Calls code at addr through a function descriptor (ELFv1 ABI), with our own TOC.
int CallCode(uint64_t addr) {
	typedef int (*Fn)(void);
	static uint64_t descriptor[3];
	const uint64_t *own = (const uint64_t *)(void *)&MapiCoreVersion;
	descriptor[0] = addr;
	descriptor[1] = own[1];
	descriptor[2] = 0;
	Fn fn = (Fn)(void *)descriptor;
	return fn();
}

struct Report {
	std::vector<std::string> lines;
	int passed = 0, failed = 0;
	void Line(const char *format, ...) __attribute__((format(printf, 2, 3))) {
		char text[256];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		lines.push_back(text);
		PS3Frontend::Trace("JIT probe: %s", text);
	}
	bool Check(bool ok, const char *what) {
		Line("%s  %s", ok ? "PASS" : "FAIL", what);
		if (ok) passed++; else failed++;
		return ok;
	}
	// Logged before a step that could crash the app if the route doesn't work.
	void Step(const char *what) { PS3Frontend::Trace("JIT probe step: %s", what); }
};

bool WriteWords(uint32_t pid, uint64_t addr, const std::vector<uint32_t> &words) {
	return MapiSetMem(pid, addr, words.data(), words.size() * 4) == 0;
}

}  // namespace

std::vector<std::string> MapiJitProbe() {
	Report r;
	r.Line("PPSSPP PS3 JIT probe (PS3MAPI executable memory)");
	const uint32_t pid = sysProcessGetPid();

	r.Step("query PS3MAPI core version");
	const int core = MapiCoreVersion();
	if (core < 0x0120) {
		r.Check(false, "PS3MAPI available (needs Cobra/webMAN CFW with PS3MAPI)");
		r.Line("INFO  core version reply %d; nothing else can be tested", core);
		r.Line("%d passed, %d failed", r.passed, r.failed);
		return r.lines;
	}
	r.Check(true, "PS3MAPI available");
	r.Line("INFO  PS3MAPI core %X, firmware %X, process id %u", (unsigned)core, (unsigned)MapiFwVersion(), pid);

	// Route A: write into our own code segment (the MAPI Dynarec Lib approach).
	bool routeA = false, routeB = false;
	const uint64_t area = (uint64_t)(uintptr_t)ppsspp_mapi_code_area;
	r.Line("INFO  code area at %08llx, %u KB", (unsigned long long)area, CODE_AREA_BYTES / 1024);
	r.Step("A1 write 'return 42' into the code segment");
	bool aWrite = WriteWords(pid, area, {LiR3(42), BLR});
	uint32_t readBack[2] = {0, 0};
	MapiGetMem(pid, area, readBack, sizeof(readBack));
	if (r.Check(aWrite && readBack[0] == LiR3(42) && readBack[1] == BLR, "A: PS3MAPI writes into our code segment")) {
		FlushIcache(area, 8);
		r.Step("A2 call generated code");
		const int value = CallCode(area);
		r.Check(value == 42, "A: generated function runs and returns 42");

		// Overwrite in place; see whether the cache flush is needed.
		r.Step("A3 rewrite to 'return 7'");
		WriteWords(pid, area, {LiR3(7), BLR});
		const int stale = CallCode(area);
		FlushIcache(area, 8);
		const int fresh = CallCode(area);
		r.Line("INFO  after rewrite: %d before cache flush, %d after", stale, fresh);
		r.Check(fresh == 7, "A: rewritten code runs after an instruction cache flush");

		// A bigger block: li r3,0 + 4000 x addi r3,r3,1 + blr (16 KB), timed.
		std::vector<uint32_t> big;
		big.push_back(LiR3(0));
		for (int i = 0; i < 4000; ++i) big.push_back(ADDI_R3_1);
		big.push_back(BLR);
		r.Step("A4 write and run a 16 KB block");
		const double t0 = Now();
		const int rounds = 32;
		bool ok = true;
		for (int i = 0; i < rounds && ok; ++i) ok = WriteWords(pid, area, big);
		const double seconds = Now() - t0;
		FlushIcache(area, big.size() * 4);
		const int sum = ok ? CallCode(area) : -1;
		routeA = r.Check(ok && sum == 4000, "A: 16 KB generated block returns 4000") && fresh == 7 && value == 42;
		if (seconds > 0)
			r.Line("INFO  code write speed: %.1f MB/s (%.2f ms per 16 KB block)", rounds * big.size() * 4 / seconds / 1e6,
				seconds * 1000.0 / rounds);
		// Leave the area as nops ending in blr, as it started.
		std::vector<uint32_t> nops(big.size(), 0x60000000);
		WriteWords(pid, area, nops);
		FlushIcache(area, nops.size() * 4);
	}

	// Route B: a freshly allocated executable page (webMAN's payload loader does this).
	uint64_t table[2] = {0, 0};
	r.Step("B1 allocate a 64 KB executable page");
	const int alloc = MapiPageAllocate(pid, 64 * 1024, 0x100, PAGE_FLAGS, 1, table);
	r.Line("INFO  page allocate returned %d, user address %08llx, kernel address %016llx", alloc,
		(unsigned long long)table[0], (unsigned long long)table[1]);
	if (r.Check(alloc == 0 && table[0] != 0, "B: PS3MAPI allocates an executable page")) {
		r.Step("B2 write 'return 99' into the page");
		bool written = WriteWords(pid, table[0], {LiR3(99), BLR});
		uint32_t back[2] = {0, 0};
		MapiGetMem(pid, table[0], back, sizeof(back));
		if (r.Check(written && back[0] == LiR3(99) && back[1] == BLR, "B: write and read back through PS3MAPI")) {
			FlushIcache(table[0], 8);
			r.Step("B3 call code in the allocated page (if the app stops here, route B doesn't work)");
			routeB = r.Check(CallCode(table[0]) == 99, "B: code in the allocated page runs and returns 99");
		}
		r.Step("B4 free the page");
		MapiPageFree(pid, PAGE_FLAGS, table);
	}

	if (routeA || routeB)
		r.Line("RESULT  A JIT can run on this console through PS3MAPI (route %s).", routeA && routeB ? "A and B" : (routeA ? "A, code segment" : "B, allocated pages"));
	else
		r.Line("RESULT  JIT through PS3MAPI did not work here.");
	r.Line("%d passed, %d failed", r.passed, r.failed);
	FILE *file = fopen(PS3Frontend::ReportPath("jit_probe.txt").c_str(), "w");
	if (file) {
		for (const auto &line : r.lines) fprintf(file, "%s\n", line.c_str());
		fclose(file);
	}
	return r.lines;
}
