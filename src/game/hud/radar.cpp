#include "game/hud/radar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/pack.h"
#include "game/hud/hudtex.h"
#include "game/hud/subfb.h"
#include "game/player.h"
#include "game/streaming.h"

// Port of the CRadar bits we need (gta-reversed Radar.cpp):
//  - world -> radar space: offset from the player rotated so heading is up
//  - map tiles on a 12x12 grid, 500 world units each, indexed SW->NE
//  - dynamic range (180..350) eased toward a speed-based target
//  - blips transformed into radar space, clamped to the disc rim
namespace radar {

// radar disc geometry on the 256x192 screen
static const int CX = 58, CY = 136, R = 50;

// world units per radar pixel ~= range/R; 20.12 fixed point everywhere here
static const int RMIN = 180, RMAX = 350;

struct Blip {
	bool on;
	s16 x, y;            // world units
	u8 r, g, b;          // square color (sprite == nullptr)
	subfb::Sprite spr;   // decoded sprite, empty = square
};
static Blip s_blips[16];

static void* s_hudTxd;
static subfb::Sprite s_arrow, s_north;

// All 144 radar tiles decoded+pre-filtered at init into a static arena
// (~1.15MB). Runtime never does file I/O or decoding: a tile miss on FAT
// costs ms and tanks fps; decode-once also lets us box-filter in place so
// the inner loop stays a single-texel lookup.
static const int TILES = 12;
static const int TILE_PX = 64 * 64;
static u16 s_tiles[TILES * TILES][TILE_PX];
static u8 s_tok[TILES * TILES];

static float s_range = 180.0f;

static u16 rgb(int r, int g, int b) { return subfb::rgb(r, g, b); }
static u16 s_sea;

// integer sqrt for the circle-row half-width (Q24 -> Q12)
static int isqrt(u32 v) {
	u32 r = 0, bit = 1 << 30;
	while (bit > v) bit >>= 2;
	while (bit) {
		if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return (int)r;
}

// in-place 2x2 box smooth: softens texel edges so nearest sampling doesn't
// shimmer while the map scrolls/rotates (cheap bilinear stand-in)
static void smooth(u16* t) {
	for (int y = 0; y < 63; y++) {
		for (int x = 0; x < 63; x++) {
			const u16 c00 = t[(y << 6) | x],       c01 = t[(y << 6) | (x + 1)];
			const u16 c10 = t[((y + 1) << 6) | x], c11 = t[((y + 1) << 6) | (x + 1)];
			const int r  = ((c00 & 31) + (c01 & 31) + (c10 & 31) + (c11 & 31)) >> 2;
			const int gg = (((c00 >> 5) & 31) + ((c01 >> 5) & 31) +
			                ((c10 >> 5) & 31) + ((c11 >> 5) & 31)) >> 2;
			const int b  = (((c00 >> 10) & 31) + ((c01 >> 10) & 31) +
			                ((c10 >> 10) & 31) + ((c11 >> 10) & 31)) >> 2;
			t[(y << 6) | x] = (u16)(BIT(15) | r | (gg << 5) | (b << 10));
		}
	}
}

void init() {
	s_sea = rgb(111, 137, 170);
	memset(s_tok, 0, sizeof s_tok);
	memset(s_blips, 0, sizeof s_blips);
	s_hudTxd = hudtex::readFile("nitro:/models/hud.txd");
	if (s_hudTxd) {
		hudtex::decodeSprite(hudtex::find(s_hudTxd, "radar_centre"), s_arrow);
		hudtex::decodeSprite(hudtex::find(s_hudTxd, "radar_north"), s_north);
	}
	// preload the whole map once: ~650KB of reads, no runtime stalls
	for (int k = 0; k < TILES * TILES; k++) {
		char name[16];
		snprintf(name, sizeof name, "radar%02d.txd", k);
		void* blob = streaming::g_models.load(streaming::g_models.find(name));
		if (!blob) continue;
		const ds::TxdHeader* th = (const ds::TxdHeader*)blob;
		const ds::TxdEntry* e = (const ds::TxdEntry*)((const u8*)blob + sizeof(ds::TxdHeader));
		if (th->magic == DS_FOURCC('D', 'T', 'X', 'D') && th->count &&
		    hudtex::decodeRgbInto((const ds::TexHeader*)((const u8*)blob + e->offset),
		                          s_tiles[k])) {
			smooth(s_tiles[k]);
			s_tok[k] = 1;
		}
		free(blob);
	}
	s_range = RMIN;
}

// Returns rgb15 tile data (64x64) for tile key, or nullptr when unavailable.
static const u16* tile(int tx, int ty) {
	if ((unsigned)tx >= (unsigned)TILES || (unsigned)ty >= (unsigned)TILES) return nullptr;
	const int key = ty * TILES + tx;
	return s_tok[key] ? s_tiles[key] : nullptr;
}

// The map proper: for each fb row of the disc, march across in world space.
static void drawMap() {
	const player::State& p = player::g;

	// dynamic zoom toward speed-based target (DrawMap's range easing)
	float target = p.speed * 0.6666f + 120.0f;
	if (target < RMIN) target = RMIN;
	if (target > RMAX) target = RMAX;
	s_range += (target - s_range) * 0.06f;

	// heading -> rotation (radar->world): wx = ox + k*(c*u + s*v)
	// (libnds brads: 32768 = full circle)
	const int brad = (int)(p.heading * 32768.0f / (2.0f * 3.14159265f)) & 0xFFFF;
	const int c = cosLerp((s16)brad), s = sinLerp((s16)brad); // 4.12

	const int kQ = (int)(s_range * 4096.0f / R);           // world units/px, Q12
	const int pxQ = (int)(p.x * 4096.0f), pyQ = (int)(p.y * 4096.0f);
	u16* fb = subfb::frame();

	// incremental marching: world coords step by (dwx,dwy) per screen pixel
	const int invR = (1 << 12) / R;                         // Q12
	const int dwx = (int)(((s64)kQ * c * invR) >> 24);      // world step/px, Q12
	const int dwy = (int)(((s64)kQ * s * invR) >> 24);

	for (int j = CY - R; j <= CY + R; j++) {
		if ((unsigned)j >= (unsigned)subfb::H) continue;
		const int v = ((CY - j) << 12) / R;                // radar v, Q12
		if (v * v > (1 << 24)) continue;
		const int umax = isqrt((1 << 24) - v * v);         // Q12
		const int xs = CX - ((umax * R) >> 12) < 0 ? 0 : CX - ((umax * R) >> 12);
		const int x1 = CX + ((umax * R) >> 12);
		// world coords at the row's left edge (radar -> world by heading)
		const int u0 = (xs - CX) * invR;
		int wx = pxQ + ((kQ * ((c * u0 - s * v) >> 12)) >> 12);
		int wy = pyQ + ((kQ * ((s * u0 + c * v) >> 12)) >> 12);
		const u16* t = nullptr;
		int tkey = -1;
		for (int i = xs; i <= x1 && i < subfb::W; i++) {
			// world -> tile grid (tile px = mod 500 * 64/500, no division)
			const int wxi = wx >> 12, wyi = wy >> 12;
			const int ax = wxi + 3000, ay = 3000 - wyi;
			const int tx = (ax * 8389) >> 22, ty = (ay * 8389) >> 22;
			const int key = ty * TILES + tx;
			if (key != tkey) { t = tile(tx, ty); tkey = key; }
			u16 col = s_sea;
			if (t) {
				const int lx = ((ax - tx * 500) * 536896) >> 22;
				const int ly = ((ay - ty * 500) * 536896) >> 22;
				col = t[(ly << 6) | lx];
			}
			fb[j * subfb::STRIDE + i] = col;
			wx += dwx;
			wy += dwy;
		}
	}

	// rim: dark outline + inner shadow, like the PC radardisc border
	subfb::circle(CX, CY, R + 1, rgb(0, 0, 0));
	subfb::circle(CX, CY, R, rgb(40, 40, 40));
}

// world -> radar space (TransformRealWorldPointToRadarSpace):
// u = offset . right = c*ox + s*oy; v = offset . forward = -s*ox + c*oy
static void worldToRadar(float wx, float wy, float& u, float& v) {
	const player::State& p = player::g;
	const float ox = wx - p.x, oy = wy - p.y;
	const float c = cosf(p.heading), s = sinf(p.heading);
	u = (ox * c + oy * s) / s_range;
	v = (-ox * s + oy * c) / s_range;
}

static void drawSprite(const subfb::Sprite& s, float u, float v, float clamp_r) {
	float d = u * u + v * v;
	if (clamp_r > 0 && d > clamp_r * clamp_r) {
		float m = sqrtf(d);
		u = u / m * clamp_r;
		v = v / m * clamp_r;
	}
	const int x = CX + (int)(u * R) - s.w / 2;
	const int y = CY - (int)(v * R) - s.h / 2;
	subfb::blit(s, x, y);
}

static void drawBlips() {
	for (int i = 0; i < 16; i++) {
		const Blip& b = s_blips[i];
		if (!b.on) continue;
		float u, v;
		worldToRadar((float)b.x, (float)b.y, u, v);
		const float d = u * u + v * v;
		if (d > 1.0f) { const float m = sqrtf(d); u /= m; v /= m; }
		if (b.spr.col) drawSprite(b.spr, u, v, -1);
		else {
			const int x = CX + (int)(u * R), y = CY - (int)(v * R);
			subfb::rect(x - 3, y - 3, 7, 7, rgb(0, 0, 0));
			subfb::rect(x - 2, y - 2, 5, 5, rgb(b.r, b.g, b.b));
		}
	}
}

void draw() {
	drawMap();
	// north indicator on the rim (world-north direction -> radar space)
	if (s_north.col) {
		const float c = cosf(player::g.heading), s = sinf(player::g.heading);
		drawSprite(s_north, s * 0.88f, c * 0.88f, -1);
	}
	drawBlips();
	if (s_arrow.col) subfb::blit(s_arrow, CX - s_arrow.w / 2, CY - s_arrow.h / 2);
	else subfb::circle(CX, CY, 3, rgb(255, 255, 255));
}

void setBlip(int id, float x, float y, const char* sprite, u8 r, u8 g, u8 b) {
	if ((unsigned)id >= 16) return;
	Blip& bl = s_blips[id];
	hudtex::freeSprite(bl.spr);
	bl = {};
	bl.on = true;
	bl.x = (s16)x; bl.y = (s16)y;
	bl.r = r; bl.g = g; bl.b = b;
	if (sprite && *sprite && s_hudTxd)
		hudtex::decodeSprite(hudtex::find(s_hudTxd, sprite), bl.spr);
}

void clearBlip(int id) {
	if ((unsigned)id >= 16) return;
	hudtex::freeSprite(s_blips[id].spr);
	s_blips[id] = {};
}

float range() { return s_range; }

int tilesCached() {
	int n = 0;
	for (int i = 0; i < TILES * TILES; i++)
		if (s_tok[i]) n++;
	return n;
}

}
