// Copyright (c) 2026- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Test build only: the PS3 JIT's executable-memory interface on 64-bit PowerPC Linux (run under
// qemu-ppc64 to test the PPC JIT backend off the console). Same "mirror + commit" model as the
// PS3, so the backend's commit logic gets exercised too.
#include "ppsspp_config.h"
#if (PPSSPP_ARCH(POWERPC) && PPSSPP_ARCH(64BIT) && !PPSSPP_PLATFORM(PS3)) || defined(PPSSPP_RSX_HOSTSIM)
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include "PS3/jit/PS3JitMemory.h"

namespace PS3Jit {

static constexpr size_t AREA_BYTES = 8 * 1024 * 1024;
static uint8_t *s_area = nullptr;
static uint64_t s_bytes = 0, s_calls = 0;

uint8_t *CodeArea() {
	if (!s_area) {
		void *p = mmap(nullptr, AREA_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		s_area = p == MAP_FAILED ? nullptr : (uint8_t *)p;
	}
	return s_area;
}
size_t CodeAreaSize() { return AREA_BYTES; }
bool Available() { return CodeArea() != nullptr; }
const char *StatusText() { return "Linux test build (mmap RWX)"; }

bool Commit(const uint8_t *code, const uint8_t *src, size_t len) {
#ifdef PPSSPP_RSX_HOSTSIM
	// Host test build: the code area is ordinary memory (never run).
	if (code != src)
		memcpy((void *)code, src, len);
	s_bytes += len;
	s_calls++;
	return true;
#endif
	if (code < s_area || code + len > s_area + AREA_BYTES) {
		fprintf(stderr, "JIT commit outside the code area\n");
		return false;
	}
	memcpy((void *)code, src, len);
	__builtin___clear_cache((char *)code, (char *)code + len);
	s_bytes += len;
	s_calls++;
	return true;
}

uint64_t CommittedBytes() { return s_bytes; }
uint64_t CommitCalls() { return s_calls; }
uint64_t CommitTicks() { return 0; }

}  // namespace PS3Jit

typedef unsigned char u8;
u8 *PS3JitCodeArea(size_t *size, size_t requested) {
	*size = requested < PS3Jit::CodeAreaSize() ? requested : PS3Jit::CodeAreaSize();
	return PS3Jit::CodeArea();
}
#endif
