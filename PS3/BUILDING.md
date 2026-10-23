# Building PPSSPP for PS3

Builds are done on Linux (x86-64; WSL works). The output is an installable `.pkg`.

## 1. Toolchain and libraries

Build the ps3dev toolchain, PSL1GHT and ps3libraries. The port was built with these commits
(newer ones will most likely work too):

| Repository | Commit |
| --- | --- |
| https://github.com/ps3dev/ps3dev | `bc09c6292f1be95be874cfda9226ea9ba9ceaef3` |
| https://github.com/ps3dev/PSL1GHT | `f649a08fd536a9e27c08c7db2d93a2d7ee4c3bbe` |
| https://github.com/ps3dev/ps3libraries | `93c0200bb96ff571f80918a247e16a8a93e5e8ff` |
| https://github.com/shagkur/SDL_PSL1GHT (SDL2) | `b81fbad4489918f5ed9c9c052238d7a25da9bab9` |

Follow the ps3dev README (it builds binutils, GCC 7.2.0 and newlib for the PPU and SPU, then
PSL1GHT and the libraries). Then build SDL_PSL1GHT (`script.sh`, `make`, `make install`) so that
`$PS3DEV/portlibs/ppu/lib/libSDL2.a` exists.

```sh
export PS3DEV=/path/to/ps3dev        # contains ppu/, spu/, bin/, portlibs/
export PSL1GHT=$PS3DEV
```

You also need CMake 3.21 or newer, Ninja and Python 3.

## 2. Thread-enabled C++ runtime

PSL1GHT's GCC ships a single-threaded libstdc++; PPSSPP needs `std::thread`, `std::mutex`,
condition variables and so on. Rebuild libstdc++ from the GCC 7.2.0 release source against the
SDK's pthread library (the SDK itself is not modified):

```sh
wget https://ftp.gnu.org/gnu/gcc/gcc-7.2.0/gcc-7.2.0.tar.xz
tar xf gcc-7.2.0.tar.xz
python3 ppsspp/PS3/runtime/prepare.py gcc-7.2.0          # once, on a fresh source tree
bash ppsspp/PS3/runtime/build.sh gcc-7.2.0 runtime-build runtime-install
export PPSSPP_PS3_RUNTIME=$PWD/runtime-install
```

See [`runtime/README.md`](runtime/README.md) for what `prepare.py` changes.

## 3. FFmpeg (video, MP3 and AAC)

PSP video (PMF/MPEG cutscenes), MP3 and AAC go through FFmpeg, built from PPSSPP's FFmpeg tree
(FFmpeg 3.0.2 with the scripts PPSSPP uses):

```sh
git clone --depth 1 https://github.com/hrydgard/ppsspp-ffmpeg.git
bash ppsspp/PS3/ffmpeg/build.sh ppsspp-ffmpeg ffmpeg-build ffmpeg-ps3
export PPSSPP_PS3_FFMPEG=$PWD/ffmpeg-ps3
```

Without `PPSSPP_PS3_FFMPEG` the emulator builds without FFmpeg (no video or MP3/AAC sound). ATRAC3/3+
sound doesn't need it.

## 4. Source

```sh
git clone <this repository> ppsspp
cd ppsspp
git submodule update --init
```

## 5. Build the package

```sh
JOBS=4 bash PS3/frontend/build.sh ../build-ps3 ../ps3-artifacts
```

This configures CMake with `PS3/psl1ght.cmake`, builds the `PPSSPPPS3` target, and writes
`../ps3-artifacts/PPSSPP_PS3_Frontend_<version>.pkg` (the version is set at the top of
`build.sh` and in the window title in `PS3/frontend/Main.cpp`). A full build takes a while;
later builds only rebuild what changed.

Install the pkg with the PS3's package installer. Put PSP games (ISO/CSO) in `/dev_hdd0/PSPISO`.

## Generated files (only when you change their sources)

These are committed, so a normal build doesn't need the tools below.

- **RSX shaders** (`PS3/rsx/RSXShaders.inc`) come from `PS3/rsx/make-shaders.py`, which runs
  PSL1GHT's `cgcomp`. The transform & lighting vertex program is longer than stock cgcomp's
  256-instruction limit, so build cgcomp with
  [`tools/cgcomp-512-instructions.patch`](tools/cgcomp-512-instructions.patch) applied
  (`patch -p1` in the PSL1GHT tree, then rebuild `tools/cgcomp`). The patch only raises the
  limit; output for shorter programs is identical. `PS3/rsx/check-tl.py` checks the RSX
  transform & lighting program against the CPU path.
- **SPU programs** (`PS3/spu/*.inc`): `PS3_SDK=$PS3DEV bash PS3/spu/build.sh`.

## Testing without a PS3

- [`tools/ppc64-test/run.sh`](tools/ppc64-test/run.sh) builds the PPC JIT for 64-bit PowerPC Linux
  and runs the PSP CPU tests (pspautotests) under `qemu-ppc64`, comparing the JIT with the IR
  interpreter. Instructions are at the top of the script.
- `PS3/tests/` checks the PS3 memory, socket and file adapters against Linux models
  (`bash PS3/tests/run-host.sh`).
- The same tree builds on a PC as PPSSPPHeadless (`cmake -S . -B build -G Ninja -DHEADLESS=ON
  -DHEADLESS_CROSS=ON -DUSE_FFMPEG=OFF`), with environment switches for replaying GE dumps through
  the PS3 vertex paths (`PPSSPP_NULL_BACKEND`, `PPSSPP_TLSUM`, `PPSSPP_TL_DUMP`, ...).

## License

GPL 2.0 or later, as PPSSPP ([`LICENSE.TXT`](../LICENSE.TXT)). The rebuilt libstdc++ keeps its
GPLv3 with the GCC Runtime Library Exception. Third-party components are listed in
[`cpu-test/THIRD_PARTY.txt`](cpu-test/THIRD_PARTY.txt).
