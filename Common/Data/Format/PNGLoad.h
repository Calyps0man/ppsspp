#pragma once

#include <cstdint>

#include "Common/BitSet.h"
#include "Common/File/Path.h"
#include "Common/Swap.h"

// *image_data_ptr should be deleted with free()
// return value of 1 == success.
int pngLoad(const char *file, int *pwidth, 
            int *pheight, unsigned char **image_data_ptr);

int pngLoadPtr(const unsigned  char *input_ptr, size_t input_len, int *pwidth,
            int *pheight, unsigned char **image_data_ptr,
            int maxWidth = 8192, int maxHeight = 8192);

// PNG peeker - just read the start of a PNG straight into this struct, in order to
// look at basic parameters like width and height. Note that while PNG is a chunk-based
// format, the IHDR chunk is REQUIRED to be the first one, so this will work.
// Does not handle Apple's weirdo extension CgBI. http://iphonedevwiki.net/index.php/CgBI_file_format
// That should not be an issue.
struct PNGHeaderPeek {
	u32_le magic;
	uint32_t ignore0;
	uint32_t ignore1;
	u32_le ihdrTag;
	u32_be be_width;
	u32_be be_height;
	uint8_t bitDepth;   // bits per channel, can be 1, 2, 4, 8, 16
	uint8_t colorType;  // really, pixel format. 0 = grayscale, 2 = rgb, 3 = palette index, 4 = gray+alpha, 6 = rgba

	bool IsValidPNGHeader() const;
	int Width() const { return be_width; }
	int Height() const { return be_height; }
};

bool pngSave(const Path &filename, const void *buffer, int w, int h, int bytesPerPixel);
