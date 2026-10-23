#pragma once

class PointerWrap;

void Register_sceFont();
void Register_sceLibFttt();

// Whether flash0:/font holds the fonts this game's firmware would have had. Used two ways: the
// HLE font loader asks so it knows whether to unpack them off the disc's updater first, and
// CheckDisableHLEAvailability asks because the real libfont.prx a disc ships reads its fonts from
// there and has nothing to fall back on - so it can only replace our HLE when they're present.
bool NandFontsComplete();

// PS3 "PSP fonts" setting: true lets the fonts be unpacked from the disc's firmware updater and the
// disc's own libfont.prx run with them, as desktop PPSSPP does. False (default) keeps the HLE
// sceFont with the bundled fonts and never unpacks anything (as before build 167).
extern bool g_fontsFromDisc;
// Unpacks the firmware's fonts from the mounted disc's updater into flash0:/font. True if any.
bool InstallFontsFromDiscUpdater();

void __FontInit();
void __FontShutdown();
void __FontDoState(PointerWrap &p);