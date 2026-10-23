# Native PPSSPP headless executable

This target links the full PPSSPP Core, IR interpreter, HLE and software GPU
against the actual PPU SDK and rebuilt threaded C++ runtime. It is a command-line
headless entry point, with no PS3 display/pad/audio frontend. It has not run on a
PS3 or booted a game on a PS3. It is not an installable XMB emulator PKG.

Use CMake 3.21 or newer, Ninja, Python 3, the PSL1GHT SDK and the complete
threaded runtime described in runtime/README.md. Stock SDK libstdc++ is unsuitable.

```sh
export PS3DEV=/absolute/path/to/ps3dev
export PPSSPP_PS3_RUNTIME=/absolute/path/to/native-runtime/install
bash PS3/build-headless.sh /absolute/path/to/build-ps3 /absolute/path/to/output
```

Optional CMAKE and NINJA variables select host tool paths; JOBS selects build
parallelism. The toolchain supplies PSL1GHT to the linker. The script keeps the
raw linker output intact, fixes a copy with sprxlinker, then verifies Cell OSABI,
import counts and native function descriptors. Dependency checks stay enabled.
Never package a raw, unprocessed ELF as EBOOT.BIN.

Native link adaptations use real SDK pthread/sysfs functions and glslang's
pthread OS implementation. Vulkan is unavailable on PS3. File reads serialize
seek/read with 64-bit offsets. sendmsg gathers vectors into one real send/sendto;
multiple vectors require a temporary allocation and may fail with ENOMEM. ZIP
secure randomness uses LV2 in chunks no larger than 4096 bytes and propagates
failure. Native FFmpeg remains disabled for this build.

Next is a PS3 entry point with visible startup/error logs, game selection,
software framebuffer presentation, controller input and audio, followed by
console validation. Native memory mapping, TLS/thread lifecycle, socket
behavior, filesystem IO and floating-point behavior still need hardware checks.
No HLE dispatch indices or savestate serialization layouts were changed.
