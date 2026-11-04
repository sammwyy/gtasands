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
static float s_camPitch;        // radians above horizontal; + looks down
static float s_betaSpeed;
static int s_zoneTimer;

// radians -> [-pi, pi]
static float wrap(float a) {
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}

float camYaw() { return s_camYaw; }
float camPitch() { return s_camPitch; }

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
	s_camPitch = 0.42f;     // a little above CJ, the default follow-ped framing
	s_betaSpeed = 0;
	s_zoneTimer = 0;
	cj::init();
}

// Follow-ped camera, the on-foot half of CCam::Process_FollowPed_SA.
// Beta (yaw) is where the camera looks; the eye sits behind that heading.
// L/R orbit beta and X/Y orbit alpha, like the right stick. While the stick
// is neutral and CJ is moving, beta eases behind his heading (WellBufferMe:
// speed approaches diff*topSpeed, accelerated by speedStep). It does not
// chase the wish direction — that coupled with camera-relative movement and
// made him spiral.
// CTimer timestep is 1.0 at 30 fps. We step at 60, so 0.5.
static constexpr float CAM_TIMESTEP = 0.5f;

static void followCam(float targetYaw) {
	const float diff = wrap(targetYaw - s_camYaw);
	const float desired = diff * 0.10f;          // topSpeed
	const float gap = desired - s_betaSpeed;
	float step = fabsf(gap) * 0.020f * CAM_TIMESTEP; // speedStep
	if (step > fabsf(gap)) step = fabsf(gap);
	s_betaSpeed += gap > 0.0f ? step : -step;
	s_camYaw = wrap(s_camYaw + fminf(CAM_TIMESTEP, 10.0f) * s_betaSpeed);
}

// D-pad moves camera-relative (up = away from camera), same as
// CTaskSimplePlayerOnFoot::PlayerControlZelda. B = run, A = sprint.
// The step itself comes from the anim's root bone, not a made-up speed,
// so the feet stay on the ground they were authored for.
static void updateMove() {
	const input::State& in = input::get();
	const float dt = 1.0f / 60.0f;
	const float ix = (in.held & KEY_RIGHT ? 1.0f : 0.0f) - (in.held & KEY_LEFT ? 1.0f : 0.0f);
	const float iy = (in.held & KEY_UP ? 1.0f : 0.0f) - (in.held & KEY_DOWN ? 1.0f : 0.0f);
	const bool looking = in.held & (KEY_L | KEY_R);

	if (in.held & KEY_L) s_camYaw -= 2.0f * dt;
	if (in.held & KEY_R) s_camYaw += 2.0f * dt;
	if (in.held & KEY_X) s_camPitch -= 1.2f * dt;
	if (in.held & KEY_Y) s_camPitch += 1.2f * dt;
	if (s_camPitch < -0.45f) s_camPitch = -0.45f;   // ClipAlpha, looking up
	if (s_camPitch > 1.05f) s_camPitch = 1.05f;     // looking down
	s_camYaw = wrap(s_camYaw);

	const bool moving = ix != 0.0f || iy != 0.0f;
	const char* an = "IDLE_stance";
	if (moving) {
		// camera-relative wish direction in SA xy. Facing is (-sin h, cos h).
		const float dx = -sinf(s_camYaw) * iy + cosf(s_camYaw) * ix;
		const float dy =  cosf(s_camYaw) * iy + sinf(s_camYaw) * ix;
		const float target = atan2f(-dx, dy);
		float turn = wrap(target - g.heading);
		const float maxTurn = 6.0f * dt;
		if (turn > maxTurn) turn = maxTurn;
		if (turn < -maxTurn) turn = -maxTurn;
		g.heading = wrap(g.heading + turn);
		an = in.held & KEY_A ? "sprint_civi" : in.held & KEY_B ? "run_civi" : "WALK_player";
	}
	cj::setAnim(an);
	cj::advance(1);

	float mx, my;
	cj::rootDelta(mx, my);
	const float c = cosf(g.heading), s = sinf(g.heading);
	const float wx = mx * c - my * s;   // right*(cos,sin) + forward*(-sin,cos)
	const float wy = mx * s + my * c;
	g.x += wx;
	g.y += wy;
	g.speed = sqrtf(wx * wx + wy * wy) / dt;

	if (moving && !looking) followCam(g.heading);
	else s_betaSpeed = 0.0f;
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
