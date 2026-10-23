#!/usr/bin/env python3
"""Adapt GCC 7.2's complete libstdc++ to PSL1GHT pthread-embedded.

Run on a fresh GCC 7.2.0 source tree. The SDK itself is never modified.
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])

def replace(path, old, new, count=1):
	with path.open(newline='') as f:
		text = f.read()
	if text.count(old) != count:
		raise RuntimeError(f"Unexpected GCC source in {path}")
	with path.open('w', newline='') as f:
		f.write(text.replace(old, new))

# The compiler reports its original single-thread build. This separately built
# runtime uses the real pthread backend throughout, including exception guards.
replace(root / 'libstdc++-v3/configure',
	"target_thread_file=`$CXX -v 2>&1 | sed -n 's/^Thread model: //p'`",
	"target_thread_file=posix", count=2)

header = root / 'libgcc/gthr-posix.h'
replace(header, '#include <pthread.h>', '''#include <pthread.h>

/* PTE uses pointer sentinels for static mutex/condition initializers. They
   cannot initialize C++ constexpr objects; use its real initialization APIs. */
#define _GTHREAD_USE_MUTEX_INIT_FUNC 1
#define _GTHREAD_USE_RECURSIVE_MUTEX_INIT_FUNC 1
#define _GTHREAD_USE_COND_INIT_FUNC 1

#ifdef __cplusplus
/* PTE handles contain both an object address and a reuse generation. */
inline bool operator==(pthread_t a, pthread_t b) {
  return a.p == b.p && a.x == b.x;
}
inline bool operator<(pthread_t a, pthread_t b) {
  return (unsigned long)a.p < (unsigned long)b.p
    || (a.p == b.p && a.x < b.x);
}
#endif''')

# Thread IDs are opaque. PTE's struct handle has no ostream insertion operator.
replace(root / 'libstdc++-v3/include/std/thread',
	'return __out << __id._M_thread;',
	'return __out << _Hash_impl::hash(__id._M_thread);')

# GCC 7's string_view constructor requires constexpr character length, but
# its char_traits specialization predates that C++17 requirement.
replace(root / 'libstdc++-v3/include/bits/char_traits.h',
	'''static /* _GLIBCXX17_CONSTEXPR */ size_t
      length(const char_type* __s)
      { return __builtin_strlen(__s); }''',
	'''static _GLIBCXX17_CONSTEXPR size_t
      length(const char_type* __s)
      {
        size_t result = 0;
        while (__s[result] != char_type())
          ++result;
        return result;
      }''')

# Configure probes include gthr.h before generated target headers exist.
(root / 'libgcc/gthr-default.h').write_text('#include "gthr-posix.h"\n')
