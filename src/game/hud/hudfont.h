#pragma once

#include <nds.h>

#include "game/text/gxt.h"

// CFont for the bottom screen: same glyph math as game/text/font.cpp (16-col
// grid, 32x20 cells, fonts.dat prop widths, ~color codes~) but rasterizes
// coverage maps into the software framebuffer instead of main-engine quads.
namespace hudfont {

// mirrors eFontStyle: FONT_MENU->font2, FONT_PRICEDOWN/FONT_SUBTITLES->font1
enum Style : u8 { STYLE_GOTHIC = 0, STYLE_PRICEDOWN = 1, STYLE_MENU = 2 };
enum Align : u8 { LEFT = 0, CENTER = 1, RIGHT = 2 };

bool init();   // loads fonts.txd coverage + fonts.dat; returns success

// x,y are fb pixels; sw,sh are virtual-640 scales like the original
// (glyph quad = 32*sw x 20*sh px on screen). alpha multiplies coverage.
void print(Style st, int x, int y, const GxtChar* s, float sw, float sh,
           u16 color, Align a = LEFT, int edge = 0, u16 drop = 0, u8 alpha = 255);
float width(Style st, const GxtChar* s, float sw);

}
