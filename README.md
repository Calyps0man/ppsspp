PPSSPP for PS3
==============

An experimental port of [PPSSPP](https://www.ppsspp.org/), the PSP emulator by Henrik Rydgård
and contributors, to the PlayStation 3, running natively on the Cell processor and RSX.

The original PPSSPP README is in [README-PPSSPP.md](README-PPSSPP.md). This port is based on
PPSSPP's source and keeps its license, the GNU GPL 2.0 or later ([LICENSE.TXT](LICENSE.TXT)).
It is not an official PPSSPP release; please don't report bugs in this port to the PPSSPP project.

What's in the port
------------------

Everything PS3-specific is under [`PS3/`](PS3/), plus PS3 code paths in the core:

| Part | Where |
| --- | --- |
| Frontend: game list, settings menu, per-game settings, pad input, sound, logs, diagnostics | `PS3/frontend/` |
| PowerPC JIT (PSP MIPS → Cell PPU code, from PPSSPP's IR), with AltiVec VFPU code | `Core/MIPS/PPC/`, `Common/PPCEmitter.*` |
| JIT code memory (direct writes or PS3MAPI) | `PS3/jit/` |
| RSX hardware renderer: render targets, textures, shaders, RSX transform & lighting | `PS3/rsx/`, `GPU/Software/HardwareBackend.h` |
| GE worker thread and the CPU/RSX vertex paths | `GPU/Software/` (`SoftGpu`, `TransformUnit`, `BinManager`) |
| SPU programs: vertex decode and skinning, texture decode, SAS audio mixing | `PS3/spu/` |
| Thread-enabled C++ runtime for the PSL1GHT toolchain | `PS3/runtime/` |
| Host-side test harnesses (ppc64 Linux under qemu, replay benches) | `PS3/hostsim/`, `PS3/tests/`, `PS3/cpu-test/` |

The user-facing notes for every build, including all settings, are in
[`PS3/frontend/README.md`](PS3/frontend/README.md) and
[`PS3/frontend/games-README.txt`](PS3/frontend/games-README.txt).

Status
------

- Runs commercial games on a real PS3 with the JIT and the RSX renderer. Speed depends a lot on
  the game; heavy 3D scenes can still drop well below full speed.
- Video cutscenes (PMF/MPEG), MP3 and AAC play through FFmpeg (since Frontend 59). Not done yet: networking.
- Requires a PS3 that can install homebrew packages (CFW or HEN). PSP games are not included;
  use your own dumps in `/dev_hdd0/PSPISO`.

Building
--------

See [`PS3/BUILDING.md`](PS3/BUILDING.md).

Credits
-------

PPSSPP by Henrik Rydgård and the PPSSPP contributors. PS3 toolchain and libraries by the
[ps3dev](https://github.com/ps3dev) project (PSL1GHT, ps3toolchain, ps3libraries) and
[SDL_PSL1GHT](https://github.com/shagkur/SDL_PSL1GHT). Third-party licenses are listed in
[`PS3/cpu-test/THIRD_PARTY.txt`](PS3/cpu-test/THIRD_PARTY.txt).
