#include "game/player.h"

#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/aepedaudio.h"
#include "audio/aeweaponaudio.h"
#include "game/cj.h"
#include "game/collision.h"
#include "game/debugopts.h"
#include "game/melee.h"
#include "game/menu/menu.h"
#include "game/pad.h"
#include "game/pedmove.h"
#include "game/peds.h"
#include "game/timecycle.h"
#include "game/vehicle.h"
#include "game/vehicledata.h"
#include "game/vehiclemodel.h"
#include "game/waterlevel.h"
#include "game/zones.h"
#include "hud/hud.h"
#include "platform/input.h"
#include "platform/timing.h"

// New-game snapshot (CPlayerInfo::Clear, CStats::Init, CClock::Initialise) plus
// the on-foot controls in PC keyboard mode (button map in game/pad.h):
//   D-pad   WASD: full deflection -> CJ jogs; L held = walk (LALT)
//   B       sprint. Taps raise m_fMoveSpeed the way ControlButtonSprint does,
//           so tapping outruns a held sprint. Energy is m_fTimeCanRun.
//   A       punch (fists / melee) or fire (a gun, once one is given)
//   X       jump (JUMP_launch / glide / land). Not while aiming.
//   Y       enter/exit edge for the vehicle port. Nothing is spawned yet.
//   L tap   next occupied weapon slot (ProcessWeaponSwitch, one direction)
//   R       aim. Fists go to FIGHTIDLE; a gun free-aims along the camera.
//   touch   mouse look: orbits the chase cam.
// Locomotion (move blend, turn, clip blending) lives in game/pedmove.
namespace player {

State g;
unsigned g_clockH, g_clockM, g_clockDay, g_clockMonth;
int g_weekday;

// Grove Street, the on-foot restart the new game leaves CJ at.
static constexpr float SPAWN_X = 2495.0f, SPAWN_Y = -1664.0f, SPAWN_Z = 13.5f;

// CClock::daysInMonth, February included as 29 the way the original table is.
static const uint8_t k_daysInMonth[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

// weapon.dat, STD skill: slot, clip, and whether the trigger repeats.
// txd/icon are the gta3.dspk names the HUD already knows how to draw.
struct WepDef {
	int type;
	int slot;
	int clip;
	bool melee;
	bool automatic;
	const char* txd;
	const char* icon;
};

static const WepDef k_weps[] = {
	{ WEAPON_UNARMED,         0,   1, true,  false, nullptr,            nullptr },
	{ WEAPON_BRASSKNUCKLE,    0,   1, true,  false, "brassknuckle.txd", "brassknuckleicon" },
	{ WEAPON_GOLFCLUB,        1,   1, true,  false, "golfclub.txd",     "golfclubicon" },
	{ WEAPON_NIGHTSTICK,      1,   1, true,  false, "nitestick.txd",    "nitestickicon" },
	{ WEAPON_KNIFE,           1,   1, true,  false, "knifecur.txd",     "knifecuricon" },
	{ WEAPON_BASEBALLBAT,     1,   1, true,  false, "bat.txd",          "baticon" },
	{ WEAPON_SHOVEL,          1,   1, true,  false, "shovel.txd",       "shovelicon" },
	{ WEAPON_POOLCUE,         1,   1, true,  false, "poolcue.txd",      "poolcueicon" },
	{ WEAPON_KATANA,          1,   1, true,  false, "katana.txd",       "katanaicon" },
	{ WEAPON_CHAINSAW,        1,   1, true,  false, "chnsaw.txd",       "chnsawicon" },
	{ WEAPON_DILDO1,         10,   1, true,  false, "gun_dildo1.txd",   "gun_dildo1icon" },
	{ WEAPON_DILDO2,         10,   1, true,  false, "gun_dildo2.txd",   "gun_dildo2icon" },
	{ WEAPON_VIBE1,          10,   1, true,  false, "gun_vibe1.txd",    "gun_vibe1icon" },
	{ WEAPON_VIBE2,          10,   1, true,  false, "gun_vibe2.txd",    "gun_vibe2icon" },
	{ WEAPON_FLOWERS,        10,   1, true,  false, "flowera.txd",      "floweraicon" },
	{ WEAPON_CANE,           10,   1, true,  false, "gun_cane.txd",     "gun_caneicon" },
	{ WEAPON_GRENADE,         8,   1, false, false, "grenade.txd",      "grenadeicon" },
	{ WEAPON_TEARGAS,         8,   1, false, false, "teargas.txd",      "teargasicon" },
	{ WEAPON_MOLOTOV,         8,   1, false, false, "molotov.txd",      "molotovicon" },
	{ WEAPON_PISTOL,          2,  17, false, false, "colt45.txd",       "colt45icon" },
	{ WEAPON_PISTOL_SILENCED, 2,  17, false, false, "silenced.txd",     "silencedicon" },
	{ WEAPON_DESERT_EAGLE,    2,   7, false, false, "desert_eagle.txd", "desert_eagleicon" },
	{ WEAPON_SHOTGUN,         3,   1, false, false, "chromegun.txd",    "chromegunicon" },
	{ WEAPON_SAWNOFF,         3,   2, false, false, "sawnoff.txd",      "sawnofficon" },
	{ WEAPON_SPAS12,          3,   7, false, false, "shotgspa.txd",     "shotgspaicon" },
	{ WEAPON_MICRO_UZI,       4,  50, false, true,  "micro_uzi.txd",    "micro_uziicon" },
	{ WEAPON_MP5,             4,  30, false, true,  "mp5lng.txd",       "mp5lngicon" },
	{ WEAPON_AK47,            5,  30, false, true,  "ak47.txd",         "ak47icon" },
	{ WEAPON_M4,              5,  50, false, true,  "m4.txd",           "m4icon" },
	{ WEAPON_TEC9,            4,  50, false, true,  "tec9.txd",         "tec9icon" },
	{ WEAPON_COUNTRYRIFLE,    6,   1, false, false, "cuntgun.txd",      "cuntgunicon" },
	{ WEAPON_SNIPERRIFLE,     6,   1, false, false, "sniper.txd",       "snipericon" },
	{ WEAPON_RLAUNCHER,       7,   1, false, false, "rocketla.txd",     "rocketlaicon" },
	{ WEAPON_RLAUNCHER_HS,    7,   1, false, false, "rocketla.txd",     "rocketlaicon" },
	{ WEAPON_FLAMETHROWER,    7, 500, false, true,  "flame.txd",        "flameicon" },
	{ WEAPON_MINIGUN,         7, 500, false, true,  "minigun.txd",      "minigunicon" },
	{ WEAPON_SATCHEL,         8,   1, false, false, "satchel.txd",      "satchelicon" },
	{ WEAPON_DETONATOR,      12,   1, false, false, "bomb.txd",         "bombicon" },
	{ WEAPON_SPRAYCAN,        9, 500, false, true,  "spraycan.txd",     "spraycanicon" },
	{ WEAPON_EXTINGUISHER,    9, 500, false, true,  "fire_ex.txd",      "fire_exicon" },
	{ WEAPON_CAMERA,          9,  36, false, false, "camera.txd",       "cameraicon" },
	{ WEAPON_PARACHUTE,      11,   1, true,  false, "gun_para.txd",     "gun_paraicon" },
};

static constexpr int NUM_SLOTS = 13; // eWeaponSlot, unarmed .. detonator

struct Slot {
	int type;
	int total;
	int clip;
	int clipSize;
	bool melee;
	bool automatic;
	const char* txd;
	const char* icon;
};

static Slot s_slots[NUM_SLOTS];
static int s_slot;

// CPlayerPedData::m_fTimeCanRun. GetFatAndMuscleModifier(STAT_MOD_TIME_CAN_RUN)
// is still an unreversed call. A new game has STAT_STAMINA 100 (CStats::Init,
// not 0). The bar used here is 5s at stamina 0 plus 0.9 per point, drained at
// one CTimer step per frame (timestep 1 at 30 fps). 100 stamina lasts ~8s.
static float s_stamina = 100.0f;
static float s_timeCanRun;
static bool s_spent;             // hit empty -> no sprint until 40% is back
static float s_moveSpeed;        // ControlButtonSprint's m_fMoveSpeed, 0..10
static uint32_t s_sprintMs;      // CStats::m_SprintStaminaCounter
static uint32_t s_runMs;         // CStats::m_RunningCounter

// PLAYER_SPRINT_SET[SPRINT_GROUND] at 0x8D2460.
static constexpr float SP_TAP = 4.0f;        // added on SprintJustDown
static constexpr float SP_HOLD = 0.7f;       // climb toward the threshold
static constexpr float SP_DECAY = 0.2f;      // per timestep, always
static constexpr float SP_THRESHOLD = 5.0f;  // PLAYER_SPRINT_THRESHOLD
static constexpr float SP_CAP = 10.0f;
static constexpr float SP_EXTRA = 0.3f;      // GetButtonSprintResults field_1C

static float s_camYaw;
static float s_camPitch;
static float s_betaSpeed;
static int s_betaSign;
static int s_zoneTimer;
static int s_teleportWait;
static bool s_camDrag;
static int s_lastTx, s_lastTy;

static uint32_t s_lastVbl;
static uint32_t s_clockAcc;      // vblanks toward the next game minute
static int s_hoursNoEat;         // CPlayerInfo::m_nNumHoursDidntEat
static bool s_hungryTold;

static pedmove::State s_move;    // CJ's locomotion (move blend, phases, clip weights)
static const pedmove::PoseApi k_cjPose = { cj::pose, cj::clipPhase, cj::clipEnded };

// ACT_BEHIT: CTaskSimpleBeHit, ACT_DOWN / ACT_GETUP: CTaskComplexFallAndGetUp,
// ACT_DEAD: CTaskComplexDie (held until the WASTED respawn)
enum Action { ACT_NONE, ACT_LAUNCH, ACT_GLIDE, ACT_LAND, ACT_PUNCH, ACT_BEHIT, ACT_DOWN, ACT_GETUP, ACT_DEAD };
static Action s_action;
static int s_combo;
static float s_jumpVX, s_jumpVY, s_jumpVZ;
static int s_reload;             // frames left before the clip refills
static int s_fireCd;
static bool s_queued;            // CTaskSimpleFight::m_nNextCommand: the next move of the combo is asked for
static const char* s_hitClip;    // the be-hit / knock-down / death clip
static const char* s_getupClip;
static int s_downSteps;          // lying on the floor before the getup

static constexpr float CAM_TIMESTEP = 0.5f; // CTimer is 1 at 30 fps; we step at 60
static constexpr float DT = 1.0f / 60.0f;
static constexpr uint32_t MAX_STEPS = 4;    // simulation catch-up cap per frame
// CTaskSimpleJump / CTaskSimpleInAir / CTaskComplexInAirAndLand, converted
// from SA's per-step units (1 step = 1/50 s) to world units per second:
//  - the ped stays on the ground for the whole JUMP_launch(_R) clip (its
//    root motion carries the take-off step), and only when it ends Launch()
//    applies the jump: ApplyMoveForce(0,0,8.5) on a 70 kg ped = 0.121/step
//  - horizontal speed 0.1/step standing or walking, 0.1..0.17 by run blend,
//    0.17..0.22 by sprint blend, along the facing
//  - while rising the forward speed is held at >= 0.05/step
//  - gravity GAME_GRAVITY 0.008/step^2; peds keep a little air drag
//  - JUMP_land only when landing at move blend > 1.5 with the stick held,
//    FALL_land otherwise; x2 speed when landing a held sprint
static constexpr float JUMP_UP_SPEED = 8.5f / 70.0f * 50.0f;     // 6.07 u/s
static constexpr float JUMP_GRAVITY = 0.008f * 50.0f * 50.0f;    // 20 u/s^2
static constexpr float JUMP_H_STAND = 0.10f * 50.0f;             // 5 u/s
static constexpr float JUMP_H_RUN = 0.17f * 50.0f;               // 8.5 u/s
static constexpr float JUMP_H_SPRINT = 0.22f * 50.0f;            // 11 u/s
static constexpr float JUMP_MIN_RISE_FWD = 0.05f * 50.0f;        // 2.5 u/s
static constexpr float JUMP_AIR_DRAG = 0.99f;                    // per 60 Hz step
static const char* s_launchClip = "JUMP_launch";
static const char* s_landClip = "JUMP_land";
static const char* s_glideClip = "JUMP_glide";   // FALL_fall when walking off a ledge
static float s_jumpH;            // horizontal speed Launch() will apply
static int s_landSpeed = 1;      // ticks per step for the land clip
static float s_stepVX, s_stepVY; // last on-foot step velocity (u/s), carried into a fall

// Collision (game/collision): CPed vs the map, reduced. The three ped spheres
// push CJ out of walls every step; the feet follow the highest floor found
// by a vertical probe from STEP_UP above them (curbs and stairs are walked
// up), drops deeper than SNAP_DOWN start a fall (FALL_fall + glide physics).
static constexpr float STEP_UP = 0.5f;
static constexpr float SNAP_DOWN = 0.4f;
static constexpr float FALL_PROBE = 80.0f;     // a floor this far below makes walking off a ledge a fall
static constexpr float COLLAPSE_VZ = -12.0f;   // landing faster than this: FALL_collapse
static constexpr float CAM_MARGIN = 0.3f;      // camera kept this far in front of a wall
static uint8_t s_surface;        // m_nContactSurface: COL surface of the last floor under the feet
static float s_footPhase = -1.0f; // lead cycle phase at the last step (CPed::PlayFootSteps)
static bool s_swung;             // AE_PED_SWING raised for the current punch
static bool s_struck;            // CTaskSimpleFight::FightHitPed done for the current punch
static uint32_t s_chaos;         // CWanted::m_ChaosLevel
static uint32_t s_wantedMs;      // vblanks toward the next chaos decrease
static bool s_saidFall;          // CTX_GLOBAL_PAIN_CJ_HIGH_FALL said during this fall
static float s_viewZ = SPAWN_Z;  // smoothed feet z: render / camera reference
static float s_camDist = 3.6f;   // chase distance after the camera collision
// vehicle tasks (see the Vehicles section below)
enum CarTask : uint8_t { CT_NONE, CT_GOTO, CT_OPEN, CT_GETIN, CT_CLOSE, CT_DRIVE, CT_GETOUT, CT_CLOSEOUT };
static CarTask s_carTask;
static int s_car = -1;
static int s_taskSteps;
static bool s_wantExit, s_low;
static float s_wastedT;
static pad::Pad s_goPad;
static float s_camCarDist = 6.0f, s_lookH = CAM_LOOK_H;
static void endCarTask(bool onFoot);

static float wrap(float a) {
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}

float camYaw() { return s_camYaw; }
float camPitch() { return s_camPitch; }
float viewZ() { return s_viewZ; }
float camDist() { return s_camDist; }

static bool physics() { return dbg::opts.physics && col::available(); }

// Highest floor between top and bottom under CJ (flat SPAWN_Z without
// collision). Also feeds the ped light from the face's tColLighting.
static bool floorAt(float top, float bottom, float& z) {
	if (!physics()) {
		z = SPAWN_Z;
		return z <= top && z >= bottom;
	}
	col::Hit h;
	if (!col::groundAt(g.x, g.y, top, bottom, h)) return false;
	z = h.z;
	s_surface = h.surface;
	timecycle::setSurfaceLight(h.light);
	return true;
}

void placeOnGround() {
	float z;
	if (floorAt(g.z + 2.0f, g.z - 20.0f, z) || floorAt(g.z + 60.0f, g.z - 60.0f, z)) g.z = z;
	s_viewZ = g.z;
}
#ifdef GTASADS_DEBUG
void debugSetView(float yaw, float pitch) {
	g.heading = s_camYaw = wrap(yaw);
	s_camPitch = pitch;
}
#endif

// Sound position: the ped origin (CPed::GetPosition is ~1 m above the feet)
static aeped::Pos pedPos() { return { g.x, g.y, g.z + 1.0f }; }

static const WepDef* defFor(int type) {
	for (unsigned i = 0; i < sizeof k_weps / sizeof k_weps[0]; i++)
		if (k_weps[i].type == type) return &k_weps[i];
	return nullptr;
}

static float energyMax() { return 150.0f + s_stamina * 0.9f; }

static void syncWeapon() {
	const Slot& s = s_slots[s_slot];
	g.weaponType = s.type;
	g.weapon.txd = s.txd;
	g.weapon.icon = s.icon;
	// total < 0: melee, the HUD draws the icon and skips the ammo line.
	if (s.melee || s.type == WEAPON_UNARMED) {
		g.weapon.clip = 0;
		g.weapon.total = -1;
	} else {
		g.weapon.clip = s.clip;
		g.weapon.total = s.total;
	}
}

static void equip(int slot) {
	if (slot < 0 || slot >= NUM_SLOTS) return;
	s_slot = slot;
	s_reload = 0;
	s_fireCd = 0;
	syncWeapon();
	aeweapon::prepare(s_slots[slot].type);   // the gun's sounds stream in before the first shot
}

void giveWeapon(int type, int ammo) {
	const WepDef* d = defFor(type);
	if (!d || type == WEAPON_UNARMED) return;
	if (ammo < 0) ammo = 0;
	Slot& s = s_slots[d->slot];
	if (s.type == type && !d->melee) {
		s.total += ammo;
	} else {
		s.type = type;
		s.total = d->melee ? 1 : ammo;
		s.clipSize = d->clip > 0 ? d->clip : 1;
		s.clip = d->melee ? 1 : (ammo < s.clipSize ? ammo : s.clipSize);
		s.melee = d->melee;
		s.automatic = d->automatic;
		s.txd = d->txd;
		s.icon = d->icon;
	}
	// CPed::GiveWeapon selects what it just gave.
	equip(d->slot);
}

void clearWeapons() {
	memset(s_slots, 0, sizeof s_slots);
	s_slots[0].type = WEAPON_UNARMED;
	s_slots[0].melee = true;
	equip(0);
}

void giveAllWeapons() {
	static const int k_set[] = { WEAPON_BASEBALLBAT, WEAPON_DESERT_EAGLE, WEAPON_SPAS12, WEAPON_MP5,
	                             WEAPON_M4, WEAPON_SNIPERRIFLE, WEAPON_RLAUNCHER, WEAPON_GRENADE,
	                             WEAPON_SPRAYCAN, WEAPON_CAMERA, WEAPON_DETONATOR, WEAPON_PARACHUTE };
	for (int t : k_set) giveWeapon(t, 500);
}

int weaponTypeAt(int index) {
	return (unsigned)index < sizeof k_weps / sizeof k_weps[0] ? k_weps[index].type : -1;
}

void setBodyStats(float fat, float muscle) {
	g.fat = fat < 0.0f ? 0.0f : fat > 1000.0f ? 1000.0f : fat;
	g.muscle = muscle < 0.0f ? 0.0f : muscle > 1000.0f ? 1000.0f : muscle;
	cj::setStats(g.fat, g.muscle);
}

void refillSprint() {
	s_spent = false;
	s_timeCanRun = energyMax();
	g.sprint = 100.0f;
}

void respawn() {
	if (s_carTask != CT_NONE) endCarTask(false);
	s_teleportWait = 0;
	g.x = SPAWN_X; g.y = SPAWN_Y; g.z = SPAWN_Z;
	g.heading = 0.0f;
	s_camYaw = 0.0f;
	s_action = ACT_NONE;
	s_jumpVX = s_jumpVY = s_jumpVZ = 0.0f;
	pedmove::reset(s_move);
	placeOnGround();
}

static void poseOne(const char* name, bool loop, bool extract, bool restart);

void teleport(float x, float y, float z, float heading) {
	if (s_carTask != CT_NONE) endCarTask(false);
	g.x = x; g.y = y; g.z = z;
	g.heading = s_camYaw = wrap(heading);
	s_action = ACT_NONE;
	s_jumpVX = s_jumpVY = s_jumpVZ = 0.0f;
	s_stepVX = s_stepVY = 0.0f;
	g.speed = 0.0f;
	s_viewZ = z;
	s_lookH = CAM_LOOK_H;
	s_zoneTimer = 0;
	s_camDrag = false;
	pedmove::reset(s_move);
	poseOne(cj::motion().idle, true, false, true);
	s_teleportWait = 300;
}

void locoDebug(char* buf, int n) {
	static const char* const k_state[] = { "none", "walk", "run", "sprint" };
	snprintf(buf, n, "mbr %.2f %s %s%s", s_move.moveBlend, k_state[s_move.moveState & 3],
	         pedmove::phaseName(s_move), pad::get().pedWalk ? " WALK" : "");
}

static void nextWeapon() {
	for (int i = 1; i <= NUM_SLOTS; i++) {
		const int sl = (s_slot + i) % NUM_SLOTS;
		if (sl == 0 || s_slots[sl].type != WEAPON_UNARMED) {
			if (sl != s_slot) equip(sl);
			return;
		}
	}
}

static void refillClip() {
	Slot& s = s_slots[s_slot];
	if (s.melee || s.clipSize <= 0) return;
	int n = s.total < s.clipSize ? s.total : s.clipSize;
	s.clip = n;
}

// One shot. Returns false when the gun is empty or mid-reload.
static bool fireOne() {
	Slot& s = s_slots[s_slot];
	if (s.melee) return false;
	if (s_reload > 0) return false;
	if (s.clip <= 0 || s.total <= 0) return false;
	s.clip--;
	s.total--;
	if (s.clip == 0 && s.total > 0) {
		s_reload = 54; // ~0.9s, then the clip fills
		const aeped::Pos p = pedPos();
		aeweapon::reload(s.type, false, p.x, p.y, p.z);
	}
	syncWeapon();
	return true;
}

// GetButtonSprintResults: 0 at rest, 1 up to the threshold, up to 1.3 at the cap.
static float sprintResults() {
	if (s_moveSpeed <= 0.0f) return 0.0f;
	if (s_moveSpeed <= SP_THRESHOLD) return 1.0f;
	float progress = s_moveSpeed / SP_THRESHOLD - 1.0f;
	if (progress < 0.0f) progress = 0.0f;
	return SP_EXTRA * progress + 1.0f;
}

// ControlButtonSprint(SPRINT_GROUND), minus the unreversed body. A press adds
// SP_TAP (the "run faster" tap). Holding climbs to the threshold and the
// decay keeps it there. Another tap while held pushes past it, up to the cap,
// and that extra bleeds off — so tapping outruns holding.
static float controlSprint(bool held, bool justDown) {
	const float ts = CAM_TIMESTEP;
	if (justDown) {
		s_moveSpeed += SP_TAP;
		if (s_moveSpeed > SP_CAP) s_moveSpeed = SP_CAP;
	}
	if (held && s_moveSpeed < SP_THRESHOLD) {
		s_moveSpeed += SP_HOLD * ts;
		if (s_moveSpeed > SP_THRESHOLD) s_moveSpeed = SP_THRESHOLD;
	}
	s_moveSpeed -= SP_DECAY * ts;
	if (s_moveSpeed < 0.0f) s_moveSpeed = 0.0f;

	if (s_spent || s_timeCanRun <= 0.0f) {
		if (held) s_moveSpeed = 0.0f;
		return 0.0f;
	}
	if (!(held && s_moveSpeed > 0.0f)) return sprintResults();

	// HandleSprintEnergy(true, 1): one timestep per call, floor at 0.
	s_timeCanRun -= ts;
	if (s_timeCanRun <= 0.0f) {
		s_timeCanRun = 0.0f;
		s_spent = true;
		s_moveSpeed = 0.0f;
		return 0.0f;
	}
	return sprintResults();
}

static void recoverEnergy(bool running, bool sprinting) {
	if (sprinting) return;
	const float maxE = energyMax();
	if (s_timeCanRun >= maxE) {
		s_timeCanRun = maxE;
		s_spent = false;
		return;
	}
	// Not sprinting: HandleSprintEnergy adds timestep * rate / 2 (rate 1).
	// A run refills slower, CPlayerPed::ProcessControl's 0.15 branch.
	s_timeCanRun += running ? CAM_TIMESTEP * 0.15f : CAM_TIMESTEP * 0.5f;
	if (s_timeCanRun > maxE) s_timeCanRun = maxE;
	if (s_spent && s_timeCanRun > maxE * 0.4f) s_spent = false;
}

// CStats::UpdateStatsWhenSprinting / WhenRunning. The reaction table is
// ar_stats.dat: +10 stamina after 300s of sprint, +5 after 300s of running.
static void exercise(bool sprinting, bool running, uint32_t ms) {
	if (sprinting) {
		if (300.0f * 1000.0f >= (float)s_sprintMs) s_sprintMs += ms;
		else {
			s_sprintMs = 0;
			s_stamina += 10.0f;
			if (s_stamina > 1000.0f) s_stamina = 1000.0f;
		}
	} else if (running) {
		if (300.0f * 1000.0f >= (float)s_runMs) s_runMs += ms;
		else {
			s_runMs = 0;
			s_stamina += 5.0f;
			if (s_stamina > 1000.0f) s_stamina = 1000.0f;
		}
	}
}

unsigned clockSeconds() { return s_clockAcc; }   // 60 vblanks per game minute -> 1 vblank = 1 game second
float minutesToday() { return (float)(g_clockH * 60 + g_clockM) + (float)s_clockAcc / 60.0f; }
void setClock(unsigned h, unsigned m) {
	g_clockH = h % 24;
	g_clockM = m % 60;
	s_clockAcc = 0;
}

// CClock::Update. dbg::opts.clockRate scales it (0 = gbFreezeTime).
static void tickClock(uint32_t vblanks) {
	s_clockAcc += vblanks * (uint32_t)dbg::opts.clockRate;
	while (s_clockAcc >= 60) { // 60 vblanks = 1 real second = 1 game minute
		s_clockAcc -= 60;
		g_clockM++;
		if (g_clockM < 60) continue;
		g_clockM = 0;
		g_clockH++;
		if (g_clockH >= 24) {
			g_clockH = 0;
			g_clockDay++;
			g_weekday = g_weekday >= 7 ? 1 : g_weekday + 1;
			if (g_clockDay > k_daysInMonth[g_clockMonth - 1]) {
				g_clockDay = 1;
				g_clockMonth = g_clockMonth >= 12 ? 1 : g_clockMonth + 1;
			}
		}
		// CPlayerInfo::WorkOutEnergyFromHunger. Fat and muscle are 0 on a
		// new game, so once 48 hours have passed each new hour takes 2 health.
		s_hoursNoEat++;
		if (s_hoursNoEat <= 48) {
			s_hungryTold = false;
		} else if (!s_hungryTold) {
			s_hungryTold = true;
			hud::log("CJ hasn't eaten in %d hours", s_hoursNoEat);
		} else {
			g.health -= 2.0f;
			if (g.health < 0.0f) g.health = 0.0f;
		}
	}
}

// CCam::Process_FollowPed: the camera swings round only behind a ped that
// walks away from it (or sideways). Stick back = CJ turns to face the camera
// and walks straight at it; the camera keeps its heading and just backs off
// (it is on a string around him), it never spins 180 degrees.
// Decided from the stick, not from CJ's heading: the stick is camera-relative,
// so a camera that turns while CJ is still turning drags the target heading
// with it and he walks in circles. With no stick (stopping) the heading rule
// lets the camera settle behind him unless he ended up facing it.
static bool camFollows(int walkUD, bool hasInput) {
	if (hasInput) return walkUD >= 0;
	return fabsf(wrap(g.heading - s_camYaw)) < 1.9f;
}

static void followCam(float targetYaw) {
	float diff = wrap(targetYaw - s_camYaw);
	const float absDiff = fabsf(diff);
	if (absDiff > 2.6f) {
		if (s_betaSign == 0)
			s_betaSign = (s_betaSpeed < 0.0f || (s_betaSpeed == 0.0f && diff < 0.0f)) ? -1 : 1;
		diff = (float)s_betaSign * absDiff;
	} else if (absDiff < 0.8f) {
		s_betaSign = 0;
	}
	float desired = diff * 0.50f;
	if (desired > 0.35f) desired = 0.35f;
	if (desired < -0.35f) desired = -0.35f;
	const float gap = desired - s_betaSpeed;
	float step = fabsf(gap) * 0.25f * CAM_TIMESTEP;
	if (step > fabsf(gap)) step = fabsf(gap);
	s_betaSpeed += gap > 0.0f ? step : -step;
	s_camYaw = wrap(s_camYaw + fminf(CAM_TIMESTEP, 10.0f) * s_betaSpeed);
}

static void poseOne(const char* name, bool loop, bool extract, bool restart) {
	const cj::Clip c = { name, 1.0f, loop, extract, false, restart, 0.0f };
	cj::pose(&c, 1);
}

static void updateAction(bool jumpDown, bool attackDown, bool aiming, bool melee,
                         bool moveHeld, bool sprintHeld) {
	if (s_reload > 0) {
		if (--s_reload == 0) {
			refillClip();
			const aeped::Pos p = pedPos();
			aeweapon::reload(s_slots[s_slot].type, true, p.x, p.y, p.z);
		}
	}
	if (s_fireCd > 0) s_fireCd--;

	if (s_action == ACT_PUNCH) {
		// CTaskSimpleFight with melee.dat's UNARMED_1: the strike lands at the
		// move's hit frame, a press during the move queues the next one, which
		// starts at the chain frame (FightA_1 -> 2 -> 3; FightA_G alone)
		const melee::Info& mv = melee::k_unarmed[s_combo];
		const int t = cj::clipTime(mv.clip);
		if (!s_swung && t >= melee::hitTicks(s_combo) - 4) {
			s_swung = true;
			aeped::swing(s_slots[s_slot].type, pedPos());
			aeped::sayPain(aeped::PAIN_CJ_BOXING, pedPos());
		}
		if (!s_struck && t >= melee::hitTicks(s_combo)) {
			s_struck = true;
			peds::meleeHit(g.x, g.y, g.z, g.heading, melee::REACH, s_slots[s_slot].type, s_combo);
		}
		if (attackDown && melee && s_combo < melee::MOVE_HIT_3) s_queued = true;
		const bool ended = t < 0 || cj::clipEnded(mv.clip);
		if (s_queued && s_struck && (t >= melee::chainTicks(s_combo) || ended)) {
			s_combo++;
			s_queued = s_swung = s_struck = false;
			// Fight clips carry the authored lunge on the root (FIGHT_* are
			// CAN_EXTRACT_VELOCITY). Extracting it moves the ped for real.
			poseOne(melee::k_unarmed[s_combo].clip, false, true, true);
		} else if (ended) {
			s_action = ACT_NONE;
		} else {
			poseOne(mv.clip, false, true, false);
		}
		return;
	}
	if (s_action == ACT_BEHIT) {
		if (cj::clipEnded(s_hitClip) || cj::clipPhase(s_hitClip) < 0.0f) {
			s_action = ACT_NONE;
			pedmove::resume(s_move, false);
		} else {
			poseOne(s_hitClip, false, false, false);
		}
		return;
	}
	if (s_action == ACT_DOWN) {
		// on the floor until the knock-down (or floor hit) clip ends + ~1 s
		poseOne(s_hitClip, false, false, false);
		if ((cj::clipEnded(s_hitClip) || cj::clipPhase(s_hitClip) < 0.0f) && ++s_downSteps > 60) {
			s_action = ACT_GETUP;
			poseOne(s_getupClip, false, false, true);
		}
		return;
	}
	if (s_action == ACT_GETUP) {
		if (cj::clipEnded(s_getupClip) || cj::clipPhase(s_getupClip) < 0.0f) {
			s_action = ACT_NONE;
			pedmove::resume(s_move, false);
		} else {
			poseOne(s_getupClip, false, false, false);
		}
		return;
	}
	if (s_action == ACT_DEAD) {
		poseOne(s_hitClip, false, false, false);
		return;
	}
	if (s_action == ACT_LAUNCH) {
		// on the ground (collide() keeps the feet on it): the launch clip's
		// root motion is the take-off step
		if (!cj::clipEnded(s_launchClip) && cj::clipPhase(s_launchClip) >= 0.0f) {
			poseOne(s_launchClip, false, true, false);
			return;
		}
		// CTaskSimpleJump::Launch
		s_jumpVZ = JUMP_UP_SPEED;
		s_jumpVX = -sinf(g.heading) * s_jumpH;
		s_jumpVY = cosf(g.heading) * s_jumpH;
		s_action = ACT_GLIDE;
		s_glideClip = "JUMP_glide";
		poseOne(s_glideClip, true, false, true);
		return;
	}
	if (s_action == ACT_GLIDE) {
		// CTaskSimpleInAir: hold a minimum forward speed while rising
		const float fx = -sinf(g.heading), fy = cosf(g.heading);
		const float fwd = s_jumpVX * fx + s_jumpVY * fy;
		if (s_jumpVZ > 0.0f && fwd < JUMP_MIN_RISE_FWD) {
			s_jumpVX += fx * (JUMP_MIN_RISE_FWD - fwd);
			s_jumpVY += fy * (JUMP_MIN_RISE_FWD - fwd);
		}
		// CTaskSimpleInAir: a long fall (-20 m/s) makes CJ yell, once
		if (s_jumpVZ <= -20.0f && !s_saidFall) {
			s_saidFall = true;
			aeped::sayPain(aeped::PAIN_CJ_HIGH_FALL, pedPos());
		}
		g.x += s_jumpVX * DT;
		g.y += s_jumpVY * DT;
		const float zPrev = g.z;
		g.z += s_jumpVZ * DT;
		s_jumpVZ -= JUMP_GRAVITY * DT;
		s_jumpVX *= JUMP_AIR_DRAG;
		s_jumpVY *= JUMP_AIR_DRAG;
		// the feet crossed a floor on the way down (probe from where they were)
		float fz;
		if (s_jumpVZ < 0.0f && floorAt(zPrev + 0.05f, g.z - 0.01f, fz)) {
			g.z = fz;
			const float vz = s_jumpVZ;
			s_jumpVX = s_jumpVY = s_jumpVZ = 0.0f;
			// CTaskComplexInAirAndLand: a running landing with the stick held
			// rolls on with JUMP_land (its root motion is the follow-through);
			// anything else plants with FALL_land, a long fall collapses
			const bool runLand = s_move.moveBlend > 1.5f && moveHeld;
			s_landClip = vz < COLLAPSE_VZ ? "FALL_collapse" : runLand ? "JUMP_land" : "FALL_land";
			s_landSpeed = runLand && sprintHeld && vz >= COLLAPSE_VZ ? 2 : 1;
			s_action = ACT_LAND;
			s_saidFall = false;
			// AE_PED_COLLAPSE_AFTER_FALL / AE_PED_LAND_ON_FEET_AFTER_FALL
			aeped::landing(vz < COLLAPSE_VZ, s_surface, pedPos());
			poseOne(s_landClip, false, true, true);
		} else {
			poseOne(s_glideClip, true, false, false);
		}
		return;
	}
	if (s_action == ACT_LAND) {
		if (cj::clipEnded(s_landClip) || cj::clipPhase(s_landClip) < 0.0f) {
			s_action = ACT_NONE;
			s_landSpeed = 1;
			// back to locomotion with the move blend the jump started with
			pedmove::resume(s_move, moveHeld);
		} else {
			poseOne(s_landClip, false, true, false);
		}
		return;
	}

	if (jumpDown && !aiming) {
		// StartLaunchAnim: the foot that is down picks launch / launch_R
		// (moving cycle phase + 0.367), and the run/sprint blend sets the
		// horizontal speed Launch() will give
		const cj::Motion mo = cj::motion();
		const pedmove::Cycle c = pedmove::dominantCycle(s_move);
		const char* cyc = c == pedmove::CYCLE_SPRINT ? mo.sprint : c == pedmove::CYCLE_RUN ? mo.run : mo.walk;
		float ph = pedmove::cycleWeight(s_move, c) > 0.3f ? cj::clipPhase(cyc) : -1.0f;
		ph = ph < 0.0f ? 0.0f : ph + 0.367f;
		if (ph > 1.0f) ph -= 1.0f;
		s_launchClip = ph >= 0.5f ? "JUMP_launch_R" : "JUMP_launch";
		const float wSprint = pedmove::cycleWeight(s_move, pedmove::CYCLE_SPRINT);
		const float wRun = pedmove::cycleWeight(s_move, pedmove::CYCLE_RUN);
		if (wSprint > 0.0f) s_jumpH = JUMP_H_RUN + (JUMP_H_SPRINT - JUMP_H_RUN) * wSprint;
		else if (wRun > 0.0f) s_jumpH = JUMP_H_STAND + (JUMP_H_RUN - JUMP_H_STAND) * wRun;
		else s_jumpH = JUMP_H_STAND;
		s_action = ACT_LAUNCH;
		poseOne(s_launchClip, false, true, true);
		return;
	}
	if (attackDown && melee) {
		// a ped on the floor in front gets the kick (AGROUND), else the combo starts
		s_action = ACT_PUNCH;
		s_combo = peds::downedInFront(g.x, g.y, g.z, g.heading, melee::REACH) ? melee::MOVE_GROUND : melee::MOVE_HIT_1;
		s_queued = s_swung = s_struck = false;
		poseOne(melee::k_unarmed[s_combo].clip, false, true, true);
	}
}

// CPed::PlayFootSteps: a step when the leading walk / run / sprint cycle
// passes 1/15 of its length (one foot) and 1/2 + 1/15 (the other), once the
// cycles carry more than half the blend. Volume / pitch by the move state.
static void playFootSteps() {
	const float w = pedmove::cycleWeight(s_move, pedmove::CYCLE_WALK) + pedmove::cycleWeight(s_move, pedmove::CYCLE_RUN) +
	                pedmove::cycleWeight(s_move, pedmove::CYCLE_SPRINT);
	const cj::Motion mo = cj::motion();
	const pedmove::Cycle c = pedmove::dominantCycle(s_move);
	const char* cyc = c == pedmove::CYCLE_SPRINT ? mo.sprint : c == pedmove::CYCLE_RUN ? mo.run : mo.walk;
	const float ph = w > 0.5f ? cj::clipPhase(cyc) : -1.0f;
	const float prev = s_footPhase;
	s_footPhase = ph;
	if (ph < 0.0f || prev < 0.0f) return;
	auto crossed = [&](float t) { return prev <= ph ? prev < t && ph >= t : prev < t || ph >= t; };
	const float vol = c == pedmove::CYCLE_SPRINT ? 0.0f : c == pedmove::CYCLE_RUN ? -6.0f : -12.0f;
	const float speed = c == pedmove::CYCLE_SPRINT ? 1.2f : c == pedmove::CYCLE_RUN ? 1.1f : 0.9f;
	if (crossed(1.0f / 15.0f)) aeped::footstep(true, vol, speed, s_surface, pedPos());
	if (crossed(0.5f + 1.0f / 15.0f)) aeped::footstep(false, vol, speed, s_surface, pedPos());
}

static void updateMove() {
	const input::State& in = input::get();
	const pad::Pad& pd = pad::get();
	const bool inAction = s_action != ACT_NONE;

	g.aiming = pd.target && !inAction && !g.inVehicle;
	if (pd.nextWeaponJustDown) nextWeapon();
	const Slot& wep = s_slots[s_slot];
	const bool melee = wep.melee || wep.type == WEAPON_UNARMED;

	// Touch is the mouse: a drag orbits the camera, it does not steer CJ.
	bool looking = false;
	// on the debug screen the touch panel drives the debug UI, not the camera
	if (in.touching && hud::screen() == hud::Screen::GAME) {
		if (s_camDrag) {
			s_camYaw = wrap(s_camYaw + (float)(in.touchX - s_lastTx) * 0.012f * FrontEndMenuManager.m_touchSensitivity);
			s_camPitch += (float)(in.touchY - s_lastTy) * 0.010f * FrontEndMenuManager.m_touchSensitivity;
		}
		s_lastTx = in.touchX;
		s_lastTy = in.touchY;
		s_camDrag = true;
		looking = true;
	} else {
		s_camDrag = false;
	}
	if (s_camPitch < -0.45f) s_camPitch = -0.45f;
	if (s_camPitch > 1.05f) s_camPitch = 1.05f;

	const bool hasMoveInput = pd.walkLR || pd.walkUD;

	// PlayerControlZelda: stick -> move blend + aiming rotation. Aiming owns
	// the ped (no blend). During a jump/punch PlayerControlZelda does not run
	// at all: the move blend is frozen (the landing picks its clip from it and
	// the run resumes at it) and the heading stays put (no air steering).
	bool moving = false;
	if (!inAction) {
		const pedmove::Input mi = { (float)pd.walkLR, (float)pd.walkUD, pd.pedWalk, !g.aiming, s_camYaw };
		float wish = g.heading;
		moving = pedmove::control(s_move, mi, CAM_TIMESTEP, wish);
		if (moving) pedmove::turn(g.heading, wish, DT, false);
		else if (g.aiming) pedmove::turn(g.heading, s_camYaw, DT, true);
	}

	// sprint keeps its pace through a jump while the button stays held
	const bool sprintable = (moving || (inAction && hasMoveInput)) && !g.aiming;
	const bool wantSprint = sprintable && pd.sprint;
	const bool tapSprint = sprintable && pd.sprintJustDown;
	const float sprintR = controlSprint(wantSprint, tapSprint);
	const bool sprinting = sprintR >= 1.0f;

	recoverEnergy(moving && !sprinting && s_move.moveBlend > 1.0f, sprinting);
	g.sprint = s_timeCanRun <= 0.0f ? 0.0f : s_timeCanRun * 100.0f / energyMax();
	if (g.sprint > 100.0f) g.sprint = 100.0f;

	if (!melee && !inAction) {
		const bool trigger = wep.automatic ? pd.attack : pd.attackJustDown;
		const int gap = wep.automatic ? 5 : 16;
		if (trigger && s_fireCd == 0 && fireOne()) {
			s_fireCd = gap;
			const aeped::Pos p = pedPos();
			aeweapon::fire(wep.type, p.x, p.y, p.z);   // CAEWeaponAudioEntity::WeaponFire
			// CWeapon::FireInstantHit, reduced: along the aim (a little under the camera
			// pitch), stopped by the map; vehicles in the way take it (tyres burst)
			const float pitch = s_camPitch - 0.25f, cp = cosf(pitch);
			const float a[3] = { g.x, g.y, g.z + 1.25f };
			float b[3] = { a[0] - sinf(g.heading) * cp * 45.0f, a[1] + cosf(g.heading) * cp * 45.0f, a[2] - sinf(pitch) * 45.0f };
			col::Hit h;
			if (col::castSegment(a, b, h))
				for (int k = 0; k < 3; k++) b[k] = a[k] + (b[k] - a[k]) * h.t;
			peds::bulletHit(a, b, wep.type);   // a ped in the way stops it
			veh::bulletHit(a, b, 25.0f);
			peds::gunShot(g.x, g.y, g.z);      // CEventGunShot
		}
	}

	updateAction(pd.jumpJustDown && !g.aiming, pd.attackJustDown, g.aiming, melee, hasMoveInput,
	             pd.sprint && sprinting);
	if (s_action != ACT_NONE) {
		s_footPhase = -1.0f;   // bIsInTheAir / an action owns the legs: no steps
		cj::advance(s_action == ACT_LAND ? s_landSpeed : 1);
		float mx, my;
		cj::rootDelta(mx, my);
		const float c = cosf(g.heading), s = sinf(g.heading);
		const float wx = mx * c - my * s;
		const float wy = mx * s + my * c;
		g.x += wx;
		g.y += wy;
		s_stepVX = wx / DT;
		s_stepVY = wy / DT;
		// Jump displacement is physics-driven (not root motion), so retain it
		// in the speed snapshot the radar uses while CJ is airborne. Only the
		// glide still integrates s_jumpVX; the launch's carry is root motion.
		const float vx = wx / DT + (s_action == ACT_GLIDE ? s_jumpVX : 0.0f);
		const float vy = wy / DT + (s_action == ACT_GLIDE ? s_jumpVY : 0.0f);
		g.speed = sqrtf(vx * vx + vy * vy);
		if (FrontEndMenuManager.m_cameraFollowDpad && !looking && !g.aiming &&
		    camFollows(pd.walkUD, hasMoveInput)) followCam(g.heading);
		else { s_betaSpeed = 0.0f; s_betaSign = 0; }
		return;
	}

	if (!g.aiming) {
		pedmove::setRealMoveAnim(s_move, cj::motion(), sprinting, s_spent, DT, k_cjPose);
	} else {
		pedmove::suspend(s_move);
		const char* pose = melee ? "FIGHTIDLE" : "IDLE_armed";
		if (hasMoveInput) {
			// Stick relative to the aim heading: forward / back / strafe
			// (ControlGunMove / the fight shuffle).
			const float ix = pd.walkLR / 128.0f, iy = pd.walkUD / 128.0f;
			const float fx = -sinf(g.heading), fy = cosf(g.heading);
			const float rx = cosf(g.heading), ry = sinf(g.heading);
			const float dx = -sinf(s_camYaw) * iy + cosf(s_camYaw) * ix;
			const float dy =  cosf(s_camYaw) * iy + sinf(s_camYaw) * ix;
			const float fwd = dx * fx + dy * fy;
			const float side = dx * rx + dy * ry;
			if (melee) {
				if (fabsf(side) > fabsf(fwd)) pose = side > 0.0f ? "FightSh_Right" : "FightSh_Left";
				else pose = fwd >= 0.0f ? "FightSh_FWD" : "FightSh_BWD";
			} else {
				if (fabsf(side) > fabsf(fwd)) pose = side > 0.0f ? "GunMove_R" : "GunMove_L";
				else pose = fwd >= 0.0f ? "GunMove_FWD" : "GunMove_BWD";
			}
		} else if (!melee) {
			pose = "Gun_stand";
		}
		// The aim shuffles and FIGHTIDLE all carry CAN_EXTRACT_VELOCITY; the
		// gun idles (Gun_stand / IDLE_armed) do not.
		poseOne(pose, true, hasMoveInput || melee, false);
	}

	cj::advance(1);
	if (!g.aiming) playFootSteps();
	else s_footPhase = -1.0f;
	float mx, my;
	cj::rootDelta(mx, my);
	if (sprinting && sprintR > 1.0f) {
		mx *= sprintR;
		my *= sprintR;
	}
	mx *= dbg::opts.playerSpeedQ * 0.25f;
	my *= dbg::opts.playerSpeedQ * 0.25f;
	const float c = cosf(g.heading), s = sinf(g.heading);
	const float wx = mx * c - my * s;
	const float wy = mx * s + my * c;
	g.x += wx;
	g.y += wy;
	g.speed = sqrtf(wx * wx + wy * wy) / DT;
	s_stepVX = wx / DT;
	s_stepVY = wy / DT;

	// keyboard-mode camera: swings back behind CJ while he moves (or is
	// still finishing a start/stop) unless the player is looking around
	if (FrontEndMenuManager.m_cameraFollowDpad && !looking && !g.aiming &&
	    (moving || s_move.phase != 0) && camFollows(pd.walkUD, hasMoveInput))
		followCam(g.heading);
	else { s_betaSpeed = 0.0f; s_betaSign = 0; }
}

// CTaskComplexInAirAndLand from a walk off a ledge: the glide physics with
// the step's velocity and FALL_fall.
static void startFall() {
	s_action = ACT_GLIDE;
	s_saidFall = false;
	s_jumpVX = s_stepVX;
	s_jumpVY = s_stepVY;
	s_jumpVZ = 0.0f;
	s_glideClip = "FALL_fall";
	poseOne(s_glideClip, true, false, true);
}

// After every step: walls, then the feet (the glide lands by itself).
static void collide() {
	if (physics())
		for (const float hz : col::PED_SPHERE_Z) col::pushSphere(g.x, g.y, g.z + hz, col::PED_RADIUS);
	// hit by a moving vehicle: knocked back and hurt (CPed::KillPedWithCar, reduced)
	veh::PedHit hit;
	veh::pushPed(g.x, g.y, g.z + 1.0f, col::PED_RADIUS, vehicleId(), &hit);
	static uint32_t s_lastHit;
	if (hit.id >= 0 && hit.speed > 0.08f && timing::vblanks() - s_lastHit > 30) {
		s_lastHit = timing::vblanks();
		hitByVehicle(hit.vx * 60.0f, hit.vy * 60.0f, hit.speed * 120.0f);
		veh::pedHitSound(hit.id, g.x, g.y, g.z);
	}
	peds::pushPlayer(g.x, g.y, g.z, col::PED_RADIUS);
	if (s_action == ACT_GLIDE) return;
	float z;
	if (floorAt(g.z + STEP_UP, g.z - SNAP_DOWN, z)) {
		// steps up ease in over a few frames, slopes and steps down snap
		g.z = z > g.z + 0.02f ? g.z + (z - g.z) * 0.5f : z;
		return;
	}
	if (physics() && col::pending()) return;   // the floor may still be streaming: hold
	if (floorAt(g.z + STEP_UP, g.z - FALL_PROBE, z)) {
		startFall();
		return;
	}
	// nothing below at all: under the map (teleport, respawn): back on top
	if (floorAt(g.z + 60.0f, g.z, z)) g.z = z;
}

// CCam::Process_FollowPed's line of sight: CJ's head -> the eye. A wall in
// between pulls the camera in at once; it eases back out when clear.
static void updateCamera() {
	const bool inCar = s_carTask != CT_NONE && s_carTask != CT_GOTO;
	const float want = inCar ? s_camCarDist : dbg::opts.camDist;
	float d = want;
	if (physics()) {
		const float cp = cosf(s_camPitch), sp = sinf(s_camPitch), len = want + CAM_MARGIN;
		const float a[3] = { g.x, g.y, s_viewZ + s_lookH };
		const float b[3] = { a[0] + sinf(s_camYaw) * cp * len, a[1] - cosf(s_camYaw) * cp * len, a[2] + sp * len };
		col::Hit h;
		if (col::castSegment(a, b, h)) d = h.t * len - CAM_MARGIN;
		if (d < 0.5f) d = 0.5f;
	}
	s_camDist = d < s_camDist ? d : s_camDist + (d - s_camDist) * 0.08f;
}

// ----------------------------------------------------------------------------
// Vehicles: CTaskComplexEnterCarAsDriver (go to door, CAR_open_LHS,
// CAR_getin_LHS, CAR_closedoor_LHS), CTaskSimpleCarDrive (CAR_sit + Drive_L/R
// by the steer), CTaskComplexLeaveCar (CAR_getout_LHS, CAR_close_LHS).
// The CAR_* clips are not velocity-extracted: the ped origin stays at the door
// point D while their root translation carries the mesh into the seat, so
// D = seat - root translation at the end of CAR_getin_LHS (ped.ifp:
// (0.951, 0.486, -0.35), the low-car L variant (0.96, 0.51, -0.693)).
// Bikes / BMX / quads (CTaskSimpleCarGetIn with the bike anim group): the
// vehicle block's jumpon / getoff clips with the root extracted while the ped
// origin slides from the side point to the seat; riding = <group>_Ride +
// Left / Right by the steer (+ the pedal cycle on bicycles). Boats, aircraft,
// trains: CJ walks to the side and sits down at once (Drive_boat / CAR_sit).
// RC toys (CVehicle::IsRCVehicle): driven from outside, CJ is hidden.
// ----------------------------------------------------------------------------
enum EntryStyle : uint8_t { ES_CAR, ES_BIKE, ES_DIRECT };
static EntryStyle s_style;
static char s_animPrefix[12];     // bike group clip prefix ("BIKEs_", "BMX_", "QUAD_"...)
static bool s_remote;             // RC: CJ hidden while driving
static bool s_pedalBike;          // BMX / MTB / choppa: the pedal cycle while accelerating

int vehicleId() { return s_carTask == CT_NONE ? -1 : s_car; }
bool driving() { return s_carTask == CT_DRIVE; }
float camLookH() { return s_lookH; }
bool hiddenInVehicle() { return s_remote && s_carTask == CT_DRIVE; }

static const char* carClip(CarTask t) {
	static char buf[32];
	if (s_style == ES_BIKE) {
		const bool quad = !strcmp(s_animPrefix, "QUAD_");
		const char* suffix = t == CT_GETIN ? (quad ? "geton_LHS" : "jumponL") : t == CT_GETOUT ? (quad ? "getoff_LHS" : "getoffLHS") : nullptr;
		if (!suffix) return nullptr;
		snprintf(buf, sizeof buf, "%s%s", s_animPrefix, suffix);
		return buf;
	}
	switch (t) {
	case CT_OPEN: return "CAR_open_LHS";
	case CT_GETIN: return s_low ? "CAR_getinL_LHS" : "CAR_getin_LHS";
	case CT_CLOSE: return s_low ? "CAR_closedoorL_LHS" : "CAR_closedoor_LHS";
	case CT_GETOUT: return s_low ? "CAR_getoutL_LHS" : "CAR_getout_LHS";
	case CT_CLOSEOUT: return "CAR_close_LHS";
	default: return nullptr;
	}
}

// driver seat / door point (ped origin, vehicle model space)
static bool carPoints(const veh::Vehicle& v, float seat[3], float door[3]) {
	const vehmdl::Model* m = vehmdl::get(v.model);
	if (!m) return false;
	memcpy(seat, m->seat, 12);
	if (s_style == ES_CAR) {
		const float t[3] = { s_low ? 0.96f : 0.951f, s_low ? 0.51f : 0.486f, s_low ? -0.693f : -0.35f };
		for (int k = 0; k < 3; k++) door[k] = seat[k] - t[k];
	} else if (s_style == ES_BIKE) {
		door[0] = seat[0] - 0.85f;
		door[1] = seat[1] - 0.3f;
		door[2] = seat[2];
	} else {
		// beside the hull, level with the seat
		door[0] = m->bmin[0] - 0.6f;
		door[1] = seat[1];
		door[2] = seat[2];
	}
	return true;
}

static float smooth01(float x, float a, float b) {
	const float t = x <= a ? 0.0f : x >= b ? 1.0f : (x - a) / (b - a);
	return t * t * (3.0f - 2.0f * t);
}

// the ped origin at a vehicle-space point: g = its world xy, feet below it
static void placeAt(const veh::Vehicle& v, const float l[3]) {
	float w[3];
	veh::toWorld(v, l, w);
	g.x = w[0];
	g.y = w[1];
	g.z = w[2] - cj::lift();
	g.heading = veh::heading(v);
}

bool attachedMatrix(float ox, float oy, float oz, s32 out[12]) {
	if (s_carTask == CT_NONE || s_carTask == CT_GOTO) return false;
	const veh::Vehicle* v = veh::get(s_car);
	float seat[3], door[3];
	if (!v || !carPoints(*v, seat, door)) return false;
	float at[3];
	if (s_carTask == CT_DRIVE) memcpy(at, seat, 12);
	else if (s_style == ES_BIKE) {
		// the origin slides between the side point and the seat over the clip
		float ph = cj::clipPhase(carClip(s_carTask));
		if (ph < 0.0f) ph = 0.0f;
		const float t = s_carTask == CT_GETIN ? smooth01(ph, 0.1f, 0.8f) : 1.0f - smooth01(ph, 0.15f, 0.85f);
		for (int k = 0; k < 3; k++) at[k] = door[k] + (seat[k] - door[k]) * t;
	} else memcpy(at, door, 12);
	veh::attachMatrix(*v, at, ox, oy, oz, out);
	return true;
}

static void endCarTask(bool onFoot) {
	pad::setOverride(nullptr);
	veh::Vehicle* v = veh::get(s_car);
	if (v && s_carTask != CT_GOTO) {
		float seat[3], door[3];
		if (carPoints(*v, seat, door)) {
			// boats: step off on whichever side has ground above the water
			if (v->cls == veh::CLS_BOAT) {
				const vehmdl::Model* m = vehmdl::get(v->model);
				const float sides[4][2] = { { m->bmin[0] - 0.6f, seat[1] }, { m->bmax[0] + 0.6f, seat[1] },
				                            { 0.0f, m->bmin[1] - 0.8f }, { 0.0f, m->bmax[1] + 0.8f } };
				for (const auto& sd : sides) {
					const float l[3] = { sd[0], sd[1], seat[2] };
					float w[3], wz;
					veh::toWorld(*v, l, w);
					col::Hit h;
					const bool ground = col::groundAt(w[0], w[1], w[2] + 2.0f, w[2] - 8.0f, h);
					if (ground && (!water::levelAt(w[0], w[1], wz) || h.z > wz)) { door[0] = sd[0]; door[1] = sd[1]; break; }
				}
			}
			placeAt(*v, door);
		}
		veh::setDoor(s_car, veh::DOOR_LF, 0.0f);
		if (s_carTask == CT_DRIVE) veh::setDriver(s_car, false);
	}
	s_carTask = CT_NONE;
	s_car = -1;
	s_remote = false;
	cj::setVehicleBlock(nullptr);
	g.inVehicle = false;
	g.vehicleName = nullptr;
	s_action = ACT_NONE;
	if (onFoot) {
		placeOnGround();
		pedmove::reset(s_move);
		poseOne(cj::motion().idle, true, false, true);
	}
}

void leaveVehicleNow() { endCarTask(true); }

static void startDriving() {
	s_carTask = CT_DRIVE;
	s_wantExit = false;
	g.inVehicle = true;
	veh::setDriver(s_car, true);
	ds::VehInfo info;
	const veh::Vehicle* v = veh::get(s_car);
	if (v && vehdata::read(v->vehIndex, info)) g.vehicleName = TheText.get(info.gxt);
}

// entry style + the bike clip prefix from the IDE anim group
static void chooseStyle(const veh::Vehicle& v, const ds::VehInfo& info) {
	s_low = info.modelFlags & 4;   // IS_LOW: the L clip set
	s_remote = veh::isRemote(v.modelId);
	s_pedalBike = v.type == ds::VEH_BMX;
	s_animPrefix[0] = 0;
	if (s_remote) s_style = ES_DIRECT;
	else if (v.cls == veh::CLS_BIKE || v.type == ds::VEH_QUAD) {
		static const char* const k_groups[][2] = { { "bikes", "BIKEs_" }, { "bikev", "BIKEv_" }, { "bikeh", "BIKEh_" },
		                                           { "biked", "BIKEd_" }, { "bmx", "BMX_" }, { "choppa", "CHOPPA_" },
		                                           { "mtb", "MTB_" }, { "wayfarer", "WF_" }, { "quad", "QUAD_" } };
		const char* pre = v.type == ds::VEH_BMX ? "BMX_" : v.type == ds::VEH_QUAD ? "QUAD_" : "BIKEs_";
		const char* blk = v.type == ds::VEH_BMX ? "bmx" : v.type == ds::VEH_QUAD ? "quad" : "bikes";
		for (const auto& gr : k_groups)
			if (!strcmp(info.anims, gr[0])) { blk = gr[0]; pre = gr[1]; }
		snprintf(s_animPrefix, sizeof s_animPrefix, "%s", pre);
		cj::setVehicleBlock(blk);
		s_style = ES_BIKE;
	} else if (v.cls == veh::CLS_AUTOMOBILE) s_style = ES_CAR;
	else s_style = ES_DIRECT;
}

void enterVehicleNow(int id) {
	veh::Vehicle* v = veh::get(id);
	if (!v || v->status == veh::STATUS_WRECKED || !vehmdl::ready(v->model)) return;
	if (s_carTask != CT_NONE) endCarTask(false);
	ds::VehInfo info;
	if (!vehdata::read(v->vehIndex, info)) return;
	s_car = id;
	chooseStyle(*v, info);
	startDriving();
}

static void startCarTask(CarTask t) {
	s_carTask = t;
	s_taskSteps = 0;
	if (const char* clip = carClip(t)) poseOne(clip, false, s_style == ES_BIKE, true);
}

// Y on foot: the nearest vehicle with its door in reach
static void tryEnter() {
	const int id = veh::findEnterable(g.x, g.y, g.z, 5.0f);
	if (id < 0) return;
	veh::Vehicle* v = veh::get(id);
	ds::VehInfo info;
	if (!v || !vehdata::read(v->vehIndex, info)) return;
	s_car = id;
	chooseStyle(*v, info);
	s_carTask = CT_GOTO;
	s_taskSteps = 0;
	s_action = ACT_NONE;
}

// CTaskSimpleCarDrive per vehicle type: the pad -> veh::Controls and the pose
static void driveStep(veh::Vehicle& v) {
	const pad::Pad& pd = pad::raw();
	const u32 held = input::get().held;
	const bool flying = v.cls == veh::CLS_HELI || v.cls == veh::CLS_PLANE;
	veh::Controls c = {};
	c.steer = (float)pd.walkLR / 128.0f;
	if (flying) {
		// A throttle, B descend / brake, d-pad up/down pitch (up = nose down), left/right roll, L / R yaw
		c.accel = held & KEY_A ? 1.0f : 0.0f;
		c.brake = held & KEY_B ? 1.0f : 0.0f;
		c.pitch = held & KEY_UP ? -1.0f : held & KEY_DOWN ? 1.0f : 0.0f;
		c.yaw = (held & KEY_R ? 1.0f : 0.0f) - (held & KEY_L ? 1.0f : 0.0f);
	} else {
		c.accel = (held & (KEY_UP | KEY_A)) ? 1.0f : 0.0f;
		c.brake = (held & (KEY_DOWN | KEY_B)) ? 1.0f : 0.0f;
		c.handbrake = held & KEY_R;
		c.horn = held & KEY_L;
	}
	if (pd.enterExitJustDown) s_wantExit = true;
	if (s_wantExit) { c.accel = 0.0f; c.brake = v.speedFwd > 0.0f && !flying ? 1.0f : 0.0f; c.steer = 0.0f; }
	veh::setControls(s_car, c);
	const float s = v.rawSteer;
	const float ws = fabsf(s) > 1.0f ? 1.0f : fabsf(s);
	if (s_style == ES_BIKE) {
		char ride[24], turn[24];
		snprintf(ride, sizeof ride, "%s%s", s_animPrefix, s_pedalBike && v.gas > 0.1f ? "pedal" : "Ride");
		snprintf(turn, sizeof turn, "%s%s", s_animPrefix, s > 0.0f ? "Left" : "Right");
		const cj::Clip cl[2] = { { ride, 1.0f - ws * 0.999f, true, false, false, false, 0.0f },
		                         { turn, ws * 0.999f + 0.001f, true, false, false, false, 0.0f } };
		cj::pose(cl, 2);
	} else if (!s_remote) {
		const bool boat = v.cls == veh::CLS_BOAT;
		const char* sit = boat ? "Drive_boat" : s_low ? "CAR_Lsit" : "CAR_sit";
		const char* turnClip = boat ? (s > 0.0f ? "Drive_boat_L" : "Drive_boat_R")
		                       : s > 0.0f ? (s_low ? "Drive_LO_l" : "Drive_L") : (s_low ? "Drive_LO_R" : "Drive_R");
		const cj::Clip cl[2] = { { sit, 1.0f - ws * 0.999f, true, false, false, false, 0.0f },
		                         { turnClip, ws * 0.999f + 0.001f, true, false, false, false, 0.0f } };
		cj::pose(cl, 2);
	}
	cj::advance(1);
}

// One 60 Hz step while a car task owns CJ. True = it did (no on-foot step).
static bool carStep() {
	veh::Vehicle* v = veh::get(s_car);
	float seat[3], door[3];
	if (!v || !carPoints(*v, seat, door)) {
		endCarTask(true);
		return false;
	}
	s_taskSteps++;
	if (s_carTask == CT_GOTO) {
		// CTaskComplexGoToCarDoorAndStandStill: walk at the door point with a
		// scripted stick (camera-relative, like the real pad)
		float w[3];
		veh::toWorld(*v, door, w);
		const float dx = w[0] - g.x, dy = w[1] - g.y, d = sqrtf(dx * dx + dy * dy);
		const float reach = s_style == ES_DIRECT ? 1.2f : 0.45f;
		if (d < reach || s_taskSteps > 240) {
			pad::setOverride(nullptr);
			if (d > 1.5f && s_style != ES_DIRECT) { endCarTask(true); return false; }
			pedmove::suspend(s_move);
			if (s_style == ES_CAR) startCarTask(CT_OPEN);
			else if (s_style == ES_BIKE) startCarTask(CT_GETIN);
			else startDriving();
			return true;
		}
		const float ux = dx / d, uy = dy / d, cy = cosf(s_camYaw), sy = sinf(s_camYaw);
		s_goPad = {};
		s_goPad.walkUD = (int16_t)((-sy * ux + cy * uy) * 128.0f);
		s_goPad.walkLR = (int16_t)((cy * ux + sy * uy) * 128.0f);
		s_goPad.pedWalk = d < 1.2f;
		pad::setOverride(&s_goPad);
		return false;
	}
	pad::setOverride(nullptr);
	if (s_carTask == CT_DRIVE) {
		driveStep(*v);
		placeAt(*v, seat);
		g.speed = fabsf(v->speedFwd) * 50.0f;
		const bool flying = v->cls == veh::CLS_HELI || v->cls == veh::CLS_PLANE;
		const float still = flying ? 0.12f : 0.06f;
		if (s_wantExit && (fabsf(v->speedFwd) < still || s_style == ES_DIRECT)) {
			veh::setDriver(s_car, false);
			g.vehicleName = nullptr;
			if (s_style == ES_DIRECT) endCarTask(true);
			else startCarTask(CT_GETOUT);
		}
		return true;
	}
	// the enter / exit clips at the door point (bikes: root extracted, the origin slides)
	const char* clip = carClip(s_carTask);
	poseOne(clip, false, s_style == ES_BIKE, false);
	cj::advance(1);
	{
		float dx, dy;
		cj::rootDelta(dx, dy);   // the extracted bike-clip root motion is replaced by the slide below
	}
	const float ph = cj::clipPhase(clip);
	if (s_style == ES_BIKE) {
		float at[3];
		const float t = s_carTask == CT_GETIN ? smooth01(ph < 0.0f ? 0.0f : ph, 0.1f, 0.8f) : 1.0f - smooth01(ph < 0.0f ? 0.0f : ph, 0.15f, 0.85f);
		for (int k = 0; k < 3; k++) at[k] = door[k] + (seat[k] - door[k]) * t;
		placeAt(*v, at);
	} else {
		placeAt(*v, door);
	}
	switch (s_carTask) {
	case CT_OPEN: veh::setDoor(s_car, veh::DOOR_LF, smooth01(ph, 0.2f, 0.65f)); break;
	case CT_GETIN: if (s_style == ES_CAR) veh::setDoor(s_car, veh::DOOR_LF, 1.0f); break;
	case CT_CLOSE: veh::setDoor(s_car, veh::DOOR_LF, 1.0f - smooth01(ph, 0.15f, 0.6f)); break;
	case CT_GETOUT: if (s_style == ES_CAR) veh::setDoor(s_car, veh::DOOR_LF, smooth01(ph, 0.0f, 0.3f)); break;
	case CT_CLOSEOUT: veh::setDoor(s_car, veh::DOOR_LF, 1.0f - smooth01(ph, 0.25f, 0.65f)); break;
	default: break;
	}
	// a clip that never streamed in must not hang the task
	const bool done = cj::clipEnded(clip) || (ph < 0.0f && s_taskSteps > 10) || s_taskSteps > 240;
	if (done) {
		switch (s_carTask) {
		case CT_OPEN: startCarTask(CT_GETIN); break;
		case CT_GETIN:
			if (s_style == ES_BIKE) startDriving();
			else startCarTask(CT_CLOSE);
			break;
		case CT_CLOSE: veh::setDoor(s_car, veh::DOOR_LF, 0.0f); startDriving(); break;
		case CT_GETOUT:
			if (s_style == ES_BIKE) endCarTask(true);
			else startCarTask(CT_CLOSEOUT);
			break;
		default: endCarTask(true); break;
		}
	}
	return true;
}

void vehicleExploded(int id) {
	if (id != s_car) return;
	endCarTask(true);
	g.health = 0.0f;
}

void explosionDamage(float x, float y, float, float damage) {
	g.health -= damage;
	if (g.health < 0.0f) g.health = 0.0f;
	if (s_carTask != CT_NONE) return;
	const float dx = g.x - x, dy = g.y - y, d = sqrtf(dx * dx + dy * dy) + 0.1f;
	s_action = ACT_GLIDE;
	s_jumpVX = dx / d * 6.0f;
	s_jumpVY = dy / d * 6.0f;
	s_jumpVZ = 5.0f;
	s_glideClip = "FALL_fall";
	poseOne(s_glideClip, true, false, true);
}

void hitByVehicle(float vx, float vy, float damage) {
	if (s_carTask != CT_NONE && s_carTask != CT_GOTO) return;
	if (s_carTask == CT_GOTO) endCarTask(false);
	g.health -= damage;
	if (g.health < 0.0f) g.health = 0.0f;
	s_action = ACT_GLIDE;
	s_jumpVX = vx * 0.8f;
	s_jumpVY = vy * 0.8f;
	s_jumpVZ = 3.0f;
	s_glideClip = "FALL_fall";
	poseOne(s_glideClip, true, false, true);
}

void takeDamage(float damage) {
	if (damage <= 0.0f || g.health <= 0.0f) return;
	const float a = g.armour < damage ? g.armour : damage;
	g.armour -= a;
	g.health -= damage - a;
	if (g.health < 0.0f) g.health = 0.0f;
}

bool isDown() { return s_action == ACT_DOWN || s_action == ACT_GETUP || s_action == ACT_DEAD; }

static float relHeading(float fx, float fy) {
	return wrap(atan2f(-(fx - g.x), fy - g.y) - g.heading);   // 0 = in front, > 0 on the left
}

static void dieNow(float rel) {
	const bool front = fabsf(rel) < 0.785f, back = fabsf(rel) > 2.356f;
	s_hitClip = front ? "KO_skid_front" : back ? "KO_skid_back" : rel > 0.0f ? "KO_spin_L" : "KO_spin_R";
	s_action = ACT_DEAD;
	s_jumpVX = s_jumpVY = s_jumpVZ = 0.0f;
	pedmove::suspend(s_move);
	aeped::sayPain(aeped::PAIN_CJ_DEATH, pedPos());
	poseOne(s_hitClip, false, false, true);
}

void meleeHitBy(float fx, float fy, int move, float damage) {
	if (s_action == ACT_DEAD || g.health <= 0.0f) return;
	if (s_carTask != CT_NONE && s_carTask != CT_GOTO) { takeDamage(damage); return; }
	if (s_carTask == CT_GOTO) endCarTask(false);
	takeDamage(damage);
	aeped::pedHit(pedPos());
	const float rel = relHeading(fx, fy);
	const bool front = fabsf(rel) < 0.785f, back = fabsf(rel) > 2.356f;
	if (g.health <= 0.0f) { dieNow(rel); return; }
	pedmove::suspend(s_move);
	s_footPhase = -1.0f;
	s_jumpVX = s_jumpVY = s_jumpVZ = 0.0f;
	if (s_action == ACT_DOWN || s_action == ACT_GETUP) {
		// kicked on the floor (AGROUND): FLOOR_hit lying on the back, _f face down
		s_hitClip = !strcmp(s_getupClip, "getup") ? "FLOOR_hit" : "FLOOR_hit_f";
		s_action = ACT_DOWN;
		s_downSteps = 0;
		aeped::sayPain(aeped::PAIN_CJ_GENERIC_LOW, pedPos());
	} else if (melee::knocksDown(move)) {
		// CTaskComplexFallAndGetUp: hit from the front = on the back
		s_hitClip = back ? "KO_skid_back" : "KO_skid_front";
		s_getupClip = back ? "getup_front" : "getup";
		s_action = ACT_DOWN;
		s_downSteps = 0;
		aeped::sayPain(aeped::PAIN_CJ_GENERIC_HIGH, pedPos());
	} else {
		// CTaskSimpleBeHit::StartAnim: the attacker's combo move from the front
		s_hitClip = front ? melee::frontHitClip(move) : back ? "HIT_back" : rel > 0.0f ? "HIT_L" : "HIT_R";
		s_action = ACT_BEHIT;
		aeped::sayPain(aeped::PAIN_CJ_GENERIC_LOW, pedPos());
	}
	poseOne(s_hitClip, false, false, true);
}

// CWanted::UpdateWantedLevel
static void updateWanted() {
	if (s_chaos > 4600) s_chaos = 4600;   // MaximumChaosLevel
	static const uint32_t k_stars[6] = { 50, 180, 550, 1200, 2400, 4600 };
	int w = 0;
	while (w < 6 && s_chaos >= k_stars[w]) w++;
	g.wantedLevel = w;
}

void registerCrime(Crime c) {
	static const uint8_t k_mult[] = { 5, 45, 35, 100, 18, 25, 2 };   // ReportCrimeNow, m_Multiplier 1
	s_chaos += k_mult[c];
	updateWanted();
}

void setWantedLevel(int level) {
	static const uint32_t k_chaos[7] = { 0, 70, 200, 570, 1220, 2420, 4620 };
	s_chaos = k_chaos[level < 0 ? 0 : level > 6 ? 6 : level];
	updateWanted();
}

// CCam::Process_FollowCar_SA, reduced: behind the vehicle (its velocity
// heading once it moves), distance and height from the model's size;
// aircraft sit further back and look along their pitch a little
static void updateCarCamera(const veh::Vehicle& v, bool looking) {
	const vehmdl::Model* m = vehmdl::get(v.model);
	const float len = m ? m->bmax[1] - m->bmin[1] : 4.5f;
	const float top = m ? m->bmax[2] : 1.0f;
	const float sp = sqrtf(v.moveSpeed[0] * v.moveSpeed[0] + v.moveSpeed[1] * v.moveSpeed[1]);
	const bool flying = v.cls == veh::CLS_HELI || v.cls == veh::CLS_PLANE;
	float target = veh::heading(v);
	if (sp > 0.05f && v.speedFwd > 0.0f && v.cls != veh::CLS_HELI) target = atan2f(-v.moveSpeed[0], v.moveSpeed[1]);
	if (!looking && FrontEndMenuManager.m_cameraFollowDpad) {
		const float d = wrap(target - s_camYaw);
		s_camYaw = wrap(s_camYaw + d * (sp > 0.02f ? 0.12f : 0.05f));
		const float wantPitch = flying ? 0.3f - v.fwd[2] * 0.6f : 0.22f;
		s_camPitch += (wantPitch - s_camPitch) * 0.05f;
	}
	const float want = len * (flying ? 1.4f : 1.05f) + (flying ? 4.0f : 2.2f) + sp * 4.0f;
	s_camCarDist += (want - s_camCarDist) * 0.1f;
	s_lookH += (top + 0.9f - s_lookH) * 0.2f;
}

void init() {
	g = {};
	g.x = SPAWN_X; g.y = SPAWN_Y; g.z = SPAWN_Z;
	g.health = g.maxHealth = 100.0f;     // STAT_MAX_HEALTH 569 -> 100
	g.armour = 0.0f;
	g.maxArmour = 100.0f;                // CPlayerInfo::m_nMaxArmour
	g.maxBreath = 100.0f;                // lung capacity stat starts at 0
	g.breath = g.maxBreath;              // on land the breath bar stays hidden
	g.money = 0;
	g.wantedLevel = 0;
	s_chaos = s_wantedMs = 0;
	g.fat = 0.0f;
	g.muscle = 0.0f;
	g_clockH = 12; g_clockM = 0;
	g_clockDay = 1; g_clockMonth = 1;
	g_weekday = 4;                       // CClock::CurrentDay
	s_camYaw = 0;
	s_camPitch = 0.42f;
	s_viewZ = SPAWN_Z;
	s_camDist = dbg::opts.camDist;
	s_stepVX = s_stepVY = 0.0f;
	s_betaSpeed = 0;
	s_betaSign = 0;
	s_zoneTimer = 0;
	s_teleportWait = 0;
	s_camDrag = false;
	pedmove::reset(s_move);
	s_spent = false;
	s_action = ACT_NONE;
	s_combo = 0;
	s_jumpVX = s_jumpVY = s_jumpVZ = 0.0f;
	s_reload = 0;
	s_fireCd = 0;
	s_stamina = 100.0f;
	s_timeCanRun = energyMax();
	s_moveSpeed = 0;
	s_sprintMs = s_runMs = 0;
	g.sprint = 100.0f;
	s_hoursNoEat = 0;
	s_hungryTold = false;
	s_clockAcc = 0;
	s_lastVbl = timing::vblanks();
	memset(s_slots, 0, sizeof s_slots);
	s_slots[0].type = WEAPON_UNARMED;
	s_slots[0].melee = true;
	s_slot = 0;
	syncWeapon();
	g.zoneName = nullptr;
	g.vehicleName = nullptr;
	g.aiming = false;
	g.inVehicle = false;
	g.vehicleRequest = false;
	cj::setStats(0.0f, 0.0f);   // the model itself is loaded by the loading screen (cj::initStep)
	hud::log("new game $%d fists wanted %d", g.money, g.wantedLevel);
}

void update() {
	pad::update(input::get());
	g.vehicleRequest = pad::get().enterExitJustDown;
	// inVehicle stays false until a car exists to enter. Y is the edge the
	// vehicle port will consume; pressing it on foot changes nothing else.

	const u32 v = timing::vblanks();
	uint32_t dv = v - s_lastVbl;
	s_lastVbl = v;
	if (dv > 6) dv = 6; // a stall shouldn't skip the clock or the exercise timers
	tickClock(dv);

	if (s_teleportWait) {
		col::Hit hit;
		if (s_teleportWait == 300 || (col::available() && (col::pending() ||
		    !col::groundAt(g.x, g.y, g.z + 4.0f, g.z - 30.0f, hit)))) {
			if (--s_teleportWait) {
				input::consumeEdges();
				pad::consumeEdges();
				return;
			}
		} else {
			if (col::available()) g.z = hit.z;
			s_teleportWait = 0;
		}
		s_viewZ = g.z;
	}

	// Fixed 60 Hz simulation: one step per elapsed vblank, so a 30 fps frame
	// runs two. Every constant below (CAM_TIMESTEP, DT, sprint rates, clip
	// advance(1)) is per 1/60 s — stepping once per *frame* played animations
	// and movement in slow motion whenever the frame rate dropped. Button
	// edges belong to the first step only. Capped so a long stall can't
	// spiral into more work.
	// Debug: sim speed scales the step count (quarter steps accumulate, so
	// x0.25 runs one step every 4 vblanks); fixedStep off = old 1 step/frame.
	static uint32_t s_stepAcc;
	s_stepAcc += (dbg::opts.fixedStep ? dv : 1) * (uint32_t)dbg::opts.simSpeedQ;
	uint32_t steps = s_stepAcc / 4;
	s_stepAcc %= 4;
	if (steps > MAX_STEPS) steps = MAX_STEPS;
	for (uint32_t i = 0; i < steps; i++) {
		// Y on foot (not mid-jump / punch): CTaskComplexEnterCarAsDriver
		if (s_carTask == CT_NONE && s_action == ACT_NONE && pad::raw().enterExitJustDown && g.health > 0.0f) tryEnter();
		if (!(s_carTask != CT_NONE && carStep())) {
			updateMove();
			collide();
		}
		veh::step();
		if (s_carTask == CT_DRIVE) {   // the driver follows the car it just moved
			float seat[3], door[3];
			const veh::Vehicle* cv = veh::get(s_car);
			if (cv && carPoints(*cv, seat, door)) placeAt(*cv, seat);
		}
		if (i == 0) {
			input::consumeEdges();
			pad::consumeEdges();
		}
	}
	// WASTED, reduced: a few seconds, then back to the Grove Street spawn
	if (g.health <= 0.0f) {
		if (s_carTask != CT_NONE) endCarTask(true);
		// shot / starved / blown up on foot: CTaskComplexDie (a fall finishes first)
		if (s_action != ACT_DEAD && s_action != ACT_GLIDE) dieNow(0.0f);
		if ((s_wastedT += (float)dv / 60.0f) > 3.0f) {
			hud::log("wasted");
			s_wastedT = 0.0f;
			respawn();
			g.health = g.maxHealth;
			setWantedLevel(0);
		}
	} else {
		s_wastedT = 0.0f;
	}
	// render reference height: follows the feet, smoothed (stairs, jumps)
	s_viewZ = fabsf(g.z - s_viewZ) > 4.0f ? g.z : s_viewZ + (g.z - s_viewZ) * 0.25f;
	if (s_carTask != CT_NONE && s_carTask != CT_GOTO) {
		if (const veh::Vehicle* cv = veh::get(s_car)) updateCarCamera(*cv, s_camDrag);
	} else {
		s_lookH += (CAM_LOOK_H - s_lookH) * 0.2f;
	}
	updateCamera();

	// CWanted::Update: a chaos point goes every second nobody from the police is near
	if (s_chaos && (s_wantedMs += dv) >= 60) {
		s_wantedMs = 0;
		if (!peds::copNear(g.x, g.y, 18.0f)) {
			s_chaos--;
			updateWanted();
		}
	}

	const bool running = pedmove::isRunning(s_move) && !g.aiming;
	const bool sprinting = pedmove::isSprinting(s_move);
	exercise(sprinting, running, dv * (1000u / 60u));

	if (--s_zoneTimer <= 0) {
		s_zoneTimer = 30;
		g.zoneName = zones::nameAt(g.x, g.y);
	}
}

}
