#pragma once

// Internals shared by game/vehicle.cpp (pool, damage, drawing) and
// game/vehphys.cpp (the fixed-point physics). Not for other modules.
#include "assets/ds_formats.h"
#include "game/fx.h"
#include "game/vehicle.h"

namespace veh {

using fx::q;
using fx::V;

constexpr q TS = fx::F(50.0 / 60.0);          // CTimer::ms_fTimeStep per 60 Hz step
constexpr q GRAVITY = fx::F(0.008);           // per step^2
constexpr int NUM_SPH = 10;

struct Gear { q maxVel, up, down; };

// tHandlingData (+ bike / boat / flying) in fixed point, game units.
struct Hand {
	q traction, tractionLoss, tractionBias;
	q maxVel, maxRev, engineAccel, engineInertia;
	q brakeDecel, brakeBias, steerLock;       // steerLock in radians
	q suspForce, suspDamp, suspUpper, suspLower, suspBias, antiDive;
	q idleBrake;                              // WHEEL_FRICTION / mass (x 0.1 under 500 kg)
	q kTurn;                                  // mass / turnMass: forces are kept per unit mass
	q airRes;                                 // m_fAirResistance
	uint8_t numGears;
	char drive;                               // 'F' / 'R' / '4'
	uint32_t hflags, mflags;
	// tBikeHandlingData
	q maxLean, desLean, speedSteer, slipSteer;
	// tBoatHandlingData
	q thrustY, thrustZ, thrustAppZ, aqForce, aqLimit, aqOffset;
	q boatMoveRes[3], boatTurnRes[3];         // ^TS
	q buoy;                                   // up accel per m of hull sample depth
	// tFlyingHandlingData
	bool fly;
	q thrust, thrustFallOff, yaw, yawStab, sideSlip, roll, rollStab, pitch, pitchStab;
	q formLift, attackLift, gearDownL, windMult, moveResTS;
	q lnTurnRes[3];                           // TS * ln(m_vecTurnRes)
	q speedRes[3];
};

struct Car {
	Vehicle v;                  // public view (floats), see publish()
	ds::VehInfo h;              // the record as read (floats; damage, audio, names)
	Hand k;
	Gear gears[7];
	// CPhysical, fixed point
	fx::wp pos[3];              // world, Q24 in s64
	V right, fwd, up;           // CMatrix rows
	V move, turn;               // m_vecMoveSpeed / m_vecTurnSpeed, per step
	V com;                      // m_vecCentreOfMass, model space
	q radius;                   // around pos: the swept broad phase box
	V bmin, bmax;               // model body bounds
	// CAutomobile wheels (eCarWheel order)
	V lineStart[4], lineEnd[4]; // model space suspension lines
	q lineLen[4], springLen, wheelR[2];
	q susp[4], frac[4];
	V contactRel[4], contactN[4];   // world axes, relative to pos
	uint8_t surf[4], wheelState[4];
	q wheelCounts[4], wheelRot[4], wheelSpin[4], slip[4];
	q steer, rawSteer, gas, brake;
	q inertia1, inertia2;
	uint8_t gear, numContact, driveOnGround;
	q speedFwd;
	// body spheres (model space) for the map / other vehicles
	uint8_t numSph;
	V sph[NUM_SPH];
	q sphR[NUM_SPH];
	// driver input
	Controls pad;
	// contact of the step: strongest impulse (per unit mass) and where
	q impactJ;
	V impactRel;
	// bike
	q lean;
	// boat
	q immersion;                // 0..1 of the hull samples under water
	// heli / plane
	q rotorSpeed, rotorAngle, propAngle;
	// train (kinematic on a tracks.dat loop)
	int8_t track;
	float trackDist, trackSpeed;
	int16_t roadFrom, roadTo, roadNext;
	uint32_t trafficBorn;
	uint16_t trafficOffscreen;
	bool trafficSeen;
	// state
	bool placed;
	uint16_t placeWait;         // frames the ready model waited for its ground collision
	uint32_t hasDam;            // components with a *_dam variant
	uint32_t doorSound;         // door edges already played (bit per door: open)
	bool windscreenSaid;
	float wreckBurn;            // ms the wreck keeps burning
	uint16_t fakePhysics;       // resting steps
	float damageIntensity, impactPos[3];
	uint32_t wreckedMs;
	int8_t skidMark[4];         // skidmarks slot per wheel, -1 = none
};

extern Car g_cars[MAX];

// vehphys.cpp
extern u32 g_colTicks;                  // collision query ticks (wheel lines + body spheres)
void physSetup(Car& c);                 // after the model is ready: lines, spheres, handling, ground
void physStep(Car& c);                  // one 60 Hz step
void physPair(Car& a, Car& b);          // vehicle vs vehicle
void physPlace(Car& c);                 // drop onto the ground / water / track
void publish(Car& c);                   // fixed -> Vehicle floats
void pull(Car& c);                      // Vehicle floats (pos / axes / moveSpeed) -> fixed
void hullDraw(const Car& c, float lean[2]);   // bike lean pivot (model z, angle)
// vehicle.cpp
void vehicleDamage(Car& c);
void controlsIdle(Car& c);

}
