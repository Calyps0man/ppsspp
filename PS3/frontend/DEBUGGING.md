# Finding bugs in PPSSPP PS3

This is how the PS3 port's logs work, what each switch on the **Debug** tab adds to them, and which
switches to use for which kind of bug. It is the same method used to fix the bugs listed in
README.txt: turn on the group of lines for the part of the emulator you suspect, reproduce the bug
once, and read what the log says happened just before it.

Leave **Debug off for normal play.** Every log line is written to several files and flushed to
disk, which costs time; some switches slow games down a lot more (noted below).

## Where the logs are

With Debug on, each game run gets its own folder, emptied when the game starts:

| File | What it holds |
|---|---|
| `USRDIR/logs/<game>/launch.log` | The frontend's log: everything on the Debug tab goes here |
| `USRDIR/logs/<game>/core.log` | PPSSPP's own log (Core logging, see below) |
| `USRDIR/logs/<game>/stdout.log` | Standard output |
| `USRDIR/logs/<game>/settings.ini` | The settings that run used |
| `memstick/PSP/SYSTEM/DUMP/` | GE dumps (`.ppdmp`) with `.ram` and `.vram` files next to them |

launch.log is also written to `USRDIR/launch.log`, `/dev_hdd0/tmp/PPSSPP_PS3.log` and, if a USB
drive is plugged in, `PPSSPP_PS3.log` on it. So a log survives even when the PS3 has to be
switched off.

`USRDIR` is `/dev_hdd0/game/PPSSPP001/USRDIR`.

## The Debug tab

Every switch can also be set as a line in `settings.ini` (all games) or `USRDIR/games/<game>.ini`
(one game: Triangle on a game, View/edit game config). The ini key is in brackets.

### Logs (left column)

| Switch | Lines in launch.log | Default | Cost |
|---|---|---|---|
| **Debug** (`debug`) | The master switch. Off: no logs at all, and none of the switches below do anything. On: launch.log, plus in game hold L3 JIT profile, L2+R2 dump. (The START+SELECT menu and L3+R3 FPS toggle are switches of their own in Advanced since build 212.) | Off | Small, but log lines are slow to write |
| **Core logging** (`log`) | core.log: PPSSPP's own messages. Warnings = errors and warnings only; Full debug = everything. A separate switch: turning Debug on does not turn it on (it only works while Debug is on). Switching either off stops core.log at once | Off | Warnings: small. Full debug: very slow, games may not be playable |
| **Perf report** (`perflog`) | `Perf:` lines every 10 s (or 2 s): game and shown frame rate, speed, per-frame time split into PSP CPU / GE / drawing / waiting, GE list thread, memory, JIT compiling, busiest HLE calls, sound | Every 10 s | Small |
| **CPU/GE timing** (`geprofile`) | Adds `Perf: display lists (geprofile...)` with PPSSPP's own GE time, and times every HLE call | Off | Slightly slower |
| **Stall reports** (`stalllog`) | `STALL:` lines when no frame comes for 5 s: each PSP thread and what it waits for, the code at the stuck pc with registers, the last HLE calls, the GE worker, memory, the save dialog step | On | None until a stall |
| **Video statistics** (`videolog`) | `Video:` lines once a second while a video plays: pictures decoded, audio, disc reads into the buffer, video/audio time; `Video threads`: what the game's threads did at each vblank; `Video GE`: GE syncs and waits; `Video GE lists`: GE time per list run | On | Small, only during videos |
| **Save dialog steps** (`savelog`) | `Savedata:` lines: each dialog step and screen, sizes and free space reported to the game, IO start and end with times, list reloads | On | None |
| **GE list watch** (`gewatchlog`) | `GE list watch:` code that wrote over display list memory the GE hadn't finished with; `GE: sub-list in VRAM` contents and changes | On | Small |
| **RSX renderer events** (`rsxlog`) | `RSX renderer:` new framebuffers and their layouts, a buffer drawn in a new layout, write-backs to PSP memory, depth reads, textures read from drawn buffers, unusual blends (each kind capped) | On | Small |
| **JIT events** (`jitlog`) | `JIT cache full` (all code compiles again: a hitch), blocks the JIT had to compile again | On | None |
| **Sound debug** (`snddebug`) | `Sound:` lines every 2 s while there is sound: the final mix before the Volume setting (peak, samples over full scale), each audio channel (its volume, the game's samples and how many were already at full scale), SAS (voices summed, the effect send, the game's own buffer, what was written and how much clipped) with the loudest voices' volumes and envelopes, ATRAC decoder output. "Log, voices on the PPU" also mixes SAS voices without the SPU, to compare | Off | Small, a few lines every 2 s |

