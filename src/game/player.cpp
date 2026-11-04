#include "game/player.h"

#include <nds.h>
#include <math.h>
#include <string.h>

#include "game/cj.h"
#include "game/zones.h"
#include "hud/hud.h"
#include "platform/input.h"
#include "platform/timing.h"

// New-game snapshot (CPlayerInfo::Clear, CStats::Init, CClock::Initialise) plus
// the on-foot controls this port can honour:
//   D-pad  left stick, full deflection (blend eases up to a run)
//   B      sprint. Taps raise m_fMoveSpeed the way ControlButtonSprint does,
//          so a double tap outruns a held sprint. Energy is m_fTimeCanRun.
//   A      punch (fists / melee) or fire (a gun, once one is given)
//   X      jump (JUMP_launch / glide / land). Not while aiming, as in Zelda.
//   Y      enter/exit edge for the vehicle port. Nothing is spawned yet.
//   L      next occupied weapon slot (ProcessWeaponSwitch, one direction)
//   R      aim. Fists go to FIGHTIDLE; a gun free-aims along the camera.
//   touch  right stick: orbits the chase cam. There is no second stick.
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
static bool s_camDrag;
static int s_lastTx, s_lastTy;

static uint32_t s_lastVbl;
static uint32_t s_clockAcc;      // vblanks toward the next game minute
static int s_hoursNoEat;         // CPlayerInfo::m_nNumHoursDidntEat
static bool s_hungryTold;

enum Phase { PH_IDLE, PH_START, PH_MOVE, PH_STOP };
static Phase s_phase;
static float s_ratio;
static bool s_stopR;
static bool s_restartStart, s_restartStop;
static float s_wIdle, s_wWalk, s_wRun, s_wSprint, s_wStart, s_wStop, s_wTired;

enum Action { ACT_NONE, ACT_LAUNCH, ACT_GLIDE, ACT_LAND, ACT_PUNCH };
static Action s_action;
static int s_combo;
static float s_jumpVX, s_jumpVY, s_jumpVZ;
static int s_reload;             // frames left before the clip refills
static int s_fireCd;
static const char* k_punch[] = { "FightA_1", "FightA_2", "FightA_3" };

static constexpr float CAM_TIMESTEP = 0.5f; // CTimer is 1 at 30 fps; we step at 60
static constexpr float DT = 1.0f / 60.0f;
// GTA's on-foot jump is driven by ped physics, not just root motion in the
// animation. Keep a modest horizontal carry so a running D-pad jump clears
// ground rather than becoming a vertical hop. Units are SA world units/sec.
static constexpr float JUMP_UP_SPEED = 5.2f;
static constexpr float JUMP_GRAVITY = 16.0f;
static constexpr float JUMP_FORWARD_SPEED = 3.8f;

static float wrap(float a) {
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}

float camYaw() { return s_camYaw; }
float camPitch() { return s_camPitch; }
float groundZ() { return SPAWN_Z; }

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
	if (s.clip == 0 && s.total > 0) s_reload = 54; // ~0.9s, then the clip fills
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

