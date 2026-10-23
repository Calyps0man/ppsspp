// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
// Exercises the production PS3 adapter against shared POSIX mappings, with
// syscall failure injection. This does not test LV2 or console execution.
#include <cassert>
#include <cstdio>
#include <map>
#include <sys/mman.h>
#include <unistd.h>
#include "Common/MemArena.h"
#include "Core/MemMap.h"
#include <lv2/memory.h>

static_assert(sizeof(void *) == 8, "Exercise the PS3 masked path with 64-bit pointers");
#ifndef MASKED_PSP_MEMORY
#error PS3 must mask guest addresses even with 64-bit host pointers
#endif

namespace Memory {
u8 *base = nullptr;
}

namespace {
struct Allocation { int fd; size_t size; };
std::map<sys_mem_id_t, Allocation> allocations;
std::map<sys_mem_addr_t, sys_mem_id_t> mappings;
sys_mem_id_t nextId = 1;
void *reservation = nullptr;
size_t reservationSize = 0;
bool failReserve = false, failAllocate = false, failMap = false;
bool failUnmap = false, failFree = false, failRelease = false;
}

int sysMMapperAllocateAddress(size_t size, uint64_t flags, size_t alignment, sys_mem_addr_t *address) {
	assert(flags == SYS_MEMORY_PAGE_SIZE_64K && alignment == size);
	if (failReserve) return -1;
	reservation = mmap(reinterpret_cast<void *>(0x30000000), size, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	assert(reservation != MAP_FAILED);
	reservationSize = size;
	*address = reinterpret_cast<uintptr_t>(reservation);
	return 0;
}
int sysMMapperFreeAddress(sys_mem_addr_t address) {
	if (failRelease) return -1;
	assert(mappings.empty() && allocations.empty());
	assert(address == reinterpret_cast<uintptr_t>(reservation));
	assert(munmap(reservation, reservationSize) == 0);
	reservation = nullptr;
	return 0;
}
int sysMMapperAllocateMemory(size_t size, uint64_t flags, sys_mem_id_t *id) {
	assert(flags == SYS_MEMORY_PAGE_SIZE_64K && size % 65536 == 0);
	if (failAllocate) return -1;
	int fd = memfd_create("psp-memory-test", 0);
	assert(fd >= 0 && ftruncate(fd, size) == 0);
	*id = nextId++;
	allocations[*id] = {fd, size};
	return 0;
}
int sysMMapperFreeMemory(sys_mem_id_t id) {
	if (failFree) return -1;
	for (const auto &view : mappings) assert(view.second != id);
	assert(close(allocations.at(id).fd) == 0);
	allocations.erase(id);
	return 0;
}
int sysMMapperMapMemory(sys_mem_addr_t address, sys_mem_id_t id, uint64_t flags) {
	assert(flags == SYS_MEMORY_PROT_READ_WRITE);
	if (failMap) return -1;
	if (mappings.count(address)) return -1;
	const auto &allocation = allocations.at(id);
	void *p = mmap(reinterpret_cast<void *>(static_cast<uintptr_t>(address)), allocation.size,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, allocation.fd, 0);
	assert(reinterpret_cast<uintptr_t>(p) == address);
	mappings[address] = id;
	return 0;
}
int sysMMapperUnmapMemory(sys_mem_addr_t address, sys_mem_id_t *id) {
	if (failUnmap) return -1;
	*id = mappings.at(address);
	size_t size = allocations.at(*id).size;
	void *p = mmap(reinterpret_cast<void *>(static_cast<uintptr_t>(address)), size,
		PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	assert(reinterpret_cast<uintptr_t>(p) == address);
	mappings.erase(address);
	return 0;
}

int main() {
	MemArena arena;
	assert(!arena.GrabMemSpace(0));
	assert(arena.roundup(SIZE_MAX) == 0);
	assert(arena.GrabMemSpace(32 * 1024 * 1024));
	failReserve = true;
	assert(!arena.Find4GBBase());
	failReserve = false;
	u8 *base = arena.Find4GBBase();
	assert(base);
	Memory::base = base;
	u8 *vram = base + 0x04000000;
	u8 *mirror = base + 0x04200000;
	assert(arena.CreateView(0, 2 * 1024 * 1024, vram) == vram);
	assert(arena.CreateView(0, 2 * 1024 * 1024, mirror) == mirror);
	assert(allocations.size() == 1 && mappings.size() == 2);
	vram[0] = 0x12;
	mirror[2 * 1024 * 1024 - 1] = 0x34;
	assert(mirror[0] == 0x12 && vram[2 * 1024 * 1024 - 1] == 0x34);
	// Use the emulator's real fast accessors through uncached and kernel aliases.
	for (u32 alias : {0x04000000U, 0x44000000U, 0x84000000U, 0xC4000000U}) {
		assert(Memory::GetPointerUnchecked(alias) == vram);
		assert(Memory::GetPointerWriteUnchecked(alias) == vram);
		Memory::WriteUnchecked_U32(0x12345678, alias + 4);
		assert(Memory::ReadUnchecked_U32(0x04200004) == 0x12345678);
		assert(vram[4] == 0x78 && vram[7] == 0x12);
		Memory::WriteUnchecked_U64(0x123456789ABCDEF0ULL, alias + 8);
		assert(Memory::ReadUnchecked_U64(0x44200008) == 0x123456789ABCDEF0ULL);
		Memory::WriteUnchecked_U16(0xABCD, alias + 16);
		assert(Memory::ReadUnchecked_U16(0x84200010) == 0xABCD);
		Memory::WriteUnchecked_U8(0xEF, alias + 18);
		assert(Memory::ReadUnchecked_U8(0xC4200012) == 0xEF);
		Memory::WriteUnchecked_Float(1.25f, alias + 20);
		assert(Memory::ReadUnchecked_Float(0x04200014) == 1.25f);
	}
	assert(!arena.CreateView(0, 65536, base + 0x04400000));
	assert(!arena.CreateView(65536, 65536, base + 0x04400000));
	assert(!arena.CreateView(-1, 65536, base));
	assert(!arena.CreateView(0, SIZE_MAX, base));
	assert(!arena.CreateView(0, 65536, base - 65536));
	assert(!arena.CreateView(0, 65536, base + 0x10000000));
	failAllocate = true;
	assert(!arena.CreateView(0x200000, 16384, base + 0x10000));
	failAllocate = false;
	failMap = true;
	assert(!arena.CreateView(0x200000, 16384, base + 0x10000));
	failMap = false;
	assert(arena.CreateView(0x200000, 16384, base + 0x10000));
	assert(allocations.size() == 2);
	arena.ReleaseView(0, mirror, 0);
	arena.ReleaseView(0, mirror, 0);
	assert(mappings.size() == 2);
	failUnmap = true;
	assert(!arena.GrabMemSpace(65536));
	assert(reservation && allocations.size() == 2);
	failUnmap = false;
	failFree = true;
	arena.ReleaseSpace();
	assert(mappings.empty() && allocations.size() == 2);
	failFree = false;
	failRelease = true;
	arena.ReleaseSpace();
	assert(allocations.empty() && reservation);
	failRelease = false;
	assert(arena.GrabMemSpace(65536));
	assert(!reservation);
	arena.ReleaseSpace();
	Memory::base = nullptr;
	assert(allocations.empty() && mappings.empty() && !reservation);
	puts("PS3 memory adapter: 64-bit guest aliases, shared VRAM, bounds, failures and cleanup PASS (host syscall model)");
}
