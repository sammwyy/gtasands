#pragma once

#include <stdint.h>

// Random peds: CPopulation (AddToPopulation / ManagePed / RemovePed), CPopCycle
// (popcycle.dat densities per zone type and time, zone gang strengths), the
// ped streaming part of CStreaming (a few ped models resident, swapped for the
// zone's groups), CPed + the tasks a street ped runs, reduced to a state
// machine per ped:
//   CTaskComplexWanderStandard   follow the ped path graph (nodes*.dat) with a lane offset, pauses
//   CTaskComplexAvoidOtherPed    side-step peds / CJ in the way
//   CTaskComplexSmartFleeEntity  run away from a threat (gun shots, being hit, aimed at)
//   CTaskComplexKillPedOnFoot    melee (FightA_*) or shoot (gang weapons, cops) at CJ or a rival gang ped
//   CTaskSimpleHitHead/Front/..  HIT_* reactions, KO_* knock-downs + getup, death (KO_shot_* / KO_skid_*)
//   CTaskComplexEvasiveDive      jump out of the way of a car (EV_dive / EV_step)
//   CTaskSimpleDuck / HandsUp    DUCK_cower / handsup
// Relationships follow ped.dat (gangs hate other gangs) + the new-game script
// (Grove respects CJ, Ballas / Vagos hate him), cops go for CJ while he is wanted.
// Dead peds drop their money (walk over the body) and stay ~30 s.
// Pool of budget::MAX_PEDS; animation per ped (game/pedanim), meshes shared
// per model (game/pedmodel); poses are evaluated only for drawn peds, less
// often far away.
namespace peds {

constexpr int MAX = 12;   // mem::budget::MAX_PEDS

enum Task : uint8_t {
	TASK_WANDER, TASK_STAND, TASK_FOLLOW, TASK_FLEE, TASK_ATTACK, TASK_HIT, TASK_FALL, TASK_GETUP,
	TASK_DIVE, TASK_COWER, TASK_HANDSUP, TASK_DEAD, TASK_COUNT
};
const char* taskName(int t);

// Public view (debug UI / tests).
struct Info {
	bool used;
	uint8_t task, pedType;
	int16_t modelId;
	float x, y, z, heading, health;
	int8_t target;   // -2 CJ, -1 none, else ped
};
bool info(int i, Info& out);

// New game: empty pool, ped model streaming reset. Loading job: tables, the
// first models (synchronous) and CPopulation::GeneratePedsAtStartOfGame.
void init();
bool initStep();
// Once per in-game frame after player::update (dt60 = elapsed 1/60 s ticks).
void update(int dt60);
// Inside renderer::draw after the vehicles (view matrix loaded, player-local
// GL space with `origin` at the GL origin).
struct View { float originX, originY, originZ; float eyeX, eyeY, eyeZ, yaw, pitch; };
void draw(const View& v);
void vblank();            // ped model texture uploads

// --- interactions (the player / vehicles call these) ---
// CTaskSimpleFight::FightHitPed: the strike of melee.dat move `move`
// (melee::Move) from (x, y, z) facing `heading` reaches `range` m. Standing
// moves only find standing peds, MOVE_GROUND only peds on the floor. True if a
// ped took it.
bool meleeHit(float x, float y, float z, float heading, float range, int weaponType, int move);
// A ped lying on the floor in front (CTaskSimpleFight::FindTargetOnGround).
bool downedInFront(float x, float y, float z, float heading, float range);
// CWeapon::DoBulletImpact on peds: the first ped crossed by a -> b takes the
// shot; b is moved to the hit. True on a hit.
bool bulletHit(const float a[3], float b[3], int weaponType);
// CEventGunShot: peds around (x, y) hear CJ fire.
void gunShot(float x, float y, float z);
// CExplosion::AddExplosion on peds: thrown and hurt within radius.
void explosion(float x, float y, float z, float radius, float damage);
// Ped vs vehicle bodies, called by peds::update (veh::pushPed per ped).
// CJ on foot vs the peds: pushes (x, y) out of them. True if touched.
bool pushPlayer(float& x, float& y, float z, float r);
// CWanted::WorkOutPolicePresence: a live cop within r m.
bool copNear(float x, float y, float r);

// --- debug ---
int count();
int spawnNear(float x, float y, float z, int modelId = -1);   // a random / given model on the nearest path
void removeAll();
void debugPlace(int i, float x, float y, float z, float heading);   // headless tests
struct Stats {
	uint16_t live, drawn, posed, polys, spawned, removed, models, wanted;
	uint16_t maxPeds, civ, gang, cops, dealers;
	uint8_t zoneType, weekend, slot;
	uint32_t updateUs, drawUs;
	uint32_t poseUs, taskUs, physUs;   // parts of drawUs / updateUs
	uint32_t posePeakUs;               // slowest single pose evaluation so far
	uint32_t poseTotalUs, poseCount;   // all evaluations so far (average)
};
const Stats& stats();

}
