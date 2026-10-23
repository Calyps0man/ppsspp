// Copyright (c) 2013- PPSSPP Project.

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

#include "GPU/PS3Profile.h"
#if defined(__ALTIVEC__)
#include <altivec.h>
#undef vector
#undef pixel
#undef bool
#endif
#include "ppsspp_config.h"

#include <cfloat>
#include <climits>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>

#include "Common/Common.h"
#include "Common/CPUDetect.h"
#include "Common/Data/Convert/ColorConv.h"
#include "Common/Math/math_util.h"
#include "Common/MemoryUtil.h"
#include "Common/Profiler/Profiler.h"
#include "GPU/GPUState.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Common/SoftwareTransformCommon.h"
#include "GPU/Common/TransformCommon.h"
#include "GPU/Common/VertexReader.h"
#include "GPU/GPUStateSIMDUtil.h"
#include "Common/Math/SIMDHeaders.h"
#include "GPU/Software/BinManager.h"
#include "GPU/Software/HardwareBackend.h"
#include "GPU/Software/Clipper.h"
#include "GPU/Software/Lighting.h"
#include "GPU/Software/RasterizerRectangle.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Software/TransformUnit.h"
#include "GPU/Software/TLPrepSpu.h"
#define TLK_GET(ls, ea, len) memcpy((ls), (const void *)(uintptr_t)(ea), (len))
#define TLK_PUT(ls, ea, len) memcpy((void *)(uintptr_t)(ea), (ls), (len))
#define TLK_SYNC()
#include "GPU/Software/TLPrepKernel.h"

// For the SSE4 stuff
#if PPSSPP_ARCH(SSE2)
#include <smmintrin.h>
#endif

#define TRANSFORM_BUF_SIZE (65536 * 48)

TransformUnit::TransformUnit() {
	decoded_ = (u8 *)AllocateAlignedMemory(TRANSFORM_BUF_SIZE, 16);
	_assert_(decoded_);
	binner_ = new BinManager();
}

TransformUnit::~TransformUnit() {
	FreeAlignedMemory(decoded_);
	delete binner_;
}

SoftwareDrawEngine::SoftwareDrawEngine() {
	flushOnParams_ = false;
}

SoftwareDrawEngine::~SoftwareDrawEngine() {}

void SoftwareDrawEngine::NotifyConfigChanged() {
	DrawEngineCommon::NotifyConfigChanged();
}

void SoftwareDrawEngine::Flush() {
	transformUnit.Flush(gpuCommon_, "debug");
}

void SoftwareDrawEngine::DispatchSubmitPrim(const void *verts, const void *inds, GEPrimitiveType prim, int vertexCount, u32 vertTypeID, bool clockwise, int *bytesRead, ClipInfoFlags clipInfoFlags) {
	_assert_msg_(clockwise, "Mixed cull mode not supported.");
	transformUnit.SubmitPrimitive(verts, inds, prim, vertexCount, vertTypeID, bytesRead, this);
}

void SoftwareDrawEngine::DispatchSubmitImm(GEPrimitiveType prim, TransformedVertex *buffer, int vertexCount, int cullMode, bool continuation) {
	uint32_t vertTypeID = GetVertTypeID(gstate.vertType | GE_VTYPE_POS_FLOAT, gstate.getUVGenMode());

	int flipCull = cullMode != gstate.getCullMode() ? 1 : 0;
	// TODO: For now, just setting all dirty.
	transformUnit.SetDirty(SoftDirty(-1));
	gstate.cullmode ^= flipCull;

	// TODO: This is a bit ugly.  Should bypass when clipping...
	uint32_t xScale = gstate.viewportxscale;
	uint32_t xCenter = gstate.viewportxcenter;
	uint32_t yScale = gstate.viewportyscale;
	uint32_t yCenter = gstate.viewportycenter;
	uint32_t zScale = gstate.viewportzscale;
	uint32_t zCenter = gstate.viewportzcenter;

	// Force scale to 1 and center to zero.
	gstate.viewportxscale = (GE_CMD_VIEWPORTXSCALE << 24) | 0x3F8000;
	gstate.viewportxcenter = (GE_CMD_VIEWPORTXCENTER << 24) | 0x000000;
	gstate.viewportyscale = (GE_CMD_VIEWPORTYSCALE << 24) | 0x3F8000;
	gstate.viewportycenter = (GE_CMD_VIEWPORTYCENTER << 24) | 0x000000;
	// Z we scale to 65535 for neg z clipping.
	gstate.viewportzscale = (GE_CMD_VIEWPORTZSCALE << 24) | 0x477FFF;
	gstate.viewportzcenter = (GE_CMD_VIEWPORTZCENTER << 24) | 0x000000;

	// Before we start, submit 0 prims to reset the prev prim type.
	// Following submits will always be KEEP_PREVIOUS.
	if (!continuation)
		transformUnit.SubmitPrimitive(nullptr, nullptr, prim, 0, vertTypeID, nullptr, this);

	for (int i = 0; i < vertexCount; i++) {
		ClipVertexData vert;
		vert.clippos = ClipCoords(buffer[i].pos);
		vert.v.texturecoords.x = buffer[i].u;
		vert.v.texturecoords.y = buffer[i].v;
		vert.v.texturecoords.z = buffer[i].uv_w;
		if (gstate.isModeThrough()) {
			vert.v.texturecoords.x *= gstate.getTextureWidth(0);
			vert.v.texturecoords.y *= gstate.getTextureHeight(0);
		} else {
			vert.clippos.z *= 1.0f / 65535.0f;
		}
		vert.v.clipw = buffer[i].pos_w;
		vert.v.color0 = buffer[i].color0_32;
		vert.v.color1 = gstate.isUsingSecondaryColor() && !gstate.isModeThrough() ? buffer[i].color1_32 : 0;
		vert.v.fogdepth = buffer[i].fog;
		vert.v.screenpos.x = (int)(buffer[i].x * 16.0f);
		vert.v.screenpos.y = (int)(buffer[i].y * 16.0f);
		vert.v.screenpos.z = (u16)(u32)buffer[i].z;

		transformUnit.SubmitImmVertex(vert, this);
	}

	gstate.viewportxscale = xScale;
	gstate.viewportxcenter = xCenter;
	gstate.viewportyscale = yScale;
	gstate.viewportycenter = yCenter;
	gstate.viewportzscale = zScale;
	gstate.viewportzcenter = zCenter;

	gstate.cullmode ^= flipCull;
	// TODO: Should really clear, but a bunch of values are forced so we this is safest.
	transformUnit.SetDirty(SoftDirty(-1));
}

VertexDecoder *SoftwareDrawEngine::FindVertexDecoder(u32 vtype) {
	const u32 vertTypeID = GetVertTypeID(vtype, gstate.getUVGenMode());
	return DrawEngineCommon::GetVertexDecoder(vertTypeID);
}

WorldCoords TransformUnit::ModelToWorldNormal(const ModelCoords &coords) {
	// Each component summed like a matrix row (gpu/probe exp61).
	const float *m = gstate.worldMatrix;
	return WorldCoords(GEDot(coords, Vec3f(m[0], m[3], m[6])), GEDot(coords, Vec3f(m[1], m[4], m[7])), GEDot(coords, Vec3f(m[2], m[5], m[8])));
}

// A clip space component from the combined matrix (gpu/probe exp32, exp34, exp42). The position is a
// float24; the translation is a term of its own.
static inline float GEClipComponent(const Vec3f &v, const float m[16], int c) {
	GERowTerm terms[4] = {
		GEProduct(TruncateToFloat24(v.x), m[c]),
		GEProduct(TruncateToFloat24(v.y), m[4 + c]),
		GEProduct(TruncateToFloat24(v.z), m[8 + c]),
		GEProduct(1.0f, m[12 + c]),
	};
	return GERowSum(terms, 4);
}


// A texture coordinate from the 4x3 texture matrix, summed like a clip space row (gpu/probe exp64).
static inline float GETexGenComponent(const Vec3f &v, const float m[12], int c) {
	GERowTerm terms[4] = {
		GEProduct(TruncateToFloat24(v.x), m[c]),
		GEProduct(TruncateToFloat24(v.y), m[3 + c]),
		GEProduct(TruncateToFloat24(v.z), m[6 + c]),
		GEProduct(1.0f, m[9 + c]),
	};
	return GERowSum(terms, 4);
}

// Multiplies two matrices the way the GE combines world, view and projection (gpu/probe exp35, exp42):
// in the order (world * view) * projection, each entry summed like a row in GEClipComponent.
// On float bits (GEProductBits, GERowSumBits): the same results with no float <-> int moves.
static void GECombineMatricesBits(uint32_t out[16], const uint32_t a[16], const uint32_t b[16]) {
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 4; ++c) {
			GERowTerm terms[4];
			for (int k = 0; k < 4; ++k) {
				terms[k] = GEProductBits(a[r * 4 + k], b[k * 4 + c]);
			}
			out[r * 4 + c] = GERowSumBits(terms, 4);
		}
	}
}

// The last few products, by their inputs (build 155): a draw's transform state is recomputed for
// a lighting or other change with the same matrices, and the GE's exact products are slow.
static void GECombineMatricesBitsMemo(uint32_t out[16], const uint32_t a[16], const uint32_t b[16]) {
	struct Entry {
		uint32_t a[16], b[16], out[16];
		bool valid;
	};
	static Entry cache[8];
	static int next = 0;
	for (Entry &e : cache) {
		if (e.valid && memcmp(e.a, a, sizeof(e.a)) == 0 && memcmp(e.b, b, sizeof(e.b)) == 0) {
			memcpy(out, e.out, sizeof(e.out));
#if PPSSPP_ARCH(POWERPC)
			PS3Profile::xfMemoHits++;
#endif
			return;
		}
	}
	GECombineMatricesBits(out, a, b);
	Entry &e = cache[next];
	next = (next + 1) & 7;
	memcpy(e.a, a, sizeof(e.a));
	memcpy(e.b, b, sizeof(e.b));
	memcpy(e.out, out, sizeof(e.out));
	e.valid = true;
#if PPSSPP_ARCH(POWERPC)
	PS3Profile::xfMemoMisses++;
#endif
}

// A 4x3 matrix (as in gstate) as a 4x4 one in float bits, read straight from memory.
static void Matrix4x3To4x4Bits(uint32_t out[16], const float *m4x3) {
	uint32_t m[12];
	memcpy(m, m4x3, sizeof(m));
	out[0] = m[0]; out[1] = m[1]; out[2] = m[2]; out[3] = 0;
	out[4] = m[3]; out[5] = m[4]; out[6] = m[5]; out[7] = 0;
	out[8] = m[6]; out[9] = m[7]; out[10] = m[8]; out[11] = 0;
	out[12] = m[9]; out[13] = m[10]; out[14] = m[11]; out[15] = 0x3F800000;
}

[[maybe_unused]] static void GECombineMatrices(float out[16], const float a[16], const float b[16]) {
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 4; ++c) {
			GERowTerm terms[4];
			for (int k = 0; k < 4; ++k) {
				terms[k] = GEProduct(a[r * 4 + k], b[k * 4 + c]);
			}
			out[r * 4 + c] = GERowSum(terms, 4);
		}
	}
}

// A screen coordinate as the GE computes it (gpu/depth/transformprecision for Z, gpu/probe for X and Y):
// the component divided by w is it times the reciprocal above, truncated to a float24, then scaled and
// offset with GEAdd. Around a center of 2048, that lands X and Y on the 1/16 subpixel grid.
static inline float GEViewport(float clipC, float clipW, float scale, float center) {
	const float w = TruncateToFloat24(clipW);
	if (!std::isfinite(w) || !std::isfinite(clipC) || fabsf(w) < FLT_MIN) {
		return clipC * scale / clipW + center;
	}
	const float ndc = ProductToFloat24((double)TruncateToFloat24(clipC) * GERecip(w));
	return GEAdd(ProductToFloat24((double)ndc * scale), center);
}

// Screen Z is floored.
static inline float GEScreenZ(float clipZ, float clipW, float zScale, float zCenter) {
	return floorf(GEViewport(clipZ, clipW, zScale, zCenter));
}

template <bool depthClamp, bool alwaysCheckRange>
static ScreenCoords ClipToScreenInternal(Vec3f scaled, const ClipCoords &coords, bool *outside_range_flag) {
	// Account for rounding for X and Y.
	// TODO: Validate actual rounding range.
	constexpr float SCREEN_BOUND = 4095.0f + (15.5f / 16.0f);

	// This matches hardware tests - depth is clamped when this flag is on.
	if constexpr (depthClamp) {
		// A vertex the near plane clips away (z < -w) doesn't set the flag, even for x and y. One exactly on
		// the plane isn't clipped, so its range counts (gpu/clipping/guardband).
		if ((alwaysCheckRange || !(coords.z < -coords.w)) && (scaled.x >= SCREEN_BOUND || scaled.y >= SCREEN_BOUND || scaled.x < 0 || scaled.y < 0)) {
			*outside_range_flag = true;
		}

		if (scaled.z < 0.f)
			scaled.z = 0.f;
		else if (scaled.z > 65535.0f)
			scaled.z = 65535.0f;
	} else if (scaled.x > SCREEN_BOUND || scaled.y >= SCREEN_BOUND || scaled.x < 0 || scaled.y < 0 || scaled.z < 0.0f || scaled.z >= 65536.0f) {
		*outside_range_flag = true;
	}

	// 16 = 0xFFFF / 4095.9375
	static_assert(SCREEN_SCALE_FACTOR == 16, "Currently only supports scale 16");
	int x = (int)floorf(scaled.x * 16.0f) - gstate.getOffsetX16();
	int y = (int)floorf(scaled.y * 16.0f) - gstate.getOffsetY16();
	return ScreenCoords(x, y, scaled.z);
}

static inline ScreenCoords ClipToScreenInternal(const ClipCoords &coords, bool *outside_range_flag) {
	// Parameters here can seem invalid, but the PSP is fine with negative viewport widths etc.
	// The checking that OpenGL and D3D do is actually quite superflous as the calculations still "work"
	// with some pretty crazy inputs, which PSP games are happy to do at times.
	float xScale = gstate.getViewportXScale();
	float xCenter = gstate.getViewportXCenter();
	float yScale = gstate.getViewportYScale();
	float yCenter = gstate.getViewportYCenter();
	float zScale = gstate.getViewportZScale();
	float zCenter = gstate.getViewportZCenter();

	float x = GEViewport(coords.x, coords.w, xScale, xCenter);
	float y = GEViewport(coords.y, coords.w, yScale, yCenter);
	float z = GEScreenZ(coords.z, coords.w, zScale, zCenter);

	if (gstate.isDepthClipEnabled()) {
		return ClipToScreenInternal<true, true>(Vec3f(x, y, z), coords, outside_range_flag);
	}
	return ClipToScreenInternal<false, true>(Vec3f(x, y, z), coords, outside_range_flag);
}

ScreenCoords TransformUnit::ClipToScreen(const ClipCoords &coords, bool *outsideRangeFlag) {
	return ClipToScreenInternal(coords, outsideRangeFlag);
}

// Near plane clipping (gpu/probe exp43-46): from the inside vertex, t = d_in / (d_in - d_out) with
// d = z + w, using the GE's reciprocal, and each coordinate is in + t * (out - in), all in float24 math.
float TransformUnit::NearPlaneT(const ClipCoords &in, const ClipCoords &out) {
	const float dIn = GEAdd(in.z, in.w);
	const float dOut = GEAdd(out.z, out.w);
	const float den = TruncateToFloat24(GEAdd(dIn, -dOut));
	return ProductToFloat24((double)TruncateToFloat24(dIn) * GERecip(den));
}

ClipCoords TransformUnit::NearPlanePoint(const ClipCoords &in, const ClipCoords &out, float t) {
	ClipCoords result;
	for (int c = 0; c < 4; ++c) {
		const float delta = TruncateToFloat24(GEAdd(out[c], -in[c]));
		result[c] = TruncateToFloat24(GEAdd(ProductToFloat24((double)t * delta), in[c]));
	}
	return result;
}

ScreenCoords TransformUnit::DrawingToScreen(const DrawingCoords &coords, u16 z) {
	ScreenCoords ret;
	ret.x = (u32)coords.x * SCREEN_SCALE_FACTOR;
	ret.y = (u32)coords.y * SCREEN_SCALE_FACTOR;
	ret.z = z;
	return ret;
}

enum class MatrixMode {
	POS_TO_CLIP = 1,
	WORLD_TO_CLIP = 2,
};

struct TransformState {
	Lighting::State lightingState;

	float matrix[16];
	Vec4f posToFog;
	// With finite fog parameters, the GE's own arithmetic (gpu/probe exp20): the view z as a row of the
	// combined world-view matrix, then float24(GEAdd(z, end) * slope).
	bool fogGE;
	float viewZColumn[4];
	float fogEnd;
	float fogSlope;
	Vec3f screenScale;
	Vec3f screenAdd;

	ScreenCoords(*roundToScreen)(Vec3f scaled, const ClipCoords &coords, bool *outside_range_flag);

	struct {
		bool enableTransform : 1;
		bool enableLighting : 1;
		bool enableFog : 1;
		bool readUV : 1;
		bool negateNormals : 1;
		uint8_t uvGenMode : 2;
		uint8_t matrixMode : 2;
	};
	// The UV scale and offset, applied in ReadVertex with the GE's arithmetic (the vertex decoder leaves
	// them out then, see UsesGEUVScale).
	bool geUVScale;
	float uvScale[2];
	float uvOffset[2];
};

// UV gen mode 0 in transform mode: u * scale + offset as the GE computes it (gpu/probe exp66).
static bool UsesGEUVScale(u32 vertexType) {
	return (vertexType & GE_VTYPE_THROUGH_MASK) == 0 && gstate.getUVGenMode() == GE_TEXMAP_TEXTURE_COORDS;
}

void ComputeTransformState(TransformState *state, const VertexReader &vreader) {
	state->enableTransform = !vreader.isThrough();
	state->enableLighting = gstate.isLightingEnabled();
	state->enableFog = gstate.isFogEnabled();
	state->readUV = !gstate.isModeClear() && gstate.isTextureMapEnabled() && vreader.hasUV();
	state->geUVScale = !vreader.isThrough() && gstate.getUVGenMode() == GE_TEXMAP_TEXTURE_COORDS;
	state->uvScale[0] = TruncateToFloat24(getFloat24(gstate.texscaleu));
	state->uvScale[1] = TruncateToFloat24(getFloat24(gstate.texscalev));
	state->uvOffset[0] = TruncateToFloat24(getFloat24(gstate.texoffsetu));
	state->uvOffset[1] = TruncateToFloat24(getFloat24(gstate.texoffsetv));
	state->negateNormals = gstate.areNormalsReversed();

	state->uvGenMode = gstate.getUVGenMode();
	if (state->uvGenMode == GE_TEXMAP_UNKNOWN)
		state->uvGenMode = GE_TEXMAP_TEXTURE_COORDS;

	if (state->enableTransform) {
		bool canSkipWorldPos = true;
		if (state->enableLighting) {
			PS3_PROFILE(geXfLight);
			Lighting::ComputeState(&state->lightingState, vreader.hasColor0());
			canSkipWorldPos = !state->lightingState.usesWorldPos;
		} else {
			state->lightingState.usesWorldNormal = state->uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP;
		}
		if (state->uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP) {
			// Shade mapping uses the light vector as lighting sees it, which depends on position for other lights.
			if (!gstate.isDirectionalLight(gstate.getUVLS0()) || !gstate.isDirectionalLight(gstate.getUVLS1())) {
				canSkipWorldPos = false;
			}
		}
		// The viewer direction (see PSPViewDirection), normalized like the GE does.
		Vec3f viewDir(gstate.viewMatrix[2], gstate.viewMatrix[5], gstate.viewMatrix[8]);
		if (GENormalize(viewDir) == 0.0f)
			viewDir = Vec3f(0.0f, 0.0f, 1.0f);
		state->lightingState.viewDir = viewDir;

		uint32_t world[16], view[16], worldviewBits[16], proj[16], mvp[16];
		Matrix4x3To4x4Bits(view, gstate.viewMatrix);
		Matrix4x3To4x4Bits(world, gstate.worldMatrix);
		GECombineMatricesBitsMemo(worldviewBits, world, view);
		float worldview[16];
		memcpy(worldview, worldviewBits, sizeof(worldview));

		// Clip coordinates always come from the model position and the matrices combined like the GE does
		// it, in the order (world * view) * projection. The world position is only needed for lighting.
		state->matrixMode = (uint8_t)(canSkipWorldPos ? MatrixMode::POS_TO_CLIP : MatrixMode::WORLD_TO_CLIP);
		memcpy(proj, gstate.projMatrix, sizeof(proj));
		GECombineMatricesBitsMemo(mvp, worldviewBits, proj);
		memcpy(state->matrix, mvp, sizeof(mvp));

		if (state->enableFog) {
			float fogEnd = getFloat24(gstate.fog1);
			float fogSlope = getFloat24(gstate.fog2);

			// We bake fog end and slope into the dot product.
			state->posToFog = Vec4f(worldview[2], worldview[6], worldview[10], worldview[14] + fogEnd);
			state->fogGE = !my_isnanorinf(fogEnd) && !my_isnanorinf(fogSlope);
			for (int i = 0; i < 4; ++i)
				state->viewZColumn[i] = worldview[2 + 4 * i];
			state->fogEnd = TruncateToFloat24(fogEnd);
			state->fogSlope = TruncateToFloat24(fogSlope);

			// If either are NAN/INF, we simplify so there's no inf + -inf muddying things.
			// This is required for Outrun to render proper skies, for example.
			// The PSP treats these exponents as if they were valid.
			if (my_isnanorinf(fogEnd)) {
				bool sign = std::signbit(fogEnd);
				// The multiply would reverse it if it wasn't infinity (doesn't matter if it's infnan.)
				if (std::signbit(fogSlope))
					sign = !sign;
				// Also allow a multiply by zero (slope) to result in zero, regardless of sign.
				// Act like it was negative and clamped to zero.
				if (fogSlope == 0.0f)
					sign = true;

				// Since this is constant for the entire draw, we don't even use infinity.
				float forced = sign ? 0.0f : 1.0f;
				state->posToFog = Vec4f(0.0f, 0.0f, 0.0f, forced);
			} else if (my_isnanorinf(fogSlope)) {
				// We can't have signs differ with infinities, so we use a large value.
				// Anything outside [0, 1] will clamp, so this essentially forces extremes.
				fogSlope = std::signbit(fogSlope) ? -262144.0f : 262144.0f;
				state->posToFog *= fogSlope;
			} else {
				state->posToFog *= fogSlope;
			}
		}

		state->screenScale = Vec3f(gstate.getViewportXScale(), gstate.getViewportYScale(), gstate.getViewportZScale());
		state->screenAdd = Vec3f(gstate.getViewportXCenter(), gstate.getViewportYCenter(), gstate.getViewportZCenter());
	}

	if (gstate.isDepthClipEnabled())
		state->roundToScreen = &ClipToScreenInternal<true, false>;
	else
		state->roundToScreen = &ClipToScreenInternal<false, false>;
}

#if defined(_M_SSE)
#if defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
[[gnu::target("sse4.1")]]
#endif
static inline __m128 Dot43SSE4(__m128 a, __m128 b) {
	__m128 multiplied = _mm_mul_ps(a, _mm_insert_ps(b, _mm_set1_ps(1.0f), 0x30));
	__m128 lanes3311 = _mm_movehdup_ps(multiplied);
	__m128 partial = _mm_add_ps(multiplied, lanes3311);
	return _mm_add_ss(partial, _mm_movehl_ps(lanes3311, partial));
}
#endif

static inline float Dot43(const Vec4f &a, const Vec3f &b) {
#if defined(_M_SSE) && !PPSSPP_ARCH(X86)
	if (cpu_info.bSSE4_1)
		return _mm_cvtss_f32(Dot43SSE4(a.vec, b.vec));
#elif PPSSPP_ARCH(ARM64_NEON)
	float32x4_t multipled = vmulq_f32(a.vec, vsetq_lane_f32(1.0f, b.vec, 3));
	float32x2_t add1 = vget_low_f32(vpaddq_f32(multipled, multipled));
	float32x2_t add2 = vpadd_f32(add1, add1);
	return vget_lane_f32(add2, 0);
#endif
	return Dot(a, Vec4f(b, 1.0f));
}

ClipVertexData TransformUnit::ReadVertex(const VertexReader &vreader, const TransformState &state) {
	PROFILE_THIS_SCOPE("read_vert");
	// If we ever thread this, we'll have to change this.
	ClipVertexData vertex;

	ModelCoords pos;
	vreader.ReadPosThrough(pos.AsArray());

	static Vec3Packedf lastTC;
	if (state.readUV) {
		vreader.ReadUV(vertex.v.texturecoords.AsArray());
		vertex.v.texturecoords.q() = 0.0f;
		if (state.geUVScale) {
			// The decoder only normalized them (8 and 16 bit UVs are unsigned).
			for (int i = 0; i < 2; ++i) {
				const float scaled = ProductToFloat24((double)TruncateToFloat24(vertex.v.texturecoords[i]) * state.uvScale[i]);
				vertex.v.texturecoords[i] = TruncateToFloat24(GEAdd(scaled, state.uvOffset[i]));
			}
		}
		lastTC = vertex.v.texturecoords;
	} else {
		vertex.v.texturecoords = lastTC;
	}

	static Vec3f lastnormal;
	if (vreader.hasNormal())
		vreader.ReadNrm(lastnormal.AsArray());
	Vec3f normal = lastnormal;
	if (state.negateNormals)
		normal = -normal;

	if (vreader.hasColor0()) {
		vertex.v.color0 = vreader.ReadColor0_8888();
	} else {
		vertex.v.color0 = gstate.getMaterialAmbientRGBA();
	}

	vertex.v.color1 = 0;

	if (state.enableTransform) {
		// Clip coordinates with the GE's precision; the Test Drive map depends on it together with the
		// depth math below (#12786).
		{
			for (int c = 0; c < 4; ++c) {
				vertex.clippos[c] = GEClipComponent(pos, state.matrix, c);
			}
		}

		Vec3f screenScaled;
		screenScaled.x = GEViewport(vertex.clippos.x, vertex.clippos.w, state.screenScale.x, state.screenAdd.x);
		screenScaled.y = GEViewport(vertex.clippos.y, vertex.clippos.w, state.screenScale.y, state.screenAdd.y);
		screenScaled.z = GEScreenZ(vertex.clippos.z, vertex.clippos.w, state.screenScale.z, state.screenAdd.z);
		bool outside_range_flag = false;
		vertex.v.screenpos = state.roundToScreen(screenScaled, vertex.clippos, &outside_range_flag);
		if (outside_range_flag) {
			// We use this, essentially, as the flag.
			vertex.v.screenpos.x = 0x7FFFFFFF;
			return vertex;
		}

		if (state.enableFog && state.fogGE) {
			GERowTerm terms[4] = {
				GEProduct(TruncateToFloat24(pos.x), state.viewZColumn[0]),
				GEProduct(TruncateToFloat24(pos.y), state.viewZColumn[1]),
				GEProduct(TruncateToFloat24(pos.z), state.viewZColumn[2]),
				GEProduct(1.0f, state.viewZColumn[3]),
			};
			const float viewZ = GERowSum(terms, 4);
			const float f = ProductToFloat24((double)TruncateToFloat24(GEAdd(viewZ, state.fogEnd)) * state.fogSlope);
			vertex.v.fogdepth = GEFogFactor(f) * (1.0f / 256.0f);
		} else if (state.enableFog) {
			vertex.v.fogdepth = GEFogFactor(Dot43(state.posToFog, pos)) * (1.0f / 256.0f);
		} else {
			vertex.v.fogdepth = 1.0f;
		}
		vertex.v.clipw = vertex.clippos.w;

		// The normal stays as the world matrix leaves it: lighting scales its dot products by the
		// reciprocal length instead (gpu/probe exp3, world matrix cases).
		Vec3<float> worldnormal;
		float normalRsqrt = 1.0f;
		if (state.lightingState.usesWorldNormal) {
			worldnormal = TransformUnit::ModelToWorldNormal(normal);
			const float len2 = GEDot(worldnormal, worldnormal);
			if (len2 > 0.0f && std::isfinite(len2)) {
				normalRsqrt = GERsqrt(len2);
			} else if (len2 != 0.0f) {
				worldnormal = Vec3f(0.0f, 0.0f, 1.0f);
			}
			// A zero normal stays zero: no diffuse or specular from any light (gpu/probe exp173; SOCOM
			// UCES01242 has meshes without normals but lit).
		}

		// Time to generate some texture coords.  Lighting will handle shade mapping.
		if (state.uvGenMode == GE_TEXMAP_TEXTURE_MATRIX) {
			Vec3f source;
			switch (gstate.getUVProjMode()) {
			case GE_PROJMAP_POSITION:
				source = pos;
				break;

			case GE_PROJMAP_UV:
				source = Vec3f(vertex.v.texturecoords.uv(), 0.0f);
				break;

			case GE_PROJMAP_NORMALIZED_NORMAL:
				// This does not use 0, 0, 1 if length is zero.
				source = normal;
				GENormalize(source);
				break;

			case GE_PROJMAP_NORMAL:
				source = normal;
				break;
			}

			// Note that UV scale/offset are not used in this mode.
			vertex.v.texturecoords = Vec3Packedf(GETexGenComponent(source, gstate.tgenMatrix, 0), GETexGenComponent(source, gstate.tgenMatrix, 1), GETexGenComponent(source, gstate.tgenMatrix, 2));
		} else if (state.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP) {
			Lighting::GenerateLightST(vertex.v, pos, worldnormal, normalRsqrt, state.lightingState.viewDir);
		}

		PROFILE_THIS_SCOPE("light");
		if (state.enableLighting)
			Lighting::Process(vertex.v, pos, worldnormal, normalRsqrt, state.lightingState);
	} else {
		vertex.v.screenpos.x = (int)(pos[0] * SCREEN_SCALE_FACTOR);
		vertex.v.screenpos.y = (int)(pos[1] * SCREEN_SCALE_FACTOR);
		vertex.v.screenpos.z = pos[2];
		vertex.v.clipw = 1.0f;
		vertex.v.fogdepth = 1.0f;
	}

	return vertex;
}

