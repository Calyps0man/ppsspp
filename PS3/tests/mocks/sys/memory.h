#pragma once
#include <cstddef>
#include <cstdint>
using sys_mem_addr_t = uint32_t;
using sys_mem_id_t = uint32_t;
constexpr uint64_t SYS_MEMORY_PAGE_SIZE_64K = 0x200;
constexpr uint64_t SYS_MEMORY_PROT_READ_WRITE = 0x40000;
int sysMMapperAllocateAddress(size_t, uint64_t, size_t, sys_mem_addr_t *);
int sysMMapperFreeAddress(sys_mem_addr_t);
int sysMemoryAllocate(size_t, uint64_t, sys_mem_addr_t *);
int sysMemoryFree(sys_mem_addr_t);
