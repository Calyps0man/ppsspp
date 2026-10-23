# PPSSPP PS3, ver 0.1 (build 270)

A port of the [PPSSPP](https://www.ppsspp.org) PSP emulator to the PlayStation 3, built with the
open-source PSL1GHT SDK. It runs PSP games from the PS3's hard drive, with a JIT for the PSP's CPU
and a graphics renderer that draws on the PS3's RSX graphics chip.

# Notes from the author

I have only focused on the following games that I personally wanted to get running on PS3. Most of these did not work on PSP Remasters.
There are probably many more games that can run, but I will not be doing more work on them. Maybe somebody can continue this project...

| Game | Notes |
| --- | --- |
| 3rd Birthday | No issues, 2x res, 30 FPS |
| Call of Duty Road to Victory | No issues,  2x res, 20-30 FPS |
| Dead to Rights Retribution | No issues, 2x res, 30 FPS |
| Def Jam Fight for NY: Takeover | No issues, 2x res, 30 FPS |
| Gran Turismo | Quite slow, 10-15 fps |
| Killzone | Mostly around 30 FPS but at 1x res. The first cutscene is slightly glitchy at the very beginning. |
| Midnight Club LA | Runs slowly at 10–15 FPS. Some minor texture issues. Lowering resolution does not help much |
| Silent Hill Origins | No issues, 30 FPS, 2x res, 30 FPS |

Every game is different, so most settings can be changed per game (see
[Per-game settings](#per-game-settings)).

Interface is mostly self-explanatory. Pressing Start+Select together will open an in-game menu to change some settings or quit into the game list.
You can also load configs or edit them yourself. There are many options, I have uploaded best configs for those specific games I have tested. 
However you may find other setting combinations better. Configs must be named according to the game's ISO file (but with INI extension) and placed into the app's "Games" folder.
Keep in mind that config files override global options.


## Contents

- [Requirements](#requirements)
- [Installing](#installing)
- [Adding games](#adding-games)
- [Using the app](#using-the-app)
- [Buttons in a game](#buttons-in-a-game)
- [In-game options](#in-game-options)
- [Settings](#settings)
- [Per-game settings](#per-game-settings)
- [Tips for some games](#tips-for-some-games)
- [Files and folders](#files-and-folders)
- [Logs and bug reports](#logs-and-bug-reports)
- [Building from source](#building-from-source)
- [License and credits](#license-and-credits)

## Requirements

- A PS3 that can install homebrew PKG files (custom firmware or HEN).
- For the fast CPU JIT (the default): PS3MAPI, which Cobra-based custom firmware provides.
  Without it, set **CPU core** to **IR interpreter** (much slower). You can check it in the app:
  SELECT on the game list runs the JIT check.
- Your own PSP games, as ISO or CSO files.

## Installing

1. Download the PKG for your PS3 from the release:
   - custom firmware: `PPSSPP_PS3_build_<number>.pkg`
   - official firmware with HEN: `PPSSPP_PS3_build_<number>_HEN.pkg` (enable HEN before
     installing and before starting the app). The plain PKG doesn't start there ("An error
     occurred during the start operation", 80010009): it is fake-signed, which only custom
     firmware accepts.
2. Copy the PKG to a USB stick and install it with **Package Manager → Install Package Files**
   (or install it with webMAN / your usual PKG installer).
3. The app shows up in the XMB under Game as **PPSSPP PS3 ver 0.1 (build 270)** (with HEN after
   the number for the HEN package). Both install to the same place; installing a newer build over
   an older one keeps your settings, saves and logs.

## Adding games

Copy your PSP games to `/dev_hdd0/PSPISO/` on the PS3's hard drive (FTP, webMAN's file manager,
or a file manager such as multiMAN or IRISMAN). Supported files: `.iso`, `.cso`, `.pbp`, `.elf`,
`.prx`.

## Using the app

The main screen has four tabs along the top: **Games**, **Basic options**, **Advanced** and
**Debug**. The build number is shown in the top right corner.

### Games

Games are shown as a grid of their icons, with the title from the game's own data (ISO, CSO and
PBP files). Icons are read a few at a time as you scroll.

| Button | Action |
| --- | --- |
| D-pad | Choose a game |
| X | Start the game |
| Triangle | Game menu (see below) |
| Square | Change the list order |
| L2 / R2 | Page up / page down |
| L1 / R1 | Previous / next tab (works on every tab) |
| Up from the top row | The **Refresh** and **Sort** buttons above the games |
| Circle | Go up to the tabs (Circle again goes back down) |
| SELECT | JIT check: whether the fast CPU JIT can run on this PS3 (PS3MAPI or direct code writes) |

**Refresh** scans `/dev_hdd0/PSPISO` again. **Sort** (or Square) switches between name A-Z,
name Z-A, newest first and largest first. The order is kept in `settings.ini` (`gamesort=`).

**Triangle** on a game opens its menu:

- **Start game**
- **View/edit game config**: only when the game has a [per-game settings](#per-game-settings)
  file: the whole file to read. Up/Down move line by line, Left/Right or L2/R2 a page, Circle
  closes. X edits the selected line with an on-screen keyboard: D-pad and X type a key, Square
  deletes, Triangle adds a space, L1/R1 move the cursor, L2/R2 go up and down a line, L3/R3 a
  page, START asks, then saves, Circle closes (it asks about unsaved changes).
- **New game config**: only when the game has no such file: an empty one to write in the same
  editor (created when you save).
- **Delete game config**: only when the game has such a file; asks first. The game then uses
  your main settings.
- **Delete game**: asks first. Deletes only the game file, not its saves or settings.

### Options pages

Basic options, Advanced and Debug each have two columns with headings.

| Button | Action |
| --- | --- |
| Up / Down | Choose a setting (the column scrolls when needed) |
| L2 / R2 | Jump to the other column |
| X or Right | Next value |
| Left | Previous value |
| L1 / R1 | Previous / next tab |
| Circle | Go up to the tabs |

Changes are saved to `settings.ini` straight away. The box at the bottom of the screen explains
the setting you're on. Greyed-out settings need another one turned on first (for example Debug).

To quit the app or a game, press the **PS button** and choose Quit Game. That returns you to the
XMB. To leave a game and go back to the game list instead, use **Quit to game list** in the
[in-game options](#in-game-options).

## Buttons in a game

The PSP's buttons are on the same PS3 buttons, and the left stick is the PSP's analog stick. The
PS3's right stick does nothing unless **Right stick** is set (see [Settings](#settings)).

The PS3 also has these:

| Buttons | Action | Turned on by |
| --- | --- | --- |
| START + SELECT together | Open the [in-game options](#in-game-options) (the game pauses) | Advanced > START+SELECT menu (on by default; RSX renderer only) |
| L3 + R3 (both sticks pressed in) | Show or hide the FPS counter | Advanced > L3+R3 FPS toggle (on by default) |
| Hold L3 half a second | Record a 5 second JIT profile into `launch.log` | Debug |
| L2 + R2 | Save a GE dump (a recording of one frame's graphics, for bug reports), or log the next 3000 draws as text | Debug |

The PSP has no L3, R3, L2 or R2, so these combinations never reach the game. With the START+SELECT
menu on, START and SELECT reach the game about 0.15 s late (to tell a single press from the
combo). Turn the menu off and they go straight to the game.

## In-game options

Press **START + SELECT** together in a game. The game pauses and a list of options appears in the
corner of the screen.

| Button | Action |
| --- | --- |
| Up / Down | Choose an option |
| X or Right | Next value |
| Left | Previous value |
| START + SELECT, or Circle | Close the list and continue the game |

The options: Frameskip, Speed limit, Frame cap, GE timing, FPS overlay, Sound when slow, File
loading, Filtering, Draw merging, Keep buffer layouts, Right stick, GE lists, Lens flare
occlusion, Volume, Anisotropic filtering and Frame cap in videos. What each one does is in [Settings](#settings).

The last row, **Quit to game list**, asks "Are you sure?" (No is selected to start with) and
then stops the game and goes back to the PPSSPP game list instead of the XMB.

The changes take effect at once and are saved to `settings.ini`, so they apply to **all games**
from then on (they show on the options tabs too). A game's [per-game file](#per-game-settings)
still wins for that game. Settings that need a restart (renderer, resolution, CPU core...) are
only on the options tabs.

## Settings

Every setting is on one of the options tabs and in `settings.ini`. The **Key** column is the name
used in `settings.ini` and in per-game files. Values are numbers: `1` = on, `0` = off unless
stated.

### Basic options

**Graphics**

| Setting | Key | Values (default in bold) | What it does |
| --- | --- | --- | --- |
| Screen size | `screen` | **0** Fit, 1 Stretch, 2 Fit 90%, 3 Pixel 2x | How the PSP picture fills the TV. Fit keeps the shape, Stretch fills the screen, Fit 90% leaves room for TVs that cut the edges, Pixel 2x is exactly 960x544. |
| Filtering | `bilinear` | **1** Bilinear, 0 Nearest | Smooth or sharp scaling of the picture. |
| Anisotropic filter | `anisotropy` | **0** Off, 2, 4, 8, 16 | Sharper textures on surfaces seen at an angle (roads, floors), for textures the game filters smoothly. Costs some graphics-chip time. |
| Renderer | `renderer` | **1** RSX, 0 Software | RSX draws on the PS3's graphics chip (fast). Software is a slow fallback. |
| Resolution | `resolution` | **1**, 2, 3 | Internal resolution of the RSX renderer (1x, 2x, 3x the PSP's). Higher is sharper and slower. |
| VRAM sync | `vramsync` | **0** Minimal, 1 Every frame | Every frame copies the RSX's pictures back to PSP memory each frame (most compatible). Minimal only when needed (faster). |
| Fast 3D path | `fast3dmode` | **2** RSX transform & lighting, 1 CPU transform, 0 Off | 2 is the fastest. 1 and 0 are slower fallbacks for broken 3D. |
| Curve quality | `spline` | 0 Low, 1 Medium, **2** High | Detail of curved surfaces in games that use them. |
| Frameskip | `frameskip` | **-1** Auto, 0 Off, 1-3 | Auto skips drawing frames only when the game is slow. 1-3 always draws 1 of every 2-4 frames. |
| Videos | `videos` | **-1** Auto (on), 1 On, 0 Off | Video cutscenes. Off: not decoded; the video's clock still runs, so games get past them. |
| Speed limit | `speedlimit` | **1** On, 0 Off | On: never faster than a real PSP. Off: as fast as the PS3 can (music and timers may run too fast). |
| Frame cap | `fpscap` | 60 (no cap), **30**, 20 | Most frames a second a game may make (all three are evenly spaced on a 60 Hz TV). A game that would run faster does less work: at 30 a game running at 60 does half the work. 20 for games that can't hold 30. While a video plays, Frame cap in videos decides instead. |
| FPS overlay | `showfps` | **1** On, 0 Off | Frame rate, speed and timings in the corner. |
| CPU core | `cpujit` | **1** JIT, 0 IR interpreter | The JIT is much faster and needs PS3MAPI. |
| CPU clock | `cpuclock` | **0** Game's own, or MHz: 111, 166, 222, 266, 333, 444 (10-1000 in the ini) | The emulated PSP CPU speed. Higher can smooth games held back by the PSP's CPU but costs PS3 time; lower does less work. Some games break with it. |
| PSP memory | `pspmem64` | **0** 32 MB, 1 64 MB | 64 MB only for games that need a PSP-2000 or later. |
| PSP fonts | `discfonts` | **0** Emulated, 1 Game's own | 1: the PSP's real fonts are unpacked from the firmware updater on the game's disc (into `memstick/PSP/NAND`) the first time, and the game's own font library draws its text with them. May fix wrong letter spacing (Silent Hill Origins). |

**Audio**

| Setting | Key | Values (default in bold) | What it does |
| --- | --- | --- | --- |
| Sound | `sound` | **1** On, 0 Off | Sound output. |
| Volume | `volume` | 0-100, **100** | Game volume in percent. |
| Sound when slow | `audioslow` | **0** Short gaps, 1 Wait and refill, 2 Play slower | What the sound does while the game runs below full speed: short crackles, fewer but longer gaps, or slower and lower-pitched sound without gaps. |

**Controls**

| Setting | Key | Values (default in bold) | What it does |
| --- | --- | --- | --- |
| Right stick | `rstick` | **0** Off, 1 Face buttons, 2 D-pad | The PSP has no right stick. 1: up/down/left/right press Triangle/X/Square/Circle (camera in Call of Duty). 2: it presses the D-pad. |
| Confirm button | `confirm` | **-1** Auto, 1 X, 0 Circle | The button that means OK in the game's menus. Auto: the same as your PS3's setting. |

**PSP system** (take effect when a game starts)

| Setting | Key | Values (default in bold) | What it does |
| --- | --- | --- | --- |
| Language | `language` | **-1** Auto, 0 Japanese, 1 English, 2 French, 3 Spanish, 4 German, 5 Italian, 6 Dutch, 7 Portuguese, 8 Russian, 9 Korean, 10 Chinese (traditional), 11 Chinese (simplified) | The PSP system language, for games with several languages. Auto: your PS3's language (English if the PSP doesn't have it). |
| Date format | `dateformat` | **-1** Auto, 0 YYYY-MM-DD, 1 MM-DD-YYYY, 2 DD-MM-YYYY | How games that show dates write them. |
| Time format | `timeformat` | **-1** Auto, 0 24-hour, 1 12-hour | How games that show the time write it. |
| Nickname | `nickname` | **empty** = your PS3 nickname, or any text | The PSP user name some games show. The menu switches between your PS3 nickname and "PPSSPP"; any other name goes in `settings.ini`, e.g. `nickname=YourName`. |

**Per game**

| Setting | Key | Values | What it does |
| --- | --- | --- | --- |
| Load from a game | - | Press X | Loads one game's config into the Basic options and Advanced tabs (games that have one are marked [config]), so you can change it in the menu and write it back with Save for a game. The tabs are also the settings for games without a config, so those change too (Reset to defaults puts them back). |
| Save for a game | - | Press X | Saves the Basic options and Advanced settings as they are now as one game's config: choose the game, confirm, and they go to its [per-game file](#per-game-settings) (replacing what it had). That game then always starts with them. |

**Reset**

| Setting | Key | Values | What it does |
| --- | --- | --- | --- |
| Reset to defaults | - | - | Sets every Basic option and Advanced setting back to its default (not the Debug tab). Settings saved for a game are not changed. Shows "Done" once pressed. |

### Advanced

These are for testing and for games that need them. Leave them at their defaults unless a game
needs a change; the ones marked EXPERIMENTAL can break games.

**Graphics**

| Setting | Key | Values (default in bold) | What it does |
| --- | --- | --- | --- |
| GE thread | `gethread` | **1** On, 0 Off | 3D drawing on a second PS3 thread, in parallel with the game. |
| SPU vertices | `spuvertex` | **1** On, 0 Off | Vertex decoding and skinning on the SPUs (needs GE thread). |
| SPU textures | `sputexture` | **1** On, 0 Off | Texture change checks and decoding on the SPUs. |
| Vertex data | `vertexring` | **1** Main memory, 0 RSX memory | Where vertices for the RSX are written. |
| Texture check | `texcheck` | **0** After memory writes, 1 Every use | When textures are checked for changes. Every use is slower. |
| Write-back | `wbrows` | **0** Drawn rows, 1 All rows | How much of a picture the RSX drew is copied back to PSP memory. |
| Screen copies | `rttreuse` | **1** Reused, 0 Fresh | Copies of the screen a game uses as a texture are reused within a frame, or made fresh for every draw. |
| Draw merging | `tlmerge` | **1** On, 0 Off | Joins similar 3D draws that follow each other into one RSX draw. |
| RSX tiling | `rsxtile` | **1** On, 2 Screens only, 0 Off | Render targets in tiled RSX memory (faster). Screens only leaves depth buffers untiled. Turn off if screens look scrambled. |
| GE timing | `getiming` | **100** to 400 | How long the emulated graphics chip takes per vertex. 200-400 can let heavy scenes run at 30 instead of 60 game frames with half the work. Use 100 if a game runs in slow motion. |
| Late depth reads | `latedepth` | **0** Off, 1 On | EXPERIMENTAL. A depth buffer read as a texture gets the previous frame's depth: no waiting, but the effect lags a frame. |
| Keep buffer layouts | `keeplayouts` | **0** Off, 1 On | EXPERIMENTAL. A buffer drawn in two layouts keeps both on the RSX (faster in Midnight Club). The game's CPU then can't read those pixels. |
| Lens flare occlusion | `lensflare` | **1** On, 0 Off | Off: the depth a game reads to hide lens flares behind things isn't copied back (faster in Midnight Club; flares may show through buildings). |
| GE lists | `gelists` | **0** Emulation thread, 1 Own thread, 2 Own thread run ahead, 3 Don't wait | EXPERIMENTAL. Where the PSP's display lists are processed. 3 is a hack: the game doesn't wait for its graphics, which may glitch. |

**CPU, sound & other**

| Setting | Key | Values (default in bold) | What it does |
| --- | --- | --- | --- |
| JIT floats | `vmx` | **1** AltiVec, 0 FPU | Which PS3 registers hold the PSP's float and vector values. AltiVec is faster. |
| JIT warm-up | `jitwarmup` | **1** On, 0 Off | New code runs in the interpreter 20 times before it is compiled: fewer hitches from code that runs once. |
| JIT cache | `jitlarge` | **0** Normal, 1 Large | EXPERIMENTAL. Room for 50% more compiled code before the JIT starts over (a hitch). Needs ~9 MB more memory: big games can run out of memory with it (Call of Duty did). With Normal, the cache grows to the Large size by itself the first time it fills, if there's memory for it. |
| Denormals (FTZ) | `ftz` | **-1** Auto, 1 Flushed, 0 Kept | Very small float values flushed to zero when the game asks, as on a PSP. |
| Sound mixing | `sasdirect` | **0** Own thread, 1 Emulation thread | EXPERIMENTAL. 1 mixes the game's sound effects on an SPU straight from the emulation thread (faster). |
| File loading | `iohost` | **0** Fast, 1 Host | EXPERIMENTAL. 1: while a file is read, the game keeps running instead of the whole emulator waiting for the disk (for games that stream data, like Midnight Club). |
| Skip keeps menus | `skipkeep` | **0** Off, 1 On | EXPERIMENTAL. With Frameskip, skipped frames still draw what a game draws only now and then (menus, loading screens). Helped Gran Turismo's menus, broke Silent Hill Origins. |
| Video deblocking | `videodeblock` | **-1** Auto, 1 On, 0 Off | Smooths blocky edges in videos, but can nearly double decoding time on the PS3. Auto: on, and switched off for the rest of a movie that falls below full speed (its sound would crackle). |
| Frame cap in videos | `videocap` | **60** Off, 40, 30, -1 Auto | The Frame cap is lifted while a video plays, so the game keeps up with the video. 40 or 30: the game is capped to that during videos. Auto: the Frame cap comes back only when a video runs below full speed (and goes for that video if the game makes too few frames for it). With any of them, the cap is lifted while the game falls behind the video's pictures, until it has caught up. |
| Dialog memory wipe | `volwipe` | **0** Off, 1 On | Leave off. On: memory a game lent to a save/message dialog is zeroed when it closes, as desktop PPSSPP does (broke Midnight Club). |
| START+SELECT menu | `ingamemenu` | **1** On, 0 Off | START+SELECT in a game opens the [in-game options](#in-game-options) (RSX renderer). Off: START and SELECT go straight to the game. |
| L3+R3 FPS toggle | `r3fps` | **1** On, 0 Off | L3+R3 in a game shows or hides the FPS counter. Off: the combination does nothing. |

`settings.ini` also has `geasync` (default 1), an older switch for running display lists on the
GE thread. Leave it at 1, or set 0 for a game that hangs or flickers.

### Debug

Everything that decides what goes into the logs. Leave **Debug** off for normal play: logging
costs time. The full guide, with which switches to use for which kind of bug, is
[`PS3/frontend/DEBUGGING.md`](PS3/frontend/DEBUGGING.md) (installed as `USRDIR/DEBUGGING.txt`).

**Logs**

| Setting | Key | Values (default in bold) | What it does |
| --- | --- | --- | --- |
| Debug | `debug` | **0** Off, 1 On | The master switch: `launch.log` and the game's log folder, every switch on this tab, and in a game hold L3 (JIT profile) and L2+R2 (GE dump). Off: no logs at all. |
| Core logging | `log` | **0** Off, 1 Warnings, 2 Full debug | PPSSPP's own log (`core.log`). A switch of its own: Debug doesn't turn it on, but it only works with Debug on. Full debug is very slow. |
| Perf report | `perflog` | 0 Off, **1** Every 10 s, 2 Every 2 s | `Perf:` lines: frame rate, speed, where each frame's time goes, memory, JIT, sound. |
| CPU/GE timing | `geprofile` | **0** Off, 1 On | Adds PPSSPP's own display list time to the Perf report and times every HLE call. Slightly slower. |
| Stall reports | `stalllog` | **1** On, 0 Off | `STALL:` lines when no frame comes for 5 s: what each PSP thread waits for. For freezes. |
| Video statistics | `videolog` | **1** On, 0 Off | `Video` lines once a second during a cutscene. For jerky or frozen videos. |
| Save dialog steps | `savelog` | **1** On, 0 Off | `Savedata:` lines for each step of a save/load dialog. For save problems. |
| GE list watch | `gewatchlog` | **1** On, 0 Off | Code that wrote over a display list the GE hadn't finished. For garbage graphics. |
| RSX renderer events | `rsxlog` | **1** On, 0 Off | New framebuffers and layouts, write-backs, unusual blends. For RSX-only glitches. |
| JIT events | `jitlog` | **1** On, 0 Off | `JIT cache full` and recompiled blocks. For hitches every so often. |
| Sound debug | `snddebug` | **0** Off, 1 Log, 2 Log + voices on the PPU | `Sound:` lines every 2 s: volumes, loudest samples and clipping at each step of the sound. For distorted sound. |

**Finding a bug**

| Setting | Key | Values (default in bold) | What it does |
| --- | --- | --- | --- |
| L2+R2 in game | `l2r2` | **0** GE dump, 1 Draw log | GE dump: the next frame's graphics commands are saved to replay on a PC. Draw log: the next 3000 draws written to `launch.log` as text. |
| Dump: PSP RAM | `dumpram` | **1** On, 0 Off | With each GE dump, PSP RAM saved as a `.ram` file next to it. |
| Dump: RSX picture | `dumpvram` | **1** On, 0 Off | With each GE dump, what the RSX drew saved as a `.vram` file next to it. |
| JIT profile | `jitsample` | **0** Off, 1 On | Where the PSP code spends its time: samples at 40 s and every minute after. Needs the JIT. |
| NaN watch | `nanwatch` | **0** Off, 1 FPU, 3 + VFPU, 5 + infinities, 7 all | Names the code that first makes a NaN float (broken cameras, objects flying away). Needs the IR interpreter. |
| Store watch | `storewatch` | **0** Off, 1 On | Names the code that wrote vertex colours a draw finds half-written. Needs the IR interpreter. |
| JIT: interpret | `jitgeneric` | **0**, or a sum of groups 1-64 | The JIT leaves a group of instructions to the interpreter, to find which part of the JIT has a bug. |
| JIT: interpret ops | `jitops` | **empty**, or IR op names | The same for single instructions, e.g. `jitops=Madd,Min`. Only set in an ini file. |

## Per-game settings

Any setting can be different for one game. The quickest way: set the options tabs the way the game
needs them, then **Save for a game** (on the Basic options tab), and choose the game. Its file
then holds every setting of those two tabs (not the Debug tab), so the game always starts with
them, whatever the tabs say later. Set the tabs back for your other games afterwards.
To change a game's file later, use **Load from a game** (above Save for a game): its
settings come into the tabs, you change them, and Save for a game writes them back.

You can also write the file yourself: a text file in
`/dev_hdd0/game/PPSSPP001/USRDIR/games/` named after the game file without its extension, ending
in `.ini`. For `/dev_hdd0/PSPISO/Midnight Club LA.iso` that is:

```
/dev_hdd0/game/PPSSPP001/USRDIR/games/Midnight Club LA.ini
```

Put one `key=value` line per setting, using the keys from the tables above. Only the lines in the
file change; everything else comes from the normal settings. For example:

```ini
# Midnight Club: L.A. Remix
speedlimit=0
frameskip=0
sasdirect=1
getiming=400
jitlarge=1
```

Notes:

- The file name has to match the game file's name (letter case doesn't matter).
- Triangle on the game: **View/edit game config** shows the whole file and edits it (or **New
  game config** makes one), and **Delete game config** removes it.
- A file written by **Save for a game** has all the settings of the two tabs; one you write
  yourself only needs the lines that differ.
- Per-game values only last while that game runs; `settings.ini` keeps the normal ones.
- The PSP system settings (confirm button, language, date and time format, nickname, CPU
  clock) take effect when a game starts.
- Changes made in the [in-game options](#in-game-options) go to `settings.ini` (all games), not
  to this file.
- `launch.log` (with Debug on) says whether a per-game file was found and what it applied.
- The folder has a `README.txt` with a short version of this.

## Files and folders

Everything is in `/dev_hdd0/game/PPSSPP001/USRDIR/`:

| Path | What |
| --- | --- |
| `settings.ini` | Your settings |
| `games/*.ini` | [Per-game settings](#per-game-settings) |
| `memstick/PSP/SAVEDATA/` | Game saves (the emulated memory stick) |
| `memstick/PSP/SYSTEM/DUMP/` | GE dumps (L2 + R2 with Debug on), with their `.ram` and `.vram` files |
| `logs/<game>/` | Logs of the last run of each game (with Debug on) |
| `launch.log` | The app's own log |
| `README.txt` | Notes for each build (what changed) |
| `DEBUGGING.txt` | How to find bugs with the Debug tab |

Games go in `/dev_hdd0/PSPISO/`.

## Logs and bug reports

Turn on **Debug** (Debug tab) and play the game. When you quit, the game's folder in
`USRDIR/logs/` has:

- `launch.log`: the app's log, with the settings used and Perf lines every 10 s (frame rate, where
  the time goes, memory). This is the most useful file.
- `core.log`: PPSSPP's own log (set **Core logging** to Warnings, or Full debug for hard bugs;
  Full debug is very slow).
- `stdout.log` and a copy of `settings.ini`.

`launch.log` is also copied to `/dev_hdd0/tmp/PPSSPP_PS3.log` and, if a USB drive is plugged in,
`PPSSPP_PS3.log` on it, so the log survives even if the PS3 has to be switched off.

For a graphics bug, a GE dump (L2 + R2 at the moment it shows) lets the bug be replayed on a PC.
Dumps go into `USRDIR/memstick/PSP/SYSTEM/DUMP/`: `<game id>_0001.ppdmp` and so on, each with a
`.ram` file (PSP memory) and, with the RSX renderer, a `.vram` file (what the PS3 drew in that
frame). Send them along with the log folder.

`DEBUGGING.txt` says which Debug switches to turn on for each kind of bug (freezes, slow games,
jerky videos, saves, graphics, JIT-only bugs).

When reporting, say which build you used, which game (with its region), what you did and what
went wrong, and attach the game's log folder (zipped). A photo or video of the screen helps too.

## Building from source

The source is in this repository (the PS3 parts are in `PS3/`). See
[`PS3/BUILDING.md`](PS3/BUILDING.md) for the toolchain (ps3dev, PSL1GHT, a thread-enabled
libstdc++ and FFmpeg). With those set up:

```sh
export PS3DEV=/path/to/ps3dev
export PSL1GHT=$PS3DEV
export PPSSPP_PS3_RUNTIME=/path/to/runtime-install
export PPSSPP_PS3_FFMPEG=/path/to/ffmpeg-ps3     # optional: video and MP3/AAC sound
bash PS3/frontend/build.sh build-ps3 artifacts
```

The PKG ends up in `artifacts/`. `PS3/frontend/README.md` has the notes for every build.

## License and credits

PPSSPP is by Henrik Rydgård and the PPSSPP contributors, licensed under the GNU GPL version 2 or
later (`LICENSE.TXT`). This port is under the same license. It uses the PSL1GHT SDK and the
ps3dev toolchain, FFmpeg (LGPL) for video, and other libraries listed in
`PS3/cpu-test/THIRD_PARTY.txt`.

This project is not affiliated with Sony or with the PPSSPP project. PlayStation, PS3 and PSP are
trademarks of Sony Interactive Entertainment. Use your own legally obtained games.
