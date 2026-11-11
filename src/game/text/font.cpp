#include "game/text/font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/sprite2d.h"

namespace font {

struct FontData {
	uint8_t prop[210]; // 208 values + spaceValue + unpropValue
};

static FontData s_data[2];
static const tex::Texture* s_tex[2]; // [0]=font2 (menu), [1]=font1 (gothic/pricedown)
static bool s_ok;

static struct {
	uint8_t r, g, b, a;
	uint8_t dr, dg, db, da;
	float w, h;
	uint8_t edge, drop;
	eFontAlignment align;
	eFontTextureName tex;
	eFontTextureStyle style;
	bool prop;
	float wrapx, rjwrap, centre;
	uint8_t space;
} rs;

static void loadFontValues(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return;
	char line[128];
	int fontId = -1, pline = 0;
	while (fgets(line, sizeof line, f)) {
		if (*line == '#' || *line == '\n' || *line == '\r') continue;
		if (!strncmp(line, "[FONT_ID]", 9)) {
			fgets(line, sizeof line, f);
			fontId = atoi(line);
			pline = 0;
		} else if (!strncmp(line, "[PROP]", 6) && fontId >= 0) {
	
			for (int i = 0; i < 26 && fgets(line, sizeof line, f); i++) {
				uint8_t* d = s_data[fontId].prop + i * 8;
				(void)sscanf(line, "%hhu %hhu %hhu %hhu %hhu %hhu %hhu %hhu",
				             d, d + 1, d + 2, d + 3, d + 4, d + 5, d + 6, d + 7);
			}
		} else if (!strncmp(line, "[UNPROP]", 8) && fontId >= 0) {
			fgets(line, sizeof line, f);
			s_data[fontId].prop[209] = (uint8_t)atoi(line);
		} else if (!strncmp(line, "[REPLACEMENT_SPACE_CHAR]", 24) && fontId >= 0) {
			fgets(line, sizeof line, f);
			s_data[fontId].prop[208] = (uint8_t)atoi(line);
		}
		(void)pline;
	}
	fclose(f);
}

void init() {
	loadFontValues("game:/data/fonts.dat");
	for (FontData& d : s_data)
		if (!d.prop[0]) for (uint8_t& v : d.prop) v = 12;
	s_data[0].prop[208] = 10;
	s_data[1].prop[208] = 10;
	s_ok = true;
}

void shutdown() {
	s_ok = false;
}

void setFontStyle(eFontStyle style) {
	switch (style) {
	case FONT_MENU:      rs.tex = eFontTextureName::FONT2; rs.style = eFontTextureStyle::MENU; break;
	case FONT_PRICEDOWN: rs.tex = eFontTextureName::FONT1; rs.style = eFontTextureStyle::PRICEDOWN; break;
	default:             rs.tex = eFontTextureName::FONT1; rs.style = eFontTextureStyle::SUBTITLES_AND_GOTHIC; break;
	}
}
void setOrientation(eFontAlignment a) { rs.align = a; }
void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { rs.r = r; rs.g = g; rs.b = b; rs.a = a; }
void setDropColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { rs.dr = r; rs.dg = g; rs.db = b; rs.da = a; }
void setEdge(uint8_t n) { rs.edge = n; }
void setDrop(uint8_t n) { rs.drop = n; }
void setScale(float w, float h) { rs.w = w; rs.h = h; }
void setProportional(bool b) { rs.prop = b; }
void setWrapx(float x) { rs.wrapx = x; }
void setRightJustifyWrap(float x) { rs.rjwrap = x; }
void setCentreSize(float x) { rs.centre = x; }
void setBackground(bool, bool) {}
void setAlphaFade(float) {}

static uint8_t findSubFontChar(uint8_t letter, eFontTextureStyle style) {
	if (style == eFontTextureStyle::PRICEDOWN) {
		switch (letter) {
		case 1: return 208;
		case 4: return 93;
		case 7: return 206;
		case 8:
		case 9: return letter + 86;
		case 14: return 207;
		case 26: return 154;
		}
	}
	if (letter == 6) return 10;
	if (letter >= 16 && letter <= 25) return letter + 128;
	return letter;
}

static float propValue(uint8_t letter) {
	const FontData& d = s_data[(int)rs.tex];
	return (rs.prop ? d.prop[letter] : d.prop[209]) * rs.w;
}

void printChar(float x, float y, uint8_t letter) {
	const tex::Texture* t = s_tex[(int)rs.tex];
	if (!t) return;
	const float u1 = (letter % 16) / 16.0f;
	const float v19 = (letter / 16) / 12.8f;
	const float u2 = u1 + 0.0615f;
	sprite::Rect rt;
	float v1, v3;
	if (letter < 0xC0) {
		rt = { x, y + 20.0f * rs.h, x + 32.0f * rs.w, y };
		v1 = v19 + 0.0021f;
		v3 = v19 + 0.076025f;
	} else {
		rt = { x, y + 16.0f * rs.h, x + 32.0f * rs.w, y };
		v1 = v19 + 0.0021f;
		v3 = v19 + 0.078125f - 0.016f;
	}
	sprite::drawUV(t, rt, u1, v1, u2, v3, { rs.r, rs.g, rs.b, rs.a });
}

// ~x~ codes: w/s white, g green, b blue, h highlighted, p purple, o orange, r red, l black, t gray, n default
static void codeColor(char c, uint8_t& r, uint8_t& g, uint8_t& b) {
	struct { char c; uint8_t r, g, b; } m[] = {
		{ 's', 255, 255, 255 }, { 'h', 255, 255, 255 }, { 'w', 175, 175, 175 }, { 'g', 26, 154, 96 },
		{ 'b', 74, 90, 107 }, { 'p', 131, 67, 161 }, { 'o', 233, 145, 21 }, { 'r', 150, 49, 48 },
		{ 'l', 0, 0, 0 }, { 't', 150, 150, 150 }, { 'n', rs.r, rs.g, rs.b },
	};
	for (auto& e : m)
		if (e.c == c) { r = e.r; g = e.g; b = e.b; return; }
}

static void printLine(float x, float y, const GxtChar* s) {
	uint8_t r = rs.r, g = rs.g, b = rs.b;
	if (rs.drop || rs.edge) {
		float ox = rs.edge * rs.w + rs.drop, oy = rs.edge * rs.h + rs.drop;
		uint8_t sr = rs.r, sg = rs.g, sb = rs.b;
		rs.r = rs.dr; rs.g = rs.dg; rs.b = rs.db;
		float px = x;
		for (const GxtChar* p = s; *p; p++) {
			if (*p == '~' && p[2] == '~') { p += 2; continue; }
			uint8_t letter = *p - 32;
			uint8_t id = rs.style != eFontTextureStyle::SUBTITLES_AND_GOTHIC ? findSubFontChar(letter, rs.style) : letter;
			if (*p != ' ') printChar(px + ox, y + oy, id);
			px += propValue(id);
		}
		rs.r = sr; rs.g = sg; rs.b = sb;
	}
	float px = x;
	for (const GxtChar* p = s; *p; p++) {
		if (*p == '~' && p[2] == '~') {
			codeColor((char)p[1], r, g, b);
			p += 2;
			continue;
		}
		uint8_t letter = *p - 32;
		uint8_t id = rs.style != eFontTextureStyle::SUBTITLES_AND_GOTHIC ? findSubFontChar(letter, rs.style) : letter;
		uint8_t sr = rs.r, sg = rs.g, sb = rs.b;
		rs.r = r; rs.g = g; rs.b = b;
		if (*p != ' ') printChar(px, y, id);
		rs.r = sr; rs.g = sg; rs.b = sb;
		px += propValue(id);
	}
}

float getStringWidth(const GxtChar* text, bool) {
	float w = 0;
	for (const GxtChar* p = text; *p; p++) {
		if (*p == '~' && p[2] == '~') { p += 2; continue; }
		uint8_t letter = *p - 32;
		uint8_t id = rs.style != eFontTextureStyle::SUBTITLES_AND_GOTHIC ? findSubFontChar(letter, rs.style) : letter;
		w += propValue(id);
	}
	return w;
}

float getNumberLines(float, float, const GxtChar*) { return 1; }

void printString(float x, float y, const GxtChar* text) {
	if (!s_ok) init();
	if (rs.align == ALIGN_CENTER) x -= getStringWidth(text) / 2;
	else if (rs.align == ALIGN_RIGHT) x -= getStringWidth(text);
	printLine(x, y, text);
}

void printStringFromBottom(float x, float y, const GxtChar* text) {
	printString(x, y - 18.0f * rs.h, text);
}

void attachFontTextures(const tex::Texture* font2, const tex::Texture* font1) {
	s_tex[0] = font2;
	s_tex[1] = font1;
}

}
