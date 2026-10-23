Per-game settings
=================

Put a file here named after the game image, without its extension, ending in .ini.
For /dev_hdd0/PSPISO/MyGame.iso that is:

    games/MyGame.ini

It uses the same key=value lines as settings.ini; only the lines present are changed for that
game, and only while it runs. For example:

    speedlimit=0
    frameskip=0
    sasdirect=1

The quickest way to make one: set the Basic options and Advanced tabs the way the game needs
them, then "Save for a game" on the Basic options tab, and choose the game. Its file then has every setting
of those two tabs (not the Debug tab). Triangle on a game: View/edit or Delete game config.
To change a config in the menu: "Load from a game", change the tabs, then "Save for a game".

Keys (default first):

General: screen (0 fit, 1 stretch, 2 fit 90%, 3 pixel 2x), bilinear (1, 0), frameskip (-1 auto,
0 off, 1-3), speedlimit (1, 0), showfps (1, 0), log (0 off, 1 warnings, 2 full debug),
spline (2 high, 1, 0), renderer (1 RSX, 0 software), resolution (1, 2, 3), vramsync (0 minimal,
1 every frame), geprofile (0, 1), cpujit (1 JIT, 0 IR interpreter), sound (1, 0),
pspmem64 (0 32 MB, 1 64 MB), fast3dmode (2 RSX T&L, 1 CPU transform, 0 off),
getiming (100-400), fpscap (60, 30, 20), debugbuttons (0, 1), rstick (0 off, 1 face buttons,
2 D-pad), ftz (-1 auto, 1, 0), videos (-1 auto, 1, 0), audioslow (0 short gaps, 1 wait and
refill, 2 play slower), confirm (-1 auto as the PS3, 1 X, 0 Circle),
language (-1 auto as the PS3, 0-11: Japanese, English, French, Spanish, German, Italian, Dutch,
Portuguese, Russian, Korean, Chinese traditional, Chinese simplified), dateformat (-1 auto,
0 YYYY-MM-DD, 1 MM-DD-YYYY, 2 DD-MM-YYYY), timeformat (-1 auto, 0 24-hour, 1 12-hour),
nickname (text; empty = the PS3's nickname), cpuclock (0 the game's own, or MHz),
volume (100, 0-100), anisotropy (0 off, 2, 4, 8, 16), discfonts (0 emulated, 1 game's own).

Advanced: gethread (1, 0), geasync (1, 0), vmx (1, 0), spuvertex (1, 0), sputexture (1, 0),
vertexring (1, 0), texcheck (0, 1), wbrows (0, 1), rttreuse (1, 0), tlmerge (1, 0),
jitsample (0, 1), jitwarmup (1, 0), rsxtile (1, 2, 0), skipkeep (0, 1), volwipe (0, 1),
latedepth (0, 1), keeplayouts (0, 1), sasdirect (0, 1), iohost (0, 1), jitlarge (0, 1),
gelists (0 emulation thread, 1 own thread, 2 run ahead, 3 don't wait), lensflare (1, 0),
videodeblock (-1 auto, 1, 0), videocap (60 off, 40, 30, -1 auto), ingamemenu (1, 0), r3fps (1, 0).

What each one does: the settings menu (the box at the bottom explains the selected setting) and
the GitHub README.

In a game: START+SELECT opens the in-game options and L3+R3 shows or hides the FPS counter
(Advanced > START+SELECT menu and L3+R3 FPS toggle: ingamemenu, r3fps). With Debug on, L2+R2
saves a GE dump and holding L3 records a 5 second JIT profile into launch.log.
In-game option changes (START+SELECT) are saved to settings.ini (all games), not to these files.

launch.log says which file was applied ("Per-game settings applied from ...").

Diagnostics: jitgeneric=<groups> leaves groups of CPU instructions to the interpreter while the
JIT compiles the rest (1 integer ALU, 2 integer loads/stores, 4 float/vector loads/stores,
8 FPU arithmetic, 16 GPR<->FPR moves, 32 vector ops, 64 no fused patterns; add them up).
jitops=<names> does the same for single instructions (IR op names, comma separated, e.g.
jitops=Madd,Min,Max).

Debug tab keys (build 201; what they log and when to use them: DEBUGGING.txt in USRDIR):
perflog (1 every 10 s, 0 off, 2 every 2 s), stalllog (1, 0), videolog (1, 0), savelog (1, 0),
gewatchlog (1, 0), rsxlog (1, 0), jitlog (1, 0), l2r2 (0 GE dump, 1 draw log), dumpram (1, 0),
dumpvram (1, 0),
nanwatch (0, 1 FPU, 3 +VFPU, 5 +infinities, 7 all; IR interpreter only), storewatch (0, 1;
IR interpreter only), jitgeneric (above). All of them need debug=1.
