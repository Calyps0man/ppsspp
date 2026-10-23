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

#include "Canvas.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Canvas {

static uint32_t *pixels;
static uint32_t *backdrop;
static const uint8_t *smallFont;
static const uint8_t *largeFont;

// A ribbon of light: its centre line is two sine waves drifting at different speeds.
struct Ribbon {
	float base, amp1, len1, speed1, amp2, len2, speed2, phase;
	int width;  // Glow radius (1/e) in pixels.
	int peak;   // Brightness added at the centre line.
	uint8_t r, g, b;
};

static const Ribbon ribbons[] = {
	{455.0f, 42.0f, 1150.0f, 0.30f, 16.0f, 530.0f, -0.21f, 0.0f, 54, 40, 110, 180, 255},
	{490.0f, 58.0f, 1550.0f, -0.23f, 14.0f, 400.0f, 0.29f, 1.7f, 80, 28, 150, 160, 255},
	{530.0f, 30.0f, 900.0f, 0.17f, 20.0f, 620.0f, 0.13f, 3.1f, 26, 34, 170, 225, 255},
};
enum { RIBBONS = sizeof(ribbons) / sizeof(ribbons[0]), MAX_REACH = 220 };
// Per ribbon, the RGB added at each distance from the centre line (fixed point, 8 bits).
static int16_t glow[RIBBONS][MAX_REACH + 1][3];
static int reach[RIBBONS];

enum { DOT = 9, DOTS = 36 };
static uint8_t dotKernel[DOT * DOT];

static inline int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static inline uint32_t Rgb(int r, int g, int b) {
	return 0xFF000000u | ((uint32_t)Clamp255(r) << 16) | ((uint32_t)Clamp255(g) << 8) | (uint32_t)Clamp255(b);
}

static void BuildBackdrop() {
	// Vertical gradient plus two soft glows (each separable: exp(-(dx²+dy²)) = exp(-dx²) * exp(-dy²)).
	static float gx1[W], gy1[H], gx2[W], gy2[H];
	for (int x = 0; x < W; ++x) {
		const float d1 = (x - 240.0f) / 560.0f, d2 = (x - 1120.0f) / 470.0f;
		gx1[x] = expf(-d1 * d1);
		gx2[x] = expf(-d2 * d2);
	}
	for (int y = 0; y < H; ++y) {
		const float d1 = (y + 140.0f) / 560.0f, d2 = (y - 800.0f) / 470.0f;
		gy1[y] = expf(-d1 * d1);
		gy2[y] = expf(-d2 * d2);
	}
	uint32_t seed = 0x9E3779B9u;
	for (int y = 0; y < H; ++y) {
		const float t = (float)y / (H - 1);
		const float s = t * t * (3.0f - 2.0f * t);
		const float baseR = 12.0f + (5.0f - 12.0f) * s;
		const float baseG = 34.0f + (11.0f - 34.0f) * s;
		const float baseB = 78.0f + (30.0f - 78.0f) * s;
		uint32_t *row = backdrop + y * W;
		for (int x = 0; x < W; ++x) {
			const float a = gx1[x] * gy1[y], b = gx2[x] * gy2[y];
			// Small noise so the gradient doesn't show 8-bit bands on a TV.
			seed = seed * 1664525u + 1013904223u;
			const float n = (float)(seed >> 24) / 255.0f - 0.5f;
			row[x] = Rgb((int)(baseR + 34.0f * a + 46.0f * b + n + 0.5f),
				(int)(baseG + 62.0f * a + 14.0f * b + n + 0.5f),
				(int)(baseB + 92.0f * a + 66.0f * b + n + 0.5f));
		}
	}
	for (int i = 0; i < RIBBONS; ++i) {
		const Ribbon &rb = ribbons[i];
		reach[i] = rb.width * 5 / 2 > MAX_REACH ? MAX_REACH : rb.width * 5 / 2;
		for (int d = 0; d <= MAX_REACH; ++d) {
			const float q = (float)d / rb.width;
			float v = rb.peak * expf(-q * q);
			if (d <= 1) v += 34.0f - 14.0f * d;  // Thin bright core.
			for (int c = 0; c < 3; ++c) {
				const int channel = c == 0 ? rb.r : (c == 1 ? rb.g : rb.b);
				glow[i][d][c] = d > reach[i] ? 0 : (int16_t)(v * channel / 255.0f * 256.0f);
			}
		}
	}
	for (int y = 0; y < DOT; ++y) {
		for (int x = 0; x < DOT; ++x) {
			const float dx = (x - DOT / 2) / 2.2f, dy = (y - DOT / 2) / 2.2f;
			dotKernel[y * DOT + x] = (uint8_t)(255.0f * expf(-(dx * dx + dy * dy)));
		}
	}
}

bool Init(const uint8_t *smallAtlas, const uint8_t *largeAtlas) {
	if (pixels)
		return true;
	smallFont = smallAtlas;
	largeFont = largeAtlas;
	pixels = (uint32_t *)malloc(W * H * 4);
	backdrop = (uint32_t *)malloc(W * H * 4);
	if (!pixels || !backdrop || !smallFont) {
		Release();
		return false;
	}
	BuildBackdrop();
	memcpy(pixels, backdrop, W * H * 4);
	return true;
}

