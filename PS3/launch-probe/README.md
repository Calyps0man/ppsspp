# PS3 launch probe 04

The CPU Test 02 package returned to XMB and the user found no logs. That does not establish whether failure happened before main or whether stdio logging failed.

This control app contains no PPSSPP interpreter, SDL, C++, or stdio calls. It uses direct LV2 file syscalls, an early constructor (priority 101, before SDK heap/stdio constructors), and a main entry marker. It deliberately returns to XMB after 3 seconds. The normal SDK startup/runtime is still linked; this does not bypass the console loader or prove it will launch.

Install over PPSCPU001, leave a writable FAT32 USB drive connected before launching, then look for `PPSCPU04.txt` at the USB root. It attempts all ports `/dev_usb000` through `/dev_usb007`, without relying on libc stat or mount detection. Other attempted paths:

* `/dev_hdd0/game/PPSCPU001/USRDIR/PPSCPU04.txt`
* `/dev_hdd0/tmp/PPSCPU04.txt`

Expected lines: early constructor reached; main reached; simple PPU arithmetic PASS; returning to XMB intentionally. An early-only log narrows failure to later SDK initialization. A main marker shows the basic C control launched. No file remains inconclusive because all file writes may have failed, or launch/startup may have failed earlier.

Build with the complete ps3dev SDK nightly-2026-07-26:

```sh
PS3DEV=/absolute/path/to/ps3dev bash PS3/launch-probe/build.sh ./launch-build
```

Compiled with warnings as errors and packaged; not hardware tested. Source is GPL-2.0-or-later, see ../../LICENSE.TXT.

## Probe 03 hardware result and Probe 04

On 4.93 CFW, the user found only the early-constructor line from Probe 03. This proves the loader reached user code and the direct filesystem write succeeded. The expected main marker was absent, so the next scope is SDK startup after the early constructor. It does not yet identify a specific failing function.

Probe 04 records constructor priorities 102 through 106, the r13 TLS register, and entry/return markers around SDK spinlock, heap, libc reentrancy state, lightweight-mutex creation, and cwd strdup. Linker wrapping forwards the original calls unchanged. A wrapped abort records its caller address and exits directly. Disassembly and the constructor table were inspected to verify wrapper calls and relative marker order. It is a diagnostic control, not a runtime fix or PPSSPP test.

Its log is PPSCPU04.txt at the same USB/HDD locations. Send the entire log. An early-only log is still useful. Normal completion still returns to XMB intentionally. The previous probe source and build script are retained as previous-03-main.c and previous-03-build.sh.

## Import fixup discovery

Probe 04 on 4.93 CFW stopped at ENTER SDK spinlock initialize. Its r13 value was 0x30007060. Inspection found the distributed ELF omitted sprxlinker and had sysPrxForUser import count zero. The standard PSL1GHT ppu_rules runs strip and sprxlinker before SELF creation; this required step was missing in the custom scripts. The current build script is corrected and verifies the resulting import tables and descriptor fixups. Historical previous-03-build.sh deliberately retains the old build as a failure reproduction. CPU Test 05 incorporates the corrected pipeline and early raw logging; hardware confirmation is pending.
