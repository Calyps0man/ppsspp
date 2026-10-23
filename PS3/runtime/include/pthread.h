#pragma once

// PSL1GHT's pthread-embedded library exports C symbols. Its SDK header omits
// the linkage guard, so C++ callers must supply it.
#ifdef __cplusplus
extern "C" {
#endif
#include_next <pthread.h>
#ifdef __cplusplus
}
#endif
