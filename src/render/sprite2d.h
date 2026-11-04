#pragma once

#include <nds.h>

#include "assets/texture.h"

// CSprite2d port: 2D ortho drawing on the main engine (top screen).
// Coordinates are virtual 640x480, stretched like the PC frontend.
namespace sprite {

struct Rect {
	float left, top, right, bottom;
};

struct RGBA {
	u8 r, g, b, a;
	constexpr RGBA(u8 r, u8 g, u8 b, u8 a = 255) : r(r), g(g), b(b), a(a) {}
};

void beginFrame();  // sets ortho 0..640 x 0..480
void endFrame();

void draw(const tex::Texture* t, const Rect& rt, RGBA color);
void drawUV(const tex::Texture* t, const Rect& rt, float u1, float v1, float u2, float v2, RGBA color);
void rect(const Rect& rt, RGBA color);

}
