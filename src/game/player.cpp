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
static int s_betaSign;          // committed yaw direction across the ±pi cut
static int s_zoneTimer;

// On-foot anim flow, the part of CPlayerPed::SetRealMoveAnim this port uses.
// Weights ease at the original blend delta (8/s, ~0.125 s). The stick release
// itself is instant. run_stop / run_stopR only plant a run or a sprint
// (blend ratio > 1). A walk fades back to idle.
enum Phase { PH_IDLE, PH_START, PH_MOVE, PH_STOP };
static Phase s_phase;
static float s_ratio;           // 0 idle .. 1 walk .. 2 run
static float s_energy;          // sprint bar, 0..100
static bool s_spent;            // bar hit empty -> idle_tired until it recovers
static bool s_stopR;            // which plant clip, chosen from the foot phase
static bool s_restartStart;
static bool s_restartStop;
static float s_wIdle, s_wWalk, s_wRun, s_wSprint, s_wStart, s_wStop, s_wTired;

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
	g.fat = 0.0f;
	g.muscle = 0.0f;
	g.sprint = 100.0f;
	g_clockH = 12; g_clockM = 0;
	s_camYaw = 0;
	s_camPitch = 0.42f;     // a little above CJ, the default follow-ped framing
	s_betaSpeed = 0;
	s_betaSign = 0;
	s_zoneTimer = 0;
	s_phase = PH_IDLE;
	s_ratio = 0;
	s_energy = 100.0f;
	s_spent = false;
	s_stopR = false;
	s_restartStart = s_restartStop = false;
	s_wIdle = 1.0f;
	s_wWalk = s_wRun = s_wSprint = s_wStart = s_wStop = s_wTired = 0.0f;
	cj::init();
	cj::setStats(0.0f, 0.0f);
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
	float diff = wrap(targetYaw - s_camYaw);
	const float absDiff = fabsf(diff);
	// wrap() flips sign at ±pi. That angle is the eye in front of CJ, and a
	// speed still near zero reverses every frame, so the view slides across
	// his front and chatters left/right. Past ~150 deg, keep the direction
	// already in progress until the eye is back behind him.
	if (absDiff > 2.6f) {
		if (s_betaSign == 0)
			s_betaSign = (s_betaSpeed < 0.0f || (s_betaSpeed == 0.0f && diff < 0.0f)) ? -1 : 1;
		diff = (float)s_betaSign * absDiff;
	} else if (absDiff < 0.8f) {
		s_betaSign = 0;
	}

	// Ped yaws at most 0.1 rad/frame. Steady lag while turning is that rate
	// over (timestep * topSpeed); 0.50 leaves ~0.3 rad, so the eye stays
	// behind instead of stalling near the front. Cap stops a 180 from snapping.
	float desired = diff * 0.50f;
	if (desired > 0.35f) desired = 0.35f;
	if (desired < -0.35f) desired = -0.35f;
	const float gap = desired - s_betaSpeed;
	float step = fabsf(gap) * 0.25f * CAM_TIMESTEP;
	if (step > fabsf(gap)) step = fabsf(gap);
	s_betaSpeed += gap > 0.0f ? step : -step;
	s_camYaw = wrap(s_camYaw + fminf(CAM_TIMESTEP, 10.0f) * s_betaSpeed);
}

// Blend delta from AnimBlendAssociation::UpdateBlend: amount += 8 * seconds.
static void approach(float& v, float target, float dt) {
	const float step = 8.0f * dt;
	if (v < target) v = v + step < target ? v + step : target;
	else if (v > target) v = v - step > target ? v - step : target;
}

static float clipOr(const char* name, float fallback) {
	const float p = cj::clipPhase(name);
	return p >= 0.0f ? p : fallback;
}

