#pragma once

// Expose newlib's implemented C99 conversion APIs to libstdc++ probes.
#pragma push_macro("__STRICT_ANSI__")
#undef __STRICT_ANSI__
#include_next <stdlib.h>
#pragma pop_macro("__STRICT_ANSI__")
