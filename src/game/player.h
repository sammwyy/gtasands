#pragma once

#include "game/text/gxt.h"

// CPlayerPed/CPlayerInfo/CWanted/CClock stand-in: the single snapshot the
// in-game HUD reads every frame. Fed by the demo driver until real entities,
// wanted logic and weapons land. All coordinates are SA world units
// (-3000..3000), heading 0 = north, clockwise.
namespace player {

struct Weapon {
	const char* txd;     // dspk entry name ("ak47.txd"), nullptr = fist
	const char* icon;    // texture inside that txd ("ak47icon"), nullptr = fist
	int clip, total;
};

struct State {
	float x, y, z;
	float heading;        // radians, 0 = north
	float speed;          // world units/sec -> radar zoom
	float health;         // 0..maxHealth
	float maxHealth;
	float armour;         // 0..maxArmour
	float maxArmour;
	float breath;         // 0..maxBreath, only relevant < maxBreath
	float maxBreath;
	int money;
	int wantedLevel;      // 0..6
	Weapon weapon;
	const GxtChar* zoneName;    // nullptr = none shown
	const GxtChar* vehicleName; // nullptr = none shown
};

extern State g;

// Game clock (CClock::ms_nGameClockHours/Minutes). 1 game minute = 1 real
// second, like the original.
extern unsigned g_clockH, g_clockM;

void init();
void update();            // movement + anim + clock; call once per frame while INGAME
float camYaw();           // chase camera heading, radians (0 = north, clockwise)

}
