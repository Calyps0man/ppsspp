// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <cstdint>

// Menu screens drawn by the CPU into a 1280x720 image (0xAARRGGBB pixels), which the frontend
// copies to the screen in one transfer. The PS3's SDL renderer can only copy rectangles (no
// blending), so text on a picture has to be composed here.
namespace Canvas {

enum { W = 1280, H = 720 };

// Glyph coverage atlases (0-255), 16 glyphs per row from ' ' to '~': small 12x24 cells (192x144),
// large 24x48 cells (384x288, may be null: large text is then the small font doubled).
bool Init(const uint8_t *smallFont, const uint8_t *largeFont);
void Release();
bool Ready();
const uint32_t *Pixels();

// Background picture with slowly moving light waves; seconds is any running clock.
void Background(double seconds);
// Darkened rounded panel; shade 0-255 is how much darker.
void Panel(int x, int y, int w, int h, int shade);
// Tinted bar (selection), color 0xRRGGBB mixed in at alpha 0-255.
void Bar(int x, int y, int w, int h, uint32_t color, int alpha);
// A plain solid rectangle (no shading or rounded corners).
void Fill(int x, int y, int w, int h, uint32_t color);
// Picture of w x h 0xAARRGGBB pixels, blended by its alpha.
void Image(int x, int y, int w, int h, const uint32_t *argb);
// Rectangle outline, thickness pixels wide, color 0xRRGGBB mixed in at alpha 0-255.
void Outline(int x, int y, int w, int h, int thickness, uint32_t color, int alpha);
// Draws text with a soft shadow; returns its width in pixels.
int Text(int x, int y, const char *text, uint32_t color, bool large = false);
int TextWidth(const char *text, bool large = false);
inline int CharWidth(bool large = false) { return large ? 24 : 12; }

}  // namespace Canvas
