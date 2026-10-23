// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <map>
#include <sys/mman.h>
#include <sys/memory.h>
#include "Common/MemoryUtil.h"
static std::map<sys_mem_addr_t, size_t> allocations;
static bool failAllocate;
int sysMemoryAllocate(size_t size, uint64_t flags, sys_mem_addr_t *address) {
	assert(flags == SYS_MEMORY_PAGE_SIZE_64K && size % 65536 == 0);
	if (failAllocate) return -1;
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	assert(p != MAP_FAILED);
	*address = reinterpret_cast<uintptr_t>(p);
	allocations[*address] = size;
	return 0;
}
int sysMemoryFree(sys_mem_addr_t address) {
	assert(munmap(reinterpret_cast<void *>(static_cast<uintptr_t>(address)), allocations.at(address)) == 0);
	allocations.erase(address);
	return 0;
}
int main() {
	assert(GetMemoryProtectPageSize() == 65536);
	assert(!AllocateMemoryPages(0, MEM_PROT_READ | MEM_PROT_WRITE));
	assert(!AllocateMemoryPages(SIZE_MAX, MEM_PROT_READ | MEM_PROT_WRITE));
	assert(!AllocateMemoryPages(65536, MEM_PROT_READ | MEM_PROT_EXEC));
	assert(!AllocateExecutableMemory(65536));
	failAllocate = true;
	assert(!AllocateMemoryPages(1, MEM_PROT_READ | MEM_PROT_WRITE));
	failAllocate = false;
	auto *p = static_cast<unsigned char *>(AllocateMemoryPages(65537, MEM_PROT_READ | MEM_PROT_WRITE));
	assert(p && allocations.begin()->second == 131072);
	p[0] = 12; p[65536] = 34;
	assert(!ProtectMemoryPages(p, 1, MEM_PROT_READ));
	assert(!ProtectMemoryPages(p, 1, MEM_PROT_READ | MEM_PROT_EXEC));
	FreeMemoryPages(p, 65537);
	FreeMemoryPages(nullptr, 0);
	assert(allocations.empty());
	assert(!AllocateAlignedMemory(100, 3));
	assert(!AllocateAlignedMemory(0, 64));
	void *aligned = AllocateAlignedMemory(100, 64);
	assert(aligned && reinterpret_cast<uintptr_t>(aligned) % 64 == 0);
	FreeAlignedMemory(aligned);
	puts("PS3 page adapter: rounding, permissions, failure, writes and free PASS (host syscall model)");
}
