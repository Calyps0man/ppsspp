# Native PS3 C++ runtime

The PSL1GHT SDK ships GCC 7.2.0 with a single-thread libstdc++. Full PPSSPP
requires threads, condition variables, exception guards and atomic shared
ownership. Rebuild the **complete** library with the pthread-embedded backend;
do not combine thread-enabled headers with the SDK's old libstdc++.a.

Restore the original GCC 7.2.0 release source (including libgcc and configure
support files). The companion native runtime archive includes the original
source release and the installed runtime. To rebuild:

```sh
export PS3DEV="$PWD/native-sdk/ps3dev"
export PSL1GHT="$PS3DEV"
python3 ppsspp/PS3/runtime/prepare.py gcc-7.2.0
bash ppsspp/PS3/runtime/build.sh gcc-7.2.0 native-runtime/build native-runtime/install
export PPSSPP_PS3_RUNTIME="$PWD/native-runtime/install"
cmake -S ppsspp -B build-ps3 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=PS3/psl1ght.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-ps3 --target Core
```

`prepare.py` must run once on a fresh source tree. It adapts the PTE handle
representation, replaces non-constexpr pointer sentinel initializers with real
initialization calls, and supplies C++17 character length. The include wrappers
restore C linkage for the SDK pthread header and expose actual newlib C99
functions in strict C++ modes. Weak pthread references are disabled so static
linking pulls in the real implementations.

`probe.cpp` checks mutex contention, shared ownership across threads, call_once,
condition signaling and timeouts, and C++17 string support. The native executable
compiles, links and passes Cell ELF post-link checks. It has **not run on a PS3**.
The same test passes on Linux; that validates the test, not the PS3 runtime.

The native Core archive and full headless executable now compile and link.
The toolchain resolves the rebuilt C++ runtime and real SDK pthread library in
one link group after target archives, and supplies the SDK link environment.
This does not establish console startup or playable games. Display, pad and
audio integration remain; CPU Test 07 remains the confirmed console baseline.

GCC/libstdc++ sources retain their GPLv3 and GCC Runtime Library Exception
licenses; see the original release's COPYING3 and COPYING.RUNTIME.