void TransformUnit::SetDirty(SoftDirty flags) {
	binner_->SetDirty(flags);
}
SoftDirty TransformUnit::GetDirty() {
	return binner_->GetDirty();
}

static float ReadRawComponent(const u8 *p, int fmt, int i) {
	switch (fmt) {
	case 1: return ((const s8 *)p)[i] * (1.0f / 128.0f);
	case 2: { s16_le v; memcpy(&v, p + 2 * i, 2); return v * (1.0f / 32768.0f); }
	case 3: { float_le v; memcpy(&v, p + 4 * i, 4); return v; }
	default: return 0.0f;
	}
}

static float ReadRawWeight(const u8 *p, int fmt, int i) {
	switch (fmt) {
	case 1: return p[i] * (1.0f / 128.0f);
	case 2: { u16_le v; memcpy(&v, p + 2 * i, 2); return v * (1.0f / 32768.0f); }
	case 3: { float_le v; memcpy(&v, p + 4 * i, 4); return v; }
	default: return 0.0f;
	}
}

// One morphed component as the GE computes it (gpu/probe exp67, exp107): each frame's value times its
// weight as a float24, summed in frame order with the GE's adder.
template <typename Read>
static float GEMorphComponent(const VertexDecoder &dec, const u8 *in, int off, int c, Read read) {
	float acc = 0.0f;
	for (int k = 0; k < dec.morphcount; ++k) {
		const float v = TruncateToFloat24(read(in + k * dec.onesize_ + off, c));
		const float w = TruncateToFloat24(gstate_c.morphWeights[k]);
		const float term = v == 0.0f || w == 0.0f ? 0.0f : ProductToFloat24((double)v * w);
		if (term != 0.0f)
			acc = acc == 0.0f ? term : TruncateToFloat24(GEAdd(acc, term));
	}
	return acc;
}

// Skinning as the GE does it (gpu/probe exp68, exp70, exp71, bit exact): each bone matrix is scaled by
// its weight, every entry a float24 product, and one accumulator then runs through all the bones in
// order, adding each bone's translation, then x, y and z times its column, with the GE's adder.
// Normals the same without the translation. With morph targets, the morph comes first, weights included
// (gpu/probe exp108). Overwrites what the vertex decoder skinned in float.
static void ApplyGESkinning(u8 *decoded, const VertexDecoder &dec, const u8 *raw, int count) {
	const DecVtxFormat &fmt = dec.GetDecVtxFmt();
	const int nweights = dec.nweights;
	for (int v = 0; v < count; ++v) {
		const u8 *in = raw + v * dec.VertexSize();
		u8 *out = decoded + v * fmt.stride;
		auto component = [&](int off, int c, auto read) {
			return dec.morphcount > 1 ? GEMorphComponent(dec, in, off, c, read) : read(in + off, c);
		};
		float bones[8][12];
		for (int b = 0; b < nweights; ++b) {
			const float w = TruncateToFloat24(component(dec.weightoff, b, [&](const u8 *p, int i) { return ReadRawWeight(p, dec.weighttype, i); }));
			for (int k = 0; k < 12; ++k) {
				const float m = gstate.boneMatrix[b * 12 + k];
				bones[b][k] = m == 0.0f || w == 0.0f ? 0.0f : ProductToFloat24((double)TruncateToFloat24(m) * w);
			}
		}
		auto skin = [&](const float src[3], bool translate, float dst[3]) {
			for (int c = 0; c < 3; ++c) {
				float acc = 0.0f;
				auto add = [&](float term) {
					if (term != 0.0f)
						acc = acc == 0.0f ? TruncateToFloat24(term) : TruncateToFloat24(GEAdd(acc, term));
				};
				for (int b = 0; b < nweights; ++b) {
					if (translate)
						add(bones[b][9 + c]);
					for (int j = 0; j < 3; ++j)
						add(GEProduct(TruncateToFloat24(src[j]), bones[b][j * 3 + c]).Value());
				}
				dst[c] = acc;
			}
		};
		if (dec.pos) {
			float pos[3], skinned[3];
			for (int i = 0; i < 3; ++i)
				pos[i] = component(dec.posoff, i, [&](const u8 *p, int c) { return ReadRawComponent(p, dec.pos, c); });
			skin(pos, true, skinned);
			memcpy(out + fmt.posoff, skinned, sizeof(skinned));
		}
		if (dec.nrm && fmt.nrmfmt == DEC_FLOAT_3) {
			float nrm[3], skinned[3];
			for (int i = 0; i < 3; ++i)
				nrm[i] = component(dec.nrmoff, i, [&](const u8 *p, int c) { return ReadRawComponent(p, dec.nrm, c); });
			skin(nrm, false, skinned);
			memcpy(out + fmt.nrmoff, skinned, sizeof(skinned));
		}
	}
}

// A vertex color channel (0 r, 1 g, 2 b, 3 a) expanded to 8 bits.
static float ReadRawColorChannel(const u8 *p, int fmt, int c) {
	u16_le v16;
	memcpy(&v16, p, 2);
	switch (fmt) {
	case GE_VTYPE_COL_565 >> GE_VTYPE_COL_SHIFT: {
		const u32 c8 = RGB565ToRGBA8888(v16);
		return (float)((c8 >> (8 * c)) & 0xFF);
	}
	case GE_VTYPE_COL_5551 >> GE_VTYPE_COL_SHIFT: {
		const u32 c8 = RGBA5551ToRGBA8888(v16);
		return (float)((c8 >> (8 * c)) & 0xFF);
	}
	case GE_VTYPE_COL_4444 >> GE_VTYPE_COL_SHIFT: {
		const u32 c8 = RGBA4444ToRGBA8888(v16);
		return (float)((c8 >> (8 * c)) & 0xFF);
	}
	case GE_VTYPE_COL_8888 >> GE_VTYPE_COL_SHIFT:
		return (float)p[c];
	default:
		return 0.0f;
	}
}

// Morphing as the GE does it (gpu/probe exp67, exp107): each frame's value times its weight as a
// float24, summed in frame order with the GE's adder. Positions and normals as read; UVs too (8 and
// 16 bit ones unsigned); colors per channel expanded to 8 bits, the sum floored. Overwrites the
// decoder's float result.
static void ApplyGEMorph(u8 *decoded, const VertexDecoder &dec, const u8 *raw, int count) {
	const DecVtxFormat &fmt = dec.GetDecVtxFmt();
	auto morphWith = [&](const u8 *in, int off, int n, float *dst, auto read) {
		for (int c = 0; c < n; ++c)
			dst[c] = GEMorphComponent(dec, in, off, c, read);
	};
	auto morph = [&](const u8 *in, int off, int cfmt, float dst[3]) {
		morphWith(in, off, 3, dst, [&](const u8 *p, int c) { return ReadRawComponent(p, cfmt, c); });
	};
	for (int v = 0; v < count; ++v) {
		const u8 *in = raw + v * dec.VertexSize();
		u8 *out = decoded + v * fmt.stride;
		float r[3];
		if (dec.pos) {
			morph(in, dec.posoff, dec.pos, r);
			memcpy(out + fmt.posoff, r, sizeof(r));
		}
		if (dec.nrm && fmt.nrmfmt == DEC_FLOAT_3) {
			morph(in, dec.nrmoff, dec.nrm, r);
			memcpy(out + fmt.nrmoff, r, sizeof(r));
		}
		if (dec.tc && fmt.uvfmt == DEC_FLOAT_2) {
			float uv[2];
			morphWith(in, dec.tcoff, 2, uv, [&](const u8 *p, int c) { return ReadRawWeight(p, dec.tc, c); });
			memcpy(out + fmt.uvoff, uv, sizeof(uv));
		}
		if (dec.col && fmt.c0fmt == DEC_U8_4) {
			float ch[4];
			morphWith(in, dec.coloff, 4, ch, [&](const u8 *p, int c) { return ReadRawColorChannel(p, dec.col, c); });
			for (int c = 0; c < 4; ++c)
				out[fmt.c0off + c] = (u8)std::clamp((int)std::floor(ch[c]), 0, 255);
		}
	}
}

class SoftwareVertexReader {
public:
	SoftwareVertexReader(u8 *base, VertexDecoder &vdecoder, u32 vertex_type, int vertex_count, const void *vertices, const void *indices, const TransformState &transformState, TransformUnit &transform)
	: vreader_(base, vdecoder.GetDecVtxFmt(), vertex_type), conv_(vertex_type, indices), transformState_(transformState), transform_(transform) {
		useIndices_ = indices != nullptr;
		lowerBound_ = 0;
		upperBound_ = vertex_count == 0 ? 0 : vertex_count - 1;

		if (useIndices_)
			GetIndexBounds(indices, vertex_count, vertex_type, &lowerBound_, &upperBound_);
		used_ = nullptr;
		dec_ = &vdecoder;
		raw_ = (const u8 *)vertices + vdecoder.VertexSize() * lowerBound_;
		vertices_ = vertices;
		if (vertex_count != 0) {
			PS3_PROFILE(decode);
			const int count = upperBound_ - lowerBound_ + 1;
			const UVScale uvScale = UsesGEUVScale(vertex_type) ? UVScale{ 1.0f, 1.0f, 0.0f, 0.0f } : LoadUVScaleOffset(gstate);
			// Indices that use few vertices spread over a large range: decode only the used ones (in
			// runs), and let the batched path skip the rest. Only with the hardware renderer, whose
			// paths all read vertices through the indices.
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
			const bool hw = true;  // Test build: exercise it against the reference.
#else
			const bool hw = Rasterizer::g_hardwareBackend != nullptr;
#endif
			const bool sparse = hw && useIndices_ && count > 64 && count > 2 * vertex_count;
			if (sparse) {
				static std::vector<uint8_t> used;
				used.assign(count, 0);
				for (int i = 0; i < vertex_count; ++i)
					used[conv_(i) - lowerBound_] = 1;
				used_ = used.data();
				const u8 *src = (const u8 *)vertices + vdecoder.VertexSize() * lowerBound_;
				const int stride = vdecoder.GetDecVtxFmt().stride;
				for (int j = 0; j < count;) {
					if (!used[j]) {
						++j;
						continue;
					}
					int k = j + 1;
					while (k < count && used[k])
						++k;
					vdecoder.DecodeVerts(base + j * stride, src + j * vdecoder.VertexSize(), &uvScale, k - j);
					j = k;
				}
			} else {
				vdecoder.DecodeVerts(base, (const u8 *)vertices + vdecoder.VertexSize() * lowerBound_, &uvScale, count);
			}
			const u8 *raw = (const u8 *)vertices + vdecoder.VertexSize() * lowerBound_;
			// The decoder has already morphed and skinned in plain float. The GE-exact versions are
			// expensive; the PS3 hardware renderer goes without them.
			const bool exact = Rasterizer::g_hardwareBackend == nullptr;
			if (exact && vdecoder.morphcount > 1 && !vdecoder.throughmode)
				ApplyGEMorph(base, vdecoder, raw, count);
			if (exact && vdecoder.weighttype != 0 && !vdecoder.throughmode)
				ApplyGESkinning(base, vdecoder, raw, count);
		}

		// If we're only using a subset of verts, it's better to decode with random access (usually.)
		// However, if we're reusing a lot of verts, we should read and cache them.
		useCache_ = useIndices_ && vertex_count > (upperBound_ - lowerBound_ + 1);
		if (useCache_ && (int)cached_.size() < upperBound_ - lowerBound_ + 1)
			cached_.resize(std::max(128, upperBound_ - lowerBound_ + 1));
	}

	const VertexReader &GetVertexReader() const {
		return vreader_;
	}

	// For the batched hardware path: index into the decoded range, and the decoded-range size.
	int DecodedIndex(int vtx) {
		return useIndices_ ? (int)conv_(vtx) - lowerBound_ : vtx;
	}
	int DecodedCount() const {
		return upperBound_ - lowerBound_ + 1;
	}
	// Decoded vertices actually referenced (nullptr: all of them).
	const uint8_t *UsedMap() const {
		return used_;
	}
	VertexReader &RawReader() {
		return vreader_;
	}
	// The PSP vertex behind decoded vertex i, and the decoder (for the direct T&L check).
	const u8 *RawVertex(int i) const {
		return raw_ + (size_t)i * dec_->VertexSize();
	}
	const VertexDecoder &Decoder() const {
		return *dec_;
	}
	const void *Vertices() const {
		return vertices_;
	}

	bool IsThrough() const {
		return vreader_.isThrough();
	}

	void UpdateCache() {
		if (!useCache_)
			return;

		for (int i = 0; i < upperBound_ - lowerBound_ + 1; ++i) {
			vreader_.Goto(i);
			cached_[i] = transform_.ReadVertex(vreader_, transformState_);
		}
	}

	inline ClipVertexData Read(int vtx) {
		if (useIndices_) {
			if (useCache_) {
				return cached_[conv_(vtx) - lowerBound_];
			}
			vreader_.Goto(conv_(vtx) - lowerBound_);
		} else {
			vreader_.Goto(vtx);
		}

		return transform_.ReadVertex(vreader_, transformState_);
	};

protected:
	VertexReader vreader_;
	const IndexConverter conv_;
	const TransformState &transformState_;
	TransformUnit &transform_;
	uint16_t lowerBound_;
	uint16_t upperBound_;
	static std::vector<ClipVertexData> cached_;
	bool useIndices_ = false;
	bool useCache_ = false;
	const uint8_t *used_ = nullptr;
	const VertexDecoder *dec_ = nullptr;
	const u8 *raw_ = nullptr;
	const void *vertices_ = nullptr;
};

// Static to reduce allocations mid-frame.
std::vector<ClipVertexData> SoftwareVertexReader::cached_;

// Lighting and generated texture coordinates for the batched path, in plain float math (the
// software GPU's ReadVertex does them with the GE's exact float24 arithmetic, far too slow here).
static void FastLightAndTexGen(const VertexReader &r, const TransformState &ts, const float pos[3], Rasterizer::FastVertex &o) {
	static Vec3f lastnormal;
	if (r.hasNormal())
		r.ReadNrm(lastnormal.AsArray());
	Vec3f normal = ts.negateNormals ? -lastnormal : lastnormal;
	const float *w = gstate.worldMatrix;
	const Lighting::State &ls = ts.lightingState;
	Vec3f worldpos(0.0f, 0.0f, 0.0f), worldnormal(0.0f, 0.0f, 0.0f);
	if (ls.usesWorldPos || ts.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP) {
		worldpos.x = w[0] * pos[0] + w[3] * pos[1] + w[6] * pos[2] + w[9];
		worldpos.y = w[1] * pos[0] + w[4] * pos[1] + w[7] * pos[2] + w[10];
		worldpos.z = w[2] * pos[0] + w[5] * pos[1] + w[8] * pos[2] + w[11];
	}
	if (ls.usesWorldNormal) {
		worldnormal.x = w[0] * normal.x + w[3] * normal.y + w[6] * normal.z;
		worldnormal.y = w[1] * normal.x + w[4] * normal.y + w[7] * normal.z;
		worldnormal.z = w[2] * normal.x + w[5] * normal.y + w[8] * normal.z;
		const float len2 = worldnormal.x * worldnormal.x + worldnormal.y * worldnormal.y + worldnormal.z * worldnormal.z;
		if (len2 > 0.0f && std::isfinite(len2)) {
			const float inv = 1.0f / sqrtf(len2);
			worldnormal.x *= inv; worldnormal.y *= inv; worldnormal.z *= inv;
		} else if (len2 != 0.0f) {
			worldnormal = Vec3f(0.0f, 0.0f, 1.0f);
		}
	}

	VertexData v;
	v.color0 = o.color0;
	v.color1 = 0;
	v.texturecoords = Vec3Packedf(o.s, o.t, 0.0f);
	if (ts.uvGenMode == GE_TEXMAP_TEXTURE_MATRIX) {
		Vec3f source;
		switch (gstate.getUVProjMode()) {
		case GE_PROJMAP_POSITION: source = Vec3f(pos[0], pos[1], pos[2]); break;
		case GE_PROJMAP_UV: source = Vec3f(o.s, o.t, 0.0f); break;
		case GE_PROJMAP_NORMALIZED_NORMAL: {
			source = normal;
			const float len2 = source.x * source.x + source.y * source.y + source.z * source.z;
			if (len2 > 0.0f) {
				const float inv = 1.0f / sqrtf(len2);
				source.x *= inv; source.y *= inv; source.z *= inv;
			}
			break;
		}
		default: source = normal; break;
		}
		const float *t = gstate.tgenMatrix;
		v.texturecoords = Vec3Packedf(
			source.x * t[0] + source.y * t[3] + source.z * t[6] + t[9],
			source.x * t[1] + source.y * t[4] + source.z * t[7] + t[10],
			source.x * t[2] + source.y * t[5] + source.z * t[8] + t[11]);
	} else if (ts.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP) {
		Lighting::GenerateLightSTFast(v, worldpos, worldnormal, ls.viewDir);
	}
	if (ts.enableLighting)
		Lighting::ProcessFast(v, worldpos, worldnormal, ls);
	o.color0 = v.color0;
	o.color1 = v.color1;
	o.s = v.texturecoords.s();
	o.t = v.texturecoords.t();
	o.q = v.texturecoords.q();
}

// Hardware transform & lighting (TLParams, HardwareBackend.h): the per-draw constants.
static void BuildTLParams(const TransformState &ts, Rasterizer::TLParams &p) {
	memcpy(p.mvp, ts.matrix, sizeof(p.mvp));
	for (int i = 0; i < 3; ++i) {
		p.screenScale[i] = ts.screenScale[i];
		p.screenAdd[i] = ts.screenAdd[i];
	}
	p.offX = gstate.getOffsetX16() * (1.0f / 16.0f);
	p.offY = gstate.getOffsetY16() * (1.0f / 16.0f);
	memset(p.c, 0, sizeof(p.c));
	auto C = [&](int k) -> float * { return p.c[k - 3]; };
	auto set4 = [](float *o, float a, float b, float c, float d) { o[0] = a; o[1] = b; o[2] = c; o[3] = d; };
	auto color = [](float *o, uint32_t c, bool alpha) {
		for (int i = 0; i < 3; ++i)
			o[i] = ((c >> (8 * i)) & 0xFF) * (1.0f / 255.0f);
		o[3] = alpha ? (c >> 24) * (1.0f / 255.0f) : 0.0f;
	};
	const float *w = gstate.worldMatrix;
	for (int r = 0; r < 3; ++r)
		set4(C(7 + r), w[r], w[3 + r], w[6 + r], w[9 + r]);
	if (ts.enableFog && ts.fogGE) {
		set4(C(10), ts.viewZColumn[0], ts.viewZColumn[1], ts.viewZColumn[2], ts.viewZColumn[3]);
		set4(C(11), ts.fogEnd, ts.fogSlope, 1.0f, 0.0f);
	} else if (ts.enableFog) {
		set4(C(10), ts.posToFog.x, ts.posToFog.y, ts.posToFog.z, ts.posToFog.w);
		set4(C(11), 0.0f, 1.0f, 1.0f, 0.0f);
	}
	C(11)[3] = ts.enableLighting ? 1.0f : 0.0f;
	if (ts.geUVScale)
		set4(C(12), ts.uvScale[0], ts.uvScale[1], ts.uvOffset[0], ts.uvOffset[1]);
	else
		set4(C(12), 1.0f, 1.0f, 0.0f, 0.0f);
	set4(C(21), 0.0f, 1.0f, 0.5f, 1e-20f);
	for (int l = 0; l < 4; ++l) {
		const int b = 22 + 6 * l;
		set4(C(b + 1), 1.0f, 0.0f, 0.0f, 0.0f);
		set4(C(b + 2), 0.0f, 0.0f, 0.0f, -2.0f);
	}
	// The view direction (half vectors), and depth clamping (also needed unlit).
	set4(C(20), ts.lightingState.viewDir.x, ts.lightingState.viewDir.y, ts.lightingState.viewDir.z, gstate.isDepthClipEnabled() ? 0.0f : 1.0f);
	if (ts.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP) {
		// uv from two lights' vectors (Lighting::GenerateLightST): their positions even when they
		// or lighting are off (they then add no color), N.H instead of N.L for specular lights.
		const int ls[2] = { gstate.getUVLS0(), gstate.getUVLS1() };
		for (int k = 0; k < 2; ++k) {
			const int l = ls[k];
			const int b = 22 + 6 * l;
			float x = getFloat24(gstate.lpos[l * 3]), y = getFloat24(gstate.lpos[l * 3 + 1]), z = getFloat24(gstate.lpos[l * 3 + 2]);
			const bool directional = gstate.isDirectionalLight(l);
			if (directional) {
				const float len2 = x * x + y * y + z * z;
				if (len2 > 0.0f) {
					const float inv = 1.0f / sqrtf(len2);
					x *= inv; y *= inv; z *= inv;
				}
			}
			set4(C(b), x, y, z, directional ? 1.0f : 0.0f);
			const bool spec = gstate.isUsingSpecularLight(l);
			C(46 + l)[(k * 2) + (spec ? 1 : 0)] = 1.0f;
		}
		C(50)[0] = 1.0f;
	}
	if (ts.uvGenMode == GE_TEXMAP_TEXTURE_MATRIX) {
		// (s, t, q) = tgen matrix (3 columns of 4) times the source (Call of Duty's UVs).
		const float *t = gstate.tgenMatrix;
		for (int r = 0; r < 3; ++r)
			set4(C(51 + r), t[r], t[3 + r], t[6 + r], t[9 + r]);
		float *src = C(54);
		switch (gstate.getUVProjMode()) {
		case GE_PROJMAP_POSITION: src[0] = 1.0f; break;
		case GE_PROJMAP_UV: src[1] = 1.0f; break;
		case GE_PROJMAP_NORMAL: src[2] = 1.0f; break;
		default: src[3] = 1.0f; break;
		}
		C(50)[1] = 1.0f;
	}
	if (!ts.enableLighting)
		return;
	const Lighting::State &ls = ts.lightingState;
	color(C(13), gstate.getMaterialEmissive(), false);
	color(C(14), gstate.getAmbientRGBA(), true);
	set4(C(15), ls.colorForAmbient ? 1.0f : 0.0f, ls.colorForDiffuse ? 1.0f : 0.0f, ls.colorForSpecular ? 1.0f : 0.0f, 0.0f);
	color(C(16), gstate.getMaterialAmbientRGBA(), true);
	color(C(17), gstate.getMaterialDiffuse(), false);
	color(C(18), gstate.getMaterialSpecular(), false);
	set4(C(19), ls.specularExp, ls.setColor1 ? 1.0f : 0.0f, ls.addColor1 ? 1.0f : 0.0f, 0.0f);
	for (int l = 0; l < 4; ++l) {
		const auto &L = ls.lights[l];
		if (!L.enabled)
			continue;
		const int b = 22 + 6 * l;
		set4(C(b), L.pos.x, L.pos.y, L.pos.z, L.directional ? 1.0f : 0.0f);
		if (!L.directional)
			set4(C(b + 1), L.att.x, L.att.y, L.att.z, 0.0f);
		if (L.spot)
			set4(C(b + 2), L.spotDir.x * L.spotDirRsqrt, L.spotDir.y * L.spotDirRsqrt, L.spotDir.z * L.spotDirRsqrt, L.spotCutoff);
		if (L.ambient)
			color(C(b + 3), gstate.getLightAmbientColor(l), false);
		C(b + 3)[3] = L.spot ? L.spotExp : 0.0f;
		if (L.diffuse)
			color(C(b + 4), gstate.getDiffuseColor(l), false);
		if (L.specular)
			color(C(b + 5), gstate.getSpecularColor(l), false);
		C(b + 5)[3] = L.spot ? 1.0f : 0.0f;
	}
}

// Whether a draw can use hardware T&L: transformed, ordinary UVs, and no powered diffuse (the
// vertex program doesn't do it).
static bool CanUseTL(const TransformState &ts, bool through) {
	if (through || (ts.uvGenMode != GE_TEXMAP_TEXTURE_COORDS && ts.uvGenMode != GE_TEXMAP_ENVIRONMENT_MAP && ts.uvGenMode != GE_TEXMAP_TEXTURE_MATRIX))
		return false;
	if (ts.enableLighting) {
		for (int l = 0; l < 4; ++l)
			if (ts.lightingState.lights[l].enabled && gstate.isUsingPoweredDiffuseLight(l))
				return false;
	}
	return true;
}

