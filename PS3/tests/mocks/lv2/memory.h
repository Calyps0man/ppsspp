#pragma once
#include <sys/memory.h>
int sysMMapperAllocateMemory(size_t, uint64_t, sys_mem_id_t *);
int sysMMapperFreeMemory(sys_mem_id_t);
int sysMMapperMapMemory(sys_mem_addr_t, sys_mem_id_t, uint64_t);
int sysMMapperUnmapMemory(sys_mem_addr_t, sys_mem_id_t *);
