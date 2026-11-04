#pragma once

#include <nds/ndstypes.h>
#include <stdint.h>

#include "game/vehiclemodel.h"

// CVehicle / CAutomobile (+ the CPhysical parts they use), reduced to what a
// handful of cars around the player needs. Physics runs in SA units like the
// original (speeds per 1/50 s step, CTimer::ms_fTimeStep = 50/60 per 60 Hz
// step) so the handling.cfg values behave the same:
//  - ProcessSuspension: one collision line per wheel (CAutomobile::
//    SetupSuspensionLines: wheel dummy + upper limit down to lower limit -
//    wheel radius), ApplySpringCollisionAlt + ApplySpringDampening,
//  - ProcessCarWheelPair / CVehicle::ProcessWheel: tyre forces from the
//    surface adhesion (surfinfo / surface.dat), drive (cTransmission::
//    CalculateDriveAcceleration), brakes, handbrake, steering,
//  - ApplyGravity / ApplyAirResistance / ApplyMoveSpeed / ApplyTurnSpeed,
//  - body vs map: a set of spheres around the body (col::sphereContact),
//    impulse + friction like ApplyCollisionAlt, car vs car sphere pairs,
//  - VehicleDamage: health loss above m_fMass / 1500 * 25, CDamageManager-ish
//    panel / door states (ok -> dam -> fall off), fire below 250 health,
//    BlowUpCar after the burn timer (explosion, wreck, debris).
// Cars far from the player (outside the collision near list) are frozen,
// resting cars go to sleep (m_nFakePhysics) until something hits them.
namespace veh {

constexpr int MAX = 8;   // budget::MAX_VEHICLES

enum Status : uint8_t { STATUS_FREE, STATUS_PHYSICS, STATUS_PLAYER, STATUS_WRECKED };
enum CompState : uint8_t { DMG_OK, DMG_DAMAGED, DMG_MISSING };
enum Door : uint8_t { DOOR_LF, DOOR_RF, DOOR_LR, DOOR_RR, NUM_DOORS };

struct Vehicle {
	uint8_t status;
	int8_t model;               // vehmdl handle
	int16_t vehIndex;           // vehdata record
	uint16_t modelId;
	// CMatrix: world = pos + x * right + y * fwd + z * up
	float pos[3], right[3], fwd[3], up[3];
	float moveSpeed[3], turnSpeed[3];   // per 1/50 s step
	float mass, turnMass, airRes, com[3];
	// wheels (eCarWheel order)
	float susp[4], suspPrev[4];         // m_fWheelsSuspensionCompression: 1 = no contact
	float lineStart[4][3], lineEnd[4][3];   // model space suspension lines
	float springLen, lineLen[4];
	float contact[4][3], contactN[4][3];    // world
	uint8_t contactSurf[4], wheelState[4];
	float wheelCounts[4], wheelRot[4], wheelSpin[4];
	float steer, rawSteer, gas, brake;
	bool handbrake, horn, engineOn, lights;
	uint8_t gear, numContact, driveOnGround;
	float inertia1, inertia2;
	float health;
	float burnTimer;            // ms on fire
	uint8_t colPrim, colSec;    // carcols indices
	uint8_t dmg[vehmdl::COMP_COUNT];
	float compDamage[vehmdl::COMP_COUNT];
	float door[NUM_DOORS];      // 0 shut .. 1 open
	uint16_t fakePhysics;       // resting steps
	bool asleep;
	float damageIntensity, impactPos[3], impactDir[3];
	uint32_t wreckedMs;
	float skid;                 // sum of wheel skid for the audio
	float speedFwd;             // DotProduct(m_vecMoveSpeed, GetForward())
	float wheelSpinAudio;
	// body spheres (model space) for the map / other cars
	uint8_t numSpheres;
	float sph[10][4];
	int audio;                  // aevehicle entity handle
};

void init();
// Spawns vehdata record `vehIndex` (a car) at an SA position / heading
// (radians, 0 = north, CCW like peds). The model streams in over a few
// frames; the car is placed on the ground once its collision is there.
// Returns the id or -1.
int spawn(int vehIndex, float x, float y, float z, float heading, int colour = -1);
void remove(int id);
void removeAll();
Vehicle* get(int id);
int count();

// One 60 Hz simulation step of every awake car (player::update calls it in
// its fixed-step loop, after the driver's inputs).
void step();
// Once per frame: model streaming, fire / explosion timers, audio entities.
void update();
// Inside renderer::draw with the view loaded (player-local GL space).
struct View { float originX, originY, originZ; float eyeX, eyeY, eyeZ, yaw, pitch; };
void draw(const View& v);

// Driver controls (CAutomobile::ProcessControlInputs): steer -1 (left) .. 1,
// accel / brake 0..1 (the pedal logic picks brake vs reverse).
void setControls(int id, float steerLR, float accel, float brake, bool handbrake, bool horn);
void setDriver(int id, bool player);   // STATUS_PLAYER / STATUS_PHYSICS (+ engine)
// World transforms of model-space points / vectors.
void toWorld(const Vehicle& v, const float l[3], float w[3]);
void toLocal(const Vehicle& v, const float w[3], float l[3]);
float heading(const Vehicle& v);       // of the forward axis, ped convention
// Entity matrix for drawing something attached (GX rows, player-local GL)
// at model-space point `l` (CJ in the driver seat).
void attachMatrix(const Vehicle& v, const float l[3], float ox, float oy, float oz, s32 out[12]);
// Nearest car whose driver door is within `range` of (x, y) and can be
// entered (not wrecked, upright). -1 if none.
int findEnterable(float x, float y, float z, float range);
// Door ratio (0 shut .. 1 open) driven by the enter/exit anims; plays the
// open / close sounds on the edges.
void setDoor(int id, int door, float ratio);
// Ped vs car (CJ on foot): pushes (x, y) out of the car bodies. True if touched.
bool pushPed(float& x, float& y, float z, float r);

// Debug actions
void repair(int id);
void blowUp(int id);
void flip(int id);
void setHealth(int id, float h);

struct Stats { uint16_t active, awake, drawn, polys; uint32_t stepUs, drawUs; };
const Stats& stats();

}