// The T&L vertex for decoded vertex i.
static void ReadTLVertex(VertexReader &r, const TransformState &ts, Rasterizer::TLVertex &o) {
	static Vec3f lastnormal;
	float pos[3];
	r.ReadPosThrough(pos);
	o.x = pos[0]; o.y = pos[1]; o.z = pos[2];
	if (r.hasNormal())
		r.ReadNrm(lastnormal.AsArray());
	const float sign = ts.negateNormals ? -1.0f : 1.0f;
	o.nx = lastnormal.x * sign; o.ny = lastnormal.y * sign; o.nz = lastnormal.z * sign;
	tlk_fix_normal(&o.nx, &o.ny, &o.nz);
	const uint32_t c = r.hasColor0() ? r.ReadColor0_8888() : gstate.getMaterialAmbientRGBA();
	o.c[0] = (uint8_t)c; o.c[1] = (uint8_t)(c >> 8); o.c[2] = (uint8_t)(c >> 16); o.c[3] = (uint8_t)(c >> 24);
	float uv[2] = {0.0f, 0.0f};
	if (ts.readUV)
		r.ReadUV(uv);
	o.u = uv[0]; o.v = uv[1];
}

// Direct T&L decode: reads the PSP vertex format straight into the T&L vertex the RSX program
// takes, skipping VertexDecoder (a chain of per-component step calls on the PPU, no vertex JIT
// there) and VertexReader. Same values as DecodeVerts + ReadTLVertex for what it accepts (no
// morphing, no remastered double UVs).
struct TLDecodeSetup {
	const u8 *base = nullptr;
	int size = 0;
	u8 wt = 0, nw = 0, tc = 0, col = 0, nrm = 0, pos = 0;
	u8 woff = 0, tcoff = 0, coloff = 0, nrmoff = 0, posoff = 0;
	u8 morph = 1, onesize = 0;
	bool readUV = false;
	// Bone matrices and morph weights (gstate's, or a copy taken for the GE worker thread).
	const float *bones = nullptr;
	const float *morphW = nullptr;
	float nsign = 1.0f;
	u32 matAmbient = 0;
	// Build 269: the vertex has no normal and nothing reads one (no lighting, no environment mapping,
	// no texture matrix from the normal): every vertex gets (0, 0, 1). Otherwise such vertices take
	// the last normal decoded, which the SPUs (each keeping its own) and the PPU got differently -
	// Killzone's walls, drawn without normals and unlit, turned white for seconds at a time with SPU
	// vertices on (the diag build's check found normals the only difference, on exactly such draws).
	bool fixedNormal = false;
};

static inline u16 TLRdU16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline float TLRdF(const u8 *p) {
	const u32 v = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

static bool SetupTLDecode(TLDecodeSetup &s, const VertexDecoder &dec, const TransformState &ts, const void *vertices) {
	extern bool g_DoubleTextureCoordinates;
	if (g_DoubleTextureCoordinates || dec.pos == 0 || dec.throughmode)
		return false;
	// Misaligned vertices decode as zeros in VertexDecoder; leave those to it.
	if (((uintptr_t)vertices & (dec.biggest - 1)) != 0)
		return false;
	s.base = (const u8 *)vertices;
	s.size = dec.VertexSize();
	s.morph = dec.morphcount;
	s.onesize = dec.onesize_;
	s.wt = dec.weighttype; s.nw = dec.nweights; s.tc = dec.tc; s.col = dec.col; s.nrm = dec.nrm; s.pos = dec.pos;
	s.woff = dec.weightoff; s.tcoff = dec.tcoff; s.coloff = dec.coloff; s.nrmoff = dec.nrmoff; s.posoff = dec.posoff;
	s.readUV = ts.readUV && dec.tc != 0;
	s.nsign = ts.negateNormals ? -1.0f : 1.0f;
	s.matAmbient = gstate.getMaterialAmbientRGBA();
	{
		const GETexProjMapMode proj = gstate.getUVProjMode();
		const bool normalUsed = ts.enableLighting || ts.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP ||
			(ts.uvGenMode == GE_TEXMAP_TEXTURE_MATRIX && proj != GE_PROJMAP_POSITION && proj != GE_PROJMAP_UV);
		s.fixedNormal = dec.nrm == 0 && !normalUsed;
	}
	s.bones = gstate.boneMatrix;
	s.morphW = gstate_c.morphWeights;
	return true;
}

static void SkinMatrix(const TLDecodeSetup &s, const u8 *p, float skin[12]) {
	float w[8];
	const u8 *wd = p + s.woff;
	for (int j = 0; j < s.nw; ++j)
		w[j] = s.wt == 1 ? wd[j] * (1.0f / 128.0f) : (s.wt == 2 ? TLRdU16(wd + j * 2) * (1.0f / 32768.0f) : TLRdF(wd + j * 4));
	for (int i = 0; i < 12; ++i)
		skin[i] = 0.0f;
	for (int j = 0; j < s.nw; ++j) {
		if (w[j] != 0.0f) {
			const float *bone = &s.bones[j * 12];
			for (int i = 0; i < 12; ++i)
				skin[i] += w[j] * bone[i];
		}
	}
}

// Morphed vertices: the frames' components blended with gstate_c.morphWeights, in the decoder's
// arithmetic order (its CROSSSIMD_SLOW steps). Weights are read from the first frame.
static void DecodeTLVertexMorph(const TLDecodeSetup &s, const u8 *p, Rasterizer::TLVertex &o, float lastNormal[3], bool &fullAlpha) {
	const float *mw = s.morphW;
	float skin[12];
	if (s.wt)
		SkinMatrix(s, p, skin);
	float u = 0.0f, v = 0.0f;
	if (s.readUV) {
		float uv[2] = { 0.0f, 0.0f };
		for (int n = 0; n < s.morph; ++n) {
			const u8 *t = p + s.onesize * n + s.tcoff;
			switch (s.tc) {
			case 1: uv[0] += (float)t[0] * mw[n]; uv[1] += (float)t[1] * mw[n]; break;
			case 2: uv[0] += (float)TLRdU16(t) * mw[n]; uv[1] += (float)TLRdU16(t + 2) * mw[n]; break;
			default: uv[0] += TLRdF(t) * mw[n]; uv[1] += TLRdF(t + 4) * mw[n]; break;
			}
		}
		const float k = s.tc == 1 ? 1.0f / 128.0f : (s.tc == 2 ? 1.0f / 32768.0f : 1.0f);
		u = uv[0] * k; v = uv[1] * k;
	}
	o.u = u; o.v = v;
	if (s.col) {
		float col[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (int n = 0; n < s.morph; ++n) {
			const float w = mw[n];
			const u8 *c = p + s.onesize * n + s.coloff;
			const u16 cd = TLRdU16(c);
			switch (s.col) {
			case 4:
				col[0] += w * (cd & 0x1f) * (255.0f / 31.0f);
				col[1] += w * ((cd >> 5) & 0x3f) * (255.0f / 63.0f);
				col[2] += w * ((cd >> 11) & 0x1f) * (255.0f / 31.0f);
				break;
			case 5:
				col[0] += w * (cd & 0x1f) * (255.0f / 31.0f);
				col[1] += w * ((cd >> 5) & 0x1f) * (255.0f / 31.0f);
				col[2] += w * ((cd >> 10) & 0x1f) * (255.0f / 31.0f);
				col[3] += w * ((cd >> 15) ? 255.0f : 0.0f);
				break;
			case 6:
				for (int j = 0; j < 4; ++j)
					col[j] += w * ((cd >> (j * 4)) & 0xF) * (255.0f / 15.0f);
				break;
			case 7:
				for (int j = 0; j < 4; ++j)
					col[j] += (float)c[j] * w;
				break;
			default:
				break;
			}
		}
		if (s.col >= 4 && s.col <= 7) {
			for (int j = 0; j < 4; ++j)
				o.c[j] = clamp_u8((int)col[j]);
			if (s.col == 4)
				o.c[3] = 255;
			else if (!(s.col == 7 ? (int)col[3] >= 255 : col[3] >= 255.0f))
				fullAlpha = false;
		} else {
			memset(o.c, 0, 4);
		}
	} else {
		const u32 c = s.matAmbient;
		o.c[0] = (u8)c; o.c[1] = (u8)(c >> 8); o.c[2] = (u8)(c >> 16); o.c[3] = (u8)(c >> 24);
	}
	if (s.nrm) {
		float f[3] = { 0.0f, 0.0f, 0.0f };
		for (int n = 0; n < s.morph; ++n) {
			const u8 *q = p + s.onesize * n + s.nrmoff;
			switch (s.nrm) {
			case 1: { const float m = mw[n] * (1.0f / 128.0f); for (int i = 0; i < 3; ++i) f[i] += (s8)q[i] * m; break; }
			case 2: { const float m = mw[n] * (1.0f / 32768.0f); for (int i = 0; i < 3; ++i) f[i] += (s16)TLRdU16(q + i * 2) * m; break; }
			default: for (int i = 0; i < 3; ++i) f[i] += TLRdF(q + i * 4) * mw[n]; break;
			}
		}
		if (s.wt) {
			lastNormal[0] = f[0] * skin[0] + f[1] * skin[3] + f[2] * skin[6];
			lastNormal[1] = f[0] * skin[1] + f[1] * skin[4] + f[2] * skin[7];
			lastNormal[2] = f[0] * skin[2] + f[1] * skin[5] + f[2] * skin[8];
		} else {
			lastNormal[0] = f[0]; lastNormal[1] = f[1]; lastNormal[2] = f[2];
		}
	}
	o.nx = lastNormal[0] * s.nsign; o.ny = lastNormal[1] * s.nsign; o.nz = lastNormal[2] * s.nsign;
	tlk_fix_normal(&o.nx, &o.ny, &o.nz);
	if (s.fixedNormal) {
		o.nx = 0.0f; o.ny = 0.0f; o.nz = 1.0f;
	}
	float f[3] = { 0.0f, 0.0f, 0.0f };
	for (int n = 0; n < s.morph; ++n) {
		const u8 *q = p + s.onesize * n + s.posoff;
		switch (s.pos) {
		case 1: for (int i = 0; i < 3; ++i) f[i] += (float)(s8)q[i] * ((1.0f / 128.0f) * mw[n]); break;
		case 2: for (int i = 0; i < 3; ++i) f[i] += (float)(s16)TLRdU16(q + i * 2) * ((1.0f / 32768.0f) * mw[n]); break;
		default: for (int i = 0; i < 3; ++i) f[i] += TLRdF(q + i * 4) * mw[n]; break;
		}
	}
	if (s.wt) {
		o.x = f[0] * skin[0] + f[1] * skin[3] + f[2] * skin[6] + skin[9];
		o.y = f[0] * skin[1] + f[1] * skin[4] + f[2] * skin[7] + skin[10];
		o.z = f[0] * skin[2] + f[1] * skin[5] + f[2] * skin[8] + skin[11];
	} else {
		o.x = f[0]; o.y = f[1]; o.z = f[2];
	}
}

static inline void DecodeTLVertex(const TLDecodeSetup &s, const u8 *p, Rasterizer::TLVertex &o, float lastNormal[3], bool &fullAlpha) {
	if (s.morph > 1) {
		DecodeTLVertexMorph(s, p, o, lastNormal, fullAlpha);
		return;
	}
	float skin[12];
	if (s.wt) {
		float w[8];
		const u8 *wd = p + s.woff;
		for (int j = 0; j < s.nw; ++j)
			w[j] = s.wt == 1 ? wd[j] * (1.0f / 128.0f) : (s.wt == 2 ? TLRdU16(wd + j * 2) * (1.0f / 32768.0f) : TLRdF(wd + j * 4));
		for (int i = 0; i < 12; ++i)
			skin[i] = 0.0f;
		for (int j = 0; j < s.nw; ++j) {
			if (w[j] != 0.0f) {
				const float *bone = &s.bones[j * 12];
				for (int i = 0; i < 12; ++i)
					skin[i] += w[j] * bone[i];
			}
		}
	}
	// UV (the decoder's prescale with 1/0, the GE scale is applied by the program).
	float u = 0.0f, v = 0.0f;
	if (s.readUV) {
		const u8 *t = p + s.tcoff;
		switch (s.tc) {
		case 1: u = t[0] * (1.0f / 128.0f); v = t[1] * (1.0f / 128.0f); break;
		case 2: u = TLRdU16(t) * (1.0f / 32768.0f); v = TLRdU16(t + 2) * (1.0f / 32768.0f); break;
		default: u = TLRdF(t); v = TLRdF(t + 4); break;
		}
	}
	o.u = u; o.v = v;
	// Color.
	if (s.col) {
		const u8 *c = p + s.coloff;
		u32 rgba;
		switch (s.col) {
		case 4: rgba = RGB565ToRGBA8888(TLRdU16(c)); break;
		case 5: rgba = RGBA5551ToRGBA8888(TLRdU16(c)); break;
		case 6: rgba = RGBA4444ToRGBA8888(TLRdU16(c)); break;
		case 7: rgba = (u32)c[0] | ((u32)c[1] << 8) | ((u32)c[2] << 16) | ((u32)c[3] << 24); break;
		default: rgba = 0; break;  // Invalid formats decode nothing (zeroed memory, as good as any).
		}
		if ((rgba >> 24) != 0xFF)
			fullAlpha = false;
		o.c[0] = (u8)rgba; o.c[1] = (u8)(rgba >> 8); o.c[2] = (u8)(rgba >> 16); o.c[3] = (u8)(rgba >> 24);
	} else {
		const u32 c = s.matAmbient;
		o.c[0] = (u8)c; o.c[1] = (u8)(c >> 8); o.c[2] = (u8)(c >> 16); o.c[3] = (u8)(c >> 24);
	}
	// Normal.
	if (s.nrm) {
		const u8 *n = p + s.nrmoff;
		float f[3];
		switch (s.nrm) {
		case 1: for (int i = 0; i < 3; ++i) f[i] = (s8)n[i] * (1.0f / 128.0f); break;
		case 2: for (int i = 0; i < 3; ++i) f[i] = (s16)TLRdU16(n + i * 2) * (1.0f / 32768.0f); break;
		default: for (int i = 0; i < 3; ++i) f[i] = TLRdF(n + i * 4); break;
		}
		if (s.wt) {
			lastNormal[0] = f[0] * skin[0] + f[1] * skin[3] + f[2] * skin[6];
			lastNormal[1] = f[0] * skin[1] + f[1] * skin[4] + f[2] * skin[7];
			lastNormal[2] = f[0] * skin[2] + f[1] * skin[5] + f[2] * skin[8];
		} else {
			lastNormal[0] = f[0]; lastNormal[1] = f[1]; lastNormal[2] = f[2];
		}
	}
	o.nx = lastNormal[0] * s.nsign; o.ny = lastNormal[1] * s.nsign; o.nz = lastNormal[2] * s.nsign;
	tlk_fix_normal(&o.nx, &o.ny, &o.nz);
	if (s.fixedNormal) {
		o.nx = 0.0f; o.ny = 0.0f; o.nz = 1.0f;
	}
	// Position.
	const u8 *q = p + s.posoff;
	float f[3];
	switch (s.pos) {
	case 1: for (int i = 0; i < 3; ++i) f[i] = (s8)q[i] * (1.0f / 128.0f); break;
	case 2: for (int i = 0; i < 3; ++i) f[i] = (s16)TLRdU16(q + i * 2) * (1.0f / 32768.0f); break;
	default:
		for (int i = 0; i < 3; ++i) {
			f[i] = TLRdF(q + i * 4);
			if (!s.wt && (std::isnan(f[i]) || std::isinf(f[i])))
				f[i] = 0.0f;
		}
		break;
	}
	if (s.wt) {
		o.x = f[0] * skin[0] + f[1] * skin[3] + f[2] * skin[6] + skin[9];
		o.y = f[0] * skin[1] + f[1] * skin[4] + f[2] * skin[7] + skin[10];
		o.z = f[0] * skin[2] + f[1] * skin[5] + f[2] * skin[8] + skin[11];
	} else {
		o.x = f[0]; o.y = f[1]; o.z = f[2];
	}
}

// Many vertices at once (no morphing), in three passes so the PPU never reloads into a float
// register what it has just stored from an integer one (a load-hit-store stall of ~40 cycles each,
// which DecodeTLVertex pays on every component). Pass 1 (integer unit): each vertex's components
// as 16 words (weights 0-7, u v, normal, position): little-endian floats byte-swapped, integers
// widened; colors straight into the output. Pass 2: words to floats (AltiVec where there is one).
// Pass 3 (FPU): skinning and the rest. Same results as DecodeTLVertex.
struct alignas(16) TLStage {
	int32_t w[16];
};

static inline void StageInts(int32_t *d, const u8 *p, int type, int count, bool isSigned) {
	for (int i = 0; i < count; ++i) {
		switch (type) {
		case 1: d[i] = isSigned ? (int32_t)(s8)p[i] : (int32_t)p[i]; break;
		case 2: { const u16 v = TLRdU16(p + i * 2); d[i] = isSigned ? (int32_t)(s16)v : (int32_t)v; break; }
		default: d[i] = (int32_t)((u32)p[i * 4] | ((u32)p[i * 4 + 1] << 8) | ((u32)p[i * 4 + 2] << 16) | ((u32)p[i * 4 + 3] << 24)); break;
		}
	}
}

// In pieces of TL_STAGE_VERTS vertices through a fixed buffer (same results: the passes are per
// vertex, and lastNormal/fullAlpha carry over in order). Until Frontend 93 the buffer was a vector
// that grew with the biggest draw so far, 64 bytes a vertex; on the PS3 that new big block was
// what failed once main memory was in small pieces (Silent Hill Origins, flashlight on).
static constexpr int TL_STAGE_VERTS = 512;
static void DecodeTLVerticesChunk(const TLDecodeSetup &s, const uint16_t *src, int n, Rasterizer::TLVertex *out, float lastNormal[3], bool &fullAlpha);
static void DecodeTLVertices(const TLDecodeSetup &s, const uint16_t *src, int n, Rasterizer::TLVertex *out, float lastNormal[3], bool &fullAlpha) {
	for (int base = 0; base < n; base += TL_STAGE_VERTS)
		DecodeTLVerticesChunk(s, src + base, std::min(TL_STAGE_VERTS, n - base), out + base, lastNormal, fullAlpha);
}

static void DecodeTLVerticesChunk(const TLDecodeSetup &s, const uint16_t *src, int n, Rasterizer::TLVertex *out, float lastNormal[3], bool &fullAlpha) {
	static TLStage stage[TL_STAGE_VERTS];
	// Pass 1.
	for (int m = 0; m < n; ++m) {
		const u8 *p = s.base + (size_t)src[m] * s.size;
		int32_t *d = stage[m].w;
		for (int i = 0; i < 16; ++i)
			d[i] = 0;
		if (s.wt)
			StageInts(d, p + s.woff, s.wt, s.nw, false);
		if (s.readUV)
			StageInts(d + 8, p + s.tcoff, s.tc, 2, false);
		if (s.nrm)
			StageInts(d + 10, p + s.nrmoff, s.nrm, 3, true);
		StageInts(d + 13, p + s.posoff, s.pos, 3, true);
		Rasterizer::TLVertex &o = out[m];
		u32 rgba;
		if (s.col) {
			const u8 *c = p + s.coloff;
			switch (s.col) {
			case 4: rgba = RGB565ToRGBA8888(TLRdU16(c)); break;
			case 5: rgba = RGBA5551ToRGBA8888(TLRdU16(c)); break;
			case 6: rgba = RGBA4444ToRGBA8888(TLRdU16(c)); break;
			case 7: rgba = (u32)c[0] | ((u32)c[1] << 8) | ((u32)c[2] << 16) | ((u32)c[3] << 24); break;
			default: rgba = 0; break;
			}
			if ((rgba >> 24) != 0xFF)
				fullAlpha = false;
		} else {
			rgba = s.matAmbient;
		}
		o.c[0] = (u8)rgba; o.c[1] = (u8)(rgba >> 8); o.c[2] = (u8)(rgba >> 16); o.c[3] = (u8)(rgba >> 24);
	}
	// Pass 2: per word, either float bits or an integer times 1/128 or 1/32768 (exact, like the
	// decoder's int-to-float then scale).
	alignas(16) int32_t isFloat[16];
	alignas(16) float scale[16];
	auto kind = [&](int first, int count, int type) {
		for (int i = first; i < first + count; ++i) {
			isFloat[i] = type == 3 ? -1 : 0;
			scale[i] = type == 1 ? 1.0f / 128.0f : (type == 2 ? 1.0f / 32768.0f : 0.0f);
		}
	};
	kind(0, 8, s.wt ? s.wt : 1);
	kind(8, 2, s.readUV ? s.tc : 1);
	kind(10, 3, s.nrm ? s.nrm : 1);
	kind(13, 3, s.pos);
#if defined(__ALTIVEC__)
	__vector float sc[4];
	__vector unsigned int fm[4];
	for (int k = 0; k < 4; ++k) {
		sc[k] = vec_ld(k * 16, scale);
		fm[k] = (__vector unsigned int)vec_ld(k * 16, isFloat);
	}
	// -0.0f as the addend keeps a product's sign (x * s + -0 == x * s, also for -0).
	const __vector float mz = (__vector float)vec_sl(vec_splat_u32(-1), vec_splat_u32(-1));
	for (int m = 0; m < n; ++m) {
		int32_t *d = stage[m].w;
		for (int k = 0; k < 4; ++k) {
			const __vector signed int v = vec_ld(k * 16, d);
			const __vector float asInt = vec_madd(vec_ctf(v, 0), sc[k], mz);
			vec_st((__vector signed int)vec_sel(asInt, (__vector float)v, fm[k]), k * 16, d);
		}
	}
#else
	for (int m = 0; m < n; ++m) {
		int32_t *d = stage[m].w;
		for (int i = 0; i < 16; ++i) {
			if (!isFloat[i]) {
				const float f = (float)d[i] * scale[i];
				memcpy(&d[i], &f, 4);
			}
		}
	}
#endif
	// Pass 3.
	for (int m = 0; m < n; ++m) {
		const float *f = (const float *)stage[m].w;
		Rasterizer::TLVertex &o = out[m];
		float skin[12]{};
		if (s.wt) {
			for (int j = 0; j < s.nw; ++j) {
				const float w = f[j];
				if (w != 0.0f) {
					const float *bone = &s.bones[j * 12];
					for (int i = 0; i < 12; ++i)
						skin[i] += w * bone[i];
				}
			}
		}
		o.u = f[8]; o.v = f[9];
		if (s.nrm) {
			if (s.wt) {
				lastNormal[0] = f[10] * skin[0] + f[11] * skin[3] + f[12] * skin[6];
				lastNormal[1] = f[10] * skin[1] + f[11] * skin[4] + f[12] * skin[7];
				lastNormal[2] = f[10] * skin[2] + f[11] * skin[5] + f[12] * skin[8];
			} else {
				lastNormal[0] = f[10]; lastNormal[1] = f[11]; lastNormal[2] = f[12];
			}
		}
		o.nx = lastNormal[0] * s.nsign; o.ny = lastNormal[1] * s.nsign; o.nz = lastNormal[2] * s.nsign;
		tlk_fix_normal(&o.nx, &o.ny, &o.nz);
		if (s.fixedNormal) {
			o.nx = 0.0f; o.ny = 0.0f; o.nz = 1.0f;
		}
	tlk_fix_normal(&o.nx, &o.ny, &o.nz);
		float x = f[13], y = f[14], z = f[15];
		if (s.wt) {
			o.x = x * skin[0] + y * skin[3] + z * skin[6] + skin[9];
			o.y = x * skin[1] + y * skin[4] + z * skin[7] + skin[10];
			o.z = x * skin[2] + y * skin[5] + z * skin[8] + skin[11];
		} else {
			if (s.pos == 3) {
				if (std::isnan(x) || std::isinf(x)) x = 0.0f;
				if (std::isnan(y) || std::isinf(y)) y = 0.0f;
				if (std::isnan(z) || std::isinf(z)) z = 0.0f;
			}
			o.x = x; o.y = y; o.z = z;
		}
	}
}

// A queued backend call, self-contained: everything it reads from PSP memory and GE state is copied
// in (vertices, indices, bones, morph weights, the CLUT, the GE registers, the state), so it can run
// later on the GE worker thread while the emulation thread carries on.
enum class GEJobType : uint8_t {
	TL_RAW,     // Direct T&L draw from copied PSP vertices (decoded on the worker).
	TL_VERTS,   // T&L draw with decoded vertices.
	INDEXED,
	TRIANGLES,
	TRIANGLE,
	RECT,
	LINE,
	POINT,
	FLUSH,
	PRIMS,      // Several TRIANGLE/RECT/LINE/POINT primitives of one draw (primType), vertices in pv.
	CPU_ROWS,   // TakeCpuRows: count rows, rowsWidth bytes each, rowsStride apart, from rowsAddr.
	XFER_RAM,   // QueueTransferToRam: as CPU_ROWS from VRAM, to xferDst (xferDstStride apart).
};

// A vector allocator whose resize leaves new elements uninitialized: job buffers are always
// written right after they grow, and zero-filling them first was a large part of queueing a draw.
template <class T>
struct NoInitAllocator : std::allocator<T> {
	template <class U> struct rebind { using other = NoInitAllocator<U>; };
	NoInitAllocator() = default;
	template <class U> NoInitAllocator(const NoInitAllocator<U> &) {}
	template <class U> void construct(U *p) { ::new ((void *)p) U; }
	template <class U, class... Args> void construct(U *p, Args &&...args) { ::new ((void *)p) U(std::forward<Args>(args)...); }
};
using RawBytes = std::vector<u8, NoInitAllocator<u8>>;
using IdxVec = std::vector<uint16_t, NoInitAllocator<uint16_t>>;

struct TLJob {
	GEJobType type = GEJobType::TL_RAW;
	Rasterizer::RasterizerState state;
	Rasterizer::TLParams params;
	GEState regs;                 // GE registers (cmdmem only) for the backend.
	TLDecodeSetup s;
	float bones[96];
	float morph[8];
	alignas(16) u8 clut[1024];
	RawBytes raw;                 // TL_RAW: vertices lower .. upper.
	const u8 *rawExt = nullptr;   // TL_RAW drawn at once (PrepareTLJob inPlace): the vertices in PSP memory, not copied.
	uint32_t geSerial = 0;        // TL_RAW: Rasterizer::g_geStateSerial its state matches.
	IdxVec idx;                   // TL_RAW: the draw's vertices (relative to lower), in GE order. Others: indices.
	std::vector<Rasterizer::TLVertex> tlv;
	std::vector<Rasterizer::FastVertex> fv;
	VertexData v[3];
	std::vector<VertexData> pv;   // PRIMS
	GEJobType primType = GEJobType::TRIANGLE;
	GEPrimitiveType prim = GE_PRIM_TRIANGLES;
	int cull = 0;
	int count = 0;
	bool flag = false;            // through / clear.
	const char *reason = nullptr;
	uint32_t prep = 0;            // TL_RAW: job number on the SPU (TLPrep), 0 if none.
	u32 vertexType = 0;           // TL_RAW: for merging the next draw into this job.
	uint32_t rawVerts = 0;        // TL_RAW: vertices in raw.
	bool prepTried = false;
	uint32_t rowsAddr = 0, rowsStride = 0, rowsWidth = 0;  // CPU_ROWS, XFER_RAM.
	uint32_t xferDst = 0, xferDstStride = 0;              // XFER_RAM.
};

// Hands a queued TL_RAW job to the SPU. False: no room right now (try again later).
static bool SubmitTLPrep(TLJob &job) {
	const TLDecodeSetup &s = job.s;
	// Lines (cull 3) are decoded on the worker: the SPU kernel builds triangle lists.
	if (s.morph > 1 || job.raw.empty() || job.idx.size() < 3 || job.cull == 3) {
		job.prepTried = true;
		return true;
	}
	TLPrep::Request r;
	r.raw = job.raw.data();
	r.rawBytes = (uint32_t)job.raw.size();
	r.vertexSize = (uint32_t)s.size;
	r.idx = job.idx.data();
	r.idxCount = (uint32_t)job.idx.size();
	r.prim = job.prim == GE_PRIM_TRIANGLES ? TLK_PRIM_TRIANGLES : (job.prim == GE_PRIM_TRIANGLE_STRIP ? TLK_PRIM_STRIP : TLK_PRIM_FAN);
	r.wt = s.wt; r.nw = s.nw; r.tc = s.tc; r.col = s.col; r.nrm = s.nrm; r.pos = s.pos; r.readUV = s.readUV ? 1 : 0;
	r.woff = s.woff; r.tcoff = s.tcoff; r.coloff = s.coloff; r.nrmoff = s.nrmoff; r.posoff = s.posoff;
	r.nsign = s.nsign;
	r.fixedNormal = s.fixedNormal ? 1 : 0;
	r.matAmbient = s.matAmbient;
	r.bones = job.bones;
	const uint32_t c = TLPrep::Submit(r);
	if (c == 0) {
		// Doesn't fit the SPU path at all, or no free slot: only the latter is worth a retry.
		const uint32_t rawCount = s.size ? (uint32_t)(job.raw.size() / s.size) : 0;
		if (rawCount == 0 || rawCount > TLK_MAX_RAW || ((uintptr_t)job.raw.data() & 15) != 0 || s.nw > 8 || !TLPrep::Available()) {
			job.prepTried = true;
			return true;
		}
		return false;
	}
	job.prep = c;
	job.prepTried = true;
	return true;
}

// The PPU decode of a TL_RAW job (the path without the SPU): its unique vertices into g_tlVerts and
// the triangle (or line) indices into g_tlIdx. False: more vertices than the backend draws.
static constexpr int TL_DECODE_MAX_VERTS = 20000;
static uint16_t g_tlIdx[3 * 65536];
static uint16_t g_tlSrcList[65536];
static Rasterizer::TLVertex g_tlVerts[TL_DECODE_MAX_VERTS];

static bool DecodeTLJobOnPPU(TLJob &job, int *nOut, int *nIdxOut) {
	TLDecodeSetup &s = job.s;
	s.base = job.rawExt ? job.rawExt : job.raw.data();
	s.bones = job.bones;
	s.morphW = job.morph;
	// Vertex number -> position in srcList, valid where remapGen matches this draw's generation (no
	// per-draw clear of the whole index range).
	static std::vector<uint32_t> remapGen(65536, 0);
	static std::vector<uint16_t> remap(65536);
	static uint32_t gen = 0;
	if (++gen == 0) {
		std::fill(remapGen.begin(), remapGen.end(), 0);
		gen = 1;
	}
	// Fixed buffers, not vectors grown to the biggest draw so far (Frontend 93: on the PS3 such a
	// new block failed once main memory was in small pieces). A draw has at most 65536 different
	// vertices and 3 indices per vertex; the backend takes at most TL_DECODE_MAX_VERTS vertices.
	uint16_t *tlIdx = g_tlIdx;
	uint16_t *srcList = g_tlSrcList;
	Rasterizer::TLVertex *tlVerts = g_tlVerts;
	int nIdx = 0, nSrc = 0;
	const uint16_t *in = job.idx.data();
	auto vtx = [&](int i) {
		const uint16_t d = in[i];
		if (remapGen[d] != gen) {
			remapGen[d] = gen;
			remap[d] = (uint16_t)nSrc;
			srcList[nSrc++] = d;
		}
		tlIdx[nIdx++] = remap[d];
	};
	const int vertexCount = std::min((int)job.idx.size(), 65536);
	switch (job.prim) {
	case GE_PRIM_TRIANGLES:
		for (int i = 0; i + 2 < vertexCount; i += 3) { vtx(i); vtx(i + 1); vtx(i + 2); }
		break;
	case GE_PRIM_TRIANGLE_STRIP:
		for (int i = 0; i + 2 < vertexCount; ++i) {
			if (i & 1) { vtx(i + 1); vtx(i); vtx(i + 2); }
			else { vtx(i); vtx(i + 1); vtx(i + 2); }
		}
		break;
	case GE_PRIM_LINES:
		for (int i = 0; i + 1 < vertexCount; i += 2) { vtx(i); vtx(i + 1); }
		break;
	case GE_PRIM_LINE_STRIP:
		for (int i = 0; i + 1 < vertexCount; ++i) { vtx(i); vtx(i + 1); }
		break;
	default:
		for (int i = 2; i < vertexCount; ++i) { vtx(0); vtx(i - 1); vtx(i); }
		break;
	}
	const int n = nSrc;
	if (n > TL_DECODE_MAX_VERTS)
		return false;  // More than the backend draws (DrawTL refuses it).
	{
		PS3_PROFILE(decode);
		static float lastNormal[3] = { 0.0f, 0.0f, 1.0f };
		bool fullAlpha = true;
		if (s.morph > 1) {
			for (int m = 0; m < n; ++m)
				DecodeTLVertex(s, s.base + (size_t)srcList[m] * s.size, tlVerts[m], lastNormal, fullAlpha);
		} else {
			DecodeTLVertices(s, srcList, n, tlVerts, lastNormal, fullAlpha);
		}
	}
	*nOut = n;
	*nIdxOut = nIdx;
	return true;
}

// SPU vertex check (diag builds, Rasterizer::g_spuVertexCheck): each draw whose vertices the SPUs
// prepared is decoded on the PPU too and compared; when they differ, the PPU's result is drawn and
// the difference goes into a report the frontend writes to launch.log.
namespace Rasterizer {
bool g_spuVertexCheck = false;
}
static std::mutex g_spuCheckMutex;
static std::string g_spuCheckReport;
static uint64_t g_spuChecked = 0, g_spuDiffer = 0;
static uint64_t g_spuFieldCount[6] = {};
static int g_spuDetails = 0;
static uint64_t g_spuReportedChecked = 0;

void Rasterizer::TakeSpuVertexCheckReport(std::string &out) {
	std::lock_guard<std::mutex> lock(g_spuCheckMutex);
	if (g_spuChecked == g_spuReportedChecked && g_spuCheckReport.empty())
		return;
	out.swap(g_spuCheckReport);
	g_spuCheckReport.clear();
	char line[320];
	snprintf(line, sizeof(line), "SPU vertex check: %llu draws checked, %llu differ (vertex/index count %llu, position %llu, normal %llu, color %llu, uv %llu, indices %llu); those were drawn with the PPU's vertices\n",
		(unsigned long long)g_spuChecked, (unsigned long long)g_spuDiffer, (unsigned long long)g_spuFieldCount[0], (unsigned long long)g_spuFieldCount[1],
		(unsigned long long)g_spuFieldCount[2], (unsigned long long)g_spuFieldCount[3], (unsigned long long)g_spuFieldCount[4], (unsigned long long)g_spuFieldCount[5]);
	out += line;
	g_spuReportedChecked = g_spuChecked;
}

// True: the SPU's vertices match the PPU's (g_tlVerts/g_tlIdx, n/nIdx).
static bool CheckSpuVertices(const TLJob &job, uint32_t prep, const Rasterizer::TLVertex *sv, int sn, const uint16_t *si, int sni, int n, int nIdx) {
	auto far = [](float a, float b) {
		if (a == b) return false;
		if (a != a || b != b) return true;  // NaN.
		return fabsf(a - b) > 1e-3f * std::max(1.0f, fabsf(a));
	};
	int fields = 0, bad = 0, first = -1;
	if (sn != n || sni != nIdx) {
		fields |= 1;
	} else {
		if (memcmp(si, g_tlIdx, (size_t)nIdx * sizeof(uint16_t)) != 0)
			fields |= 32;
		for (int i = 0; i < n; ++i) {
			const Rasterizer::TLVertex &a = g_tlVerts[i], &b = sv[i];
			int f = 0;
			if (far(a.x, b.x) || far(a.y, b.y) || far(a.z, b.z)) f |= 2;
			if (far(a.nx, b.nx) || far(a.ny, b.ny) || far(a.nz, b.nz)) f |= 4;
			if (memcmp(a.c, b.c, 4) != 0) f |= 8;
			if (far(a.u, b.u) || far(a.v, b.v)) f |= 16;
			if (f) {
				fields |= f;
				bad++;
				if (first < 0) first = i;
			}
		}
	}
	std::lock_guard<std::mutex> lock(g_spuCheckMutex);
	g_spuChecked++;
	if (!fields)
		return true;
	g_spuDiffer++;
	for (int k = 0; k < 6; ++k)
		if (fields & (1 << k)) g_spuFieldCount[k]++;
	if (g_spuDetails < 48) {
		g_spuDetails++;
		const TLDecodeSetup &s = job.s;
		const uint8_t *raw = job.rawExt ? job.rawExt : job.raw.data();
		char line[1024];
		int len = snprintf(line, sizeof(line), "SPU vertex check: job %u (draw %llu) differs: fields %02x (1 counts, 2 pos, 4 normal, 8 color, 16 uv, 32 indices), %d of %d vertices; "
			"vertices %d/%d indices %d/%d (SPU/PPU); format: size %d wt %d nw %d tc %d col %d nrm %d pos %d uv %d morph %d nsign %g matAmbient %08x; "
			"prim %d cull %d, %u raw bytes (%u vertices) at %p, %d GE indices, lighting %d",
			prep, (unsigned long long)g_spuChecked, fields, bad, n, sn, n, sni, nIdx, s.size, s.wt, s.nw, s.tc, s.col, s.nrm, s.pos, s.readUV ? 1 : 0, s.morph,
			s.nsign, s.matAmbient, (int)job.prim, job.cull, (unsigned)job.raw.size(), job.rawVerts, (const void *)raw, (int)job.idx.size(),
			job.params.c[8][3] != 0.0f ? 1 : 0);
		if (first >= 0 && len > 0 && len < (int)sizeof(line)) {
			const Rasterizer::TLVertex &a = g_tlVerts[first], &b = sv[first];
			uint32_t ca, cb;
			memcpy(&ca, a.c, 4);
			memcpy(&cb, b.c, 4);
			snprintf(line + len, sizeof(line) - len, "; vertex %d SPU/PPU: pos %g,%g,%g / %g,%g,%g normal %g,%g,%g / %g,%g,%g color %08x / %08x uv %g,%g / %g,%g",
				first, b.x, b.y, b.z, a.x, a.y, a.z, b.nx, b.ny, b.nz, a.nx, a.ny, a.nz, cb, ca, b.u, b.v, a.u, a.v);
		}
		g_spuCheckReport += line;
		g_spuCheckReport += '\n';
	}
	return false;
}

static void ExecuteTLJob(TLJob &job, Rasterizer::HardwareBackend *backend) {
#if PPSSPP_ARCH(POWERPC)
	const uint64_t start = PS3Profile::Now();
#endif
	if (job.prep) {
		// Prepared on the SPU while earlier jobs were drawn.
		const void *verts = nullptr;
		const uint16_t *idx = nullptr;
		int nVerts = 0, nIdx = 0;
		bool ok;
		{
			PS3_PROFILE(decode);
			ok = TLPrep::Wait(job.prep, &verts, &nVerts, &idx, &nIdx);
		}
		// Build 269 diag: one draw in 8 (checking every one made the game slower than SPU vertices off).
		static uint32_t checkTurn = 0;
		if (ok && Rasterizer::g_spuVertexCheck && (++checkTurn & 7) == 0) {
			int n = 0, pn = 0;
			if (DecodeTLJobOnPPU(job, &n, &pn) &&
				!CheckSpuVertices(job, job.prep, (const Rasterizer::TLVertex *)verts, nVerts, idx, nIdx, n, pn)) {
				// Draw the PPU's vertices instead.
				verts = g_tlVerts;
				idx = g_tlIdx;
				nVerts = n;
				nIdx = pn;
			}
		}
		if (ok) {
#if PPSSPP_ARCH(POWERPC)
			PS3Profile::batch += PS3Profile::Now() - start;
			PS3Profile::directTL++;
#endif
			if (backend && nIdx > 0)
				backend->DrawTL(job.state, job.params, (const Rasterizer::TLVertex *)verts, nVerts, idx, nIdx, job.cull);
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
			else if (!backend) {
				extern void TLJobTestCapture(const Rasterizer::TLVertex *verts, int n, const uint16_t *indices, int nidx);
				TLJobTestCapture((const Rasterizer::TLVertex *)verts, nVerts, idx, nIdx);
			}
#endif
			TLPrep::Finish(job.prep);
			return;
		}
		TLPrep::Finish(job.prep);
	}
	int n = 0, nIdx = 0;
	if (!DecodeTLJobOnPPU(job, &n, &nIdx))
		return;
#if PPSSPP_ARCH(POWERPC)
	PS3Profile::batch += PS3Profile::Now() - start;
	PS3Profile::directTL++;
#endif
	if (backend)
		backend->DrawTL(job.state, job.params, g_tlVerts, n, g_tlIdx, nIdx, job.cull);
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	else {
		// Test build: hand the result to the job test.
		extern void TLJobTestCapture(const Rasterizer::TLVertex *verts, int n, const uint16_t *indices, int nidx);
		TLJobTestCapture(g_tlVerts, n, g_tlIdx, nIdx);
	}
#endif
}

static void RunJob(TLJob &job, Rasterizer::HardwareBackend *r) {
	switch (job.type) {
	case GEJobType::TL_RAW: ExecuteTLJob(job, r); break;
	case GEJobType::TL_VERTS: r->DrawTL(job.state, job.params, job.tlv.data(), (int)job.tlv.size(), job.idx.data(), (int)job.idx.size(), job.cull); break;
	case GEJobType::INDEXED: r->DrawIndexed(job.state, job.fv.data(), (int)job.fv.size(), job.idx.data(), (int)job.idx.size(), job.flag); break;
	case GEJobType::TRIANGLES: r->DrawTriangles(job.state, job.fv.data(), (int)job.fv.size(), job.flag); break;
	case GEJobType::TRIANGLE: r->Triangle(job.state, job.v[0], job.v[1], job.v[2]); break;
	case GEJobType::RECT: r->Rect(job.state, job.v[0], job.v[1], job.flag); break;
	case GEJobType::LINE: r->Line(job.state, job.v[0], job.v[1]); break;
	case GEJobType::POINT: r->Point(job.state, job.v[0]); break;
	case GEJobType::FLUSH: r->Flush(job.reason); break;
	case GEJobType::CPU_ROWS: r->TakeCpuRows(job.rowsAddr, job.rowsStride, job.rowsWidth, (uint32_t)job.count); break;
	case GEJobType::XFER_RAM: {
		// What the hardware drew there first, then the copy (both checked valid when queued).
		r->WriteBackRange(job.rowsAddr, job.rowsStride, job.rowsWidth, (uint32_t)job.count);
		const u8 *src = Memory::GetPointerUnchecked(job.rowsAddr);
		u8 *dst = Memory::GetPointerWriteUnchecked(job.xferDst);
		for (int y = 0; y < job.count; ++y)
			memcpy(dst + (size_t)y * job.xferDstStride, src + (size_t)y * job.rowsStride, job.rowsWidth);
		break;
	}
	case GEJobType::PRIMS: {
		const VertexData *v = job.pv.data();
		const size_t n = job.pv.size();
		switch (job.primType) {
		case GEJobType::TRIANGLE: for (size_t i = 0; i + 2 < n; i += 3) r->Triangle(job.state, v[i], v[i + 1], v[i + 2]); break;
		case GEJobType::RECT: for (size_t i = 0; i + 1 < n; i += 2) r->Rect(job.state, v[i], v[i + 1], job.flag); break;
		case GEJobType::LINE: for (size_t i = 0; i + 1 < n; i += 2) r->Line(job.state, v[i], v[i + 1]); break;
		case GEJobType::POINT: for (size_t i = 0; i < n; ++i) r->Point(job.state, v[i]); break;
		default: break;
		}
		break;
	}
	}
}

// Memory the GE worker's job slots hold (read by the PS3 frontend for launch.log), and a request to
// give it all back (the frontend sets 64 when main memory runs low).
std::atomic<uint32_t> g_geJobBufferBytes{0};
std::atomic<int> g_geTrimJobs{0};
// Set by the frontend once main memory has run low: merged T&L jobs stay small, so their buffers
// never need a big free block (Silent Hill Origins ran out of memory growing one to ~1 MB while
// 12 MB were free in small pieces).
std::atomic<bool> g_geLowMemory{false};
static inline size_t MergeRawLimit() { return g_geLowMemory.load(std::memory_order_relaxed) ? 64 * 1024 : 512 * 1024; }

// Vertex copies into T&L jobs. The PS3's memcpy (newlib) copies byte by byte unless both addresses
// are 8-byte aligned - and PSP vertices (20, 36 bytes...) at their offsets almost never are: ~430
// MB/s, 2-3 MB a frame in Silent Hill Shattered Memories' slow parts (8-11 ms of merging, ~2.5 ms of
// new jobs a frame, build 255 log). With AltiVec: 16 bytes at a time, aligned stores, the source
// realigned with vec_perm; only the bytes in [src, src + n) are read (whole aligned 16-byte blocks
// that hold some of them).
static inline void CopyVerts(u8 *dst, const u8 *src, size_t n) {
#if defined(__ALTIVEC__)
	if (n >= 64) {
		const size_t head = (16 - ((uintptr_t)dst & 15)) & 15;
		for (size_t i = 0; i < head; ++i)
			dst[i] = src[i];
		dst += head;
		src += head;
		n -= head;
		const size_t blocks = n >> 4;
		if (((uintptr_t)src & 15) == 0) {
			for (size_t i = 0; i < blocks; ++i) {
				if ((i & 7) == 0)
					__asm__ volatile("dcbt 0,%0" : : "r"(src + 512));
				vec_st(vec_ld(0, src), 0, dst);
				src += 16;
				dst += 16;
			}
		} else {
			// Misaligned: block i is the end of aligned block k and the start of k + 1 (both hold
			// bytes of [src, src + 16), as src isn't on a boundary).
			const __vector unsigned char perm = vec_lvsl(0, src);
			__vector unsigned char lo = vec_ld(0, src);
			for (size_t i = 0; i < blocks; ++i) {
				if ((i & 7) == 0)
					__asm__ volatile("dcbt 0,%0" : : "r"(src + 512));
				const __vector unsigned char hi = vec_ld(16, src);
				vec_st(vec_perm(lo, hi, perm), 0, dst);
				lo = hi;
				src += 16;
				dst += 16;
			}
		}
		n &= 15;
		for (size_t i = 0; i < n; ++i)
			dst[i] = src[i];
		return;
	}
#endif
	memcpy(dst, src, n);
}

// For the frontend's stall report: the job type the GE worker runs (-1: none) and since when (time
// base), and whether the emulation thread is waiting for it.
std::atomic<int> g_geWorkerJob{-1};
std::atomic<uint64_t> g_geWorkerJobStart{0};
std::atomic<int> g_geProducerWaiting{0};

// The GE worker: runs jobs in order on its own thread. The emulation thread waits for it (Drain)
// only where it must: before the backend writes rendered pixels back to PSP memory, and at the
// PSP's GE sync points.
class TLWorker {
public:
	explicit TLWorker(Rasterizer::HardwareBackend *real) : real_(real) {
		thread_ = std::thread([this] { Loop(); });
	}
	~TLWorker() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			quit_ = true;
		}
		cond_.notify_all();
		thread_.join();
	}
	// A free job to fill (waits while the queue is full).
	TLJob &Acquire() {
		SubmitOpen();
		std::unique_lock<std::mutex> lock(mutex_);
		if (count_ == RING) {
#if PPSSPP_ARCH(POWERPC)
			const uint64_t t0 = PS3Profile::Now();
#endif
			producerWaiting_ = true;
			g_geProducerWaiting = 1;
			done_.wait(lock, [this] { return count_ < RING; });
			g_geProducerWaiting = 0;
			producerWaiting_ = false;
#if PPSSPP_ARCH(POWERPC)
			PS3Profile::geWait += PS3Profile::Now() - t0;
			PS3Profile::geWaitFull++;
#endif
		}
		return ring_[head_];
	}
	// Signals the worker only when it is asleep: a condition variable signal is a system call on
	// the PS3 even with nobody waiting, and draws come by the thousand per frame.
	void Submit() {
		bool wake;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			head_ = (head_ + 1) % RING;
			count_++;
			pending_ = true;
			wake = workerWaiting_;
		}
		if (wake)
			cond_.notify_one();
	}
	// A PRIMS job still being filled (acquired, not yet submitted): primitives of the same draw are
	// added to it instead of each becoming a job of its own (Dead or Alive Paradise draws ~10,000
	// lines a frame, each one used to copy the whole rasterizer state and GE registers).
	TLJob *OpenPrims(uint64_t key, const void *state) const { return open_ && !openTL_ && openKey_ == key && openState_ == state ? open_ : nullptr; }
	void SetOpen(TLJob *job, uint64_t key, const void *state) { open_ = job; openKey_ = key; openState_ = state; openTL_ = false; }
	// A T&L job later draws with the same state may be added to (MergeTLJob).
	TLJob *OpenTL() const { return open_ && openTL_ ? open_ : nullptr; }
	void SetOpenTL(TLJob *job) { open_ = job; openTL_ = true; }
	void SubmitOpen() {
		if (open_) {
			open_ = nullptr;
			Submit();
		}
	}

	// Waits until every submitted job has run.
	void Drain() {
		SubmitOpen();
		std::unique_lock<std::mutex> lock(mutex_);
		if (count_ == 0)
			return;
#if PPSSPP_ARCH(POWERPC)
		const uint64_t t0 = PS3Profile::Now();
#endif
		// Report a worker that seems stuck (once per wait), with the job it's on.
		bool reported = false;
		producerWaiting_ = true;
		while (!done_.wait_for(lock, std::chrono::seconds(2), [this] { return count_ == 0; })) {
			if (!reported) {
				reported = true;
				ERROR_LOG(Log::G3D, "GE worker: still busy after 2 s, %d jobs queued, running a job of type %d", count_, (int)ring_[tail_].type);
#if PPSSPP_PLATFORM(PS3)
				extern void PS3GEWorkerStuck(int queued, int type);
				PS3GEWorkerStuck(count_, (int)ring_[tail_].type);
#endif
			}
		}
		producerWaiting_ = false;
#if PPSSPP_ARCH(POWERPC)
		PS3Profile::geWait += PS3Profile::Now() - t0;
		PS3Profile::geDrains++;
#endif
	}
	Rasterizer::HardwareBackend *Real() const { return real_; }

