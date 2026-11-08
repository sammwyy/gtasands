#include "game/hud/subfb.h"

#include <string.h>

namespace subfb {

static u16 s_buf[W * H] __attribute__((aligned(4)));  // RAM backbuffer (96KB)
static u16* s_vram;        // B16 bitmap target in VRAM C
static bool s_dirty = true;   // written since the last present
static inline u16* fb() { s_dirty = true; return s_buf; }

void init() {
	videoSetModeSub(MODE_5_2D);
	int id = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	bgSetPriority(id, 0);
	s_vram = bgGetGfxPtr(id);
	s_dirty = true;
}

u16* frame() { return fb(); }

// Inside vblank only: flush D-cache then DMA the 192 visible rows into the
// bitmap. ~0.6ms, well inside the ~4.4ms blanking interval; skipped when
// nothing drew (the dirty-region HUD standing still, a static menu screen).
void present() {
	if (!s_dirty) return;
	s_dirty = false;
	DC_FlushRange(s_buf, sizeof s_buf);
	dmaCopy(s_buf, s_vram, sizeof s_buf);
}

void clear(u16 c) {
	u16* p = fb();
	// 32-bit fill: two identical pixels per word
	u32 w = (u32)c | ((u32)c << 16);
	for (int i = 0; i < W * H / 2; i++) ((u32*)p)[i] = w;
}

void pixel(int x, int y, u16 c) {
	if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H)
		fb()[y * STRIDE + x] = c;
}

void hline(int x, int y, int w, u16 c) {
	if ((unsigned)y >= (unsigned)H || w <= 0) return;
	if (x < 0) { w += x; x = 0; }
	if (x + w > W) w = W - x;
	if (w <= 0) return;
	u16* p = fb() + y * STRIDE + x;
	for (int i = 0; i < w; i++) p[i] = c;
}

void rect(int x, int y, int w, int h, u16 c) {
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > W) w = W - x;
	if (y + h > H) h = H - y;
	if (w <= 0 || h <= 0) return;
	const u32 w2 = (u32)c | ((u32)c << 16);
	for (int j = 0; j < h; j++) {
		u16* p = fb() + (y + j) * STRIDE + x;
		int n = w;
		if ((uintptr_t)p & 2) { *p++ = c; n--; }      // align to a word
		u32* q = (u32*)p;
		for (; n >= 2; n -= 2) *q++ = w2;           // two pixels per store
		if (n) *(u16*)q = c;
	}
}

static inline u16 blend(u16 dst, u16 src, u8 a) {
	// a in 0..256 domain
	if (a >= 255) return src;
	if (!a) return dst;
	const int dr = dst & 31, dg = (dst >> 5) & 31, db = (dst >> 10) & 31;
	const int sr = src & 31, sg = (src >> 5) & 31, sb = (src >> 10) & 31;
	return (u16)(((dr * (256 - a) + sr * a) >> 8) |
	           (((dg * (256 - a) + sg * a) >> 8) << 5) |
	           (((db * (256 - a) + sb * a) >> 8) << 10) | BIT(15));
}

void rectBlend(int x, int y, int w, int h, u16 c, u8 a) {
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > W) w = W - x;
	if (y + h > H) h = H - y;
	if (w <= 0 || h <= 0) return;
	for (int j = 0; j < h; j++) {
		u16* p = fb() + (y + j) * STRIDE + x;
		for (int i = 0; i < w; i++) p[i] = blend(p[i], c, a);
	}
}

void circle(int cx, int cy, int r, u16 c) {
	int x = r, y = 0, d = 1 - r;
	while (x >= y) {
		pixel(cx + x, cy + y, c); pixel(cx - x, cy + y, c);
		pixel(cx + x, cy - y, c); pixel(cx - x, cy - y, c);
		pixel(cx + y, cy + x, c); pixel(cx - y, cy + x, c);
		pixel(cx + y, cy - x, c); pixel(cx - y, cy - x, c);
		y++;
		if (d < 0) d += 2 * y + 1;
		else { x--; d += 2 * (y - x) + 1; }
	}
}

void blit(const Sprite& s, int x, int y) {
	for (int j = 0; j < s.h; j++) {
		const int dy = y + j;
		if ((unsigned)dy >= (unsigned)H) continue;
		u16* dst = fb() + dy * STRIDE;
		for (int i = 0; i < s.w; i++) {
			const int dx = x + i;
			if ((unsigned)dx >= (unsigned)W) continue;
			const u8 a = s.alpha[j * s.w + i];
			if (a) dst[dx] = blend(dst[dx], s.col[j * s.w + i], a);
		}
	}
}

void blitScaled(const Sprite& s, int x, int y, int w, int h) {
	// 16.16 src stepping: adds, not per-pixel divides
	const u32 xStep = ((u32)s.w << 16) / (u32)w;
	const u32 yStep = ((u32)s.h << 16) / (u32)h;
	u32 jy = 0;
	for (int j = 0; j < h; j++) {
		const int dy = y + j;
		const int sy = (int)(jy >> 16);
		jy += yStep;
		if ((unsigned)dy >= (unsigned)H) continue;
		const u16* crow = s.col + sy * s.w;
		const u8* arow = s.alpha + sy * s.w;
		u16* dst = fb() + dy * STRIDE;
		u32 ix = 0;
		for (int i = 0; i < w; i++) {
			const int dx = x + i;
			const int sx = (int)(ix >> 16);
			ix += xStep;
			if ((unsigned)dx >= (unsigned)W) continue;
			const u8 a = arow[sx];
			if (a) dst[dx] = blend(dst[dx], crow[sx], a);
		}
	}
}

void blitRot(const Sprite& s, int cx, int cy, int brad) {
	// inverse-map each dst pixel of the sprite's bbox through the rotation
	const int sn = sinLerp(brad), cs = cosLerp(brad); // 12.4 fixed
	const int hw = s.w / 2, hh = s.h / 2, r = hw > hh ? hw : hh;
	for (int j = -r; j <= r; j++) {
		const int dy = cy + j;
		if ((unsigned)dy >= (unsigned)H) continue;
		for (int i = -r; i <= r; i++) {
			const int dx = cx + i;
			if ((unsigned)dx >= (unsigned)W) continue;
			// rotate (i,j) back into sprite space (brad rotates counter-clockwise)
			const int sx = ( i * cs + j * sn) / 4096 + hw;
			const int sy = (-i * sn + j * cs) / 4096 + hh;
			if ((unsigned)sx >= (unsigned)s.w || (unsigned)sy >= (unsigned)s.h) continue;
			const u8 a = s.alpha[sy * s.w + sx];
			if (a) {
				u16* dst = fb() + dy * STRIDE + dx;
				*dst = blend(*dst, s.col[sy * s.w + sx], a);
			}
		}
	}
}

void mask(int x, int y, int w, int h, const u8* cov, int pitch, u16 c, u8 ga) {
	// pre-clip once: the inner loop runs per glyph stamp, so no per-pixel
	// bounds checks in here. ga multiplies the coverage (name fades reuse
	// the cached glyph maps without re-rasterizing per alpha step)
	const int x0 = x < 0 ? 0 : x, x1 = x + w > W ? W : x + w;
	const int y0 = y < 0 ? 0 : y, y1 = y + h > H ? H : y + h;
	for (int j = y0; j < y1; j++) {
		u16* dst = fb() + j * STRIDE;
		const u8* row = cov + (j - y) * pitch;
		for (int i = x0; i < x1; i++) {
			const int a = (row[i - x] * ga) >> 8;
			if (a) dst[i] = blend(dst[i], c, (u8)a);
		}
	}
}

}
