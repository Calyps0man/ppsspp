# PS3 adapter checks

Run `bash PS3/tests/run-host.sh` from the repository. The test runner compiles
and runs the production PS3 memory adapters against a Linux syscall model.
Shared POSIX mappings exercise VRAM alias writes; injected failures check
cleanup retries, bounds, page rounding and unsupported permissions. This
model does not establish that LV2 permits the same mappings on real PS3.

The PS3 build must compile these files with the installed PSL1GHT headers and
the rebuilt pthread-enabled C++ runtime. Never ship the mock headers or their
implementations with the emulator. Native executable allocation is currently
unsupported in this model (the PowerPC JIT is tested with PS3/tools/ppc64-test).

Regression risks: insufficient physical memory, LV2 mapping/alias restrictions,
failed cleanup during shutdown, thread lifecycle, clock initialization order,
and filesystem errors. Console testing remains required before distributing
a game-loading PKG. CPU Test 07 is the working console baseline.

The postoffice socket model also checks connection cleanup, partial packets,
would-block, EOF, EINTR and bounded polling with cancellation. Native IPv6 is
explicitly unsupported. PS3 configuration generates an IPv4-only copy of the
pinned postoffice sources in the build directory via PS3/prepare-postoffice.py;
the upstream submodule is left intact. Overlay preparation fails if the expected
source patterns change. The eight host groups are models, not console tests.

Native executable link checks add sendmsg gathering (one real send/sendto per
message), positioned file reads with a mutex around seek/read, and LV2 ZIP
randomness. All eleven host groups are API models. Secure randomness propagates
syscall failure; only the separate non-cryptographic ZIP API has a fallback.
The native headless ELF link and Cell import/function-descriptor checks are
build checks. They do not establish console startup or game compatibility.
