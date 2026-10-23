#pragma once

// Newlib hides its implemented C99 integer limits under strict C++ modes.
// Lua must see the same long long width in C and C++ translation units.
#pragma push_macro("__STRICT_ANSI__")
#undef __STRICT_ANSI__
#include_next <limits.h>
#pragma pop_macro("__STRICT_ANSI__")

// GCC 7's target limits header tests __STDC_VERSION__ even for C++17.
// Supply standard limits from the compiler's actual long long representation.
#ifndef LLONG_MAX
#define LLONG_MAX __LONG_LONG_MAX__
#define LLONG_MIN (-LLONG_MAX - 1LL)
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#endif
