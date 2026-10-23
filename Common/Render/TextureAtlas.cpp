#include <cstring>
#include <cstddef>
#include <cstdint>
#include <zstd.h>

#include "Common/Log.h"
#include "Common/Render/TextureAtlas.h"

// Atlas metadata is little-endian on disk; every numeric field is 4 bytes except AtlasChar::pw/ph.
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
static inline void SwapWords(void *p, size_t bytes) {
	uint8_t *b = (uint8_t *)p;
	for (size_t i = 0; i + 4 <= bytes; i += 4) {
		uint8_t t0 = b[i], t1 = b[i + 1];
		b[i] = b[i + 3]; b[i + 1] = b[i + 2]; b[i + 2] = t1; b[i + 3] = t0;
	}
}
static inline void FixEndian(AtlasHeader &h) { SwapWords(&h, sizeof(h)); }
static inline void FixEndian(AtlasFontHeader &h) { SwapWords(&h, offsetof(AtlasFontHeader, name)); }
static inline void FixEndian(AtlasImage &h) { SwapWords(&h, offsetof(AtlasImage, name)); }
static inline void FixEndian(AtlasCharRange &h) { SwapWords(&h, sizeof(h)); }
static inline void FixEndian(AtlasChar &h) {
	SwapWords(&h, offsetof(AtlasChar, pw));
	h.pw = (unsigned short)((h.pw >> 8) | (h.pw << 8));
	h.ph = (unsigned short)((h.ph >> 8) | (h.ph << 8));
}
template <class T> static inline void FixEndianArray(T *t, size_t n) { for (size_t i = 0; i < n; i++) FixEndian(t[i]); }
#else
template <class T> static inline void FixEndian(T &) {}
template <class T> static inline void FixEndianArray(T *, size_t) {}
#endif

class ByteReader {
public:
	ByteReader(const uint8_t *data, size_t size) : data_(data), offset_(0), size_(size) {}

	template<class T>
	T Read() {
		_dbg_assert_(offset_ + sizeof(T) <= size_);
		T x;
		memcpy(&x, data_ + offset_, sizeof(T));
		offset_ += sizeof(T);
		return x;
	}

	template<class T>
	void ReadInto(T *t) {
		_dbg_assert_(offset_ + sizeof(T) <= size_);
		memcpy(t, data_ + offset_, sizeof(T));
		offset_ += sizeof(T);
	}

	template<class T>
	T *ReadMultipleAlloc(size_t count, bool compressed) {
		T *t = new T[count];
		if (!compressed) {
			_dbg_assert_(offset_ + sizeof(T) * count <= size_);
			memcpy(t, data_ + offset_, sizeof(T) * count);
			offset_ += sizeof(T) * count;
		} else {
			_dbg_assert_(offset_ + sizeof(uint32_t) <= size_);
			const uint8_t *cs = data_ + offset_;
			uint32_t compressed_size = (uint32_t)cs[0] | ((uint32_t)cs[1] << 8) | ((uint32_t)cs[2] << 16) | ((uint32_t)cs[3] << 24);
			offset_ += sizeof(uint32_t);

			_dbg_assert_(offset_ + compressed_size <= size_);
			ZSTD_decompress(t, sizeof(T) * count, data_ + offset_, compressed_size);
			offset_ += compressed_size;
		}
		return t;
	}

private:
	const uint8_t *data_;
	size_t offset_;
	size_t size_;
};

void Atlas::Clear() {
	num_images = 0;
	num_fonts = 0;
	delete[] images;
	delete[] fonts;
	images = nullptr;
	fonts = nullptr;
}

bool Atlas::LoadMeta(const uint8_t *data, size_t data_size) {
	ByteReader reader(data, data_size);

	AtlasHeader header = reader.Read<AtlasHeader>();
	FixEndian(header);
	if (header.magic != ATLAS_MAGIC) {
		return false;
	}

	Clear();

	num_images = header.numImages;
	num_fonts = header.numFonts;

	images = reader.ReadMultipleAlloc<AtlasImage>(num_images, header.version >= 1);
	FixEndianArray(images, num_images);
	fonts = new AtlasFont[num_fonts];
	for (int i = 0; i < num_fonts; i++) {
		AtlasFontHeader font_header = reader.Read<AtlasFontHeader>();
		FixEndian(font_header);
		fonts[i].padding = font_header.padding;
		fonts[i].height = font_header.height;
		fonts[i].ascend = font_header.ascend;
		fonts[i].distslope = font_header.distslope;
		fonts[i].numRanges = font_header.numRanges;
		fonts[i].numChars = font_header.numChars;
		AtlasCharRange *ranges = reader.ReadMultipleAlloc<AtlasCharRange>(font_header.numRanges, header.version >= 1);
		FixEndianArray(ranges, font_header.numRanges);
		AtlasChar *chars = reader.ReadMultipleAlloc<AtlasChar>(font_header.numChars, header.version >= 1);
		FixEndianArray(chars, font_header.numChars);
		fonts[i].ranges = ranges;
		fonts[i].charData = chars;
		memcpy(fonts[i].name, font_header.name, sizeof(font_header.name));
	}
	return true;
}

const AtlasFont *Atlas::getFont(FontID id) const {
	if (id.isInvalid())
		return nullptr;

	for (int i = 0; i < num_fonts; i++) {
		if (!strcmp(id.id, fonts[i].name))
			return &fonts[i];
	}
	return nullptr;
}

const AtlasImage *Atlas::getImage(ImageID name) const {
	if (name.isInvalid())
		return nullptr;

	for (int i = 0; i < num_images; i++) {
		if (name.id == images[i].name)
			return &images[i];
	}
	return nullptr;
}

bool Atlas::measureImage(ImageID id, float *w, float *h) const {
	const AtlasImage *image = getImage(id);
	if (image) {
		*w = (float)image->w;
		*h = (float)image->h;
		return true;
	} else {
		*w = 0.0f;
		*h = 0.0f;
		return false;
	}
}

const AtlasChar *AtlasFont::getChar(int utf32) const {
	for (int i = 0; i < numRanges; i++) {
		if (utf32 >= ranges[i].start && utf32 < ranges[i].end) {
			const AtlasChar *c = &charData[ranges[i].result_index + utf32 - ranges[i].start];
			if (c->ex == 0 && c->ey == 0)
				return nullptr;
			else
				return c;
		}
	}
	return nullptr;
}

Atlas::~Atlas() {
	delete[] images;
	delete[] fonts;
}

AtlasFont::~AtlasFont() {
	delete[] ranges;
	delete[] charData;
}
