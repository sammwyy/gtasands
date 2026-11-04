#include "game/player.h"

#include <nds.h>
#include <math.h>
#include <string.h>

#include "game/cj.h"
#include "game/zones.h"
#include "platform/input.h"
#include "platform/timing.h"

// CPlayerPed/CPlayerInfo/CWanted/CClock stand-in: the single snapshot the
// in-game HUD reads every frame. Position/heading/speed are real now (D-pad
// driven); the rest is still the demo driver until real systems land.
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

// Grove Street (the classic new-game spawn), SA units.
static constexpr float SPAWN_X = 2495.0f, SPAWN_Y = -1664.0f, SPAWN_Z = 13.5f;

static float s_camYaw;          // chase camera heading (SA convention)
static int s_zoneTimer;

// radians -> [-pi, pi]
static float wrap(float a) {
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}

float camYaw() { return s_camYaw; }

void init() {
	g = {};
	g.x = SPAWN_X; g.y = SPAWN_Y; g.z = SPAWN_Z;
	g.health = g.maxHealth = 100.0f;
	g.armour = 50.0f;
	g.maxArmour = 100.0f;
	g.breath = 300.0f;      // not drowning
	g.maxBreath = 212.0f;
	g.money = 350;
	g.wantedLevel = 0;
	g.weapon = k_weapons[0];
	g_clockH = 12; g_clockM = 0;
	s_camYaw = 0;
	s_zoneTimer = 0;
	cj::init();
}

// D-pad moves camera-relative (up = away from camera) like the original.
// B (bottom) = run, A (right) = sprint. L/R nudge the camera; it otherwise
// settles behind the movement direction.
static void updateMove() {
	const input::State& in = input::get();
	const float dt = 1.0f / 60.0f;
	const float ix = (in.held & KEY_RIGHT ? 1.0f : 0.0f) - (in.held & KEY_LEFT ? 1.0f : 0.0f);
	const float iy = (in.held & KEY_UP ? 1.0f : 0.0f) - (in.held & KEY_DOWN ? 1.0f : 0.0f);

	if (in.held & KEY_L) s_camYaw -= 2.4f * dt;
	if (in.held & KEY_R) s_camYaw += 2.4f * dt;

	if (ix != 0.0f || iy != 0.0f) {
		// camera-relative wish direction in SA xy
		const float dx = -sinf(s_camYaw) * iy + cosf(s_camYaw) * ix;
		const float dy =  cosf(s_camYaw) * iy + sinf(s_camYaw) * ix;
		const float target = atan2f(-dx, dy); // facing = (-sin h, cos h)
		g.heading += wrap(target - g.heading) * (1.0f - expf(-12.0f * dt));
		const float vmax = in.held & KEY_A ? 10.0f : in.held & KEY_B ? 6.0f : 2.5f;
		g.speed += (vmax - g.speed) * (1.0f - expf(-8.0f * dt));
		g.x += -sinf(g.heading) * g.speed * dt;
		g.y +=  cosf(g.heading) * g.speed * dt;
		// camera settles behind the move direction (GTA-like chase follow)
		s_camYaw += wrap(target - s_camYaw) * (1.0f - expf(-4.5f * dt));
	} else {
		g.speed += (0.0f - g.speed) * (1.0f - expf(-10.0f * dt));
	}

	const char* an = g.speed > 7.0f ? "sprint_civi" :
	                 g.speed > 3.5f ? "run_civi" :
	                 g.speed > 0.4f ? "WALK_player" : "IDLE_stance";
	cj::setAnim(an);
	cj::advance(1);
}

// Leftovers from the demo driver: wanted steps, weapon cycling, health/armour
// wiggle and the breath cycle so every HUD widget still has data to chew on.
static void updateDemo() {
	const u32 v = timing::vblanks();
	static u32 s_t0;
	if (!s_t0) s_t0 = v;
	const u32 t = v - s_t0;

	g.wantedLevel = k_wantedSteps[(t / 300) % (sizeof k_wantedSteps / sizeof k_wantedSteps[0])];
	g.weapon = k_weapons[(t / 480) % (sizeof k_weapons / sizeof k_weapons[0])];
	g.vehicleName = (t % 1200) < 300 ? (const GxtChar*)"LANDSTALKER" : nullptr;
	g.health = g.maxHealth * (0.55f + 0.45f * sinf((float)t / 300.0f));
	g.armour = 50.0f + 45.0f * sinf((float)t / 200.0f + 1.0f);

	const int ph = (int)(t % 2400);
	g.breath = ph < 900 ? 212.0f * (1.0f - (float)ph / 900.0f)
	                    : 212.0f + (float)(ph - 900) * 0.4f;
	if (g.breath > 300.0f) g.breath = 300.0f;

	const u32 mins = t / 60;
	g_clockH = (12 + mins / 60) % 24;
	g_clockM = mins % 60;
}

void update() {
	updateMove();
	updateDemo();

	if (--s_zoneTimer <= 0) {
		s_zoneTimer = 30;
		g.zoneName = zones::nameAt(g.x, g.y);
	}
}

}
