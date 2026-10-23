# PPSSPP PS3

Up to build 137 this app was called "PPSSPP PS3 Frontend". From build 138 on it is just
"PPSSPP PS3" (the same app: it installs over the old one and keeps your settings and logs).

## What's new in build 270 (ver 0.1)

Build 268 plus the Killzone fix tested in 269 diag, and menu changes (folder PPSSPP001: installs over 268, keeps your settings, configs and saves). Two packages as before: PPSSPP_PS3_build_270.pkg (custom firmware) and PPSSPP_PS3_build_270_HEN.pkg (HEN). Build 269 was only a diag build.

- Fix: Killzone's walls turning white for half a second or more at a time (GE thread and SPU vertices on). Vertices without a normal took the last normal decoded, and each SPU kept its own, left over from other draws, which differed from the PPU's. Where a draw doesn't use normals (no lighting, environment mapping or texture matrix from the normal), such vertices now get (0, 0, 1) on the SPUs and the PPU alike. Your 269 diag log: 55,292 draws checked, 0 differences (268 diag: about half differed). Draws that use normals are unchanged.
- Basic options tab, right column, top to bottom: CONFIG PER GAME (Load from a game, Save for a game), RESET, CONTROLS, PSP SYSTEM, AUDIO. The left column is as in 268.
- Triangle on a game: "View/edit game config" (X on a line edits it) replaces "View game config" and "Edit game config". A game without a config has "New game config".

## What's new in build 269 (ver 0.1)

Build 268 plus a fix for Killzone's white walls and two menu changes. Built as a diag build first (see below).

- Killzone's walls turning white for half a second or more at a time (with GE thread and SPU vertices on). The 268 diag log: of 240,565 draws checked, 116,108 got different vertices from the SPUs than from the PPU, and the only difference was ever the normals, on draws whose vertices have no normal and that don't use one (no lighting) - most of Killzone's world. A vertex without a normal takes the last normal decoded; each SPU kept its own, left over from other draws (e.g. 0, 10, 21), while the PPU had another. Such vertices now get the normal (0, 0, 1) when the draw doesn't use normals (no lighting, environment mapping or texture matrix from the normal), on the SPUs and on the PPU alike. Draws that use normals are unchanged.
- Basic options tab: CONFIG PER GAME (Load from a game, Save for a game) is now the first section, at the top of the left column, so the cursor starts on Load from a game. To make room, CPU clock, PSP memory and PSP fonts moved to PSP SYSTEM in the right column. The right column is now CONTROLS, PSP SYSTEM, RESET and AUDIO last.
- Triangle on a game: "View game config" is "View/edit game config" (X on a line already edited it), and "Edit game config" is gone. A game without a config has "New game config" instead.

## Build 269 diag

Build 269 as a separate app (folder PPSSPP002, "PPSSPP PS3 ver 0.1 (build 269 diag)", Debug on from the start), with the SPU vertex check of 268 diag on one draw in 8 (instead of every draw, which was slower than SPU vertices off). With the fix, the check should find no differences: launch.log "SPU vertex check: N draws checked, 0 differ". Packages: PPSSPP_PS3_build_269_diag.pkg (custom firmware) and PPSSPP_PS3_build_269_HEN-diag.pkg (HEN).

## Build 268 diag (diagnostic, installs next to the normal app)

Build 268 plus an SPU vertex check, for Killzone's white walls (they come and go for half a second or more; SPU vertices Off makes most of it go away). Installs as a separate app (folder PPSSPP002, "PPSSPP PS3 ver 0.1 (build 268 diag)"), with its own settings and logs, and Debug on from the start. Packages: PPSSPP_PS3_build_268_diag.pkg (custom firmware) and PPSSPP_PS3_build_268_HEN-diag.pkg (HEN).

- Every 3D draw whose vertices the SPUs prepared is decoded on the PPU too, and the two are compared (position, normal, color, texture coordinates, indices). When they differ, the draw uses the PPU's vertices (so the white walls should be gone in this build) and launch.log gets a line "SPU vertex check: job ... differs" with the details (up to 48 such lines), plus a count every 120 frames. Slower than the normal build (each such draw is decoded twice).

## What's new in build 268 (ver 0.1)

Build 267 with menu text fixes and a new icon (folder PPSSPP001: installs over 267, keeps your settings, configs and saves). Two packages as before: PPSSPP_PS3_build_268.pkg (custom firmware) and PPSSPP_PS3_build_268_HEN.pkg (HEN).

- Basic options tab: the heading is "CONFIG PER GAME" (was "PER GAME"), and "Load from a game" is above "Save for a game".
- The help text of "Load from a game" was cut off; it is shorter now and fits. A check (PS3/frontend/check-help.py) now makes sure every help text fits the 3 lines of the help panel.
- The XMB icon says "PPSSPP for PS3" (the badge said only "PS3").

## What's new in build 267 (ver 0.1)

Build 265 plus one new row (folder PPSSPP001: installs over 265 or 266, keeps your settings, configs and saves). Nothing from test build 266 is in it (its "Wait for drawing" didn't stop Killzone's flicker). Two packages as before: PPSSPP_PS3_build_267.pkg (custom firmware) and PPSSPP_PS3_build_267_HEN.pkg (HEN).

- Basic options tab, under CONFIG PER GAME: "Load from a game", above "Save for a game". Press X and choose the game (games that have a config are marked [config] in the list): its config (USRDIR/games/<game>.ini) is loaded into the Basic options and Advanced tabs, so you can change it in the menu instead of editing the text. Then use Save for a game to write it back. The tabs are also the settings for games without a config, so those change with it (Reset to defaults puts them back). A hand-written config that lists only some settings changes only those.

## What's new in build 265 (ver 0.1)

Build 264 (folder PPSSPP001: installs over 264, keeps your settings, configs and saves), in two packages: PPSSPP_PS3_build_265.pkg for custom firmware (signed as before) and PPSSPP_PS3_build_265_HEN.pkg for official firmware with HEN (4.89 and later; "PPSSPP PS3 ver 0.1 (build 265 HEN)" in the XMB). Both install to the same folder. Nothing changes in how games run.

- The game list's hint line shows "SELECT: JIT check" (SELECT on the game list checks whether the fast CPU JIT can run; it always did, but nothing on screen said so).
- With HEN the app didn't start: "An error occurred during the start operation" (80010009). Its EBOOT.BIN is fake-signed (key revision 0x8000), which only custom firmware accepts. In the HEN package it is a retail-signed NPDRM SELF with a free license, as PSL1GHT's own "make pkg" makes it, and its content ID has the standard 36 characters (UP0001-PPSSPP001_00-PPSSPPPS3FR00265). Both signed files are compressed: ~10 MB instead of 32 MB.
- "Quit to game list" starts RELOAD.SELF, which the HEN package brings retail-signed (with RELOAD.BLD); the app doesn't write it. If HEN refuses to start it, the app ends to the XMB instead.
- Not tested on HEN here. The fast CPU JIT needs either direct writes to its code area or PS3MAPI; launch.log says which it found ("JIT: available - ..."); without either the app uses the IR interpreter by itself (much slower; "JIT: NOT available, using the IR interpreter").
- The build script makes either package: PS3_SIGN=hen PS3_EDITION=HEN for HEN, nothing for custom firmware (signed exactly as before).

## What's new in build 264 (ver 0.1)

Build 263 (the normal app, folder PPSSPP001: installs over 263, keeps your settings, configs and saves), plus:

- The Third Birthday's glow without copying pictures back from the RSX. The game reads its screen through a palette to build the glow: the whole 8888 screen as a 32-bit palette texture with the index 12 bits in (shift 12, mask 15), then a small 4444 buffer as a 16-bit palette texture. The RSX path only did 32-bit palette reads at whole-byte shifts, so each of these (6 a frame in your dumps) waited for the RSX, copied the picture back to PSP memory, converted it on the CPU and uploaded it again - most of the drawing thread's ~22 ms a frame that the game waited for (your 262/263 logs). Now a palette texture read from a framebuffer with the texel being one pixel (16-bit palette reads of 16-bit buffers; 32-bit ones of 8888 buffers at any shift with the index inside one byte) is done on the RSX by a new fragment program. The palette reads that worked before go the way they always did; none of the 129 test dumps of other games takes the new path. Checked: the program's arithmetic on 15,000 random pixels (8888, 4444, 565, 5551; RSX reciprocals off by up to 4 parts in a million) gives the exact palette entry.
- Reset to defaults is on the Settings tab (heading RESET, no longer on Advanced) and sets every option on the Settings and Advanced tabs back to its default (not the Debug tab; settings saved for a game are not changed).

## What's new in build 263 (ver 0.1)

Build 262 (the normal app, folder PPSSPP001: installs over 262, keeps your settings, configs and saves), plus:

- GE dumps (L2+R2) are recorded much faster. To keep a dump small, the recorder looked for each piece of data (vertices, palettes, textures) in everything recorded so far, byte by byte; one frame of Midnight Club was ~3800 such searches over up to 7.7 MB, ~15 GB of searching - 20+ s on the PS3 (build 257: then the PS3 hung; 261-262: given up after 8 s). Repeated data is now found by a hash: a second or so for a frame like that. Data that only appears inside a bigger piece recorded earlier is stored again (dumps ~1-2% larger). Checked on a PC: 15 dumps of 5 games recorded again while playing replay to the same pictures.
- A GE dump no longer ends at a screen change with nothing drawn in between (your Midnight Club dump in 262 held no drawing at all): the palette saved at the start counted as drawing.
- The time limit for recording a dump is 10 s (was 8), only a safety net now.
- Advanced > Reset to defaults resets every option on the Advanced tab. It used to reset only the older performance switches (GE thread, SPU, tiling...), which are almost always at their defaults, so it seemed to do nothing; GE timing, GE lists, Keep buffer layouts, File loading, JIT cache etc. stayed. The row shows "Done" once pressed (no "Press X"). Settings saved for a game are not changed.

## What's new in build 262 (ver 0.1)

Build 261 (the normal app, folder PPSSPP001: installs over 261, keeps your settings, configs and saves), plus:

- Edge smoothing has a Light setting: Off / Light / Full (Settings > General, and in the in-game options). Your 261 report: Full makes text quite blurry at 1x, where the picture is smoothed at the PSP's own 480x272 and then scaled up to the TV, so the softening is enlarged too. Light follows an edge for at most 2 pixels (Full: 8) and mixes only half of the smoothed colour into each pixel: jagged edges are softened less, text and small details stay sharper. Per game: fxaa=2 Light, fxaa=1 Full (as in 261). The Perf report's RSX line says which one ran.

## What's new in build 261 (ver 0.1)

Build 258 (the normal app, folder PPSSPP001: installs over 258, keeps your settings, configs and saves), plus:

- Edge smoothing (Settings > General, and in the in-game options): smooths jagged edges of the finished picture (FXAA, at the internal resolution, before it is scaled to the TV). Off by default. Costs some RSX time; text gets a little softer. RSX renderer only. Per game: fxaa=1. The Perf report's RSX line says on how many frames it ran.
- A GE dump (L2+R2) no longer freezes the PS3 (from diagnostics build 260). With the RSX renderer, recording one frame can take 5-20+ s; a frame that takes more than 8 s to record is now given up: nothing is written, its memory is freed, the game goes on ("GE dump given up (took too long)", and a line in launch.log).
- From diagnostics build 259: the RSX's part of the Perf report counts the small copies a game reads back with its CPU, and Perf report lines can be up to 2048 characters. Only a measurement.
- Not included: 260's wait for those small copies (it cost frame rate and didn't steady the flashlight glow in Silent Hill Origins).

## What's new in build 260 (ver 0.1, diagnostics build)

Build 259 (the separate app "PPSSPP PS3 diag", folder PPSSPP002), plus:

- Silent Hill Origins' flashlight glow on Harry's jacket changing brightness now and then. Your 259 log: none of the game's readback pixels were lost, but each reached PSP memory 2-4 GE syncs after it was drawn, so the game read the lamp's visibility from 1-3 frames earlier (older when the RSX fell behind). Now a GE sync with such a copy waits for it and writes it before the game goes on, so the game reads this frame's value. Only for these few pixels; the Perf report's RSX line says how many syncs waited and how long in all.
- A GE dump (L2+R2) no longer freezes the PS3 (Midnight Club, your 257 log: the game stalled 20+ s in its GE calls while memory ran out, then hung). With the RSX renderer, recording one frame copies every texture the game takes from a buffer it drew back from the RSX, one by one: ~5 s a frame in Silent Hill Origins, 15 s in Shattered Memories (then dropped for memory), 20+ s in Midnight Club. A frame that takes more than 8 s to record is now given up: nothing is written, its memory is freed, the game goes on ("GE dump given up (took too long)", and a line in launch.log).

## What's new in build 259 (ver 0.1, diagnostics build)

Build 258 as the separate app "PPSSPP PS3 diag" (folder PPSSPP002), plus a measurement; nothing else changes how games run.

- Silent Hill Origins' flashlight glow on Harry's jacket changing brightness now and then (your 257 log and dumps; since build 247 most likely). The game finds out whether the lamp is hidden by drawing one pixel (480,0 of a buffer, outside the picture) and reading it with its CPU. Since 247 that pixel is copied from the RSX without waiting and written to PSP memory at a later GE sync. The RSX's part of the Perf report now counts these copies: made, rows written to VRAM, rows kept because the CPU had changed that row since, dropped because the buffer was written back in full since, GE syncs where a newer drawing waited because a copy was still in progress. launch.log also names the first rows kept.
- Perf report lines can be up to 2048 characters (the RSX line was cut off at 1024).

## What's new in build 258 (ver 0.1)

Build 254 (the normal app, folder PPSSPP001: installs over 254, keeps your settings, configs and saves), plus what the diagnostics builds 255-257 found:

- Faster 3D draws for every game with 3D draws on the GE worker thread: their vertices are copied for the worker 16 bytes at a time with the PPU's vector unit. The PS3's memcpy copied them byte by byte (it only copies fast when both addresses are multiples of 8, and PSP vertices almost never are). Silent Hill Shattered Memories, heavy parts (your 257 log): the copy ~1.6 times faster, merging draws ~3 ms less a frame, the GE thread 5-10 ms less a frame. Other games gain less (Midnight Club, Silent Hill Origins: a few ms a frame in heavy scenes). Same pictures and memory use as before.
- Perf report: a "T&L merge attempts per frame" line (where merging draws spends its time).
- The build script can make a second app in another folder (PS3_APP_ID, PS3_EDITION), as the diagnostics builds were.

## What's new in build 257 (ver 0.1, diagnostics build)

Build 255 (separate app "PPSSPP PS3 diag", folder PPSSPP002) with a different fix; 256's merge buffers are taken out again.

- 256 didn't make merging faster (your 256 log: appending still ~10 us a merge) and its 8 buffers ran out, so a merge fell back to a big allocation that failed (the emergency reserve saved it). The real cost: the PS3's memcpy copies byte by byte unless both addresses are multiples of 8, and PSP vertices (20, 36 bytes...) almost never are - ~430 MB/s, for the 2.5-3 MB of vertices a frame that merging copies in Silent Hill Shattered Memories' slow parts, and ~1 MB for new 3D draw jobs. Vertices are now copied 16 bytes at a time with the PPU's vector unit (AltiVec), whatever their alignment. Checked: 200,000 random copies on an emulated PowerPC the same as memcpy, none reading past the source's end; on a PC the same pictures.

## What's new in build 256 (ver 0.1, diagnostics build)

Build 255 (separate app "PPSSPP PS3 diag", folder PPSSPP002), plus:

