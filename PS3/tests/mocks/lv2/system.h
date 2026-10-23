#pragma once
#include <stdint.h>
typedef uint64_t u64;
#define RANDOM_NUMBER_MAX_SIZE 4096
#ifdef __cplusplus
extern "C" {
#endif
uint32_t sysGetRandomNumber(void *, uint64_t);
#ifdef __cplusplus
}
#endif
