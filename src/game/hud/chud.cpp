#include "game/hud/chud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/hud/hudfont.h"
#include "game/hud/hudtex.h"
#include "game/hud/radar.h"
#include "game/hud/subfb.h"
#include "game/player.h"
#include "game/streaming.h"
#include "platform/timing.h"

// Port of the CHud pieces that fit the DS bottom screen (gta-reversed
// Hud.cpp). PC positions are in virtual 640x480 coords; ours are hand-tuned
// for 256x192 but keep the original ordering and scales (font scales are the
// original values x ~0.65).
namespace chud {

// HudColours.cpp subset (index = eHudColours)
static const u16 COL_GREEN       = 0; // filled in init (54,104,44)
static u16 c_green, c_red, c_lightBlue, c_lightGray, c_darkGray, c_gold, c_cream, c_black;
static u16 c_bg;                        // backdrop, matches glClearColor

// layout constants (fb pixels) — user spec: top-left weapon icon + ammo,
// bottom-left radar disc, top-right stats (clock, money, bars, stars),
// bottom-right zone/vehicle names only.
static const int RCOL = 248;            // right-aligned column edge
static const int CLOCK_Y = 4;
static const int MONEY_Y = 32;
// the stat column: clock/money right-align on RCOL; the bar stack's left
// edge tracks the money text's left edge (measured per frame — the block
// width moves with the digit count)
static const float MONEY_SW = 0.72f, MONEY_SH = 1.35f;
static const int BAR_H = 6;
static const int HEALTH_Y = 66, BAR_PITCH = 10;
static const int N_STARS = 6;
// star glyph "]" draws a 32*sw px quad; at sw=0.5 that's 16px, slots 19px
// apart -> real gaps between stars instead of the overlap we had at 22/16
static const int STAR_W = 19;
static const float STAR_SW = 0.5f, STAR_SH = 1.0f;
static const int ICON_X = 8, ICON_Y = 8, ICON_S = 46;       // weapon icon box
static const int AMMO_Y = 56;
static const int VEHICLE_Y = 158, ZONE_Y = 170;

// DrawWanted state (0xBAA414 block in the original): level display + flash
enum NameState { NAME_DONT_SHOW, NAME_FADE_IN, NAME_SHOW, NAME_FADE_OUT, NAME_SWITCH };
static int s_zoneState, s_vehicleState;
static int s_zoneTimer, s_zoneFade;
static int s_vehicleTimer, s_vehicleFade;
static const GxtChar* s_zoneShow;     // name currently printed
static const GxtChar* s_zoneLast;     // last requested name
static const GxtChar* s_vehShow;
static const GxtChar* s_vehLast;

static int s_wantedShown;             // stars currently lit
static int s_wantedFlash;             // frames since level change (flashes new star)

static subfb::Sprite s_fist, s_weapon;
static const char* s_weaponTxd;       // txd name of the loaded weapon sprite
static bool s_ok;

// ---- helpers ---------------------------------------------------------------

static u16 rgb(int r, int g, int b) { return subfb::rgb(r, g, b); }
static int frameMs() { return (int)(timing::vblanks() * 1000 / 60); }

// CSprite2d::DrawBarChart port (fill + half-bright tail + 1px black border)
static void barChart(int x, int y, int w, int h, float pct, u16 color) {
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	const int cur = w * (int)pct / 100;
	const u16 dim = (u16)((((color >> 0) & 31) / 2) | (((((color >> 5) & 31) / 2) << 5)) |
	                      (((((color >> 10) & 31) / 2) << 10)) | BIT(15));
	subfb::rect(x, y, cur, h, color);
	subfb::rect(x + cur, y, w - cur, h, dim);
	subfb::hline(x, y, w, c_black);
	subfb::hline(x, y + h - 1, w, c_black);
	subfb::rect(x, y, 1, h, c_black);
	subfb::rect(x + w - 1, y, 1, h, c_black);
}

// NAME_* fade machine used by both zone and vehicle names; dt = ms this frame
static int nameFsm(int& state, int& timer, int& fade, const GxtChar* req,
                   const GxtChar*& last, const GxtChar*& show, int& otherState, int dt) {
	if (!req) {
		state = NAME_DONT_SHOW;
		timer = fade = 0;
		last = nullptr;
		return -1;
	}
	if (req != last) {
		switch (state) {
		case NAME_DONT_SHOW:
			state = NAME_FADE_IN;
			timer = fade = 0;
			show = req;
			if (otherState == NAME_SHOW || otherState == NAME_FADE_IN) otherState = NAME_FADE_OUT;
			break;
		default:
			state = NAME_SWITCH;
			timer = 0;
			break;
		}
		last = req;
	}
	if (state == NAME_DONT_SHOW) return -1;
	int alpha;
	switch (state) {
	case NAME_SHOW:
		if (timer > 3000) { state = NAME_FADE_OUT; fade = 1000; }
		alpha = 255;
		break;
	case NAME_FADE_IN:
		fade += dt;
		if (fade > 1000) { fade = 1000; state = NAME_SHOW; }
		alpha = fade * 255 / 1000;
		break;
	case NAME_FADE_OUT:
		fade -= dt;
		if (fade < 0) { state = NAME_DONT_SHOW; fade = 0; }
		alpha = fade * 255 / 1000;
		break;
	case NAME_SWITCH:
		fade -= dt;
		if (fade < 0) { timer = 0; state = NAME_FADE_IN; fade = 0; show = req; }
		alpha = fade * 255 / 1000;
		break;
	default:
		alpha = 0;
	}
	timer += dt;
	return alpha;
}

// ---- elements (DrawClock/DrawMoney/DrawAmmo/DrawWanted/RenderXxxBar ports) --

static void drawClock() {
	char s[8];
	snprintf(s, sizeof s, "%02u:%02u", player::g_clockH, player::g_clockM);
	hudfont::print(hudfont::STYLE_PRICEDOWN, RCOL, CLOCK_Y, (const GxtChar*)s,
	               0.72f, 1.35f, c_lightGray, hudfont::RIGHT, 2, c_black);
}

static int s_barsX = RCOL - 108;   // left edge for the bar stack, set by drawMoney

static void drawMoney() {
	char s[16];
	const int m = player::g.money;
	const u16 c = m < 0 ? c_red : c_green;
	if (m < 0) snprintf(s, sizeof s, "-$%07d", -m);
	else snprintf(s, sizeof s, "$%08d", m);
	hudfont::print(hudfont::STYLE_PRICEDOWN, RCOL, MONEY_Y, (const GxtChar*)s,
	               MONEY_SW, MONEY_SH, c, hudfont::RIGHT, 2, c_black);
	s_barsX = RCOL - (int)hudfont::rwidth(hudfont::STYLE_PRICEDOWN,
	                                    (const GxtChar*)s, MONEY_SW);
}

static void drawWanted(int y) {
	const int lvl = player::g.wantedLevel;
	if (lvl == 0) { s_wantedShown = 0; return; } // PC: no stars at all at 0
	if (lvl != s_wantedShown) {
		if (lvl > s_wantedShown) s_wantedFlash = 0;
		s_wantedShown = lvl;
	}
	s_wantedFlash++;
	// 6 stars under the bars, evenly spaced STAR_W apart with the last
	// quad's right edge pinned to RCOL; the newest star flashes ~5x like
	// the original DrawWanted. y tracks the bottom of the bar block, so
	// the row slides up when armour/breath aren't being drawn
	const int starQW = (int)(32 * STAR_SW);
	for (int i = 0; i < N_STARS; i++) {
		const int x = RCOL - starQW - (N_STARS - 1 - i) * STAR_W;
		u16 c = c_black;
		if (i < lvl) {
			c = c_gold;
			if (i == lvl - 1 && s_wantedFlash < 60 && (s_wantedFlash / 6) % 2) c = c_darkGray;
		} else {
			c = c_darkGray;
		}
		hudfont::print(hudfont::STYLE_MENU, x, y, (const GxtChar*)"]",
		               STAR_SW, STAR_SH, c, hudfont::LEFT, 1, c_black);
	}
}

static void drawWeapon() {
	const player::Weapon& w = player::g.weapon;
	// weapon icon: "<name>icon" from its dspk txd; fist sprite when unarmed
	// or while the weapon sprite hasn't decoded yet (retry every 30 frames)
	static int s_retry;
	if (w.txd != s_weaponTxd) {
		hudtex::freeSprite(s_weapon);
		s_weaponTxd = w.txd;
		s_retry = 0;
	}
	if (w.txd && w.icon && !s_weapon.col && s_retry-- <= 0) {
		s_retry = 30;
		void* blob = streaming::g_models.load(streaming::g_models.find(w.txd));
		if (blob) {
			hudtex::decodeSprite(hudtex::find(blob, w.icon), s_weapon);
			free(blob);
		}
	}
	const subfb::Sprite& s = (w.icon && s_weapon.col) ? s_weapon : s_fist;
	if (s.col) subfb::blitScaled(s, ICON_X, ICON_Y, ICON_S, ICON_S);

	// ammo: "reserve-clip" (DrawAmmo: totalAmmo-clip - clip), gothic under the icon
	if (w.txd && w.total >= 0) {
		char s2[24];
		snprintf(s2, sizeof s2, "%d-%d", w.total - w.clip, w.clip);
		hudfont::print(hudfont::STYLE_GOTHIC, ICON_X + ICON_S / 2, AMMO_Y, (const GxtChar*)s2,
		               0.4f, 0.8f, c_lightBlue, hudfont::CENTER, 1, c_black);
	}
}

// Stack the bars from HEALTH_Y skipping hidden slots, return where the
// wanted-stars row hangs (block bottom + a small gap). PC layout scaled to
// the money column: health is the wide bar — its width is proportional to
// maxHealth (109 units at max=100, so it reaches further left as max health
// grows); armour/breath are the small fixed 62-unit bars. All right-anchored.
static int drawBars() {
	const player::State& p = player::g;
	const int W = RCOL - s_barsX;
	const int wSmall = W * 62 / 109;
	int y = HEALTH_Y;
	const int wh = W * (int)p.maxHealth / 100;
	barChart(RCOL - wh, y, wh, BAR_H, p.health * 100.0f / p.maxHealth, c_red);
	y += BAR_PITCH;
	if (p.armour > 1.0f) {
		barChart(RCOL - wSmall, y, wSmall, BAR_H, p.armour * 100.0f / p.maxArmour, c_lightGray);
		y += BAR_PITCH;
	}
	if (p.breath < p.maxBreath) {
		barChart(RCOL - wSmall, y, wSmall, BAR_H, p.breath * 100.0f / p.maxBreath, c_lightBlue);
		y += BAR_PITCH;
	}
	return y + 4;
}

static void drawNames() {
	static int s_lastMs;
	const int now = frameMs();
	const int dt = now - s_lastMs;
	s_lastMs = now;
	int za = nameFsm(s_zoneState, s_zoneTimer, s_zoneFade, player::g.zoneName,
	                 s_zoneLast, s_zoneShow, s_vehicleState, dt);
	if (za > 0 && s_zoneShow)
		hudfont::print(hudfont::STYLE_GOTHIC, RCOL, ZONE_Y, s_zoneShow,
		               0.55f, 0.9f, c_lightBlue, hudfont::RIGHT, 2, c_black, (u8)za);
	int va = nameFsm(s_vehicleState, s_vehicleTimer, s_vehicleFade, player::g.vehicleName,
	                 s_vehLast, s_vehShow, s_zoneState, dt);
	if (va > 0 && s_vehShow)
		hudfont::print(hudfont::STYLE_MENU, RCOL, VEHICLE_Y, s_vehShow,
		               0.5f, 0.75f, c_green, hudfont::RIGHT, 2, c_black, (u8)va);
}

// ---- lifecycle ---------------------------------------------------------------

bool init() {
	if (s_ok) return true;
	// the GAME screen is the default now, so init runs while the frontend is
	// still up — wait for the pack (the loadscreen opens it) or the radar
	// would latch an empty tile cache forever
	if (!streaming::g_models.isOpen()) return false;
	c_bg = RGB15(10, 14, 22) | BIT(15);
	c_red = rgb(180, 25, 29);
	c_green = rgb(54, 104, 44);
	c_lightBlue = rgb(172, 203, 241);
	c_lightGray = rgb(225, 225, 225);
	c_darkGray = rgb(150, 150, 150);
	c_gold = rgb(144, 98, 16);
	c_cream = rgb(226, 192, 99);
	c_black = rgb(0, 0, 0);
	if (!hudfont::init()) return false;
	radar::init();
	void* hud = hudtex::readFile("nitro:/models/hud.txd");
	if (hud) {
		hudtex::decodeSprite(hudtex::find(hud, "fist"), s_fist);
		free(hud);
	}
	s_ok = true;
	return true;
}

void draw() {
	subfb::clear(c_bg);
	radar::draw();
	drawClock();
	drawMoney();
	drawWanted(drawBars());
	drawWeapon();
	drawNames();
	// fps counter in the empty corner the round radar leaves
	char fb[8];
	snprintf(fb, sizeof fb, "%lu", timing::fps());
	hudfont::print(hudfont::STYLE_GOTHIC, 5, 178, (const GxtChar*)fb,
	               0.5f, 0.8f, c_lightGray, hudfont::LEFT, 1, c_black);
}

void stats(char* buf, int n) {
	snprintf(buf, n, "hudok range=%d tiles=%d fist=%d weap=%s wicon=%d wanted=%d zone=%.16s",
	         (int)radar::range(), radar::tilesCached(), (int)!!s_fist.col,
	         s_weaponTxd ? s_weaponTxd : "fist", (int)!!s_weapon.col,
	         player::g.wantedLevel,
	         player::g.zoneName ? (const char*)player::g.zoneName : "-");
}

}