static void tickClock(uint32_t vblanks) {
	s_clockAcc += vblanks;
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

static void approach(float& v, float target, float dt) {
	const float step = 8.0f * dt;
	if (v < target) v = v + step < target ? v + step : target;
	else if (v > target) v = v - step > target ? v - step : target;
}

static float clipOr(const char* name, float fallback) {
	const float p = cj::clipPhase(name);
	return p >= 0.0f ? p : fallback;
}

static void turnToward(float target, float rate) {
	float turn = wrap(target - g.heading);
	const float maxTurn = rate * DT;
	if (turn > maxTurn) turn = maxTurn;
	if (turn < -maxTurn) turn = -maxTurn;
	g.heading = wrap(g.heading + turn);
}

static void poseOne(const char* name, bool loop, bool extract, bool restart) {
	const cj::Clip c = { name, 1.0f, loop, extract, false, restart, 0.0f };
	cj::pose(&c, 1);
}

static void updateAction(bool jumpDown, bool attackDown, bool aiming, bool melee,
                         bool moveHeld) {
	if (s_reload > 0) {
		if (--s_reload == 0) refillClip();
	}
	if (s_fireCd > 0) s_fireCd--;

	if (s_action == ACT_PUNCH) {
		const char* name = k_punch[s_combo];
		const float ph = cj::clipPhase(name);
		if (attackDown && melee && ph >= 0.45f && s_combo < 2) {
			s_combo++;
			// Fight clips carry a sizeable root translation (the authored lunge).
			// There is no collision/target system yet to consume it, so extracting
			// it moves the whole ped a metre or two when the clip finishes.
			poseOne(k_punch[s_combo], false, false, true);
		} else if (ph < 0.0f || cj::clipEnded(name)) {
			s_action = ACT_NONE;
		} else {
			poseOne(name, false, false, false);
		}
		return;
	}
	if (s_action == ACT_LAUNCH) {
		g.x += s_jumpVX * DT;
		g.y += s_jumpVY * DT;
		g.z += s_jumpVZ * DT;
		s_jumpVZ -= JUMP_GRAVITY * DT;
		// Let go of the stick without making the jump stop dead in mid-air.
		const float drag = moveHeld ? 0.992f : 0.965f;
		s_jumpVX *= drag;
		s_jumpVY *= drag;
		if (cj::clipEnded("JUMP_launch") || cj::clipPhase("JUMP_launch") < 0.0f) {
			s_action = ACT_GLIDE;
			poseOne("JUMP_glide", true, false, true);
		} else {
			poseOne("JUMP_launch", false, false, false);
		}
		return;
	}
	if (s_action == ACT_GLIDE) {
		g.x += s_jumpVX * DT;
		g.y += s_jumpVY * DT;
		g.z += s_jumpVZ * DT;
		s_jumpVZ -= JUMP_GRAVITY * DT;
		s_jumpVX *= 0.965f;
		s_jumpVY *= 0.965f;
		if (g.z <= groundZ()) {
			s_action = ACT_LAND;
			g.z = groundZ();
			s_jumpVX = s_jumpVY = s_jumpVZ = 0.0f;
			// The horizontal jump carry above is authoritative. JUMP_land itself
			// has almost one world unit of authored root motion; extracting both
			// was the abrupt extra step at the end of every jump.
			poseOne("JUMP_land", false, false, true);
		} else {
			poseOne("JUMP_glide", true, false, false);
		}
		return;
	}
	if (s_action == ACT_LAND) {
		g.z = groundZ();
		if (cj::clipEnded("JUMP_land") || cj::clipPhase("JUMP_land") < 0.0f) s_action = ACT_NONE;
		else poseOne("JUMP_land", false, false, false);
		return;
	}

	if (jumpDown && !aiming) {
		s_action = ACT_LAUNCH;
		s_jumpVZ = JUMP_UP_SPEED;
		// Facing is (-sin(h), cos(h)). Starting a jump from a held direction
		// carries CJ forward; a standing jump stays vertical.
		if (moveHeld) {
			s_jumpVX = -sinf(g.heading) * JUMP_FORWARD_SPEED;
			s_jumpVY =  cosf(g.heading) * JUMP_FORWARD_SPEED;
		} else {
			s_jumpVX = s_jumpVY = 0.0f;
		}
		poseOne("JUMP_launch", false, false, true);
		return;
	}
	if (attackDown && melee) {
		s_action = ACT_PUNCH;
		s_combo = 0;
		poseOne(k_punch[0], false, false, true);
	}
}

static void updateMove() {
	const input::State& in = input::get();
	const bool jumpDown = (in.down & KEY_X) != 0;
	const bool attackDown = (in.down & KEY_A) != 0;
	const bool attackHeld = (in.held & KEY_A) != 0;
	const bool inAction = s_action != ACT_NONE;

	g.aiming = (in.held & KEY_R) && !inAction && !g.inVehicle;
	if (in.down & KEY_L) nextWeapon();
	const Slot& wep = s_slots[s_slot];
	const bool melee = wep.melee || wep.type == WEAPON_UNARMED;

	// Touch is the right stick. A drag orbits; it does not steer CJ.
	bool looking = false;
	if (in.touching) {
		if (s_camDrag) {
			s_camYaw = wrap(s_camYaw + (float)(in.touchX - s_lastTx) * 0.012f);
			s_camPitch += (float)(in.touchY - s_lastTy) * 0.010f;
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

	const float ix = (in.held & KEY_RIGHT ? 1.0f : 0.0f) - (in.held & KEY_LEFT ? 1.0f : 0.0f);
	const float iy = (in.held & KEY_UP ? 1.0f : 0.0f) - (in.held & KEY_DOWN ? 1.0f : 0.0f);
	const bool hasMoveInput = ix != 0.0f || iy != 0.0f;
	const bool moving = hasMoveInput && !inAction;

	float wish = g.heading;
	if (moving && !g.aiming) {
		const float dx = -sinf(s_camYaw) * iy + cosf(s_camYaw) * ix;
		const float dy =  cosf(s_camYaw) * iy + sinf(s_camYaw) * ix;
		wish = atan2f(-dx, dy);
		turnToward(wish, 6.0f);
	} else if (g.aiming) {
		turnToward(s_camYaw, 10.0f);
	}

	const bool wantSprint = moving && (in.held & KEY_B) && !g.aiming && !inAction;
	const bool tapSprint = moving && (in.down & KEY_B) && !g.aiming && !inAction;
	const float sprintR = controlSprint(wantSprint, tapSprint);
	const bool sprinting = sprintR >= 1.0f;

	const float gait = s_ratio;
	if (sprinting) s_ratio = 2.0f;
	else if (moving && !g.aiming) {
		// Full stick asks for a run (blend 2). It gets there at 0.07/timestep.
		const float target = 2.0f;
		const float step = 2.1f * DT;
		if (s_ratio < target) s_ratio = s_ratio + step < target ? s_ratio + step : target;
		else if (s_ratio > target) s_ratio = s_ratio - step > target ? s_ratio - step : target;
	} else if (!moving) {
		s_ratio = 0.0f;
	} else {
		// Aiming: walk pace only. The fight/gun shuffle carries the step.
		s_ratio = 1.0f;
	}

	recoverEnergy(moving && !sprinting && !g.aiming && s_ratio > 1.0f, sprinting);
	g.sprint = s_timeCanRun <= 0.0f ? 0.0f : s_timeCanRun * 100.0f / energyMax();
	if (g.sprint > 100.0f) g.sprint = 100.0f;

	if (!melee && !inAction) {
		const bool trigger = wep.automatic ? attackHeld : attackDown;
		const int gap = wep.automatic ? 5 : 16;
		if (trigger && s_fireCd == 0 && fireOne()) s_fireCd = gap;
	}

	updateAction(jumpDown && !g.aiming, attackDown, g.aiming, melee, hasMoveInput);
	if (s_action != ACT_NONE) {
		cj::advance(1);
		float mx, my;
		cj::rootDelta(mx, my);
		const float c = cosf(g.heading), s = sinf(g.heading);
		const float wx = mx * c - my * s;
		const float wy = mx * s + my * c;
		g.x += wx;
		g.y += wy;
		// Jump displacement is physics-driven (not root motion), so retain it
		// in the speed snapshot the radar uses while CJ is airborne.
		const float vx = wx / DT + s_jumpVX;
		const float vy = wy / DT + s_jumpVY;
		g.speed = sqrtf(vx * vx + vy * vy);
		if (!looking && !g.aiming) followCam(g.heading);
		else { s_betaSpeed = 0.0f; s_betaSign = 0; }
		return;
	}

	const cj::Motion mo = cj::motion();
	if (!g.aiming) {
		if (moving) {
			if (s_phase == PH_IDLE || s_phase == PH_STOP) {
				if (sprinting) s_phase = PH_MOVE;
				else { s_phase = PH_START; s_restartStart = true; }
			} else if (s_phase == PH_START &&
			           (sprinting || cj::clipEnded(mo.start) || cj::clipPhase(mo.start) < 0.0f)) {
				s_phase = PH_MOVE;
			}
		} else if (s_phase == PH_MOVE || s_phase == PH_START) {
			if (s_phase == PH_MOVE && gait > 1.0f) {
				float ph = clipOr(mo.run, -1.0f);
				if (s_wSprint >= s_wRun && s_wSprint >= s_wWalk) ph = clipOr(mo.sprint, ph);
				else if (s_wWalk >= s_wRun) ph = clipOr(mo.walk, ph);
				if (ph < 0.0f) ph = clipOr(mo.start, 0.0f);
				s_stopR = ph >= 0.5f;
				s_phase = PH_STOP;
				s_restartStop = true;
			} else {
				s_phase = PH_IDLE;
			}
		} else if (s_phase == PH_STOP) {
			const char* sn = s_stopR ? mo.stopR : mo.stop;
			if (cj::clipEnded(sn) || cj::clipPhase(sn) < 0.0f) s_phase = PH_IDLE;
		}

		float tIdle = 0, tWalk = 0, tRun = 0, tSprint = 0, tStart = 0, tStop = 0, tTired = 0;
		if (s_phase == PH_IDLE) {
			if (s_spent) tTired = 1.0f;
			else tIdle = 1.0f;
		} else if (s_phase == PH_START) {
			tStart = 1.0f;
		} else if (s_phase == PH_MOVE) {
			if (sprinting) tSprint = 1.0f;
			else if (s_ratio <= 1.0f) { tWalk = s_ratio; tIdle = 1.0f - s_ratio; }
			else { tWalk = 2.0f - s_ratio; tRun = s_ratio - 1.0f; }
		} else {
			tStop = 1.0f;
		}
		approach(s_wIdle, tIdle, DT);
		approach(s_wWalk, tWalk, DT);
		approach(s_wRun, tRun, DT);
		approach(s_wSprint, tSprint, DT);
		approach(s_wStart, tStart, DT);
		approach(s_wStop, tStop, DT);
		approach(s_wTired, tTired, DT);

		cj::Clip clips[7];
		int n = 0;
		const float walkPh = clipOr(mo.walk, 0.0f);
		auto add = [&](const char* name, float w, bool loop, bool extract, bool sync, bool restart, float phase) {
			if (w <= 0.01f || n >= 7) return;
			clips[n++] = { name, w, loop, extract, sync, restart, phase };
		};
		add(mo.idle, s_wIdle, true, false, false, false, 0.0f);
		add(mo.tired, s_wTired, true, false, false, false, 0.0f);
		add(mo.start, s_wStart, false, true, false, s_restartStart, 0.0f);
		add(mo.walk, s_wWalk, true, true, false, false, 0.0f);
		add(mo.run, s_wRun, true, true, s_wWalk > 0.05f, false, walkPh);
		add(mo.sprint, s_wSprint, true, true, false, false, 0.0f);
		add(s_stopR ? mo.stopR : mo.stop, s_wStop, false, true, false, s_restartStop, 0.0f);
		s_restartStart = s_restartStop = false;
		cj::pose(clips, n);
	} else {
		s_phase = PH_IDLE;
		s_wWalk = s_wRun = s_wSprint = s_wStart = s_wStop = s_wTired = 0.0f;
		s_wIdle = 0.0f;
		const char* pose = melee ? "FIGHTIDLE" : "IDLE_armed";
		if (moving) {
			// Stick relative to the aim heading: forward / back / strafe.
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
		poseOne(pose, true, moving, false);
	}

	cj::advance(1);
	float mx, my;
	cj::rootDelta(mx, my);
	if (sprinting && sprintR > 1.0f) {
		mx *= sprintR;
		my *= sprintR;
	}
	const float c = cosf(g.heading), s = sinf(g.heading);
	const float wx = mx * c - my * s;
	const float wy = mx * s + my * c;
	g.x += wx;
	g.y += wy;
	g.speed = sqrtf(wx * wx + wy * wy) / DT;

	if (!looking && !g.aiming &&
	    (moving || s_phase == PH_START || s_phase == PH_MOVE || s_phase == PH_STOP))
		followCam(g.heading);
	else { s_betaSpeed = 0.0f; s_betaSign = 0; }
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
	g.fat = 0.0f;
	g.muscle = 0.0f;
	g_clockH = 12; g_clockM = 0;
	g_clockDay = 1; g_clockMonth = 1;
	g_weekday = 4;                       // CClock::CurrentDay
	s_camYaw = 0;
	s_camPitch = 0.42f;
	s_betaSpeed = 0;
	s_betaSign = 0;
	s_zoneTimer = 0;
	s_camDrag = false;
	s_phase = PH_IDLE;
	s_ratio = 0;
	s_spent = false;
	s_stopR = false;
	s_restartStart = s_restartStop = false;
	s_wIdle = 1.0f;
	s_wWalk = s_wRun = s_wSprint = s_wStart = s_wStop = s_wTired = 0.0f;
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
	cj::init();
	cj::setStats(0.0f, 0.0f);
	hud::log("new game $%d fists wanted %d", g.money, g.wantedLevel);
}

void update() {
	const input::State& in = input::get();
	g.vehicleRequest = (in.down & KEY_Y) != 0;
	// inVehicle stays false until a car exists to enter. Y is the edge the
	// vehicle port will consume; pressing it on foot changes nothing else.

	const u32 v = timing::vblanks();
	uint32_t dv = v - s_lastVbl;
	s_lastVbl = v;
	if (dv > 6) dv = 6; // a stall shouldn't skip the clock or the exercise timers
	tickClock(dv);

	updateMove();

	const bool running = s_phase == PH_MOVE && s_wRun > s_wSprint && s_wRun > 0.4f && !g.aiming;
	const bool sprinting = s_wSprint > 0.5f;
	exercise(sprinting, running, dv * (1000u / 60u));

	if (--s_zoneTimer <= 0) {
		s_zoneTimer = 30;
		g.zoneName = zones::nameAt(g.x, g.y);
	}
}

}
