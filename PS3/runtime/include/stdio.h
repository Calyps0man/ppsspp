#pragma once

// This older newlib hides its implemented C99 APIs in strict C++ modes.
#pragma push_macro("__STRICT_ANSI__")
#undef __STRICT_ANSI__
#include_next <stdio.h>
#pragma pop_macro("__STRICT_ANSI__")
