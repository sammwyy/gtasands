#pragma once

#include <stdint.h>

// CTaskSimpleFight's CMeleeInfo for the fist fighting style both CJ and the
// street peds use: melee.dat START_COMBO UNARMED_1 (ANIMGROUP melee_1 = the
// ped.ifp FightA_* clips). hit / chain are anim times in 30 fps frames: the
// strike lands at `hit`, a queued next move starts at `chain` (99 = when the
// clip ends). The hit column picks the victim's reaction (CTaskSimpleBeHit /
// CTaskComplexFallAndGetUp): 1 / 2 knock down, 4 a kick on the floor, the
// others the be-hit anims (HitA_n from the front, HIT_L / back / R).
namespace melee {

enum Move : uint8_t { MOVE_HIT_1, MOVE_HIT_2, MOVE_HIT_3, MOVE_GROUND, MOVE_MOVING, NUM_MOVES };
enum Level : uint8_t { LEVEL_H, LEVEL_L, LEVEL_G, LEVEL_HL };

struct Info {
	const char* clip;
	float hit, chain, radius;
	uint8_t level, damage, hitType;
};

constexpr Info k_unarmed[NUM_MOVES] = {
	{ "FightA_1", 5.0f, 10.0f, 0.4f, LEVEL_H, 6, 5 },
	{ "FightA_2", 8.0f, 17.0f, 0.4f, LEVEL_H, 9, 5 },
	{ "FightA_3", 10.0f, 20.0f, 0.4f, LEVEL_H, 15, 1 },
	{ "FightA_G", 10.5f, 20.0f, 0.4f, LEVEL_G, 25, 4 },
	{ "FightA_M", 7.0f, 10.0f, 0.4f, LEVEL_HL, 5, 5 },
};
constexpr float RANGE = 1.6f;    // RANGES: how close the attacker closes in
constexpr float REACH = 1.35f;   // the strike sphere (FightSetUpCol) reaches this far in front

inline int hitTicks(int m) { return (int)(k_unarmed[m].hit * 2.0f); }      // 30 fps frames -> 60 Hz ticks
inline int chainTicks(int m) { return (int)(k_unarmed[m].chain * 2.0f); }
inline bool knocksDown(int m) { return k_unarmed[m].hitType == 1 || k_unarmed[m].hitType == 2; }
// the be-hit anim of a front hit by move m (ANIM_ID_FIGHT_HIT_1 + move)
inline const char* frontHitClip(int m) { return m == MOVE_HIT_1 ? "HitA_1" : m == MOVE_HIT_2 ? "HitA_2" : m == MOVE_HIT_3 ? "HitA_3" : "HIT_front"; }

}
