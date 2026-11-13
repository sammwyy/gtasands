#include "game/pedmove.h"

#include <math.h>
#ifdef ARM9
#include <nds/arm9/math.h>
#endif
#include "game/trig.h"

namespace pedmove {

enum Phase : uint8_t { PH_IDLE, PH_START, PH_MOVE, PH_STOP };
enum Slot : uint8_t { W_IDLE, W_WALK, W_RUN, W_SPRINT, W_START, W_STOP, W_TIRED, W_COUNT };

// PlayerControlZelda: blend = |stick| / 60, so a full digital deflection
// (128) asks for 2.13 = a run; the walk key caps it at 1.0. It moves toward
// the target by at most 0.07 per CTimer step (1 step = 1/30 s).
static constexpr float STICK_TO_BLEND = 1.0f / 60.0f;
static constexpr float BLEND_RATE = 0.07f;
static constexpr float BLEND_MAX = 2.0f;           // SetRealMoveAnim saturates at a full run
// Turn speed toward m_fAimingRotation. Tuned to the PC feel: a 180 degree
// about-face in ~0.3 s on foot, a touch quicker while aiming.
static constexpr float TURN_RATE = 10.5f;          // rad/s
static constexpr float TURN_RATE_AIM = 12.0f;
// Clip weights chase their target at 8/s (~0.12 s crossfades).
static constexpr float WEIGHT_RATE = 8.0f;

static float wrap(float a) {
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}

static void approach(float& v, float target, float step) {
	if (v < target) v = v + step < target ? v + step : target;
	else if (v > target) v = v - step > target ? v - step : target;
}

static float stickLength(float x, float y) {
#ifdef ARM9
	return hw_sqrtf(x * x + y * y);
#else
	return sqrtf(x * x + y * y);
#endif
}

void reset(State& s) {
	s = {};
	s.w[W_IDLE] = 1.0f;
}

bool control(State& s, const Input& in, float timestep, float& aimHeading) {
	s.prevBlend = s.moveBlend;
	float target = in.allowed && (in.walkLR != 0.0f || in.walkUD != 0.0f)
	                   ? stickLength(in.walkLR, in.walkUD) * STICK_TO_BLEND : 0.0f;
	if (in.pedWalk && target > 1.0f) target = 1.0f;
	if (target <= 0.0f) {
		// releasing the stick drops the blend at once (the stop clip and
		// the weight crossfade smooth it, not the ratio)
		s.moveBlend = 0.0f;
		s.moveState = PEDMOVE_NONE;
		return false;
	}
	if (target > BLEND_MAX) target = BLEND_MAX;
	// stick in camera space: GetRadianAngleBetweenPoints(0,0,-x,y) - cam
	const float ix = in.walkLR / 128.0f, iy = in.walkUD / 128.0f;
	float sy, cy;
	trig::sincos(in.camYaw, sy, cy);
	const float dx = -sy * iy + cy * ix;
	const float dy = cy * iy + sy * ix;
	aimHeading = trig::atan2(-dx, dy);
	approach(s.moveBlend, target, BLEND_RATE * timestep);
	s.moveState = s.moveBlend > 1.0f ? PEDMOVE_RUN : PEDMOVE_WALK;
	return true;
}

void turn(float& heading, float target, float dt, bool aiming) {
	float d = wrap(target - heading);
	const float maxTurn = (aiming ? TURN_RATE_AIM : TURN_RATE) * dt;
	if (d > maxTurn) d = maxTurn;
	if (d < -maxTurn) d = -maxTurn;
	heading = wrap(heading + d);
}

void suspend(State& s) {
	s.phase = PH_IDLE;
	for (float& w : s.w) w = 0.0f;
}

void resume(State& s, bool moving) {
	for (float& w : s.w) w = 0.0f;
	s.phase = moving && s.moveBlend > 0.0f ? PH_MOVE : PH_IDLE;
}

float cycleWeight(const State& s, Cycle c) { return s.w[W_WALK + c]; }

Cycle dominantCycle(const State& s) {
	Cycle best = CYCLE_WALK;
	for (int c = CYCLE_RUN; c <= CYCLE_SPRINT; c++)
		if (s.w[W_WALK + c] > s.w[W_WALK + best]) best = (Cycle)c;
	return best;
}

static float phaseOr(const PoseApi& api, const char* name, float fallback) {
	const float p = api.phase(name);
	return p >= 0.0f ? p : fallback;
}

void setRealMoveAnim(State& s, const cj::Motion& mo, bool sprinting, bool tired, float dt, const PoseApi& api) {
	const bool moving = s.moveBlend > 0.0f;
	if (sprinting && moving) s.moveState = PEDMOVE_SPRINT;

	// phase machine: walk_start out of idle, run_stop(R) when a run is let go
	if (moving) {
		if (s.phase == PH_IDLE || s.phase == PH_STOP) {
			if (sprinting) s.phase = PH_MOVE;
			else { s.phase = PH_START; s.restartStart = true; }
		} else if (s.phase == PH_START && (sprinting || api.ended(mo.start) || api.phase(mo.start) < 0.0f)) {
			s.phase = PH_MOVE;
		}
	} else if (s.phase == PH_MOVE || s.phase == PH_START) {
		if (s.phase == PH_MOVE && s.prevBlend > 1.0f) {
			// which foot is down: the dominant cycle's phase picks run_stop
			// (left foot, < 0.5) or run_stopR, as SetRealMoveAnim does
			float ph = phaseOr(api, mo.run, -1.0f);
			if (s.w[W_SPRINT] >= s.w[W_RUN] && s.w[W_SPRINT] >= s.w[W_WALK]) ph = phaseOr(api, mo.sprint, ph);
			else if (s.w[W_WALK] >= s.w[W_RUN]) ph = phaseOr(api, mo.walk, ph);
			if (ph < 0.0f) ph = phaseOr(api, mo.start, 0.0f);
			s.stopR = ph >= 0.5f;
			s.phase = PH_STOP;
			s.restartStop = true;
		} else {
			s.phase = PH_IDLE;   // walks just settle into idle
		}
	} else if (s.phase == PH_STOP) {
		const char* sn = s.stopR ? mo.stopR : mo.stop;
		if (api.ended(sn) || api.phase(sn) < 0.0f) s.phase = PH_IDLE;
	}

	// targets: idle<->walk over blend 0..1, walk<->run over 1..2, sprint alone
	float t[W_COUNT] = {};
	switch (s.phase) {
	case PH_IDLE: t[tired ? W_TIRED : W_IDLE] = 1.0f; break;
	case PH_START: t[W_START] = 1.0f; break;
	case PH_MOVE:
		if (sprinting) t[W_SPRINT] = 1.0f;
		else if (s.moveBlend <= 1.0f) { t[W_WALK] = s.moveBlend; t[W_IDLE] = 1.0f - s.moveBlend; }
		else { t[W_WALK] = 2.0f - s.moveBlend; t[W_RUN] = s.moveBlend - 1.0f; }
		break;
	default: t[W_STOP] = 1.0f; break;
	}
	for (int i = 0; i < W_COUNT; i++) approach(s.w[i], t[i], WEIGHT_RATE * dt);

	// The three cycles share one foot phase: the heaviest leads and the
	// others are kept on its phase (CAnimBlendAssociation::SyncAnimation),
	// so walk->run->sprint crossfades never cross the legs.
	const char* cyc[3] = { mo.walk, mo.run, mo.sprint };
	int lead = 0;
	for (int i = 1; i < 3; i++)
		if (s.w[W_WALK + i] > s.w[W_WALK + lead]) lead = i;
	const float leadPh = s.w[W_WALK + lead] > 0.05f ? api.phase(cyc[lead]) : -1.0f;

	cj::Clip clips[W_COUNT];
	int n = 0;
	auto add = [&](const char* name, float w, bool loop, bool extract, bool sync, bool restart, float phase) {
		if (w <= 0.01f || n >= W_COUNT) return;
		clips[n++] = { name, w, loop, extract, sync, restart, phase };
	};
	add(mo.idle, s.w[W_IDLE], true, false, false, false, 0.0f);
	add(mo.tired, s.w[W_TIRED], true, false, false, false, 0.0f);
	add(mo.start, s.w[W_START], false, true, false, s.restartStart, 0.0f);
	for (int i = 0; i < 3; i++)
		add(cyc[i], s.w[W_WALK + i], true, true, i != lead && leadPh >= 0.0f, false, leadPh);
	add(s.stopR ? mo.stopR : mo.stop, s.w[W_STOP], false, true, false, s.restartStop, 0.0f);
	s.restartStart = s.restartStop = false;
	api.pose(clips, n);
}

const char* phaseName(const State& s) {
	static const char* const k[] = { "idle", "start", "move", "stop" };
	return s.phase < 4 ? k[s.phase] : "?";
}

bool isRunning(const State& s) {
	return s.phase == PH_MOVE && s.w[W_RUN] > s.w[W_SPRINT] && s.w[W_RUN] > 0.4f;
}

bool isSprinting(const State& s) { return s.w[W_SPRINT] > 0.5f; }

}