// D-pad moves camera-relative (up = away from camera), same as
// CTaskSimplePlayerOnFoot::PlayerControlZelda. B = run, A = sprint.
// The step itself comes from the anim's root bone, not a made-up speed,
// so the feet stay on the ground they were authored for.
//
// Flow (SetRealMoveAnim, on foot, unarmed):
//   idle -> walk_start (once) -> walk, blended into run by the move ratio
//   A -> sprint, until the bar empties (then run, and idle_tired once stopped)
//   release from a run/sprint -> run_stop or run_stopR, then idle
//   release from a walk or walk_start -> idle (no walk_stop in the group)
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
	}

	// Sprint energy. STAT_STAMINA is 0 on a new game, so the bar lasts ~5 s
	// and refills in ~8 s. Hitting empty forces a run and, once stopped, idle_tired.
	const bool wantSprint = moving && (in.held & KEY_A);
	// New-game STAT_STAMINA is 0, which is the fast end of the drain
	// (~21.6/s, a full bar lasts about five seconds). Recover is 12/s.
	const float drain = 21.6f;
	if (wantSprint && s_energy > 0.0f) {
		s_energy -= drain * dt;
		if (s_energy <= 0.0f) { s_energy = 0.0f; s_spent = true; }
	} else {
		s_energy += 12.0f * dt;
		if (s_energy > 100.0f) s_energy = 100.0f;
		if (s_energy > 40.0f) s_spent = false;
	}
	g.sprint = s_energy;
	const bool sprinting = wantSprint && s_energy > 0.0f;
	// Bar empty while A is still held: drop to run, not back to a walk.
	const bool forcedRun = wantSprint && !sprinting;

	// Captured before a release zeroes it: this is the gait we're leaving.
	const float gait = s_ratio;
	if (sprinting) s_ratio = 2.0f;
	else if (moving) {
		// PlayerControlZelda: fMaximumMoveBlendRatio steps at ~0.07 * timestep.
		// Timestep 1 at 30 fps is 2.1/s. Digital pad: walk = 1, B = run = 2.
		const float target = ((in.held & KEY_B) || forcedRun) ? 2.0f : 1.0f;
		const float step = 2.1f * dt;
		if (s_ratio < target) s_ratio = s_ratio + step < target ? s_ratio + step : target;
		else if (s_ratio > target) s_ratio = s_ratio - step > target ? s_ratio - step : target;
	} else {
		s_ratio = 0.0f;
	}

	const cj::Motion mo = cj::motion();
	if (moving) {
		if (s_phase == PH_IDLE || s_phase == PH_STOP) {
			if (sprinting) s_phase = PH_MOVE;
			else { s_phase = PH_START; s_restartStart = true; }
		} else if (s_phase == PH_START &&
		           (sprinting || cj::clipEnded(mo.start) || cj::clipPhase(mo.start) < 0.0f)) {
			s_phase = PH_MOVE;
		}
	} else if (s_phase == PH_MOVE || s_phase == PH_START) {
		// SetRealMoveAnim plays run_stop only above a walk (move blend > 1).
		if (s_phase == PH_MOVE && gait > 1.0f) {
			float ph = clipOr(mo.run, -1.0f);
			if (s_wSprint >= s_wRun && s_wSprint >= s_wWalk) ph = clipOr(mo.sprint, ph);
			else if (s_wWalk >= s_wRun) ph = clipOr(mo.walk, ph);
			if (ph < 0.0f) ph = clipOr(mo.start, 0.0f);
			s_stopR = ph >= 0.5f;
			s_phase = PH_STOP;
			s_restartStop = true;
		} else {
			s_phase = PH_IDLE;
		}
	} else if (s_phase == PH_STOP) {
		const char* sn = s_stopR ? mo.stopR : mo.stop;
		if (cj::clipEnded(sn) || cj::clipPhase(sn) < 0.0f) s_phase = PH_IDLE;
	}

	float tIdle = 0, tWalk = 0, tRun = 0, tSprint = 0, tStart = 0, tStop = 0, tTired = 0;
	if (s_phase == PH_IDLE) {
		if (s_spent) tTired = 1.0f;
		else tIdle = 1.0f;
	} else if (s_phase == PH_START) {
		tStart = 1.0f;
	} else if (s_phase == PH_MOVE) {
		if (sprinting) tSprint = 1.0f;
		else if (s_ratio <= 1.0f) { tWalk = s_ratio; tIdle = 1.0f - s_ratio; }
		else { tWalk = 2.0f - s_ratio; tRun = s_ratio - 1.0f; }
	} else {
		tStop = 1.0f;
	}
	approach(s_wIdle, tIdle, dt);
	approach(s_wWalk, tWalk, dt);
	approach(s_wRun, tRun, dt);
	approach(s_wSprint, tSprint, dt);
	approach(s_wStart, tStart, dt);
	approach(s_wStop, tStop, dt);
	approach(s_wTired, tTired, dt);

	cj::Clip clips[7];
	int n = 0;
	const float walkPh = clipOr(mo.walk, 0.0f);
	auto add = [&](const char* name, float w, bool loop, bool extract, bool sync, bool restart, float phase) {
		if (w <= 0.01f || n >= 7) return;
		clips[n++] = { name, w, loop, extract, sync, restart, phase };
	};
	add(mo.idle, s_wIdle, true, false, false, false, 0.0f);
	add(mo.tired, s_wTired, true, false, false, false, 0.0f);
	add(mo.start, s_wStart, false, true, false, s_restartStart, 0.0f);
	add(mo.walk, s_wWalk, true, true, false, false, 0.0f);
	// Walk and run are ANIMATION_IS_SYNCRONISED: same foot phase, weights = ratio.
	add(mo.run, s_wRun, true, true, s_wWalk > 0.05f, false, walkPh);
	add(mo.sprint, s_wSprint, true, true, false, false, 0.0f);
	add(s_stopR ? mo.stopR : mo.stop, s_wStop, false, true, false, s_restartStop, 0.0f);
	s_restartStart = s_restartStop = false;

	cj::pose(clips, n);
	cj::advance(1);

	float mx, my;
	cj::rootDelta(mx, my);
	const float c = cosf(g.heading), s = sinf(g.heading);
	const float wx = mx * c - my * s;   // right*(cos,sin) + forward*(-sin,cos)
	const float wy = mx * s + my * c;
	g.x += wx;
	g.y += wy;
	g.speed = sqrtf(wx * wx + wy * wy) / dt;

	if (!looking && (moving || s_phase == PH_START || s_phase == PH_MOVE || s_phase == PH_STOP))
		followCam(g.heading);
	else { s_betaSpeed = 0.0f; s_betaSign = 0; }
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

	// Body shape, same demo cadence as the wanted stars: 20 s normal, 20 s
	// fat, 20 s muscle. The real stats will replace this when gameplay lands.
	const int body = (int)((t / 1200) % 3);
	g.fat = body == 1 ? 1000.0f : 0.0f;
	g.muscle = body == 2 ? 1000.0f : 0.0f;
	cj::setStats(g.fat, g.muscle);
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
