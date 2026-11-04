#include "game/player.h"

#include <math.h>
#include <string.h>

#include "game/zones.h"
#include "platform/timing.h"

// Demo driver: a fake CJ doing laps around Los Santos so the HUD has real
// world data to show (scrolling radar, changing zone names) until real
// gameplay state exists. Everything below is throwaway-but-shaped-like-the-
// real-thing code.
namespace player {

State g;
unsigned g_clockH, g_clockM;

static const Weapon k_weapons[] = {
	{ nullptr,   nullptr,              0,   0 },   // fist
	{ "ak47.txd",      "ak47icon",      30, 120 },
	{ "desert_eagle.txd","desert_eagleicon", 7,  35 },
	{ "tec9.txd",      "tec9icon",      50, 200 },
};
static const int k_wantedSteps[] = { 0, 1, 2, 3, 4, 5, 6, 3, 0 };

static u32 s_startVbl;      // vblank counter at init (demo clock base)
static int s_zoneTimer;

void init() {
	g = {};
	g.x = 1800.0f; g.y = -1650.0f; g.z = 13.5f;
	g.health = g.maxHealth = 100.0f;
	g.armour = 50.0f;
	g.breath = 300.0f;      // not drowning
	g.money = 350;
	g.wantedLevel = 0;
	g.weapon = k_weapons[0];
	g_clockH = 12; g_clockM = 0;
	s_startVbl = timing::vblanks();
	s_zoneTimer = 0;
}

// Demo path: circle around downtown LS.
static void updateDemo() {
	const u32 v = timing::vblanks() - s_startVbl;

	// circular path, ~75s per lap
	const float ang = (float)v * (2.0f * 3.14159265f) / (60.0f * 75.0f);
	const float r = 400.0f, cx = 1550.0f, cy = -1650.0f;
	g.x = cx + r * cosf(ang);
	g.y = cy + r * sinf(ang);
	// SA facing = (-sin h, cos h) must equal velocity (-sin a, cos a) -> h = a
	g.heading = ang;
	g.speed = r * (2.0f * 3.14159265f) / (60.0f * 75.0f);

	// wanted level steps every ~5s
	g.wantedLevel = k_wantedSteps[(v / 300) % (sizeof k_wantedSteps / sizeof k_wantedSteps[0])];

	// weapon swap every ~8s
	g.weapon = k_weapons[(v / 480) % (sizeof k_weapons / sizeof k_weapons[0])];

	// vehicle name every ~20s for ~5s (LANDSTALKER is a real GXT key)
	g.vehicleName = (v % 1200) < 300 ? (const GxtChar*)"LANDSTALKER" : nullptr;

	// health/armour/breath slowly wiggle so bars feel alive
	g.health = g.maxHealth * (0.55f + 0.45f * sinf((float)v / 300.0f));
	g.armour = 50.0f + 45.0f * sinf((float)v / 200.0f + 1.0f);

	// game clock: 1 min per real second
	const u32 mins = v / 60;
	g_clockH = (12 + mins / 60) % 24;
	g_clockM = mins % 60;

	// zone name every ~0.5s
	if (--s_zoneTimer <= 0) {
		s_zoneTimer = 30;
		g.zoneName = zones::nameAt(g.x, g.y);
	}
}

void update() { updateDemo(); }

}
