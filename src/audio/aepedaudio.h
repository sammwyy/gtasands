#pragma once

#include <stdint.h>

// CAEPedAudioEntity (Audio/Entities/AEPedAudioEntity.cpp) for the player,
// plus the CJ pain voice of CAEPedSpeechAudioEntity (PAIN_A_CARL). The
// events come from game/player.cpp the way CPed::PlayFootSteps,
// CTaskComplexInAirAndLand and CTaskSimpleFight raise them.
namespace aeped {

// SurfaceInfos_c::LoadSurfaceAudioInfos (data/surfaud.dat): audio class per
// COL surface id. Loading job.
bool loadSurfaceAudio();
void preload();   // StaticInitialise: FEET_GENERIC (+ the swing / pain sounds)

struct Pos { float x, y, z; };   // the ped's origin (SA world, ~1 m above the feet)

// HandleFootstepEvent. volume / speed per move state (CPed::PlayFootSteps:
// walk -12 dB x0.9, run -6 x1.1, sprint 0 x1.2), surface = contact surface.
void footstep(bool left, float volume, float speed, uint8_t surface, const Pos& p);
// HandleLandingEvent: AE_PED_LAND_ON_FEET_AFTER_FALL / AE_PED_COLLAPSE_AFTER_FALL
void landing(bool collapse, uint8_t surface, const Pos& p);
// HandlePedSwing (unreversed on PC; the fist / weapon swipe of the weapons bank)
void swing(int weaponType, const Pos& p);

// CAEPedSpeechAudioEntity pain contexts the player raises (gPainSpeechLookup, CARL)
enum Pain : uint8_t { PAIN_CJ_BOXING, PAIN_CJ_HIGH_FALL };
void sayPain(Pain ctx, const Pos& p);

// The generic ped voices of PAIN_A_MALE / PAIN_A_FEMALE (gPainSpeechLookup
// CTX_PAIN_PED_*): AI peds hit, dying, panicking. One voice at a time per
// speech slot, `volume` dB on top (distant peds quieter).
enum PedPain : uint8_t { PAIN_PED_DEATH_HIGH, PAIN_PED_DEATH_LOW, PAIN_PED_GENERIC, PAIN_PED_PANIC };
void sayPedPain(bool female, PedPain ctx, const Pos& p);
// HandlePedHit (AE_PED_HIT_HIGH_UNARMED): a fist / melee weapon lands on a ped
void pedHit(const Pos& p);
// AE_BULLET_HIT on a ped (GENRL_BULLET_HITS BULPED1..3)
void bulletHitPed(const Pos& p);

}