private:
	void Loop() {
#if PPSSPP_PLATFORM(PS3)
		extern void PS3GEWorkerStarted();
		PS3GEWorkerStarted();
#endif
		std::unique_lock<std::mutex> lock(mutex_);
		while (true) {
			if (!quit_ && count_ == 0) {
				workerWaiting_ = true;
				cond_.wait(lock, [this] { return quit_ || count_ > 0; });
				workerWaiting_ = false;
			}
			if (quit_)
				break;
			// Start the SPU on the T&L jobs queued behind this one (in order: it runs them in order).
			for (int k = 0, i = tail_; k < count_ && k < 12; ++k, i = (i + 1) % RING) {
				TLJob &ahead = ring_[i];
				if (ahead.type != GEJobType::TL_RAW || ahead.prepTried)
					continue;
				if (!SubmitTLPrep(ahead))
					break;
			}
			TLJob &job = ring_[tail_];
			lock.unlock();
#if PPSSPP_ARCH(POWERPC)
			const uint64_t t0 = PS3Profile::Now();
#endif
			Rasterizer::g_drawStateOverride = &job.regs;
#if PPSSPP_ARCH(POWERPC)
			g_geWorkerJobStart.store(PS3Profile::Now(), std::memory_order_relaxed);
#endif
			g_geWorkerJob.store((int)job.type, std::memory_order_relaxed);
			RunJob(job, real_);
			g_geWorkerJob.store(-1, std::memory_order_relaxed);
			Rasterizer::g_drawStateOverride = nullptr;
			job.prep = 0;
			job.prepTried = false;
			// The 64 slots keep their buffers for the next job, but not big ones: each slot's
			// buffers grew to the largest draw it ever had and stayed (tens of MB after a while on
			// the PS3, where ~25 MB of main memory is left; Dead to Rights ran out and froze).
			// When main memory runs low (g_geTrimJobs, set by the frontend), the next 64 jobs give
			// their buffers back entirely, which empties every slot.
			const bool trim = g_geTrimJobs.load(std::memory_order_relaxed) > 0;
			if (trim)
				g_geTrimJobs.fetch_sub(1, std::memory_order_relaxed);
			if (job.raw.capacity() > (trim ? 0u : 48u * 1024))
				RawBytes().swap(job.raw);
			if (job.idx.capacity() > (trim ? 0u : 16u * 1024))
				IdxVec().swap(job.idx);
			if (job.tlv.capacity() > (trim ? 0u : 1024u))
				std::vector<Rasterizer::TLVertex>().swap(job.tlv);
			if (job.fv.capacity() > (trim ? 0u : 1024u))
				std::vector<Rasterizer::FastVertex>().swap(job.fv);
			if (job.pv.capacity() > (trim ? 0u : 1024u))
				std::vector<VertexData>().swap(job.pv);
			// Now and then, what the slots hold (for launch.log).
			if ((++jobsRun_ & 255) == 0 || trim) {
				size_t bytes = 0;
				for (int k = 0; k < RING; ++k) {
					const TLJob &j = ring_[k];
					bytes += j.raw.capacity() + j.idx.capacity() * sizeof(uint16_t) + j.tlv.capacity() * sizeof(Rasterizer::TLVertex) +
						j.fv.capacity() * sizeof(Rasterizer::FastVertex) + j.pv.capacity() * sizeof(VertexData);
				}
				g_geJobBufferBytes.store((uint32_t)bytes, std::memory_order_relaxed);
			}
#if PPSSPP_ARCH(POWERPC)
			{
				const uint64_t dt = PS3Profile::Now() - t0;
				PS3Profile::geWorker += dt;
				if (dt > 79800 / 2) {  // > 0.5 ms
					const int ty = std::min((int)job.type, 15);
					PS3Profile::geSlowJobs[ty]++;
					PS3Profile::geSlowJobTicks[ty] += dt;
				}
			}
#endif
			lock.lock();
			// The slot is reused only after this, so the emulation thread never writes a job the
			// worker is still running.
			tail_ = (tail_ + 1) % RING;
			count_--;
			if (producerWaiting_)
				done_.notify_all();
		}
	}

	// Small: memory is tight on the PS3 (each job keeps its buffers for reuse). 96 from build 153
	// (64 before): Midnight Club's emulation thread waited ~2-4 ms a frame for a free slot.
	static constexpr int RING = 96;
	uint32_t jobsRun_ = 0;
	Rasterizer::HardwareBackend *real_;
	TLJob ring_[RING];
	int head_ = 0, tail_ = 0, count_ = 0;
	bool quit_ = false;
	bool pending_ = false;
	// Under mutex_: who is blocked on a condition variable (only they need a signal).
	bool workerWaiting_ = false;
	bool producerWaiting_ = false;
	TLJob *open_ = nullptr;  // Emulation thread only.
	uint64_t openKey_ = 0;
	const void *openState_ = nullptr;
	bool openTL_ = false;
	std::mutex mutex_;
	std::condition_variable cond_, done_;
	std::thread thread_;
};

