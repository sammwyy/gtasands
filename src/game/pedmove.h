#pragma once

#include <stdint.h>

#include "game/cj.h"

// Ped locomotion: CTaskSimplePlayerOnFoot::PlayerControlZelda (stick ->
// move blend ratio + aiming rotation), the heading turn, and a
// CPlayerPed::SetRealMoveAnim-style blend of the motion group's
// idle / walk_start / walk / run / sprint / run_stop(R) / idle_tired.
// The state is per ped and the clips go through a PoseApi, so AI peds can
// reuse it with their own controller and model once they exist.
namespace pedmove {

enum MoveState : uint8_t { PEDMOVE_NONE, PEDMOVE_WALK, PEDMOVE_RUN, PEDMOVE_SPRINT };

// Where the pose goes (cj:: for the player).
struct PoseApi {
	void (*pose)(const cj::Clip* clips, int n);
	float (*phase)(const char* name);     // -1 when not playing
	bool (*ended)(const char* name);
};

struct State {
	float moveBlend;          // CPlayerPedData::m_fMoveBlendRatio, 0..2
	float prevBlend;          // blend before this step's control (picks the stop clip)
	MoveState moveState;
	uint8_t phase;            // internal: idle / start / move / stop
	bool stopR, restartStart, restartStop;
	float w[7];               // live blend weights per clip slot (see pedmove.cpp)
};

struct Input {
	float walkLR, walkUD;     // pad stick, -128..128
	bool pedWalk;             // walk modifier: blend capped at 1
	bool allowed;             // false while an action owns the ped (jump, punch, aim)
	float camYaw;             // TheCamera.m_fOrientation equivalent (0 = north)
};

void reset(State& s);

// PlayerControlZelda. Updates moveBlend (rate-limited 0.07 per 30 Hz step)
// and returns true with the wanted heading when the stick is deflected.
bool control(State& s, const Input& in, float timestep, float& aimHeading);

// m_fHeadingChangeRate-style turn toward `target` (radians, wrapped).
void turn(float& heading, float target, float dt, bool aiming);

// Blend weights + clip list for this step. `sprinting` = ControlButtonSprint
// result >= 1, `tired` = sprint energy spent (idle_tired instead of idle).
void setRealMoveAnim(State& s, const cj::Motion& mo, bool sprinting, bool tired, float dt, const PoseApi& api);

// The ped is busy with something else this step (jump, fight, aim): forget
// the locomotion pose so it restarts cleanly afterwards.
void suspend(State& s);

// Back on foot after a jump/landing: keeps the move blend, restarts the
// cycles from zero weight (they crossfade in while the land clip blends out),
// straight into the move phase when the stick is held (no walk_start).
void resume(State& s, bool moving);

// Live weight of the walk / run / sprint cycle (0..1) and the heaviest one.
enum Cycle { CYCLE_WALK, CYCLE_RUN, CYCLE_SPRINT };
float cycleWeight(const State& s, Cycle c);
Cycle dominantCycle(const State& s);

// Locomotion debug (PERF tab): heaviest cycle and blend.
const char* phaseName(const State& s);
bool isRunning(const State& s);     // run is the dominant cycle
bool isSprinting(const State& s);

}
