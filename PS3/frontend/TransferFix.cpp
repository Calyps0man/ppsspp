// SPDX-License-Identifier: GPL-2.0-or-later
// Corrections for the installed SDL PSL1GHT scaled-blit backend, which turns every
// SDL_RenderCopy into one RSX transfer-scale command.
//
// 1. Font atlas (from CPU Test 07, unchanged): SDL supplies a 12x24 glyph with integer atlas
//    coordinates, but the RSX command wants 12.4 fixed-point origins and full source dimensions.
// 2. PSP screen texture (480x272): SDL always asks for point sampling. Optionally switch it to the
//    RSX's bilinear interpolator so the scaled-up image is smooth instead of blocky. The RSX does
//    this for free during the copy.
//
// 3. Large sources: the RSX's scaled-image engine only accepts sources up to 1024x1024. A larger
//    one (SDL texture over 1024 wide, the RSX renderer's 3x framebuffer) hangs the RSX, and then
//    everything waiting for a flip hangs with it - the RSX Diagnostics test-pattern freeze. Such
//    transfers are split into tiles of at most 1024x1024 source pixels.
// 4. Menu canvas (1280x720): bilinear, so 1080p output is smooth.
#include <rsx/rsx.h>

static volatile bool g_bilinearScreen = true;

void PS3FrontendSetBilinear(bool enable) {
	g_bilinearScreen = enable;
}

extern "C" void __real_rsxSetTransferScaleSurface(gcmContextData *, const gcmTransferScale *, const gcmTransferSurface *);
// Splits a scaled transfer whose source exceeds the hardware limit into tiles.
void PS3ScaledTransfer(gcmContextData *context, const gcmTransferScale *scale, const gcmTransferSurface *surface) {
	const int maxIn = 1024;
	if ((scale->inW <= maxIn && scale->inH <= maxIn) || scale->inX != 0 || scale->inY != 0 || !scale->outW || !scale->outH) {
		__real_rsxSetTransferScaleSurface(context, scale, surface);
		return;
	}
	const int bpp = scale->format == GCM_TRANSFER_SCALE_FORMAT_R5G6B5 ? 2 : 4;
	const int step = maxIn - 16;  // Leaves room for 16 overlap pixels (filtering at tile edges).
	const int inW = scale->inW, inH = scale->inH;
	const int clipX0 = scale->clipX, clipX1 = scale->clipX + scale->clipW;
	const int clipY0 = scale->clipY, clipY1 = scale->clipY + scale->clipH;
	for (int cy = 0; cy < inH; cy += step) {
		const int sh = inH - cy < step ? inH - cy : step;
		const int pieceH = inH - cy < sh + 16 ? inH - cy : sh + 16;
		const int oy0 = scale->outY + (int)((long long)cy * scale->outH / inH);
		const int oy1 = cy + sh >= inH ? scale->outY + scale->outH : scale->outY + (int)((long long)(cy + sh) * scale->outH / inH);
		for (int cx = 0; cx < inW; cx += step) {
			const int sw = inW - cx < step ? inW - cx : step;
			const int pieceW = inW - cx < sw + 16 ? inW - cx : sw + 16;
			const int ox0 = scale->outX + (int)((long long)cx * scale->outW / inW);
			const int ox1 = cx + sw >= inW ? scale->outX + scale->outW : scale->outX + (int)((long long)(cx + sw) * scale->outW / inW);
			const int kx0 = ox0 > clipX0 ? ox0 : clipX0, kx1 = ox1 < clipX1 ? ox1 : clipX1;
			const int ky0 = oy0 > clipY0 ? oy0 : clipY0, ky1 = oy1 < clipY1 ? oy1 : clipY1;
			if (kx1 <= kx0 || ky1 <= ky0) continue;
			gcmTransferScale piece = *scale;
			piece.inW = (u16)pieceW;
			piece.inH = (u16)pieceH;
			piece.offset = scale->offset + (u32)cy * scale->pitch + (u32)cx * bpp;
			piece.outX = (u16)ox0;
			piece.outY = (u16)oy0;
			piece.outW = (u16)(((long long)pieceW * scale->outW + inW - 1) / inW);
			piece.outH = (u16)(((long long)pieceH * scale->outH + inH - 1) / inH);
			piece.clipX = (u16)kx0;
			piece.clipY = (u16)ky0;
			piece.clipW = (u16)(kx1 - kx0);
			piece.clipH = (u16)(ky1 - ky0);
			__real_rsxSetTransferScaleSurface(context, &piece, surface);
		}
	}
}

extern "C" void __wrap_rsxSetTransferScaleSurface(gcmContextData *context, const gcmTransferScale *scale, const gcmTransferSurface *surface) {
	if (scale->pitch == 192 * 4 && scale->inW == 12 && scale->inH == 24 && scale->inX <= 180 && scale->inY <= 120) {
		gcmTransferScale fixed = *scale;
		fixed.inX = rsxGetFixedUint16((f32)scale->inX);
		fixed.inY = rsxGetFixedUint16((f32)scale->inY);
		fixed.inW = 192; fixed.inH = 144;
		__real_rsxSetTransferScaleSurface(context, &fixed, surface);
	} else if (scale->pitch == 480 * 4 && scale->inW == 480 && scale->inH == 272 && g_bilinearScreen) {
		gcmTransferScale smooth = *scale;
		smooth.interp = GCM_TRANSFER_INTERPOLATOR_LINEAR;
		__real_rsxSetTransferScaleSurface(context, &smooth, surface);
	} else if (scale->pitch == 1280 * 4 && scale->inW == 1280 && scale->inH == 720) {
		// Menu canvas: smooth when scaled to 1080p (identical at 720p).
		gcmTransferScale smooth = *scale;
		smooth.interp = GCM_TRANSFER_INTERPOLATOR_LINEAR;
		PS3ScaledTransfer(context, &smooth, surface);
	} else {
		PS3ScaledTransfer(context, scale, surface);
	}
}
