#pragma once

#include <nds.h>

// Software renderer for the bottom screen: draws into a RAM backbuffer that
// is DMA'd to the sub engine's B16 (15-bit + alpha flag) bitmap background
// during vblank. The bitmap lives at the start of VRAM bank C (0x06200000);
// only the first 192 rows are visible. Everything the game HUD needs (map,
// text, icons, bars) is rendered to RAM by the CPU — no palettes, no sprite
// banks, and no VRAM writes outside of the blanking interval.
namespace subfb {

constexpr int W = 256, H = 192, STRIDE = 256;

// aBBBBBGGGGGRRRRR, bit15 set = opaque
constexpr inline u16 rgb(int r, int g, int b) {
	return (u16)(RGB15(r >> 3, g >> 3, b >> 3) | BIT(15));
}

void init();              // MODE_5_2D + B16 256x256 on BG3
u16* frame();             // RAM backbuffer; frame[y*STRIDE+x]
// Call once per frame inside vblank: flushes + DMAs the backbuffer into the
// VRAM C bitmap (~0.6ms). The CPU never touches VRAM -> zero tearing.
void present();
void clear(u16 c);

void pixel(int x, int y, u16 c);
void rect(int x, int y, int w, int h, u16 c);
void rectBlend(int x, int y, int w, int h, u16 c, u8 a); // src-over fill, a in 0..255
void hline(int x, int y, int w, u16 c);
void circle(int cx, int cy, int r, u16 c);               // 1px outline

// Sprite decoded to RAM: two planes, color (rgb15) and alpha (0..255).
struct Sprite {
	u8 w, h;
	const u16* col;
	const u8* alpha;
};

void blit(const Sprite& s, int x, int y);                 // src-over
void blitScaled(const Sprite& s, int x, int y, int w, int h); // nearest-sample scale
void blitRot(const Sprite& s, int cx, int cy, int brad);  // rotated around center, brad = -32768..32767 (128 = 45deg)

// Coverage text: glyph masks (u8, 0..255) blended toward a color.
void mask(int x, int y, int w, int h, const u8* cov, int pitch, u16 c);

}
