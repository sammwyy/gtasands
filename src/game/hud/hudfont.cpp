#include "game/hud/hudfont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/ds_formats.h"
#include "game/hud/hudtex.h"
#include "game/hud/subfb.h"

namespace hudfont {

static u8* s_cov[2];            // coverage maps: [0]=font2, [1]=font1
static u8 s_prop[2][210];
static bool s_ok;

// same parser as font.cpp::loadFontValues
static void loadProp(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return;
	char line[128];
	int id = -1;
	while (fgets(line, sizeof line, f)) {
		if (*line == '#' || *line == '\n' || *line == '\r') continue;
		if (!strncmp(line, "[FONT_ID]", 9)) {
			fgets(line, sizeof line, f);
			id = atoi(line);
		} else if (!strncmp(line, "[PROP]", 6) && id >= 0) {
			for (int i = 0; i < 26 && fgets(line, sizeof line, f); i++)
				(void)sscanf(line, "%hhu %hhu %hhu %hhu %hhu %hhu %hhu %hhu",
				             s_prop[id] + i * 8, s_prop[id] + i * 8 + 1, s_prop[id] + i * 8 + 2,
				             s_prop[id] + i * 8 + 3, s_prop[id] + i * 8 + 4, s_prop[id] + i * 8 + 5,
				             s_prop[id] + i * 8 + 6, s_prop[id] + i * 8 + 7);
		} else if (!strncmp(line, "[UNPROP]", 8) && id >= 0) {
			fgets(line, sizeof line, f);
			s_prop[id][209] = (u8)atoi(line);
		} else if (!strncmp(line, "[REPLACEMENT_SPACE_CHAR]", 24) && id >= 0) {
			fgets(line, sizeof line, f);
			s_prop[id][208] = (u8)atoi(line);
		}
	}
	fclose(f);
}

bool init() {
	if (s_ok) return true;
	void* txd = hudtex::readFile("nitro:/models/fonts.txd");
	if (!txd) return false;
	s_cov[0] = hudtex::decodeCoverage(hudtex::find(txd, "font2"));
	s_cov[1] = hudtex::decodeCoverage(hudtex::find(txd, "font1"));
	free(txd);
	if (!s_cov[0] || !s_cov[1]) return false;
	loadProp("nitro:/data/fonts.dat");
	for (int i = 0; i < 2; i++)
		if (!s_prop[i][0]) for (u8& v : s_prop[i]) v = 12;
	s_prop[0][208] = s_prop[1][208] = 10;
	s_ok = true;
	return true;
}

// texture index + sub-font remap for a style (mirrors font.cpp)
static int texOf(Style st) { return st == STYLE_MENU ? 0 : 1; }

static u8 remap(Style st, u8 letter) {
	if (st == STYLE_PRICEDOWN) {
		switch (letter) {
		case 1: return 208;
		case 4: return 93;
		case 7: return 206;
		case 8: case 9: return letter + 86;
		case 14: return 207;
		case 26: return 154;
		}
	}
	if (letter == 6) return 10;
	if (letter >= 16 && letter <= 25) return letter + 128;
	return letter;
}

static float prop(Style st, u8 id, float sw) { return s_prop[texOf(st)][id > 209 ? 209 : id] * sw; }

// draw one glyph: cell 16x20 in the 256x256 coverage map, quad 32*sw x 20*sh
static void glyph(int fi, u8 id, int x, int y, float sw, float sh, u16 c, u8 alpha) {
	const u8* cov = s_cov[fi];
	const int sx = (id % 16) * 16 + 1;         // ~0.0021 src margin like the original
	const int sy = (id / 16) * 20;
	if (sy >= 256) return;                     // glyph rows past the texture edge
	const int dstW = (int)(32.0f * sw);
	const int dstH = (int)((id < 0xC0 ? 20.0f : 16.0f) * sh);
	const int srcW = 15, srcH0 = id < 0xC0 ? 19 : 16;
	const int srcH = srcH0 < 256 - sy ? srcH0 : 256 - sy; // last row is clipped
	if (dstW <= 0 || dstH <= 0 || srcH <= 0) return;
	for (int j = 0; j < dstH; j++) {
		const int gy = y + j;
		if ((unsigned)gy >= (unsigned)subfb::H) continue;
		const u8* row = cov + (sy + (j * srcH) / dstH) * 256 + sx;
		u16* dst = subfb::frame() + gy * subfb::STRIDE;
		for (int i = 0; i < dstW; i++) {
			const int gx = x + i;
			if ((unsigned)gx >= (unsigned)subfb::W) continue;
			const u8 a = (u8)(row[(i * srcW) / dstW] * alpha >> 8);
			if (!a) continue;
			const u16 d = dst[gx];
			const int dr = d & 31, dg = (d >> 5) & 31, db = (d >> 10) & 31;
			const int sr = c & 31, sg = (c >> 5) & 31, sb = (c >> 10) & 31;
			dst[gx] = (u16)(((dr * (256 - a) + sr * a) >> 8) |
			                (((dg * (256 - a) + sg * a) >> 8) << 5) |
			                (((db * (256 - a) + sb * a) >> 8) << 10) | BIT(15));
		}
	}
}

static void codeColor(char ch, u8& r, u8& g, u8& b) {
	struct { char c; u8 r, g, b; } m[] = {
		{ 's', 255, 255, 255 }, { 'h', 255, 255, 255 }, { 'w', 175, 175, 175 }, { 'g', 26, 154, 96 },
		{ 'b', 74, 90, 107 }, { 'p', 131, 67, 161 }, { 'o', 233, 145, 21 }, { 'r', 150, 49, 48 },
		{ 'l', 0, 0, 0 }, { 't', 150, 150, 150 },
	};
	for (auto& e : m)
		if (e.c == ch) { r = e.r; g = e.g; b = e.b; return; }
}

static float pass(Style st, int x, int y, const GxtChar* s, float sw, float sh, u16 c, u8 alpha, bool colorCodes) {
	const int fi = texOf(st);
	u8 r = (c & 31) << 3, g = (c & 0x3E0) >> 2, b = (c & 0x7C00) >> 7;
	float px = (float)x;
	for (const GxtChar* p = s; *p; p++) {
		if (*p == '~' && p[2] == '~') {
			if (colorCodes) {
				codeColor((char)p[1], r, g, b);
				c = subfb::rgb(r, g, b);
			}
			p += 2;
			continue;
		}
		u8 letter = *p - 32;
		u8 id = remap(st, letter);
		if (px >= -32.0f && px < 288.0f) glyph(fi, id, (int)px, y, sw, sh, c, alpha);
		px += prop(st, id, sw);
	}
	return px;
}

float width(Style st, const GxtChar* s, float sw) {
	float w = 0;
	for (const GxtChar* p = s; *p; p++) {
		if (*p == '~' && p[2] == '~') { p += 2; continue; }
		w += prop(st, remap(st, (u8)(*p - 32)), sw);
	}
	return w;
}

void print(Style st, int x, int y, const GxtChar* s, float sw, float sh,
           u16 color, Align a, int edge, u16 drop, u8 alpha) {
	if (!s_ok && !init()) return;
	if (a == CENTER) {
		x -= (int)(width(st, s, sw) / 2);
	} else if (a == RIGHT) {
		float w = width(st, s, sw);
		// the last glyph's quad (32*sw) overshoots its advance: anchor the
		// visible edge on x, otherwise right-aligned text clips offscreen
		const GxtChar* lp = nullptr;
		for (const GxtChar* p = s; *p; p++) {
			if (*p == '~' && p[2] == '~') { p += 2; continue; }
			lp = p;
		}
		const float lastAdv = lp ? prop(st, remap(st, (u8)(*lp - 32)), sw) : 0;
		x -= (int)(w - lastAdv + 32.0f * sw);
	}
	if (edge) { // drop-shadow pass, like CFont's edge
		const int ox = (int)(edge * sw), oy = (int)(edge * sh);
		pass(st, x + ox, y + oy, s, sw, sh, drop, alpha, false);
	}
	pass(st, x, y, s, sw, sh, color, alpha, true);
}

}
