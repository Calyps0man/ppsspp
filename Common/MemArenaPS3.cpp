// Copyright (c) 2012- PPSSPP Project.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ppsspp_config.h"
#if PPSSPP_PLATFORM(PS3)

#include <algorithm>
#include <new>
#include <sys/memory.h>
#include <lv2/memory.h>
#include "Common/MemArena.h"
#include "Common/Log.h"

namespace {
constexpr size_t PAGE_SIZE = 64 * 1024;
constexpr size_t ARENA_SIZE = 256 * 1024 * 1024;
constexpr size_t MAX_VIEWS = 32;
struct Backing {
	s64 offset;
	size_t size;
	sys_mem_id_t id;
};
}

struct MemArenaData {
	size_t size = 0;
	sys_mem_addr_t base = 0;
	Backing backing[MAX_VIEWS]{};
	sys_mem_addr_t views[MAX_VIEWS]{};
	size_t backingCount = 0;
	size_t viewCount = 0;
};

size_t MemArena::roundup(size_t size) {
	if (size > SIZE_MAX - (PAGE_SIZE - 1))
		return 0;
	return (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

bool MemArena::NeedsProbing() {
	return false;
}

bool MemArena::GrabMemSpace(size_t size) {
	ReleaseSpace();
	// A failed kernel cleanup must retain its bookkeeping for a later retry.
	if (data_ || !size || size > ARENA_SIZE)
		return false;
	data_ = new (std::nothrow) MemArenaData;
	if (!data_)
		return false;
	data_->size = roundup(size);
	return true;
}

u8 *MemArena::Find4GBBase() {
	if (!data_)
		return nullptr;
	if (!data_->base) {
		// LV2 reserves address ranges in 256 MB units. This reserves virtual
		// address space only; each PSP region gets its own physical allocation.
		int result = sysMMapperAllocateAddress(ARENA_SIZE, SYS_MEMORY_PAGE_SIZE_64K,
			ARENA_SIZE, &data_->base);
		if (result) {
			ERROR_LOG(Log::MemMap, "PS3 reserve memory arena failed: %08x", result);
			data_->base = 0;
			return nullptr;
		}
	}
	return reinterpret_cast<u8 *>(static_cast<uintptr_t>(data_->base));
}

void *MemArena::CreateView(s64 offset, size_t size, void *base) {
	size_t pages = roundup(size);
	uintptr_t address = reinterpret_cast<uintptr_t>(base);
	if (!data_ || !data_->base || offset < 0 || (offset & (PAGE_SIZE - 1)) ||
		!pages || pages > ARENA_SIZE || static_cast<u64>(offset) + pages > data_->size ||
		address < data_->base || address - data_->base > ARENA_SIZE - pages ||
		(address & (PAGE_SIZE - 1)) || data_->viewCount == MAX_VIEWS)
		return nullptr;

	Backing *backing = nullptr;
	for (size_t i = 0; i < data_->backingCount; ++i) {
		if (data_->backing[i].offset == offset) {
			backing = &data_->backing[i];
			if (backing->size != pages)
				return nullptr;
			break;
		}
		const Backing &other = data_->backing[i];
		if (offset < other.offset + static_cast<s64>(other.size) &&
			other.offset < offset + static_cast<s64>(pages))
			return nullptr;
	}
	bool created = !backing;
	Backing pending{offset, pages, 0};
	if (created) {
		if (data_->backingCount == MAX_VIEWS)
			return nullptr;
		int result = sysMMapperAllocateMemory(pages, SYS_MEMORY_PAGE_SIZE_64K, &pending.id);
		if (result) {
			ERROR_LOG(Log::MemMap, "PS3 allocate PSP region failed: %08x", result);
			return nullptr;
		}
		// Track it before mapping so even a failed map/free can be retried.
		backing = &data_->backing[data_->backingCount++];
		*backing = pending;
	}
	int result = sysMMapperMapMemory(static_cast<sys_mem_addr_t>(address), backing->id,
		SYS_MEMORY_PROT_READ_WRITE);
	if (result) {
		ERROR_LOG(Log::MemMap, "PS3 map PSP region failed: %08x", result);
		return nullptr;
	}
	data_->views[data_->viewCount++] = static_cast<sys_mem_addr_t>(address);
	return base;
}

void MemArena::ReleaseView(s64 offset, void *view, size_t size) {
	if (!data_)
		return;
	uintptr_t address = reinterpret_cast<uintptr_t>(view);
	for (size_t i = 0; i < data_->viewCount; ++i) {
		if (data_->views[i] != address)
			continue;
		sys_mem_id_t id;
		int result = sysMMapperUnmapMemory(data_->views[i], &id);
		if (result) {
			ERROR_LOG(Log::MemMap, "PS3 unmap PSP region failed: %08x", result);
			return;
		}
		data_->views[i] = data_->views[--data_->viewCount];
		return;
	}
	// Cached and uncached PSP pointers can refer to one view on 32-bit hosts.
}

void MemArena::ReleaseSpace() {
	if (!data_)
		return;
	for (size_t i = data_->viewCount; i > 0; --i)
		ReleaseView(0, reinterpret_cast<void *>(static_cast<uintptr_t>(data_->views[i - 1])), 0);
	if (data_->viewCount)
		return;
	for (size_t i = data_->backingCount; i > 0; --i) {
		int result = sysMMapperFreeMemory(data_->backing[i - 1].id);
		if (result) {
			ERROR_LOG(Log::MemMap, "PS3 free PSP region failed: %08x", result);
			continue;
		}
		data_->backing[i - 1] = data_->backing[--data_->backingCount];
	}
	if (data_->backingCount)
		return;
	if (data_->base) {
		int result = sysMMapperFreeAddress(data_->base);
		if (result) {
			ERROR_LOG(Log::MemMap, "PS3 release memory arena failed: %08x", result);
			return;
		}
	}
	delete data_;
	data_ = nullptr;
}
#endif