static TLWorker *g_tlWorker = nullptr;

// The parts of a job every kind shares: rasterizer state with its CLUT, and the GE registers.
static void CaptureJobState(TLJob &job, const Rasterizer::RasterizerState &state) {
	// Byte for byte (padding included): the RSX renderer compares states with memcmp.
	memcpy((void *)&job.state, (const void *)&state, sizeof(job.state));
	if (state.enableTextures && state.samplerID.cached.clut) {
		// Only what the texture can reach (Midnight Club: thousands of 16-entry palettes a frame).
		memcpy(job.clut, state.samplerID.cached.clut, ClutBytesUsed(state.samplerID));
		job.state.samplerID.cached.clut = job.clut;
	}
	memcpy(job.regs.cmdmem, gstate.cmdmem, sizeof(job.regs.cmdmem));
}

// Wraps the real backend while the worker runs. Drawing is queued. Questions about pixels not yet
// written back are answered from a record of the buffers queued drawing writes to (a superset of
// what the backend would say), so they never wait. Only a flush that writes pixels back to PSP
// memory, and Sync, wait for the worker.
class TLWorkerBackend : public Rasterizer::HardwareBackend {
public:
	explicit TLWorkerBackend(TLWorker *worker) : w_(worker), r_(worker->Real()) {}
	bool SupportsTL() const override { return r_->SupportsTL(); }
	bool DrawTL(const Rasterizer::RasterizerState &state, const Rasterizer::TLParams &p, const Rasterizer::TLVertex *verts, int nverts, const uint16_t *indices, int nidx, int cull) override {
		if (nverts > 20000)
			return false;
		TLJob &job = Begin(GEJobType::TL_VERTS, state);
		job.params = p;
		job.tlv.assign(verts, verts + nverts);
		job.idx.assign(indices, indices + nidx);
		job.cull = cull;
		w_->Submit();
		return true;
	}
	// Inside one buffer drawn since the last write-back: the worker's renderer holds it, so queued
	// draws that read it as a texture read its copy (see HardwareBackend::CanTakeCpuRows).
	bool CanTakeCpuRows(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) override {
		if ((start & 0x0F800000) != 0x04000000 || !r_->CanTakeCpuRows(start, stride, w, h))
			return false;
		const uint32_t s0 = start & 0x1FFFFF;
		const uint32_t s1 = s0 + stride * (h ? h - 1 : 0) + w;
		for (const auto &t : targets_) {
			if (s0 >= t.first && s1 <= t.second)
				return true;
		}
		return false;
	}
	bool QueueTransferToRam(uint32_t src, uint32_t srcStride, uint32_t dst, uint32_t dstStride, uint32_t w, uint32_t h) override {
		TLJob &job = w_->Acquire();
		job.type = GEJobType::XFER_RAM;
		job.rowsAddr = src;
		job.rowsStride = srcStride;
		job.rowsWidth = w;
		job.count = (int)h;
		job.xferDst = dst;
		job.xferDstStride = dstStride;
		w_->Submit();
		return true;
	}
	void TakeCpuRows(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) override {
		TLJob &job = w_->Acquire();
		job.type = GEJobType::CPU_ROWS;
		job.rowsAddr = start;
		job.rowsStride = stride;
		job.rowsWidth = w;
		job.count = (int)h;
		w_->Submit();
	}
	bool DrawTriangles(const Rasterizer::RasterizerState &state, const Rasterizer::FastVertex *verts, int count, bool through) override {
		TLJob &job = Begin(GEJobType::TRIANGLES, state);
		job.fv.assign(verts, verts + count);
		job.flag = through;
		w_->Submit();
		return true;
	}
	bool DrawIndexed(const Rasterizer::RasterizerState &state, const Rasterizer::FastVertex *verts, int nverts, const uint16_t *indices, int nidx, bool through) override {
		if (nverts > 20000)
			return false;
		TLJob &job = Begin(GEJobType::INDEXED, state);
		job.fv.assign(verts, verts + nverts);
		job.idx.assign(indices, indices + nidx);
		job.flag = through;
		w_->Submit();
		return true;
	}
	void Triangle(const Rasterizer::RasterizerState &state, const VertexData &v0, const VertexData &v1, const VertexData &v2) override {
		TLJob &job = OpenPrims(GEJobType::TRIANGLE, state, false);
		job.pv.push_back(v0); job.pv.push_back(v1); job.pv.push_back(v2);
		ClosePrimsIfFull(job);
	}
	void Rect(const Rasterizer::RasterizerState &state, const VertexData &v0, const VertexData &v1, bool clear) override {
		TLJob &job = OpenPrims(GEJobType::RECT, state, clear);
		job.pv.push_back(v0); job.pv.push_back(v1);
		ClosePrimsIfFull(job);
	}
	void Line(const Rasterizer::RasterizerState &state, const VertexData &v0, const VertexData &v1) override {
		TLJob &job = OpenPrims(GEJobType::LINE, state, false);
		job.pv.push_back(v0); job.pv.push_back(v1);
		ClosePrimsIfFull(job);
	}
	void Point(const Rasterizer::RasterizerState &state, const VertexData &v0) override {
		TLJob &job = OpenPrims(GEJobType::POINT, state, false);
		job.pv.push_back(v0);
		ClosePrimsIfFull(job);
	}
	void Flush(const char *reason) override {
#if !PPSSPP_PLATFORM(PS3)
		static const bool flushLog = getenv("PPSSPP_FLUSHLOG") != nullptr;
		if (flushLog) fprintf(stderr, "WFLUSH %s\n", reason);
#endif
		// The RSX renderer only writes pixels back for these (it samples render targets itself);
		// the rest just end its current batch, which can wait in the queue.
		static const char *const readers[] = {"blockxfer", "blockxfer_wrap", "loadclut", "debug", "output"};
		// Called with other reasons thousands of times a frame (Gran Turismo): first letters first.
		const char c0 = reason[0];
		if (c0 != 'b' && c0 != 'l' && c0 != 'd' && c0 != 'o')
			return;
		for (const char *rd : readers) {
			if (strcmp(reason, rd) == 0) {
				PS3_PROFILE(geReaderFlush);
#if PPSSPP_ARCH(POWERPC)
				PS3Profile::geReaderFlushes++;
#endif
				w_->Drain();
				Rasterizer::ClearAsyncRamWrites();
				r_->Flush(reason);
				const Rasterizer::FlushRange range = Rasterizer::g_flushRange;
				if (strcmp(reason, "blockxfer") == 0 && range.end > range.start) {
					// The renderer wrote back only the buffers over the transfer's memory: the
					// others are still drawn-only, and later reads of them must still flush.
					if ((range.start & 0x0F800000) == 0x04000000) {
						const uint32_t s0 = range.start & 0x1FFFFF;
						const uint32_t s1 = s0 + (range.end - range.start);
						for (size_t i = 0; i < targets_.size();) {
							if (s0 < targets_[i].second && s1 > targets_[i].first) {
								targets_[i] = targets_.back();
								targets_.pop_back();
							} else {
								++i;
							}
						}
					}
				} else {
					targets_.clear();
				}
				textures_.clear();
				texturesAll_ = false;
				return;
			}
		}
		// The rest would only end the backend's current batch, which it does itself when anything
		// changes; queueing them also cut off draws that could have been merged (a list ends after
		// every few draws in some games).
	}
	bool HasPendingWrite(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) override {
		// VRAM only (mirrors folded); render targets are always there.
		if ((start & 0x0F800000) != 0x04000000)
			return false;
		const uint32_t s0 = start & 0x1FFFFF;
		const uint32_t s1 = s0 + stride * (h ? h - 1 : 0) + w;
		for (const auto &t : targets_) {
			if (s0 < t.second && s1 > t.first)
				return true;
		}
		return false;
	}
	void Sync() override {
		PS3_PROFILE(geSyncT);
#if PPSSPP_ARCH(POWERPC)
		PS3Profile::geSyncs++;
#endif
		w_->Drain();
		Rasterizer::ClearAsyncRamWrites();
		r_->Sync();
		textures_.clear();
		texturesAll_ = false;
	}
	bool MayRead(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) override {
		if (texturesAll_)
			return true;
		const uint32_t s0 = Rasterizer::GeNormalizeAddr(start);
		const uint32_t s1 = s0 + stride * (h ? h - 1 : 0) + w;
		for (const auto &t : textures_) {
			if (s0 < t.second && s1 > t.first)
				return true;
		}
		return false;
	}
	void NoteCurrentTargets(const Rasterizer::RasterizerState &state) {
		NoteTargets();
		NoteTexture(state);
	}

private:
	// The open PRIMS job for this draw (same BinManager draw, state, primitive kind and flag), or a
	// new one.
	TLJob &OpenPrims(GEJobType type, const Rasterizer::RasterizerState &state, bool flag) {
		const uint64_t key = ((uint64_t)Rasterizer::g_drawSerial << 32) | ((uint64_t)type << 1) | (flag ? 1 : 0);
		if (TLJob *open = w_->OpenPrims(key, &state))
			return *open;
		TLJob &job = Begin(GEJobType::PRIMS, state);
		job.primType = type;
		job.flag = flag;
		job.pv.clear();
		w_->SetOpen(&job, key, &state);
		return job;
	}
	void ClosePrimsIfFull(TLJob &job) {
		if (job.pv.size() >= 3072)
			w_->SubmitOpen();
	}
	TLJob &Begin(GEJobType type, const Rasterizer::RasterizerState &state) {
		TLJob &job = w_->Acquire();
		job.type = type;
		CaptureJobState(job, state);
		NoteTargets();
		NoteTexture(state);
		return job;
	}
	// The memory a queued draw's texture comes from: overwriting it (a block transfer) has to wait
	// until that draw has taken its copy (MayRead). Everything else a job needs is copied into it.
	void NoteTexture(const Rasterizer::RasterizerState &state) {
		if (!state.enableTextures || texturesAll_)
			return;
		const uint32_t a = Rasterizer::GeNormalizeAddr(state.texaddr[0]);
		const uint32_t bytes = (uint32_t)((uint64_t)textureBitsPerPixel[state.samplerID.TexFmt()] * state.texbufw[0] * state.samplerID.cached.sizes[0].h / 8);
		for (auto &t : textures_) {
			if (t.first == a) {
				t.second = std::max(t.second, a + bytes);
				return;
			}
		}
		if (textures_.size() < 64)
			textures_.emplace_back(a, a + bytes);
		else
			texturesAll_ = true;
	}
	// The color and depth buffers the current GE state draws to (generously: 512 rows).
	void NoteTargets() {
		// Only the rows a draw can reach (scissor and region): with 512 rows, Call of Duty's data
		// copies into VRAM right after its 272-row second frame buffer counted as reading it, and
		// each made the RSX write the buffer back (~20 a frame, 4-5 ms each).
		const uint32_t rows = std::min(512, std::min(gstate.getScissorY2(), gstate.getRegionY2()) + 1);
		const uint32_t fb = gstate.getFrameBufAddress() & 0x1FFFFF;
		const uint32_t fbBytes = gstate.FrameBufStride() * (gstate.FrameBufFormat() == GE_FORMAT_8888 ? 4 : 2) * rows;
		const uint32_t zb = gstate.getDepthBufAddress() & 0x1FFFFF;
		const uint32_t zbBytes = gstate.DepthBufStride() * 2 * rows;
		Note(fb, fb + fbBytes);
		Note(zb, zb + zbBytes);
	}
	void Note(uint32_t a, uint32_t b) {
		for (auto &t : targets_) {
			if (t.first == a) {
				t.second = std::max(t.second, b);
				return;
			}
		}
		if (targets_.size() < 32) {
			targets_.emplace_back(a, b);
		} else {
			// Too many to track: one range covering all of VRAM.
			targets_.clear();
			targets_.emplace_back(0u, 0x200000u);
		}
	}

	TLWorker *w_;
	Rasterizer::HardwareBackend *r_;
	std::vector<std::pair<uint32_t, uint32_t>> targets_;
	// Textures of the draws queued since the last Sync (normalized address ranges).
	std::vector<std::pair<uint32_t, uint32_t>> textures_;
	bool texturesAll_ = false;
};

static TLWorkerBackend *g_tlWorkerBackend = nullptr;

namespace Rasterizer {

HardwareBackend *StartGEWorker(HardwareBackend *real) {
	StopGEWorker();
	g_tlWorker = new TLWorker(real);
	g_tlWorkerBackend = new TLWorkerBackend(g_tlWorker);
#if !PPSSPP_PLATFORM(PS3)
	// Host tests: PPSSPP_SPU_CHECK=1 as the PS3 diag build's SPU vertex check (with PPSSPP_TLK=1).
	if (getenv("PPSSPP_SPU_CHECK")) {
		g_spuVertexCheck = true;
		atexit([] {
			std::string report;
			TakeSpuVertexCheckReport(report);
			fputs(report.c_str(), stderr);
		});
	}
#endif
	return g_tlWorkerBackend;
}

// The GE call returns to the game: the draw left open for later ones to join goes to the worker
// now. Held back, it was only drawn after the game had run again, and read texture memory the game
// had changed by then (Killzone's HUD as white boxes, letters missing from Silent Hill Origins'
// menu: both fine with draw merging off). Gran Turismo makes ~10 GE calls a frame, so merging
// within a call keeps nearly all of its merges.
void GEBurstEnd() {
	if (!g_tlWorker)
		return;
	g_tlWorker->SubmitOpen();
}

void StopGEWorker() {
	if (!g_tlWorker)
		return;
	g_tlWorker->Drain();

	delete g_tlWorkerBackend;
	delete g_tlWorker;
	g_tlWorkerBackend = nullptr;
	g_tlWorker = nullptr;
}

}  // namespace Rasterizer

// Fills job with the draw (copies of the vertices it uses and its indices). False: too big to copy.
// The GE registers the RSX renderer reads while drawing (besides the rasterizer state): equal
// values mean two draws can share one job.
static bool SameDrawRegs(const GEState &a, const GEState &b) {
	return a.fbptr == b.fbptr && a.fbwidth == b.fbwidth && a.zbptr == b.zbptr && a.zbwidth == b.zbwidth &&
		a.framebufpixformat == b.framebufpixformat && a.scissor1 == b.scissor1 && a.scissor2 == b.scissor2 &&
		a.region1 == b.region1 && a.region2 == b.region2 && a.pmskc == b.pmskc && a.pmska == b.pmska &&
		a.blendfixa == b.blendfixa && a.blendfixb == b.blendfixb && a.stenciltest == b.stenciltest &&
		a.lop == b.lop && a.clutformat == b.clutformat;
}

static bool SameDecode(const TLDecodeSetup &a, const TLDecodeSetup &b) {
	return a.size == b.size && a.wt == b.wt && a.nw == b.nw && a.tc == b.tc && a.col == b.col && a.nrm == b.nrm && a.pos == b.pos &&
		a.woff == b.woff && a.tcoff == b.tcoff && a.coloff == b.coloff && a.nrmoff == b.nrmoff && a.posoff == b.posoff &&
		a.morph == b.morph && a.onesize == b.onesize && a.readUV == b.readUV && a.nsign == b.nsign && a.matAmbient == b.matAmbient &&
		a.fixedNormal == b.fixedNormal;
}

// Strips and fans as plain lists (the order ExecuteTLJob draws them in), so draws can be merged.
static GEPrimitiveType ListPrim(GEPrimitiveType prim) {
	switch (prim) {
	case GE_PRIM_TRIANGLE_STRIP: case GE_PRIM_TRIANGLE_FAN: return GE_PRIM_TRIANGLES;
	case GE_PRIM_LINE_STRIP: return GE_PRIM_LINES;
	default: return prim;
	}
}
static void ExpandToList(const uint16_t *in, int n, GEPrimitiveType prim, IdxVec &out) {
	const size_t base = out.size();
	switch (prim) {
	case GE_PRIM_TRIANGLE_STRIP: {
		const int tris = std::max(0, n - 2);
		out.resize(base + (size_t)tris * 3);
		uint16_t *o = out.data() + base;
		for (int i = 0; i < tris; ++i, o += 3) {
			const bool odd = (i & 1) != 0;
			o[0] = in[i + (odd ? 1 : 0)];
			o[1] = in[i + (odd ? 0 : 1)];
			o[2] = in[i + 2];
		}
		break;
	}
	case GE_PRIM_TRIANGLE_FAN: {
		const int tris = std::max(0, n - 2);
		out.resize(base + (size_t)tris * 3);
		uint16_t *o = out.data() + base;
		for (int i = 2; i < n; ++i, o += 3) {
			o[0] = in[0];
			o[1] = in[i - 1];
			o[2] = in[i];
		}
		break;
	}
	case GE_PRIM_LINE_STRIP: {
		const int lines = std::max(0, n - 1);
		out.resize(base + (size_t)lines * 2);
		uint16_t *o = out.data() + base;
		for (int i = 0; i < lines; ++i, o += 2) {
			o[0] = in[i];
			o[1] = in[i + 1];
		}
		break;
	}
	case GE_PRIM_TRIANGLES:
		out.insert(out.end(), in, in + n - n % 3);
		break;
	default:
		out.insert(out.end(), in, in + n - n % 2);
		break;
	}
}

// Adds a draw to an open T&L job when everything but the vertices is the same (Gran Turismo draws
// ~14,000 single triangles a frame, each a draw of its own). False: not merged, nothing changed.
#if !PPSSPP_PLATFORM(PS3)
// PPSSPP_MERGECHECK (test builds): draws the serial would have merged go through the full
// comparison instead, and any that fail it are reported.
static const bool g_mergeCheck = getenv("PPSSPP_MERGECHECK") != nullptr;
static bool g_fastWould = false;
static void MergeFail(int reason) {
	if (g_fastWould) {
		static int reports;
		if (reports++ < 200)
			fprintf(stderr, "MERGECHECK: same serial, but the full comparison fails (reason %d)\n", reason);
		g_fastWould = false;
	}
	static const bool log = getenv("PPSSPP_MERGELOG") != nullptr;
	static int counts[16];
	static int total;
	if (!log) return;
	counts[reason & 15]++;
	if ((++total & 255) == 0) {
		fprintf(stderr, "MERGEFAIL:");
		for (int i = 0; i < 16; ++i) if (counts[i]) fprintf(stderr, " %d=%d", i, counts[i]);
		fprintf(stderr, "\n");
	}
}
// Reason 0 in the log: merged without the state comparison.
#define PS3_MERGE_FAST() MergeFail(0)
#else
static inline void MergeFail(int reason) { PS3Profile::tlMergeFail[reason & 15]++; }
#define PS3_MERGE_FAST() PS3Profile::tlMergeFast++
#endif

// The index range a draw uses; false when appending it would make the job too big.
static inline bool MergeRange(const TLJob &job, const TLDecodeSetup &s, const void *indices, u32 idxType, int vertexCount, int &lower, int &upper) {
	if (job.idx.size() + vertexCount > 30000)
		{ MergeFail(3); return false; }
	lower = 0;
	upper = vertexCount - 1;
	if (indices) {
		lower = 0xFFFF;
		upper = 0;
		if (idxType == GE_VTYPE_IDX_8BIT) {
			const u8 *i8 = (const u8 *)indices;
			for (int i = 0; i < vertexCount; ++i) {
				const int d = i8[i];
				lower = std::min(lower, d);
				upper = std::max(upper, d);
			}
		} else {
			const u16_le *i16 = (const u16_le *)indices;
			for (int i = 0; i < vertexCount; ++i) {
				const int d = i16[i];
				lower = std::min(lower, d);
				upper = std::max(upper, d);
			}
		}
	}
	const uint32_t addVerts = (uint32_t)(upper - lower + 1);
	if (job.rawVerts + addVerts > 16000 || job.raw.size() + (size_t)addVerts * s.size > MergeRawLimit())
		{ MergeFail(4); return false; }
	return true;
}

// Vertices appended to the job's copied range, indices rebased onto them.
static void MergeAppend(TLJob &job, const TLDecodeSetup &s, const void *indices, u32 idxType, GEPrimitiveType prim, int vertexCount, int lower, int upper) {
	// A strip or fan becomes a list once something joins it (build 153: not when queued, most never
	// get anything added).
	if (job.prim != ListPrim(job.prim)) {
		PS3_PROFILE(tlExpand);
		static IdxVec list;
		list.clear();
		ExpandToList(job.idx.data(), (int)job.idx.size(), job.prim, list);
		job.idx.swap(list);
		job.prim = ListPrim(job.prim);
	}
	const uint32_t addVerts = (uint32_t)(upper - lower + 1);
	const size_t oldBytes = job.raw.size();
	const size_t newBytes = oldBytes + (size_t)addVerts * s.size;
	if (newBytes > job.raw.capacity()) {
		// Grow at most to the merge limit, not to twice the size (a 1 MB block for a 512 KB job).
		job.raw.reserve(std::max(newBytes, std::min(newBytes * 2, MergeRawLimit())));
	}
	job.raw.resize(newBytes);
	CopyVerts(job.raw.data() + oldBytes, s.base + (size_t)lower * s.size, (size_t)addVerts * s.size);
	const int base = (int)job.rawVerts - lower;
	job.rawVerts += addVerts;
	// Straight into the job's list.
	auto emit = [&](auto at) {
		if (prim == GE_PRIM_TRIANGLES || prim == GE_PRIM_LINES) {
			const int n = vertexCount - vertexCount % (prim == GE_PRIM_TRIANGLES ? 3 : 2);
			const size_t pos = job.idx.size();
			job.idx.resize(pos + n);
			uint16_t *o = job.idx.data() + pos;
			for (int i = 0; i < n; ++i)
				o[i] = (uint16_t)(at(i) + base);
		} else if (prim == GE_PRIM_TRIANGLE_STRIP) {
			const int tris = std::max(0, vertexCount - 2);
			const size_t pos = job.idx.size();
			job.idx.resize(pos + (size_t)tris * 3);
			uint16_t *o = job.idx.data() + pos;
			for (int i = 0; i < tris; ++i, o += 3) {
				const int odd = i & 1;
				o[0] = (uint16_t)(at(i + odd) + base);
				o[1] = (uint16_t)(at(i + 1 - odd) + base);
				o[2] = (uint16_t)(at(i + 2) + base);
			}
		} else if (prim == GE_PRIM_TRIANGLE_FAN) {
			const int tris = std::max(0, vertexCount - 2);
			const size_t pos = job.idx.size();
			job.idx.resize(pos + (size_t)tris * 3);
			uint16_t *o = job.idx.data() + pos;
			const uint16_t first = (uint16_t)(at(0) + base);
			for (int i = 2; i < vertexCount; ++i, o += 3) {
				o[0] = first;
				o[1] = (uint16_t)(at(i - 1) + base);
				o[2] = (uint16_t)(at(i) + base);
			}
		} else {
			const int lines = std::max(0, vertexCount - 1);  // Line strip.
			const size_t pos = job.idx.size();
			job.idx.resize(pos + (size_t)lines * 2);
			uint16_t *o = job.idx.data() + pos;
			for (int i = 0; i < lines; ++i, o += 2) {
				o[0] = (uint16_t)(at(i) + base);
				o[1] = (uint16_t)(at(i + 1) + base);
			}
		}
	};
	if (!indices) {
		emit([](int i) { return i; });
	} else if (idxType == GE_VTYPE_IDX_8BIT) {
		const u8 *i8 = (const u8 *)indices;
		emit([i8](int i) { return (int)i8[i]; });
	} else {
		const u16_le *i16 = (const u16_le *)indices;
		emit([i16](int i) { return (int)i16[i]; });
	}
}

#if PPSSPP_ARCH(POWERPC)
// Times MergeTLJob's parts (PS3Profile::tlMergeT): Part(k) charges the time since the last call to
// the part before k; the rest goes to the last part when it ends.
struct MergeTimer {
	uint64_t t = PS3Profile::Now();
	int part = 0;
	MergeTimer() { PS3Profile::tlMergeTries++; }
	void Part(int next) { const uint64_t n = PS3Profile::Now(); PS3Profile::tlMergeT[part] += n - t; t = n; part = next; }
	~MergeTimer() { PS3Profile::tlMergeT[part] += PS3Profile::Now() - t; }
};
#define MERGE_TIMER() MergeTimer mergeTimer
#define MERGE_PART(k) mergeTimer.Part(k)
#else
#define MERGE_TIMER()
#define MERGE_PART(k)
#endif

static bool MergeTLJob(TLJob &job, const TLDecodeSetup &s, const Rasterizer::RasterizerState &state, const TransformState &ts,
	const void *indices, u32 vertexType, GEPrimitiveType prim, int vertexCount, int cull) {
	MERGE_TIMER();
	if (job.type != GEJobType::TL_RAW || ListPrim(job.prim) != ListPrim(prim) || job.cull != cull || job.vertexType != vertexType)
		{ MergeFail(1); return false; }
	if (s.morph > 1 || !SameDecode(s, job.s))
		{ MergeFail(2); return false; }
	const u32 idxType = vertexType & GE_VTYPE_IDX_MASK;
	int lower, upper;
	MERGE_PART(1);
	if (!MergeRange(job, s, indices, idxType, vertexCount, lower, upper))
		return false;
	MERGE_PART(2);
	// No GE command but vertex/index addresses since the job's state was taken or last matched:
	// everything below is the same.
	const bool sameGE = job.geSerial == Rasterizer::g_geStateSerial;
#if !PPSSPP_PLATFORM(PS3)
	if (sameGE && g_mergeCheck) {
		g_fastWould = true;
	} else
#endif
	if (sameGE) {
		PS3_MERGE_FAST();
		goto merge;
	}
	if (s.wt && memcmp(job.bones, gstate.boneMatrix, sizeof(job.bones)) != 0)
		{ MergeFail(5); return false; }
	// What usually differs between draws (Gran Turismo: the texture), before the full comparison.
	if (state.texaddr[0] != job.state.texaddr[0] || state.texbufw[0] != job.state.texbufw[0] ||
		memcmp(&state.pixelID, &job.state.pixelID, sizeof(state.pixelID)) != 0)
		{ MergeFail(7); return false; }
	if (!SameDrawRegs(job.regs, gstate))
		{ MergeFail(6); return false; }
	// Rasterizer state: the same apart from the CLUT pointer (compared by content) and the CPU
	// epoch (texture checks happen per job).
	{
		alignas(16) Rasterizer::RasterizerState n, o;
		memcpy((void *)&n, (const void *)&state, sizeof(n));
		memcpy((void *)&o, (const void *)&job.state, sizeof(o));
		Rasterizer::NormalizeForHardware(n);
		Rasterizer::NormalizeForHardware(o);
		n.cpuEpoch = o.cpuEpoch;
		if (memcmp(&n, &o, sizeof(n)) != 0) {
#if !PPSSPP_PLATFORM(PS3)
			static const bool diffLog = getenv("PPSSPP_MERGEDIFF") != nullptr;
			if (diffLog) {
				const uint8_t *a = (const uint8_t *)&n, *b = (const uint8_t *)&o;
				for (size_t i = 0; i < sizeof(n); ++i) if (a[i] != b[i]) { fprintf(stderr, "MERGEDIFF %d\n", (int)i); break; }
			}
#endif
			MergeFail(7);
			return false;
		}
		if (state.enableTextures && state.samplerID.cached.clut && memcmp(state.samplerID.cached.clut, job.clut, ClutBytesUsed(state.samplerID)) != 0)
			{ MergeFail(8); return false; }
	}
	MERGE_PART(3);
	{
		static Rasterizer::TLParams params;
		BuildTLParams(ts, params);
		if (memcmp(&params, &job.params, sizeof(params)) != 0)
			{ MergeFail(9); return false; }
	}
#if !PPSSPP_PLATFORM(PS3)
	{
		static const bool slog = getenv("PPSSPP_SERIALLOG") != nullptr;
		static int hist[260];
		static int n;
		if (slog) {
			hist[Rasterizer::g_lastSerialCmd + 1]++;
			if ((++n & 1023) == 0) {
				fprintf(stderr, "SERIAL:");
				for (int i = 0; i < 260; ++i) if (hist[i] > 8) fprintf(stderr, " %x=%d", i - 1, hist[i]);
				fprintf(stderr, "\n");
			}
		}
		Rasterizer::g_lastSerialCmd = -1;
	}
#endif
	job.geSerial = Rasterizer::g_geStateSerial;
#if !PPSSPP_PLATFORM(PS3)
	g_fastWould = false;
#endif
merge:
#if !PPSSPP_PLATFORM(PS3)
	{
		static const bool okLog = getenv("PPSSPP_MERGEOKLOG") != nullptr;
		if (okLog)
			fprintf(stderr, "MERGEOK tex %08x prim %d n %d vtype %06x into job with %d indices%s\n", state.texaddr[0], (int)prim, vertexCount, vertexType, (int)job.idx.size(), sameGE ? " (same serial)" : "");
	}
#endif
	MERGE_PART(4);
	MergeAppend(job, s, indices, idxType, prim, vertexCount, lower, upper);
	return true;
}

