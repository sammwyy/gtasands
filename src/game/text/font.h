#pragma once

#include <stdint.h>

#include "assets/texture.h"
#include "game/text/gxt.h"

// CFont port (SA): glyph quads from font1/font2, proportional widths from
// data/fonts.dat, GXT color codes, drop shadow.
enum eFontStyle : uint8_t { FONT_GOTHIC, FONT_SUBTITLES, FONT_MENU, FONT_PRICEDOWN };
enum class eFontTextureName : uint8_t { FONT2 = 0, FONT1 = 1 };
enum class eFontTextureStyle : uint8_t { SUBTITLES_AND_GOTHIC = 0, PRICEDOWN = 1, MENU = 2 };
enum eFontAlignment : uint8_t { ALIGN_CENTER, ALIGN_LEFT, ALIGN_RIGHT };

namespace font {

void init();
void shutdown();
void attachFontTextures(const tex::Texture* font2, const tex::Texture* font1);

void setFontStyle(eFontStyle style);
void setOrientation(eFontAlignment a);
void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
void setDropColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
void setEdge(uint8_t n);
void setDrop(uint8_t n);
void setScale(float w, float h);
void setProportional(bool b);
void setWrapx(float x);
void setRightJustifyWrap(float x);
void setCentreSize(float x);
void setBackground(bool bg, bool outline);
void setAlphaFade(float a);

void printChar(float x, float y, uint8_t letter);
void printString(float x, float y, const GxtChar* text);
void printStringFromBottom(float x, float y, const GxtChar* text);
float getStringWidth(const GxtChar* text, bool spaced = true);
float getNumberLines(float x, float y, const GxtChar* text);

}
