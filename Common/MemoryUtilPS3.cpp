// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ppsspp_config.h"
#if PPSSPP_PLATFORM(PS3)
#include <cstdlib>
#include <malloc.h>
#include <sys/memory.h>
#include "Common/MemoryUtil.h"
#include "Common/Log.h"

int GetMemoryProtectPageSize() { return 64 * 1024; }
bool PlatformIsWXExclusive() { return true; }

void *AllocateExecutableMemory(size_t size) {
	ERROR_LOG(Log::MemMap, "PS3 executable memory is unsupported; select the IR interpreter");
	return nullptr;
}
void FreeExecutableMemory(void *ptr, size_t size) {
	if (ptr)
		ERROR_LOG(Log::MemMap, "Unexpected PS3 executable allocation");
}

void *AllocateMemoryPages(size_t size, uint32_t flags) {
	constexpr size_t PAGE_SIZE = 64 * 1024;
	if (!size || size > SIZE_MAX - (PAGE_SIZE - 1) || flags != (MEM_PROT_READ | MEM_PROT_WRITE))
		return nullptr;
	sys_mem_addr_t address;
	int result = sysMemoryAllocate((size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), SYS_MEMORY_PAGE_SIZE_64K, &address);
	if (result) {
		ERROR_LOG(Log::MemMap, "PS3 allocate memory pages failed: %08x", result);
		return nullptr;
	}
	return reinterpret_cast<void *>(static_cast<uintptr_t>(address));
}

bool ProtectMemoryPages(const void *ptr, size_t size, uint32_t flags) {
	// PSL1GHT has no supported page-protection transition API. Never report
	// success for a permission change that has not actually been performed.
	return false;
}

void FreeMemoryPages(void *ptr, size_t size) {
	if (!ptr)
		return;
	int result = sysMemoryFree(static_cast<sys_mem_addr_t>(reinterpret_cast<uintptr_t>(ptr)));
	if (result)
		ERROR_LOG(Log::MemMap, "PS3 free memory pages failed: %08x", result);
}

void *AllocateAlignedMemory(size_t size, size_t alignment) {
	if (!size || alignment < sizeof(void *) || (alignment & (alignment - 1)))
		return nullptr;
	return memalign(alignment, size);
}
void FreeAlignedMemory(void *ptr) { free(ptr); }
#endif