// Big vertex ranges aren't copied into a queued job: a multi-MB block may not be there once main memory
// is in small pieces (Silent Hill Origins ran out after ~7 minutes, the PS3 had 24 MB free but no
// 2 MB in one piece). Such a draw is drawn at once from PSP memory instead (inPlace).
static size_t TLCopyLimit() {
#if PPSSPP_PLATFORM(PS3)
	return 1024 * 1024;
#else
	// Host tests: PPSSPP_TL_COPY_LIMIT=0 draws every T&L job in place.
	static const size_t limit = getenv("PPSSPP_TL_COPY_LIMIT") ? (size_t)atol(getenv("PPSSPP_TL_COPY_LIMIT")) : 1024 * 1024;
	return limit;
#endif
}

static bool PrepareTLJob(TLJob &job, const TLDecodeSetup &s, const Rasterizer::RasterizerState &state, const TransformState &ts,
	const void *indices, u32 vertexType, GEPrimitiveType prim, int vertexCount, int cull, bool inPlace = false) {
	const u32 idxType = vertexType & GE_VTYPE_IDX_MASK;
	int lower = 0, upper = vertexCount - 1;
#if PPSSPP_ARCH(POWERPC)
	uint64_t tp = PS3Profile::Now();
	auto lap = [&tp](uint64_t &acc) { const uint64_t t = PS3Profile::Now(); acc += t - tp; tp = t; };
#endif
	job.idx.resize(vertexCount);
	uint16_t *out = job.idx.data();
	if (indices) {
		const u8 *i8 = (const u8 *)indices;
		const u16_le *i16 = (const u16_le *)indices;
		lower = 0xFFFF;
		upper = 0;
		for (int i = 0; i < vertexCount; ++i) {
			const int d = idxType == GE_VTYPE_IDX_8BIT ? i8[i] : (int)i16[i];
			out[i] = (uint16_t)d;
			lower = std::min(lower, d);
			upper = std::max(upper, d);
		}
		if (lower != 0) {
			for (int i = 0; i < vertexCount; ++i)
				out[i] = (uint16_t)(out[i] - lower);
		}
	} else {
		for (int i = 0; i < vertexCount; ++i)
			out[i] = (uint16_t)i;
	}
	const size_t bytes = (size_t)(upper - lower + 1) * s.size;
#if !PPSSPP_PLATFORM(PS3)
	static const bool rawLog = getenv("PPSSPP_RAWLOG") != nullptr;
	if (rawLog) {
		static uint64_t total = 0, used = 0, n = 0;
		total += bytes; used += (size_t)vertexCount * s.size; n++;
		if ((n & 1023) == 0)
			fprintf(stderr, "RAW: %llu draws, %.1f MB copied, %.1f MB in index order (%d verts range %d)\n", (unsigned long long)n, total / 1048576.0, used / 1048576.0, vertexCount, upper - lower + 1);
	}
#endif
	if (bytes > 4 * 1024 * 1024 || (!inPlace && bytes > TLCopyLimit()))
		return false;
#if PPSSPP_ARCH(POWERPC)
	lap(PS3Profile::tlScan);
	if (!inPlace)
		PS3Profile::tlCopyBytes += bytes;
#endif
	job.type = GEJobType::TL_RAW;
	if (inPlace) {
		job.raw.clear();
		job.rawExt = s.base + (size_t)lower * s.size;
	} else {
		job.rawExt = nullptr;
		job.raw.resize(bytes);
		CopyVerts(job.raw.data(), s.base + (size_t)lower * s.size, bytes);
	}
#if PPSSPP_ARCH(POWERPC)
	lap(PS3Profile::tlCopy);
#endif
	job.rawVerts = (uint32_t)(upper - lower + 1);
	job.vertexType = vertexType;
	job.s = s;
	if (s.wt)
		memcpy(job.bones, gstate.boneMatrix, sizeof(job.bones));
	if (s.morph > 1)
		memcpy(job.morph, gstate_c.morphWeights, sizeof(job.morph));
	if (g_tlWorker)
		CaptureJobState(job, state);
	else
		job.state = state;
#if PPSSPP_ARCH(POWERPC)
	lap(PS3Profile::tlCapture);
#endif
	BuildTLParams(ts, job.params);
#if PPSSPP_ARCH(POWERPC)
	lap(PS3Profile::tlParams);
#endif
	job.prim = prim;
	job.cull = cull;
	job.geSerial = Rasterizer::g_geStateSerial;
	return true;
}

// The whole T&L draw straight from PSP memory: index list, decode of each used vertex once, draw.
// False: not handled (nothing done), take the regular path.
// The last draw that went into the open T&L job, for TransformUnit::FastMergePrim: while no GE
// state command comes (the serial), the next draws with the same vertex format and primitive only
// need their vertices and indices appended.
struct FastMergeRecord {
	uint32_t serial = 0;
	bool valid = false;
	u32 vertexType = 0;
	GEPrimitiveType prim = GE_PRIM_TRIANGLES;
	int align = 1;
	const TLJob *job = nullptr;
	TLDecodeSetup s;
};
static FastMergeRecord g_fastMerge;

static void NoteFastMerge(const TLJob *job, const TLDecodeSetup &s, const VertexDecoder &dec, u32 vertexType, GEPrimitiveType prim) {
	g_fastMerge.valid = s.morph <= 1;
	g_fastMerge.serial = Rasterizer::g_geStateSerial;
	g_fastMerge.vertexType = vertexType;
	g_fastMerge.prim = prim;
	g_fastMerge.align = dec.biggest;
	g_fastMerge.job = job;
	g_fastMerge.s = s;
}

static bool DirectTLDraw(const VertexDecoder &dec, const TransformState &ts, const Rasterizer::RasterizerState &state, const void *vertices,
	const void *indices, u32 vertexType, GEPrimitiveType prim, int vertexCount, CullType cullType, int &miss) {
	miss = 3;
	// Lines too (cull 3 tells the backend): Dead or Alive Paradise draws every character mesh a
	// second time as lines, which took the per-primitive software path (skinned on the CPU).
	const bool lines = prim == GE_PRIM_LINES || prim == GE_PRIM_LINE_STRIP;
	if (prim != GE_PRIM_TRIANGLES && prim != GE_PRIM_TRIANGLE_STRIP && prim != GE_PRIM_TRIANGLE_FAN && !lines) {
		miss = 0;
		return false;
	}
	if (vertexCount < (lines ? 2 : 3) || gstate.isModeClear())
		return false;
	if (!CanUseTL(ts, false)) {
		miss = 1;
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
		static const bool missLog = getenv("PPSSPP_TLMISS_LOG") != nullptr;
		if (missLog) {
			int powered = 0;
			for (int l = 0; l < 4; ++l)
				if (ts.enableLighting && ts.lightingState.lights[l].enabled && gstate.isUsingPoweredDiffuseLight(l))
					powered++;
			fprintf(stderr, "TLMISS uvgen %d proj %d powered %d lighting %d count %d\n", (int)ts.uvGenMode, (int)gstate.getUVProjMode(), powered, ts.enableLighting ? 1 : 0, vertexCount);
		}
#endif
		return false;
	}
	if (indices && (vertexType & GE_VTYPE_IDX_MASK) == GE_VTYPE_IDX_32BIT) {
		miss = 2;
		return false;
	}
	// The backend takes up to 20000 vertices per draw (more go the regular way).
	if (vertexCount > 20000)
		return false;
	TLDecodeSetup s;
	{
		PS3_PROFILE(tlSetup);
		if (!SetupTLDecode(s, dec, ts, vertices))
			return false;
	}
	const int cull = lines ? 3 : (cullType == CullType::CCW ? 1 : (cullType == CullType::CW ? 2 : 0));
	if (g_tlWorker) {
#if PPSSPP_PLATFORM(PS3)
		const bool mergeable = Rasterizer::g_tlMergeEnabled;
#else
		static const bool noMerge = getenv("PPSSPP_NO_TLMERGE") != nullptr;
		const bool mergeable = !noMerge && Rasterizer::g_tlMergeEnabled;
#endif
		if (mergeable) {
			if (TLJob *open = g_tlWorker->OpenTL()) {
				bool merged;
				{
					PS3_PROFILE_SAMPLED(geMerge);
					merged = MergeTLJob(*open, s, state, ts, indices, vertexType, prim, vertexCount, cull);
				}
				if (merged) {
#if PPSSPP_ARCH(POWERPC)
					PS3Profile::tlMerged++;
#endif
					NoteFastMerge(open, s, dec, vertexType, prim);
					return true;
				}
			}
		}
		TLJob &job = g_tlWorker->Acquire();
		bool prepared;
		{
			PS3_PROFILE_SAMPLED(gePrep);
			prepared = PrepareTLJob(job, s, state, ts, indices, vertexType, prim, vertexCount, cull);
		}
		if (prepared) {
			{
				PS3_PROFILE(tlNote);
				g_tlWorkerBackend->NoteCurrentTargets(state);
			}
			// (A strip triples in size as a list.)
			if (mergeable && s.morph <= 1 && job.idx.size() <= (job.prim == ListPrim(job.prim) ? 30000u : 10000u)) {
				// Open, so later draws can be added (submitted by the next Acquire, Drain or a draw
				// that can't join it); MergeAppend makes it a list first.
				g_tlWorker->SetOpenTL(&job);
				NoteFastMerge(&job, s, dec, vertexType, prim);
			}
			else
				g_tlWorker->Submit();
			return true;
		}
		// Too big to copy (over 1 MB of vertices): drawn here, from PSP memory, once the worker is idle.
		g_tlWorker->Drain();
	}
	static TLJob job;
	if (!PrepareTLJob(job, s, state, ts, indices, vertexType, prim, vertexCount, cull, true))
		return false;
	ExecuteTLJob(job, g_tlWorker ? g_tlWorker->Real() : Rasterizer::g_hardwareBackend);
	return true;
}

// 3D rectangles (sprites outside through mode) straight from PSP memory, for the hardware backend:
// the direct T&L decode, the transform in plain float math (as the batched triangle path), then the
// same checks as Clipper::ProcessRect and the binner's AddRect. The regular path ran the GE's exact
// arithmetic per vertex on the emulation thread: Silent Hill Origins' fog draws ~1,900 sprites a
// frame, which cost 15-20 ms of every drawn frame there. Draws with lighting, fog, generated UVs,
// projected textures, weights or morphing still take the regular path.
static bool DirectRects(BinManager &binner, const VertexDecoder &dec, const TransformState &ts, const void *vertices,
	const void *indices, u32 vertexType, int vertexCount) {
	if (vertexCount < 2 || (vertexCount & 1) || gstate.isModeClear())
		return false;
	if (!ts.enableTransform || ts.enableLighting || ts.enableFog || ts.uvGenMode != GE_TEXMAP_TEXTURE_COORDS)
		return false;
	const Rasterizer::RasterizerState &state = binner.State();
	if (state.textureProj)
		return false;
	if (indices && (vertexType & GE_VTYPE_IDX_MASK) == GE_VTYPE_IDX_32BIT)
		return false;
	TLDecodeSetup s;
	if (!SetupTLDecode(s, dec, ts, vertices) || s.wt != 0 || s.morph > 1)
		return false;
	// Without UVs the regular path reuses the last vertex's (across draws); leave that to it.
	if (state.enableTextures && !s.readUV)
		return false;
	const float *m = ts.matrix;
	const bool depthClip = gstate.isDepthClipEnabled();
	const int offX16 = gstate.getOffsetX16(), offY16 = gstate.getOffsetY16();
	const bool idx8 = indices && (vertexType & GE_VTYPE_IDX_MASK) == GE_VTYPE_IDX_8BIT;
	float lastNormal[3] = { 0.0f, 0.0f, 1.0f };
	bool fullAlpha = true;
	struct Corner {
		VertexData v;
		float cx, cy, cz, cw;
		bool outside;
	};
	auto make = [&](int k, Corner &c) {
		const int vi = !indices ? k : (idx8 ? ((const u8 *)indices)[k] : ((const u16 *)indices)[k]);
		Rasterizer::TLVertex t;
		DecodeTLVertex(s, s.base + (size_t)vi * s.size, t, lastNormal, fullAlpha);
		float u = t.u, v = t.v;
		if (ts.geUVScale) {
			u = u * ts.uvScale[0] + ts.uvOffset[0];
			v = v * ts.uvScale[1] + ts.uvOffset[1];
		}
		c.v.texturecoords = Vec3Packedf(u, v, 0.0f);
		c.v.color0 = (u32)t.c[0] | ((u32)t.c[1] << 8) | ((u32)t.c[2] << 16) | ((u32)t.c[3] << 24);
		c.v.color1 = 0;
		c.v.fogdepth = 1.0f;
		c.cx = m[0] * t.x + m[4] * t.y + m[8] * t.z + m[12];
		c.cy = m[1] * t.x + m[5] * t.y + m[9] * t.z + m[13];
		c.cz = m[2] * t.x + m[6] * t.y + m[10] * t.z + m[14];
		c.cw = m[3] * t.x + m[7] * t.y + m[11] * t.z + m[15];
		c.v.clipw = c.cw;
		c.outside = false;
		if (!(c.cw > 0.0f))
			return;  // Culled below anyway.
		const float inv = 1.0f / c.cw;
		const float sx = c.cx * inv * ts.screenScale.x + ts.screenAdd.x;
		const float sy = c.cy * inv * ts.screenScale.y + ts.screenAdd.y;
		float sz = floorf(c.cz * inv * ts.screenScale.z + ts.screenAdd.z);
		// The GE's screen range check, as ClipToScreenInternal.
		constexpr float SCREEN_BOUND = 4095.0f + (15.5f / 16.0f);
		if (depthClip) {
			if (!(c.cz < -c.cw) && (sx >= SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0.0f || sy < 0.0f))
				c.outside = true;
			sz = std::min(std::max(sz, 0.0f), 65535.0f);
		} else if (sx > SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0.0f || sy < 0.0f || sz < 0.0f || sz >= 65536.0f) {
			c.outside = true;
		}
		c.v.screenpos = ScreenCoords((int)floorf(sx * 16.0f) - offX16, (int)floorf(sy * 16.0f) - offY16, (u16)sz);
	};
	auto outsideMask = [](const Corner &c) {
		int mask = 0;
		if (c.cx > c.cw) mask |= 1;
		if (-c.cx > c.cw) mask |= 2;
		if (c.cy > c.cw) mask |= 4;
		if (-c.cy > c.cw) mask |= 8;
		if (c.cz > c.cw) mask |= 16;
		if (-c.cz > c.cw) mask |= 32;
		return mask;
	};
	for (int k = 0; k + 1 < vertexCount; k += 2) {
		Corner a, b;
		make(k, a);
		make(k + 1, b);
		if (a.outside || b.outside)
			continue;
		if (outsideMask(a) & outsideMask(b))
			continue;
		if (!(a.cw > 0.0f && b.cw > 0.0f))
			continue;
		binner.AddRect(a.v, b.v);
	}
#if PPSSPP_ARCH(POWERPC)
	PS3Profile::directRects++;
#endif
	return true;
}

// Batched hardware path (PS3 RSX backend): transform the decoded vertices with plain float math
// in one tight loop, cull on the CPU, and hand all triangles of the draw to the backend at once.
// Skips the clipper (the RSX clips), the binner and the per-triangle calls. Draws that need what
// this doesn't do yet (lighting, texture matrix / environment UVs) return false and take the
// regular path.
static bool HardwareBatch(TransformUnit &tu, SoftwareVertexReader &vreader, const TransformState &ts, const Rasterizer::RasterizerState &state,
	GEPrimitiveType prim, int vertexCount, CullType cullType) {
	if (prim != GE_PRIM_TRIANGLES && prim != GE_PRIM_TRIANGLE_STRIP && prim != GE_PRIM_TRIANGLE_FAN)
		return false;
	uint64_t batchStart = 0;
#if PPSSPP_ARCH(POWERPC)
	batchStart = PS3Profile::Now();
	struct BatchTimer {
		uint64_t &start;
		~BatchTimer() { if (start) PS3Profile::batch += PS3Profile::Now() - start; }
	} batchTimer{batchStart};
#endif
	if (vertexCount < 3 || gstate.isModeClear())
		return false;
	const bool through = vreader.IsThrough();
	if (Rasterizer::g_hardwareBackend && Rasterizer::g_hardwareBackend->SupportsTL() && CanUseTL(ts, through)) {
		// Indices in the GE's orientation, no CPU culling (the backend culls).
		static std::vector<uint16_t> tlIdx;
		static std::vector<int32_t> tlRemap;
		static std::vector<Rasterizer::TLVertex> tlVerts;
		const int n = vreader.DecodedCount();
		tlIdx.clear();
		tlVerts.clear();
		tlRemap.assign(n, -1);
		VertexReader &r = vreader.RawReader();
		auto vtx = [&](int v) {
			const int d = vreader.DecodedIndex(v);
			int32_t &m = tlRemap[d];
			if (m < 0) {
				m = (int32_t)tlVerts.size();
				tlVerts.emplace_back();
				r.Goto(d);
				ReadTLVertex(r, ts, tlVerts.back());
			}
			tlIdx.push_back((uint16_t)m);
		};
		auto tri = [&](int a, int b, int c) { vtx(a); vtx(b); vtx(c); };
		switch (prim) {
		case GE_PRIM_TRIANGLES:
			for (int i = 0; i + 2 < vertexCount; i += 3)
				tri(i, i + 1, i + 2);
			break;
		case GE_PRIM_TRIANGLE_STRIP:
			for (int i = 0; i + 2 < vertexCount; ++i) {
				if (i & 1)
					tri(i + 1, i, i + 2);
				else
					tri(i, i + 1, i + 2);
			}
			break;
		default:
			for (int i = 2; i < vertexCount; ++i)
				tri(0, i - 1, i);
			break;
		}
		static Rasterizer::TLParams params;
		BuildTLParams(ts, params);
		const int cull = cullType == CullType::CCW ? 1 : (cullType == CullType::CW ? 2 : 0);
#if PPSSPP_ARCH(POWERPC)
		PS3Profile::batch += PS3Profile::Now() - batchStart;
		batchStart = 0;
#endif
		if (tlIdx.empty())
			return true;
		if (Rasterizer::g_hardwareBackend->DrawTL(state, params, tlVerts.data(), (int)tlVerts.size(), tlIdx.data(), (int)tlIdx.size(), cull))
			return true;
#if PPSSPP_ARCH(POWERPC)
		batchStart = PS3Profile::Now();
#endif
	}
	// Lighting and generated texture coordinates: the software GPU's own per-vertex code (exact
	// colors), still once per vertex and without the clipper/binner.
	const bool fullVertex = !through && (ts.enableLighting || ts.uvGenMode != GE_TEXMAP_TEXTURE_COORDS);

	// Per decoded vertex (indexed meshes reuse vertices).
	static std::vector<Rasterizer::FastVertex> decoded;
	static std::vector<Rasterizer::FastVertex> tris;
	const int n = vreader.DecodedCount();
	if ((int)decoded.size() < n)
		decoded.resize(n);
	VertexReader &r = vreader.RawReader();
	const bool readUV = ts.readUV;
	const float offX = gstate.getOffsetX16() * (1.0f / 16.0f), offY = gstate.getOffsetY16() * (1.0f / 16.0f);
	const float *m = ts.matrix;
	const bool depthClip = gstate.isDepthClipEnabled();
	static std::vector<uint8_t> outside;
	if ((int)outside.size() < n)
		outside.resize(n);
	const uint8_t *usedMap = vreader.UsedMap();
	for (int i = 0; i < n; ++i) {
		if (usedMap && !usedMap[i]) {
			outside[i] = 1;
			continue;
		}
		r.Goto(i);
		Rasterizer::FastVertex &o = decoded[i];
		outside[i] = 0;
		float pos[3];
		r.ReadPosThrough(pos);
		float uv[2] = {0.0f, 0.0f};
		if (readUV) {
			r.ReadUV(uv);
			if (ts.geUVScale) {
				uv[0] = uv[0] * ts.uvScale[0] + ts.uvOffset[0];
				uv[1] = uv[1] * ts.uvScale[1] + ts.uvOffset[1];
			}
		}
		o.s = uv[0];
		o.t = uv[1];
		o.q = 0.0f;
		o.color0 = r.hasColor0() ? r.ReadColor0_8888() : gstate.getMaterialAmbientRGBA();
		o.color1 = 0;
		if (through) {
			o.x = pos[0];
			o.y = pos[1];
			o.z = pos[2];
			o.w = 1.0f;
			o.fog = 1.0f;
			continue;
		}
		const float cx = m[0] * pos[0] + m[4] * pos[1] + m[8] * pos[2] + m[12];
		const float cy = m[1] * pos[0] + m[5] * pos[1] + m[9] * pos[2] + m[13];
		const float cz = m[2] * pos[0] + m[6] * pos[1] + m[10] * pos[2] + m[14];
		const float cw = m[3] * pos[0] + m[7] * pos[1] + m[11] * pos[2] + m[15];
		// Screen = clip / w * scale + center - offset, kept homogeneous for the RSX.
		o.x = cx * ts.screenScale.x + (ts.screenAdd.x - offX) * cw;
		o.y = cy * ts.screenScale.y + (ts.screenAdd.y - offY) * cw;
		o.z = cz * ts.screenScale.z + ts.screenAdd.z * cw;
		o.w = cw;
		// The GE's screen range check (ClipToScreenInternal): a primitive with a vertex outside the
		// 4096 pixel space is dropped.
		if (cw > 0.0f) {
			constexpr float SCREEN_BOUND = 4095.0f + (15.5f / 16.0f);
			const float inv = 1.0f / cw;
			const float sx = o.x * inv + offX, sy = o.y * inv + offY, sz = o.z * inv;
			if (depthClip) {
				if (!(cz < -cw) && (sx >= SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0.0f || sy < 0.0f))
					outside[i] = 1;
			} else if (sx > SCREEN_BOUND || sy >= SCREEN_BOUND || sx < 0.0f || sy < 0.0f || sz < 0.0f || sz >= 65536.0f) {
				outside[i] = 1;
			}
		}
		if (ts.enableFog && ts.fogGE) {
			const float viewZ = ts.viewZColumn[0] * pos[0] + ts.viewZColumn[1] * pos[1] + ts.viewZColumn[2] * pos[2] + ts.viewZColumn[3];
			o.fog = GEFogFactor((viewZ + ts.fogEnd) * ts.fogSlope) * (1.0f / 256.0f);
		} else if (ts.enableFog) {
			o.fog = GEFogFactor(Dot43(ts.posToFog, Vec3f(pos[0], pos[1], pos[2]))) * (1.0f / 256.0f);
		} else {
			o.fog = 1.0f;
		}
		if (fullVertex)
			FastLightAndTexGen(r, ts, pos, o);
	}

#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	// Test build: compare the batched path's vertices with the software GPU's own.
	if (!Rasterizer::g_hardwareBackend) {
		// Hardware T&L check: dump the vertex program's inputs and constants with the batched
		// path's results, for PS3/rsx/check-tl.py (which runs the program's assembly).
		// The direct T&L decode against DecodeVerts + ReadTLVertex.
		TLDecodeSetup tls;
		if (CanUseTL(ts, through) && SetupTLDecode(tls, vreader.Decoder(), ts, vreader.Vertices())) {
			static int tlBad = 0;
			static uint64_t tlChecked = 0;
			static float lastN[3] = { 0.0f, 0.0f, 1.0f };
			bool fa = true;
			for (int i = 0; i < n; ++i) {
				if (usedMap && !usedMap[i])
					continue;
				r.Goto(i);
				Rasterizer::TLVertex want, got;
				ReadTLVertex(r, ts, want);
				DecodeTLVertex(tls, vreader.RawVertex(i), got, lastN, fa);
				tlChecked++;
				auto far = [](float a, float b) { return fabsf(a - b) > 1e-5f * std::max(1.0f, fabsf(a)); };
				const bool nrmOk = !vreader.Decoder().nrm || !(far(want.nx, got.nx) || far(want.ny, got.ny) || far(want.nz, got.nz));
				if (far(want.x, got.x) || far(want.y, got.y) || far(want.z, got.z) || !nrmOk || far(want.u, got.u) || far(want.v, got.v) ||
					memcmp(want.c, got.c, 4) != 0) {
					if (tlBad++ < 20)
						fprintf(stderr, "DIRECT TL: vtype %08x vertex %d differs: pos %g,%g,%g / %g,%g,%g nrm %g,%g,%g / %g,%g,%g uv %g,%g / %g,%g col %08x / %08x\n",
							vreader.Decoder().VertexType(), i, want.x, want.y, want.z, got.x, got.y, got.z, want.nx, want.ny, want.nz, got.nx, got.ny, got.nz,
							want.u, want.v, got.u, got.v, *(uint32_t *)want.c, *(uint32_t *)got.c);
				}
			}
			if ((tlChecked & ~1023) != ((tlChecked - n) & ~1023) || tlChecked == (uint64_t)n)
				fprintf(stderr, "DIRECT TL: %llu vertices checked, %d differ\n", (unsigned long long)tlChecked, tlBad);
		}
		static FILE *tlDump = getenv("PPSSPP_TL_DUMP") ? fopen(getenv("PPSSPP_TL_DUMP"), "a") : nullptr;
		static int tlDumped = 0;
		// PPSSPP_TL_DUMP_ENV: only the environment-mapped draws.
		static const bool tlDumpEnvOnly = getenv("PPSSPP_TL_DUMP_ENV") != nullptr;
		if (tlDump && tlDumped < 20000 && CanUseTL(ts, through) && (!tlDumpEnvOnly || ts.uvGenMode == GE_TEXMAP_ENVIRONMENT_MAP)) {
			static Rasterizer::TLParams params;
			BuildTLParams(ts, params);
			const float kx = 2.0f / 480.0f, ky = 2.0f / 272.0f, kz = 2.0f / 65535.0f;
			float rows[4][4];
			Rasterizer::FoldTLMatrix(params, kx, ky, kz, false, rows);
			memcpy(params.c[0], rows, sizeof(rows));
			fprintf(tlDump, "D");
			for (int k = 0; k < Rasterizer::TL_CONST_COUNT; ++k)
				for (int j = 0; j < 4; ++j)
					fprintf(tlDump, " %.9g", params.c[k][j]);
			fprintf(tlDump, "\n");
			for (int i = 0; i < n; ++i) {
				if (outside[i] || !(decoded[i].w > 0.0f))
					continue;
				r.Goto(i);
				Rasterizer::TLVertex in;
				ReadTLVertex(r, ts, in);
				const Rasterizer::FastVertex &o = decoded[i];
				fprintf(tlDump, "V %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d %d %.9g %.9g  %.9g %.9g %.9g %.9g %08x %08x %.9g %.9g %.9g %.9g\n",
					in.x, in.y, in.z, in.nx, in.ny, in.nz, in.c[0], in.c[1], in.c[2], in.c[3], in.u, in.v,
					kx * o.x - o.w, ky * o.y - o.w, kz * o.z - o.w, o.w, o.color0, o.color1, o.s, o.t, o.fog,
					ts.uvGenMode == GE_TEXMAP_TEXTURE_MATRIX ? o.q : 1.0f);
				tlDumped++;
			}
		}
		static int reported = 0;
		static uint64_t checked = 0;
		for (int i = 0; i < n && reported < 40; ++i) {
			if (outside[i]) continue;
			r.Goto(i);
			const ClipVertexData cv = tu.ReadVertex(r, ts);
			if (cv.v.screenpos.x == 0x7FFFFFFF || !(decoded[i].w > 0.0f)) continue;
			checked++;
			const Rasterizer::FastVertex &o = decoded[i];
			const float sx = cv.v.screenpos.x / 16.0f, sy = cv.v.screenpos.y / 16.0f, sz = cv.v.screenpos.z;
			const float fx = o.x / o.w, fy = o.y / o.w, fz = o.z / o.w;
			auto colorFar = [](uint32_t a, uint32_t b, int tol) {
				for (int k = 0; k < 32; k += 8)
					if (abs((int)((a >> k) & 0xFF) - (int)((b >> k) & 0xFF)) > tol) return true;
				return false;
			};
			const int tol = fullVertex ? 3 : 0;
			bool bad = fabsf(fx - sx) > 0.6f || fabsf(fy - sy) > 0.6f || fabsf(fz - sz) > 2.0f || colorFar(o.color0, cv.v.color0, tol) || colorFar(o.color1, cv.v.color1, tol) ||
				fabsf(o.s - cv.v.texturecoords.s()) > 1e-3f || fabsf(o.t - cv.v.texturecoords.t()) > 1e-3f || fabsf(o.fog - cv.v.fogdepth) > 0.01f;
			if (bad) {
				reported++;
				fprintf(stderr, "FASTPATH MISMATCH through=%d full=%d fast (%f %f %f) col %08x/%08x st %f %f fog %f vs sw (%f %f %f) col %08x/%08x st %f %f fog %f\n",
					through, fullVertex ? 1 : 0, fx, fy, fz, o.color0, o.color1, o.s, o.t, o.fog, sx, sy, sz, cv.v.color0, cv.v.color1, cv.v.texturecoords.s(), cv.v.texturecoords.t(), cv.v.fogdepth);
			}
		}
		if ((checked & 0xFFFF) == 1) fprintf(stderr, "FASTPATH checked %llu vertices\n", (unsigned long long)checked);
		return false;
	}
#endif

	// Triangle list in the GE's orientation (strips alternate; fans pivot on vertex 0), culled
	// like BinManager::AddTriangle: kept if the screen-space cross product of the given order is
	// positive (CCW), negative (CW), or either (culling off). Built as indices into the decoded
	// vertices, so each vertex is converted and sent once.
	static std::vector<float> sxy;
	if ((int)sxy.size() < n * 2)
		sxy.resize(n * 2);
	for (int i = 0; i < n; ++i) {
		if (outside[i])
			continue;
		const Rasterizer::FastVertex &v = decoded[i];
		const float inv = v.w > 0.0f ? 1.0f / v.w : 0.0f;
		sxy[i * 2] = v.x * inv;
		sxy[i * 2 + 1] = v.y * inv;
	}
	static std::vector<uint16_t> idx;
	idx.clear();
	auto emit = [&](int a, int b, int c) {
		const int ia = vreader.DecodedIndex(a), ib = vreader.DecodedIndex(b), ic = vreader.DecodedIndex(c);
		if (outside[ia] || outside[ib] || outside[ic])
			return;
		const float w0 = decoded[ia].w, w1 = decoded[ib].w, w2 = decoded[ic].w;
		if (!through) {
			if (w0 < 0.0f && w1 < 0.0f && w2 < 0.0f)
				return;
			// Without depth clip the GE doesn't clip at the near plane; a vertex behind the camera
			// drops the whole triangle instead (Clipper::ProcessTriangle).
			if (!depthClip && !(w0 > 0.0f && w1 > 0.0f && w2 > 0.0f))
				return;
		}
		if (w0 > 0.0f && w1 > 0.0f && w2 > 0.0f) {
			const float x0 = sxy[ia * 2], y0 = sxy[ia * 2 + 1];
			const float d01x = x0 - sxy[ib * 2], d01y = y0 - sxy[ib * 2 + 1];
			const float d02x = x0 - sxy[ic * 2], d02y = y0 - sxy[ic * 2 + 1];
			const float cross = d01x * d02y - d01y * d02x;
			if (cross == 0.0f)
				return;
			if (cullType == CullType::CCW && cross < 0.0f)
				return;
			if (cullType == CullType::CW && cross > 0.0f)
				return;
		}
		idx.push_back((uint16_t)ia);
		idx.push_back((uint16_t)ib);
		idx.push_back((uint16_t)ic);
	};
	switch (prim) {
	case GE_PRIM_TRIANGLES:
		for (int i = 0; i + 2 < vertexCount; i += 3)
			emit(i, i + 1, i + 2);
		break;
	case GE_PRIM_TRIANGLE_STRIP:
		for (int i = 0; i + 2 < vertexCount; ++i) {
			if (i & 1)
				emit(i + 1, i, i + 2);
			else
				emit(i, i + 1, i + 2);
		}
		break;
	default:
		for (int i = 2; i < vertexCount; ++i)
			emit(0, i - 1, i);
		break;
	}
	if (idx.empty())
		return true;
#if PPSSPP_ARCH(POWERPC)
	PS3Profile::batch += PS3Profile::Now() - batchStart;
	batchStart = 0;
#endif
	// Upload only the vertices the kept triangles use.
	static std::vector<int32_t> remap;
	static std::vector<Rasterizer::FastVertex> compact;
	const Rasterizer::FastVertex *uploadVerts = decoded.data();
	int uploadCount = n;
	if (usedMap || (int)idx.size() < n) {
		remap.assign(n, -1);
		compact.clear();
		for (uint16_t &i : idx) {
			int32_t &m = remap[i];
			if (m < 0) {
				m = (int32_t)compact.size();
				compact.push_back(decoded[i]);
			}
			i = (uint16_t)m;
		}
		uploadVerts = compact.data();
		uploadCount = (int)compact.size();
	}
	if (Rasterizer::g_hardwareBackend->DrawIndexed(state, uploadVerts, uploadCount, idx.data(), (int)idx.size(), through))
		return true;
	tris.clear();
	for (uint16_t i : idx)
		tris.push_back(uploadVerts[i]);
	return Rasterizer::g_hardwareBackend->DrawTriangles(state, tris.data(), (int)tris.size(), through);
}

