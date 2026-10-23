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
#include <string>
#include <vector>

// The game list's pictures: the title (PARAM.SFO) and ICON0.PNG of an ISO, CSO or PBP file.
namespace GameIcons {

struct Info {
	std::string title;            // Empty if the file has no PARAM.SFO title.
	std::vector<uint32_t> icon;   // 0xAARRGGBB, scaled to the size asked for; empty if there is no icon.
	int iconW = 0, iconH = 0;
};

// Reads the file's title and icon; the icon is scaled (bilinear) to w x h. False if the file can't be read.
bool Load(const std::string &path, int w, int h, Info *info);

// Bilinear scale of 0xAARRGGBB pixels.
void Scale(const uint32_t *src, int srcW, int srcH, uint32_t *dst, int dstW, int dstH);

}  // namespace GameIcons