These lines are always written with Debug on, with no switch of their own: startup and settings,
out-of-memory and low-memory events, RSX setup and errors, sound start and stop with underruns,
the slow-frame diagnostics (`Slow:`, when a frame takes over 1/3 s), NaN camera warnings,
`ASSERT failed`.

### Finding a bug (right column)

| Switch | What it does | Needs |
|---|---|---|
| **L2+R2 in game** (`l2r2`) | GE dump: the next frame's graphics commands are saved, to replay on a PC. Draw log: the next 3000 draws are written to launch.log as text instead (`DRAW prim`, `DRAW transfer`...), which needs no big memory | Debug |
| **Dump: PSP RAM** (`dumpram`) | With each GE dump, PSP RAM at the end of that frame as `.ram` next to it: the game's code and data, to disassemble or compare | Debug, GE dump |
| **Dump: RSX picture** (`dumpvram`) | With each GE dump, what the RSX drew in that frame as `.vram` next to it: to compare with the software renderer's result | Debug, GE dump, RSX renderer |
| **JIT profile** (`jitsample`) | Where the PSP code spends its time: `JIT profile:` lines listing the busiest blocks with their MIPS, IR and PPC code. Samples at 40 s and every minute after (5 in all); hold L3 for a 5 s sample any time | Debug, JIT. A little slower |
| **NaN watch** (`nanwatch`) | `NaN watch #n:` names the code that first makes each NaN (not-a-number) float, with registers and disassembly. 1 FPU, 3 also VFPU, 5 also infinities, 7 all | Debug, IR interpreter. VFPU is very slow |
| **Store watch** (`storewatch`) | `Store watch #n:` names the code that wrote vertex colours a draw finds half-written | Debug, IR interpreter. Free when off |
| **JIT: interpret** (`jitgeneric`) | The JIT leaves a group of instructions to the interpreter: 1 integer arithmetic, 2 integer loads/stores, 4 float/vector loads/stores, 8 FPU arithmetic, 16 integer<->float moves, 32 vector ops, 64 no fused patterns. In an ini, add numbers to combine groups | JIT. Slower |
| **JIT: interpret ops** (`jitops`) | The same for single instructions, by IR op name: `jitops=Madd,Min,Max`. Only in an ini file (it is text); the tab shows it when set | JIT |

## Which switches for which bug

**The game freezes.** Debug on, Stall reports on. Reproduce the freeze and wait 10 seconds before
quitting. The first `STALL: PSP threads` line says what every thread is waiting for: a semaphore,
the GE, audio, a file read, a dialog. `STALL: code at pc` shows where a busy thread is stuck. If a
dialog was open, `STALL: save dialog at step` says which step.

**The game is slow.** Debug on, Perf report every 10 s. Play the slow part for 30 s. `Perf: game
frames ... per frame:` says whether the time goes into the PSP CPU, the GE (drawing) or waiting. If
it's the CPU, turn on JIT profile to see which game code is busiest; if it's the GE, the `Perf:`
lines about the GE worker and transform show which part. Compare a run with the setting you
suspect turned off.

**A video is jerky or freezes.** Debug on, Video statistics on. Each `Video:` line covers one
second: at full speed a 30 fps video decodes about 30 pictures. Fewer pictures with many "calls
without one" means the data isn't there (disc reads); 30 pictures but a jerky picture means the
display side: `Video GE` shows how far ahead the GE syncs are scheduled and how long the game waits
for them, and `Video threads` which thread waits for what. (Midnight Club L.A.'s intro: 30 pictures
a second, but GE syncs scheduled about 206 ms ahead.)