#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
// Test build (PPSSPP_DIRECT_TL_FUZZ=n): random vertex formats and data through VertexDecoder +
// ReadTLVertex and through DecodeTLVertex.
static void DirectTLFuzz(SoftwareDrawEngine *drawEngine, int iterations) {
	uint32_t rng = 777;
	auto rnd = [&](uint32_t n) { rng = rng * 1103515245u + 12345u; return (uint32_t)((rng >> 8) % n); };
	const u32 savedTexmap = gstate.texmapmode;
	float savedBones[96];
	memcpy(savedBones, gstate.boneMatrix, sizeof(savedBones));
	gstate.texmapmode &= ~3;
	int bad = 0;
	long checked = 0;
	alignas(16) static u8 rawBuf[64 * 512];
	alignas(16) static u8 decoded[64 * 128];
	float savedMorph[8];
	memcpy(savedMorph, gstate_c.morphWeights, sizeof(savedMorph));
	for (int it = 0; it < iterations; ++it) {
		u32 vtype = rnd(4) | (rnd(5) == 0 ? 0 : (4 + rnd(4))) << 2 | rnd(4) << 5 | (1 + rnd(3)) << 7;
		if (rnd(2))
			vtype |= (1 + rnd(3)) << 9 | rnd(8) << 14;
		if (rnd(3) == 0)
			vtype |= (1 + rnd(7)) << 18;  // Morph frames.
		for (int i = 0; i < 8; ++i)
			gstate_c.morphWeights[i] = (float)((int)rnd(2001) - 500) / 1000.0f;
		u8 *raw = rawBuf;
		for (int i = 0; i < 96; ++i)
			gstate.boneMatrix[i] = (float)((int)rnd(4001) - 2000) / 1000.0f;
		VertexDecoder *dec = drawEngine->FindVertexDecoder(vtype);
		const int count = 1 + rnd(64);
		const bool smallFloats = rnd(4) != 0;
		for (int i = 0; i < dec->VertexSize() * count; ++i)
			raw[i] = (u8)rnd(256);
		if (smallFloats) {
			// Mostly sane floats in the float components (random bits elsewhere).
			for (int v = 0; v < count; ++v) {
				u8 *p = raw + v * dec->VertexSize();
				auto put = [&](int off, int n) { for (int k = 0; k < n; ++k) { float f = (float)((int)rnd(20001) - 10000) / 1000.0f; u32 b; memcpy(&b, &f, 4); for (int q = 0; q < 4; ++q) p[off + k * 4 + q] = (u8)(b >> (8 * q)); } };
				for (int f = 0; f < dec->morphcount; ++f) {
					const int fo = f * dec->onesize_;
					if (dec->weighttype == 3) put(fo + dec->weightoff, dec->nweights);
					if (dec->tc == 3) put(fo + dec->tcoff, 2);
					if (dec->nrm == 3) put(fo + dec->nrmoff, 3);
					if (dec->pos == 3) put(fo + dec->posoff, 3);
				}
			}
		}
		UVScale uvScale{ 1.0f, 1.0f, 0.0f, 0.0f };
		dec->DecodeVerts(decoded, raw, &uvScale, count);
		TransformState ts{};
		ts.readUV = dec->tc != 0;
		ts.negateNormals = rnd(2) != 0;
		VertexReader r(decoded, dec->GetDecVtxFmt(), vtype);
		TLDecodeSetup s;
		if (!SetupTLDecode(s, *dec, ts, raw)) {
			printf("DIRECT TL FUZZ: vtype %08x not accepted\n", vtype);
			continue;
		}
		static float lastN[3];
		bool fa = true;
		// Unmorphed formats: the batched decode (as the direct path uses), in a shuffled order.
		static Rasterizer::TLVertex batch[64];
		uint16_t order[64];
		for (int i = 0; i < count; ++i)
			order[i] = (uint16_t)i;
		for (int i = count - 1; i > 0; --i)
			std::swap(order[i], order[rnd(i + 1)]);
		const bool useBatch = s.morph == 1;
		bool faBatch = true;
		if (useBatch)
			DecodeTLVertices(s, order, count, batch, lastN, faBatch);
		for (int i = 0; i < count; ++i) {
			r.Goto(i);
			Rasterizer::TLVertex want, got;
			ReadTLVertex(r, ts, want);
			if (useBatch) {
				int at = 0;
				while (order[at] != i)
					++at;
				got = batch[at];
				if (!dec->nrm) {
					got.nx = want.nx; got.ny = want.ny; got.nz = want.nz;  // Carried over, order-dependent.
				}
			} else {
				DecodeTLVertex(s, raw + i * dec->VertexSize(), got, lastN, fa);
			}
			++checked;
			auto far = [](float a, float b) { if (std::isnan(a) || std::isnan(b)) return std::isnan(a) != std::isnan(b); return fabsf(a - b) > 1e-5f * std::max(1.0f, fabsf(a)); };
			const bool nrmBad = dec->nrm && (far(want.nx, got.nx) || far(want.ny, got.ny) || far(want.nz, got.nz));
			if (far(want.x, got.x) || far(want.y, got.y) || far(want.z, got.z) || nrmBad || far(want.u, got.u) || far(want.v, got.v) || memcmp(want.c, got.c, 4) != 0) {
				if (bad++ < 20)
					printf("DIRECT TL FUZZ: vtype %08x vertex %d differs: pos %g,%g,%g / %g,%g,%g nrm %g,%g,%g / %g,%g,%g uv %g,%g / %g,%g col %08x / %08x\n",
						vtype, i, want.x, want.y, want.z, got.x, got.y, got.z, want.nx, want.ny, want.nz, got.nx, got.ny, got.nz,
						want.u, want.v, got.u, got.v, *(uint32_t *)want.c, *(uint32_t *)got.c);
			}
		}
	}
	memcpy(gstate_c.morphWeights, savedMorph, sizeof(savedMorph));
	printf("DIRECT TL FUZZ: %d formats, %ld vertices, %d differ\n", iterations, checked, bad);
	gstate.texmapmode = savedTexmap;
	memcpy(gstate.boneMatrix, savedBones, sizeof(savedBones));
}
#endif

#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
void TLJobTestCapture(const Rasterizer::TLVertex *verts, int n, const uint16_t *indices, int nidx) {}

// Test build (PPSSPP_TLJOB_FUZZ=n): random direct T&L draws queued to the GE worker thread, drawn by a
// backend that records the triangles, compared with decoding the same vertices from memory directly.
class RecordingBackend : public Rasterizer::HardwareBackend {
public:
	bool SupportsTL() const override { return true; }
	bool DrawTL(const Rasterizer::RasterizerState &state, const Rasterizer::TLParams &p, const Rasterizer::TLVertex *verts, int nverts, const uint16_t *indices, int nidx, int cull) override {
		Draw d;
		for (int i = 0; i < nidx; ++i)
			d.tris.push_back(verts[indices[i]]);
		d.cull = cull;
		d.mvp0 = p.mvp[0];
		d.fbp = Rasterizer::g_drawStateOverride ? Rasterizer::g_drawStateOverride->fbptr : gstate.fbptr;
		draws.push_back(d);
		return true;
	}
	void Triangle(const Rasterizer::RasterizerState &, const VertexData &, const VertexData &, const VertexData &) override {}
	void Rect(const Rasterizer::RasterizerState &, const VertexData &, const VertexData &, bool) override {}
	void Line(const Rasterizer::RasterizerState &, const VertexData &, const VertexData &) override {}
	void Point(const Rasterizer::RasterizerState &, const VertexData &) override {}
	void Flush(const char *) override {}
	bool HasPendingWrite(uint32_t, uint32_t, uint32_t, uint32_t) override { return false; }
	struct Draw {
		std::vector<Rasterizer::TLVertex> tris;
		int cull;
		float mvp0;
		uint32_t fbp;
	};
	std::vector<Draw> draws;
};

static void TLJobFuzz(SoftwareDrawEngine *drawEngine, int iterations) {
	uint32_t rng = 4242;
	auto rnd = [&](uint32_t n) { rng = rng * 1103515245u + 12345u; return (uint32_t)((rng >> 8) % n); };
	const u32 savedTexmap = gstate.texmapmode, savedFbp = gstate.fbptr, savedClear = gstate.clearmode;
	gstate.clearmode = 0;
	float savedBones[96];
	memcpy(savedBones, gstate.boneMatrix, sizeof(savedBones));
	gstate.texmapmode &= ~3;
	RecordingBackend rec;
	// PPSSPP_TLJOB_FUZZ_SYNC: the same draws without the worker (gethread=0).
	const bool sync = getenv("PPSSPP_TLJOB_FUZZ_SYNC") != nullptr;
	Rasterizer::HardwareBackend *savedBackend = Rasterizer::g_hardwareBackend;
	Rasterizer::HardwareBackend *wrapped = sync ? &rec : Rasterizer::StartGEWorker(&rec);
	if (sync)
		Rasterizer::g_hardwareBackend = &rec;
	struct Want {
		std::vector<Rasterizer::TLVertex> tris;
		int cull;
		uint32_t fbp;
	};
	std::vector<Want> wants;
	// The source memory is overwritten after each draw is queued, as a game may do: the queued
	// copy must not see that.
	static u8 raw[256 * 300];
	static u8 idxBuf[1024];
	Rasterizer::RasterizerState state{};
	int bad = 0;
	for (int it = 0; it < iterations; ++it) {
		u32 vtype = rnd(4) | (rnd(5) == 0 ? 0 : (4 + rnd(4))) << 2 | rnd(4) << 5 | (1 + rnd(3)) << 7;
		if (rnd(2))
			vtype |= (1 + rnd(3)) << 9 | rnd(8) << 14;
		const int idxKind = rnd(3);  // none, 8-bit, 16-bit
		vtype |= idxKind << 11;
		for (int i = 0; i < 96; ++i)
			gstate.boneMatrix[i] = (float)((int)rnd(4001) - 2000) / 1000.0f;
		gstate.fbptr = rnd(0x10000) << 4;
		VertexDecoder *dec = drawEngine->FindVertexDecoder(vtype);
		const int nverts = 3 + rnd(200);
		for (int i = 0; i < dec->VertexSize() * nverts; ++i)
			raw[i] = (u8)rnd(256);
		for (int v = 0; v < nverts; ++v) {
			u8 *p = raw + v * dec->VertexSize();
			auto put = [&](int off, int n) { for (int k = 0; k < n; ++k) { float f = (float)((int)rnd(20001) - 10000) / 1000.0f; u32 b; memcpy(&b, &f, 4); for (int q = 0; q < 4; ++q) p[off + k * 4 + q] = (u8)(b >> (8 * q)); } };
			if (dec->weighttype == 3) put(dec->weightoff, dec->nweights);
			if (dec->tc == 3) put(dec->tcoff, 2);
			if (dec->nrm == 3) put(dec->nrmoff, 3);
			if (dec->pos == 3) put(dec->posoff, 3);
		}
		const int count = 3 + rnd(idxKind == 1 ? 200 : 300);
		const int base = rnd(nverts);
		std::vector<int> order(count);
		for (int i = 0; i < count; ++i) {
			order[i] = idxKind ? (int)((base + rnd(nverts)) % nverts) : (int)rnd(nverts);
			if (idxKind == 1) order[i] &= 0xFF;
			if (order[i] >= nverts) order[i] = nverts - 1;
		}
		int vcount = count;
		if (!idxKind)
			vcount = std::min(count, nverts);
		for (int i = 0; i < vcount; ++i) {
			if (idxKind == 1) idxBuf[i] = (u8)order[i];
			if (idxKind == 2) { idxBuf[i * 2] = (u8)order[i]; idxBuf[i * 2 + 1] = (u8)(order[i] >> 8); }
			if (!idxKind) order[i] = i;
		}
		const GEPrimitiveType prim = (GEPrimitiveType)(GE_PRIM_TRIANGLES + rnd(3));
		const CullType cull = (CullType)rnd(3);
		TransformState ts{};
		ts.readUV = dec->tc != 0;
		ts.negateNormals = rnd(2) != 0;
		// Reference: decode the vertices in GE order from memory now.
		TLDecodeSetup s;
		if (!SetupTLDecode(s, *dec, ts, raw)) {
			continue;
		}
		Want w;
		auto ref = [&](int i) {
			static float lastN[3];
			bool fa = true;
			Rasterizer::TLVertex o;
			lastN[0] = lastN[1] = lastN[2] = 0.0f;
			DecodeTLVertex(s, raw + order[i] * dec->VertexSize(), o, lastN, fa);
			w.tris.push_back(o);
		};
		switch (prim) {
		case GE_PRIM_TRIANGLES: for (int i = 0; i + 2 < vcount; i += 3) { ref(i); ref(i + 1); ref(i + 2); } break;
		case GE_PRIM_TRIANGLE_STRIP: for (int i = 0; i + 2 < vcount; ++i) { if (i & 1) { ref(i + 1); ref(i); ref(i + 2); } else { ref(i); ref(i + 1); ref(i + 2); } } break;
		default: for (int i = 2; i < vcount; ++i) { ref(0); ref(i - 1); ref(i); } break;
		}
		w.cull = cull == CullType::CCW ? 1 : (cull == CullType::CW ? 2 : 0);
		w.fbp = gstate.fbptr;
		int miss = 0;
		if (!DirectTLDraw(*dec, ts, state, raw, idxKind ? idxBuf : nullptr, vtype, prim, vcount, cull, miss)) {
			printf("TLJOB FUZZ: draw %d not taken (miss %d)\n", it, miss);
			continue;
		}
		wants.push_back(w);
		// Scribble over the source while the worker may still be on it.
		for (int i = 0; i < dec->VertexSize() * nverts; ++i)
			raw[i] = (u8)rnd(256);
		gstate.fbptr = 0xDEAD0;
	}
	wrapped->Sync();
	if (rec.draws.size() != wants.size()) {
		printf("TLJOB FUZZ: %d draws queued, %d drawn\n", (int)wants.size(), (int)rec.draws.size());
		bad++;
	}
	long verts = 0;
	for (size_t d = 0; d < std::min(rec.draws.size(), wants.size()); ++d) {
		const auto &got = rec.draws[d];
		const auto &want = wants[d];
		if (got.cull != want.cull || got.fbp != want.fbp || got.tris.size() != want.tris.size()) {
			if (bad++ < 20)
				printf("TLJOB FUZZ: draw %d: cull %d/%d fbp %x/%x count %d/%d\n", (int)d, got.cull, want.cull, got.fbp, want.fbp, (int)got.tris.size(), (int)want.tris.size());
			continue;
		}
		for (size_t i = 0; i < got.tris.size(); ++i) {
			const auto &a = want.tris[i], &b = got.tris[i];
			auto far = [](float x, float y) { if (std::isnan(x) || std::isnan(y)) return std::isnan(x) != std::isnan(y); return fabsf(x - y) > 1e-5f * std::max(1.0f, fabsf(x)); };
			++verts;
			// Normals are compared only when the format has them (otherwise carried over between draws).
			const bool nrm = (b.nx != a.nx || b.ny != a.ny || b.nz != a.nz) && !(a.nx == 0 && a.ny == 0 && a.nz == 0);
			if (far(a.x, b.x) || far(a.y, b.y) || far(a.z, b.z) || far(a.u, b.u) || far(a.v, b.v) || memcmp(a.c, b.c, 4) != 0 || (nrm && (far(a.nx, b.nx) || far(a.ny, b.ny) || far(a.nz, b.nz)))) {
				if (bad++ < 20)
					printf("TLJOB FUZZ: draw %d vertex %d differs: pos %g,%g,%g / %g,%g,%g uv %g,%g / %g,%g\n", (int)d, (int)i, a.x, a.y, a.z, b.x, b.y, b.z, a.u, a.v, b.u, b.v);
				break;
			}
		}
	}
	Rasterizer::StopGEWorker();
	Rasterizer::g_hardwareBackend = savedBackend;
	printf("TLJOB FUZZ: %d draws, %ld vertices compared, %d bad (%llu prepared by the kernel: %s)\n", (int)wants.size(), verts, bad, (unsigned long long)TLPrep::JobsDone(), TLPrep::Description());
	gstate.texmapmode = savedTexmap;
	gstate.fbptr = savedFbp;
	gstate.clearmode = savedClear;
	memcpy(gstate.boneMatrix, savedBones, sizeof(savedBones));
}
#endif

bool TransformUnit::FastMergePrim(const void *vertices, const void *indices, GEPrimitiveType prim, int vertexCount, u32 vertexType, int *bytesRead) {
	const FastMergeRecord &f = g_fastMerge;
#if !PPSSPP_PLATFORM(PS3)
	if (g_mergeCheck)
		return false;
#endif
	if (!f.valid || f.serial != Rasterizer::g_geStateSerial || f.vertexType != vertexType || f.prim != prim || !g_tlWorker || isImmDraw_ ||
		vertexCount < 3 || (gstate_c.skipDrawReason & SKIPDRAW_SKIPFRAME) != 0)
		return false;
	TLJob *open = g_tlWorker->OpenTL();
	if (open != f.job || open->geSerial != f.serial || ((uintptr_t)vertices & (f.align - 1)) != 0)
		return false;
	TLDecodeSetup s = f.s;
	s.base = (const u8 *)vertices;
	const u32 idxType = vertexType & GE_VTYPE_IDX_MASK;
	int lower, upper;
	if (!MergeRange(*open, s, indices, idxType, vertexCount, lower, upper))
		return false;
#if !PPSSPP_PLATFORM(PS3)
	{
		static const bool okLog = getenv("PPSSPP_MERGEOKLOG") != nullptr;
		if (okLog)
			fprintf(stderr, "MERGEOK fast prim %d n %d vtype %06x into job with %d indices\n", (int)prim, vertexCount, vertexType, (int)open->idx.size());
	}
#endif
	MergeAppend(*open, s, indices, idxType, prim, vertexCount, lower, upper);
#if PPSSPP_ARCH(POWERPC)
	PS3Profile::tlMerged++;
#endif
	PS3_MERGE_FAST();
	*bytesRead = vertexCount * s.size;
	data_index_ = 0;
	prev_prim_ = prim;
	hasDraws_ = true;
	return true;
}

