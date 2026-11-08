#pragma once

#include <nds/ndstypes.h>
#include <stdint.h>

#include "game/vehiclemodel.h"

// CVehicle and its subclasses (+ the CPhysical parts they use), reduced to
// what a handful of vehicles around the player needs. Physics runs in SA units
// like the original (speeds per 1/50 s step, CTimer::ms_fTimeStep = 50/60 per
// 60 Hz step) so the handling.cfg values behave the same, in fixed point
// (game/fx.h, Q8.24; game/vehphys.cpp):
//  - CAutomobile (car, mtruck, quad, kart, trailer; also the wheels / skids of
//    planes and helis): one collision line per wheel (SetupSuspensionLines),
//    ApplySpringCollisionAlt + ApplySpringDampening, ProcessCarWheelPair /
//    CVehicle::ProcessWheel tyre forces from the surface adhesion, cTransmission
//    drive, brakes, handbrake, burst tyres,
//  - CBike / CBmx: the same wheel code on two narrow line pairs, kept upright by
//    a roll stabiliser; the lean into turns is visual (body + rider rotate about
//    the contact line),
//  - CBoat: mod_Buoyancy-style hull samples against CWaterLevel, ProcessBoatControl
//    thrust / rudder / aquaplane, water resistance from the boat handling,
//  - CHeli / CPlane: CVehicle::FlyingControl (FLIGHTMODEL_HELI / PLANE) with the
//    flying handling, rotor spin-up, propellers and control surfaces,
//  - CTrain: kinematic along the nearest tracks.dat loop (no physics),
//  - ApplyGravity / ApplyAirResistance / ApplyMoveSpeed / ApplyTurnSpeed,
//  - body vs map: a set of spheres around the body (col::sphereContactCu),
//    impulse + friction like ApplyCollisionAlt, vehicle vs vehicle sphere pairs,
//  - VehicleDamage: health loss above m_fMass / 1500 * 25, CDamageManager-ish
//    panel / door states (ok -> dam -> fall off), fire below 250 health,
//    BlowUpCar after the burn timer (explosion, wreck, debris).
// Vehicles far from the player (outside the collision near list) are frozen,
// resting ones go to sleep (m_nFakePhysics) until something hits them.
namespace veh {

constexpr int MAX = 8;   // budget::MAX_VEHICLES

enum Status : uint8_t { STATUS_FREE, STATUS_PHYSICS, STATUS_PLAYER, STATUS_WRECKED };
enum CompState : uint8_t { DMG_OK, DMG_DAMAGED, DMG_MISSING };
enum Door : uint8_t { DOOR_LF, DOOR_RF, DOOR_LR, DOOR_RR, NUM_DOORS };
// CVehicle::m_nVehicleSubType collapsed to the physics paths
enum Class : uint8_t { CLS_AUTOMOBILE, CLS_BIKE, CLS_BOAT, CLS_HELI, CLS_PLANE, CLS_TRAIN };

// Public view, refreshed after every step (the physics state is fixed point).
struct Vehicle {
	uint8_t status;
	uint8_t type;               // ds::VehType
	uint8_t cls;                // Class
	int8_t model;               // vehmdl handle
	int16_t vehIndex;           // vehdata record
	uint16_t modelId;
	// CMatrix: world = pos + x * right + y * fwd + z * up
	float pos[3], right[3], fwd[3], up[3];
	float moveSpeed[3];         // per 1/50 s step
	float susp[4];              // m_fWheelsSuspensionCompression: 1 = no contact
	float steer, rawSteer, gas, brake;
	bool handbrake, horn, engineOn, lights;
	uint8_t gear, numContact;
	float health;
	float burnTimer;            // ms on fire
	uint8_t colPrim, colSec;    // carcols indices
	uint8_t dmg[vehmdl::COMP_COUNT];
	float compDamage[vehmdl::COMP_COUNT];
	float door[NUM_DOORS];      // 0 shut .. 1 open
	bool asleep;
	float skid;                 // sum of wheel skid for the audio
	float speedFwd;             // DotProduct(m_vecMoveSpeed, GetForward())
	float lean;                 // bikes: visual lean, radians (+ = right side down)
	float rotor;                // heli rotor / propeller speed, 0..1
	float pitchIn, rollIn, yawIn;   // control surfaces, -1..1
	uint8_t burst;              // bit per wheel (eCarWheel): tyre burst
	bool inWater;               // hull / body in water (boats float, the rest drown)
	int audio;                  // aevehicle entity handle
	bool npcDriver, playerOwned;
	uint8_t radioStation;
};

void init();
// Spawns vehdata record `vehIndex` at an SA position / heading (radians,
// 0 = north, CCW like peds). The model streams in over a few frames; the
// vehicle is placed on the ground (boats: the water, trains: the nearest
// track) once its collision is there. Returns the id or -1.
int spawn(int vehIndex, float x, float y, float z, float heading, int colour = -1,
          const char* cutsceneModel = nullptr);
int spawnInFront(int vehIndex);   // debug / cheat spawn at a clear spot in front of CJ
bool prepareCutscene(int id);
void positionCutscene(int id, float x, float y, float z, float heading);
void remove(int id);
void removeAll();
Vehicle* get(int id);
int count();

// One 60 Hz simulation step of every awake vehicle (player::update calls it
// in its fixed-step loop, after the driver's inputs).
void step();
// Once per frame: model streaming, fire / explosion timers, audio entities.
void update();
// Inside renderer::draw with the view loaded (player-local GL space).
struct View { float originX, originY, originZ; float eyeX, eyeY, eyeZ, yaw, pitch; };
void draw(const View& v);

// Driver controls (CAutomobile / CBike / CBoat::ProcessControlInputs, the
// flying ones for CPlane / CHeli): steer -1 (left) .. 1, accel / brake 0..1
// (the pedal logic picks brake vs reverse), pitch -1 (nose down) .. 1, yaw
// -1 (left) .. 1 (rudder / tail rotor: L / R).
struct Controls {
	float steer, accel, brake, pitch, yaw;
	bool handbrake, horn;
};
void setControls(int id, const Controls& c);
void setDriver(int id, bool player);   // STATUS_PLAYER / STATUS_PHYSICS (+ engine)
// World transforms of model-space points / vectors.
void toWorld(const Vehicle& v, const float l[3], float w[3]);
void toLocal(const Vehicle& v, const float w[3], float l[3]);
float heading(const Vehicle& v);       // of the forward axis, ped convention
// Entity matrix for drawing something attached (GX rows, player-local GL)
// at model-space point `l` (CJ in the driver seat; bikes add the lean).
void attachMatrix(const Vehicle& v, const float l[3], float ox, float oy, float oz, s32 out[12]);
// Nearest vehicle whose driver door is within `range` of (x, y) and can be
// entered (not wrecked, upright, not a trailer). -1 if none.
int findEnterable(float x, float y, float z, float range);
// Door ratio (0 shut .. 1 open) driven by the enter/exit anims; plays the
// open / close sounds on the edges.
void setDoor(int id, int door, float ratio);
// Ped vs vehicle: pushes (x, y) out of the bodies (vehicle `skip` excluded:
// the car CJ is walking to / sitting in). `hit` (optional) gets the fastest
// moving vehicle touched (CPed::KillPedWithCar input; id -1 = none), speed
// per 1/50 s step. True if touched.
struct PedHit { int id; float vx, vy, speed; };
bool pushPed(float& x, float& y, float z, float r, int skip = -1, PedHit* hit = nullptr);
// AE_PED_HIT from vehicle `id` (the bump of a ped going under / over it)
void pedHitSound(int id, float x, float y, float z);
// CWeapon::DoBulletImpact on vehicles: segment a -> b (SA world). A tyre in
// the way bursts (CAutomobile::BurstTyre), the body takes `damage`. True on a hit.
bool bulletHit(const float a[3], const float b[3], float damage);
// Where vehdata record `vehIndex` should appear when wanted near (x, y):
// boats on the nearest water, trains on the nearest track node; false = here.
bool spawnPoint(int vehIndex, float x, float y, float& ox, float& oy, float& oz);
// RC vehicles (CVehicle::IsRCVehicle model ids): driven from outside.
bool isRemote(int modelId);

// Debug actions
void repair(int id);
void blowUp(int id);
void flip(int id);
void setHealth(int id, float h);
void burstTyre(int id, int wheel);   // -1 = all

struct Stats {
	uint16_t active, awake, drawn, polys;
	uint32_t stepUs, drawUs, queryUs;
	uint16_t skids, lights, traffic, spawned, removed, trafficCandidates;
	uint32_t trafficAttempts, trafficSpots, trafficModelMiss, trafficSpawnFail;
};
const Stats& stats();

}
