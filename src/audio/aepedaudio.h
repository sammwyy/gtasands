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

}