void TransformUnit::SubmitPrimitive(const void* vertices, const void* indices, GEPrimitiveType prim_type, int vertex_count, u32 vertex_type, int *bytesRead, SoftwareDrawEngine *drawEngine)
{
	PS3_PROFILE(transform);
#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	static bool fuzzed = false;
	static const bool fuzzEnv = getenv("PPSSPP_DIRECT_TL_FUZZ") != nullptr;
	if (!fuzzed && fuzzEnv) {
		fuzzed = true;
		DirectTLFuzz(drawEngine, atoi(getenv("PPSSPP_DIRECT_TL_FUZZ")));
	}
	static bool jobFuzzed = false;
	static const bool jobFuzzEnv = getenv("PPSSPP_TLJOB_FUZZ") != nullptr;
	if (!jobFuzzed && jobFuzzEnv) {
		jobFuzzed = true;
		TLJobFuzz(drawEngine, atoi(getenv("PPSSPP_TLJOB_FUZZ")));
	}
#endif
	VertexDecoder &vdecoder = *drawEngine->FindVertexDecoder(vertex_type);

	if (bytesRead)
		*bytesRead = vertex_count * vdecoder.VertexSize();

	// Frame skipping.
	if (gstate_c.skipDrawReason & SKIPDRAW_SKIPFRAME) {
		return;
	}
	// Vertices without position are just entirely culled.
	// Note: Throughmode does draw 8-bit primitives, but positions are always zero - handled in decode.
	if ((vertex_type & GE_VTYPE_POS_MASK) == 0)
		return;

	static TransformState transformState;
	// RSX transform & lighting straight from PSP memory: no software decode at all.
	// A continued primitive restarts like on the batched path below (which also needs data_index_ 0).
	const GEPrimitiveType directPrim = prim_type == GE_PRIM_KEEP_PREVIOUS ? prev_prim_ : prim_type;
	if (Rasterizer::g_hardwareBackend && Rasterizer::g_hardwareBackend->SupportsTL() && (prim_type != GE_PRIM_KEEP_PREVIOUS || data_index_ == 0) && !isImmDraw_ &&
		(vertex_type & GE_VTYPE_THROUGH_MASK) == 0 && vertex_count >= 2) {
		{
			PS3_PROFILE_SAMPLED(geState);
			binner_->UpdateState();
		}
		if (binner_->HasDirty(SoftDirty::LIGHT_ALL | SoftDirty::TRANSFORM_ALL)) {
			PS3_PROFILE(geXformState);
			VertexReader stateReader(decoded_, vdecoder.GetDecVtxFmt(), vertex_type);
			ComputeTransformState(&transformState, stateReader);
			binner_->ClearDirty(SoftDirty::LIGHT_ALL | SoftDirty::TRANSFORM_ALL);
		}
		const bool skipCullTL = !gstate.isCullEnabled() || gstate.isModeClear();
		const CullType cullTL = skipCullTL ? CullType::OFF : (gstate.getCullMode() ? CullType::CCW : CullType::CW);
		int miss = 3;
		bool direct;
		{
			PS3_PROFILE_SAMPLED(geDirect);
#if !PPSSPP_PLATFORM(PS3)
			static const bool noDirectRects = getenv("PPSSPP_NO_DIRECTRECTS") != nullptr;  // Test builds: compare.
#else
			constexpr bool noDirectRects = false;
#endif
			if (directPrim == GE_PRIM_RECTANGLES) {
				direct = data_index_ == 0 && !noDirectRects && DirectRects(*binner_, vdecoder, transformState, vertices, indices, vertex_type, vertex_count);
				miss = 0;
			} else
				direct = DirectTLDraw(vdecoder, transformState, binner_->State(), vertices, indices, vertex_type, directPrim, vertex_count, cullTL, miss);
		}
		if (direct) {
			data_index_ = 0;
			prev_prim_ = directPrim;
			hasDraws_ = true;
			return;
		}
#if PPSSPP_ARCH(POWERPC)
		PS3Profile::tlMiss[miss]++;
#endif
	}
#if PPSSPP_ARCH(POWERPC)
	else if ((vertex_type & GE_VTYPE_THROUGH_MASK) == 0 && Rasterizer::g_hardwareBackend) {
		PS3Profile::tlMiss[3]++;
	}
#endif
#if PPSSPP_ARCH(POWERPC)
	PS3Profile::geCpuPrims++;
	PS3Profile::Scope cpuPathTimer(PS3Profile::geCpuPath);
#endif
#if !PPSSPP_PLATFORM(PS3)
	struct CpuPathLog {
		std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		int prim, count; u32 vtype;
		~CpuPathLog() {
			static const bool on = getenv("PPSSPP_CPUPATHLOG") != nullptr;
			if (on) fprintf(stderr, "CPUPATH prim %d n %d vtype %06x thru %d us %.1f\n", prim, count, vtype & 0xFFFFFF, (vtype & GE_VTYPE_THROUGH_MASK) ? 1 : 0,
				std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
		}
	} cpuPathLog{std::chrono::steady_clock::now(), (int)prim_type, vertex_count, vertex_type};
#endif
	SoftwareVertexReader vreader(decoded_, vdecoder, vertex_type, vertex_count, vertices, indices, transformState, *this);

	if (prim_type != GE_PRIM_KEEP_PREVIOUS) {
		data_index_ = 0;
		prev_prim_ = prim_type;
	} else {
		prim_type = prev_prim_;
	}

	binner_->UpdateState();
	hasDraws_ = true;

	if (binner_->HasDirty(SoftDirty::LIGHT_ALL | SoftDirty::TRANSFORM_ALL)) {
		ComputeTransformState(&transformState, vreader.GetVertexReader());
		binner_->ClearDirty(SoftDirty::LIGHT_ALL | SoftDirty::TRANSFORM_ALL);
	}
	bool skipCull = !gstate.isCullEnabled() || gstate.isModeClear();
	const CullType cullType = skipCull ? CullType::OFF : (gstate.getCullMode() ? CullType::CCW : CullType::CW);

#if defined(PPSSPP_NO_GL) && !PPSSPP_PLATFORM(PS3)
	const bool tryBatch = true;  // Test build: run the comparison.
#else
	const bool tryBatch = Rasterizer::g_hardwareBackend != nullptr;
#endif
	if (tryBatch && data_index_ == 0 && !isImmDraw_ &&
		HardwareBatch(*this, vreader, transformState, binner_->State(), prim_type, vertex_count, cullType)) {
		return;
	}
	// Full per-vertex transform of the whole indexed range (after the batched path, which does its
	// own and doesn't need this).
	vreader.UpdateCache();

	if (vreader.IsThrough() && cullType == CullType::OFF && prim_type == GE_PRIM_TRIANGLES && data_index_ == 0 && vertex_count >= 6 && ((vertex_count) % 6) == 0) {
		// Some games send rectangles as a series of regular triangles.
		// We look for this, but only in throughmode.
		ClipVertexData buf[6];
		// Could start at data_index_ and copy to buf, but there's little reason.
		int buf_index = 0;
		_assert_(data_index_ == 0);

		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			buf[buf_index++] = vreader.Read(vtx);
			if (buf_index < 6)
				continue;

			int tl = -1, br = -1;
			if (Rasterizer::DetectRectangleFromPair(binner_->State(), buf, &tl, &br) && Rasterizer::RectangleMatchesTriangles(binner_->State(), buf[tl].v, buf[br].v)) {
				Clipper::ProcessRect(buf[tl], buf[br], *binner_);
			} else {
				SendTriangle(cullType, &buf[0]);
				SendTriangle(cullType, &buf[3]);
			}

			buf_index = 0;
		}

		if (buf_index >= 3) {
			SendTriangle(cullType, &buf[0]);
			data_index_ = 0;
			for (int i = 3; i < buf_index; ++i) {
				data_[data_index_++] = buf[i];
			}
		} else if (buf_index > 0) {
			for (int i = 0; i < buf_index; ++i) {
				data_[i] = buf[i];
			}
			data_index_ = buf_index;
		} else {
			data_index_ = 0;
		}

		return;
	}

	// Note: intentionally, these allow for the case of vertex_count == 0, but data_index_ > 0.
	// This is used for immediate-mode primitives.
	switch (prim_type) {
	case GE_PRIM_POINTS:
		for (int i = 0; i < data_index_; ++i)
			Clipper::ProcessPoint(data_[i], *binner_);
		data_index_ = 0;
		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			data_[0] = vreader.Read(vtx);
			Clipper::ProcessPoint(data_[0], *binner_);
		}
		break;

	case GE_PRIM_LINES:
		for (int i = 0; i < data_index_ - 1; i += 2)
			Clipper::ProcessLine(data_[i + 0], data_[i + 1], *binner_);
		data_index_ &= 1;
		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			data_[data_index_++] = vreader.Read(vtx);
			if (data_index_ == 2) {
				Clipper::ProcessLine(data_[0], data_[1], *binner_);
				data_index_ = 0;
			}
		}
		break;

	case GE_PRIM_TRIANGLES:
		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			data_[data_index_++] = vreader.Read(vtx);
			if (data_index_ < 3) {
				// Keep reading.  Note: an incomplete prim will stay read for GE_PRIM_KEEP_PREVIOUS.
				continue;
			}
			// Okay, we've got enough verts.  Reset the index for next time.
			data_index_ = 0;

			SendTriangle(cullType, &data_[0]);
		}
		// In case vertex_count was 0.
		if (data_index_ >= 3) {
			SendTriangle(cullType, &data_[0]);
			data_index_ = 0;
		}
		break;

	case GE_PRIM_RECTANGLES:
		for (int vtx = 0; vtx < vertex_count; ++vtx) {
			data_[data_index_++] = vreader.Read(vtx);

			if (data_index_ == 4 && vreader.IsThrough() && cullType == CullType::OFF) {
				if (Rasterizer::DetectRectangleThroughModeSlices(binner_->State(), data_)) {
					data_[1] = data_[3];
					data_index_ = 2;
				}
			}

			if (data_index_ == 4) {
				Clipper::ProcessRect(data_[0], data_[1], *binner_);
				Clipper::ProcessRect(data_[2], data_[3], *binner_);
				data_index_ = 0;
			}
		}

		if (data_index_ >= 2) {
			Clipper::ProcessRect(data_[0], data_[1], *binner_);
			data_index_ -= 2;
		}
		break;

	case GE_PRIM_LINE_STRIP:
		{
			// Don't draw a line when loading the first vertex.
			// If data_index_ is 1 or 2, etc., it means we're continuing a line strip.
			int skip_count = data_index_ == 0 ? 1 : 0;
			for (int vtx = 0; vtx < vertex_count; ++vtx) {
				data_[(data_index_++) & 1] = vreader.Read(vtx);

				if (skip_count) {
					--skip_count;
				} else {
					// We already incremented data_index_, so data_index_ & 1 is previous one.
					Clipper::ProcessLine(data_[data_index_ & 1], data_[(data_index_ & 1) ^ 1], *binner_);
				}
			}
			// If this is from immediate-mode drawing, we always had one new vert (already in data_.)
			if (isImmDraw_ && data_index_ >= 2)
				Clipper::ProcessLine(data_[data_index_ & 1], data_[(data_index_ & 1) ^ 1], *binner_);
			break;
		}

	case GE_PRIM_TRIANGLE_STRIP:
		{
			// Don't draw a triangle when loading the first two vertices.
			int skip_count = data_index_ >= 2 ? 0 : 2 - data_index_;
			int start_vtx = 0;

			// If index count == 4, check if we can convert to a rectangle.
			// This is for Darkstalkers (and should speed up many 2D games).
			if (data_index_ == 0 && vertex_count >= 4 && (vertex_count & 1) == 0 && cullType == CullType::OFF) {
				for (int base = 0; base < vertex_count - 2; base += 2) {
					for (int vtx = base == 0 ? 0 : 2; vtx < 4; ++vtx) {
						data_[vtx] = vreader.Read(base + vtx);
					}

					// If a strip is effectively a rectangle, draw it as such!
					int tl = -1, br = -1;
					if (Rasterizer::DetectRectangleFromStrip(binner_->State(), data_, &tl, &br) && Rasterizer::RectangleMatchesTriangles(binner_->State(), data_[tl].v, data_[br].v)) {
						Clipper::ProcessRect(data_[tl], data_[br], *binner_);
						start_vtx += 2;
						skip_count = 2;
						if (base + 4 >= vertex_count) {
							start_vtx = vertex_count;
							break;
						}

						// Just copy the first two so we can detect easier.
						// TODO: Maybe should give detection two halves?
						data_[0] = data_[2];
						data_[1] = data_[3];
						data_index_ = 2;
					} else {
						// Go into triangle mode.  Unfortunately, we re-read the verts.
						break;
					}
				}
			}

			for (int vtx = start_vtx; vtx < vertex_count && skip_count > 0; ++vtx) {
				int provoking_index = (data_index_++) % 3;
				data_[provoking_index] = vreader.Read(vtx);
				--skip_count;
				++start_vtx;
			}

			for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
				int provoking_index = (data_index_++) % 3;
				data_[provoking_index] = vreader.Read(vtx);

				int wind = (data_index_ - 1) % 2;
				CullType altCullType = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
				// Odd triangles reach us in the opposite order from the GE's (gpu/probe exp45).
				SendTriangle(altCullType, &data_[0], provoking_index, wind != 0);
			}

			// If this is from immediate-mode drawing, we always had one new vert (already in data_.)
			if (isImmDraw_ && data_index_ >= 3) {
				int provoking_index = (data_index_ - 1) % 3;
				int wind = (data_index_ - 1) % 2;
				CullType altCullType = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
				// Odd triangles reach us in the opposite order from the GE's (gpu/probe exp45).
				SendTriangle(altCullType, &data_[0], provoking_index, wind != 0);
			}
			break;
		}

	case GE_PRIM_TRIANGLE_FAN:
		{
			// Don't draw a triangle when loading the first two vertices.
			// (this doesn't count the central one.)
			int skip_count = data_index_ <= 1 ? 1 : 0;
			int start_vtx = 0;

			// Only read the central vertex if we're not continuing.
			if (data_index_ == 0 && vertex_count > 0) {
				data_[0] = vreader.Read(0);
				data_index_++;
				start_vtx = 1;
			}

			if (data_index_ == 1 && vertex_count == 4 && cullType == CullType::OFF) {
				for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
					data_[vtx] = vreader.Read(vtx);
				}

				int tl = -1, br = -1;
				if (Rasterizer::DetectRectangleFromFan(binner_->State(), data_, &tl, &br) && Rasterizer::RectangleMatchesTriangles(binner_->State(), data_[tl].v, data_[br].v)) {
					Clipper::ProcessRect(data_[tl], data_[br], *binner_);
					break;
				}
			}

			for (int vtx = start_vtx; vtx < vertex_count && skip_count > 0; ++vtx) {
				int provoking_index = 2 - ((data_index_++) % 2);
				data_[provoking_index] = vreader.Read(vtx);
				--skip_count;
				++start_vtx;
			}

			for (int vtx = start_vtx; vtx < vertex_count; ++vtx) {
				int provoking_index = 2 - ((data_index_++) % 2);
				data_[provoking_index] = vreader.Read(vtx);

				int wind = (data_index_ - 1) % 2;
				CullType altCullType = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
				// Odd triangles reach us in the opposite order from the GE's (gpu/probe exp45).
				SendTriangle(altCullType, &data_[0], provoking_index, wind != 0);
			}

			// If this is from immediate-mode drawing, we always had one new vert (already in data_.)
			if (isImmDraw_ && data_index_ >= 3) {
				int wind = (data_index_ - 1) % 2;
				int provoking_index = 2 - wind;
				CullType altCullType = cullType == CullType::OFF ? cullType : CullType((int)cullType ^ wind);
				SendTriangle(altCullType, &data_[0], provoking_index, wind != 0);
			}
			break;
		}

	default:
		ERROR_LOG(Log::G3D, "Unexpected prim type: %d", prim_type);
		break;
	}
}

void TransformUnit::SubmitImmVertex(const ClipVertexData &vert, SoftwareDrawEngine *drawEngine) {
	// Where we put it is different for STRIP/FAN types.
	switch (prev_prim_) {
	case GE_PRIM_POINTS:
	case GE_PRIM_LINES:
	case GE_PRIM_TRIANGLES:
	case GE_PRIM_RECTANGLES:
		// This is the easy one.  SubmitPrimitive resets data_index_.
		data_[data_index_++] = vert;
		break;

	case GE_PRIM_LINE_STRIP:
		// This one alternates, and data_index_ > 0 means it draws a segment.
		data_[(data_index_++) & 1] = vert;
		break;

	case GE_PRIM_TRIANGLE_STRIP:
		data_[(data_index_++) % 3] = vert;
		break;

	case GE_PRIM_TRIANGLE_FAN:
		if (data_index_ == 0) {
			data_[data_index_++] = vert;
		} else {
			int provoking_index = 2 - ((data_index_++) % 2);
			data_[provoking_index] = vert;
		}
		break;

	default:
		_assert_msg_(false, "Invalid prim type: %d", (int)prev_prim_);
		break;
	}

	// Hm, shouldn't we mask away the position bits of the vertType?
	uint32_t vertTypeID = GetVertTypeID(gstate.vertType | GE_VTYPE_POS_FLOAT, gstate.getUVGenMode());
	// This now processes the step with shared logic, given the existing data_.
	isImmDraw_ = true;
	Clipper::SetCullXY(false);
	SubmitPrimitive(nullptr, nullptr, GE_PRIM_KEEP_PREVIOUS, 0, vertTypeID, nullptr, drawEngine);
	Clipper::SetCullXY(true);
	isImmDraw_ = false;
}

void TransformUnit::SendTriangle(CullType cullType, const ClipVertexData *verts, int provoking, bool orderReversed) {
	if (cullType == CullType::OFF) {
		Clipper::ProcessTriangle(verts[0], verts[1], verts[2], verts[provoking], *binner_, orderReversed);
		Clipper::ProcessTriangle(verts[2], verts[1], verts[0], verts[provoking], *binner_, !orderReversed);
	} else if (cullType == CullType::CW) {
		Clipper::ProcessTriangle(verts[2], verts[1], verts[0], verts[provoking], *binner_, !orderReversed);
	} else {
		Clipper::ProcessTriangle(verts[0], verts[1], verts[2], verts[provoking], *binner_, orderReversed);
	}
}

void TransformUnit::Flush(GPUCommon *common, const char *reason) {
	if (!hasDraws_)
		return;

	binner_->Flush(reason);
	common->NotifyFlush();
	// A block transfer's flush writes back only the buffers over its memory (g_flushRange): with
	// others still drawn-only, later reads of them must still flush.
	hasDraws_ = Rasterizer::g_hardwareBackend && Rasterizer::g_flushRange.end > Rasterizer::g_flushRange.start &&
		Rasterizer::g_hardwareBackend->HasPendingWrite(0x04000000, 0, 0x200000, 1);
}

void TransformUnit::GetStats(StringWriter &w) {
	// TODO: More stats?
	binner_->GetStats(w);
}

void TransformUnit::FlushIfOverlap(GPUCommon *common, const char *reason, bool modifying, uint32_t addr, uint32_t stride, uint32_t w, uint32_t h) {
	if (!hasDraws_)
		return;

#if !PPSSPP_PLATFORM(PS3)
	// Test builds: PPSSPP_NO_XFERRANGE=1 writes back everything as before (to compare).
	static const bool noRange = getenv("PPSSPP_NO_XFERRANGE") != nullptr;
#else
	constexpr bool noRange = false;
#endif
	if (!noRange) {
		Rasterizer::g_flushRange.start = addr;
		Rasterizer::g_flushRange.end = addr + stride * (h ? h - 1 : 0) + w;
		Rasterizer::g_flushRange.stride = stride;
		Rasterizer::g_flushRange.width = w;
	}
	if (binner_->HasPendingWrite(addr, stride, w, h))
		Flush(common, reason);
	if (modifying && binner_->HasPendingRead(addr, stride, w, h))
		Flush(common, reason);
	Rasterizer::g_flushRange = Rasterizer::FlushRange();
}

bool TransformUnit::HasPendingWrite(uint32_t addr, uint32_t stride, uint32_t w, uint32_t h) {
	return hasDraws_ && binner_->HasPendingWrite(addr, stride, w, h);
}

void TransformUnit::NotifyTexFlush() {
	binner_->NotifyTexFlush();
}

void TransformUnit::NotifyClutUpdate(const void *src) {
	binner_->UpdateClut(src);
}

#if !PPSSPP_PLATFORM(PS3)
// Test builds: PPSSPP_NULL_BACKEND=1 installs a hardware backend that takes every draw and does
// nothing (=2: behind the GE worker thread), to time the emulation-side cost of a GE dump replay.
namespace {
class NullBackend : public Rasterizer::HardwareBackend {
public:
	bool SupportsTL() const override { return true; }
	bool DrawTL(const Rasterizer::RasterizerState &st, const Rasterizer::TLParams &tp, const Rasterizer::TLVertex *verts, int, const uint16_t *indices, int nidx, int cull) override {
		// Checksum of what would be drawn (for comparing merged and unmerged jobs).
		for (int i = 0; i < nidx; ++i) {
			const Rasterizer::TLVertex &v = verts[indices[i]];
			sum_ += v.x * 1.1 + v.y * 1.3 + v.z * 1.7 + v.u * 0.7 + v.v * 0.9 + v.c[0] + tp.mvp[0] + cull;
		}
		{
			// And of the state each index is drawn with (as the backend sees it).
			alignas(16) Rasterizer::RasterizerState n;
			memcpy((void *)&n, (const void *)&st, sizeof(n));
			Rasterizer::NormalizeForHardware(n);
			n.cpuEpoch = 0;
			n.texptr[0] = nullptr;  // Host address: differs between runs.
			uint32_t h = 2166136261u;
			const uint8_t *b = (const uint8_t *)&n;
			for (size_t i = 0; i < sizeof(n); ++i)
				h = (h ^ b[i]) * 16777619u;
			stateSum_ += (double)(h % 9973) * nidx;
		}
		idx_ += nidx;
		tlCalls_++;
		// PPSSPP_FETCHDUMP=dir: each texture as the RSX renderer's slow path decodes it (Sampler fetch).
		static const char *fetchDir = getenv("PPSSPP_FETCHDUMP");
		if (fetchDir && st.enableTextures && st.texptr[0]) {
			static std::set<uint32_t> seen;
			if (seen.insert(st.texaddr[0]).second) {
				Sampler::FetchFunc fetch = Sampler::GetFetchFunc(st.samplerID, nullptr);
				const int w = st.samplerID.cached.sizes[0].w, h = st.samplerID.cached.sizes[0].h;
				char path[512];
				snprintf(path, sizeof(path), "%s/fetch_%08x_fmt%d.ppm", fetchDir, st.texaddr[0], (int)st.samplerID.TexFmt());
				FILE *f = fopen(path, "wb");
				if (f && fetch) {
					fprintf(f, "P6 %d %d 255\n", w, h);
					int alpha0 = 0;
					for (int y = 0; y < h; ++y)
						for (int x = 0; x < w; ++x) {
							const uint32_t rgba = Math3D::Vec4<int>(fetch(x, y, st.texptr[0], st.texbufw[0], 0, st.samplerID)).ToRGBA();
							const uint8_t px[3] = { (uint8_t)rgba, (uint8_t)(rgba >> 8), (uint8_t)(rgba >> 16) };
							if ((rgba >> 24) < 0x21) alpha0++;
							fwrite(px, 1, 3, f);
						}
					fprintf(stderr, "FETCH %08x fmt %d %dx%d: %d texels with alpha <= 0x20\n", st.texaddr[0], (int)st.samplerID.TexFmt(), w, h, alpha0);
					snprintf(path, sizeof(path), "%s/fetch_%08x_fmt%d_alpha.pgm", fetchDir, st.texaddr[0], (int)st.samplerID.TexFmt());
					if (FILE *fa = fopen(path, "wb")) {
						fprintf(fa, "P5 %d %d 255\n", w, h);
						for (int y = 0; y < h; ++y)
							for (int x = 0; x < w; ++x) {
								const uint32_t rgba = Math3D::Vec4<int>(fetch(x, y, st.texptr[0], st.texbufw[0], 0, st.samplerID)).ToRGBA();
								fputc((int)(rgba >> 24), fa);
							}
						fclose(fa);
					}
				}
				if (f) fclose(f);
			}
		}
		// Same test as the RSX renderer's ApplyState memo, to see how often it would hit.
		const uint8_t *clut = st.samplerID.cached.clut;
		Rasterizer::RasterizerState n;
		memcpy((void *)&n, &st, sizeof(n));
		Rasterizer::NormalizeForHardware(n);
		const uint8_t *a = (const uint8_t *)&n, *b = (const uint8_t *)&last_;
		bool same = memcmp(a, b, sizeof(n)) == 0 && (!clut || (hasClut_ && memcmp(clut, lastClut_, 1024) == 0));
		if (same) hits_++; else {
			misses_++;
			if (getenv("PPSSPP_MEMO_DIFF")) {
				for (size_t i = 0; i < sizeof(n); ++i) if (a[i] != b[i]) { fprintf(stderr, "MEMO: miss, first differing byte %d\n", (int)i); break; }
			}
		}
		memcpy((void *)&last_, &n, sizeof(n));
		hasClut_ = clut != nullptr;
		if (clut) memcpy(lastClut_, clut, 1024);
		if (((hits_ + misses_) & 4095) == 0) fprintf(stderr, "MEMO: %d hits %d misses\n", hits_, misses_);
		if (getenv("PPSSPP_TLSUM")) fprintf(stderr, "TLSUM: %d calls %lld indices sum %.6e state %.0f\n", tlCalls_, (long long)idx_, sum_, stateSum_);
		return true;
	}
	Rasterizer::RasterizerState last_{};
	uint8_t lastClut_[1024];
	bool hasClut_ = false;
	int hits_ = 0, misses_ = 0;
	double sum_ = 0.0;
	double stateSum_ = 0.0;
	long long idx_ = 0;
	int tlCalls_ = 0;
	bool DrawTriangles(const Rasterizer::RasterizerState &, const Rasterizer::FastVertex *, int, bool) override { return true; }
	bool DrawIndexed(const Rasterizer::RasterizerState &, const Rasterizer::FastVertex *, int, const uint16_t *, int, bool) override { return true; }
	void Triangle(const Rasterizer::RasterizerState &, const VertexData &, const VertexData &, const VertexData &) override {}
	void Rect(const Rasterizer::RasterizerState &st, const VertexData &v0, const VertexData &v1, bool clear) override {
		static const bool log = getenv("PPSSPP_RECTLOG") != nullptr;
		if (log)
			fprintf(stderr, "RECT #%u z %d tex %08x %s xy %.1f,%.1f - %.1f,%.1f st %.2f,%.2f - %.2f,%.2f clear %d\n", Rasterizer::g_drawSerial, (int)v1.screenpos.z, st.enableTextures ? st.texaddr[0] : 0, st.throughMode ? "thru" : "3d",
				v0.screenpos.x / 16.0f, v0.screenpos.y / 16.0f, v1.screenpos.x / 16.0f, v1.screenpos.y / 16.0f, v0.texturecoords.s(), v0.texturecoords.t(), v1.texturecoords.s(), v1.texturecoords.t(), clear ? 1 : 0);
	}
	void Line(const Rasterizer::RasterizerState &, const VertexData &, const VertexData &) override {}
	void Point(const Rasterizer::RasterizerState &, const VertexData &) override {}
	void Flush(const char *) override {}
	bool HasPendingWrite(uint32_t, uint32_t, uint32_t, uint32_t) override { return false; }
};
}

#ifdef PPSSPP_RSX_HOSTSIM
Rasterizer::HardwareBackend *HostSimRSXCreate(bool worker);
#endif

void InstallBenchBackendFromEnv() {
#ifdef PPSSPP_RSX_HOSTSIM
	if (const char *sim = getenv("PPSSPP_RSX_SIM")) {
		Rasterizer::g_hardwareBackend = HostSimRSXCreate(atoi(sim) == 2);
		return;
	}
#endif
	const char *v = getenv("PPSSPP_NULL_BACKEND");
	if (!v)
		return;
	static NullBackend nullBackend;
	fprintf(stderr, "BENCH: sizeof RasterizerState %d, GEState %d, VertexData %d, TLJob %d, TLParams %d\n", (int)sizeof(Rasterizer::RasterizerState), (int)sizeof(GEState), (int)sizeof(VertexData), (int)sizeof(TLJob), (int)sizeof(Rasterizer::TLParams));
	Rasterizer::g_hardwareBackend = atoi(v) == 2 ? Rasterizer::StartGEWorker(&nullBackend) : &nullBackend;
}
#endif