void Release() {
	free(pixels);
	free(backdrop);
	pixels = nullptr;
	backdrop = nullptr;
}

bool Ready() { return pixels != nullptr; }
const uint32_t *Pixels() { return pixels; }

static inline void AddRgb(uint32_t *p, int r, int g, int b) {
	const uint32_t c = *p;
	r += (c >> 16) & 0xFF; g += (c >> 8) & 0xFF; b += c & 0xFF;
	*p = 0xFF000000u | ((uint32_t)(r > 255 ? 255 : r) << 16) | ((uint32_t)(g > 255 ? 255 : g) << 8) | (uint32_t)(b > 255 ? 255 : b);
}

static inline void Blend(uint32_t *p, uint32_t color, int alpha) {
	const uint32_t c = *p;
	const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
	const int cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
	*p = 0xFF000000u | ((uint32_t)(r + (((cr - r) * alpha) >> 8)) << 16) | ((uint32_t)(g + (((cg - g) * alpha) >> 8)) << 8) | (uint32_t)(b + (((cb - b) * alpha) >> 8));
}

void Background(double seconds) {
	if (!pixels)
		return;
	memcpy(pixels, backdrop, W * H * 4);
	const float t = (float)seconds;
	const float twoPi = 6.2831853f;
	for (int i = 0; i < RIBBONS; ++i) {
		const Ribbon &rb = ribbons[i];
		const int r = reach[i];
		for (int x = 0; x < W; ++x) {
			const float centre = rb.base + rb.amp1 * sinf(twoPi * x / rb.len1 + rb.speed1 * t + rb.phase)
				+ rb.amp2 * sinf(twoPi * x / rb.len2 + rb.speed2 * t);
			// Brighter in the middle of the screen, fading towards the edges and breathing slowly.
			const float edge = sinf(3.1415926f * x / W);
			const int scale = (int)(256.0f * edge * (0.75f + 0.25f * sinf(t * 0.5f + x * 0.004f + rb.phase)));
			const int c = (int)centre;
			int y0 = c - r, y1 = c + r;
			if (y0 < 0) y0 = 0;
			if (y1 > H - 1) y1 = H - 1;
			uint32_t *p = pixels + y0 * W + x;
			for (int y = y0; y <= y1; ++y, p += W) {
				const int16_t *g = glow[i][y > c ? y - c : c - y];
				AddRgb(p, (g[0] * scale) >> 16, (g[1] * scale) >> 16, (g[2] * scale) >> 16);
			}
		}
	}
	// Specks of light drifting upwards.
	for (int i = 0; i < DOTS; ++i) {
		const float fx = (float)((i * 7919) % 1000) / 1000.0f, fy = (float)((i * 104729) % 1000) / 1000.0f;
		const float speed = 6.0f + 10.0f * (float)((i * 31) % 17) / 17.0f;
		float y = fy * (H + 40) - speed * t;
		y = fmodf(y, (float)(H + 40));
		if (y < 0) y += H + 40;
		const int px = (int)(fx * W + 18.0f * sinf(t * 0.3f + i)) - DOT / 2;
		const int py = (int)y - 20 - DOT / 2;
		const int bright = (int)(70.0f + 60.0f * sinf(t * (0.8f + 0.05f * i) + i * 1.3f));
		for (int dy = 0; dy < DOT; ++dy) {
			const int yy = py + dy;
			if (yy < 0 || yy >= H) continue;
			for (int dx = 0; dx < DOT; ++dx) {
				const int xx = px + dx;
				if (xx < 0 || xx >= W) continue;
				const int k = (dotKernel[dy * DOT + dx] * bright) >> 8;
				if (k) AddRgb(pixels + yy * W + xx, k * 3 / 4, k * 7 / 8, k);
			}
		}
	}
}

// Horizontal inset of a rounded corner at row dy (0 = the top row) for radius r.
static int CornerInset(int dy, int r) {
	if (dy >= r) return 0;
	const float q = (float)(r - dy) - 0.5f;
	return r - (int)sqrtf((float)r * r - q * q);
}

void Panel(int x, int y, int w, int h, int shade) {
	if (!pixels) return;
	const int radius = 10;
	const uint32_t tint = 0x060A16;
	for (int row = 0; row < h; ++row) {
		const int yy = y + row;
		if (yy < 0 || yy >= H) continue;
		const int inset = CornerInset(row < h / 2 ? row : h - 1 - row, radius);
		int x0 = x + inset, x1 = x + w - inset;
		if (x0 < 0) x0 = 0;
		if (x1 > W) x1 = W;
		uint32_t *p = pixels + yy * W;
		for (int xx = x0; xx < x1; ++xx)
			Blend(p + xx, tint, shade);
		// A faint light edge along the top.
		if (row == 0 || row == 1) {
			for (int xx = x0; xx < x1; ++xx)
				Blend(p + xx, 0x9FC8FF, row == 0 ? 40 : 18);
		}
	}
}