**Save or load problems** ("not enough space", a dialog that never finishes). Debug on, Save dialog
steps on. `Savedata: sizes` shows the free space and sizes the game was told; the IO lines show
whether reads and writes finished and how long they took.

**Graphics are wrong.** First switch Basic options > Renderer to Software and try again:
- Still wrong with Software: an emulation bug. Debug on, then press L2+R2 (GE dump) on a wrong
  frame. The dump replays in `PPSSPPHeadless` built from this repository on a PC, where the frame
  can be examined draw by draw. GE list watch helps when the picture turns to garbage (display lists
  overwritten).
- Fine with Software: an RSX renderer bug. RSX renderer events on, and a GE dump: its `.vram` file
  is what the RSX drew, to compare with the software result. Try the Advanced tab (Screen copies,
  Write-back, Texture check, RSX tiling, Draw merging): each goes back to how an older build did
  it, so the one that fixes it names the part that's wrong.
- If the GE dump says it was skipped or dropped for memory, set L2+R2 to Draw log.

**Something is wrong only with the JIT** (it's fine with Basic options > CPU core: IR interpreter, as
Call of Duty's rainbow wall was before build 187). Try JIT: interpret with each group in turn. When one group makes the bug go away, the bug is in the JIT's code for that group; then narrow
it to single instructions with `jitops=` in the game's ini file, a few op names at a time. Report
the group and the ops.

**The camera breaks, objects fly away or disappear, the screen goes black in 3D.** Often a NaN.
CPU core IR interpreter, NaN watch FPU (then FPU + VFPU if nothing shows). The first `NaN watch`
line names the instruction and code block that made it.

**Models flicker or have wrong colours from frame to frame.** CPU core IR interpreter, Store watch
on.

**A hitch every so often with the JIT.** JIT events on: `JIT cache full` lines line up with the
hitches when the JIT cache fills. Advanced > JIT cache: Large gives it more room.

**Errors from the game's system calls** (missing features, wrong arguments). Core logging:
Warnings, then read core.log for `HLE`, `sce` and `Unimplemented` lines.

## Reporting a bug

Send the whole `USRDIR/logs/<game>/` folder of a run where the bug happened (it has the settings
used), the GE dump for graphics bugs, and the build number (top right of the main screen). Say
what you did and roughly when in the run it went wrong.

## Developer switches on a PC

The PC build of `PPSSPPHeadless` (this repository, built for the host) has more switches, set as
environment variables. They are not compiled into the PS3 build. The ones used most:

| Variable | What it does |
|---|---|
| `PPSSPP_DRAWLOG` | Every draw: primitive, state, vertex and index addresses |
| `PPSSPP_DRAWCOLS` | With DRAWLOG: vertex colours |
| `PPSSPP_DRAWVERTS=N` | With DRAWLOG: the first N vertices of each draw |
| `PPSSPP_CMDLOG` | GE commands as they run |
| `PPSSPP_LISTCYCLES` | GE time charged per display list |
| `PPSSPP_FILLLOG` | Time estimates for video and block transfers |
| `PPSSPP_FORCEVIDEO` | Treats transfers as video frames (to test the video paths) |
| `PPSSPP_MIPSDIS=<ram>,<hexaddr>,<count>` | Disassembles PSP code from a `.ram` file of a GE dump |
| `PPSSPP_REVIVELOG` | JIT blocks brought back after the game rewrote memory |
| `PPSSPP_RSX_SIM=2` | Runs the RSX renderer's logic on the PC (no RSX) to find renderer bugs |
| `PPSSPP_NANWATCH`, `PPSSPP_JITGENERIC`, `PPSSPP_JITOPS` | The same as the ini keys above |

A frame dump from the PS3 replays with
`PPSSPPHeadless --graphics=software <file>.ppdmp`. The source code has the rest (search for
`getenv("PPSSPP_`).
