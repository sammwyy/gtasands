#include "game/hud/hudfont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/ds_formats.h"
#include "assets/txdfile.h"
#include "game/hud/hudtex.h"
#include "game/hud/subfb.h"
#include "mem/mem.h"

namespace hudfont {

static u8* s_cov[2];            // coverage maps: [0]=font2, [1]=font1
static u8 s_prop[2][210];
static bool s_ok;
static constexpr u32 ARENA_BYTES = 72 * 1024;
static u8* s_arena;             // glyph cache: fill (w*h) then stroke ((w+2e)*(h+2e)), PERM

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

// One font atlas per call (each is a 64 KB decode), then the metrics: the
// loading screen spreads them over frames. The atlases and the glyph cache
// live in PERM: they are never freed.
static TxdFile s_txd;
static int s_initStep;

bool initStep() {
	if (s_ok) return true;
	if (s_initStep == 0 && !s_txd.open("game:/models/fonts.txd")) return false;
	if (s_initStep <= 1) {
		mem::Scratch scratch;
		const int fi = s_initStep;   // [0]=font2, [1]=font1
		if (!s_cov[fi]) s_cov[fi] = hudtex::decodeCoverage(s_txd.load(fi == 0 ? "font2" : "font1", scratch));
		if (!s_cov[fi]) return false;
		s_initStep++;
		return false;
	}
	s_txd.close();
	if (!s_arena) s_arena = (u8*)mem::perm(ARENA_BYTES, mem::TAG_HUD);
	if (!s_arena) return false;
	loadProp("game:/data/fonts.dat");
	for (int i = 0; i < 2; i++)
		if (!s_prop[i][0]) for (u8& v : s_prop[i]) v = 12;
	s_prop[0][208] = s_prop[1][208] = 10;
	s_ok = true;
	return true;
}

bool init() {
	for (int i = 0; i < 4 && !s_ok; i++)
		if (!initStep() && s_initStep == 0) return false;
	return s_ok;
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

// ---- glyph coverage cache ---------------------------------------------------
// Per (font, glyph, dstW, dstH, edge): the fill coverage plus the edge-dilated
// stroke coverage, built once and reused every frame. The old path re-ran the
// source resample per glyph per frame and stamped the outline as 8 shifted
// masks — the union of those stamps is exactly a max-filter dilation, so one
// dilated stamp produces the same outline in ~1/8 of the pixel work.
static u8 s_covTmp[24 * 28];                 // fill raster while caching misses
static u8 s_strTmp[32 * 32];                 // stroke raster while caching misses

struct CEntry { u32 off; u16 w, h; u8 fi, id, edge; };
static CEntry s_ent[80];
static int s_nent;
static u32 s_arenaUsed;

// rasterize glyph coverage (no alpha — mask applies it per stamp)
static void rasterCov(int fi, u8 id, int dstW, int dstH, u8* dst) {
	const u8* cov = s_cov[fi];
	const int sx = (id % 16) * 16 + 1;         // ~0.0021 src margin like the original
	const int sy = (id / 16) * 20;
	const int srcW = 15, srcH0 = id < 0xC0 ? 19 : 16;
	const int srcH = sy >= 256 ? 0 : (srcH0 < 256 - sy ? srcH0 : 256 - sy);
	if (srcH <= 0) { memset(dst, 0, (size_t)(dstW * dstH)); return; }
	const u32 xStep = ((u32)srcW << 16) / (u32)dstW; // 16.16 src steps, adds not divs
	const u32 yStep = ((u32)srcH << 16) / (u32)dstH;
	u32 jy = 0;
	for (int j = 0; j < dstH; j++) {
		const u8* row = cov + (sy + (int)(jy >> 16)) * 256 + sx;
		u8* t = dst + j * dstW;
		u32 ix = 0;
		for (int i = 0; i < dstW; i++) {
			t[i] = row[ix >> 16];
			ix += xStep;
		}
		jy += yStep;
	}
}

// stroke coverage = fill dilated by `edge` px ((2e+1)^2 max filter). Runs
// once per cached glyph shape, so the naive O(w*h*e^2) walk is fine.
static void dilate(const u8* cov, int w, int h, int e, u8* out) {
	const int sw = w + 2 * e, sh = h + 2 * e;
	for (int j = 0; j < sh; j++)
		for (int i = 0; i < sw; i++) {
			u8 m = 0;
			const int fj = j - e, fi = i - e;
			for (int dy = -e; dy <= e; dy++) {
				const int y = fj + dy;
				if ((unsigned)y >= (unsigned)h) continue;
				const u8* r = cov + y * w;
				for (int dx = -e; dx <= e; dx++) {
					const int x = fi + dx;
					if ((unsigned)x < (unsigned)w && r[x] > m) m = r[x];
				}
			}
			out[j * sw + i] = m;
		}
}

// draw one glyph: cell 16x20 in the 256x256 coverage map, quad 32*sw x 20*sh.
// edge>0 stamps the drop-colored dilated stroke first, then the fill on top.
static void glyph(int fi, u8 id, int x, int y, float sw, float sh,
                  u16 c, u16 drop, u8 alpha, int edge) {
	const int dstW = (int)(32.0f * sw);
	const int dstH = (int)((id < 0xC0 ? 20.0f : 16.0f) * sh);
	const int swd = dstW + 2 * edge, shd = dstH + 2 * edge;
	if (dstW <= 0 || dstH <= 0 || dstW * dstH > (int)sizeof s_covTmp ||
	    swd > 32 || shd > 32) return;

	const u8* fill = nullptr;
	const u8* stroke = nullptr;
	for (int i = 0; i < s_nent; i++) {
		const CEntry& e = s_ent[i];
		if (e.fi == fi && e.id == id && e.w == dstW && e.h == dstH && e.edge == edge) {
			fill = s_arena + e.off;
			stroke = edge ? s_arena + e.off + dstW * dstH : nullptr;
			break;
		}
	}
	if (!fill) {
		rasterCov(fi, id, dstW, dstH, s_covTmp);
		if (edge) dilate(s_covTmp, dstW, dstH, edge, s_strTmp);
		const int need = dstW * dstH + (edge ? swd * shd : 0);
		if (s_nent < (int)(sizeof s_ent / sizeof s_ent[0]) &&
		    s_arenaUsed + (u32)need <= ARENA_BYTES) {
			CEntry& e = s_ent[s_nent++];
			e = { s_arenaUsed, (u16)dstW, (u16)dstH, (u8)fi, id, (u8)edge };
			memcpy(s_arena + e.off, s_covTmp, (size_t)(dstW * dstH));
			if (edge) memcpy(s_arena + e.off + dstW * dstH, s_strTmp, (size_t)(swd * shd));
			s_arenaUsed += (u32)need;
			fill = s_arena + e.off;
			stroke = edge ? s_arena + e.off + dstW * dstH : nullptr;
		} else {
			fill = s_covTmp;
			stroke = edge ? s_strTmp : nullptr;
		}
	}
	if (edge) subfb::mask(x - edge, y - edge, swd, shd, stroke, swd, drop, alpha);
	subfb::mask(x, y, dstW, dstH, fill, dstW, c, alpha);
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

static float pass(Style st, int x, int y, const GxtChar* s, float sw, float sh,
                  u16 c, u16 drop, u8 alpha, int edge) {
	const int fi = texOf(st);
	u8 r = (c & 31) << 3, g = (c & 0x3E0) >> 2, b = (c & 0x7C00) >> 7;
	float px = (float)x;
	for (const GxtChar* p = s; *p; p++) {
		if (*p == '~' && p[2] == '~') {
			codeColor((char)p[1], r, g, b);
			c = subfb::rgb(r, g, b);
			p += 2;
			continue;
		}
		u8 letter = *p - 32;
		u8 id = remap(st, letter);
		if (*p != ' ' && px >= -32.0f && px < 288.0f)
			glyph(fi, id, (int)px, y, sw, sh, c, drop, alpha, edge);
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

float rwidth(Style st, const GxtChar* s, float sw) {
	// the last glyph's quad (32*sw) overshoots its advance: the visible box
	// is wider than the advance sum by (32*sw - lastAdvance)
	const GxtChar* lp = nullptr;
	for (const GxtChar* p = s; *p; p++) {
		if (*p == '~' && p[2] == '~') { p += 2; continue; }
		lp = p;
	}
	const float lastAdv = lp ? prop(st, remap(st, (u8)(*lp - 32)), sw) : 0;
	return width(st, s, sw) - lastAdv + 32.0f * sw;
}

void print(Style st, int x, int y, const GxtChar* s, float sw, float sh,
           u16 color, Align a, int edge, u16 drop, u8 alpha) {
	if (!s_ok && !init()) return;
	if (a == CENTER) {
		x -= (int)(width(st, s, sw) / 2);
	} else if (a == RIGHT) {
		// anchor the visible edge on x, otherwise right-aligned text clips
		// offscreen (see rwidth)
		x -= (int)rwidth(st, s, sw);
	}
	// single walk: each glyph stamps the dilated stroke in `drop` then the
	// fill in `color` on top — tiny DS text stays readable over the map.
	pass(st, x, y, s, sw, sh, color, drop, alpha, edge);
}

}
