#include "game/hud/radar.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "assets/txdfile.h"
#include "game/hud/hudtex.h"
#include "game/hud/subfb.h"
#include "game/entryexits.h"
#include "game/missions.h"
#include "game/world.h"
#include "game/player.h"
#include "mem/pool.h"

// Port of the CRadar bits we need (gta-reversed Radar.cpp):
//  - world -> radar space: offset from the player rotated so heading is up
//  - map tiles on a 12x12 grid, 500 world units each, indexed SW->NE
//  - dynamic range (180..350) eased toward a speed-based target
//  - blips transformed into radar space, clamped to the disc rim
namespace radar {

// radar disc geometry on the 256x192 screen
static const int CX = 58, CY = 136, R = 50;

// world units per radar pixel ~= range/R; 20.12 fixed point everywhere here.
// The 64px tiles sample 1:1 at range~390 (7.8 u/texel); past that a 32px mip
// keeps the zoomed-out view coherent instead of aliased.
static const int RMIN = 300, RMAX = 900;
static const int MIP_RANGE = 390;         // range > this -> 32px mip tiles
static const int MIP_HYST = 70;           // prefetch the other level this close to the switch

struct Blip {
	bool on;
	s16 x, y;            // world units
	u8 r, g, b;          // square color (sprite == nullptr)
	subfb::Sprite spr;   // decoded sprite, empty = square
	char spriteName[24];
};
static Blip s_blips[16];
static ds::MapPoint* s_points;
static int s_pointCount;
static const struct { const char* icon, *key; } s_types[MAP_TYPE_COUNT] = {
	{ "radar_ammugun", "LG_03" }, { "radar_barbers", "LG_04" },
	{ "radar_burgerShot", "LG_07" }, { "radar_chicken", "LG_11" },
	{ "radar_pizza", "LG_26" }, { "radar_diner", "LG_67" },
	{ "radar_gym", "LG_53" }, { "radar_tshirt", "LG_42" },
	{ "radar_police", "LG_27" }, { "radar_tattoo", "LG_36" },
	{ "radar_saveGame", "LG_32" }, { "radar_modGarage", "LG_24" },
	{ "radar_propertyG", "LG_32" }, { "radar_centre", "LG_01" }, { "radar_waypoint", "LG_64" },
};
static subfb::Sprite s_pointSprites[MAP_TYPE_COUNT - 2];

static TxdFile s_hudTxd;              // hud.txd directory: blip sprites decode on demand
static subfb::Sprite s_arrow, s_north;

// Tiles are streamed from models/radar.dsrt (pre-baked RGB15, fixed stride)
// into two small pools: the 3x3-ish window of 64px tiles and the 5x5 window
// of 32px mips around the player. A tile is one fseek+fread, never decoded;
// the per-frame byte budget keeps a fast drive from ever stalling a frame.
static const int TILES = 12;
static const int FULL_PX = 64 * 64, MIP_PX = 32 * 32;
static const u32 IO_BUDGET = 12 * 1024;   // bytes per frame (one full tile + mips)

struct FullTile { u16 px[FULL_PX]; u32 lastUse; u8 key; };
struct MipTile { u16 px[MIP_PX]; u32 lastUse; u8 key; };
static mem::Pool<FullTile> s_fullPool;
static mem::Pool<MipTile> s_mipPool;
static FullTile* s_full[TILES * TILES];
static MipTile* s_mip[TILES * TILES];
static u8 s_present[TILES * TILES];
static FILE* s_file;
static u32 s_fullOff, s_mipOff, s_frame;

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

bool init() {
	s_sea = rgb(111, 137, 170);
	memset(s_blips, 0, sizeof s_blips);
	memset(s_full, 0, sizeof s_full);
	memset(s_mip, 0, sizeof s_mip);
	memset(s_present, 0, sizeof s_present);
	if (s_hudTxd.open("nitro:/models/hud.txd")) {
		mem::Scratch scratch;
		if (!s_arrow.col) hudtex::decodeSprite(s_hudTxd.load("radar_centre", scratch), s_arrow);
		if (!s_north.col) hudtex::decodeSprite(s_hudTxd.load("radar_north", scratch), s_north);
		for (int i = 0; i < MAP_TYPE_COUNT - 2; i++) {
			mem::Scratch iconScratch;
			if (!s_pointSprites[i].col)
				hudtex::decodeSprite(s_hudTxd.load(s_types[i].icon, iconScratch), s_pointSprites[i]);
		}
	}
	if (!s_points) {
		FILE* f = fopen("nitro:/data/map_points.dpoi", "rb");
		ds::PointHeader h;
		if (f && fread(&h, sizeof h, 1, f) == 1 && h.magic == DS_FOURCC('D', 'P', 'O', 'I') &&
		    h.version == 2 && h.count <= 1024) {
			s_points = (ds::MapPoint*)mem::alloc(h.count * sizeof(ds::MapPoint), mem::TAG_RADAR);
			if (s_points && fread(s_points, sizeof(ds::MapPoint), h.count, f) == h.count)
				s_pointCount = h.count;
			else {
				mem::free(s_points);
				s_points = nullptr;
			}
		}
		if (f) fclose(f);
	}
	if (!s_fullPool.capacity()) {
		s_fullPool.init(mem::budget::RADAR_TILE_SLOTS, mem::TAG_RADAR);
		s_mipPool.init(mem::budget::RADAR_MIP_SLOTS, mem::TAG_RADAR);
	}
	if (!s_file) s_file = fopen("nitro:/models/radar.dsrt", "rb");
	ds::RadarHeader h;
	if (s_file && fread(&h, sizeof h, 1, s_file) == 1 && h.magic == DS_FOURCC('D', 'S', 'R', 'T') &&
	    h.grid == TILES && h.tileSize == 64 && h.mipSize == 32 &&
	    fread(s_present, 1, TILES * TILES, s_file) == TILES * TILES) {
		s_fullOff = h.fullOffset;
		s_mipOff = h.mipOffset;
	} else {
		memset(s_present, 0, sizeof s_present);
	}
	s_range = RMIN;
	return s_file != nullptr;
}

const char* typeIcon(int type) {
	return (unsigned)type < MAP_TYPE_COUNT ? s_types[type].icon : nullptr;
}

const char* typeLabelKey(int type) {
	return (unsigned)type < MAP_TYPE_COUNT ? s_types[type].key : nullptr;
}

void drawLegendIcon(int type, int x, int y, int size) {
	const subfb::Sprite* icon = (unsigned)type < MAP_TYPE_COUNT - 2 ? &s_pointSprites[type]
		: type == MAP_TYPE_COUNT - 2 ? &s_arrow : nullptr;
	if (icon && icon->col) subfb::blitScaled(*icon, x, y, size, size);
}

int legendTypes(float centerX, float centerY, bool full, u8* out, int capacity) {
	bool seen[MAP_TYPE_COUNT] = {};
	for (int i = 0; i < s_pointCount; i++) {
		const ds::MapPoint& p = s_points[i];
		if (!entryexits::unlocked() && !missions::pointAllowed(p.x, p.y)) continue;
		if (p.icon < MAP_TYPE_COUNT - 2 &&
		    (full || (fabsf(p.x - centerX) < 512 && fabsf(p.y - centerY) < 384)))
			seen[p.icon] = true;
	}
	if (full || (fabsf(player::g.x - centerX) < 512 && fabsf(player::g.y - centerY) < 384))
		seen[MAP_TYPE_COUNT - 2] = true;
	for (const Blip& b : s_blips) {
		if (b.on && (full || (fabsf(b.x - centerX) < 512 && fabsf(b.y - centerY) < 384))) {
			for (int i = 0; i < MAP_TYPE_COUNT; i++)
				if (b.spr.col && !strcmp(b.spriteName, s_types[i].icon)) seen[i] = true;
		}
	}
	int count = 0;
	for (int i = 0; i < MAP_TYPE_COUNT; i++)
		if (seen[i] && count < capacity) out[count++] = (u8)i;
	return count;
}

int legendRadarTypes(u8* out, int capacity) {
	bool seen[MAP_TYPE_COUNT] = {};
	const float reach = s_range * (R - 9) / R, reach2 = reach * reach;
	for (int i = 0; i < s_pointCount; i++) {
		const ds::MapPoint& p = s_points[i];
		if (!entryexits::unlocked() && !missions::pointAllowed(p.x, p.y)) continue;
		const float dx = p.x - player::g.x, dy = p.y - player::g.y;
		if (p.icon < MAP_TYPE_COUNT - 2 && dx * dx + dy * dy < reach2) seen[p.icon] = true;
	}
	seen[MAP_TYPE_COUNT - 2] = true;
	for (const Blip& b : s_blips) {
		if (!b.on || !b.spr.col) continue;
		for (int i = 0; i < MAP_TYPE_COUNT; i++)
			if (!strcmp(b.spriteName, s_types[i].icon)) seen[i] = true;
	}
	int count = 0;
	for (int i = 0; i < MAP_TYPE_COUNT; i++)
		if (seen[i] && count < capacity) out[count++] = (u8)i;
	return count;
}

// Least recently used tile that this frame does not need.
template <class T>
static T* victim(mem::Pool<T>& pool) {
	T* best = nullptr;
	pool.forEach([&](T& t) {
		if (t.lastUse != s_frame && (!best || t.lastUse < best->lastUse)) best = &t;
	});
	return best;
}

template <class T>
static bool loadTile(int key, mem::Pool<T>& pool, T** map, u32 base, u32& budget) {
	if (map[key]) { map[key]->lastUse = s_frame; return true; }
	if (!s_present[key] || !s_file || budget < sizeof(T::px)) return false;
	T* t = pool.create();
	if (!t) {
		t = victim(pool);
		if (!t) return false;
		map[t->key] = nullptr;
	}
	if (fseek(s_file, (long)(base + (u32)key * sizeof(T::px)), SEEK_SET) != 0 ||
	    fread(t->px, 1, sizeof(T::px), s_file) != sizeof(T::px)) {
		pool.destroy(t);
		return false;
	}
	budget -= sizeof(T::px);
	t->key = (u8)key;
	t->lastUse = s_frame;
	map[key] = t;
	return true;
}

// Ensures the tiles under the disc (plus a prefetch margin) are resident,
// nearest first, within the frame's I/O budget.
static void stream(float px, float py, float reach, bool wantFull, bool wantMip, u32& budget) {
	const int x0 = (int)((px - reach + 3000.0f) / 500.0f), x1 = (int)((px + reach + 3000.0f) / 500.0f);
	const int y0 = (int)((3000.0f - py - reach) / 500.0f), y1 = (int)((3000.0f - py + reach) / 500.0f);
	const int ptx = (int)((px + 3000.0f) / 500.0f), pty = (int)((3000.0f - py) / 500.0f);
	// rings around the player's tile: the visible centre always wins the budget
	for (int ring = 0; ring <= 3; ring++)
		for (int ty = y0; ty <= y1; ty++)
			for (int tx = x0; tx <= x1; tx++) {
				if ((unsigned)tx >= (unsigned)TILES || (unsigned)ty >= (unsigned)TILES) continue;
				const int dx = tx > ptx ? tx - ptx : ptx - tx, dy = ty > pty ? ty - pty : pty - ty;
				if ((dx > dy ? dx : dy) != ring) continue;
				const int key = ty * TILES + tx;
				if (wantFull) loadTile(key, s_fullPool, s_full, s_fullOff, budget);
				if (wantMip) loadTile(key, s_mipPool, s_mip, s_mipOff, budget);
			}
}

void prefetch() {
	u32 budget = 0xFFFFFFFFu;
	s_frame++;
	stream(player::g.x, player::g.y, s_range + 150.0f, true, true, budget);
}

// Tile data for a grid cell, or nullptr when it isn't resident (yet).
static const u16* tile(int tx, int ty, bool mip) {
	if ((unsigned)tx >= (unsigned)TILES || (unsigned)ty >= (unsigned)TILES) return nullptr;
	const int key = ty * TILES + tx;
	if (mip) return s_mip[key] ? s_mip[key]->px : nullptr;
	return s_full[key] ? s_full[key]->px : nullptr;
}

// The map proper: for each fb row of the disc, march across in world space.
static void drawMap() {
	const player::State& p = player::g;

	// dynamic zoom toward speed-based target (DrawMap's range easing);
	// ~360 at rest — a bit wider than PC's 180 so a couple of blocks show
	float target = p.speed * 0.6666f + 340.0f;
	if (target < RMIN) target = RMIN;
	if (target > RMAX) target = RMAX;
	s_range += (target - s_range) * 0.06f;

	// mip when a texel would cover more than ~1.4 fb pixels
	const bool mip = s_range > (float)MIP_RANGE;
	s_frame++;
	u32 budget = IO_BUDGET;
	stream(p.x, p.y, s_range + 150.0f, !mip || s_range < MIP_RANGE + MIP_HYST, mip || s_range > MIP_RANGE - MIP_HYST, budget);

	const int tileShift = mip ? 5 : 6;
	const int tileMask = (1 << tileShift) - 1;
	const int texelShift = 12 + tileShift;           // Q12 texel coord -> tile index
	// world units -> texels of the current level (64 or 32 per 500 units), Q16
	const s64 texPerUnit = ((s64)(1 << tileShift) << 16) / 500;

	// heading -> rotation (radar->world): wx = ox + k*(c*u + s*v)
	// (libnds brads: 32768 = full circle)
	const int brad = (int)(p.heading * 32768.0f / (2.0f * 3.14159265f)) & 0xFFFF;
	const int c = cosLerp((s16)brad), s = sinLerp((s16)brad); // 4.12

	const int kQ = (int)(s_range * 4096.0f / R);           // world units/px, Q12
	const int pxQ = (int)(p.x * 4096.0f), pyQ = (int)(p.y * 4096.0f);
	u16* fb = subfb::frame();

	// incremental marching: world coords step by (dwx,dwy) per screen pixel.
	// u,v are raw px offsets from the disc centre; kQ is already units/px —
	// normalizing by R once more put the radar ~50x too zoomed in (flat map).
	const int dwx = (int)(((s64)kQ * c) >> 12);             // world step/px, Q12
	const int dwy = (int)(((s64)kQ * s) >> 12);
	// the same march in texel space (Q12, origin at the map's NW corner):
	// the inner loop is two adds, two shifts and a lookup per pixel
	const int dU = (int)(((s64)dwx * texPerUnit) >> 16);
	const int dV = (int)(((s64)-dwy * texPerUnit) >> 16);

	for (int j = CY - R; j <= CY + R; j++) {
		if ((unsigned)j >= (unsigned)subfb::H) continue;
		const int vN = ((CY - j) << 12) / R;                // normalized v, Q12
		if (vN * vN > (1 << 24)) continue;
		const int umax = isqrt((1 << 24) - vN * vN);        // Q12
		const int xs = CX - ((umax * R) >> 12) < 0 ? 0 : CX - ((umax * R) >> 12);
		const int x1 = CX + ((umax * R) >> 12);
		// world coords at the row's left edge (radar -> world by heading)
		const int u0 = xs - CX, v = CY - j;                 // px offsets
		const int wx = pxQ + (int)(((s64)kQ * (c * u0 - s * v)) >> 12);
		const int wy = pyQ + (int)(((s64)kQ * (s * u0 + c * v)) >> 12);
		int U = (int)((((s64)wx + (3000 << 12)) * texPerUnit) >> 16);
		int V = (int)((((s64)(3000 << 12) - wy) * texPerUnit) >> 16);
		const u16* t = nullptr;
		int tkey = -1;
		u16* dst = fb + j * subfb::STRIDE + xs;
		const int xe = x1 < subfb::W ? x1 : subfb::W - 1;
		for (int i = xs; i <= xe; i++, U += dU, V += dV) {
			const int tx = U >> texelShift, ty = V >> texelShift;
			const int key = (ty << 8) | (tx & 0xFF);
			if (key != tkey) { t = tile(tx, ty, mip); tkey = key; }
			*dst++ = t ? t[(((V >> 12) & tileMask) << tileShift) | ((U >> 12) & tileMask)] : s_sea;
		}
	}

	// rim: dark outline + inner shadow, like the PC radardisc border
	subfb::circle(CX, CY, R + 1, rgb(0, 0, 0));
	subfb::circle(CX, CY, R, rgb(40, 40, 40));
}

void drawPauseMap(float centerX, float centerY) {
	constexpr int step = 4 << 12;
	const s64 texPerUnit = ((s64)64 << 16) / 500;
	const int texelShift = 18;
	const int dU = (int)(((s64)step * texPerUnit) >> 16);
	const int pxQ = (int)(centerX * 4096.0f);
	const int pyQ = (int)(centerY * 4096.0f);
	const int left = pxQ - (subfb::W / 2) * step;
	int north = pyQ + (subfb::H / 2) * step;
	const int U0 = (int)((((s64)left + (3000 << 12)) * texPerUnit) >> 16);
	s_frame++;
	u32 budget = IO_BUDGET;
	stream(centerX, centerY, 550.0f, true, false, budget);
	u16* fb = subfb::frame();
	for (int y = 0; y < subfb::H; y++, north -= step) {
		int U = U0;
		const int V = (int)((((s64)(3000 << 12) - north) * texPerUnit) >> 16);
		const int ty = V >> texelShift, row = ((V >> 12) & 63) << 6;
		const u16* t = nullptr;
		int lastTx = -1;
		u16* dst = fb + y * subfb::STRIDE;
		for (int x = 0; x < subfb::W; x++, U += dU) {
			const int tx = U >> texelShift;
			if (tx != lastTx) { t = tile(tx, ty, false); lastTx = tx; }
			dst[x] = t ? t[row | ((U >> 12) & 63)] : s_sea;
		}
	}
	for (int i = 0; i < s_pointCount; i++) {
		const ds::MapPoint& p = s_points[i];
		if (!entryexits::unlocked() && !missions::pointAllowed(p.x, p.y)) continue;
		if (p.icon >= MAP_TYPE_COUNT - 2) continue;
		const int x = subfb::W / 2 + (int)((p.x - centerX) / 4.0f);
		const int y = subfb::H / 2 - (int)((p.y - centerY) / 4.0f);
		if ((unsigned)x >= subfb::W || (unsigned)y >= subfb::H) continue;
		const subfb::Sprite& icon = s_pointSprites[p.icon];
		if (icon.col) subfb::blit(icon, x - icon.w / 2, y - icon.h / 2);
	}
	for (const Blip& b : s_blips) {
		if (!b.on) continue;
		const int x = subfb::W / 2 + (b.x - (int)centerX) / 4;
		const int y = subfb::H / 2 - (b.y - (int)centerY) / 4;
		if ((unsigned)x >= subfb::W || (unsigned)y >= subfb::H) continue;
		subfb::rect(x - 3, y - 3, 7, 7, rgb(0, 0, 0));
		subfb::rect(x - 2, y - 2, 5, 5, rgb(b.r, b.g, b.b));
	}
	const int playerX = subfb::W / 2 + (int)((player::g.x - centerX) / 4.0f);
	const int playerY = subfb::H / 2 - (int)((player::g.y - centerY) / 4.0f);
	if ((unsigned)playerX < subfb::W && (unsigned)playerY < subfb::H) {
		subfb::circle(playerX, playerY, 5, rgb(0, 0, 0));
		subfb::circle(playerX, playerY, 3, rgb(255, 255, 255));
	}
}

void drawFullPoints(int left, int top, int size) {
	for (int i = 0; i < s_pointCount; i++) {
		const ds::MapPoint& p = s_points[i];
		if (!entryexits::unlocked() && !missions::pointAllowed(p.x, p.y)) continue;
		if (p.icon >= MAP_TYPE_COUNT - 2) continue;
		const int x = left + (p.x + 3000) * size / 6000;
		const int y = top + (3000 - p.y) * size / 6000;
		const subfb::Sprite& icon = s_pointSprites[p.icon];
		if (icon.col) subfb::blitScaled(icon, x - 4, y - 4, 8, 8);
	}
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

static void drawPoints() {
	if (world::area() != 0) return;
	const float c = cosf(player::g.heading), s = sinf(player::g.heading);
	const float k = R / s_range, reach = s_range * (R - 9) / R;
	const float reach2 = reach * reach;
	for (int i = 0; i < s_pointCount; i++) {
		const ds::MapPoint& p = s_points[i];
		if (!entryexits::unlocked() && !missions::pointAllowed(p.x, p.y)) continue;
		if (p.icon >= MAP_TYPE_COUNT - 2) continue;
		const float dx = p.x - player::g.x, dy = p.y - player::g.y;
		if (dx * dx + dy * dy >= reach2) continue;
		const subfb::Sprite& icon = s_pointSprites[p.icon];
		if (icon.col)
			subfb::blit(icon, CX + (int)((dx * c + dy * s) * k) - icon.w / 2,
			                CY - (int)((-dx * s + dy * c) * k) - icon.h / 2);
	}
}

void draw() {
	drawMap();
	drawPoints();
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
	if (sprite && *sprite && s_hudTxd.isOpen()) {
		snprintf(bl.spriteName, sizeof bl.spriteName, "%s", sprite);
		mem::Scratch scratch;
		hudtex::decodeSprite(s_hudTxd.load(sprite, scratch), bl.spr);
	}
}

void clearBlip(int id) {
	if ((unsigned)id >= 16) return;
	hudtex::freeSprite(s_blips[id].spr);
	s_blips[id] = {};
}

float range() { return s_range; }

int tilesCached() { return s_fullPool.count() + s_mipPool.count(); }

}
