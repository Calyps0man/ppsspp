// Copyright (c) 2017- PPSSPP Project.

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

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <set>
#include <vector>
#include <mutex>
#include <new>
#include <zstd.h>

#include "ext/xxhash.h"

#include "Common/CommonTypes.h"
#include "Common/File/FileUtil.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/Thread/ParallelLoop.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/TimeUtil.h"
#include "Common/System/System.h"

#include "Core/Core.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Debugger/Record.h"
#include "GPU/Debugger/RecordFormat.h"

namespace GPURecord {

// Appends data (or zeroes, for a null p) at a 4-byte aligned offset, zero padded: players read register blocks and headers as
// words, which a PSP can't do unaligned.
size_t g_dumpByteLimit = 0;
void (*g_dumpDroppedHook)(u32 bytes) = nullptr;
double g_dumpTimeLimit = 0.0;
bool g_dumpRecordReplays = false;
void (*g_dumpAbandonedHook)(double seconds, u32 bytes) = nullptr;

void ChunkedBuffer::resize(u32 n) {
	const u32 oldSize = size_;
	const size_t need = (n + CHUNK - 1) / CHUNK;
	if (failed_ || (limit_ && need * CHUNK > limit_)) {
		failed_ = true;
		return;
	}
	while (chunks_.size() < need) {
		u8 *piece = new (std::nothrow) u8[CHUNK];
		if (!piece) {
			failed_ = true;
			return;
		}
		chunks_.emplace_back(piece);
		memset(piece, 0, CHUNK);
	}
	while (chunks_.size() > need)
		chunks_.pop_back();
	size_ = n;
	// Bytes past the old end within its last piece may be stale after a shrink: zero them.
	if (n > oldSize && oldSize % CHUNK != 0) {
		const u32 end = std::min(n, (oldSize / CHUNK + 1) * CHUNK);
		memset(chunks_[oldSize / CHUNK].get() + oldSize % CHUNK, 0, end - oldSize);
	}
}

const u8 *ChunkedBuffer::span(u32 off, u32 *len) const {
	*len = std::min(CHUNK - off % CHUNK, size_ - off);
	return chunks_[off / CHUNK].get() + off % CHUNK;
}

u8 *ChunkedBuffer::span(u32 off, u32 *len) {
	*len = std::min(CHUNK - off % CHUNK, size_ - off);
	return chunks_[off / CHUNK].get() + off % CHUNK;
}

void ChunkedBuffer::write(u32 off, const void *p, u32 n) {
	if (off >= size_)
		return;
	n = std::min(n, size_ - off);
	const u8 *src = (const u8 *)p;
	while (n) {
		u32 len;
		u8 *d = span(off, &len);
		len = std::min(len, n);
		memcpy(d, src, len);
		off += len; src += len; n -= len;
	}
}

void ChunkedBuffer::fill(u32 off, u8 c, u32 n) {
	if (off >= size_)
		return;
	n = std::min(n, size_ - off);
	while (n) {
		u32 len;
		u8 *d = span(off, &len);
		len = std::min(len, n);
		memset(d, c, len);
		off += len; n -= len;
	}
}

void ChunkedBuffer::read(u32 off, void *p, u32 n) const {
	if (off >= size_)
		return;
	n = std::min(n, size_ - off);
	u8 *dst = (u8 *)p;
	while (n) {
		u32 len;
		const u8 *s = span(off, &len);
		len = std::min(len, n);
		memcpy(dst, s, len);
		off += len; dst += len; n -= len;
	}
}

int ChunkedBuffer::compare(u32 off, const void *p, u32 n) const {
	if (off + (u64)n > size_)
		return 1;
	const u8 *src = (const u8 *)p;
	while (n) {
		u32 len;
		const u8 *s = span(off, &len);
		len = std::min(len, n);
		const int r = memcmp(s, src, len);
		if (r != 0)
			return r;
		off += len; src += len; n -= len;
	}
	return 0;
}

static u32 AppendToPushbuf(ChunkedBuffer &pushbuf, const void *p, u32 sz) {
	const u32 ptr = ((u32)pushbuf.size() + 3) & ~3;
	pushbuf.resize(ptr + sz);
	if (p && sz)
		pushbuf.write(ptr, p, sz);
	return ptr;
}

void Recorder::FlushRegisters() {
	if (!lastRegisters.empty()) {
		Command last{ CommandType::REGISTERS };
		last.sz = (u32)(lastRegisters.size() * sizeof(u32));
		last.ptr = AppendToPushbuf(pushbuf, lastRegisters.data(), last.sz);
		lastRegisters.clear();

		commands.push_back(last);
	}
}

static Path GenRecordingFilename() {
	const Path dumpDir = GetSysDirectory(DIRECTORY_DUMP);

	File::CreateFullPath(dumpDir);

	const std::string prefix = g_paramSFO.GetDiscID();

	for (int n = 1; n < 10000; ++n) {
		std::string filename = StringFromFormat("%s_%04d.ppdmp", prefix.c_str(), n);

		const Path path = dumpDir / filename;

		if (!File::Exists(path)) {
			return path;
		}
	}

	return dumpDir / StringFromFormat("%s_%04d.ppdmp", prefix.c_str(), 9999);
}

void Recorder::DirtyAllVRAM(DirtyVRAMFlag flag) {
	if (flag == DirtyVRAMFlag::UNKNOWN) {
		for (uint32_t i = 0; i < DIRTY_VRAM_SIZE; ++i) {
			if (dirtyVRAM[i] == DirtyVRAMFlag::CLEAN)
				dirtyVRAM[i] = DirtyVRAMFlag::UNKNOWN;
		}
	} else {
		for (uint32_t i = 0; i < DIRTY_VRAM_SIZE; ++i)
			dirtyVRAM[i] = flag;
	}
}

void Recorder::DirtyVRAM(u32 start, u32 sz, DirtyVRAMFlag flag) {
	u32 count = (sz + DIRTY_VRAM_ROUND) >> DIRTY_VRAM_SHIFT;
	u32 first = (start >> DIRTY_VRAM_SHIFT) & DIRTY_VRAM_MASK;
	if (first + count > DIRTY_VRAM_SIZE) {
		DirtyAllVRAM(flag);
		return;
	}

	for (u32 i = 0; i < count; ++i)
		dirtyVRAM[first + i] = flag;
}

void Recorder::DirtyDrawnVRAM() {
	int w = std::min(gstate.getScissorX2(), gstate.getRegionX2()) + 1;
	int h = std::min(gstate.getScissorY2(), gstate.getRegionY2()) + 1;

	bool drawZ = !gstate.isModeClear() && gstate.isDepthWriteEnabled() && gstate.isDepthTestEnabled();
	bool clearZ = gstate.isModeClear() && gstate.isClearModeDepthMask();
	if (drawZ || clearZ) {
		int bytes = 2 * gstate.DepthBufStride() * h;
		if (w > gstate.DepthBufStride())
			bytes += 2 * (w - gstate.DepthBufStride());
		DirtyVRAM(gstate.getDepthBufAddress(), bytes, DirtyVRAMFlag::DRAWN);
	}

	int bpp = gstate.FrameBufFormat() == GE_FORMAT_8888 ? 4 : 2;
	int bytes = bpp * gstate.FrameBufStride() * h;
	if (w > gstate.FrameBufStride())
		bytes += bpp * (w - gstate.FrameBufStride());
	DirtyVRAM(gstate.getFrameBufAddress(), bytes, DirtyVRAMFlag::DRAWN);
}

bool Recorder::BeginRecording() {
	std::unique_lock<std::mutex> guard(callbackLock_);
	nextFrame = false;
	if (PSP_CoreParameter().fileType == IdentifiedFileType::PPSSPP_GE_DUMP && !g_dumpRecordReplays) {
		// Can't record a GE dump. RecordNextFrame refuses this too.
		writeCallback = nullptr;
		return false;
	}

	active = true;
	guard.unlock();
	recordStart_ = time_now_d();
	commandsSinceCheck_ = 0;
	lastTextures.clear();
	lastRenderTargets.clear();
	emitted_.clear();
	flipLastAction = gpuStats.totals.numFlips;
	flipFinishAt = -1;
	pushbuf.clear();
	pushbuf.setLimit(g_dumpByteLimit);

	u32 sz = 512 * 4;
	std::unique_ptr<u32_le[]> state(new u32_le[512]);
	gstate.Save(state.get());
	u32 ptr = AppendToPushbuf(pushbuf, state.get(), sz);
	commands.push_back({ CommandType::INIT, sz, ptr });
	lastVRAM.resize(2 * 1024 * 1024);

	// Also save the initial CLUT.
	GPUDebugBuffer clut;
	if (gpu->GetCurrentClut(clut)) {
		sz = clut.GetStride() * clut.PixelSize();
		_assert_msg_(sz == 1024, "CLUT should be 1024 bytes");
		ptr = AppendToPushbuf(pushbuf, clut.GetData(), sz);
		commands.push_back({ CommandType::CLUT, sz, ptr });
	}

	DirtyAllVRAM(DirtyVRAMFlag::DIRTY);
	return true;
}

// Compressed as one zstd frame per 64 KB piece, one after another (ZSTD_decompress takes a run of
// frames, so players read it as before): no buffer the size of the whole dump is needed.
template <typename GetPiece>
static void WriteCompressedPieces(FILE *fp, size_t total, GetPiece getPiece) {
	const long sizePos = ftell(fp);
	u32_le write_size = 0;
	fwrite(&write_size, sizeof(write_size), 1, fp);
	ZSTD_CCtx *cctx = ZSTD_createCCtx();
	const size_t bound = ZSTD_compressBound(ChunkedBuffer::CHUNK);
	std::unique_ptr<u8[]> compressed(new u8[bound]);
	std::unique_ptr<u8[]> piece(new u8[ChunkedBuffer::CHUNK]);
	size_t written = 0;
	for (size_t off = 0; off < total; off += ChunkedBuffer::CHUNK) {
		const size_t n = std::min<size_t>(ChunkedBuffer::CHUNK, total - off);
		getPiece(off, piece.get(), n);
		size_t c = ZSTD_compressCCtx(cctx, compressed.get(), bound, piece.get(), n, 3);
		if (ZSTD_isError(c))
			break;
		fwrite(compressed.get(), c, 1, fp);
		written += c;
	}
	ZSTD_freeCCtx(cctx);
	const long endPos = ftell(fp);
	write_size = (u32)written;
	fseek(fp, sizePos, SEEK_SET);
	fwrite(&write_size, sizeof(write_size), 1, fp);
	fseek(fp, endPos, SEEK_SET);
}

Path Recorder::WriteRecording() {
	FlushRegisters();

	if (pushbuf.failed() || lastVRAM.failed()) {
		ERROR_LOG(Log::G3D, "GE dump dropped: it needed more memory than was free (%u bytes recorded)", pushbuf.size());
		if (g_dumpDroppedHook)
			g_dumpDroppedHook(pushbuf.size());
		return Path();
	}

	const Path filename = GenRecordingFilename();

	NOTICE_LOG(Log::G3D, "Recording filename: %s", filename.c_str());

	FILE *fp = File::OpenCFile(filename, "wb");
	if (!fp) {
		ERROR_LOG(Log::G3D, "Failed to open '%s' for writing the recording", filename.c_str());
		return Path();
	}
	Header header{};
	memcpy(header.magic, HEADER_MAGIC, sizeof(header.magic));
	header.version = VERSION;
	strncpy(header.gameID, g_paramSFO.GetDiscID().c_str(), sizeof(header.gameID));
	fwrite(&header, sizeof(header), 1, fp);

	u32_le sz = (u32)commands.size();
	fwrite(&sz, sizeof(sz), 1, fp);
	u32_le bufsz = (u32)pushbuf.size();
	fwrite(&bufsz, sizeof(bufsz), 1, fp);

	WriteCompressedPieces(fp, commands.size() * sizeof(Command), [&](size_t off, u8 *dst, size_t n) {
		size_t done = 0;
		while (done < n) {
			const size_t b = off + done;
			const size_t idx = b / sizeof(Command), within = b % sizeof(Command);
			const size_t len = std::min(sizeof(Command) - within, n - done);
			memcpy(dst + done, (const u8 *)&commands[idx] + within, len);
			done += len;
		}
	});
	WriteCompressedPieces(fp, bufsz, [&](size_t off, u8 *dst, size_t n) {
		pushbuf.read((u32)off, dst, (u32)n);
	});

	fclose(fp);

	return filename;
}

static void GetVertDataSizes(int vcount, const void *indices, u32 &vbytes, u32 &ibytes) {
	VertexDecoder vdec;
	VertexDecoderOptions opts{};
	vdec.SetVertexType(gstate.vertType, opts);

	if (indices) {
		u16 lower = 0;
		u16 upper = 0;
		GetIndexBounds(indices, vcount, gstate.vertType, &lower, &upper);

		vbytes = (upper + 1) * vdec.VertexSize();
		u32 idx = gstate.vertType & GE_VTYPE_IDX_MASK;
		if (idx == GE_VTYPE_IDX_8BIT) {
			ibytes = vcount * sizeof(u8);
		} else if (idx == GE_VTYPE_IDX_16BIT) {
			ibytes = vcount * sizeof(u16);
		} else if (idx == GE_VTYPE_IDX_32BIT) {
			ibytes = vcount * sizeof(u32);
		}
	} else {
		vbytes = vcount * vdec.VertexSize();
	}
}

Command Recorder::EmitCommandWithRAM(CommandType t, const void *p, u32 sz, u32 align) {
	FlushRegisters();

	Command cmd{ t, sz, 0 };
	align = std::max(align, 4U);

	if (sz) {
		// The same data emitted before (vertices, indices, palettes, textures used again) is
		// found by its hash and size and points there. This used to search the whole buffer
		// for it with memchr/memcmp at every emission: one frame of Midnight Club, ~3800
		// emissions over up to 7.7 MB, was ~15 GB of searching - 20+ s on the PS3 (build 262:
		// dumps given up). Data only found inside a bigger earlier emission is now stored again.
		bool found = false;
		const uint64_t key = XXH3_64bits_withSeed(p, sz, sz);
		auto it = emitted_.find(key);
		if (it != emitted_.end() && (it->second & (align - 1)) == 0 && it->second + sz <= pushbuf.size() &&
			pushbuf.compare(it->second, p, sz) == 0) {
			cmd.ptr = it->second;
			found = true;
		}

		if (!found) {
			cmd.ptr = (u32)pushbuf.size();
			int pad = 0;
			if (cmd.ptr & (align - 1)) {
				pad = align - (cmd.ptr & (align - 1));
				cmd.ptr += pad;
			}
			pushbuf.resize(pushbuf.size() + sz + pad);
			if (pad) {
				pushbuf.fill(cmd.ptr - pad, 0, pad);
			}
			pushbuf.write(cmd.ptr, p, sz);
			if (!pushbuf.failed())
				emitted_[key] = cmd.ptr;
		}
	}

	commands.push_back(cmd);

	return cmd;
}

void Recorder::UpdateLastVRAM(u32 addr, u32 bytes) {
	u32 base = addr & 0x001FFFFF;
	if (base + bytes > 0x00200000) {
		lastVRAM.write(base, Memory::GetPointerUnchecked(0x04000000 | base), 0x00200000 - base);
		bytes = base + bytes - 0x00200000;
		base = 0;
	}
	lastVRAM.write(base, Memory::GetPointerUnchecked(0x04000000 | base), bytes);
}

void Recorder::ClearLastVRAM(u32 addr, u8 c, u32 bytes) {
	u32 base = addr & 0x001FFFFF;
	if (base + bytes > 0x00200000) {
		lastVRAM.fill(base, c, 0x00200000 - base);
		bytes = base + bytes - 0x00200000;
		base = 0;
	}
	lastVRAM.fill(base, c, bytes);
}

int Recorder::CompareLastVRAM(u32 addr, u32 bytes) const {
	u32 base = addr & 0x001FFFFF;
	if (base + bytes > 0x00200000) {
		int result = lastVRAM.compare(base, Memory::GetPointerUnchecked(0x04000000 | base), 0x00200000 - base);
		if (result != 0)
			return result;

		bytes = base + bytes - 0x00200000;
		base = 0;
	}
	return lastVRAM.compare(base, Memory::GetPointerUnchecked(0x04000000 | base), bytes);
}

u32 Recorder::GetTargetFlags(u32 addr, u32 sizeInRAM) {
	addr &= 0x041FFFFF;
	const bool isTarget = lastRenderTargets.find(addr) != lastRenderTargets.end();

	bool isUnknownVRAM = false;
	bool isDirtyVRAM = false;
	bool isDrawnVRAM = false;
	uint32_t start = (addr >> DIRTY_VRAM_SHIFT) & DIRTY_VRAM_MASK;
	uint32_t blocks = (sizeInRAM + DIRTY_VRAM_ROUND) >> DIRTY_VRAM_SHIFT;
	if (start + blocks >= DIRTY_VRAM_SIZE)
		return 0;
	bool startEven = (addr & DIRTY_VRAM_ROUND) == 0;
	bool endEven = ((addr + sizeInRAM) & DIRTY_VRAM_ROUND) == 0;
	for (uint32_t i = 0; i < blocks; ++i) {
		DirtyVRAMFlag flag = dirtyVRAM[start + i];
		isUnknownVRAM = (isUnknownVRAM || flag == DirtyVRAMFlag::UNKNOWN) && flag != DirtyVRAMFlag::DIRTY && flag != DirtyVRAMFlag::DRAWN;
		isDirtyVRAM = isDirtyVRAM || flag != DirtyVRAMFlag::CLEAN;
		isDrawnVRAM = isDrawnVRAM || flag == DirtyVRAMFlag::DRAWN;

		// Mark the VRAM clean now that it's been copied to VRAM.
		if (flag == DirtyVRAMFlag::UNKNOWN || flag == DirtyVRAMFlag::DIRTY) {
			if ((i > 0 || startEven) && (i < blocks || endEven))
				dirtyVRAM[start + i] = DirtyVRAMFlag::CLEAN;
		}
	}

	if (isUnknownVRAM && isDirtyVRAM) {
		// This means it's only UNKNOWN/CLEAN and not known to be actually dirty.
		// Let's check our shadow copy of what we last sent for this VRAM.
		int diff = CompareLastVRAM(addr, sizeInRAM);
		if (diff == 0)
			isDirtyVRAM = false;
	}

	// The isTarget flag is mostly used for replay of dumps on a PSP.
	u32 flags = isTarget ? 1 : 0;
	// The unchangedVRAM flag tells us we can skip recopying.
	if (!isDirtyVRAM)
		flags |= 2;
	// And the drawn flag tells us this data was potentially drawn to.
	if (isDrawnVRAM)
		flags |= 4;

	return flags;
}

void Recorder::EmitTextureData(int level, u32 texaddr) {
	GETextureFormat format = gstate.getTextureFormat();
	int w = gstate.getTextureWidth(level);
	int h = gstate.getTextureHeight(level);
	int bufw = GetTextureBufw(level, texaddr, format);
	int extraw = w > bufw ? w - bufw : 0;
	u32 sizeInRAM = (textureBitsPerPixel[format] * (bufw * h + extraw)) / 8;
	if (extraw && gstate.isTextureSwizzled() && bufw > 0) {
		// Swizzled, the columns past the buffer width are whole blocks (16 bytes by 8 rows) after
		// each block row, so the last block row reaches further than one extra row's worth.
		const u32 bitsPP = textureBitsPerPixel[format];
		const u32 blocksPerRow = std::max<u32>(1, (u32)bufw * bitsPP / 128);
		const u32 lastBlock = (u32)((h - 1) / 8) * blocksPerRow + ((u32)w * bitsPP / 8 - 1) / 16;
		sizeInRAM = std::max(sizeInRAM, (lastBlock + 1) * 128);
	}

	CommandType type = CommandType((int)CommandType::TEXTURE0 + level);
	const u8 *p = Memory::GetPointerUnchecked(texaddr);
	u32 bytes = Memory::ClampValidSizeAt(texaddr, sizeInRAM);
	struct FramebufData {
		u32_le addr;
		s32_le bufw;
		u32_le flags;
		u32_le pad;
	};
	// A framebuffer is stored as this header followed by its pixels (no copy of the two together:
	// pieces of a MB in one block aren't always there on the PS3).
	FramebufData framebuf{};
	u32 headerBytes = 0;

	if (Memory::IsVRAMAddress(texaddr)) {
		u32 flags = GetTargetFlags(texaddr, bytes);
		framebuf.addr = texaddr;
		framebuf.bufw = bufw;
		framebuf.flags = flags;
		headerBytes = (u32)sizeof(framebuf);

		if ((flags & 2) == 0)
			UpdateLastVRAM(texaddr, bytes);

		// Okay, now we'll just emit this instead.
		type = CommandType((int)CommandType::FRAMEBUF0 + level);
	}

	if (bytes > 0) {
		FlushRegisters();
		const u32 total = headerBytes + bytes;

		// Dumps are huge - let's try to find this already emitted.
		for (u32 prevptr : lastTextures) {
			if (pushbuf.size() < prevptr + total) {
				continue;
			}

			if ((headerBytes == 0 || pushbuf.compare(prevptr, &framebuf, headerBytes) == 0) && pushbuf.compare(prevptr + headerBytes, p, bytes) == 0) {
				commands.push_back({ type, total, prevptr });
				// Okay, that was easy.  Bail out.
				return;
			}
		}

		// Not there, gotta emit anew.
		if (headerBytes == 0) {
			Command cmd = EmitCommandWithRAM(type, p, bytes, 16);
			lastTextures.push_back(cmd.ptr);
		} else {
			u32 ptr = pushbuf.size();
			const u32 pad = (16 - (ptr & 15)) & 15;
			ptr += pad;
			pushbuf.resize(ptr + total);
			pushbuf.write(ptr, &framebuf, headerBytes);
			pushbuf.write(ptr + headerBytes, p, bytes);
			commands.push_back({ type, total, ptr });
			lastTextures.push_back(ptr);
		}
	}
}

void Recorder::FlushPrimState(int vcount) {
	// TODO: Eventually, how do we handle texturing from framebuf/zbuf?
	// TODO: Do we need to preload color/depth/stencil (in case from last frame)?

	lastRenderTargets.insert(PSP_GetVidMemBase() | gstate.getFrameBufRawAddress());
	lastRenderTargets.insert(PSP_GetVidMemBase() | gstate.getDepthBufRawAddress());

	// We re-flush textures always in case the game changed them... kinda expensive.
	bool textureEnabled = gstate.isTextureMapEnabled() || gstate.isAntiAliasEnabled();
	// Play it safe and allow texture coords to emit data too.
	bool textureCoords = (gstate.vertType & GE_VTYPE_TC_MASK) != 0;
	for (int level = 0; level < 8; ++level) {
		u32 texaddr = gstate.getTextureAddress(level);
		if (texaddr && (textureEnabled || textureCoords)) {
			EmitTextureData(level, texaddr);
		}
	}

	const void *verts = Memory::GetPointerOrException(gstate_c.vertexAddr);
	const void *indices = nullptr;
	if ((gstate.vertType & GE_VTYPE_IDX_MASK) != GE_VTYPE_IDX_NONE) {
		indices = Memory::GetPointerOrException(gstate_c.indexAddr);
	}

	u32 ibytes = 0;
	u32 vbytes = 0;
	GetVertDataSizes(vcount, indices, vbytes, ibytes);

	if (indices && ibytes > 0) {
		EmitCommandWithRAM(CommandType::INDICES, indices, ibytes, 4);
	}
	if (verts && vbytes > 0) {
		EmitCommandWithRAM(CommandType::VERTICES, verts, vbytes, 4);
	}
}

void Recorder::EmitTransfer(u32 op) {
	FlushRegisters();

	// This may not make a lot of sense right now, unless it's to a framebuf...
	u32 dstBasePtr = gstate.getTransferDstAddress();
	if (!Memory::IsVRAMAddress(dstBasePtr)) {
		// Skip, not VRAM, so can't affect drawing (we flush textures each prim.)
		return;
	}

	u32 srcBasePtr = gstate.getTransferSrcAddress();
	u32 srcStride = gstate.getTransferSrcStride();
	int srcX = gstate.getTransferSrcX();
	int srcY = gstate.getTransferSrcY();
	u32 dstStride = gstate.getTransferDstStride();
	int dstX = gstate.getTransferDstX();
	int dstY = gstate.getTransferDstY();
	int width = gstate.getTransferWidth();
	int height = gstate.getTransferHeight();
	int bpp = gstate.getTransferBpp();

	u32 srcBytes = ((srcY + height - 1) * srcStride + (srcX + width)) * bpp;
	srcBytes = Memory::ClampValidSizeAt(srcBasePtr, srcBytes);

	u32 dstBytes = ((dstY + height - 1) * dstStride + (dstX + width)) * bpp;
	dstBytes = Memory::ClampValidSizeAt(dstBasePtr, dstBytes);

	if (srcBytes != 0) {
		EmitCommandWithRAM(CommandType::TRANSFERSRC, Memory::GetPointerUnchecked(srcBasePtr), srcBytes, 16);
		DirtyVRAM(dstBasePtr, dstBytes, DirtyVRAMFlag::DIRTY);
	}

	lastRegisters.push_back(op);
}

void Recorder::EmitClut(u32 op) {
	u32 addr = gstate.getClutAddress();

	// Hardware rendering may be using a framebuffer as CLUT.
	// To get at this, we first run the command (normally we're called right before it has run.)
	if (Memory::IsVRAMAddress(addr))
		gpu->SetCmdValue(op);

	// Actually should only be 0x3F, but we allow enhanced CLUTs.  See #15727.
	u32 blocks = (op & 0x7F) == 0x40 ? 0x40 : (op & 0x3F);
	u32 bytes = blocks * 32;
	bytes = Memory::ClampValidSizeAt(addr, bytes);

	if (bytes != 0) {
		// Send the original address so VRAM can be reasoned about.
		if (Memory::IsVRAMAddress(addr)) {
			struct ClutAddrData {
				u32_le addr;
				u32_le flags;
			};
			u32 flags = GetTargetFlags(addr, bytes);
			ClutAddrData data{};
			data.addr = addr;
			data.flags = flags;

			FlushRegisters();
			Command cmd{ CommandType::CLUTADDR, sizeof(data), AppendToPushbuf(pushbuf, &data, sizeof(data)) };
			commands.push_back(cmd);

			if ((flags & 2) == 0)
				UpdateLastVRAM(addr, bytes);
		}
		EmitCommandWithRAM(CommandType::CLUT, Memory::GetPointerUnchecked(addr), bytes, 16);
	}

	lastRegisters.push_back(op);
}

void Recorder::EmitPrim(u32 op) {
	FlushPrimState(op & 0x0000FFFF);

	lastRegisters.push_back(op);
	DirtyDrawnVRAM();
}

void Recorder::EmitBezierSpline(u32 op) {
	int ucount = op & 0xFF;
	int vcount = (op >> 8) & 0xFF;
	FlushPrimState(ucount * vcount);

	lastRegisters.push_back(op);
	DirtyDrawnVRAM();
}

bool Recorder::RecordNextFrame(const std::function<void(const Path &)> callback) {
	if (PSP_CoreParameter().fileType == IdentifiedFileType::PPSSPP_GE_DUMP && !g_dumpRecordReplays) {
		return false;
	}
	std::lock_guard<std::mutex> guard(callbackLock_);
	// Don't take over a recording in progress, it would get the wrong callback and end point.
	if (nextFrame || active) {
		return false;
	}
	flipLastAction = gpuStats.totals.numFlips;
	flipFinishAt = -1;
	writeCallback = callback;
	nextFrame = true;
	return true;
}

void Recorder::FinishRecording() {
	// We're done - this was just to write the result out.
	if (!active) {
		return;
	}

	Path filename = WriteRecording();
	commands.clear();
	pushbuf.clear();
	lastVRAM.clear();
	emitted_.clear();

	NOTICE_LOG(Log::System, "Recording finished");
	flipLastAction = gpuStats.totals.numFlips;
	flipFinishAt = -1;
	lastEdramTrans = 0x400;

	std::function<void(const Path &)> callback;
	{
		std::lock_guard<std::mutex> guard(callbackLock_);
		callback = std::move(writeCallback);
		writeCallback = nullptr;
		active = false;
	}

	if (callback && !filename.empty()) {
		callback(filename);
	}
}

void Recorder::CheckEdramTrans() {
	if (!gpu)
		return;

	uint32_t value = gpu->GetAddrTranslation();
	if (value == lastEdramTrans)
		return;
	lastEdramTrans = value;

	FlushRegisters();
	const u32_le valueLE = value;
	Command cmd{ CommandType::EDRAMTRANS, sizeof(valueLE), AppendToPushbuf(pushbuf, &valueLE, sizeof(valueLE)) };
	commands.push_back(cmd);
}

// Gives up the recording in progress: nothing is written, its memory is freed, the game goes on.
void Recorder::AbandonRecording(double seconds) {
	const u32 bytes = (u32)pushbuf.size();
	{
		std::lock_guard<std::mutex> guard(callbackLock_);
		active = false;
		nextFrame = false;
		writeCallback = nullptr;
	}
	commands.clear();
	pushbuf.clear();
	lastVRAM.clear();
	lastRegisters.clear();
	lastTextures.clear();
	lastRenderTargets.clear();
	emitted_.clear();
	flipLastAction = gpuStats.totals.numFlips;
	flipFinishAt = -1;
	ERROR_LOG(Log::G3D, "GE dump given up: the frame took over %.0f s to record (%u bytes recorded)", seconds, bytes);
	if (g_dumpAbandonedHook)
		g_dumpAbandonedHook(seconds, bytes);
}

void Recorder::NotifyCommand(u32 pc) {
	if (!active) {
		return;
	}
	if (g_dumpTimeLimit > 0.0 && ++commandsSinceCheck_ >= 32) {
		commandsSinceCheck_ = 0;
		const double seconds = time_now_d() - recordStart_;
		if (seconds > g_dumpTimeLimit) {
			AbandonRecording(seconds);
			return;
		}
	}

	if (!Memory::IsValid4AlignedAddress(pc)) {
		ERROR_LOG(Log::G3D, "Bad pc in Recorder: %08x", pc);
		return;
	}

	CheckEdramTrans();
	const u32 op = Memory::ReadUnchecked_U32(pc);
	const GECommand cmd = GECommand(op >> 24);

	switch (cmd) {
	case GE_CMD_VADDR:
	case GE_CMD_IADDR:
	case GE_CMD_JUMP:
	case GE_CMD_CALL:
	case GE_CMD_RET:
	case GE_CMD_END:
	case GE_CMD_SIGNAL:
	case GE_CMD_FINISH:
	case GE_CMD_BASE:
	case GE_CMD_OFFSETADDR:
	case GE_CMD_ORIGIN:
		// These just prepare future commands, and are flushed with those commands.
		// TODO: Maybe add a command just to log that these were hit?
		break;

	case GE_CMD_BOUNDINGBOX:
	case GE_CMD_BJUMP:
		// Since we record each command, this is theoretically not relevant.
		// TODO: Output a CommandType to validate this.
		break;

	case GE_CMD_PRIM:
		EmitPrim(op);
		break;

	case GE_CMD_BEZIER:
	case GE_CMD_SPLINE:
		EmitBezierSpline(op);
		break;

	case GE_CMD_LOADCLUT:
		EmitClut(op);
		break;

	case GE_CMD_TRANSFERSTART:
		EmitTransfer(op);
		break;

	default:
		lastRegisters.push_back(op);
		break;
	}
}

void Recorder::NotifyMemcpy(u32 dest, u32 src, u32 sz) {
	if (!active) {
		return;
	}

	CheckEdramTrans();
	if (Memory::IsVRAMAddress(dest)) {
		FlushRegisters();
		const u32_le destLE = dest;
		Command cmd{ CommandType::MEMCPYDEST, sizeof(destLE), AppendToPushbuf(pushbuf, &destLE, sizeof(destLE)) };
		commands.push_back(cmd);

		sz = Memory::ClampValidSizeAt(dest, sz);
		if (sz != 0) {
			EmitCommandWithRAM(CommandType::MEMCPYDATA, Memory::GetPointerUnchecked(dest), sz, 1);
			UpdateLastVRAM(dest, sz);
			DirtyVRAM(dest, sz, DirtyVRAMFlag::CLEAN);
		}
	}
}

void Recorder::NotifyMemset(u32 dest, int v, u32 sz) {
	if (!active) {
		return;
	}

	CheckEdramTrans();
	struct MemsetCommand {
		u32_le dest;
		s32_le value;
		u32_le sz;
	};

	if (Memory::IsVRAMAddress(dest)) {
		sz = Memory::ClampValidSizeAt(dest, sz);
		MemsetCommand data{};
		data.dest = dest;
		data.value = v;
		data.sz = sz;

		FlushRegisters();
		Command cmd{ CommandType::MEMSET, sizeof(data), AppendToPushbuf(pushbuf, &data, sizeof(data)) };
		commands.push_back(cmd);
		ClearLastVRAM(dest, v, sz);
		DirtyVRAM(dest, sz, DirtyVRAMFlag::CLEAN);
	}
}

void Recorder::NotifyUpload(u32 dest, u32 sz) {
	// This also checks the edram translation value and dirties VRAM.
	NotifyMemcpy(dest, dest, sz);
}

bool Recorder::HasDrawCommands() const {
	if (commands.empty())
		return false;

	for (const Command &cmd : commands) {
		switch (cmd.type) {
		case CommandType::INIT:
		case CommandType::DISPLAY:
		// Not drawing: the palette saved at the start (counted as drawing, a dump ended at the
		// next display change with nothing drawn - Midnight Club, build 262), palette loads,
		// address translation and register changes.
		case CommandType::CLUT:
		case CommandType::CLUTADDR:
		case CommandType::EDRAMTRANS:
		case CommandType::REGISTERS:
			continue;

		default:
			return true;
		}
	}

	// Only init and display commands, keep going.
	return false;
}

void Recorder::NotifyDisplay(u32 framebuf, int stride, int fmt) {
	bool writePending = false;
	if (active && HasDrawCommands()) {
		writePending = true;
	}
	if (!active && nextFrame && (gstate_c.skipDrawReason & SKIPDRAW_SKIPFRAME) == 0) {
		NOTICE_LOG(Log::System, "Recording starting on display...");
		BeginRecording();  // TODO: Handle return value.
	}
	if (!active) {
		return;
	}

	CheckEdramTrans();
	struct DisplayBufData {
		PSPPointer<u8> topaddr;
		s32_le linesize, pixelFormat;
	};

	DisplayBufData disp;
	disp.topaddr.ptr = framebuf;
	disp.linesize = stride;
	disp.pixelFormat = (s32)fmt;

	FlushRegisters();
	u32 sz = (u32)sizeof(disp);
	u32 ptr = AppendToPushbuf(pushbuf, &disp, sz);

	commands.push_back({ CommandType::DISPLAY, sz, ptr });

	if (writePending) {
		NOTICE_LOG(Log::System, "Recording complete on display");
		FinishRecording();
	}
}

void Recorder::NotifyBeginFrame() {
	const bool noDisplayAction = flipLastAction + 4 < gpuStats.totals.numFlips;
	// We do this only to catch things that don't call NotifyDisplay.
	if (active && HasDrawCommands() && (noDisplayAction || gpuStats.totals.numFlips == flipFinishAt)) {
		NOTICE_LOG(Log::System, "Recording complete on frame");

		CheckEdramTrans();
		struct DisplayBufData {
			PSPPointer<u8> topaddr;
			u32_le linesize;
			s32_le pixelFormat;
		};

		DisplayBufData disp;
		u32 linesize = 0;
		GEBufferFormat pixelFormat = GE_FORMAT_565;
		__DisplayGetFramebuf(&disp.topaddr, &linesize, &pixelFormat, 0);
		disp.linesize = linesize;
		disp.pixelFormat = (s32)pixelFormat;

		FlushRegisters();
		u32 sz = (u32)sizeof(disp);
		u32 ptr = AppendToPushbuf(pushbuf, &disp, sz);

		commands.push_back({ CommandType::DISPLAY, sz, ptr });

		FinishRecording();
	}
	if (!active && nextFrame && (gstate_c.skipDrawReason & SKIPDRAW_SKIPFRAME) == 0 && noDisplayAction) {
		NOTICE_LOG(Log::System, "Recording starting on frame...");
		BeginRecording();
		// If we began on a BeginFrame, end on a BeginFrame.
		flipFinishAt = gpuStats.totals.numFlips + 1;
	}
}

void Recorder::NotifyCPU() {
	if (!active) {
		return;
	}

	DirtyAllVRAM(DirtyVRAMFlag::UNKNOWN);
}

}  // namespace GPURecord
