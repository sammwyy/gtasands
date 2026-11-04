#pragma once

#include <stdint.h>

// CAEVehicleAudioEntity, reduced to cars (AE_CAR):
//  - dummy engine (ProcessDummyVehicleEngine / ProcessDummyStateTransition):
//    the dummy bank's idle (1) and rev (0) loops crossfaded by speed ratio,
//  - player engine (ProcessPlayerVehicleEngine, states ID / AC / CRZ): the
//    player bank's acceleration sweep (0) replayed per audio gear with
//    FrqPerGearFactor, cruise (1), deceleration (2) + the dummy idle,
//  - horn (SND_BANK_GENRL_HORN, gVehicleAudioSettings horn type / pitch),
//    tyre skid (VEHICLE_GEN tarskidtwin / grass / gravel by surface),
//  - AddAudioEvent: doors open / close by door type, engine start, crash
//    (GENRL_COLLISIONS colcarNN by impulse), ped hit, windscreen, explosion
//    (GENRL_EXPLOSIONS) and the burning engine (fire loop).
namespace aeveh {

int create(int modelId);          // -1 if the pool is full
void destroy(int h);

struct Params {
	float x, y, z;
	float speed;                  // forward speed, units / 50 Hz step (negative = reversing)
	float maxVelocity;            // cTransmission::m_MaxVelocity
	float revs;                   // in-gear revs ratio 0..1 (RealRevsRatio)
	float gas;                    // m_GasPedal -1..1
	uint8_t gear, numGears;
	bool engineOn, player, wrecked, horn, onFire;
	float skid;                   // sum of the wheels' skid values
	uint8_t surface;              // contact surface (skid sound type)
};
void update(int h, const Params& p);

enum Event : uint8_t { EV_DOOR_OPEN, EV_DOOR_CLOSE, EV_START_ENGINE, EV_CRASH, EV_PED_HIT, EV_WINDSCREEN, EV_EXPLOSION, EV_PART_OFF };
void event(int h, Event e, float x, float y, float z, float intensity = 0.0f);

}