void Bar(int x, int y, int w, int h, uint32_t color, int alpha) {
	if (!pixels) return;
	const int radius = h / 3 < 12 ? h / 3 : 12;
	for (int row = 0; row < h; ++row) {
		const int yy = y + row;
		if (yy < 0 || yy >= H) continue;
		const int inset = CornerInset(row < h / 2 ? row : h - 1 - row, radius);
		int x0 = x + inset, x1 = x + w - inset;
		if (x0 < 0) x0 = 0;
		if (x1 > W) x1 = W;
		// Slightly brighter towards the top: reads as a raised bar.
		int a = alpha + ((h / 2 - row) * alpha) / (2 * h);
		// Over 256 the blend overshoots and spills into the other colour channels.
		if (a > 256) a = 256;
		if (a < 0) a = 0;
		uint32_t *p = pixels + yy * W;
		for (int xx = x0; xx < x1; ++xx)
			Blend(p + xx, color, a);
	}
}

void Fill(int x, int y, int w, int h, uint32_t color) {
	if (!pixels) return;
	int x0 = x < 0 ? 0 : x, x1 = x + w > W ? W : x + w;
	for (int row = 0; row < h; ++row) {
		const int yy = y + row;
		if (yy < 0 || yy >= H) continue;
		uint32_t *p = pixels + yy * W;
		for (int xx = x0; xx < x1; ++xx)
			p[xx] = 0xFF000000u | color;
	}
}

void Image(int x, int y, int w, int h, const uint32_t *argb) {
	if (!pixels || !argb) return;
	for (int row = 0; row < h; ++row) {
		const int yy = y + row;
		if (yy < 0 || yy >= H) continue;
		const uint32_t *src = argb + row * w;
		uint32_t *p = pixels + yy * W;
		for (int col = 0; col < w; ++col) {
			const int xx = x + col;
			if (xx < 0 || xx >= W) continue;
			const uint32_t c = src[col];
			const int a = (int)(c >> 24);
			if (a == 255)
				p[xx] = c;
			else if (a)
				Blend(p + xx, c & 0xFFFFFF, a + (a >> 7));
		}
	}
}

void Outline(int x, int y, int w, int h, int thickness, uint32_t color, int alpha) {
	if (!pixels) return;
	for (int row = 0; row < h; ++row) {
		const int yy = y + row;
		if (yy < 0 || yy >= H) continue;
		uint32_t *p = pixels + yy * W;
		const bool full = row < thickness || row >= h - thickness;
		for (int col = 0; col < w; ++col) {
			if (!full && col >= thickness && col < w - thickness) {
				col = w - thickness - 1;
				continue;
			}
			const int xx = x + col;
			if (xx >= 0 && xx < W)
				Blend(p + xx, color, alpha);
		}
	}
}

static void Glyph(int x, int y, unsigned c, uint32_t color, int alphaScale, bool large) {
	const int cw = large ? 24 : 12, ch = large ? 48 : 24;
	const bool doubled = large && !largeFont;
	const uint8_t *atlas = large && largeFont ? largeFont : smallFont;
	const int atlasW = large && largeFont ? 384 : 192;
	const int srcW = large && largeFont ? 24 : 12, srcH = large && largeFont ? 48 : 24;
	const int sx = (int)(c % 16) * srcW, sy = (int)(c / 16) * srcH;
	for (int dy = 0; dy < ch; ++dy) {
		const int yy = y + dy;
		if (yy < 0 || yy >= H) continue;
		const uint8_t *src = atlas + (sy + (doubled ? dy / 2 : dy)) * atlasW + sx;
		uint32_t *p = pixels + yy * W;
		for (int dx = 0; dx < cw; ++dx) {
			const int xx = x + dx;
			if (xx < 0 || xx >= W) continue;
			const int a = src[doubled ? dx / 2 : dx];
			if (a)
				Blend(p + xx, color, (a * alphaScale) >> 8);
		}
	}
}

int TextWidth(const char *text, bool large) {
	return (int)strlen(text) * CharWidth(large);
}

int Text(int x, int y, const char *text, uint32_t color, bool large) {
	if (!pixels) return 0;
	const int cw = CharWidth(large);
	const int shadow = large ? 3 : 2;
	int cx = x;
	for (const char *s = text; *s; ++s, cx += cw) {
		unsigned c = (unsigned char)*s;
		if (c < 32 || c > 126) c = '?';
		if (c != ' ')
			Glyph(cx + shadow, y + shadow, c - 32, 0x000000, 150, large);
	}
	cx = x;
	for (const char *s = text; *s; ++s, cx += cw) {
		unsigned c = (unsigned char)*s;
		if (c < 32 || c > 126) c = '?';
		if (c != ' ')
			Glyph(cx, y, c - 32, color & 0xFFFFFF, 256, large);
	}
	return cx - x;
}

}  // namespace Canvas