- Faster draw merging (Silent Hill Shattered Memories, your 255 log: merging took 8-14 ms of the GE's time a frame in the slow parts, 80% of it appending vertices). A merged batch of draws used to grow its buffers by doubling (a new block and a copy each time, up to 512 KB) and the GE worker freed them after drawing - every frame, big blocks asked of a heap whose largest free piece was 256 KB. Now 3 MB of buffers (8 x 256 KB for vertices, 8 x 96 KB for indices) are set aside once, at the first merged draw that needs one, and passed from batch to batch. Only when there is room: at least 16 MB free afterwards and memory not in small pieces, otherwise merging works as before. When memory runs low later they're given back for the rest of the game. launch.log says which ("GE merge buffers: ..."). Checked on a PC with 132 GE dumps: the same pictures.

## What's new in build 255 (ver 0.1, diagnostics build)

Build 254, plus a measurement; nothing else changes how games run. **It installs as a separate app**, "PPSSPP PS3 diag" (folder PPSSPP002), next to the normal one, with its own settings.ini, games/ configs, memstick (saves) and logs: copy the game's config into its games/ folder, and its save from the normal app's memstick if you want to continue there.

- The Perf report has a new line, "T&L merge attempts per frame": how often the GE tries to merge a 3D draw into the one before, and where that time goes (the kind and format checks, the index range, the state comparison, the transform parameters, appending the vertices). In Silent Hill Shattered Memories' slow parts (your 254 log) merging took ~20 ms of the GE's ~58 ms a frame, far more than it should.

## What's new in build 254 (ver 0.1, test build)

Build 253, plus:

- Killzone didn't boot (your 253 log: an RSX graphics exception at the menu, then no picture). Since build 247, a buffer drawn only in a few pixels is copied from the RSX for the game to read, without waiting; at 1x Killzone's was a single pixel, and a 1x1 copy is the likely cause of the exception. The copy on the RSX is now at least 2x2 pixels; only the drawn pixels go back to VRAM, as before. Checked on a PC with 129 GE dumps: the same pictures.

## What's new in build 253 (ver 0.1, test build)

Build 252, plus:

- Out of memory after a few minutes of play (Silent Hill Origins, your 251 log: ~7 minutes in, "OUT OF MEMORY" on the GE list thread). The PS3 still had 24 MB of main memory free, but no piece bigger than 2 MB, and one 3D draw used a vertex range of a few MB: its vertices are copied for the GE worker into one block, which couldn't be found. A draw with more than 1 MB of vertices is no longer copied: the GE worker finishes what it has, and the draw is made at once, straight from PSP memory. Checked on a PC with 129 GE dumps of different games, every 3D draw made that way: the same pictures.

## What's new in build 252 (ver 0.1, test build)

Build 251, plus:

- Debug tab > Sound debug (`snddebug`, needs Debug on): `Sound:` lines in launch.log every 2 seconds while a game makes sound, to find where distorted sound gets clipped. For each step: the final mix before the Volume setting (sound clipped there would get quieter with Volume; sound clipped earlier wouldn't), each audio channel's volume and the game's samples (how many were already at full scale when the game handed them over), SAS (all voices summed, the effect send, the game's own buffer mixed in, what was written and how much of it clipped), the loudest SAS voices' volumes and envelopes, and the ATRAC decoder's output. "Log, voices on the PPU" also mixes the SAS voices with the PPU code instead of the SPU mixer, to tell a fault of the SPU mixer from the game's own sound. Off costs nothing. Checked on a PC with the PSP sound tests: their results are the same with it on.

## What's new in build 251 (ver 0.1, test build)

- Slowdown with Keep buffer layouts On after about 40 seconds of play (Silent Hill Origins' flashlight area and mirror room, your 250 log: 4-22 fps with GE lists on Own thread, run ahead). Framebuffers unused for 1200 frames are freed, but a framebuffer's last use was only noted when it was the target at the end of a frame or on screen. A buffer layout the game switches away from each frame (the light buffers' 16-bit layouts) was freed at every frame's end and made again the next frame, loaded from VRAM - two new 2x framebuffers and uploads every frame, and 250's skip of those loads never got to work. A framebuffer's use is now noted whenever it is drawn into or looked up. Other games with framebuffers that aren't drawn last in a frame may gain too.
- Config editor: the text cursor is a plain white bar blinking every half second. It was drawn over full strength, which made the colours overflow into a rainbow-coloured line.

## What's new in build 250 (ver 0.1, test build)

- Silent Hill Origins, flashlight area with GE lists on Own thread, run ahead (your 249 log: by the truck run ahead is much faster, 22.6 against 15 fps; in the flashlight area Emulation thread was, 28.5 against 23.8). Most of the GE worker's time there went into loading the 2x light buffer again every frame: its 128x256 light-mask corner is written back for the copy the game makes, and with Keep buffer layouts On the light buffer's other layout took that for a change by the CPU. A write-back no longer makes layouts kept over the same memory load VRAM again. Checked with your GE dumps (the corner marked before the light buffer is drawn): what is left there never reaches the picture.

## What's new in build 249 (ver 0.1)

Build 248 (your tests: ~28.5 fps in Silent Hill Origins' slow area with GE lists on Emulation thread, the game's 30 fps nearly reached), plus:
- Edit game config: START asks before saving ("Save the config? The current config (file) will be saved. Do you want to proceed?" - Yes, save / No, back to editing) instead of saving at once.
- Silent Hill Origins: GE lists on Emulation thread (gelists=0, the default) and Keep buffer layouts On (keeplayouts=1) suit it now.

## What's new in build 248 (ver 0.1, test build)

Build 246 with 247's glow fix (no wait for the RSX at GE syncs): the background copy and the glow fix together. Compare with 247 (the same without the background copy).

## What's new in build 247 (ver 0.1, test build)

For comparison: build 244 plus the glow fix, without the background copy of 245/246 (copies out of VRAM are made at once, as in 244). The glow fix no longer waits for the RSX: your build 246 log had GE syncs at ~10.7 ms a frame against ~3 ms in 244, the wait for that one pixel. The pixel is copied off the RSX at a GE sync and written to VRAM at a later one, once the copy is done (at most a frame late for the game's test). launch.log: "drawn only at ...: copied for the game to read".

## What's new in build 246 (ver 0.1, test build)

Build 245 without its flashlight at the internal resolution (not needed: 244 looked right), plus:
- Fix: Silent Hill Origins' lamp glow on Harry's jacket coming and going (builds 243-245; your build 244 dumps). Each frame the game draws one pixel of its scene into a spare buffer at the lamp's position, and its code reads that pixel back to decide whether the lamp is hidden. That pixel reached PSP memory only when everything was written back at the light-mask copy; since 243 that copy writes back only what it reads, so the game mostly read an old value and left the glow out. Now, at each GE sync (where the game may read VRAM), a buffer drawn only a few pixels into since its last write-back has those pixels written back. launch.log: "drawn only at ...: written back at the GE sync for the game to read".

## What's new in build 245 (ver 0.1, test build)

- Test: Silent Hill Origins' slow area, fourth step. The copy the game makes every frame out of VRAM into main memory (its light mask) no longer stops the emulator until the RSX has drawn everything before it. It goes into the RSX's queue in order with the drawing and is made there, while the game goes on, as on a PSP. It is done before the game can see the GE finish (its syncs and GE interrupts wait for the queue), and before the emulator itself reads that memory (a palette, vertices, another copy). Applies to any game's copies from VRAM into main memory. launch.log: "block transfer to main memory, in order with the drawing" lines, and the Perf line counts them.
- The flashlight at the internal resolution: most likely the game draws its light with that copy as a texture, which in main memory has the PSP's resolution. The RSX now keeps its own pixels of the copied rectangle (copied on the RSX), and a texture read of the copy uses them while the copy's bytes are unchanged and the texture has the buffer's format. launch.log: "the RSX's pixels kept for texture reads of the copy"; the Perf line counts the reads.
- Checked with your two GE dumps with such a copy (and a draw textured with it) added: the same bytes in memory as copying at once; the texture read takes the RSX's pixels.

## What's new in build 244 (ver 0.1, test build)

- Test: Silent Hill Origins' slow area, third step. Your build 243 log names the copy: every frame the game copies a 128x256 corner of its light buffer (at 04170000) out of VRAM into main memory. 243 still wrote back three whole buffers for it: the display buffer (its 512 rows reach that address, though only 272 are drawn), a buffer beside the corner that shares its rows but not its columns, and the light buffer itself; each write-back also made buffers sharing its memory load again. Now only the rows and columns the copy reads are written back, and only from buffers the RSX drew there: the 128x256 corner of the light buffer. Checked with your two GE dumps with such a copy added: the same bytes in memory as before.
- The flashlight's lower resolution: most likely the game uses that copy in main memory as a texture, which is at the PSP's resolution. Not changed.

## What's new in build 243 (ver 0.1, test build)

- Test: Silent Hill Origins' slow area, second try. Your build 242 log: most of each frame went into one block transfer a frame that reads from or writes to a buffer the RSX draws into. Before such a transfer, every buffer the RSX had drawn into was written back to PSP memory (~6 buffers at 2x, and buffers sharing memory then loaded each other again). Now only the buffers over the transfer's own memory are written back; the others stay on the RSX. None of the 128 GE dumps here has such a transfer (your dump's frame didn't either), so they only show that nothing else changed. launch.log: "RSX renderer: block transfer over ...: ... written back" (the first 8), naming the transfer and the buffers.

## What's new in build 242 (ver 0.1, test build)

- Test: Silent Hill Origins' slow area (~5 fps, your build 241 log and dump). Its fog/light effect draws distances into a 16-bit buffer and reads them back through a palette, as a 32-bit palette texture (two pixels per texel). The RSX renderer did such palette reads on the RSX only for 32-bit buffers; this one went through PSP memory (write-backs and a CPU decode every frame). A new RSX program does it for 16-bit buffers (565, 5551, 4444) too. launch.log: "RSX renderer: palette lookup ... from 16-bit framebuffer ... done on the RSX".
- A palette read from a buffer that is kept in two layouts (Keep buffer layouts On) now uses the layout drawn last. Before, it took the 32-bit one: in Silent Hill's slow area, the old scene picture instead of the distances.
- For Silent Hill, try Keep buffer layouts On: the game switches two buffers between 32-bit and 16-bit every frame, and with it Off each switch writes the whole buffer back.

## What's new in build 241 (ver 0.1, test build)

- Test: Silent Hill Origins' menu text blinking with Frame cap in videos Off or Auto. With Frameskip on, a skipped frame left out the menu's drawing (its text) but still copied the next movie picture into the menu's screen buffer, so the next frame shown had the picture without the text. In a skipped frame, a copy from memory into VRAM that the game makes every frame is now left out too. Copies made once are always done. Other games: only while frameskip is skipping frames, such a copy shows one frame later. launch.log: "Frameskip: a copy into VRAM made every frame ... left out of a skipped frame" (the first few times).

## What's new in build 240 (ver 0.1)

- Fix: Silent Hill Origins' first loading screen (the red picture) showing only its top part. The picture lies in rows 152 and on of the buffer the game drew just before it, which holds 272 rows, so the RSX renderer's copy had only the picture's top 120 rows. A draw that reads more rows of a buffer than the RSX's copy holds now reads the picture from memory. launch.log: "RSX renderer: texture ... read from memory" (the first few times).

## What's new in build 239 (ver 0.1)

- Fix: Silent Hill Origins' main menu turned to garbage below the top 88 rows after quitting the game to the menu. Two buffers over the same memory (the menu screen and a 16-bit buffer from the game) were updated from a movie picture through one staging buffer, the second before the RSX had copied the first; it now waits in between.

## What's new in build 238 (ver 0.1)

- Triangle on a game: new "View game config" (when the game has a config): the whole file to read, line by line (Up/Down) or a page at a time (Left/Right or L2/R2). X on a line opens the editor there.
- "Edit game config" opens an existing file at the top instead of on its last line, the status line shows "line N of M", and L3/R3 move a page.

## What's new in build 237 (ver 0.1)

- Advanced tab: "Reset" is now "Reset to defaults", on its own at the bottom of the right column (heading RESET). It sets the same options back as before: GE thread, JIT floats, SPU vertices and textures, vertex data, texture check, write-back, screen copies, draw merging, JIT warm-up and RSX tiling.
- The RSX tiling help text says what Screens only is for.

## What's new in build 236 (ver 0.1.12)

Built on 230: nothing from builds 231-235 is in it.
- Advanced > RSX tiling has a third choice, "Screens only": screen buffers stay tiled, depth buffers don't. On stays the default, so other games run as in 230. For Killzone's flickering ground (a surface drawn twice where the second layer has to land at exactly the same depth): with RSX tiling Off it flickered much less. Set Screens only and use "Save for a game" for Killzone. ini: rsxtile=1 On, 2 Screens only, 0 Off.
- The right column of the Advanced tab has three headings: OTHER (JIT, denormals, sound mixing, file loading, dialog memory wipe), VIDEO PROCESSING (videos, deblocking, frame cap in videos) and BUTTONS (START+SELECT menu, L3+R3 FPS toggle).

## What's new in build 230 (ver 0.1.12)

- Fix: with Advanced > Vertex data set to RSX memory, games crashed as soon as they drew in 3D with Fast 3D path on RSX transform & lighting. The vertices of those draws are copied with a cache instruction (dcbz) that is only allowed in main memory; in RSX memory they are now copied the plain way.
- "Save for a game" is only on the Basic options tab now (it was on Advanced too). It still saves the settings of both tabs.
- Your build 229 dump of Killzone with CPU transform: the plank deck that came out white with RSX transform & lighting is drawn with its wood texture. The white textures come from the RSX transform & lighting path; still being looked into (Vertex data: RSX memory and SPU vertices: Off are the two tests).

## What's new in build 229 (ver 0.1.12)

- New on the Basic options and Advanced tabs: "Save for a game". Press X, choose the game, and the settings of those two tabs (as shown at that moment) are saved as that game's config (USRDIR/games/<game>.ini; a file it already had is replaced, after asking). That game then always starts with them; other games keep using the tabs. The Debug tab's switches aren't saved. Triangle on the game: Edit or Delete game config.
- Killzone: the washed-out band over the top half of the screen (your build 228 dump 2, with Keep buffer layouts on). Killzone grades its colours by reading its screen at 04044000 through a palette. The renderer took the picture from the first buffer it held that contains that address; with Keep buffer layouts, that was the menu's old 8888 layout of 04000000, whose rows 136 and on are 04044000's memory (in your dump the band ends at row 135). A buffer that starts exactly at the address is now used, as for other screen reads.
- Killzone's white textures in game (your dump 4: the plank deck and a sandbag drawn plain white): not found yet. The replay on the PC draws them with their textures, so the data was right; only the RSX drew them white.

## What's new in build 228 (ver 0.1.12)

- Frame cap in videos 30 (and 40, and Auto) can no longer make a video get stuck. At 30 the game has no frame to spare for a 30 fps movie: in your log of Silent Hill Origins' menu it fell to 20-27 pictures a second and the movie's sound started to run short, the start of the stuck menu video of build 222. Now, with any video cap, when the video's clock moves less than 0.93 s in a second the cap is lifted until it has caught up (the log says "cap lifted until it catches up"), then it comes back.
- Your build 227 dumps: the RSX drew the menu text again over the movie (the fix in 227 works).

## What's new in build 227 (ver 0.1.12)

- Fix: Silent Hill Origins' menu text was missing with Frameskip Auto (since build 224). In a frame auto frameskip doesn't draw, the menu's text is skipped, but the copy of the movie picture still happens, and it counted as a picture to show; since 224 that copy reaches the RSX at once, so those frames showed the movie without the text. A copy in a skipped frame now counts as skipped drawing too.
- Frame cap in videos: Off (default), 40 fps or 30 fps for the whole video, or Auto (as in builds 215-223: the Frame cap comes back once a video runs slow; it now also goes again for that video if the game falls behind the video's pictures). ini videocap=60/40/30/-1; videocap=1 from 222-223 is Auto.

## What's new in build 226 (ver 0.1.12)

- Triangle on a game: "Edit game config" opens the game's settings file (USRDIR/games/<game>.ini) as text with an on-screen keyboard, or starts a new one. D-pad and X type a key (Shift for capitals), Square deletes, Triangle adds a space, L1/R1 move the cursor, L2/R2 go up and down a line, START saves, Circle closes (asking about unsaved changes). "Delete game config" (when there is a file) removes it after asking. They replace "Open game settings", which only showed the file.

## What's new in build 225 (ver 0.1.12)

- Options changed in a game (START+SELECT menu) are saved to the main settings again, for all games, as before build 221; they show on the options tabs too. A game's own file in USRDIR/games/ still wins for that game. The L3+R3 FPS toggle is kept too.

## What's new in build 224 (ver 0.1.12)

- Silent Hill Origins' menu is much cheaper to run. Each frame it copies the movie picture over its screen; before that copy the emulator wrote every buffer the RSX had drawn back to PSP memory, ~14 ms a frame at 2x resolution (half the frame). Now a copy from memory into a buffer the RSX draws into goes straight to the RSX, in order with the drawing queued before it, with no write-back. Checked against your three menu dumps and 33 others: the same pictures as before.
- Frame cap: 60 (no cap), 30 or 20 frames a second (ini fpscap=60/30/20). All three are evenly spaced on a 60 Hz TV.
- Frame cap in videos: no cap (default), 40 or 30 (ini videocap=60/40/30). With 40 or 30 the cap comes in when a video runs below full speed, and goes again for that video as soon as the game makes fewer than 1.2 frames per video picture (it would fall behind the movie: the stuck Silent Hill menu video of build 222). A game file with videocap=1 (Auto in 222-223) means 40.
- Options changed in a game (START+SELECT menu, L3+R3) are no longer saved anywhere: they last until you quit the game.

## What's new in build 223 (ver 0.1.12)

- Fix: Silent Hill Origins' menu movie got stuck on one picture, sometimes for a long time, while the menu itself still worked. With the frame cap back on during the movie (build 215's "Auto"), the menu ran at 30 frames a second at most and sometimes fewer, so the game decoded only 20-26 of the movie's 30 pictures a second. The movie's buffer filled up with video, its sound found no more data and stopped, and the game holds the picture until the sound catches up. "Frame cap in videos" is now Off by default, as before build 215. Auto is still there (per game: videocap=1). A game file that already has videocap=1 (set from the START+SELECT menu) keeps it.

## What's new in build 222 (ver 0.1.12)

- New setting Advanced > Frame cap in videos (also in the START+SELECT menu; per game: videocap=0). Auto is how build 215 made it: with Frame cap 30, the cap is lifted while a video plays and comes back when the video can't keep up. Silent Hill Origins' menu (drawn over a movie) then runs at a steady 30 fps with clean sound, but its animation looks less smooth. Off never brings the cap back during videos: the menu runs at ~40-55 fps as before build 215, smoother, but at ~88% speed, so the music may crackle.

## What's new in build 221 (ver 0.1.12)

- Options changed in a game (START+SELECT menu) are saved for that game only, in its file in USRDIR/games/ (created if it doesn't exist yet; Triangle > Open game settings shows it). Before, they went into settings.ini and changed every game. The FPS overlay is the exception: like L3+R3, it is for all games.
- The FPS number counts new pictures only. Midnight Club L.A. keeps one display buffer and copies each finished frame into it; with Frameskip off the same picture was shown again at the vblanks in between, and each of those counted as a frame. So its "22 fps" with Frameskip off likely included repeats (build 220's log points to about half). launch.log's Perf report has "new pictures shown" next to the old counts.

## What's new in build 220 (ver 0.1.12)

- "Quit to game list" shows only "Back to the game list..." on the menu background until the game list is back, both while the game closes and while the emulator starts again (it showed the old plain text screen, then the startup message).

## What's new in build 219 (ver 0.1.12)

- Fix: "Quit to game list" (build 218) went back to the XMB. The PS3 call that starts a program in place of the running one can't start the NPDRM-signed EBOOT.BIN, only a plain fake-signed SELF. The two differ only in their first ~2.5 KB, so the package now has that plain header (RELOAD.HDR, 2.4 KB), and the first "Quit to game list" with a new build writes USRDIR/RELOAD.SELF from it plus EBOOT.BIN (a second or two, then it is kept), and restarts through that. (IRISMAN and others restart themselves the same way.)

## What's new in build 218 (ver 0.1.12)

- The in-game options (START+SELECT) have a last row, "Quit to game list". It asks "Are you sure you want to quit the game?" first (No is selected to start with); Yes ends the game and the emulator starts again straight into the game list, as if launched from the XMB (so nothing from the game is left in memory). The PS button menu still quits to the XMB.
- The "Game stopped" screen (after a game ends by itself) now offers X: back to the game list, Circle: exit to the XMB.

## What's new in build 217 (ver 0.1.12)

- Fix: the FPS overlay showed nothing with Debug off, whether turned on with R3, in the in-game options or in Basic options (its numbers were only worked out together with the Perf log lines).
- The FPS overlay is now toggled with L3+R3 (both sticks pressed in) instead of R3 alone, which games can use through the right stick setting. Advanced > L3+R3 FPS toggle turns it off (ini key still r3fps).

## What's new in build 216 (ver 0.1.12)

- Midnight Club L.A. filled the JIT cache (65536 compiled blocks) every few minutes of driving. Each time everything was thrown away and compiled again, at 10-15 fps for several seconds. Now when the cache fills and there is room in memory, it grows to the "JIT cache: Large" size (98304 blocks) for the rest of the game, so it fills far less often. The log says "JIT cache full at ... blocks: room for 98304 from now on". Turning on Advanced > JIT cache: Large still does this from the start.

## What's new in build 215 (ver 0.1.12)

- Silent Hill Origins' menu music still crackled in 214: the menu ran at ~85-93% speed even with deblocking off. The reason: the frame cap (30 fps) is lifted while a video plays (since build 189, so Call of Duty's videos don't lose pictures), and this menu draws at 60 frames a second over its 30 fps movie: twice the work of the game's 30 fps gameplay. Now the cap comes back during a video when that video runs below 95% of full speed and the game makes more than 1.4 frames per picture. Videos that keep up (Call of Duty, Midnight Club) keep the exemption. The log says "frame cap back on for this video" when it happens.

## What's new in build 214 (ver 0.1.12)

- Video deblocking has a new default, Auto: on (smoother) while a movie keeps up, and switched off for the rest of that movie as soon as it runs below 95% of full speed. On the PS3 the filter costs far more than first thought: Silent Hill Origins' menu movie took about 14 ms a picture with it, against 5-8 ms without, and the menu ran at ~83% speed, so its music crackled (thousands of sound underruns in the build 211 log). On and Off are still there (per game: videodeblock=-1 auto, 1 on, 0 off; the old deblock=0 still works).

## What's new in build 213 (ver 0.1.12)

- Basic options: the Sound heading is now called Audio (the first option under it is Sound, on/off).
- The Triangle menu on a game no longer has the list order: it's already the Sort button above the games (and Square).

## What's new in build 212 (ver 0.1.12)

- The in-game START+SELECT menu and the R3 FPS toggle no longer need Debug on. Each has its own switch in Advanced: "START+SELECT menu" and "R3 FPS toggle", both on by default (per game: ingamemenu=0, r3fps=0). With the START+SELECT menu off, START and SELECT go straight to the game with no delay. Debug now only adds the logs and the L3 (JIT profile) and L2+R2 (GE dump) buttons.

## What's new in build 211 (ver 0.1.12)

- Fix: build 210 made Silent Hill Origins pause more often in game. To make room for movies it cleared the JIT cache whenever no free piece of 1 MB was left, which in that game was every 30 seconds (and on every picture of a movie). Each clear means a pause while the game's code compiles again, and it didn't even help: the biggest free piece stayed 256 KB. That's gone; the JIT cache is cleared only when memory really runs out, as before.
- Instead, the movie picture buffer (510 KB in one piece) is now kept from the first movie (when there's room) for all later ones, so a movie late in a game doesn't need a new piece that big. Build 210's log showed the later movie's pictures being skipped for lack of one.

## What's new in build 210 (ver 0.1.12)

- Videos have the deblocking filter again (smoother, less blocky), as before build 189. Advanced > Video deblocking can turn it off for 15-25% less decoding time if a video can't keep up (per game: deblock=0). Midnight Club L.A.'s slow intro, the reason it was switched off, turned out to be the GE timing (fixed in build 208).
- Silent Hill Origins froze when quitting to its title from the game's own menu: the game starts its movie again, and the emulator was stuck inside the video decoder with almost no memory left in one piece (10 MB free, but nothing bigger than 256 KB). Now, before a movie opens, if there isn't room for it in one piece, the GE job buffers are released and the JIT cache is cleared. The JIT cache is also cleared when no piece of 1 MB is left, not only when the total runs low. If the picture buffer still can't be had, that picture is skipped instead of the colour conversion writing through a null pointer. If it freezes in a movie again, the STALL lines now say which step of the decoder it is in.

## What's new in build 209 (ver 0.1.12)

- Fix: with Videos set to Off, a game could wait in its movie forever (Silent Hill Origins' intro). Off is meant to work like a build without a video decoder, where the movie's clock still moves on a frame at a time with no picture, so the game gets past it. Here no decoder was opened, the clock stayed at 0, and the game kept waiting for it.

## What's new in build 208 (ver 0.1.12)

- Fix: Midnight Club L.A.'s intro (and other videos drawn the same way) played at a few frames a second. The GE timing estimate for drawing a video picture read the screen positions of its strips from PSP memory without swapping the bytes. PSP memory is little-endian and the PS3's CPU is big-endian, so a strip at x=32 read as x=8192. Each of the 15 narrow strips then counted as a large piece of the screen at the slow wide-texture rate: 206 ms of emulated GE time per video frame instead of about 7-10 ms. The game waits for the GE before showing the next picture, so the video showed about 5 pictures a second though 30 were decoded. Found with build 206's "GE sync far ahead" lines. Only the time estimate was wrong: the picture itself was always drawn right.

## What's new in build 207 (ver 0.1.12)

- Advanced: the right column has one heading again, CPU, sound & other.
- Debug tab: two new switches for GE dumps, "Dump: PSP RAM" (the .ram file) and "Dump: RSX picture" (the .vram file). Both are on by default, as before; turn one off to save time and space on the memory stick (ini: dumpram=0, dumpvram=0).

## What's new in build 206 (ver 0.1.12)

- Basic options: no Speed heading any more; the left column is all under Graphics, and PSP memory and PSP fonts moved there too. The right column is Sound, Controls and PSP system (language, date and time format, nickname).

## What's new in build 205 (ver 0.1.12)

- L1/R1 switch between the top tabs from anywhere (Games, Basic options, Advanced, Debug). L2/R2 now do what L1/R1 did before: the other column on an options page, a page of games in the grid.
- The options pages have section headings inside each column. Basic options: Graphics | Sound, Controls, PSP system. Advanced: Graphics | CPU & JIT, Sound & files, Other.
- Texts that didn't fit their line were shortened: the button line while a game loads (it was cut off with Debug on), the Games page's hints, one value and one label in the options.
- Video diagnostics, part 5: build 204's log of Midnight Club L.A.'s intro showed the GE's own work is almost nothing, yet every other GE sync is scheduled exactly 206 ms ahead. A "GE sync far ahead" line now says where each one over 50 ms comes from (which list, how much GE time it was charged and for what). Lists that end with a finish interrupt are counted in "Video GE lists" too (they weren't).

## What's new in build 204 (ver 0.1.12)

- No Exit tab any more: the PS button menu quits the app, as before. Circle in the game grid or on an options page goes up to the tabs, and Circle on the tabs goes back down into the page.

## What's new in build 203 (ver 0.1.12)

- The Advanced options tab is now just called Advanced.

## What's new in build 202 (ver 0.1.12)

- Exit sits apart in the top right corner now (with a red tint), and asks "Quit PPSSPP?" first. No is selected to start with, so pressing X by mistake does nothing.
- Switching Debug or Core logging off now stops stdout.log and core.log at once. Before, they kept being written until the app was restarted.
- The tabs are now called Basic options and Advanced options (shorter than "settings").
- Core logging is still a switch of its own and stays off unless you set it. Turning Debug on doesn't turn it on; it only works while Debug is on, as before. The help text and DEBUGGING.txt now say so.

## What's new in build 201 (ver 0.1.12)

- Refresh and the list order moved from the top bar into the Games page: two buttons above the game icons (press Up from the top row of games to reach them). The top bar is now Games, Settings, Advanced settings, Debug, Exit.
- New Debug tab: everything that decides what goes into the logs, in one place. Left column: Debug (the master switch, moved here), Core logging, the Perf report (off, every 10 s or every 2 s), CPU/GE timing, and on/off switches for the stall reports, video statistics, save dialog steps, GE list watch, RSX renderer events and JIT events. Right column, for finding a bug: what L2+R2 does in a game (GE dump, or a text log of the next 3000 draws), JIT profile, NaN watch, store watch and the JIT's interpret groups (until now these three were only in ini files), and the jitops list when one is set. Each one also has an ini key, for one game's file.
- New guide, DEBUGGING.txt (next to this file): where the logs are, what each Debug switch adds to them, and which switches to turn on for which kind of bug (freezes, slow games, jerky videos, saves, graphics, JIT-only bugs, NaNs).
- Fixes: CPU/GE timing (geprofile) now shows in the Perf report (it was measured but never written). The JIT profile's counters no longer run with Debug off. With Debug off, the stall watch and the video statistics no longer build reports nobody sees.

## What's new in build 200 (ver 0.1.12)

- New main screen. Games show as a grid of their icons (ICON0.PNG and the title from the ISO, CSO or PBP, read a few at a time as you scroll). A bar along the top has Games, Refresh, Settings, Advanced settings and Exit: press Up from the top row (or Circle) to reach it, Left/Right to move along it, X or Down to open a tab. Settings and Advanced settings are separate tabs now, each in two columns (L1/R1 switches column). Exit asks for one more X, so Circle no longer quits the app straight away.
- Triangle on a game opens its options: Start game, Open game settings (only when USRDIR/games/<game>.ini exists; shows the file's text), Delete game (asks first; deletes only the ISO/CSO file, not its saves or settings), and the list order.
- The list can be sorted by name A-Z, name Z-A, newest first or largest first (Square cycles it, or from the Triangle menu). The choice is kept in settings.ini (gamesort=).

## What's new in build 199 (ver 0.1.12)

- Video diagnostics, part 4: a "Video GE lists" line once a second during a video. It shows the GE time of each display list run, how much of that was waiting for earlier GE work, how much was drawing video textures and block copies, and the emulated CPU clock. Build 198's log showed Midnight Club: L.A. Remix's "GE done" scheduled 206 ms after each video frame was sent, though a GE dump of that frame costs only 11.8 ms here.

## What's new in build 198 (ver 0.1.12)

- Video diagnostics, part 3: a "Video GE" line once a second during a video. It shows how many GE syncs were scheduled and how far ahead in emulated time, plus how often threads waited for the GE and how long each wait lasted. Build 197's log showed Midnight Club: L.A. Remix's display thread waiting for the GE at about 25 of every 60 vblanks, though drawing its frame should take the PSP's GE about 10 ms.

## What's new in build 197 (ver 0.1.12)

- The PS button menu (XMB) is no longer slow over the app's own screens (game list, settings). Those screens are drawn on the PPU, and the whole 1280x720 picture was redrawn and uploaded every frame even while the XMB was open over them, slowing the XMB, which runs on the same PPU. While the XMB is up, the last picture now just stays on screen.

## What's new in build 196 (ver 0.1.12)

- The PS button menu (the XMB over a game) is smoother. While it's open the game is paused and the last frame is shown again; that happened 30 times a second, and the XMB, which draws over the app's picture, animated at 30 fps too. Now 60 times a second.
- Video diagnostics, part 2: the once-a-second "Video threads" line says what each PSP thread was doing at every vblank (running, ready, or waiting for a vblank, a semaphore, a delay, an event flag, the GE or audio). Build 195's log showed Midnight Club: L.A. Remix's player decoding all 30 pictures a second on time and never short of data, yet drawing only ~4.5 of them; this shows what its display thread waits on.

## What's new in build 195 (ver 0.1.12)

- Video diagnostics: while a video plays, launch.log gets a "Video:" line once a second. It shows how often the game asked for video and audio data and how often none was there yet, how many pictures were decoded and how long that took, how much data the game's disc reads put into the video buffer, and how far the video and audio timestamps moved in that second. For Midnight Club: L.A. Remix's intro, which still shows only ~4.5 frames a second while the emulator idles about half of each frame. Build 193 fixed the stale pictures, but not this.

## What's new in build 194 (ver 0.1.12)

- Same as build 193, but "CPU writes to drawn buffers" is out of the Settings menu again. Build 193 put it back thinking it had been left out by mistake, but it was taken out on purpose in build 159 (turning it on crashed Silent Hill Origins at boot). The per-game key cpurows=1 still turns it on for testing.

## What's new in build 193 (ver 0.1.12)

- Fixed glitchy, jerky video in Midnight Club: L.A. Remix's intro (and in any game that decodes its video into main memory and draws it as a texture). The RSX renderer's texture cache only rehashes memory it was told has changed, and the video decoder's writes never told it. So it kept drawing an old picture and refreshed only when something else happened to invalidate it. The decoder's writes now count as changed memory.

## What's new in build 192 (ver 0.1.12)

- Faster video decoding, for videos that were too slow to keep up (Midnight Club: L.A. Remix's intro):
  - Each video picture written into video memory first had the RSX write all its framebuffers back to PSP memory. That cost up to tens of milliseconds a picture, counted in sceMpegAvcDecode's time. The write-back now only happens when the RSX actually drew into that memory since the last one; Call of Duty's intro, which needs it, still gets it.
  - The PS3's FFmpeg decodes on a single PPU thread. It now skips H.264's deblocking filter and uses FFmpeg's faster, not bit-exact paths, for roughly 15-25% less decoding time. The picture is a little blockier in places at 480x272.

## What's new in build 191 (ver 0.1.12)

- Videos at their full frame rate with the 30 fps frame cap on. The cap holds the game's thread to every other vblank after each new frame. Video players step once per vblank and show whichever picture is due, so under the cap they dropped any picture that was the least bit late. Call of Duty: Roads to Victory's intro, a 29.97 fps video, showed about 15 pictures a second. The cap now switches off while a video is playing (sceMpeg or scePsmfPlayer pictures being written) and comes back half a second after the last one.

## What's new in build 190 (ver 0.1.12)

- Fixed Call of Duty: Roads to Victory saying the memory stick is full on its second boot, then freezing to a black screen after a save is deleted. When a game asks how much space is free, the emulator added up the game's save folders on the HDD. On the PS3 that scan held up the save thread for tens of seconds or longer. The game's answer came back late or wrong ("not enough space"), and after the delete it never came back at all, hence the black screen. The PS3 now skips the scan and reports the simulated 1 GB card as free, which is plenty for any PSP game. A related bug is fixed too: a save folder that couldn't be read counted as -1 bytes and turned the total into an enormous number. If save work still stalls, launch.log now says which step it's in.

## What's new in build 189 (ver 0.1.12)

- Smooth videos. Many games show a video by having the decoder write each picture straight into the screen buffer, instead of drawing it with the GE. With frameskip on (the default), a frame was only shown when the GE had drawn into the screen buffer, plus a forced refresh every 10th vblank. These videos therefore ran at 6 frames a second while the sound and the game ran at full speed. Examples: Call of Duty: Roads to Victory's intro, and Silent Hill Origins' menu background, where turning frameskip off was the workaround. A video picture written into the screen buffer now counts as a new frame. This covers both the game's own video player (sceMpeg) and the system movie player (scePsmfPlayer).

## What's new in build 188 (ver 0.1.12)

- Speed back for games that rewrite their own code. Build 187's fix made the JIT compile such code again after every rewrite. Call of Duty: Roads to Victory rewrites 500-800 blocks per frame, so the PSP CPU time went from ~18 ms to 28-47 ms per frame. The JIT now keeps the compiled versions of rewritten code (up to 16 per address) and reuses one when the identical code comes back, instead of compiling it again. The perf line in launch.log shows how many blocks were "brought back unchanged".
- Save data diagnostics in launch.log: each save dialog request (mode, names, sizes), the free space reported to the game, how long each save/load/delete took, and, if the game stalls, what the save thread is doing. This is for Call of Duty's "not enough space" message and its freeze when deleting a save.

## What's new in build 187 (ver 0.1.12)

- Fixed the JIT running old code after a game rewrites it. Some games build small functions at run time and rewrite them as they go. Call of Duty: Roads to Victory does this to convert vertex data. When the game said the code had changed, the JIT forgot the old block but left its compiled code reachable through direct block links and return caches, so the old function kept running. That is what gave Call of Duty its rainbow wall: wrong vertex colours, while the IR interpreter drew it correctly. The compiled block is now patched out when the game rewrites it, so the new code runs.

## What's new in build 186 (ver 0.1.12)

- GE dumps stay in `memstick/PSP/SYSTEM/DUMP/`, with their `.ram` and `.vram` files next to them;
  nothing is moved or copied into the log folder any more (launch.log names the dump folder).

## What's new in build 185 (ver 0.1.11)

- GE dumps go straight into the game's log folder (moved there the moment they're written,
  numbered per run), with their `.ram` and `.vram` files: no separate dump folder to collect.
- Fix: `jitops` (build 183) never took: the IR op names table is only filled when the JIT starts,
  after the per-game settings are read, so every name came out unknown. launch.log's JIT perf line
  now says "(jitops in use)" when the list took, and names the ops running in the interpreter.

## What's new in build 184 (ver 0.1.10)

- Silent Hill Origins: the colour blotches (red, green, blue, purple) mixed with the fog in the dark.
  Found with the `.vram` saved with a GE dump: the PS3's glow buffer had coloured specks where a
  replay has black. The glow's bright-pass blends with two fixed colours and a reverse subtract
  (0x20 x what is there - 0x10 x the doubled source): results of 0-4, which the RSX blender rounded
  differently in each colour channel, and the next passes double and add them into blotches. Such
  a blend (both factors fixed, subtracting, into the buffer the draw samples) is now computed in
  the fragment program exactly as the GE does, blending off, the destination read from the copy
  the draw already samples. New fragment programs fixsub_* (make-shaders.py).

## What's new in build 183 (ver 0.1.9)

- GE dumps: the `.ram` and `.vram` files go straight into the game's log folder. Copied there at
  exit (64 MB) they were cut off when the game was quit from the XMB (which allows 5 seconds).
- Diagnostics: per-game key `jitops=<names>` (IR op names, comma separated): only those
  instructions are left to the interpreter, a finer search than jitgeneric (whose integer groups
  made Call of Duty run at 0.1 fps). launch.log lists the names it took and any it didn't know.
  Call of Duty's rainbow wall, from the RAM saved with a GE dump: its vertex colours are real 32-bit
  colours with wrong values (light blue, orange, green with varying alpha) where the other wall
  pieces have greys, so per-vertex lighting is computed wrongly, with integer code.

## What's new in build 182 (ver 0.1.8)

- GE dumps also save PSP RAM at the end of the recorded frame as `<dump>.ram` (32 MB, with the
  game's code and the frame's data): the code that built a frame can then be run on a PC, in the
  JIT and in the interpreter, to find where they differ (Call of Duty's rainbow wall: its vertices
  get 16-bit colours in 32-bit colour slots with the JIT only).

## What's new in build 181 (ver 0.1.7)

- Settings: Frame cap moved to the General column, right after Speed limit, and 30 fps is now the
  default (a settings.ini that says fpscap=60 keeps 60).
- GE dumps with the RSX renderer: next to each dump, `<dump>.vram` holds what the PS3 drew in that
  frame (all 2 MB of PSP video memory, every buffer the RSX drew written back). A dump replays the
  commands; this is their result on the PS3, to compare with a replay (Silent Hill Origins' colour
  tint in the dark doesn't show in replays).

## What's new in build 180 (ver 0.1.6)

- Diagnostics only. Call of Duty's rainbow wall: the GE dumps show the wall's 8888 vertex colours
  holding a 16-bit colour in their low half (the high half is old memory), so the game's code took
  its 16-bit path for an 8888 buffer. With the IR interpreter (cpujit=0) it doesn't happen: the JIT
  gets something wrong there. New per-game key `jitgeneric=<groups>`: the JIT leaves those groups
  of instructions to the interpreter, one at a time, and compiles the rest. Add the numbers:
  1 integer ALU, 2 integer loads/stores, 4 float/vector loads/stores, 8 FPU arithmetic,
  16 GPR<->FPR moves, 32 vector ops, 64 no fused patterns or shortcuts. 127 = all of them.
  launch.log says what it took ("JIT: left to the interpreter ..."). Off (0), it costs nothing.

## What's new in build 179 (ver 0.1.5)

- Silent Hill Origins: squares in the film grain over dark areas. The grain is a 128x128 CLUT4
  texture with a buffer width of 32, swizzled: real noise all the way across, laid out for the
  GE's addressing past the buffer width. The RSX renderer hashed and decoded only 32 texels a
  row; the SPU decode left black blocks, and the column repeat added since made the grain repeat
  every 32 pixels. Such textures are now decoded in full on the PPU, as the GE reads them.
- GE dumps record all of such a texture (the recorder stopped at one row past the buffer width,
  so replays showed black squares in the grain).

## What's new in build 178 (ver 0.1.4)

- Silent Hill Origins: the huge cyan, blue or grey triangle with fog on it that covered half the
  screen for a frame. 2D draws (rectangles: the fog overlay, full-screen clears, menu text) could
  read vertices the RSX had cached from the ring memory before the PPU wrote this draw's there,
  so one triangle of a quad got another draw's corners and colour. The vertex cache is now
  flushed before them too, as it already was for the 3D draws (Frontend 115, build 161). The same
  can leave part of a full-screen clear undone, which may be what showed the white blob's scratch
  square (both confirmed fixed). The menu text still blinks a little.

## What's new in build 177 (ver 0.1.3)

- Silent Hill Origins: the black square with a white blob flashing in the top-left corner for a
  frame. Each frame the game copies its finished scene to the screen, then at once uses the scene
  buffer's top-left 128x128 as scratch space (a light mask). The RSX could still be reading the
  scene for the copy when the mask was drawn, so the copy sometimes picked it up. A buffer read in
  place as a texture is now finished with before anything draws into it (a wait on the RSX, not
  the CPU; 1-3 a frame here, rare elsewhere). The perf line counts them as "read waits".

## What's new in build 176 (ver 0.1.2)

- Diagnostics only. Call of Duty's rainbow walls: the GE dumps show the wall's 8888 vertex colours
  with only their low 16 bits written (the high half is whatever the buffer held before), so the
  game's code stored 16-bit colours into 8888 slots, in a buffer it rebuilds while you walk and
  turn. New per-game key `storewatch=1` (with `cpujit=0`, Debug on): the IR interpreter keeps a log
  of the last 32768 16-bit and unaligned stores with the block that made each, and when a 3D draw
  has many such half-written colours, launch.log names the blocks that wrote them, with their
  code ("Store watch #n: ..."). Off, it costs nothing.

## What's new in build 175 (ver 0.1.1)

- GE dumps (L2+R2) work in games that leave little memory in big pieces (Silent Hill Origins:
  "GE dump skipped (memory)"). The recorder kept the whole frame in one buffer that doubled as it
  grew and compressed it in one go, so it needed blocks of several MB; it now keeps the data in
  64 KB pieces and writes it as one compressed piece after another (players read the files as
  before; all 52 GPU test dumps replay the same as with the old recorder). A dump may use the
  memory free at the time less 6 MB; a frame that needs more is dropped with a notice instead of
  running the emulator out of memory.

## What's new in build 174 (ver 0.1, release)

- Silent Hill Origins closing when saving: the save dialog starts a thread for the file work,
  and with the game using nearly all of the PS3's memory there was no free piece big enough for
  its 1 MB stack (launch.log ended with "pthread_create returned: 11"); the failed thread start
  closed the emulator. Threads now retry with a 256 KB, then 128 and 64 KB stack, and if no
  thread can be started at all the save dialog does its file work on the emulation thread.

## What's new in build 173 (ver 0.1.7)

- Dead to Rights falling through the world at a disarm (holding R1): build 172's NaN watch found
  it. The game checks a vector's squared length (a VFPU dot product) before normalizing it; the
  vector was tiny, so the squared length was a denormal. A PSP's VFPU flushes that to 0, the
  check sees 0 and skips the vector. The IR interpreter kept the denormal, so the game normalized
  it: square root 0, 1/0 = infinity, and the next frame 0 * infinity = NaN went into the player's
  position and the camera. Results the IR interpreter writes to VFPU registers are now flushed
  to zero like the PSP does (the JIT's AltiVec mode already did; but with JIT warm-up new code
  runs in the IR interpreter first, and a disarm is rare code). Two VFPU tests (prefix_sat,
  specials) now match a real PSP more closely. Denormals Kept (ftz=0) leaves them as before.

## What's new in build 172 (ver 0.1.6)

- NaN watch, Dead to Rights: build 171's log shows the first NaN of the disarm in block 08986090
  (frame 1149): the game scales a vector it keeps at +0x4C..0x54 of an object by 0, and the vector
  is already infinite there (0 * inf = NaN); it was written by code the watch didn't cover. New
  `nanwatch` values: 1 FPU NaNs, +2 the VFPU registers too (with a quick pass now), +4 new
  infinities too - `nanwatch=7` for all, which should show where the infinity comes from.

## What's new in build 171 (ver 0.1.5)

- NaN watch: 7f800001 (the PSP's NaN, which the float registers hold from boot and games save
  and restore constantly) no longer counts - build 170's Dead to Rights log spent its 80 detailed
  reports on those. Each detailed report now also lists the 32 float registers before the block.
  The first real NaN in that log: block 08986090 at the disarm (frame 1017, the camera went NaN
  in the same frame).

## What's new in build 170 (ver 0.1.4)

- NaN watch made much cheaper (build 169's ran Dead to Rights at 1 fps): `nanwatch=1` now checks
  only the 32 FPU registers after each block, with a quick pass that finds nothing in the usual
  case; `nanwatch=2` also checks the 128 VFPU registers (the old, slow way).

## What's new in build 169 (ver 0.1.3)

- A game's launch.log (in its logs folder) now starts with the app name and build number.
- Diagnostics only, nothing else changed. Build 168's VFPU NaN lines never came in Dead to Rights'
  log, so its NaN doesn't start in a VFPU function. New per-game key `nanwatch=1` (put it in the
  game's file in games/, with `cpujit=0`; Debug on): the IR interpreter checks the float and VFPU
  registers after every block of game code, and launch.log names each block that turned a number
  into NaN ("NaN watch #n: block ... made f12 7fc00000 (was ...)", once per block, the first 80
  with their code). Only with the IR interpreter; off, it costs one branch per block.

## What's new in build 168 (ver 0.1.2)

- Diagnostics only, nothing else changed. Dead to Rights falls through the world when a disarm
  (Circle) goes wrong: launch.log said "NaN camera" with the camera position NaN, and the PSP
  float registers held 7f800001, the NaN the PSP's vector unit (VFPU) makes - so a VFPU sine,
  square root, arcsine, reciprocal or log probably got a number it can't take. With Debug on,
  launch.log now has a "VFPU NaN: vasin(3f800001) (frame ..., PSP pc ... ra ...)" line for the
  first 24 such NaNs of a run: which function, the number it got, and where the game's code was.

## What's new in build 167 (ver 0.1.1)

- New setting PSP fonts (`discfonts`, General column, default Emulated = as before). Silent Hill
  Origins' text had letters at odd distances ("PA USED", "f rom the ot her side"); its GE dumps
  replayed on a PC show the same, and the IR interpreter did too, so it isn't the renderer or the
  JIT. The game draws its text with the PSP's font library. Desktop PPSSPP unpacks the PSP's real
  fonts from the firmware updater on the game's disc and runs the game's own font library with
  them; on the PS3 that unpacking always failed ("Not a PSAR archive" in core.log): the archive
  reader read its little-endian fields in the PS3's byte order. Fixed, and with PSP fonts = Game's
  own the fonts are unpacked (once, into memstick/PSP/NAND) and the game's own font library is used.
  With Emulated nothing is unpacked any more (before, every Silent Hill boot read the whole
  updater into memory just to fail).

## What's new in build 166 (ver 0.1, release)

- The release: the same emulator code as build 163. Builds 164 and 165 changed how block
  transfers into hardware-drawn framebuffers are written back (for Silent Hill Origins' half
  first loading screen); 164 lost Silent Hill's menu text and neither fixed the loading screen,
  so both changes are gone. The half first loading screen stays a known bug.
- Denormals help text no longer says it fixed Dead to Rights' camera (not confirmed).

## What's new in build 163 (ver 0.1.7)

- Silent Hill Origins' half loading screen, second try. Your test: whole with the software
  renderer, half with the RSX one, so the RSX renderer's copy of the picture misses rows. Build 162
  covered the CPU's copies into VRAM; the other way a picture gets there without the GE is a file
  read straight into VRAM (sceIoRead), which the game most likely uses for that picture (its boot
  logs had VRAM rows changing behind the renderer in 64-row pieces). A read into VRAM now gets the
  same treatment when it finishes. Build 161's pause screen fix: confirmed by you.

## What's new in build 162 (ver 0.1.6)

- Silent Hill Origins' burning house (your GE dump): 84 of its ~21,700 3D vertices have NaN
  normals in the game's own data. The CPU transform lights them anyway; the RSX transform &
  lighting program turned them into NaN colors (checked with PS3/rsx/check-tl.py, which runs the
  RSX program on the dump's vertices: all 84 differences were NaN colors, everything else matched).
  Such normals are now set to zero when the vertices are prepared (PPU and SPU), so those vertices
  get the ambient light only instead of garbage. The CPU transform lights them slightly brighter.
- Silent Hill Origins' half loading screen (your GE dump of it): the picture is drawn the normal
  way on the PC, so the RSX renderer's copy of the game's work buffer was missing what the game's
  CPU had put there. A copy or fill into VRAM by the CPU (sceDmacMemcpy, the game's memcpy/memset)
  over a framebuffer the RSX drew into since its last write-back now writes it back first, so the
  CPU's rows are seen and sent to the RSX again. Most likely the cause; not confirmed on a PS3.

## What's new in build 161 (ver 0.1.5)

- Silent Hill Origins' pause screen: the big triangle showing part of the scene in a different
  shape every frame (your video). In your dump the pause screen darkens the picture with one
  full-screen quad, two triangles; the broken one is the second, with one corner in the wrong
  place. That draw goes through the RSX's indexed path, whose index numbers are the same ring
  slots the 3D (RSX transform & lighting) draws use. The RSX's vertex cache is now cleared before
  each such draw, as it already was before every 3D draw since Frontend 115 (Killzone's menu lost
  triangles the same way). Most likely the cause; your video will tell.

## What's new in build 160 (ver 0.1.4)

- Silent Hill Origins running out of memory after a while (your two build 158 logs): both times
  the 3D draw merging asked for one ~1 MB block to grow a merged draw, while 12-17 MB were free
  but only in pieces of 128 KB-1 MB. Once memory runs low, merged draws now stay under 64 KB, and a
  merged draw's buffer never grows past the merge limit (before: up to twice its size). Both logs
  also had "GE lists: Own thread" on; leave it on Emulation thread for Silent Hill.

## What's new in build 159 (ver 0.1.3)

- Silent Hill Origins froze at boot in build 158: right after the "CPU writes to drawn buffers"
  change took effect, the RSX stopped with a graphics exception. That setting is gone from the
  menu and off for every game (the build 156 Auto turned it on for Silent Hill), and it didn't fix
  the half loading screen either. The loading screen is still a known bug.

## What's new in build 158 (ver 0.1.2)

- Anisotropic filtering: Off (default), 2x, 4x, 8x or 16x, in the general column under Filtering
  and in the in-game options. Sharper textures on roads and floors seen at an angle, for textures
  the game filters smoothly; costs some RSX time. Per game: anisotropy=.

## What's new in build 157 (ver 0.1.1)

- New settings at the end of the general column, the PSP system settings games read: Confirm
  button, Language, Date format, Time format and Nickname. Auto (the default) takes each from your
  PS3's own system settings, so a European PS3 gets Circle or X as set there. They take effect
  when a game starts. launch.log lists what the game got ("PSP system settings: ...").
- CPU clock: PPSSPP's "Change CPU clock" (111-444 MHz, or the game's own, the default).
- Volume: game volume 0-100%, also in the in-game options.

## What's new in build 156 (GitHub release ver 0.1)

- The app is now called "PPSSPP PS3 ver 0.1 (build 156)" (XMB, menus and logs).
- Silent Hill Origins' loading screen (the red "Silent Hill Origins" picture) showing only its top
  part, most likely fixed. Your Silent Hill logs show the game writing that picture with the CPU
  into a buffer the RSX had already drawn into; with "VRAM sync: Minimal" (the default) the RSX's
  older copy was shown. New setting "CPU writes to drawn buffers" (advanced column): such a buffer
  takes the CPU's rows before it is shown or read. Auto turns it on for Silent Hill Origins only
  (ULES00869, ULUS10285); other games work as before. Per game: cpurows=1 (on) or 0 (off).
- Midnight Club's intro videos: your logs since build 106 show the same thing every time. With
  Speed limit on, the game shows only ~4.5 new video frames a second while the emulator spends
  40-60% of the time waiting to keep real-time speed, so the PS3 isn't too slow for them: the
  game itself waits for something. Not fixed in this build.

## What's new in build 155

- Your build 153 log: the race at 25-27 fps but 83-90% speed. Per frame ~36 ms: the game's code
  ~15 ms, graphics commands ~22 ms. Part of that is the emulation thread waiting ~3-4 ms for a free
  slot in the GE worker's queue, though the worker only works ~19 ms a frame.
- The GE's exact matrix products are reused when a draw's transform is set up again with the
  same matrices (results bit for bit the same).
- launch.log: "GE worker waits" shows what holds the worker up (the RSX's command buffer being
  full, RSX fences, and its slow jobs by kind): that's where the queue waits come from. Several
  launch.log lines that are only written every 10 s counted 10 s of work as one second's frames
  (numbers ~4-5x too big); fixed.

## What's new in build 154

- "Sound when slow: Play slower" is back to how it was before build 153 (down to half speed).

## What's new in build 153

- From your build 152 log: about 4/5 of the graphics time on the emulation thread is the 3D draws
  themselves; of that, preparing each draw for the GE worker (copying its vertices, state and
  parameters, turning strips into lists) is ~1/4, and waiting for a free slot in the worker's
  queue ~2-4 ms a frame.
- The worker's queue has 96 slots instead of 64 (fewer waits; ~2 MB more memory at most).
- A triangle strip or fan is turned into a list only when another draw joins it, not for every
  draw (most never get one).
- "Sound when slow: Play slower" slows down to 80% at most (was 50%: too slow in short dips). Undone in build 154.
- Your log had "Sound mixing: Own thread": it cost ~8 ms of every ~35 ms frame there.
  "Emulation thread" is faster; if it sounded wrong, tell me what you heard.

## What's new in build 152

- Your build 151 log (Sound mixing: Emulation thread, Speed limit and Frameskip off): the race at
  21-25 fps, 86-106% speed. Per frame the game's own code ~20 ms and the graphics commands ~26 ms
  on the emulation thread.
- Each queued 3D draw copied the whole 1 KB palette; now only the part the texture can use
  (Midnight Club's palettes are mostly 16 colours, 64 bytes). Small gain.
- launch.log: every graphics command handler is now timed (not 1 in 16, whose numbers didn't
  add up), "GE command loop" shows handlers vs the loop itself, and "new T&L jobs" splits the
  per-draw preparation. These show what to speed up next.

## What's new in build 151

- Your build 150 log: with "run ahead" the game's thread still waited for the list thread almost
  all the time. Midnight Club itself waits: right after handing over a frame's lists it calls
  sceGeDrawSync (wait until the GE is done), so its next frame never starts while the GE works.
  A real PSP waits there too; so no timing change lets the two run side by side.
- "GE lists" has a fourth choice, "Own thread, don't wait" (HACK): the game's wait for its lists
  returns at once and the game goes on to its next frame while the list thread still draws this
  one. Midnight Club keeps two display lists and swaps them, so this may work; if the game reuses
  other data the lists still read, models or textures may glitch for a frame. This is the last
  big lever on that side; if it doesn't help, the remaining time is the game's own code and the
  graphics work themselves. Per game: gelists=3.

## What's new in build 150

- Your build 149 log: the list thread now runs in the race, but the game's thread still waited
  for it almost the whole time (~57 of ~59 ms a frame), so nothing was gained. The reason: with
  the exact timing, the game's next frame can't start before the emulated GE is done with this
  one, and the game waits for that right away.
- "GE lists" has a third choice, "Own thread, run ahead" (EXPERIMENTAL): the game's code goes on
  into its next frame while the list thread is still working through this one. The game learns
  the GE is done only when it really is (as if the GE were slower), so it shouldn't reuse memory
  the lists still read; but the GE's timing is not the same as before, which some games may not
  like (glitches, slowdowns, hangs). This is the one that can make it faster. Per game: gelists=2.
- launch.log: the "waits ... by cause" numbers were too large (they covered 10 s, not 1 s).

## What's new in build 149

- "GE lists: Own thread" should now also work in Midnight Club's races. In your build 148 log it
  only ran in the menus. Most likely the game hands over its race lists from an interrupt
  handler, which build 148 kept on the emulation thread; build 149 allows it there too. (So the
  ~1 fps you saw was probably not this setting.)
- New setting "Lens flare occlusion" (Advanced and in-game options; default On, as before). Off:
  the depth buffer Midnight Club reads once a frame (I believe for hiding the sun's lens flare
  behind buildings) isn't copied back from the RSX. Saves ~2.5 ms a frame; the flare may show
  through buildings, or not at all. Per game: lensflare=0.

## What's new in build 148

- New setting "GE lists" (Advanced, and in the in-game options; default "Emulation thread", as
  before), EXPERIMENTAL. "Own thread": the game's graphics lists are worked through on a thread of
  their own while the game's code runs on, instead of one after the other. Your build 146 log showed
  nearly all of the game's own work (~50 ms a frame in the race) happens while the graphics lists
  could be running, so this is the change that can give a bigger jump. It keeps the GE's interrupts
  and timing as before (the emulated clock doesn't run ahead of the list thread), so games should
  behave the same; but it's new: it may glitch, hang or crash. If it does, switch it back. Can be
  changed in a game (START+SELECT). Per game: gelists=1.
- launch.log (with it on): "Perf: GE lists on their own thread" and "waits for the GE list thread
  ... by cause": how much time the game's thread still waits for the lists, and why.

## What's new in build 147

- Settings menu: rows closer together again (21 per column, close to before build 143), with the
  selection bar inside its row. The Advanced column has 2 more rows than fit and scrolls.
- In-game options: the whole list again, no scrolling.

## What's new in build 146

- Settings menu: rows keep their normal spacing again (since build 143 the longer column was
  squeezed and the selection bar ran into the next row). A column with more rows than fit now
  scrolls with the selection; "^ more" / "v more" next to the column title shows there are more.
- Also has build 145's "Perf: async GE study" line.

## What's new in build 145

- Measurement for the next big change (running the game's graphics lists on another thread while
  the game's code runs on): launch.log has a new "Perf: async GE study" line. It shows how much of
  the game's own work happens between handing the GE a list and waiting for it - the time that
  could overlap. Nothing else changed; play a race for a minute with your usual settings.

## What's new in build 144

- In-game options open with START+SELECT now (Debug on), instead of L3+R3. START+SELECT or
  Circle goes back to the game. With Debug on, START or SELECT pressed alone reaches the game
  0.15 s late, so the game doesn't get them when you open the options.
- The list shows 6 options at a time and scrolls (^ and v mark more above or below), so it covers
  less of the game.
- The "Quit combo" setting is gone (use the PS button menu to quit). L3+R3 does nothing now.
- Your build 143 log: "JIT cache: Large" worked - the JIT went past 70000 blocks with no cache clear
  (Normal clears at 65536), and 24 MB of memory was still free.
- Note: launch.log's Perf lines that cover time spent in the in-game options show very low fps.

## What's new in build 143

- In-game options: with Debug on, press L3+R3 together in a game. The game pauses and a list of
  options shows in the top left corner: Frameskip, Speed limit, Frame cap, GE timing, FPS overlay,
  Sound when slow, File loading, Filtering, Draw merging, Keep buffer layouts and Right stick.
  Up/Down chooses, Left/Right/X changes (it applies at once), L3+R3 or Circle goes back to the
  game. Changes are saved to settings.ini (a per-game file in games/ still wins for its game).
  Options that need a restart (resolution, renderer, CPU core, memory...) are only in the main menu.
- L3+R3 no longer stops the game; the Quit combo is START+SELECT held 2 s only.
- R3 alone (FPS overlay) now switches when released, so it doesn't switch when L3+R3 is pressed.
- Your build 142 logs: with "File loading: Host" the waits for disk reads are gone from the race.
  The remaining slow frames (PSP CPU 200-480 ms) come with JIT compiling, and some come right
  after the JIT cache filled up (61000+ blocks) and was cleared, so all code compiled again.
- New setting "JIT cache" (Advanced; default Normal, as before), EXPERIMENTAL. Large: room for 50%
  more compiled code before the cache is cleared. Uses ~9 MB more memory. Per game: jitlarge=1.
- launch.log: "JIT cache full: ..." says which limit was hit each time the cache is cleared
  (blocks, IR instructions or native code space). If it's the native code space, Large won't help.

## What's new in build 142

- Your build 141 log: "Sound mixing: Emulation thread" works - the sound mix went from 19-31 ms of
  every drawn frame to under 1 ms of waiting (the mixing itself, ~10 ms, now shows as
  __sceSasCoreWithMix).
- New setting "File loading" (Advanced; default Fast, as before), EXPERIMENTAL. Your log's slow
  stretches in the race (up to ~200 ms a frame) are the game streaming the city from the disk
  image: the whole emulator waited for each read (IoAsyncNotify, up to 78 ms a frame). Host: the
  read finishes on the IO thread while the game runs on, as desktop PPSSPP's "Host" I/O timing
  does. Per game: iohost=1.
- launch.log: the GE command line shows how often each command runs (its times didn't add up).

## What's new in build 141

- "Sound mixing" (Advanced) has a new choice: Emulation thread (EXPERIMENTAL; default Own thread,
  as before). Your build 140 logs: the game's sound mix (SAS) took 19-31 ms of every drawn frame
  - with your faster settings ~40% of the PSP CPU time - though an SPU does each mix in a small
  fraction of a millisecond. The time went to hand-overs: the emulation thread woke the sound
  thread, which handed the mix to the SPU and slept until it answered, then woke the emulation
  thread again, each wake-up waiting for a turn on the busy PPU (~0.3 ms a mix). Emulation thread:
  it gives the SPU the mix itself and watches for the answer, no hand-overs. The same sound.
  Per game: sasdirect=1. (This replaces build 138's "Don't wait", which didn't help.)
- The matrix change in build 140 shows in your JIT profile: Midnight Club's matrix routine went
  from ~1,800 to ~1,430 PPU cycles a call.

## What's new in build 140

- "Sound mixing" description: no longer says it is faster in Midnight Club (your build 138 log
  showed it isn't). Leave it on Wait.
- VFPU matrix multiplies (vmmul, vtfm and the like - Midnight Club runs them for every object)
  take each lane straight from its vector register. Before, the JIT stored the whole column and
  read single lanes back from memory right away, a load-hit-store stall each (12 in Midnight
  Club's main matrix routine, which is now 271 instead of 304 PPU instructions). Same results
  (checked against the interpreter). A small gain: the JIT profile shows the game's code time is
  spread over thousands of blocks, not in a few slow ones.

## What's new in build 139

- New XMB icon, without "Frontend" (and without the build number; the XMB title has it).
- Your build 138 log: Midnight Club loads a palette (LOADCLUT) before nearly every draw, ~25,000
  a drawn frame, almost always the same one. Each load made the next draw compute a whole new
  draw state - so the GE thread rebuilt its RSX state for nearly every draw, and draws couldn't
  be merged. Now only a palette that really changed does that. The same draws reach the RSX
  (checked on the Killzone, Gran Turismo, Silent Hill and Dead or Alive dumps: identical).
- "Sound mixing: Don't wait" (build 138) didn't help much in your log: the mix takes longer
  than it can wait, so it ends up waiting anyway. Keep it Off.
- launch.log: the GE command timing no longer lines up with the game's repeating command
  pattern (the times in build 138's line were skewed).

## What's new in build 138

- The name: no more "Frontend". The app is "PPSSPP PS3 build 138" (same app: it installs over
  the old one and keeps your settings and logs).
- Your build 137 log: in game, ~1 depth read a frame cost the GE thread ~3.5 ms. It converted
  ~140,000 depth values with a 64-bit divide each (slow on the PPU); the divide is now a shift and
  add that gives the same value for every one of the 16.7 million inputs (checked). The GE thread
  is the slower one in drawn frames, so this is time the emulation thread no longer waits.
- New setting "Sound mixing" (Advanced; default Wait, as before), EXPERIMENTAL. Your log: the
  game's sound mix (SAS, done on an SPU) stopped the whole emulator 5-12 ms a frame until it was
  finished. Don't wait: only the game's sound thread waits for it; its other threads (the
  renderer) keep running meanwhile. Per game: sasnowait=1. If sound or the game misbehaves with
  it, set it back to Wait.
- launch.log: "GE command handlers per frame" - which GE commands take the GE time that isn't
  drawing (~17 ms a drawn frame).

# PPSSPP PS3 Frontend 137

## What's new in Frontend 137

- Your Frontend 136 log: the draw state setup went from ~10 ms to ~2.5 ms a drawn frame. It also
  shows that in drawn frames the GE thread is now the slower side (45-55 ms a drawn frame; the
  emulation thread waits 7-17 ms for a free slot), so it has to get faster before it can take over
  work from the emulation thread. Most of its time goes to texture lookups (~8 us each, of which
  the actual decoding and hashing is a small part).
- Texture lookups hash only the palette entries the texture can reach (a 16-colour texture: 32
  bytes instead of the whole 1 KB, every lookup). Same results (checked for every palette
  setting).
- launch.log: texture lookups split into their parts, and the time in timed events (the screen
  refresh, audio, GE interrupts...) by kind: the next things to cut.

# PPSSPP PS3 Frontend 136

## What's new in Frontend 136

- Your Frontend 135 log split the draw state: ~1,300 new draw states a drawn frame, ~4 us each
  just for the "pixel setup". The cause: the software renderer looks up compiled code for each new
  state (pixel, texture: three lookups), and on the PS3 there is no such compiler (it only exists
  for x64 PCs). Every lookup missed, emptied the draw queue and tried to compile, for nothing -
  every time. They now skip straight to the code they always ended up using. Same results.
  Expected: several ms less per drawn frame in Midnight Club (and less in every 3D game).
- New setting "Sound when slow" (General, under Sound; default as before). While a game runs
  below full speed there isn't enough sound for real time. Short gaps (as before): crackles.
  Wait and refill: fewer, longer gaps. Play slower: no gaps, the sound slows down and drops in
  pitch. Per game: audioslow=0/1/2.

# PPSSPP PS3 Frontend 135

## What's new in Frontend 135

- Your Frontend 134 log (Frameskip 1): the GE work is almost all in the drawn frames' draws
  (~52 ms of ~60), not in the command loop. Part of it: each 3D draw that moves an object
  combines its matrices exactly as the PSP's GE does, and that took apart every float into its
  bits ~200 times a draw - on the PS3's PPU each of those moves stalls ~40 cycles (~6 us a draw,
  8-9 ms a drawn frame in Midnight Club). It's now done in integer registers only. Same results,
  bit for bit (checked on 25 million cases against the old code, on x86 and PowerPC).
- launch.log: "draw state per frame" splits the other big part (the draw state, 10-13 ms a
  drawn frame) to see what to cut next.

# PPSSPP PS3 Frontend 134

## What's new in Frontend 134

- Your Frontend 133 log with "Keep buffer layouts" On: the layout write-backs are gone (only
  the depth read is left, already one frame late), the emulation thread waits ~5-7 ms a frame
  for the GE thread instead of 13-31, and the game runs 27-31 of its own frames a second (was
  ~24). The emulation thread is now all of it: PSP CPU ~90-110 ms and GE ~70 ms a drawn frame.
- The JIT's VFPU compares (vcmp, ~150,000 lanes a frame in Midnight Club) and FPU compares
  (c.lt.s and the like) take the result straight from the compare: 5 PPU instructions a lane
  instead of 11, with one slow vector-to-condition step less. Same results (checked against the
  interpreter on the PSP test programs and on Midnight Club's own blocks). A small gain.
- launch.log: "GE commands per frame" (drawn and skipped frames), to find where the ~30 ms of
  GE time a frame that isn't draw commands goes.

# PPSSPP PS3 Frontend 133

## What's new in Frontend 133

- New setting "Keep buffer layouts" (Advanced, below Late depth reads; Off by default,
  EXPERIMENTAL). Your Frontend 132 log shows Midnight Club L.A. drawing one buffer (04156000) in
  two layouts by turns, 128 pixels wide 16-bit and 256 wide 16-bit, several times a frame. Each
  switch stopped everything until the RSX had finished, copied the buffer back to PSP memory,
  threw it away and loaded it again: about a third of the GE thread's time in your log. On: each
  layout keeps its own copy on the RSX, no wait at a switch (desktop PPSSPP's hardware renderers
  work this way). If the game's own code reads those pixels, it gets old ones: if something
  looks wrong with it On, turn it Off. Per game: keeplayouts=1. Try it with Late depth reads On.
- vexp2/vlog2 (about 13,000 a frame in Midnight Club) run as direct calls in the JIT instead of
  going through the slow interpreter fallback (same results).
- launch.log: write-backs are now split by cause, with their time ("write-backs by cause").
- JIT profile (Debug on, hold L3): measuring starts 3 s after the cache clear, and compiling is
  left out of block times. Before, most of a 5 s sample was the game's code being compiled again.

# PPSSPP PS3 Frontend 132

## What's new in Frontend 132

- The JIT profile (Debug on, hold L3 in a game) no longer counts HLE calls in the time of the
  game code that made them. Before, the GE's display-list work (inside sceGeListEnQueue) made a
  few two-instruction blocks look like the slowest code in Midnight Club L.A.; now the profile
  shows which of the game's own code costs the most. Your Frontend 131 log: the PSP CPU is now
  the biggest part of a drawn frame (about 90-110 ms, more than the GE), so that is next.

# PPSSPP PS3 Frontend 131

## What's new in Frontend 131

- "Core logging" is back as its own setting under Debug (Off / Warnings and errors / Full debug).
  It needs Debug on and is greyed out while Debug is off. Debug on with Core logging off: only
  launch.log, stdout.log, the settings and the per-game log folder. Both on: core.log too.
  Debug off: no logs at all.
- Shorter setting descriptions: all of them fit the 3-line box now.
- Midnight Club L.A. speed: your Frontend 130 log shows Late depth reads working (the GE thread
  went from 100 to 63 ms a frame, 3.3 to 4.5 frames a second). The emulation thread is now the
  slow part: ~100 ms a frame in draw commands, of which half was not broken down. launch.log
  now has one more line, "draw commands per frame by path", that splits it (draws added to the
  open hardware job, draws in skipped frames, the rest) - needed to aim the next step.

# PPSSPP PS3 Frontend 130

## What's new in Frontend 130

- New "Debug" setting (general column) instead of "Debug buttons" and "Core logging". Off (the
  default): no logs are written at all - no launch.log, core.log, stdout.log or per-game log
  folder, no performance reports - and the in-game test buttons are off. On: all the logs
  (core.log with PPSSPP's warnings and errors) and the test buttons (hold L3: JIT profile, R3: FPS
  counter, L2+R2: GE dump). Turn it on only to make a report: logging costs time, Midnight Club's
  core.log alone got thousands of lines a minute. (ini: debug=1; log=2 for PPSSPP's full debug log.)
- Midnight Club L.A. speed, second step: new setting "Late depth reads" (advanced column, off by
  default - turn it on for Midnight Club, or latedepth=1 in its ini). Once a frame the game reads
  its depth buffer as a texture for an effect; to give it the current depth, the RSX had to finish
  drawing the whole scene first while the GE thread waited (your 129 log: the copies back still
  took about half of the GE thread's time). With the setting on, the game gets the depth from the
  frame before, copied while the RSX keeps going - the effect can lag one frame behind.

# PPSSPP PS3 Frontend 129

## What's new in Frontend 129

- Midnight Club L.A. speed, first step. The biggest single cost in your Frontend 128 log was the
  renderer copying pictures from the RSX back to PSP memory (about 80 ms of every frame on the GE
  thread, the RSX stopping each time to finish its queue). Most of those copies were for nothing:
  the game reads its depth buffer as a 512x512 texture but only uses the top 272 rows of it, and
  the rows below are its small effect pictures - so each time it drew a strip into one of them, the
  picture was copied back. Now only the rows a 2D draw actually reads are checked. In a Midnight
  Club GE dump that is 2 copies a frame instead of 7; the other games' dumps are unchanged.
  Expect it faster, not yet fast: the next steps come from the next log.
- "Skip keeps menus" description: the warning about Silent Hill Origins is gone (it was the
  Frontend 114 change that broke Silent Hill, not this option).

# PPSSPP PS3 Frontend 128

## What's new in Frontend 128

- Silent Hill Origins: the Frontend 114 change is removed. When the game switched to a picture
  sharing memory with another one, the renderer first copied the other back to PSP memory and
  reloaded it. Silent Hill's pictures overlap all the time, so it did that constantly (slow, and
  the texture glitches you saw from 114 on). Midnight Club's green blocks were fixed by
  Frontend 127 instead, so that change should no longer be needed there.
- Core logging now defaults to "Warnings and errors" (only for a fresh settings file; an existing
  one keeps its choice: Core logging in the menu).

# PPSSPP PS3 Frontend 127

## What's new in Frontend 127

- Midnight Club L.A. freeze on "Create Profile": found in your IR interpreter log. The game keeps
  a memory manager in video memory, right after its small 256-wide effect picture: block headers
  (and the small display list from Frontend 125) from row 162 of that picture on. The game only
  draws 160 rows into the picture, but the renderer copied back every row the drawing area allows
  (272), over those headers. "Create Profile" frees a lot of that memory, walks the broken headers
  and loops forever. The renderer now copies back only the rows the drawing actually covered (for
  2D drawing, where the corners are known; before, only the columns were limited this way). In a
  Midnight Club GE dump the copy-back now stops at row 160, right before the memory manager.
- The same change stops Midnight Club's 64-wide effect picture from being copied over the first
  row of the picture after it (it was copied back 272 rows deep, over the next two pictures), so
  the green triangles may be gone as well - not confirmed yet.
- Silent Hill Origins: its 1024-wide work buffer is now copied back 272 rows deep, the rows it
  draws, instead of 512.

# PPSSPP PS3 Frontend 126

## What's new in Frontend 126

- Logs only, no change to how games run. When a game stops showing frames for 5 seconds (a hang),
  core.log is now copied to the game's log folder right away (and again every 30 seconds of it),
  so it is there even after a forced power-off. Before, it was only copied when the game was quit
  normally, and the next start of PPSSPP emptied it.
- Midnight Club L.A.: Frontend 125's fix for the brown screen is confirmed by your log. The freeze
  on the "create profile / load profile" screen is a separate problem, still being looked at.

# PPSSPP PS3 Frontend 125

## What's new in Frontend 125

- Midnight Club L.A. brown screen / hang: the Frontend 123 fix is now actually switched on. Your
  124 log had none of the lines it should have had, which showed that the check never ran: the
  software GE (which the PS3 build uses) handles the jump into that small list with its own code,
  and only the other one had the check. Tested here with a made-up frame that copies Midnight
  Club's layout (the small list right after the 256-wide effect picture): before, the copy-back
  turned the list into pixels and the GE hung on them; now the list stays intact, and launch.log
  says "would have changed the display list at ... - left as it was".

# PPSSPP PS3 Frontend 124

## What's new in Frontend 124

DIAGNOSTIC build for Midnight Club L.A. - it does not fix the brown screen.
- Your logs (122 and the 119 GE dump) show where the junk is: the small display list the game keeps in video
  memory (it calls it 7 times a frame). Frontend 123 showed the renderer's copies did not overwrite it, so
  something else does.
- Now launch.log shows what that list holds the first time it runs, and every time it changed
  since its last run (old and new contents, which frame). Emulator-side writes into it (copies,
  block transfers, file reads) are named too. No slowdown: only that one small list is checked.

# PPSSPP PS3 Frontend 123

## What's new in Frontend 123

- Midnight Club L.A. brown screen / hang: cause found in your Frontend 122 log. The game keeps a
  small display list in video memory, right after a small effect picture it draws each frame. When
  the PS3 renderer copied that picture back to PSP memory, it copied every row the drawing area
  allows, and those rows reach the display list. The junk the GE ran (the brown screen, the bad
  jumps in core.log) looks like pixels, and the list is called right where the junk was, so this
  is the most likely cause - not yet confirmed on the game itself. Now the renderer remembers which display lists the game runs from video memory and
  never copies pixels over them. If it would have, launch.log says so once ("would have changed the
  display list at ... - left as it was").
- The Frontend 122 memory watch is gone (it slowed the game to a crawl, which looked like a freeze).

# PPSSPP PS3 Frontend 122

## What's new in Frontend 122

Diagnostics for Midnight Club L.A.'s brown screen (no change to how games run until it happens).
The screen still went brown in Frontend 121 with "Dialog memory wipe" Off, so the wipe was not the
cause. Leave that setting Off (its description now says so plainly). Nothing the emulator does writes into the game's display
lists, so it's the game's own code that puts data there. Now, the first time the GE runs into
that data, every write the game makes to its display lists from then on goes to launch.log
("Memory watch: ...") with the code doing it - the first 1,500 writes, and the code around each
routine once. The game runs slower from that moment (each write is checked).

# PPSSPP PS3 Frontend 121

## What's new in Frontend 121

- Midnight Club L.A.'s brown screen / hang: found in your Frontend 120 log. The game keeps data
  (its display lists among it) in "volatile memory", lends that memory to the save data dialog
  (profile autosave), and takes it back afterwards expecting its data to be there. When the
  dialog closed, the emulator filled all 4 MB with zeros ("simulating" what the system does with
  it) - twice in your log, right before the brown screen. The PS3 build no longer does that by
  default; the new setting "Dialog memory wipe" (advanced column) turns the old behaviour back on,
  in case another game turns out to need it (also per game: volwipe=1).
- New setting "Skip keeps menus" (experimental, off by default), under Frameskip: with frameskip
  on, skipped frames still draw what a game draws only now and then (menus, loading screens).
  Helps Gran Turismo's menu texture glitches; broke something in Silent Hill Origins, so best
  turned on per game (skipkeep=1 in the game's ini). It is the Frontend 118 change, now optional.
- core.log is now copied to the game's log folder also when the game is quit from the PS button
  menu.

# PPSSPP PS3 Frontend 120

## What's new in Frontend 120

- The Frontend 119 texture copying is taken out again: it changed nothing in Killzone or Silent
  Hill, so it was only extra work.
- Videos: Auto now plays videos in every game, Killzone included (through the built-in video
  decoder, as On already did; Killzone's movies play fine that way now). The setting's text no
  longer mentions Killzone.
- Midnight Club diagnostics, fixed: the game keeps its display lists in "volatile memory"
  (08400000-08800000), which the list watch skipped as system memory - so it never watched them.
  It does now, and launch.log also notes when that memory is locked and unlocked (the save data
  dialog borrows it, and clears it when it closes).

# PPSSPP PS3 Frontend 119

## What's new in Frontend 119

- The Frontend 118 frameskip change is taken out again (frameskip works as in 117), to come
  back when there's time to test it.
- Killzone's white HUD boxes and Silent Hill Origins' "continue" losing letters: your tests (GE
  thread off: mostly gone; draw merging off: gone) point at the drawing thread running behind
  the game. When a GE call ends, the game's code runs again and can rewrite a texture that queued
  draws still have to read; a real PSP's GE would have drawn them by then. Silent Hill renders
  its menu words into textures in main memory on the CPU (the dump shows "continue" as its own
  128x32 texture). Now, at the end of each GE call, the queued draws not started yet get a copy
  of their texture as it is at that moment (up to 64 KB each; not for textures where anything
  has been drawn to).
- Midnight Club diagnostics: the display list watch now also covers the memory copies that
  weren't reported on the PS3 (the game's memcpy/memmove done by the emulator, DMA copies, zlib
  decompression, GE block transfers). In your 117 log nothing reported writing into the list,
  so either one of these did it or the game's own code did.

# PPSSPP PS3 Frontend 118

## What's new in Frontend 118

- Frameskip: a skipped frame used to skip every draw. Now it skips only the draws into a buffer
  the game also drew into in one of the last few frames (the screen it redraws every frame). A
  buffer drawn once, or again after a while - a menu background, a loading screen, a picture
  drawn off screen to be used later - is always drawn. For Gran Turismo's menu and loading
  screen glitches, which went away with frameskip off. Games that redraw everything every frame
  skip as before.
- Includes Frontend 117 (Killzone HUD / Silent Hill text fix, Midnight Club diagnostics).

# PPSSPP PS3 Frontend 117

## What's new in Frontend 117

- Killzone's white HUD boxes / white smoke, and probably Silent Hill Origins' "continue" losing
  letters: with draw merging on, the last 3D draw of each GE call was held back so later draws
  could join it, and only went to the drawing thread at the next GE call - after the game had
  run again and could have changed the texture memory it reads. It is now sent off when each GE
  call ends. (Draw merging Off fixed Killzone in your test; this keeps merging within a call,
  where Gran Turismo gets nearly all of its merges: it makes ~10 GE calls a frame.)
- Depth written back for a texture read (Frontend 113) stops at row 272.
- Midnight Club diagnostics: the "GE list watch" lines no longer fill up with the system
  dialog's own list (all 24 were used on it in your Frontend 116 log).

# PPSSPP PS3 Frontend 116

## What's new in Frontend 116

Diagnostics for Midnight Club L.A.'s brown screen (no change to how games run). The GE dump
taken on the brown screen shows the game's display list itself is damaged: about 1.3 KB in the
middle of it (where the world matrix and the draws should be) holds data instead of commands,
and one of those words reads as a 15,812-point line strip - the lines on screen. launch.log
now lists the first display list addresses, and any emulated write (file read, audio decode,
video decode, memory copy...) into memory the GE was recently given as a display list, with what
wrote it ("GE list watch: ..."). Nothing there means the game's own code (or something outside
the emulated system calls) wrote the data.

# PPSSPP PS3 Frontend 115

## What's new in Frontend 115

- Killzone: the menu background (and likely the in-game texture flicker) went wrong only with
  "Fast 3D path: RSX transform & lighting" (fine with "On, CPU transform"). One triangle of a
  draw at a time showed garbage texture coordinates, as if the RSX read an old copy of the
  draw's first vertices. A guess at the cause: the RSX's vertex cache still holding that memory
  from the draw before (whose index data ends where the next draw's vertices start). The RSX
  vertex cache is now invalidated before each T&L draw, as GCM requires when vertex memory is
  rewritten. Needs testing with the RSX transform & lighting setting back on; Gran Turismo has
  thousands of these draws a frame, so watch its speed too.
- Includes Frontend 114's Midnight Club overlapping-buffer fix.

# PPSSPP PS3 Frontend 114

## What's new in Frontend 114

- Midnight Club L.A.: its small effect buffers (at 04154000, 04156000 and 0415e000) share
  memory. When the RSX had drawn into two of them, writing one back to PSP memory made the other
  think the CPU had changed its rows, and it threw its own drawing away (launch.log: "write-back
  ... kept N rows the CPU had changed"). Likely the green blocks on the title screen. Now, before
  drawing into a buffer, any other buffer the RSX drew into that shares its memory is written
  back first. Only Midnight Club does this in my test dumps.
- The brown screen after "Manage Profile" is not fixed yet.

# PPSSPP PS3 Frontend 113

## What's new in Frontend 113

- Midnight Club L.A.: its title-screen effect reads the depth buffer as a texture (through a
  VRAM mirror). The RSX renderer kept depth only on the RSX, so the effect read old memory -
  likely the green garbage. Now, when a texture overlaps a depth buffer the RSX has drawn to, the
  depth is written back to PSP memory first (as 16-bit depth, like the software renderer). Not
  written back if the game put something else in that memory since. No other game in my test
  dumps (Killzone, Gran Turismo, Dead to Rights...) reads depth, so they are unaffected.
- The freeze / brown screen is a separate problem (a display list full of non-command data) and
  isn't fixed here.

# PPSSPP PS3 Frontend 112

## What's new in Frontend 112

- Texture cache fix: a palette (CLUT) texture's cached decode now also depends on the palette
  format word (colour format, index shift, mask and start index). Before, the same texture and
  palette data read with other palette settings got the first decode's colours. Found while
  looking into Killzone's white HUD panels; not shown to be their cause.
- Host test builds: PPSSPP_TEXLOG prints which path each texture lookup takes.
- Includes Frontend 111's stall diagnostics for Midnight Club L.A.

# PPSSPP PS3 Frontend 111

## What's new in Frontend 111

Diagnostics for Midnight Club L.A.'s hang (no change to how games run). In the Frontend 109 log
its main thread loops forever, calling a routine that keeps returning 0, while its other threads
wait (two of them on asynchronous file reads). The stall report in launch.log now also has:
- the HLE (system) calls the game made since the last perf report, most frequent first - what
  the loop polls (a file read, a semaphore, the clock...), at the start of a stall and again
  after 15 s;
- once per stall: the code around the loop, each routine it calls, and 64 bytes of memory at
  the addresses in its argument registers.

# PPSSPP PS3 Frontend 110

## What's new in Frontend 110

- The Frontend 109 frameskip change is taken out (frameskip works as in Frontend 108 again). It
  was meant for Killzone's white flicker, but the flicker stayed, and Killzone got jerkier: it
  held each skip decision a frame late and counted the skip limit in game frames (twice as many
  vblanks for a 30 fps game). The flicker's cause is still open.
- Diagnostics only: the JIT line of the perf report splits "IR to PPC" into code generation (with
  its instruction loop) and linking. By the Frontend 109 logs the IR passes got ~40% cheaper per
  block, but generating the PPC code still takes ~140 us a block on the PS3, many times what the
  same work costs elsewhere; this shows where.

# PPSSPP PS3 Frontend 109

## What's new in Frontend 109

- Killzone's flickering white bands and white letterbox bars (the photo). Killzone grades its
  colours by reading its own screen back as a palette texture in 32-pixel strips, through the
  hidden columns 480-511 (seen in its GE dumps). Auto frameskip decided at every flip - for a
  game drawing 30 frames a second that includes the vblank in the middle of its frame - so a
  frame could have its 3D scene skipped and its grading strips drawn: the grading went over the
  previous, already graded picture again, and that frame was shown (white bands, bars going
  white). Now a skip decision takes effect at the game's own frame boundary (when it sets its
  framebuffer), so a frame is wholly drawn or wholly skipped. Games that don't set framebuffers
  for half a second (GTA in gameplay) work as before.
- CPU JIT fix: vi2s (four 32-bit values packed into 16-bit halves) could get wrong low halves
  when many vector registers were in use: its mask was built in a scratch register that mapping
  the next operand could overwrite. Found by testing the less common VFPU instructions against
  the interpreter, prompted by Midnight Club L.A.'s map screen (its GE commands and vertices came
  out as 16-bit garbage - brown screen with stray lines); whether this was its cause isn't
  certain. vdot's result was in a similar spot (not known to have gone wrong) and is moved to a
  safe one. Checked with the block test tool (a test routine with 32 vector registers live and
  ~35 kinds of VFPU instructions: identical to the interpreter) and the CPU tests (unchanged).
- JIT compiling allocates far less memory: the IR passes reuse their buffers from block to block
  instead of a new vector per pass (from ~22 allocations per block to ~4 on the host). Killzone's
  slowest spots (27-50% speed) were the JIT compiling the code of a new area - by its log ~0.7 ms
  a block, much more than the work itself explains; the PS3's malloc is the suspect. The IR is
  unchanged (checked on test routines and the CPU tests).

# PPSSPP PS3 Frontend 108

## What's new in Frontend 108

Midnight Club: L.A. Remix, the real cause behind the Frontend 106 fallback. Its launch.log from
106 named the block (089f60d8, a routine that builds a rotation matrix) and its IR: an IR pass
turns "move f12 into lane 0 of a vector" into a blend whose second operand is the FPU group
f12..f15 (Vec4Blend vf0..vf3, vf0..vf3, f12..f15, 0x01). f12-f16 are kept as single values in
fixed registers in this build, so that group can't be mapped as one vector. Such a blend is now
done as single-value copies of the lanes it takes. The routine is compiled again instead of
running in the interpreter (the 106 fallback stays for anything else of the kind). Checked with
the block test tool on a copy of the routine (identical to the interpreter over 200 random runs;
before the fix it fell back the same way as on the PS3) and the CPU tests (unchanged). Includes
Frontend 107.

# PPSSPP PS3 Frontend 107

## What's new in Frontend 107

- Gran Turismo speed (graphics commands). In a race the game sends ~126,000 GE commands a frame,
  and ~60% of them load the world matrix: the same identity matrix, again for every two draws
  (from the race GE dump taken with Frontend 105). Each went through its own handler call, which
  on the PPU meant stores and reloads of the loop's state around every call (load-hit-store
  stalls). A run of world matrix commands is now handled in one go inside the command loop, as
  PPSSPP's hardware renderers do. Every game frame benefits, including the ones auto frameskip
  doesn't draw. Checked on all 17 GE dumps here (GT, Killzone, Silent Hill Origins, others): the
  matrix state every draw sees, and which draws are merged, are identical to Frontend 106.
- The XMB in-game menu no longer quits the game by itself when it hasn't appeared after 30 s
  (that was for older builds where the menu could hang).

# PPSSPP PS3 Frontend 106

## What's new in Frontend 106

Midnight Club: L.A. Remix froze during the menu boot. Its log showed the CPU JIT hitting an
internal check while compiling ("Cannot MapNativeReg a static reg mismatch"). The PS3 build keeps
the most used FPU registers (f0-f2, f12-f16) in fixed vector registers, one value each, and some
block needed one of them as part of a 4-value group. The check only logged and went on, so the
block's code was broken and the game hung in it. Now such a block is compiled again without the
4-lanes-at-once patterns, and if that still needs it, the block runs in the interpreter (as
blocks do for their first runs anyway). Either way launch.log gets a line naming the block
("JIT: block ... mapped a static register in a group") with its IR, so the real cause can be
fixed later. Checked by forcing the fallback on every block in the CPU tests and the block test
tool (results unchanged). Includes Frontend 105 (Killzone).

# PPSSPP PS3 Frontend 105

## What's new in Frontend 105

Killzone speed, two CPU changes:
- Killzone no longer runs with the JIT block counters always on. Frontend 63 turned them on for
  this game only, to find its freeze after the autosave notice; that freeze turned out to be the
  denormal flushing (fixed in the same build), but the counters stayed. They cost every block of
  game code a time base read and two load-hit-store stalls - with Killzone's short blocks, by
  estimate up to a fifth of its CPU time. Holding L3 still turns them on for a 5 s sample (the JIT
  cache is cleared before and after it, a short pause).
- vmin/vmax on 3 or 4 lanes are done 4 lanes at a time in AltiVec, instead of lane by lane (the
  same per-lane sequence, so the same results). A three- or four-lane add, subtract or multiply
  whose result goes over its second operand in place (vadd.t C000, C100, C000) is now fused too.
  One of Killzone's hot routines goes from ~520 to ~370 cycles by the rough timing model. Checked
  with the block test tool (identical to the IR interpreter over 200-300 random runs, including a
  test routine with all the variants) and the CPU tests (unchanged).

# PPSSPP PS3 Frontend 104

## What's new in Frontend 104

GTA Chinatown Wars speed. Its F103 log had ~2400 "RSX renderer: new framebuffer" lines: the game
switches one buffer (04110000) between two widths (stride 256 and 64) twice every frame, and each
switch wrote a line to launch.log. On the PS3's disk a log line costs ~40 ms, so the GE worker
spent ~80 ms a frame just logging. Now only the first 24 are listed, then one every 1024 with the
count. Nothing else changed.

# PPSSPP PS3 Frontend 103

## What's new in Frontend 103

CPU JIT speed, second part of the rework for Silent Hill Origins' light-heavy scenes. The routine
(per vertex, which side of 11 planes it is on - by the code around it, the game culling
triangles) and the small routines in its loop:
- Its third group (two vdot.t, a three-lane subtract and compare) is done four lanes at a time
  too, like the first two in Frontend 102.
- Its three vfad sums no longer go through memory: a sum's lanes are read wherever they are in
  vector registers, two independent sums are interleaved, and a sum that goes into a lane of a
  vector is put there instead of splitting the vector.
- Packing four values into one vector register now loads a lane that is in memory directly,
  instead of storing the other three and reloading all four (a load-hit-store stall).
- A zero vector is made in a register instead of loaded from a constant table.
- A float copied from one place in memory to another (lwc1 then swc1) goes through an integer
  register as it is, without two byte swaps through a vector register.
By the rough timing model, the routine goes from ~1080 cycles a run in Frontend 101 to ~640; the
copy routines before it get somewhat shorter too. Checked with the block test tool (the
routine, the copy routines and a test routine with other variants: identical to the IR
interpreter over 200 random runs each) and the CPU tests (unchanged).

# PPSSPP PS3 Frontend 102

## What's new in Frontend 102

CPU JIT speed, first part of the rework for Silent Hill Origins' scenes with many lights. The
game's biggest cost there is one routine (checking points against 12 planes - likely which lights
reach which objects), written in VFPU code the IR splits into single lanes; on the PS3's in-order
CPU each lane was its own chain of waits. Three such patterns are now done four lanes at a time
again:
- four vdot.t into the lanes of one vector (as a matrix times a vector),
- an add/subtract/multiply of a vector with a matrix row (a lane of four vectors),
- vslt-style compares of a whole vector giving 1.0/0.0.
Same arithmetic in the same order per lane, so the same results. That routine goes from 391 to
310 PPC instructions, and by a rough timing model of the PS3's CPU from ~1080 to ~810 cycles a
run. Checked with a new test tool that runs the routine 200 times with random values through the
IR interpreter and through the JIT and compares every register and memory word (identical), and
the CPU tests (unchanged).

# PPSSPP PS3 Frontend 101

## What's new in Frontend 101

Silent Hill Origins: the white screen in the burning house, fixed (found with Frontend 99's draw
log; "Screen copies: Fresh each draw" made it go away). The game builds its glow in one off-screen
buffer in several 2D passes (copy the scene, shrink, keep the bright parts, blur), each reading
what the one before drew, then adds it over the screen. With "Screen copies: Reused" (the
default, since Frontend 40 for Killzone's speed) every pass read the same copy - the full-bright
scene - so the result came out white. Now a 2D draw that draws over the area a reused copy holds
makes the next read take a fresh copy, for up to 8 such draws a frame; past that the copy is
reused as before (Killzone's ~150 screen-reading draws a frame stay as fast). With this,
"Screen copies" can stay on Reused.

# PPSSPP PS3 Frontend 100

## What's new in Frontend 100

Fix for Frontend 99: the L2+R2 draw log froze the game. Each line was its own write to launch.log
(~40 ms each on the PS3's disk), so 3000 lines took about two minutes. The lines are now
collected and written 64 KB at a time: about a second's pause in all.

# PPSSPP PS3 Frontend 99

## What's new in Frontend 99

Fix for Frontend 98: L2+R2 in Silent Hill Origins quit to the XMB. The memory check before a GE
dump counted all free memory (21 MB), but it was in small pieces and the dump needs blocks of
several MB, so it ran out. The check now also needs an 8 MB piece; without one, L2+R2 writes the
draw log to launch.log instead of recording a dump (as Frontend 98 meant to).

# PPSSPP PS3 Frontend 98

## What's new in Frontend 98

Silent Hill Origins' white screen in the burning house: Frontend 96's guess was wrong (its log
shows no blend by destination alpha at all). Finding it needs to see what the scene draws, and a
GE dump there is refused (it needs several MB of memory in one piece, which the game no longer
has). Now, when L2+R2 can't record a dump for lack of memory, it writes the next 3000 draws to
launch.log instead, one line each (what it draws into, with which texture, blending, alpha test,
stencil, fog...) plus block transfers and the displayed framebuffer. No big buffers needed. Expect
a short pause while it writes.

# PPSSPP PS3 Frontend 97

## What's new in Frontend 97

CPU JIT speed, for Silent Hill Origins' cutscenes (the JIT profiles taken in Frontend 95 show the
game's fog routine as the biggest cost there, as in the fog areas). VFPU sums of a vector's four
lanes (vfad, and a vmul followed by a sum) read each lane back from memory right after the
vector was written there, a load-hit-store stall per lane on the PPU. When the vector is in a
register, the lanes are now summed there, in the same order (same results; CPU tests unchanged).
The routine's stalls of this kind go from 6 to 4. A small gain, a few percent in that routine.

# PPSSPP PS3 Frontend 96

## What's new in Frontend 96

Silent Hill Origins: white screen in the burning house (a white over-blend; the PS2 version has
the same problem on the PS3's PS2 emulator). A likely cause, not yet confirmed: the game's main
framebuffer is 16-bit 565, which has no alpha, so on the PSP a blend by destination alpha uses 0.
Our copy of that framebuffer is a 32-bit RSX surface whose alpha is usually 255, so such a blend
added far too much - white. Destination alpha now counts as 0 for 565 framebuffers, as on the
PSP. launch.log lists the first few blends by destination alpha a game makes (and into which
framebuffer format), so it shows whether this was it.

# PPSSPP PS3 Frontend 95

## What's new in Frontend 95

Silent Hill Origins crashing to the XMB when entering a new area. Frontend 93's log: main memory
was again in small pieces (17 MB free, no piece bigger than 512 KB), and the new area's code made
the CPU JIT's block lists grow (32,000 blocks). A list that grows needs one new block of memory
twice its size - a few MB - which wasn't there: the 4 MB reserve covered the first failure, the
next one ended the game. The JIT's block lists and lookup tables now get room for 65,536 blocks
once, at the start, when memory is still in one piece; with more blocks than that the JIT clears
its cache and starts over (as when its code space is full).

# PPSSPP PS3 Frontend 94

## What's new in Frontend 94

CPU JIT speed. VFPU compares that give 1.0/0.0 per lane (vslt, vsge and the like) came out of the
IR as a compare, a copy of the FPU condition into an integer register, and a move of that register
back into a vector register - one lane at a time. The integer part was thrown away right after
(the game's code never reads it), but on the PS3's in-order CPU it was a chain of stalls in every
lane. Now the 1.0/0.0 comes straight from the compare when the condition and the integer copy are
unused. Silent Hill Origins' busiest routine (run tens of thousands of times a frame in fog) loses 61 of its 452
instructions, including 11 such stall chains. Same results (CPU tests unchanged).

# PPSSPP PS3 Frontend 93

## What's new in Frontend 93

Silent Hill Origins flashlight freeze: the cause, found by Frontend 91's report. The GE worker
decodes some draws' vertices itself (those the SPUs don't take) into buffers that grew to fit the
biggest draw so far. With the flashlight on, a bigger draw came along once main memory was in
small pieces, and the new, bigger buffer could not be had: that was the failed allocation every
time. Those buffers are now fixed (vertex decoding goes in pieces of 512 vertices through one
small buffer), so the worker never asks for memory while drawing. Same drawing results.
Frontend 91's 4 MB reserve stays as a safety net.

# PPSSPP PS3 Frontend 92

## What's new in Frontend 92

Auto frameskip draws more frames. It used to skip a frame as soon as the game was even slightly
behind real time. Silent Hill Origins with the flashlight on takes ~35 ms for a 33 ms frame, so
it skipped every other frame (15 fps), and then waited ~10 ms per drawn frame because skipping
had put it ahead. Now a frame is skipped only once the game is more than a whole frame behind, so
a game a little too slow skips one frame in several instead of every other one. Same speed, more
frames shown. Only the Auto frameskip setting changes.

# PPSSPP PS3 Frontend 91

## What's new in Frontend 91

Silent Hill Origins flashlight freeze, second try. The Frontend 89 log: the extra 8 MB did not
help because memory wasn't simply used up. Right after the flashlight went on, main memory broke
into pieces: 19 MB free, but no piece bigger than 512 KB. Then an allocation bigger than that
failed and the GE worker hung in the middle of a draw.
- 4 MB is set aside at core start. When an allocation fails, it is given back and the allocation
  tried again (so it succeeds instead of hanging), and the GE job buffers are released. The 4 MB
  is taken again later when memory allows.
- launch.log says which thread failed (the GE worker or another one) and lists the return
  addresses on its stack, so the allocation that breaks memory into pieces can be found and fixed.

# PPSSPP PS3 Frontend 90

## What's new in Frontend 90

Speed. launch.log showed every frame spending ~5 ms in present, waiting for the RSX to finish the
previous frame. Part of the reason was our own: right after each flip, the RSX was told to stop
until that flip had happened at the next vblank, so all of the next frame's drawing (which goes
into the game's framebuffers, not the screen) sat waiting for up to a vblank. Now the RSX only
waits right before it copies the finished frame onto the screen buffer, which is the one step
that needs it. Same output; should mean less waiting per frame in games that run below 60 fps.

# PPSSPP PS3 Frontend 89

## What's new in Frontend 89

Silent Hill Origins flashlight freeze. The Frontend 88 log confirmed it: main memory ran out
(a C++ allocation failed, "OUT OF MEMORY" in launch.log), and the GE worker hung in the middle of
a draw, so the game stopped. Memory was already down to ~2 MB once the 3D part started; toggling
the flashlight was the allocation that didn't fit.
- The main-memory region set aside for the RSX is 8 MB smaller (14 MB instead of 22 MB). Those
  8 MB were never used (the renderer needs ~2 MB of it, the RSX self-tests 4 MB) and are now
  free for the emulator.

# PPSSPP PS3 Frontend 88

## What's new in Frontend 88

For the Silent Hill Origins freeze (it keeps coming back in the same area, every time with main
memory nearly gone: ~1 MB that malloc hadn't taken yet). The emulation thread was stuck handing
draws to the GE worker while the RSX sat idle with everything done - consistent with the worker
having died on a failed allocation. Not proven, so this build both acts and measures:
- When less than 4 MB is left that malloc hasn't taken, the GE worker's 64 job slots give back
  all their buffers (they keep up to ~200 KB each otherwise), at most once per report.
- A failed C++ allocation is written to launch.log (thread and memory state) before it fails.
- launch.log reports memory every 10 seconds: malloc's arena, what's in use and free inside it,
  the biggest block malloc can still give, the GE job buffers and the JIT's block count. The stall
  report adds what the GE worker is doing (and for how long) and whether the emulation thread is
  waiting for a free job slot.

# PPSSPP PS3 Frontend 87

## What's new in Frontend 87

One change, from Frontend 86's breakdown: in Silent Hill Origins the CPU paths took 16-24 ms of
every drawn frame for only 25-48 draws. They're its fog: ~1,900 3D sprites (rectangles outside
through mode) a frame, each vertex decoded by VertexDecoder and transformed with the GE's exact
arithmetic on the emulation thread. 3D rectangles now take a direct path like the T&L draws: the
fast direct decode, the transform in plain float math (as the batched triangle path), then the
same checks as before (screen range, clip volume, w > 0) and the same rectangles to the renderer.
Positions differ from the exact path by at most 0.2 pixel and depth by 2 (of 65535); the number of
rectangles is the same in every Silent Hill and Gran Turismo dump. Sprites with lighting, fog,
generated or projected UVs, weights or morphing still take the regular path.

# PPSSPP PS3 Frontend 86

## What's new in Frontend 86

Silent Hill Origins now keeps up with the game's own speed, but auto frameskip shows only about one
frame in three: drawing a frame costs the emulation thread 30-45 ms of GE work on top of the PSP
CPU. Two things for that:
- The GE's exact arithmetic (vertex transform on the CPU paths: 3D sprites, lines, draws the RSX
  can't transform) scaled results with ldexpf, a library call, tens of thousands of times a
  frame; it's now a multiply by the power of two (identical results; ldexpf is kept for the rare
  out-of-range case).
- launch.log breaks the draw commands' time down: state setup, transform state, the direct T&L
  path (merging into the previous draw, preparing a new job) and the CPU paths, so the next
  change can go where the time is.

# PPSSPP PS3 Frontend 85

## What's new in Frontend 85

One change, more CPU speed for Silent Hill Origins (and any game doing vdot.t on aligned vectors):
the AltiVec JIT turns the IR's per-lane form of a 3-element dot product (three multiplies, two
dependent adds, every matrix element loaded on its own) into one vector multiply and the same
two adds in the same order, so the results are bit for bit the same. Two dots in a row are
interleaved, so the in-order PPU isn't left waiting on each add. Silent Hill's hottest block
(ten vdot.t per run, ~75,000 runs a frame) goes from 528 to 452 PPC instructions, on top of
Frontend 83, which measured it 28% faster than Frontend 82 (3209 cycles a run, down from 4464).
The CPU and VFPU tests give the same output as before.

# PPSSPP PS3 Frontend 84

## What's new in Frontend 84

One change, for a rare freeze (Silent Hill Origins in Frontend 81, Call of Duty in 82): both
stopped with the RSX idle and its command pointers (put == get) exactly at the end of a command
buffer segment - the moment the command buffer wraps to its next segment, which libgcm's default
callback does. Whoever called it never came back. The wrap-around is now our own: it lets the
RSX run up to the end of the segment, writes a jump to the next 64 KB segment (back to the start
after the last), and only waits if the RSX is still reading that segment from a lap before. If
that wait ever takes more than 2 seconds, launch.log says what it waits for; the watchdog and
stall reports also say if they happen during a wrap-around, and count the wraps. launch.log
shows the command buffer's layout at start.

# PPSSPP PS3 Frontend 83

## What's new in Frontend 83

One change: the CPU JIT (AltiVec mode, the default) avoids moving values through memory where a
store is followed straight away by a load of the same address (a load-hit-store stall, tens of
cycles each on the PPU). Silent Hill Origins spends about a quarter of its CPU time in one VFPU
routine (run ~50,000 times a frame) that had 27 of these per run; it now has 8. What changed:
- vslt/vsge (and FPU compares whose result is moved into a float register) build the 0/1 from
  the compare result in AltiVec registers instead of going through a GPR and the stack;
- floor/ceil/round/trunc to int (vf2id and friends, floor.w.s etc.) are done in AltiVec instead of
  through the FPU and memory, which also changed FPSCR (a serializing instruction) each time;
- mfc1 of a register just loaded with lwc1 loads the GPR from memory directly;
- a constant vector prefix used lane by lane (vpfxt 0,0,0,0 on vslt) is set as four scalars.
The 47 CPU and VFPU tests give exactly the same output as before under the PowerPC JIT.

# PPSSPP PS3 Frontend 82

## What's new in Frontend 82

One change, for the dark rectangles over Silent Hill Origins' fog: the game's film grain is a
128x128 swizzled 4-bit texture with only 32 texels of data per row (buffer width 32), drawn
additively over the whole screen. The columns past 32 came from beyond the end of its data, which
decoded as black blocks: no grain was added there, so they showed as darker rectangles that jump
around with the grain's random offset every frame. Those columns now repeat the 32 that exist.
Only swizzled textures wider than their buffer width are affected; none of the Dead to Rights,
Gran Turismo or Killzone dumps have one.

# PPSSPP PS3 Frontend 81

## What's new in Frontend 81

One change, for Silent Hill Origins' blocky rectangles in the fog: a framebuffer is written back
to PSP memory only in the columns the RSX drew into (from each draw's scissor, narrowed to its
vertices for rectangles, points, lines and through-mode triangles), and its rows are watched for
CPU changes only there. Silent Hill keeps a small 16-bit blur image in the spare columns (x 480-511)
of its 8888 screen buffer, drawn through a second, 1024-wide buffer; each buffer's whole-row
write-back overwrote the other's pixels, and rows the other had written then counted as changed
by the CPU, throwing away what the RSX drew. Dead to Rights', Gran Turismo's and Killzone's dumps
give the same draws, uploads and write-backs as before (now limited to the drawn 480 columns).

# PPSSPP PS3 Frontend 80

## What's new in Frontend 80

Frontend 79's change is undone (it made Silent Hill worse), and one fix for Silent Hill Origins:
the game shows its two 16-bit screens side by side in one 1024-wide buffer (040d4000 and
040d43c0) and draws both through 040d4000, the right one 480 pixels in. The PS3 showed the right
one from a separate, never drawn copy - black or an old frame every other frame (the flicker
from boot, the pause screen alternating with the game, cut-off subtitles and menu text). It is
now shown from the buffer the game drew into. Every Silent Hill GE dump since Frontend 60
alternates like this; Dead to Rights', Gran Turismo's and Killzone's present the same buffers as
before.

# PPSSPP PS3 Frontend 79

## What's new in Frontend 79

One change, for Silent Hill Origins' menu (flicker from boot): the block-transfer fixes from
Frontend 63, left out when 66 went back to the 58 engine. The game copies each video frame into
its menu screen with a block transfer and draws the text over it; the RSX copy of that screen now
takes the transfer in before the next draw, and a transfer over pixels the RSX drew writes those
back first. Dead to Rights', Gran Turismo's and Silent Hill's GE dumps give the same draws,
uploads and write-backs in the test build as before.

# PPSSPP PS3 Frontend 78

## What's new in Frontend 78

One change: the GE worker no longer gives up the PPU while it waits for the SPUs. Waiting for a
texture decode (after ~2000 spins) or for SPU-prepared vertices (after only 200 spins) yielded
the thread, handing the rest of its time slice to another thread; a wait that ran a little long
then cost a millisecond or more. In Gran Turismo's races a texture decode took ~1.3 ms on
average, where a menu's small ones took ~0.05 ms. It now spins for up to 3 ms before yielding.
Untested guess at where GT's drawing time goes: it may well change nothing.

# PPSSPP PS3 Frontend 77

## What's new in Frontend 77

FFmpeg build, Killzone only: with Videos set to On (not Auto), its movies are played through the
built-in sceMpeg, as desktop PPSSPP plays them, instead of the mpeg.prx on its disc (which crashed
the game on top of our video decoder). Auto and Off are unchanged: the movie is skipped and the
game boots, as in 76. Other games are unaffected.

# PPSSPP PS3 Frontend 76

## What's new in Frontend 76

FFmpeg build: Killzone boots with Videos off or Auto. The two builds' logs from Frontend 75
showed where they parted: sceVideocodecOpen failed without FFmpeg (and Killzone's movie player,
from its disc's mpeg.prx, gave up there and the game went on) but succeeded with FFmpeg even with
videos off, after which the game crashed on a movie object it never set up. With videos off it
now fails as without FFmpeg. The Frontend 75 call log is gone again.

# PPSSPP PS3 Frontend 75

## What's new in Frontend 75

Frontend 74 plus a log of the first 900 calls a game makes to the movie libraries (sceMpeg,
sceVideocodec, sceAudiocodec, scePsmf), memory allocation, module loading, thread creation and
file opening, with arguments and results ("MEDIA:" lines in launch.log). Nothing else changes.
For comparing Killzone's boot in the two builds (it freezes in its movie code only with FFmpeg).

# PPSSPP PS3 Frontend 74

## What's new in Frontend 74

Frontend 72's Killzone screen-copy override is taken out (it wasn't asked for): "Screen copies"
works for Killzone as for every other game again. The renderer is the same as Frontend 68's.

# PPSSPP PS3 Frontend 73

## What's new in Frontend 73

FFmpeg build: with Videos off for a game (or Auto, for Killzone) nothing FFmpeg decodes reaches
it - no H.264, no PMP video, no AAC or MP3 audio - so it gets what the build without FFmpeg
gives it (Frontend 71 still let MP3 and PMP through). The stall report now also lists the
registers and what the last call before the stuck code went to.

# PPSSPP PS3 Frontend 72

## What's new in Frontend 72

- The SPU self-checks from Frontend 69 are gone (they found nothing wrong: 4095 vertex jobs,
  every texture decode and 512 hash checks matched the PPU).
- Killzone always gets fresh screen copies (as "Screen copies: Fresh for every draw"), which
  cut down its menu and chapter-screen glitches. Other games keep the setting.

# PPSSPP PS3 Frontend 71

## What's new in Frontend 71

FFmpeg build only. With Videos off for a game (or Auto, for Killzone), there is no H.264
decoder at all and sceAudiocodec doesn't decode AAC (movie soundtracks), exactly as in the build
without FFmpeg. Frontend 70 only stopped the video frames, and Killzone still stopped after its
autosave notice.

# PPSSPP PS3 Frontend 70

## What's new in Frontend 70

Frontend 69 plus the Videos setting back (Auto / On / Off), and an FFmpeg build again. Auto
turns video decoding off for Killzone only: its movie code crashed on decoded frames (it uses
sceVideocodec directly, which the old Videos setting didn't cover, so Frontend 65 still froze
there). With videos off, Killzone gets exactly what the no-FFmpeg build gives it, which boots.
Other games (Silent Hill Origins' menu background) get their videos. The no-FFmpeg build is the
same as 69 apart from the menu line.

Also: Frontend 68's frame cap change for Killzone is undone. Killzone's game logic moves one
step per frame, so holding it to 30 frames made it run in slow motion. It runs as in 67 again.

# PPSSPP PS3 Frontend 69

## What's new in Frontend 69

Frontend 68 plus SPU self-checks, for the texture glitches that go away with SPU vertices and
SPU textures off. Nothing is drawn differently; the log gets lines starting "SPU vertex check"
and "SPU texture check":

- Every 16th draw whose vertices the SPUs prepared (the first 4096 of them) is also prepared on
  the PPU and compared.
- The first 128 SPU texture decodes are compared with the PPU decoder (and looked at again 5 ms
  later, to tell late writes from wrong ones).
- 512 times, a texture the SPU hash found unchanged is hashed again on the PPU.

# PPSSPP PS3 Frontend 68

## What's new in Frontend 68

Frontend 67 plus one fix: the 30 fps frame cap now holds in Killzone. Killzone draws as fast as
it can and shows every frame at the same address, and the cap only counted a frame when the
address changed, so the game kept drawing 60 frames a second of PSP time, which the PS3 can't
keep up with. The frame skipper then showed only about one of every five. With the cap working,
it draws 30 a second and half as much work goes into frames nobody sees. Only games on the
"render as fast as possible" list are affected (of the tested ones: Killzone and Silent Hill
Origins), and only with Frame cap at 30. The perf log has a new "frame cap" line that shows
whether it's holding the game.

# PPSSPP PS3 Frontend 67

## What's new in Frontend 67

Frontend 66 plus one fix: render target tile regions are set up and released only once the RSX
has finished its queued work. Setting one up while the RSX was drawing hung it now and then (Dead
to Rights sometimes never got past its first frames: fence timeouts, 500 ms flip waits).

# PPSSPP PS3 Frontend 66

## What's new in Frontend 66

A fresh start from Frontend 58, the last version that ran everything well (Killzone included),
with only these on top:

- The new full-screen menus (two columns with help text, larger game list).
- Frame cap 30/60 that holds in every game (from Frontend 60).
- Right stick to face buttons or D-pad (Frontend 60).
- 1 GB memory stick (games that saw a 16 GB stick's free space as too little to save).
- Silent Hill Origins: the screen buffer texture fix (Frontend 61).
- Call of Duty's video frames fix (only matters once videos are back).
- "Denormals (FTZ)" option (Advanced column; Auto = flushed, as in 58), and GE timing (as in 58).

Left out for now, each to come back only if it measurably helps: FFmpeg (videos; it is what made
Killzone freeze), the GE thread running display lists (Frontend 60), the JIT system-call shortcuts
(Frontend 59), and the Frontend 61-65 renderer changes and diagnostics. Without FFmpeg, games skip
their videos, as in 58.

# PPSSPP PS3 Frontend 65

## What's new in Frontend 65

- Killzone: it played through Frontend 58 and froze from 59. Of 59's changes only FFmpeg (linked
  in for video cutscenes) and an IR compiler buffer change touch the IR interpreter too, which also
  froze. Frontend 65 comes as two packages: the normal one, and one built without FFmpeg (as 58
  was). launch.log's "Main entered" line says which one runs.
- Gran Turismo: the T&L index copy into the RSX ring widens and offsets eight indices at a time
  with AltiVec, and zeroes whole destination cache lines (dcbz) instead of reading them from main
  memory first.

# PPSSPP PS3 Frontend 64

## What's new in Frontend 64

Gran Turismo, the cost of each draw (Frontend 60 log: the RSX state setup took ~9 us per draw,
~8 ms a frame):

- The state memo (skip all state work when a draw's state equals the last one's) almost never
  hit: Gran Turismo sends a TEXFLUSH before every draw, and its count was part of the
  comparison. It now only counts when the texture is a render target. Draws whose state differs
  only in the texture (address, size, CLUT) now just look up and bind the texture. On the race
  dump: full state setups 33,050 -> 13,600 of 36,350 draws.
- Texture and CLUT change checks use a hash made for the PPU (AltiVec adds) instead of XXH3,
  whose 64-bit multiplies are slow on the PPU. The CLUT (1 KB) was hashed on every texture
  lookup.
- T&L constants: the registers that changed are found with inline compares, not a memcmp call
  per register.
- Settings: "Denormals (FTZ)" in the Advanced column (Auto / Flush to zero / Keep), the same as
  the per-game `ftz=` key.

# PPSSPP PS3 Frontend 63

## What's new in Frontend 63

- Killzone: going through the older logs, Frontend 57 played it and Frontend 58 is the first
  that froze (same place, same code address as every version since). Of what changed in 58,
  only denormal flushing (FPSCR NI / AltiVec NJ, added for Dead to Rights' NaN camera) affects
  the IR interpreter as well as the JIT, and both froze. It is now off for Killzone (as in 57)
  and on for the rest. New per-game key `ftz=` (1 on, 0 off, -1 auto). launch.log says "CPU:
  denormal flushing off for UCES00279" when auto turns it off.
- If Killzone still stops, the stall report now names the code that runs: JIT block entry
  counters are on for it, and the STALL lines give the last block entered (with its MIPS and
  IR), the blocks run since the last report, blocks compiled/interpreted, and replaced
  functions show up like system calls.
- Silent Hill Origins menu options disappearing: it draws into its menu buffer, copies its
  background into the same buffer with a block transfer, then draws the text. The transfer
  didn't make the RSX renderer write its copy back first (the check was skipped when the
  current list had no draws left), and the renderer then kept drawing on its old copy: the
  write-back afterwards kept the transfer's rows from VRAM, without the text. Transfers now
  write back render targets they overlap, and a transfer into the bound render target is
  taken into the RSX copy before the next draw.

# PPSSPP PS3 Frontend 61

## What's new in Frontend 61

From the Frontend 60 logs and videos:

- Killzone: the freeze was never the saving (the new log shows the save check done in 44 ms). It
  comes when its intro movie starts. Video cutscenes play since Frontend 59 (FFmpeg); before
  that Killzone couldn't open its movie and skipped it. New setting "Videos" (Advanced column;
  per game `videos=`): Auto (the default) turns video decoding off for Killzone and on for every
  other game, On, Off (as before Frontend 59: most games skip their videos). launch.log says
  "Game fix: ... video cutscenes off" when Auto does it.
- Silent Hill Origins showed only the top part of the picture, and flickered between screens:
  the RSX renderer took the game's screen buffer (0404c000) for the lower rows of an old buffer
  just above it in memory (04000000, same width), and showed that buffer's stale copy, which
  only covered 120 rows. A texture now comes from the screen buffer at exactly its address
  when there is one.
- Free memory: the figure counted memory malloc had taken from the PS3 and freed again as used,
  so it only ever went down (Gran Turismo: 4 MB "free" all race long). The "low memory" JIT
  clears kept firing (each one recompiling everything: 15 ms a frame in Gran Turismo), and GE
  dumps were refused. launch.log's figure now includes malloc's free memory, with what the PS3
  still has untaken in brackets.
- If a game stops showing frames, the STALL lines now also say whether the emulated PSP is still
  running (its clock, vblanks, timing events, the event running now), whether the GE thread is
  busy and the emulation thread waiting for it, and a few CPU registers.

# Frontend 60

## What's new in Frontend 60

From the Frontend 59 logs:

- Killzone froze at its memory stick message, and Call of Duty once froze while saving (stuck
  inside the game's save call). The save/load ran on a separate thread, and in both cases that
  thread apparently never finished. On the PS3 it now runs on the emulation thread itself (a short
  pause while the files are written). launch.log now has a line per save/load ("Savedata: ...
  started", "done in N ms"), and if a game stops showing frames, the STALL lines list every PSP
  thread and what it waits for, so a hang that is still there can be found from one log.
- "Not enough free space on the memory stick": the games were told the memory stick is 16 GB. In
  the 32-bit numbers games of this age use, 16 GB minus the saves comes out as zero or negative.
  It's now a 1 GB memory stick (far more than any game's saves need).
- Frame cap now works in every game, whatever way the game paces its frames (Dead to Rights'
  way wasn't covered: it kept running at 60). The choices are now 30 and 60 (`fpscap=30` or
  `fpscap=60`). At 30 no game makes more than 30 frames a second, so games that run at 60 do half
  the work. Speed limit is something else: it keeps the emulated PSP from running faster than a
  real one (leave it on); the menu describes both now.
- Menus fill the whole screen: on a 1080p TV they were drawn at 1280x720 into the top-left corner.
- Call of Duty's intro video: sound but no picture. The video frames went into a screen the GE
  had drawn earlier, and the RSX renderer kept showing its own copy of that screen. It now picks
  up frames the CPU writes there.
- New setting "Right stick" (General column; per game `rstick=`): the PSP has no right stick, and
  some games turn the camera with the face buttons (Call of Duty) or the D-pad. Face buttons:
  pushing the right stick up/down/left/right presses Triangle/X/Square/Circle. D-pad: the
  arrows. Off by default; best set per game.
- Dead to Rights' slow scene (JIT profile, thanks): 11% of the time went into adding draws to the
  GE's command list, about 1,800 calls a frame, each one running the new draw. With the GE
  thread these now run there, and the call doesn't wait for the GE any more.

- The GE (the PSP's graphics chip) now runs on its own thread, in parallel with the game's CPU
  code, as on a real PSP. Until now the emulation thread ran the game's code, then stopped to work
  through each display list (the GE's command lists) before going on; only the final 3D drawing
  was on the second thread, so the display lists' time added to the game code's time. Now the
  emulation thread hands the lists over and goes on, and only waits when the game asks the GE
  something (list done? which command is it at?), at vblank and when it idles. A frame then takes about as long as the
  longer of the two instead of both added up. How much this helps depends on the scene: most
  where the CPU and GE times were close, little where one of them dominates.
- New setting "GE thread" with three choices: "Lists + drawing" (the default, as above),
  "Drawing only" (Frontend 59's behaviour) and "Off". Per game: `geasync=0` for "Drawing only".
  If a game hangs, flickers or misses effects, try "Drawing only" for it first.
- launch.log, with "Lists + drawing": "GE thread per frame: X ms busy ..., emulation thread
  waited Y ms for it (N waits)". X is the GE's own time per frame; Y is the time the game's code
  stood still waiting for it. When Y is close to X, the game waits on the GE right away (little
  overlap); when Y is small, the two run side by side. The "CPU" figure on the perf line is the
  emulation thread's time minus those waits.
- The GE thread does less per draw: the matrix products for each draw are cached (the same
  world/view/projection is used for many draws), and the draw queue no longer reallocates its
  buffers as often.
- PSP CPU code (JIT), all games:
  - Runs of loads and stores through one pointer (structure fields, arrays, copies; the most
    common memory access pattern in game code) compute the pointer's address once; each access
    is then one instruction plus the load or store instead of two, and byte loads and stores
    none.
  - Float compares (c.cond.s, and vcmp lane by lane) are two to three instructions shorter.
- Checked: display lists in parallel give the same draw results as before in the Gran Turismo
  replay (checksum of every draw's vertices and state); the JIT changes run the CPU test suite
  the same as the interpreter. Not yet checked on a real PS3: the two threads on the Cell
  together. If Lists + drawing is slower or misbehaves in a game, please send its launch.log.

## Frontend 59

- Faster system calls, for every game. Each call from the game to the emulated PSP system cost
  about 1 microsecond on the PS3: the JIT saved and reloaded all its registers and switched the
  FPU mode twice (two pipeline stalls). The most frequent ones now take a short direct path:
  lightweight mutex lock/unlock and interrupt suspend/resume (Gran Turismo: about 65,000 a second
  in its menus, ~7,000 a second in races), cache writebacks (Killzone: up to 2,800 a second, Dead
  to Rights: up to 1,000), the system time and the RTC tick (Call of Duty). When one has real work
  to do (a thread waiting on the mutex, an interrupt pending), it takes the normal path as before.
  All other system calls skip a table lookup, and the FPU mode change after each call no longer
  stalls. Expect a few percent in most games, more in Gran Turismo's menus. launch.log's
  "HLE calls" line ends with "JIT fast path: N syscalls per frame".
- Video cutscenes (PMF/MPEG), MP3 and AAC: FFmpeg is now included. Games that showed nothing (or
  froze) at a video should play it.
- New menus: full screen, with an animated background. Settings are two columns (General and
  Advanced, L1/R1 switches between them) with a description of the selected option at the bottom,
  so nothing needs scrolling. The game list shows more games at once, without file extensions.
- In-game buttons: the testing buttons (R3 FPS counter, L2+R2 GE dump, hold L3 for a JIT profile)
  are off unless "Debug buttons" is on in Settings. The quit combo (START+SELECT or L3+R3) now has
  to be held 2 seconds and can be turned off ("Quit combo"); the PS button menu can always quit.
  L3+R3 used to quit at once.
- Main memory: the emulator is now linked without the code the PS3 build never calls (about 3 MB),
  which pays for FFmpeg: the program takes slightly less memory than Frontend 58. The network play
  code's buffers went from 2 MB to 0.25 MB (it isn't used on the PS3).
- Smaller: the JIT compiler reuses its buffers between optimization passes (fewer memory
  allocations when new code appears), and the GE's matrix math avoids a slow library call.

## Frontend 58

- New setting "Frame cap" (`fpscap=30`): a new frame is shown at most every second vblank, so a
  game that runs its logic at 60 frames a second (Dead to Rights, Killzone and Gran Turismo do in
  their heavy scenes) runs it at 30: half the work, and auto frameskip has far fewer frames to
  drop. The game sees each frame take two vblanks, as on a PSP that can't keep up. If a game runs
  in slow motion with it, turn it off for that game.
- Main memory: the record of which code wrote which PSP memory (for PPSSPP's desktop debugger,
  unused here) grew without bound in games that upload many textures or copy a lot (Gran
  Turismo's menus, Call of Duty went down to 0 MB free; Call of Duty then quit to the XMB while
  recording a GE dump). It's off now. GE dumps are skipped (with a message) below 16 MB free.
- Dead to Rights' camera/player stuck at a NaN position (only sky visible): the GE dumps show the
  camera position had become NaN. On the PS3 denormal (tiny) floats were kept where the PSP (and
  PPSSPP on a PC) flush them to zero; a value the game expects to be zero then wasn't, and 1/x or
  inf*0 turned into NaN. The JIT now flushes them like the PSP (AltiVec non-Java mode, FPU NI
  mode with the game's FCR31 FS bit). If it still happens, launch.log says "NaN camera: ..." with
  details.

## Frontend 57

- Freezes after a few minutes of play (Dead to Rights, with 0-1 MB of main memory left in
  launch.log): two things grew all game long. The JIT's store of translated code (IR) doubled
  its size each time it filled up (needing both copies at once), and each of the 64 job slots of
  the GE thread kept the largest buffers it ever had. The IR store is now a fixed 12 MB (when
  it's full the JIT starts over, as when its code space is full), promoted blocks reuse their
  space, the GE slots drop big buffers after each job, and if main memory still gets below 6 MB
  the JIT cache is cleared ("Low memory ..." in launch.log). Keep "PSP memory" at 32 MB: 64 MB
  takes 32 MB more of the PS3's main memory.
- CPU: the code for rarely taken block exits moved out of the blocks' main path (fewer cache
  lines to fetch: the logs show code that runs less often is several times slower per
  instruction, as the PPU fetches it from memory again); stack loads and stores are one
  instruction shorter.
- Frames that auto frameskip doesn't draw (4 of 5 in slow scenes) skip almost all their 3D work
  on the emulation thread.
- New setting "GE timing" (`getiming=100/200/300/400`): the emulated GE takes longer per vertex,
  closer to a real PSP in heavy scenes. In the slow scenes all four games run their own logic at
  60 frames a second (auto frameskip then draws 1 in 5); on a PSP they most likely run at 30
  there. A game that waits for its drawing then runs at 30, which halves the work per second and
  lets more of its frames be drawn. If a game runs in slow motion with it, set it back to 100.
  launch.log now has "the game's own frames: N/s (drawn M/s)" to see the effect.

## Frontend 56

From the Frontend 55 logs (2x resolution, L3 profiles during the slow scenes):

- PSP CPU code (all games): blocks that do float math check the game's rounding mode once at the
  start instead of before every add/multiply (Dead to Rights' explosion/particle code is full of
  them); float loads and stores are one instruction shorter; a matrix loaded or saved by rows
  (lv.q/sv.q R000..R003, Call of Duty's matrix code) is four 16-byte accesses and a transpose in
  registers instead of 16 single-float accesses; "mfc1 + sw" stores the float directly instead of
  moving it through an integer register (a 40-cycle stall on the PS3 each time).
- Call of Duty: draws with texture-matrix UVs (about 150 a frame, all its lit models) are now
  transformed and lit by the RSX like the rest; before, they went through the CPU transform
  (~35 ms a frame in the busy scenes).
- RSX: render targets live in tiled RSX memory (the layout the RSX draws fastest into). Killzone
  at 2x spent 20-35 ms a frame waiting for the RSX. New setting "rsxtile" (Settings: "RSX tiling")
  turns it off if anything looks wrong (scrambled effects or screens).
- launch.log: a "present per frame" line says how long each frame waited for the GE thread and
  for the RSX, so an RSX-bound scene can be told apart from a CPU-bound one.
- Dead to Rights: the dog's NaN angle (the cause of the old freeze) is now replaced by 0 before
  the loop, instead of only ending the loop and letting the NaN reach the dog's heading (falling
  through the world). "Game fix: ..." in launch.log says it's active.

## Frontend 55

Dead to Rights: Reckoning froze for good when the dog was used (both Frontend 53 logs, with and
without vmx=0). The game was stuck in its code that wraps an angle difference into one turn
(`while (!(a <= 2*pi)) a -= 2*pi`): in some dog moves the angle is NaN or infinite, and that
loop never ends. For ULES00195 the frontend now patches the loop's two compares to also accept
NaN (no change for any other value), and if an infinite angle is caught spinning in it between
run slices, it's set to 0. launch.log says "Game fix: Dead to Rights ..." when either happens.
vmx=0 is not needed for this game.

JIT profiles on demand: hold L3 (left stick click) for half a second in a game for a 5 second sample of that
moment (an explosion, a cutscene) into launch.log; the screen says "JIT profile: measuring 5 s",
then "saved". With `jitsample=1` in the game's ini the block counters are compiled in from the
start (a little slower), so the sample starts with no pause; without it, L3 clears the JIT cache
first and the first second or so is recompiling. The PSP has no L3, so the game never sees it. L3+R3 still quits.

The slow-game diagnostics in launch.log also list the FPU registers and the top of the PSP
stack now, so a stuck loop like this one can be traced back to where its values came from.

## Frontend 54

From the Frontend 53 profiles (thanks for the JIT profile logs):
- Vector min/max (vmin/vmax) are now done in AltiVec registers. They used to move both values
  through memory to the FPU and back, three ~40-cycle stalls each. Killzone's busiest code is
  bounding box tests made of these (18 in one block), and Gran Turismo's track code uses them.
- When code uses the lanes of a loaded vector one by one, each lane now goes to its own register
  instead of being read back from memory right after the vector was stored there (another stall).
- vs2i/vi2s (16-bit to 32-bit unpacking, used by Killzone's geometry code) in AltiVec registers too,
  without going through memory.
- The vector instructions the JIT still hands to the interpreter (GT's vscmp, DTR's vsrt1) skip the
  full register save and reload around the call.
- Call of Duty: its small data copies into video memory made the RSX write a frame buffer back
  to memory each time (~20 times a frame, 4-5 ms each): a buffer counted as 512 lines tall instead
  of the 272 it draws, so copies right after it looked like they touched it.
- The JIT profile report and the performance lines are written to launch.log in one go. Each
  line was a separate open/write/sync/close of the log files (~40 ms each on the PS3), so the
  10-second performance summary itself cost ~0.4 s of every 10 s, and the profile report took
  half a minute (the "freezes" right after each JIT profile sample).

## Frontend 53

Call of Duty levels (3 fps in the Frontend 51 log): about half the frame went to running
VFPU (vector) instructions one at a time in the interpreter, ~65,000 a frame. The game leaves a
VFPU "prefix" (a modifier for the next vector instruction) set across a branch once; after that
the JIT stopped assuming prefixes are off at the start of any piece of code, and from then on sent
every vector instruction that might use one to the interpreter. Now only the pieces of code
that such a branch leads to are compiled that way. This affects any game that does this, not just
CoD.

CoD's other half is graphics: it copies image blocks out of its rendered frames ~700 times a frame
and reads back from the RSX for that. A GE frame dump from a CoD level would help with that part.

## Frontend 52

The hangs in the Frontend 51 logs (Killzone, Call of Duty, Dead to Rights) came from the JIT
profile, not from the games: when it wrote its report it asked for the size of each compiled piece
of code assuming they're laid out in order, and since the Frontend 50 warm-up they aren't. A
built-in sanity check then stopped the emulator. That check is gone, and any such check that fails
now writes "ASSERT failed: ..." to launch.log instead of stopping silently.

The JIT profile report now shows the code of the 12 busiest pieces, including the ones that are
busy because of a system call they make (Gran Turismo's and Dead to Rights' biggest ones are GE
calls). With the Frontend 51 samples: in GT races the GE work on the emulation thread is still
the biggest single part; in Dead to Rights and Killzone fights the time is spread over thousands
of small pieces of PSP code, so they need the JIT to be faster all round.

Faster float code (AltiVec JIT, vmx=1): the eight most used FPU registers (f0-f2, f12-f16) now stay
in AltiVec registers between pieces of compiled code, like the integer registers already did.
Before, every piece wrote all float registers it changed back to memory at its end and the next one
read them back right away, which stalls the Cell each time. Dead to Rights' busiest float code is
about 30% shorter.

## Frontend 51

The long freezes in Frontend 50 (Gran Turismo menus, Killzone cutscenes; 2-10 seconds of compiling
in the logs) had one cause: while compiling, the JIT looked up which compiled piece of code an
address belongs to by going through all of them, every time. The longer a game ran, the more
pieces there were (tens of thousands), and Frontend 50's warm-up compiles each hot piece a second
time, so the lookups doubled. Now that is a single table lookup. Compiling should take a small
fraction of what the Frontend 50 logs show.

Dead to Rights getting stuck (also in earlier versions, always at the same place): the game sits in
a loop that subtracts 2*pi from an angle until it's small, and the angle is NaN, so it never ends.
A likely reason is that the AltiVec JIT treats very small floats as zero where the PSP doesn't. To
check: put a file games/<your DTR image name>.ini with the line

    vmx=0

(see games/README.txt), play until it would usually get stuck, and send launch.log. vmx=0 is a bit
slower but does floats exactly like the PSP.

## Frontend 50

Gran Turismo races:
- The shiny car paint (environment mapping, ~590 draws a frame) is now done by the RSX vertex program.
  Before, those draws went through a CPU path that cost about 40 ms of every race frame.
- Draws that only add triangles to the previous one (~50,000 a frame in a race) skip the
  per-draw setup. They are copied straight into the batch that's already open, about three times
  less work for each.
- The second GE thread copies vertices for the RSX a whole cache line at a time without reading the
  destination first. That copy took 7-11 ms of that thread's time per frame in GT and DTR fights,
  and the thread shares the PPU core with the emulation.

Hitches when a new scene or new enemies appear (Killzone's intro, Dead to Rights fights):
- New PSP code first runs in the interpreter and is compiled only once it has run 20 times. Code
  that runs a few times (level setup, scripted moments) is never compiled, and that compiling caused
  the long pauses (Killzone's intro compiled ~10,000 pieces of code in a few seconds). Setting:
  "JIT warm-up" (on); off compiles everything right away like Frontend 49.
- Code that is still interpreted skips the optimization passes; they are done when it's compiled.
- Writing registers back at the end of a compiled piece looks only at the registers in use.
- Gran Turismo's svl.q/svr.q, sin/cos and the color packing instruction are now compiled; they used
  to go through the interpreter one instruction at a time.

Please help with the next step: in the settings menu turn on "JIT profile" and play a Gran Turismo
race, a Dead to Rights fight and a Killzone fight, each for 3-4 minutes. It now takes a 10 second
sample every minute (each with a short pause) and writes the busiest code to launch.log. That shows
where the PSP CPU time goes in those scenes, which is now most of the frame time in all three games.

## Frontend 49

CPU (the Frontend 47 logs: in Dead to Rights and Killzone fights the PSP code takes 50-90 ms of
every frame, far more than drawing):
- More PSP registers stay in PS3 registers between pieces of compiled code: t0-t3, s2 and s3 too
  (16 instead of 10). Handing a register over through memory right before it's read again costs the
  Cell ~40 cycles each time; Killzone's busiest loop did that several times per pass.
- A branch out of the middle of a compiled piece no longer writes all registers back on the path
  that stays (they were written and read straight back).
- Function returns usually go straight to the code they return to, instead of through the lookup
  that every return took (with a branch the Cell can't predict). A return that keeps going to
  different places falls back to the lookup.
- Float constants come from a constant table (or two instructions for small whole numbers) instead
  of through memory written just before; the float-load/store byte-swap patterns stay in registers.
- Entering a compiled piece no longer takes a branch every time, and the rarely used exact-rounding
  float paths moved out of the way.

Other:
- The JIT profile (with its pause about 40 s into a game, and slower code while it measures) is off
  by default now; it's still in the settings menu. An old settings.ini with it on doesn't turn it
  back on.
- When a game runs at about a frame a second or less (Dead to Rights after a while in the
  Frontend 47 log), launch.log gets the PSP CPU registers, the code around where it is, and the GE
  display lists, a few times per game. That tells a hang from a very slow scene.

## Frontend 48

Settings menu (no more editing settings.ini): the diagnostics switches are now in the settings list, which
scrolls. Each says which frontend version introduced the behaviour, so a change that broke something can be
turned off and checked:
- GE thread, JIT floats (AltiVec/FPU registers), SPU vertices, SPU textures (F43), Vertex data in main memory
  (F43) or RSX memory, Texture check (only after memory writes, F42, or every use), Write-back (drawn rows only,
  F41, or all rows), Screen copies (reused, F40, or fresh for every draw), Draw merging (F42), JIT profile.
- The last item resets those options to the defaults.
- launch.log lists all settings at the start of every game.
- Killzone's flickering textures: please try, one at a time, Texture check "Every use", Write-back "All rows",
  Screen copies "Fresh", Draw merging off, SPU textures off, Vertex data "RSX memory", and tell me which one (if
  any) makes the textures right again. Send logs/UCES00279/ from that run.

Gran Turismo (each race frame is ~15,000 separate draws, each handled on the emulation thread):
- Draws that follow each other with no state change in between are now joined without comparing the whole
  draw state again. Gran Turismo sends the same matrices again before every draw and changes the texture LOD
  bias (which the RSX renderer doesn't use) every time: neither counts as a change any more. In the race dump
  about 13,500 of 14,400 draws are joined this way.
- A change of only the LOD bias no longer recomputes the whole rasterizer state.
- Fixed random leftover bytes in the texture-sampler state that made identical states look different: draws
  that should have been joined weren't. Gran Turismo's race frame now goes to the RSX as ~600 draws instead of
  ~1,200; it helps the other games' merging too.
- Together: about a third less emulation-thread GE work per draw in Gran Turismo than Frontend 47 on the PC
  test build, on top of the halved RSX draw count. launch.log's Perf section has a new line, "T&L draw merging
  per frame", with how many draws were joined that way and why others weren't.

## Frontend 47

Speed, from the Frontend 45 CPU profile:
- Floating-point math in the PSP code (enemies, explosions, fire, animation) was taking a very slow path: once a
  game had written the FPU control register even once (nearly every game does, at startup), every float add,
  subtract, multiply and int-to-float conversion moved its values through memory twice, about 160 PS3 cycles
  each instead of a few. They now take the fast path whenever the game rounds to nearest, which is what games
  do (the exact slow path is still used if a game really switches rounding mode).
- Float compares and a few other JIT paths use a faster instruction for reading the condition register.
- Queueing a 3D draw on the emulation thread: no zero-filling of buffers that are overwritten right after, a
  faster strip/fan conversion, and the draw-merging check tests the texture first. About 30% less work there in
  Dead to Rights, less in Gran Turismo.

Fixes:
- Render-target write-back: after a write-back that had to keep rows the game had changed, the RSX copy is
  re-synced right away. Drawing on into the same buffer could otherwise write stale rows over textures the game
  keeps right after a render target (a possible cause of Killzone's broken textures).
- If the RSX stops responding (Dead to Rights froze once at startup in Frontend 45), launch.log now shows the
  RSX commands where it stopped.

## Frontend 46

- Logs per game: USRDIR/logs/<game file name>/ gets launch.log (written while the game runs), and when the game
  ends core.log, stdout.log, settings.ini and any GE dumps taken during that run. Each run of a game replaces the
  files in its folder. USRDIR/launch.log and core.log are still written as before.
- New defaults: RSX hardware renderer and CPU JIT on. An existing settings.ini keeps its values.
- The XMB icon shows the right version (it said "Frontend 25" since Frontend 25).
- Everything from Frontend 45 (the CPU profile is still on; please send logs).

## Frontend 45

CPU (the PSP code runs at roughly 85 PS3 cycles per PSP instruction in the Frontend 44 logs, several times
more than it should):
- The JIT writes its code straight into its code area when the PS3 allows it, instead of one PS3MAPI system call
  per piece of code (~33 us each; about 1-2 ms per frame and stutter when new code appears). Tested once at startup
  on a separate thread. If the test ever takes the app down, the next start skips it and uses PS3MAPI (launch.log
  says which one is used, in the "JIT:" line).
- Jumps through a register (every function return) no longer store the PSP address and read it straight back
  (a ~40-cycle stall on the Cell each time).
- The JIT profile now measures time per block, not just how often it runs, and writes the PSP code, the JIT's
  intermediate code and the generated PS3 code of the busiest blocks to launch.log. That shows where the time
  really goes, for the next round of JIT work. Please send logs again (a minute or more in game).

## Frontend 44

Fixes for Frontend 43:
- Games stopped about 40 seconds in ("Game stopped. Circle: exit"): starting the JIT profile ended the emulator's
  run loop. It now clears the JIT cache directly.
- The system froze after a game ended: the SPU threads were never stopped. The PS3 needs them joined before the
  app exits. They are now stopped after every game, and started again by the next one.
- Vertex preparation on 3 SPUs failed on its first job (and fell back to the PPU): the SPU startup code passes
  only three arguments to an SPU program, and each SPU's number was in the fourth.

## Frontend 43

Speed (the SPU work you asked for, and what the Frontend 42 logs showed):
- Vertex preparation for 3D draws now runs on 3 SPUs instead of 1.
- Texture checking and decoding run on 2 SPUs: big textures are hashed there, and the common formats (16/32-bit,
  4/8/16/32-bit palette, swizzled or not) are decoded straight into RSX memory. Small textures and the rare formats
  stay on the PPU. With the sound mixer that uses all 6 SPUs a game can have.
- The renderer asked the system for the time several times per draw (a system call each time). It now reads the
  CPU's own timer. This mattered most in Gran Turismo's races (thousands of draws a frame).
- Vertex and index data for the RSX now go to main memory instead of RSX memory: the PPU writes RSX memory very
  slowly, and copying vertices was a large part of each draw's cost. settings.ini "vertexring=0" switches back.
- Block transfers (Gran Turismo's menus do thousands a frame) no longer make the emulation thread wait for the GE
  worker, unless they overwrite a texture that queued drawing still needs.
- Not done yet: building the RSX commands on SPUs (still on the GE worker thread; the slow parts of it are the
  fixes above) and audio decoding on an SPU (the ATRAC3+ decoder's tables don't fit an SPU's 256 KB, and decoding
  costs 2-3 ms a frame).

New in launch.log (please send logs again):
- "waiting to keep real-time speed": time the emulator sleeps to hold 100% speed. In the Frontend 42 logs
  Killzone ran at ~100% speed with 12 fps, which would mean the game itself makes only 12 frames per emulated
  second. This number tells whether that's so.
- "JIT profile": about 40 seconds into a game, the PSP code that takes the most time, measured for 10 seconds.
  Expect a short pause when it starts and ends. settings.ini "jitprofile=0" turns it off.
- The JIT line counts code compiled again at an address it had compiled before (Dead to Rights compiles
  12-15 ms of code per frame).
- Texture lookups, SPU texture decodes and the vertex memory location are in the RSX line.

Settings: "sputexture=0" turns the SPU texture work off.

Known: Killzone's white textures are white in your dump even with the software renderer, so the texture data was
already wrong in PSP memory when the frame was drawn (not an RSX renderer problem). Still investigating.

## Frontend 42

Graphics:
- Clears with dithering on (Gran Turismo, Dead or Alive Paradise and others dither almost everything) did nothing on
  the RSX renderer: they were drawn as normal rectangles whose stencil test could never pass. So the depth buffer
  was never cleared - Gran Turismo's sky then failed the depth test (black sky), and characters fought with the
  previous frame's depth (likely Dead or Alive Paradise's "lattice").

Speed (from the Frontend 41 logs and your dumps):
- Draws with the same state are now merged into one job for the GE worker and one RSX draw: on your dumps Dead to
  Rights goes from ~300 to ~100, Dead or Alive Paradise from ~1,030 to ~300 per frame.
- The skip-unchanged-state check failed for almost every draw in Dead to Rights (526 hits, 33,408 misses): the game
  appends each draw with its own GE call, and each call counted as "the game may have changed textures". Textures
  are now rechecked only where the game wrote back its cache or copied memory (by address range), which also cuts
  the texture hashing time (605 ms per log interval in Dead to Rights).
- Flushes the software renderer needs but the RSX doesn't (one or more per draw in Gran Turismo) are gone.
- launch.log has a new line splitting the GE time on the emulation thread (draw commands, flushes, syncs, block
  transfers), and the 3D line counts merged draws.

## Frontend 41

- Gran Turismo black sky: the sky's textures sit in video memory right after a small render target the game uses.
  When the RSX renderer wrote that render target back to PSP memory it wrote all 272 rows, including rows it had
  never drawn, which replaced the sky textures. Only rows the RSX actually drew into are written back now (this
  can also fix other games' textures turning black or into garbage after effects).

## Frontend 40

From the Frontend 39 logs and dumps:
- Gran Turismo races (1 fps): almost all the time went into setting up the RSX state for each of ~14,000 draws a
  frame, because the check that skips unchanged state never matched for draws coming from the GE worker thread
  (each carries its own copy of the palette, and GT changes the mipmap LOD bias, which the RSX renderer doesn't
  use, between otherwise identical draws). On the GT race dumps it now matches for 95% of draws instead of 22%.
  launch.log's RSX line now also shows the state time split (target / apply) and the hit counts.
- Killzone and Dead or Alive Paradise: draws that sample the screen they draw to (Killzone: ~150 a frame in menus;
  DOA: the outline pass) copied the whole screen, with two GPU pipeline flushes, for each draw. The copy is now
  reused while only such draws have drawn since.
- Killzone: framebuffers stop at the depth buffer or the next buffer that still leaves a full screen (Frontend 38's
  version gave up when another buffer started inside the screen).
- Hardware T&L: when a game turns the GE's depth clipping off, depth is clamped instead of geometry being cut at
  the near/far planes.

## Frontend 39

Speed (measured on GE dumps from your games, on a PC):
- Dead or Alive Paradise: the game draws every character a second time as lines (about 10,000 a frame). Each line
  was a separate job for the GE worker thread, copying the whole draw state, and was skinned on the CPU. The lines of
  one draw are now one job, and line draws use the RSX transform & lighting path like triangles. The GE side of a
  DOA scene went from 22.7 ms to 5.7 ms on the PC test (the PS3 should gain more: its memory copies are slower).
- The same batching applies to every draw that goes through the per-primitive path (sprites, text, menus).
- The emulation thread and the GE worker only signal each other when the other one is actually waiting (each
  signal is a system call on the PS3, and there are thousands of draws per frame).

## Frontend 38

- System dialog text (save/load prompts, "Data saving in progress"): really fixed this time. The font files were read
  with the wrong byte order on the PS3 (it is big-endian), so the font never loaded. Dialog icons and buttons now use
  the font atlas the same way as the text.
- Dead or Alive Paradise "lattice" on the characters: the game draws every character again as lines textured from
  the screen, which stay invisible because the screen's alpha (the PSP's stencil) is 0. The RSX renderer wrote alpha
  on every draw, so the lines showed up. Alpha is now only written when the game uses the stencil, like the PSP.
- Killzone menu text as blocks / flickering textures: a framebuffer whose scissor reached row 511 was treated as 512
  rows tall and written back over the depth buffer and the textures behind it. Framebuffers now stop at the depth
  buffer or the next framebuffer.
- Gran Turismo garbled menu text (repeated wrong letters): the game rewrites the same small glyph buffer between draws;
  the RSX renderer reused its first copy for the whole frame. Small textures are now re-checked whenever the game
  could have changed them, bigger ones get a quick spot check between full checks.
- Per-game .ini files: settings.ini is only rewritten when a value changed (it used to be rewritten after every game),
  per-game files are found regardless of letter case, and launch.log lists the .ini files it found in games/.

## Frontend 37

Speed, for games with many small 3D draws (Gran Turismo's races: ~7,600 a frame, ~33 us each on the GE worker):
- The vertex arrays for hardware T&L draws are bound once instead of for every draw (each draw's indices are offset
  into one shared layout), and back-face culling stays set between draws instead of being switched on and off.
- Only the vertex-program constants that changed are uploaded (usually 4 of 43 registers).
- JIT: nearby code commits are merged into one PS3MAPI call more often (each call costs ~33 us).

## Frontend 36

- Text in system dialogs (save/load prompts, profile questions): the dialog font was never loaded on the PS3, so only
  the boxes and lines were drawn.
- Killzone (and likely other games) white or missing textures: writing a framebuffer back to PSP memory also wrote
  rows the game never drew, wiping textures the game had put in video memory right after the framebuffer. Rows the
  CPU changed are now left alone.
- Gran Turismo: vector store pairs (svl.q/svr.q) compiled instead of interpreted (about 60,000 a second).
- GE dumps (L2+R2) are now in the standard format, readable by desktop PPSSPP too.
- launch.log: the RSX line now breaks hardware T&L draw time into state, constants, vertex copy and submit.

## Frontend 35

- Fixed Frontend 33/34 hanging at boot in Gran Turismo, Dead or Alive Paradise and Dead to Rights: the JIT mishandled
  vi2uc (colour packing) when the result went into the source's own register group.
- System dialogs (save prompts) show their text: the RSX renderer can now read the dialog font, which used to come out
  as white boxes.
- Module loading no longer slows down with every module the game has loaded (4 s for one load in Killzone, likely the
  25 s Gran Turismo boot): only the new module's functions are hashed.
- Diagnostics: core.log is flushed every second, and if no frame comes for 5 s launch.log says what the emulator is
  doing ("STALL: ...").

## Frontend 34

- 3D vertex decoding and skinning (direct T&L draws) moved from the GE worker thread to an SPU, prepared ahead while
  earlier draws are being sent to the RSX. settings.ini `spuvertex=1` (default); `spuvertex=0` keeps it on the PPU.
  launch.log's "GE worker thread" line says where it ran and how many draws per frame the SPU prepared.

## Frontend 33

- Dead or Alive Paradise: draws queued to the GE worker no longer redo the whole RSX state setup when nothing changed (was ~14 us per triangle, 335 ms per frame in game).
- Killzone menu: fixed the diagonal half of the screen showing an old image (copy of the screen reused too long in 32).
- JIT: multiply-accumulate (madd/msub), ll/sc, and VFPU saturate, float-to-int and colour pack/unpack instructions compiled instead of interpreted (Gran Turismo, DOA, Killzone).
- L2+R2 records a GE dump of the next frame to memstick/PSP/SYSTEM/DUMP (send it for graphics bugs).
- launch.log splits JIT compile time (MIPS to IR, IR to PPC); core.log times each step of a slow module load (Gran Turismo).

## Frontend 32

- CPU JIT keeps PSP float and vector (VFPU) maths in AltiVec registers. settings.ini `vmx=1` (default); `vmx=0` goes back to the FPU path. launch.log says which one ran.
- Bit-reverse instruction compiled by the JIT instead of interpreted (Dead or Alive Paradise).
- A draw that textures from the screen it draws to reuses one copy until the game flushes textures or changes target, instead of a full-screen copy and GPU wait per draw (Dead or Alive Paradise in-game slideshow).
- Fonts fixed (assets folder was not registered).
- A per-game .ini no longer changes settings.ini or other games.

## Frontend 31
- GE worker thread, take two. In Frontend 30 the emulation thread waited for the worker almost all the time: the checks the
  software GPU makes before nearly every textured draw ("is this texture still being drawn to?") waited for the worker to
  finish, and so did every 2D draw and clear. Now every kind of draw is queued, those checks are answered from a record of the
  buffers queued drawing writes to (without waiting), and the CLUT (palette) is copied with each draw. The emulation thread
  waits only when pixels have to be written back to PSP memory, and at the GE sync points.
- Fixed a race in Frontend 30's worker: right after a draw was queued, the emulation thread could think the worker idle and use
  the RSX renderer at the same time as it. Likely the cause of Killzone freezing.
- launch.log counts the waits ("waits for all" / "for a free slot"), and says if the worker is still busy after 2 seconds.

## Frontend 30
- 3D drawing runs on its own thread (the "GE worker"), in parallel with the PSP CPU emulation. The emulation thread still
  reads the PSP's display lists, but for each 3D draw it only copies the vertices it uses, their indices and the GE state
  into a queue and moves on; the worker decodes the vertices and draws them on the RSX. The emulation thread waits for
  the worker only when it must: before any other drawing or framebuffer access, and at the PSP's GE sync points
  (sceGeDrawSync, sceGeListSync, list interrupts), after which a game may reuse the memory the drawing reads.
  Checked with 3000 random draws (777,000 vertices) through the worker, with the source memory overwritten right after
  each draw was queued.
- launch.log shows how long the worker draws per frame and how long the emulation thread waited for it.
- New setting gethread (settings.ini or a per-game ini): 1 (default) uses the worker, 0 draws on the emulation thread as
  before. Only used with fast3dmode=2. If a game shows wrong or flickering graphics with Frontend 30, try gethread=0.

## Frontend 29
- JIT: the ten most used MIPS registers (sp, ra, v0, v1, a0-a3, s0, s1) now stay in PPU registers from one compiled block
  to the next. Before, every block stored them to memory and the next block loaded them straight back, which stalls the PPU
  (load-hit-store) on each one. This is the PSP CPU time that dominates both Dead to Rights and Killzone.
- JIT: a newly compiled block and the links to and from it are written into the code area with one PS3MAPI call instead of
  one call per link. launch.log shows how many calls and how much time code writing takes.
- Both checked with all 525 pspautotests through the JIT against the IR interpreter.

## Frontend 28
- Faster 3D vertices on the direct RSX path. Frontend 27's direct reader still converted every little-endian float and
  every 8/16-bit value through memory one at a time, which stalls the PS3's PPU (a load-hit-store, about 40 cycles each).
  The vertices of a draw are now read in three passes: raw values on the integer unit, conversion to floats with
  AltiVec, then skinning on the FPU. Checked against PPSSPP's vertex decoder with 195,000 random vertices in random formats.
- The direct path no longer clears a table the size of the draw's whole index range for every draw.
- Morphed models (vertex morph frames) and continued primitives now also take the direct path.
- JIT: vcmp with a prefix that names a lane past the vector size is compiled instead of interpreted (Killzone ran it in
  the interpreter hundreds of thousands of times per minute).
- launch.log says why 3D draws didn't take the direct path (lines/points/rectangles, generated UVs or powered diffuse,
  32-bit indices, other).

## Frontend 27
- 3D draws that go through RSX transform & lighting are now read straight from the PSP's vertex data into the RSX vertex buffer.
  PPSSPP's software vertex decoder and vertex reader (a chain of small function calls per vertex component on the PS3, which
  has no vertex decoder JIT) are skipped entirely. This covers skinned models (bone weights) too. It is checked against the old
  path with 98,000 random vertices in random formats. launch.log shows how many draws per frame take this path.

## Frontend 26
- Sound mixing (sceSas voices: VAG/PCM decoding, pitch, envelopes, volumes) runs on an SPU. Before, it ran on the emulation thread
  itself, because the SDK reports one CPU core. The mixer is the same code on the SPU and the PPU, and a fuzz test shows it gives
  bit-identical results to PPSSPP's mixer. If the SPU can't be started or stops answering, mixing falls back to the PPU, and
  launch.log says why.
- The "invalid volume" sceSas warning (thousands per minute in Dead to Rights) is no longer logged.
- launch.log has more timing: sound mixing and audio decoding per frame, JIT compile time and block invalidations, and host time
  per HLE function (the six most expensive). When the game runs below 3 fps, it also lists each PSP thread and what it waits on,
  to diagnose hangs.

## What's new in Frontend 25
- Hardware T&L self-test also runs three cut-down vertex programs (position via a temp register, position written per component, T&L without the light loop), to narrow down why the full program draws nothing on the console. The T&L program now writes the position through a temp register.

## What's new in Frontend 24
- Hardware T&L startup check runs in stages and logs each (main program on the T&L vertex layout, a pass-through program, T&L unlit, T&L lit), to find why it fails on the console.

## What's new in Frontend 23
- JIT: svl.q/svr.q pairs (unaligned VFPU vector stores, Dead to Rights' most frequent interpreter fallback) are compiled as one vector store.
- Hardware T&L is still off (its startup check fails on the console); being investigated.

## Frontend 22
- Hardware T&L fixed: Frontend 21's startup check failed on the console (the test quad came out
  black), so it always fell back to the CPU transform. The vertex program computed a light's spot
  factor as 0 to the power 0 for lights that aren't spot lights, which the RSX evaluates as NaN.
  The program now never does that, and the offline checker (PS3/rsx/check-tl.py) evaluates powers
  the way the hardware does, so it would have caught it.
- Per-game settings: USRDIR/games/<image name>.ini overrides settings.ini for that game (see
  games/README.txt).
- launch.log names the MIPS instructions the JIT hands to the interpreter ("interpreted MIPS
  instructions").

## Frontend 21
- Hardware transform & lighting: 3D draws are transformed, lit (up to 4 lights: directional,
  point and spot, ambient/diffuse/specular, material colors from the vertex), fogged and culled
  by an RSX vertex program. The PS3's CPU only decodes the vertices and copies them out.
  "Fast 3D path" in Settings now has three choices: RSX transform & lighting (default), CPU
  transform (Frontend 20's path) and off. At startup the renderer checks the vertex program on
  the RSX (a lit test quad and the culling direction); if that fails, launch.log says so and the
  CPU transform is used. launch.log's RSX line counts "hardware T&L" draws.
  Not done on the RSX yet (those draws keep the CPU transform): environment mapping and texture
  matrix UVs, and "powered diffuse" lights. Specular highlights can be a little brighter than on
  a PSP, whose specular power is an approximation.
- The setting's settings.ini key is now fast3dmode, so the new default applies once.

## Frontend 20
- Indexed draws that use a few vertices spread over a large vertex buffer now decode and
  transform only the vertices they use, and every batched draw uploads only the vertices its
  remaining triangles reference.
- JIT: VFPU 16-bit unpacks (Killzone's most frequent fallback), sign and arcsine are compiled.

## Frontend 19
- Left analog stick: up and down were swapped.
- launch.log splits vertex work further: vertex decoding and the batched transform/lighting.

## Frontend 18
- Palette lookups from the framebuffer on the RSX: a game that reads its rendered picture as a
  CLUT32 texture (Killzone's color effects) was written back to PSP memory and decoded on the CPU
  ~270 times a frame (seconds per frame). A small RSX shader now does the lookup straight from
  the framebuffer into a separate texture, and reuses the result while the source hasn't changed.
  launch.log counts these as "palette lookups".
- JIT: VFPU min/max (with the VFPU's NaN rule), conditional VFPU moves, vector constants and
  VFPU control register writes are compiled natively.

## Frontend 17
- Batched 3D draws are sent to the RSX indexed: each vertex of a mesh is converted and uploaded
  once (meshes share most vertices between triangles), with culling decided per triangle from
  per-vertex screen positions computed once.
- Textures that haven't changed for 8 checks are rehashed every 4th frame instead of every frame
  (texture hashing was ~8 ms per frame in Dead to Rights).
- JIT: VFPU compares and the condition-code any/all bits, VFPU control register reads, and the
  VFPU sin/cos/reciprocal/rsqrt/sqrt helpers (called directly) no longer go through the
  interpreter fallback, which flushed every register each time.

## Frontend 16
- Fast 3D path, now for real: indexed meshes (most 3D models) were still transformed a second
  time by the old exact per-vertex code before the batched path ran, so Frontend 15 saved nothing
  on them. Lighting (up to 4 lights), environment mapping and texture-matrix coordinates are now
  computed in plain float math inside the batched path, and skinning/morphing use the vertex
  decoder's float result instead of the GE-exact recomputation. Colors can differ from the exact
  software path by a few levels.
- JIT: floating-point compares, int/float conversions, truncate/round/ceil/floor, the rounding
  mode switches and integer division are compiled natively instead of calling the interpreter
  (they were the most frequent interpreter fallbacks in Dead to Rights and Killzone).
- Hardware renderer: a texture that starts inside a framebuffer (post effects sampling part of
  the screen) is copied on the RSX instead of writing the framebuffer back to PSP memory and
  decoding it on the CPU, which cost Killzone seconds per frame. launch.log names any remaining
  framebuffer/texture overlap that still needs a write-back.

## Frontend 15
- "Fast 3D path" setting (on by default): batched triangle draws straight to the RSX.

## Frontend 14
- PS button in game: as soon as the XMB menu opens, every emulator thread drops to the lowest
  priority (restored when the menu closes). At a few FPS the emulator kept both PPU hardware
  threads busy, so the XMB's own loading never got CPU time - the endless spinner. The automatic
  quit now waits 30 s instead of 10.

## Frontend 13
- Always-on timing: the FPS overlay and launch.log split each frame into PSP CPU time and GE time
  (display lists, of which vertex transform + drawing), so we know what to speed up next.
- JIT: the perf log lists which instructions still run in the interpreter; square root is native.
- VRAM sync defaults to Minimal ("Every frame" cost Dead to Rights up to 7 s out of every 10 s).

## Frontend 12
- RSX renderer, render to texture: when a game draws into a framebuffer and then uses it as a
  texture (menus, blur/bloom effects - Killzone does this constantly), the RSX now samples its own
  copy directly. Before, every such draw wrote the frame back to PSP memory, waited for the RSX,
  and re-decoded it as a texture on the CPU (Killzone's menu: ~900 write-backs per 10 s, 2% speed).
- Texture decoding: the common PSP formats (16/32-bit, 4/8-bit palettes, swizzled or not) are
  decoded by dedicated code instead of one software-sampler call per texel.
- Framebuffers are hashed (to detect CPU changes) at most once per frame.
- Perf lines in launch.log now show where renderer time goes (texture decode, hashing,
  write-back, upload).
- Settings -> "PSP memory": 32 MB (PSP-1000) by default, 64 MB optional. Leaves ~32 MB more free
  on the PS3 - the in-game XMB may need it (the log now records free main and RSX memory when the
  PS button is pressed).
- Select in the game list runs only the JIT check now; the RSX tests are in RSX Diagnostics.

## Frontend 11: sound
- PSP audio now plays through the PS3 (Settings -> Sound, on by default). The PSP's 44.1 kHz
  output is converted to the PS3's 48 kHz. While the game runs below full speed the emulator can't
  produce sound fast enough, so it will stutter; it gets better as speed improves.
- JIT test results: the full pspautotests suite (525 tests) gives identical output with the JIT
  and the interpreter (the only 2 differences come from save files the first run left behind).

## Frontend 10: JIT (dynarec), experimental
- Settings (Square) -> "CPU core": "JIT (EXPERIMENTAL, needs PS3MAPI CFW)". PSP code is compiled
  to native PowerPC code instead of being interpreted. Needs Cobra/webMAN CFW with PS3MAPI (the
  JIT probe in Diagnostics tells you); without it the IR interpreter is used automatically.
- Natively compiled: integer ALU, shifts, compares, multiply, loads/stores, branches/exits,
  syscalls, FPU add/sub/mul/div/neg/abs/moves, float and VFPU vector loads/stores, VFPU
  add/sub/mul/div/scale/dot/moves. Everything else runs in the interpreter, one instruction at a
  time, with identical results.
- Tested off-console: the PPC JIT was built for 64-bit PowerPC Linux (same calling convention as
  the PS3) and run under an emulator on the pspautotests suite; it matches the interpreter on all
  CPU/FPU/VFPU tests (except flush-to-zero of denormals in the rarely used FS mode) and on the
  rest of the suite run so far.
- If a game crashes or misbehaves with the JIT, switch back to "IR interpreter" and send
  launch.log.

## Frontend 09
- PS button: if the XMB menu hasn't appeared 10 seconds after pressing PS (the endless spinner),
  the app now quits to the XMB by itself instead of leaving the console stuck. Every system event
  is logged with free memory, to find out why the in-game XMB doesn't load.
- Hold START+SELECT for 2 seconds (or press L3+R3) to stop the game and return to the XMB.
- Fixed: RSX renderer at 3x resolution would hang the RSX (its scaled copy only accepts sources
  up to 1024 pixels); large copies are now split into tiles.
- Diagnostics show a progress screen and can be cancelled with Circle.
- Self-test: main-memory render target check moved last, plus a draw check after it.

## Frontend 08

- Fixed: the PS button (XMB in-game menu) spun forever and the console had to be switched off.
  System events were only answered once per emulated frame (every 300-600 ms at the current
  speed). A dedicated thread now answers them every 16 ms; while the XMB menu is open the
  emulation pauses and the picture stays up. "Quit Game" from the XMB always exits: if the core
  hasn't stopped after 5 seconds the process ends directly.
- Fixed: RSX self-test readbacks were all zero and the renderer's startup calibration failed.
  RSX label memory starts with leftover values (Frontend 07 saw 322420947), so every fence looked
  complete immediately. Labels are now zeroed before first use.
- VRAM test: adds a full sweep of every free byte of RSX memory - CPU-written address patterns and
  RSX-written fills, all read back by DMA - reporting bad offsets and failing bit positions.
- New diagnostics: RSX stress test (60 s of a blended, depth-tested scene; every frame must match
  the first bit-for-bit; temperatures before/after) and a video output report (HDMI / AV multi
  port, display-reported modes, EDID colour data, which resolutions the system accepts, current
  mode, measured vblank rate). Reports: rsx_stresstest.txt, video_report.txt.
- Settings: "CPU/GE timing" (geprofile) toggle, splits the launch.log perf lines into CPU time and
  GE display-list time.

## Frontend 07 (fixes the Frontend 06 RSX freeze)

- Fixed: the RSX self-test and the RSX renderer froze the console. The cause was PSL1GHT's
  rsxSetTransferData (RSX DMA copy): it compares an unsigned pitch with a negative limit, always
  takes its row-by-row path, and that path counts an unsigned variable down with ">= 0", so it
  never ends and fills the command buffer forever. The port now issues the DMA copy commands itself.
- Diagnostics: the RSX self-test first sends each kind of RSX command on its own and waits for the
  RSX to finish it (S01..S22), logging the step before sending it. A watchdog thread reports any
  step stuck for over 3 seconds, with the RSX command pointers, and RSX exceptions are logged.
- JIT probe results from Frontend 06: both PS3MAPI routes work on 4.93 CFW (PS3MAPI core 1.24).

# PPSSPP PS3 Frontend 06 (previous)

## What's new in Frontend 06 (experimental RSX hardware renderer + RSX diagnostics)

### RSX hardware renderer (EXPERIMENTAL, off by default)
Settings (Square) -> Renderer: "RSX hardware". Also: Resolution 1x/2x/3x and VRAM sync.
- The software GPU still decodes display lists, transforms, lights and clips (that part is
  proven on this port); every resulting triangle/sprite/line/point is then drawn by the RSX into
  its own copy of the PSP framebuffer - at 1x, 2x or 3x resolution - and the RSX scales the
  result to the TV. This replaces the software rasterizer, which was the expensive part.
- Textures are decoded once with the software renderer's own sampler (all PSP formats, swizzle,
  CLUT, DXT) and cached in RSX memory until their data changes.
- PSP VRAM stays authoritative: framebuffers the CPU changed are uploaded to the RSX, and RSX
  output is written back to VRAM when something reads it (block transfers, render-to-texture,
  CLUT loads) and, with "VRAM sync: Every frame" (default), once per frame. "Minimal" is faster
  but games that read their framebuffer with the CPU may show stale images.
- When a game starts, the renderer measures screen orientation and color/texture byte order on
  the RSX (launch.log: "RSX renderer calibration ...") and adapts to what it finds.
- Known gaps: stencil is kept in the RSX depth buffer, not framebuffer alpha; doubled blend factors,
  ABSDIFF blending, color test, dithering and depth range clamping are approximated or ignored;
  lines are 1 pixel wide at any resolution; mipmaps use level 0 only.
- If a game looks wrong or crashes with RSX, switch Renderer back to Software. Please send
  launch.log (contains "Perf: RSX ..." lines every 10 s) and core.log.

### JIT probe (Diagnostics, third item)
Checks whether this console can run generated CPU code through PS3MAPI (Cobra/webMAN CFW), the
route used by crystalct's PS3 MAPI Dynarec Lib. Route A writes code into the app's own code
segment; route B allocates an executable page. Each writes small functions, runs them and checks
their results, and the probe measures how fast code can be written. Saved to USRDIR/jit_probe.txt.
Each step is logged to launch.log before it runs, so if the app stops, the last line says where.
This is groundwork for a future PowerPC JIT (the CPU still uses the interpreter).

### RSX diagnostics

Install over Frontend 05/04 (same title ID PPSSPP001). Everything from Frontend 05 is kept.

New: press SELECT in the game list for Diagnostics.
- RSX self-test: checks every RSX feature the upcoming hardware renderer relies on, by drawing
  into a small offscreen target and reading the pixels back: fences, clears, render target in
  main memory, pixel coordinate mapping, vertex color byte order, texture layout/orientation,
  projective texture coordinates, alpha blending, blend constant color, depth test, alpha test,
  stencil, the shader combiners (specular, color doubling, fog, texture BLEND), scaled image
  transfers and triangle throughput.
- VRAM test: largest free RSX memory block, CPU write/read speed to RSX memory, a 32 MB pattern
  test verified through RSX DMA, main<->RSX DMA round trip and speeds, and fill rate.
Results appear on screen (PASS green, FAIL red) and are saved to USRDIR/rsx_selftest.txt and
USRDIR/rsx_vramtest.txt (also launch.log). Please send both files back.

Also: the RSX command buffer is enlarged from 64 KB to 1 MB, with a 32 MB main-memory region
mapped for the RSX (needed by the hardware renderer). Menus and games use it transparently.

---



## What's new in Frontend 05 (performance + settings)

Install over Frontend 04 (same title ID PPSSPP001). Your games stay in /dev_hdd0/PSPISO.

Speed fixes:
- Core debug logging is OFF by default. Frontend 04 always passed --log, which logged every
  category at DEBUG level with an unbuffered write to core.log on the HDD for each line.
- Thread start-up tracing now stops when the game starts. Frontend 04 kept it on, so every
  condition variable the core created during play wrote and fsynced up to 10 log files.
- The screen is presented on its own thread. SDL's present waits for vsync; emulation used to
  stop for up to a full refresh every frame. Emulation now never waits for the display.
- Faster PSP framebuffer conversion (lookup tables, no per-frame allocations).
- No full-screen CPU clear every frame (only on start, for both display buffers).
- Speed limit (60 fps pacing) and auto frameskip now work. Game defaults match the PPSSPP app
  (no 0xDEADBEEF memory filling, bad memory accesses don't stop the game).

Settings (Square in the game list, saved to USRDIR/settings.ini):
- Screen size: Fit (fullscreen, keeps aspect) / Stretch / Fit 90% (TV overscan) / Pixel 2x.
  Sized to the real output resolution (720p or 1080p).
- Filtering: Bilinear (smooth, done by the RSX during the scale) / Nearest (sharp).
- Frameskip: Auto / Off / 1 / 2 / 3.   Speed limit: On / Off.
- FPS overlay: On / Off (R3 toggles it in game).
- Core logging: Off / Warnings / Full debug (very slow - only for bug reports).
- Curve quality: Low / Medium / High (lower is faster in games that use curved surfaces).
- Hidden: geprofile=1 in settings.ini splits emulation time into CPU vs GE in the overlay
  and log. It slows emulation slightly (it disables PPSSPP's fast syscall path).

Profiling: every 10 seconds launch.log gets a "Perf:" line - game frames per second, frames
shown, emulation speed in % of a real PSP, and per-frame milliseconds for emulation,
rasterizer wait and conversion. Please send these lines back after testing.

Crypto: also fixes big-endian byte order in savedata encryption counters (sceChnnlsv), the DRM
BB cipher counters and PGD header reads (amctrl), and NPDRM EBOOT.PBP header/block-table reads.

Still the same: native 480x272 software rendering, IR interpreter (no JIT), no audio.

---

Previous notes (Frontend 04):


Experimental native game-loading frontend, title ID PPSSPP001. The actual
PPSSPP Core, HLE, IR interpreter and software GPU are linked into this build.
It uses CPU Test07's SDL2 PSL1GHT display/font fix and native PS3 controller input.
CPU Test07 (PPSCPU001) can remain installed. No PSP games are included.

This package has compiled and passed Cell ELF format checks. It has NOT been
run on a PS3. Console startup, game compatibility and performance are unverified.
Audio is disabled, and native FFmpeg is disabled: video playback is incomplete.
This remains an experimental port for hardware testing.

Install the PKG through your CFW package manager. Start it first with no game
files: a game browser should appear. Copy your own ISO or CSO files directly into
/dev_hdd0/PSPISO. This is the only game folder scanned; USB game folders are
not scanned. Flat PBP/ELF/PRX files are also listed; subdirectories are not scanned
in this version. Install Frontend 04 over Frontend 03; the title ID is unchanged.

D-pad moves in the game grid, X launches, Triangle opens a game's options, Square changes the order; Refresh and Exit are in the bar at the top. During a
game, the face buttons, D-pad, left analog stick, Start, Select and L1/R1 map to
PSP controls. L3+R3 stops the game. The PS button opens the XMB exit path.

Startup and frame milestones: /dev_hdd0/game/PPSSPP001/USRDIR/launch.log.
Copies are attempted at /dev_hdd0/tmp/PPSSPP_PS3.log and the root of each mounted
USB device as PPSSPP_PS3.log. Core stderr: USRDIR/core.log; stdout: USRDIR/stdout.log.
Logs reset each launch. If startup freezes, preserve the logs before relaunching.
Game saves use USRDIR/memstick. First report whether the browser appears, then
whether a selected game reaches a PSP display frame, with the last log lines.

Rebuild with CMake 3.21+, Ninja, Python 3 + Pillow, the complete PSL1GHT SDK and
rebuilt threaded C++ runtime (PS3/runtime/README.md). Stock SDK libstdc++ is not
suitable. Set PS3DEV and PPSSPP_PS3_RUNTIME, then run:

    bash PS3/frontend/build.sh /absolute/build-ps3 /absolute/output

Optional CMAKE/NINJA/JOBS select host tools and parallelism. The build script
keeps raw linker output intact, strips a copy, applies sprxlinker, checks Cell
OSABI/import tables/native function descriptors, creates EBOOT.BIN, and packages
all assets. Restore dependencies from the source checkpoint's pinned submodules.

Frontend 03 (retained in 04): Core-created threads with default attributes use a 1 MiB stack.
Startup logs now mark network, Core thread manager, condition-variable creation,
thread creation, configuration and PSP boot initialization. Explicit thread
attributes retain their original settings. This build addresses stack headroom
and diagnoses startup. Frontend 03 reached PSP boot initialization on console.
The larger stacks add up to 1 MiB per Core-created thread; memory allocation
can fail, and the real SDK TLS/condition-variable behavior still needs PS3 testing.
The thread wrapper model test is PS3/tests/run-frontend-thread-model.sh.

Frontend 04 fixes little-endian PRX legacy key serialization and KIRK command
header access, including matching header builders in the savedata/AMCTRL paths.
The raw authenticated header bytes remain unchanged while hashing. Per-format
PRX decrypt results are recorded in core.log; a failed game boot now returns an
error to the frontend. Frontend 03 hardware logs confirm HDD scanning, all worker
threads and PSP boot initialization, then an encrypted-executable load failure.
Successful Lumines boot and frames remain unconfirmed. Regression risk: shared
crypto/header changes affect encrypted modules and savedata on every host;
no savestate fields or HLE dispatch entries change.

Crypto regression runner: PS3/tests/run-prx-decryption.sh BUILD_DIR OUT_DIR.
Set PS3DEV, PPSSPP_PS3_RUNTIME, PPC64_CXX, PPC64_QEMU and PPC64_SYSROOT.
It links the native PPU production PRX object and libkirk archive to Linux libc
for pure-crypto QEMU tests, not SDK runtime emulation. The committed synthetic
fixtures contain no game data. Regenerate with make-prx-fixtures.py using Python
cryptography. Both older type0 and newer type2 formats, in-place operation,
CMAC tamper rejection, short input, SHA1 and CBC headers are covered.
