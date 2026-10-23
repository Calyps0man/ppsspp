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

#include "GameIcons.h"

#include <algorithm>
#include <cstdlib>
#include <memory>

#include "Common/Data/Format/PNGLoad.h"
#include "Common/File/Path.h"
#include "Core/ELF/PBPReader.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/FileSystems/BlockDevices.h"
#include "Core/FileSystems/FileSystem.h"
#include "Core/FileSystems/ISOFileSystem.h"
#include "Core/Loaders.h"

namespace GameIcons {

static bool ReadWhole(IFileSystem *fs, const std::string &name, std::vector<u8> *out) {
	PSPFileInfo info = fs->GetFileInfo(name);
	// ICON0 is about 20 KB and PARAM.SFO under 2 KB; anything far bigger is not one.
	if (!info.exists || info.size <= 0 || info.size > 2 * 1024 * 1024) {
		return false;
	}
	const int handle = fs->OpenFile(name, FILEACCESS_READ);
	if (handle < 0) {
		return false;
	}
	out->resize((size_t)info.size);
	const size_t read = fs->ReadFile(handle, out->data(), info.size);
	fs->CloseFile(handle);
	return read == (size_t)info.size;
}

void Scale(const uint32_t *src, int srcW, int srcH, uint32_t *dst, int dstW, int dstH) {
	for (int y = 0; y < dstH; ++y) {
		// Pixel centres, 8 bits of fraction.
		int sy = ((2 * y + 1) * srcH * 128) / dstH - 128;
		sy = std::max(0, std::min(sy, (srcH - 1) * 256));
		const int y0 = sy >> 8, y1 = std::min(y0 + 1, srcH - 1), fy = sy & 0xFF;
		for (int x = 0; x < dstW; ++x) {
			int sx = ((2 * x + 1) * srcW * 128) / dstW - 128;
			sx = std::max(0, std::min(sx, (srcW - 1) * 256));
			const int x0 = sx >> 8, x1 = std::min(x0 + 1, srcW - 1), fx = sx & 0xFF;
			const uint32_t a = src[y0 * srcW + x0], b = src[y0 * srcW + x1];
			const uint32_t c = src[y1 * srcW + x0], d = src[y1 * srcW + x1];
			uint32_t out = 0;
			for (int shift = 0; shift < 32; shift += 8) {
				const int top = (int)((a >> shift) & 0xFF) * (256 - fx) + (int)((b >> shift) & 0xFF) * fx;
				const int bottom = (int)((c >> shift) & 0xFF) * (256 - fx) + (int)((d >> shift) & 0xFF) * fx;
				const int v = (top * (256 - fy) + bottom * fy + 32768) >> 16;
				out |= (uint32_t)std::min(v, 255) << shift;
			}
			dst[y * dstW + x] = out;
		}
	}
}

static void DecodeIcon(const std::vector<u8> &png, int w, int h, Info *info) {
	int pw = 0, ph = 0;
	unsigned char *rgba = nullptr;
	if (png.empty() || pngLoadPtr(png.data(), png.size(), &pw, &ph, &rgba, 1024, 1024) != 1 || !rgba || pw <= 0 || ph <= 0) {
		free(rgba);
		return;
	}
	std::vector<uint32_t> argb((size_t)pw * ph);
	for (size_t i = 0; i < argb.size(); ++i) {
		const unsigned char *p = rgba + i * 4;
		argb[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
	}
	free(rgba);
	info->icon.resize((size_t)w * h);
	Scale(argb.data(), pw, ph, info->icon.data(), w, h);
	info->iconW = w;
	info->iconH = h;
}

static void ReadTitle(const std::vector<u8> &sfo, Info *info) {
	ParamSFOData data;
	if (!sfo.empty() && data.ReadSFO(sfo)) {
		info->title = data.GetValueString("TITLE");
	}
}

bool Load(const std::string &path, int w, int h, Info *info) {
	*info = Info();
	std::string ext = Path(path).GetFileExtension();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });
	const bool pbp = ext == ".pbp";
	if (!pbp && ext != ".iso" && ext != ".cso" && ext != ".zso" && ext != ".chd") {
		return false;
	}
	std::unique_ptr<FileLoader> loader(ConstructFileLoader(Path(path)));
	if (!loader || !loader->Exists()) {
		return false;
	}
	std::vector<u8> sfo, png;
	if (pbp) {
		PBPReader reader(loader.get());
		if (!reader.IsValid()) {
			return false;
		}
		reader.GetSubFile(PBP_PARAM_SFO, &sfo);
		reader.GetSubFile(PBP_ICON0_PNG, &png);
	} else {
		std::string error;
		BlockDevice *device = ConstructBlockDevice(loader.get(), &error);
		if (!device) {
			return false;
		}
		{
			SequentialHandleAllocator handles;
			ISOFileSystem umd(&handles, std::shared_ptr<BlockDevice>(device));
			ReadWhole(&umd, "/PSP_GAME/PARAM.SFO", &sfo);
			ReadWhole(&umd, "/PSP_GAME/ICON0.PNG", &png);
		}
	}
	ReadTitle(sfo, info);
	DecodeIcon(png, w, h, info);
	return true;
}

}  // namespace GameIcons
