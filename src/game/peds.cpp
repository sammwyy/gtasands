#include "game/peds.h"

#include <initializer_list>
#include <math.h>
#include <nds.h>
#include <stdlib.h>
#include <string.h>

#include "audio/aepedaudio.h"
#include "audio/aeweaponaudio.h"
#include "game/collision.h"
#include "game/debugopts.h"
#include "game/melee.h"
#include "game/pedanim.h"
#include "game/peddata.h"
#include "game/pedmodel.h"
#include "game/pedmove.h"
#include "game/player.h"
#include "game/vehicle.h"
#include "game/weather.h"
#include "hud/hud.h"
#include "mem/pool.h"
#include "platform/timing.h"
#include "platform/video.h"

namespace peds {

static_assert(MAX == mem::budget::MAX_PEDS, "peds::MAX");

using peddata::PedType;

static constexpr int MAX_BONES = 40;
static constexpr int TARGET_NONE = -1, TARGET_PLAYER = -2;
static constexpr float DT = 1.0f / 60.0f;
static constexpr float PI = 3.14159265f;
// CPopulation::Update / ManagePed distances (PC: created 42.5..50.5 m away on
// screen, 15..25 m off screen, removed past 54.5 m, kept while within 25 m),
// scaled by 0.65: the DS crowd is 12 peds and a ped past ~35 m is a few pixels
// that cost the same polygons.
static constexpr float SPAWN_ON_MIN = 27.5f, SPAWN_ON_MAX = 33.0f;
static constexpr float SPAWN_OFF_MIN = 10.0f, SPAWN_OFF_MAX = 16.5f;
static constexpr float REMOVE_DIST = 35.5f, REMOVE_FAR = 48.0f, NEED_DIST = 16.0f;
static constexpr float DRAW_DIST = 46.0f;
static constexpr float COL_RANGE = 13.0f;    // inside the collision near list around CJ (GATHER_M 14)
static constexpr float PED_R = 0.35f;        // CTempColModels::ms_colModelPed1 sphere radius

// ----------------------------------------------------------------------------
// Ped
// ----------------------------------------------------------------------------
enum DamageKind : uint8_t { DMG_MELEE, DMG_BULLET, DMG_CAR, DMG_EXPLOSION };

struct Ped {
	uint8_t task, after;          // after: the task a reaction (hit, getup) hands back to
	uint8_t pedType, female, gang;   // gang: eGangID or 0xFF
	uint8_t weapon;               // eWeaponType in hand (0 = fists)
	uint8_t sub, combo;           // ATTACK: 0 approach, 1 strike, 2 recover
	bool struck, onScreen, poseValid, still, moneyGiven, near;
	int8_t model;                 // pedmodel handle
	int8_t target, leader;
	int16_t pedIndex;
	int32_t from, to;
	const ds::PedStat* stat;
	float x, y, z, heading;
	float vx, vy, vz;             // thrown (car hit, explosion)
	float health, lane;
	float tx, ty;                 // wander / follow target point
	float thx, thy;               // threat point (flee)
	float lastFoot, steer;
	int16_t avoidT;
	uint8_t acc;                  // ticks owed by the AI LOD (skipped frames)
	int32_t t, dur, cooldown, voiceCd;
	uint32_t needUntil, deathFrame;
	uint16_t money;
	uint8_t poseAge, surface;
	const char* clip;             // one-shot reaction clip
	const char* getup;            // the getup that follows the knock-down clip
	cj::Motion mo;
	pedmove::State move;
	pedanim::Set anim;
	s32 world[MAX_BONES * 12];
};

static mem::Pool<Ped> s_pool;
static Stats s_stats;
static uint32_t s_frame;
static bool s_inited;
static int s_driverModel = -1;
static bool s_driverLow;
static s32 s_driverWorld[2][MAX_BONES * 12];


// last frame's camera (spawn / removal visibility, CPed::GetIsOnScreen)
struct Cam { float ex, ey, ez, f[3], r[3], u[3]; bool valid; };
static Cam s_cam;

// CPopCycle state
struct Pop {
	int zoneType = 5, weekend = 0, slot = 6, worldZone = 0;
	const ds::PedZone* zone = nullptr;
	ds::PopRow row = {};
	float nOther = 0, nGang = 0, nCops = 0, nDealers = 0;
	int maxPeds = 0;
	uint8_t ok[64] = {};          // IsPedAppropriateForCurrentZone per peddata model (bitset)
};
static Pop s_pop;
static int s_numCiv, s_numGang[10], s_numGangTotal, s_numCops, s_numDealers, s_live;

// CStreaming ped models held for the population (one ref each)
static int8_t s_held[pedmodel::MAX];
static uint32_t s_heldFrame[pedmodel::MAX];
static uint8_t s_nextInGroup[peddata::NUM_PERC_GROUPS];

static const char* const k_taskNames[TASK_COUNT] = { "wander", "stand", "follow", "flee", "attack", "hit",
                                                     "fall", "getup", "dive", "cower", "handsup", "dead" };
const char* taskName(int t) { return (unsigned)t < TASK_COUNT ? k_taskNames[t] : "?"; }

static inline float frand() { return (float)(rand() & 0xFFFF) * (1.0f / 65536.0f); }
static inline float frand(float lo, float hi) { return lo + (hi - lo) * frand(); }
static inline int irand(int n) { return n > 0 ? rand() % n : 0; }

static float wrap(float a) {
	while (a > PI) a -= 2.0f * PI;
	while (a < -PI) a += 2.0f * PI;
	return a;
}

// Soft-float sinf / cosf / atan2f cost tens of microseconds each on the ARM9:
// libnds' sine table and a polynomial atan2 (max error ~0.005 rad) instead.
static inline float fsin(float a) { return (float)sinLerp((s16)(int)(a * 5215.19f)) * (1.0f / 4096.0f); }
static inline float fcos(float a) { return (float)cosLerp((s16)(int)(a * 5215.19f)) * (1.0f / 4096.0f); }
static float fatan2(float y, float x) {
	const float ax = fabsf(x), ay = fabsf(y);
	if (ax < 1e-6f && ay < 1e-6f) return 0.0f;
	const float a = ax > ay ? ay / ax : ax / ay, s = a * a;
	float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
	if (ay > ax) r = 1.57079637f - r;
	if (x < 0.0f) r = PI - r;
	return y < 0.0f ? -r : r;
}

static inline float headingTo(float dx, float dy) { return fatan2(-dx, dy); }   // 0 = north, CCW
static inline aeped::Pos voicePos(const Ped& p) { return { p.x, p.y, p.z + 1.0f }; }

static const mdl::Model* skel(const Ped& p) {
	const pedmodel::Model* m = pedmodel::get(p.model);
	return m ? m->skel : nullptr;
}

static bool isCop(const Ped& p) { return p.pedType == peddata::PT_COP; }
static bool alive(const Ped& p) { return p.health > 0.0f && p.task != TASK_DEAD; }

// ped.dat + the new-game script: gangs hate each other, Ballas / Vagos hate CJ
// (SET_RELATIONSHIP ... PEDTYPE_PLAYER1), Grove respect him, cops go for him
// while he is wanted.
static bool hatesPlayer(const Ped& p) {
	if (isCop(p)) return player::g.wantedLevel > 0;
	return p.pedType == peddata::PT_GANG1 || p.pedType == peddata::PT_GANG3;
}
static bool hates(const Ped& a, const Ped& b) { return a.gang != 0xFF && b.gang != 0xFF && a.gang != b.gang; }
static bool friendOfPlayer(const Ped& p) { return p.pedType == peddata::PT_GANG2; }

// weapon.dat damage (STD skill) for CJ's hits on peds; melee scaled like
// CTaskSimpleFight's per-move damage
static float weaponDamage(int type) {
	switch (type) {
	case player::WEAPON_UNARMED: return 8.0f;
	case player::WEAPON_BRASSKNUCKLE: return 12.0f;
	case player::WEAPON_KNIFE: return 35.0f;
	case player::WEAPON_KATANA: return 45.0f;
	case player::WEAPON_CHAINSAW: return 60.0f;
	case player::WEAPON_DILDO1: case player::WEAPON_DILDO2: case player::WEAPON_VIBE1: case player::WEAPON_VIBE2:
	case player::WEAPON_FLOWERS: return 4.0f;
	case player::WEAPON_PISTOL: return 25.0f;
	case player::WEAPON_PISTOL_SILENCED: return 40.0f;
	case player::WEAPON_DESERT_EAGLE: return 70.0f;
	case player::WEAPON_SHOTGUN: case player::WEAPON_SAWNOFF: return 45.0f;
	case player::WEAPON_SPAS12: return 40.0f;
	case player::WEAPON_MICRO_UZI: case player::WEAPON_TEC9: return 20.0f;
	case player::WEAPON_MP5: return 25.0f;
	case player::WEAPON_AK47: case player::WEAPON_M4: return 30.0f;
	case player::WEAPON_COUNTRYRIFLE: return 75.0f;
	case player::WEAPON_SNIPERRIFLE: return 125.0f;
	case player::WEAPON_MINIGUN: return 140.0f;
	default: return type <= player::WEAPON_CANE ? 20.0f : 25.0f;
	}
}

// ----------------------------------------------------------------------------
// Camera / visibility
// ----------------------------------------------------------------------------
static bool onScreen(float x, float y, float z, float r) {
	if (!s_cam.valid) return false;
	const float dx = x - s_cam.ex, dy = y - s_cam.ey, dz = z - s_cam.ez;
	const float zf = dx * s_cam.f[0] + dy * s_cam.f[1] + dz * s_cam.f[2];
	if (zf < -r) return false;
	const float xr = dx * s_cam.r[0] + dy * s_cam.r[1];
	if (fabsf(xr) > zf * video::TAN_HALF_FOV_X + r * 1.27f) return false;
	const float yu = dx * s_cam.u[0] + dy * s_cam.u[1] + dz * s_cam.u[2];
	return fabsf(yu) <= zf * video::TAN_HALF_FOV_Y + r * 1.16f;
}
bool visible(float x, float y, float z, float radius) { return !s_cam.valid || onScreen(x, y, z, radius); }

// ----------------------------------------------------------------------------
// CPopCycle
// ----------------------------------------------------------------------------
static bool raceAllowed(int race) { return race == 0 || !s_pop.zone || (s_pop.zone->races & (1 << (race - 1))); }
static bool appropriate(int pedIndex) { return pedIndex >= 0 && pedIndex < 512 && (s_pop.ok[pedIndex >> 3] & (1 << (pedIndex & 7))); }

// CPopCycle::Update + UpdatePercentages
static void updatePopCycle() {
	const int wd = player::g_weekday, h = (int)player::g_clockH;
	s_pop.weekend = wd == 7 || wd == 0 ? 1 : wd == 1 ? (h >= 20 ? 0 : 1) : wd == 6 ? (h >= 20 ? 1 : 0) : 0;
	s_pop.slot = h / 2;
	s_pop.zone = peddata::zoneAt(player::g.x, player::g.y);
	s_pop.zoneType = s_pop.zone ? s_pop.zone->popType : 5;
	const weather::Region rg = weather::g.region;
	s_pop.worldZone = rg == weather::REGION_SF ? 1 : rg == weather::REGION_LV ? 2 : 0;   // CPopulation::CurrentWorldZone
	if (!peddata::popRow(s_pop.zoneType, s_pop.weekend, s_pop.slot, s_pop.row)) s_pop.row = {};
	const ds::PedZone* z = s_pop.zone;
	int gangSum = 0;
	for (int g = 0; z && g < 10; g++) gangSum += z->gang[g];
	float pDealers = (z ? z->dealer : 0) / 100.0f;
	if (pDealers < 0.1f) pDealers = 0.1f;
	float pGangs = gangSum / 100.0f;
	if (pGangs > 0.5f) pGangs = 0.5f;
	float pCops = pGangs >= 0.15f ? fmaxf(0.03f, 0.3f - pGangs) : fmaxf(0.02f, pGangs);
	// (the original compares the zone type with ePopcyclePedGroup values here)
	switch (s_pop.zoneType) {
	case 4: case 14: case 16: pCops = fmaxf(pCops, 0.1f); break;
	case 5: pCops = fmaxf(pCops, 0.05f); break;
	case 8: case 17: pCops = 0.0f; break;
	default: break;
	}
	float pOther;
	const float sum = pDealers + pGangs + pCops;
	if (sum <= 1.0f) pOther = 1.0f - sum;
	else { pOther = 0.0f; pDealers /= sum; pGangs /= sum; pCops /= sum; }
	const ds::PopRow& r = s_pop.row;
	const float maxPeds = (float)r.maxPeds * (float)dbg::opts.pedDensity * 0.01f;
	// GetCurrentPercOther_Peds: rain keeps people indoors
	const float otherRain = (float)r.percOther * (1.0f - sqrtf(weather::g.rain) * 0.8f);
	s_pop.nDealers = maxPeds * (r.percDealers / 100.0f * pDealers);
	s_pop.nGang = maxPeds * (r.percGang / 100.0f * pGangs);
	s_pop.nCops = maxPeds * (r.percCops / 100.0f * pCops);
	s_pop.nOther = maxPeds * (pOther * otherRain / 100.0f);
	s_pop.maxPeds = (int)(maxPeds + 0.5f);
	if (s_pop.maxPeds > MAX) s_pop.maxPeds = MAX;
	// IsPedAppropriateForCurrentZone: members of the active groups, race allowed
	memset(s_pop.ok, 0, sizeof s_pop.ok);
	for (int g = 0; g < peddata::NUM_PERC_GROUPS; g++) {
		if (!r.group[g]) continue;
		const int pg = peddata::pedGroupOf(g, s_pop.worldZone);
		for (int k = 0; k < peddata::groupSize(pg); k++) {
			const int mi = peddata::groupMember(pg, k);
			const ds::PedModel* m = peddata::model(mi);
			if (m && mi < 512 && raceAllowed(m->race)) s_pop.ok[mi >> 3] |= (uint8_t)(1 << (mi & 7));
		}
	}
	s_stats.zoneType = (uint8_t)s_pop.zoneType;
	s_stats.weekend = (uint8_t)s_pop.weekend;
	s_stats.slot = (uint8_t)s_pop.slot;
	s_stats.maxPeds = (uint16_t)s_pop.maxPeds;
}

// CPopCycle::PickARandomGroupOfOtherPeds
static int pickOtherGroup() {
	int total = 0;
	for (int g = 0; g < peddata::NUM_PERC_GROUPS; g++) total += s_pop.row.group[g];
	if (total <= 0) return -1;
	int r = irand(total);
	for (int g = 0; g < peddata::NUM_PERC_GROUPS; g++) {
		if (r < s_pop.row.group[g]) return g;
		r -= s_pop.row.group[g];
	}
	return -1;
}

// CPopCycle::PickPedMIToStreamInForCurrentZone
static int pickModelToStream() {
	for (int tr = 0; tr < 10; tr++) {
		const int g = pickOtherGroup();
		if (g < 0) return -1;
		const int pg = peddata::pedGroupOf(g, s_pop.worldZone);
		const int n = peddata::groupSize(pg);
		for (int k = 0; k < n; k++) {
			s_nextInGroup[g] = (uint8_t)((s_nextInGroup[g] + 1) % n);
			const int mi = peddata::groupMember(pg, s_nextInGroup[g]);
			const ds::PedModel* m = peddata::model(mi);
			if (m && pedmodel::find(mi) < 0 && raceAllowed(m->race)) return mi;
		}
	}
	return -1;
}

enum Cat : uint8_t { CAT_CIV, CAT_GANG, CAT_COP, CAT_DEALER };
static Cat catOf(int pedIndex) {
	const ds::PedModel* m = peddata::model(pedIndex);
	if (!m) return CAT_CIV;
	if (peddata::isGang(m->pedType)) return CAT_GANG;
	if (m->pedType == peddata::PT_COP) return CAT_COP;
	if (m->pedType == peddata::PT_DEALER) return CAT_DEALER;
	return CAT_CIV;
}

// The population's wanted ped models (CStreaming::StreamZoneModels): the
// strongest gangs of the zone, a cop while one may spawn, civilians from the
// zone's groups; one swap every 20 s so the street changes faces.
static void streamModels() {
	const ds::PedZone* z = s_pop.zone;
	bool needCop = s_pop.nCops >= 0.5f || player::g.wantedLevel > 0;
	int gangs[2] = { -1, -1 };
	for (int g = 0; z && g < 10; g++) {
		if (!z->gang[g]) continue;
		if (gangs[0] < 0 || z->gang[g] > z->gang[gangs[0]]) { gangs[1] = gangs[0]; gangs[0] = g; }
		else if (gangs[1] < 0 || z->gang[g] > z->gang[gangs[1]]) gangs[1] = g;
	}
	// 1. let go of what this zone / time does not want
	int held = 0, oldestCiv = -1;
	bool haveGang[2] = { false, false }, haveCop = false;
	for (int k = 0; k < pedmodel::MAX; k++) {
		if (s_held[k] < 0) continue;
		const pedmodel::Model* m = pedmodel::get(s_held[k]);
		if (!m || m->state == pedmodel::STATE_FAILED) {
			pedmodel::release(s_held[k]);
			s_held[k] = -1;
			continue;
		}
		const ds::PedModel* pm = peddata::model(m->pedIndex);
		const Cat c = catOf(m->pedIndex);
		bool keep = true;
		if (c == CAT_GANG) {
			const int g = pm->pedType - peddata::PT_GANG1;
			keep = g == gangs[0] || g == gangs[1];
			if (g == gangs[0]) haveGang[0] = true;
			if (g == gangs[1]) haveGang[1] = true;
		} else if (c == CAT_COP) {
			keep = needCop;
			haveCop |= keep;
		} else if (c == CAT_DEALER) {
			keep = s_pop.nDealers >= 0.5f;
		} else {
			keep = appropriate(m->pedIndex);
			if (keep && (oldestCiv < 0 || s_heldFrame[k] < s_heldFrame[oldestCiv])) oldestCiv = k;
		}
		if (!keep) {
			pedmodel::release(s_held[k]);
			s_held[k] = -1;
			continue;
		}
		held++;
	}
	// 2. rotation: the longest-held civilian makes room now and then
	if (held == pedmodel::MAX && oldestCiv >= 0 && s_frame - s_heldFrame[oldestCiv] > 20 * 60) {
		pedmodel::release(s_held[oldestCiv]);
		s_held[oldestCiv] = -1;
		held--;
	}
	if (held >= pedmodel::MAX) return;
	// 3. one request per call: gangs, the cop, then civilians
	int want = -1;
	for (int i = 0; i < 2 && want < 0; i++) {
		if (gangs[i] < 0 || haveGang[i]) continue;
		const int pg = peddata::GROUP_BALLAS + gangs[i];
		const int n = peddata::groupSize(pg);
		if (n) want = peddata::groupMember(pg, irand(n));
	}
	if (want < 0 && needCop && !haveCop) {
		static const int k_cops[3] = { 280, 281, 282 };   // lapd1 / sfpd1 / lvpd1 (ChoosePolicePedOccupation)
		want = peddata::findModel(k_cops[s_pop.worldZone]);
	}
	if (want < 0) want = pickModelToStream();
	if (want < 0) return;
	const int h = pedmodel::request(want);
	if (h < 0) return;
	for (int k = 0; k < pedmodel::MAX; k++)
		if (s_held[k] < 0) {
			s_held[k] = (int8_t)h;
			s_heldFrame[k] = s_frame;
			return;
		}
	pedmodel::release(h);
}

// ----------------------------------------------------------------------------
// Path following (CTaskComplexWander / CPathFind)
// ----------------------------------------------------------------------------
static void setTarget(Ped& p) {
	const ds::PathNode* a = peddata::node(p.from);
	const ds::PathNode* b = peddata::node(p.to);
	if (!b) return;
	float px = 0.0f, py = 0.0f;
	if (a) {
		const float sx = peddata::nodeX(b) - peddata::nodeX(a), sy = peddata::nodeY(b) - peddata::nodeY(a);
		const float l = sqrtf(sx * sx + sy * sy);
		if (l > 0.01f) { px = sy / l; py = -sx / l; }   // right of the walking direction
	}
	p.tx = peddata::nodeX(b) + px * p.lane;
	p.ty = peddata::nodeY(b) + py * p.lane;
}

// CPathFind::FindNextNodeWandering, reduced: any link but the way back
// (unless it is a dead end), switched-off nodes and don't-wander ones avoided.
static int nextNode(int from, int at) {
	const ds::PathNode* n = peddata::node(at);
	if (!n) return -1;
	int cand[16], nc = 0, back = -1;
	for (int k = 0; k < n->numLinks && nc < 16; k++) {
		const int l = peddata::link(n, k);
		const ds::PathNode* ln = peddata::node(l);
		if (!ln || (ln->flags & ds::PATHNODE_SWITCHED_OFF)) continue;
		if (l == from) { back = l; continue; }
		if ((ln->flags & ds::PATHNODE_DONT_WANDER) && irand(4)) continue;
		cand[nc++] = l;
	}
	return nc ? cand[irand(nc)] : back;
}

static float pathZ(const Ped& p) {
	const ds::PathNode* a = peddata::node(p.from);
	const ds::PathNode* b = peddata::node(p.to);
	if (!a || !b) return p.z;
	const float ax = peddata::nodeX(a), ay = peddata::nodeY(a);
	const float sx = peddata::nodeX(b) - ax, sy = peddata::nodeY(b) - ay;
	const float l2 = sx * sx + sy * sy;
	float t = l2 > 0.01f ? ((p.x - ax) * sx + (p.y - ay) * sy) / l2 : 1.0f;
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return peddata::nodeZ(a) + (peddata::nodeZ(b) - peddata::nodeZ(a)) * t;
}

// ----------------------------------------------------------------------------
// Tasks
// ----------------------------------------------------------------------------
static void setTask(Ped& p, uint8_t task, int32_t dur = 0) {
	const bool wasAction = p.task >= TASK_HIT;
	p.task = task;
	p.t = 0;
	p.dur = dur;
	p.sub = 0;
	p.struck = false;
	p.still = false;
	if (task <= TASK_ATTACK && wasAction) pedmove::reset(p.move);   // locomotion restarts from idle
}

// back on the path graph from wherever a flee / fight left the ped
static void resumeWander(Ped& p) {
	p.target = TARGET_NONE;
	const int n = peddata::nearestNode(p.x, p.y, p.z, 30.0f);
	if (n < 0) { setTask(p, TASK_STAND, 600); return; }
	p.from = n;
	const int to = nextNode(-1, n);
	p.to = to >= 0 ? to : n;
	setTarget(p);
	if (p.to == p.from) {   // a lone node: walk onto it and stand
		const ds::PathNode* a = peddata::node(n);
		p.tx = peddata::nodeX(a);
		p.ty = peddata::nodeY(a);
	}
	setTask(p, p.leader >= 0 ? TASK_FOLLOW : TASK_WANDER);
}

static void startAttack(Ped& p, int target) {
	if (!alive(p)) return;
	p.target = (int8_t)target;
	p.leader = -1;
	if (p.task == TASK_HIT || p.task == TASK_FALL || p.task == TASK_GETUP || p.task == TASK_DIVE) { p.after = TASK_ATTACK; return; }
	if (p.task != TASK_ATTACK) setTask(p, TASK_ATTACK);
}

// the reactions themselves hand over with fleeNow; events arriving during one
// only change what follows it
static void fleeNow(Ped& p, float fx, float fy, int32_t dur) {
	p.thx = fx;
	p.thy = fy;
	p.leader = -1;
	const bool already = p.task == TASK_FLEE;
	setTask(p, TASK_FLEE, dur);
	p.from = peddata::nearestNode(p.x, p.y, p.z, 10.0f);
	p.to = -1;
	if (!already && p.voiceCd <= 0 && irand(2)) {
		aeped::sayPedPain(p.female, aeped::PAIN_PED_PANIC, voicePos(p));
		p.voiceCd = 240;
	}
}

static void startFlee(Ped& p, float fx, float fy, int32_t dur) {
	if (!alive(p)) return;
	if (p.task == TASK_HIT || p.task == TASK_FALL || p.task == TASK_GETUP || p.task == TASK_DIVE) {
		p.thx = fx;
		p.thy = fy;
		p.after = TASK_FLEE;
		return;
	}
	fleeNow(p, fx, fy, dur);
}

static Ped* pedAt(int i) { return s_pool.at(i); }
static int indexOf(const Ped& p) { return s_pool.indexOf(&p); }

static bool targetPos(const Ped& p, float& x, float& y, float& z) {
	if (p.target == TARGET_PLAYER) {
		x = player::g.x; y = player::g.y; z = player::g.z;
		return player::g.health > 0.0f;
	}
	const Ped* o = pedAt(p.target);
	if (!o || !alive(*o)) return false;
	x = o->x; y = o->y; z = o->z;
	return true;
}

// The gang members around a victim join in (CEventGroupEvent / ped
// scanner "gang member attacked"), civilians who see a death run.
static void alarmOthers(const Ped& victim, int attacker, float ax, float ay, bool died) {
	const int vi = indexOf(victim);
	for (int i = 0; i < MAX; i++) {
		Ped* o = pedAt(i);
		if (!o || i == vi || !alive(*o) || i == attacker) continue;
		const float dx = o->x - victim.x, dy = o->y - victim.y;
		const float d2 = dx * dx + dy * dy;
		if (victim.gang != 0xFF && o->gang == victim.gang && d2 < 15.0f * 15.0f) startAttack(*o, attacker);
		else if (died && o->gang == 0xFF && !isCop(*o) && d2 < 14.0f * 14.0f && o->task != TASK_ATTACK)
			startFlee(*o, ax, ay, 420 + irand(300));
	}
}

static void attackerPos(int attacker, float& x, float& y) {
	if (attacker == TARGET_PLAYER) { x = player::g.x; y = player::g.y; return; }
	const Ped* o = pedAt(attacker);
	if (o) { x = o->x; y = o->y; }
}

// CPed::InflictDamage + CPedDamageResponseCalculator + the reactions of
// CEventDamage (decision makers reduced to the ped type and its stats).
// `move`: the melee.dat move of a melee hit (-1 otherwise).
static void damage(Ped& p, float dmg, float fx, float fy, DamageKind kind, int weapon, int attacker, bool knockDown,
                   float vx = 0.0f, float vy = 0.0f, int move = -1) {
	if (!alive(p)) return;
	p.health -= dmg;
	const float rel = wrap(headingTo(fx - p.x, fy - p.y) - p.heading);   // 0 = attacker in front, > 0 on the left
	const bool front = fabsf(rel) < PI * 0.25f, back = fabsf(rel) > PI * 0.75f;
	const aeped::Pos vp = voicePos(p);
	if (kind == DMG_BULLET) aeped::bulletHitPed(vp);
	else if (kind == DMG_MELEE || kind == DMG_CAR) aeped::pedHit(vp);
	if (attacker == TARGET_PLAYER && kind != DMG_CAR && kind != DMG_EXPLOSION) {
		const bool blade = weapon == player::WEAPON_KNIFE || weapon == player::WEAPON_KATANA;
		player::registerCrime(isCop(p) ? (blade ? player::CRIME_STAB_COP : player::CRIME_DAMAGED_COP)
		                               : (blade ? player::CRIME_STAB_PED : player::CRIME_DAMAGED_PED));
	}
	p.vx = vx;
	p.vy = vy;
	p.vz = kind == DMG_CAR ? 2.5f + sqrtf(vx * vx + vy * vy) * 0.15f : kind == DMG_EXPLOSION ? 6.0f : 0.0f;
	if (p.health <= 0.0f) {
		// CTaskComplexDie: KO_* by the weapon and the side it came from
		p.health = 0.0f;
		if (kind == DMG_BULLET) p.clip = front ? (irand(2) ? "KO_shot_front" : "KO_shot_stom") : back ? "KO_skid_back" : rel > 0.0f ? "KO_spin_L" : "KO_spin_R";
		else p.clip = front ? "KO_skid_front" : back ? "KO_skid_back" : rel > 0.0f ? "KO_spin_L" : "KO_spin_R";
		if (move == melee::MOVE_GROUND) p.clip = p.getup && !strcmp(p.getup, "getup_front") ? "FLOOR_hit_f" : "FLOOR_hit";
		aeped::sayPedPain(p.female, kind >= DMG_CAR ? aeped::PAIN_PED_DEATH_HIGH : aeped::PAIN_PED_DEATH_LOW, vp);
		p.deathFrame = s_frame;
		p.target = TARGET_NONE;
		setTask(p, TASK_FALL);
		if (attacker == TARGET_PLAYER && kind == DMG_CAR) player::registerCrime(player::CRIME_KILL_PED_WITH_CAR);
		float ax = fx, ay = fy;
		attackerPos(attacker, ax, ay);
		alarmOthers(p, attacker, ax, ay, true);
		return;
	}
	if (p.voiceCd <= 0) {
		aeped::sayPedPain(p.female, aeped::PAIN_PED_GENERIC, vp);
		p.voiceCd = 90;
	}
	// the response once the reaction is over
	uint8_t after = TASK_FLEE;
	if (attacker != TARGET_NONE && (p.gang != 0xFF || isCop(p) ||
	                                (kind == DMG_MELEE && p.stat && p.stat->temper + irand(40) > p.stat->fear + 30))) {
		after = TASK_ATTACK;
		p.target = (int8_t)attacker;
	}
	attackerPos(attacker, p.thx, p.thy);
	if (attacker == TARGET_NONE) { p.thx = fx; p.thy = fy; }
	if (move == melee::MOVE_GROUND) {
		// kicked while down (AGROUND): FLOOR_hit on the back, _f face down; stays down
		p.clip = p.getup && !strcmp(p.getup, "getup_front") ? "FLOOR_hit_f" : "FLOOR_hit";
		setTask(p, TASK_FALL);
	} else if (knockDown || kind == DMG_CAR || kind == DMG_EXPLOSION) {
		// CTaskComplexFallAndGetUp
		if (kind == DMG_CAR && !front && !back) { p.clip = rel > 0.0f ? "KD_left" : "KD_right"; p.getup = "getup"; }
		else if (back) { p.clip = "KO_skid_back"; p.getup = "getup_front"; }
		else { p.clip = "KO_skid_front"; p.getup = "getup"; }
		setTask(p, TASK_FALL);
	} else {
		// CTaskSimpleBeHit::StartAnim: the attacker's fist move from the front
		// (HitA_1..3), else HIT_front / back / L / R
		p.clip = front ? (move >= 0 ? melee::frontHitClip(move) : "HIT_front") : back ? "HIT_back" : rel > 0.0f ? "HIT_L" : "HIT_R";
		setTask(p, TASK_HIT);
	}
	p.after = after;
	p.dur = 480 + irand(360);   // flee time
	p.leader = -1;
	float ax = fx, ay = fy;
	attackerPos(attacker, ax, ay);
	alarmOthers(p, attacker, ax, ay, false);
}

static void damageTarget(Ped& p, float dmg, DamageKind kind, int move = -1) {
	if (p.target == TARGET_PLAYER) {
		if (kind == DMG_MELEE) {
			player::meleeHitBy(p.x, p.y, move, dmg);
		} else {
			player::takeDamage(dmg);
			aeped::bulletHitPed({ player::g.x, player::g.y, player::g.z + 1.0f });
		}
		return;
	}
	if (Ped* o = pedAt(p.target))
		damage(*o, dmg, p.x, p.y, kind, p.weapon, indexOf(p), move >= 0 && melee::knocksDown(move), 0.0f, 0.0f, move);
}

// ----------------------------------------------------------------------------
// Movement + pose
// ----------------------------------------------------------------------------
static const pedmove::PoseApi k_api = { pedanim::apiPose, pedanim::apiPhase, pedanim::apiEnded };

static void applyRoot(Ped& p) {
	const float c = fcos(p.heading), s = fsin(p.heading);
	p.x += p.anim.rootX * c - p.anim.rootY * s;
	p.y += p.anim.rootX * s + p.anim.rootY * c;
}

// SetRealMoveAnim for an AI ped: the move blend chases `blend` (walk 1, run
// 2) at PlayerControlZelda's rate, the motion group's clips do the rest.
static void locomote(Ped& p, float want, float blend, bool sprint, int dt) {
	const mdl::Model* sk = skel(p);
	pedmove::turn(p.heading, want, DT * (float)dt, false);
	p.move.prevBlend = p.move.moveBlend;
	const float step = 0.035f * (float)dt;
	if (p.move.moveBlend < blend) p.move.moveBlend = fminf(blend, p.move.moveBlend + step);
	else p.move.moveBlend = blend <= 0.0f ? 0.0f : fmaxf(blend, p.move.moveBlend - step);
	p.move.moveState = p.move.moveBlend <= 0.0f ? pedmove::PEDMOVE_NONE : p.move.moveBlend > 1.0f ? pedmove::PEDMOVE_RUN : pedmove::PEDMOVE_WALK;
	pedanim::bind(&p.anim, sk, dt);
	pedmove::setRealMoveAnim(p.move, p.mo, sprint && blend > 0.0f, false, DT * (float)dt, k_api);
	pedanim::advance(p.anim, sk, dt);
	applyRoot(p);
}

static void oneShot(Ped& p, const char* clip, bool loop, bool extract, int dt) {
	const mdl::Model* sk = skel(p);
	const bool restart = p.t == 0;
	const cj::Clip c = { clip, 1.0f, loop, extract, false, restart, 0.0f };
	pedmove::suspend(p.move);
	pedanim::pose(p.anim, sk, &c, 1, dt);
	pedanim::advance(p.anim, sk, dt);
	if (extract) applyRoot(p);
}

static bool clipDone(const Ped& p, const char* clip) {
	return pedanim::ended(p.anim, clip) || (!pedanim::playing(p.anim, clip) && p.t > 20);
}

// CTaskComplexAvoidOtherPedWhileWandering, reduced: turn away from a ped / CJ
// standing in the way.
static float avoid(Ped& p, float want, int dt) {
	if ((p.avoidT -= dt) > 0) return want + p.steer;
	p.avoidT = 8;   // re-scanned every 8 ticks: the steer holds in between
	const float fx = -fsin(want), fy = fcos(want);
	float steer = 0.0f;
	auto test = [&](float ox, float oy) {
		const float dx = ox - p.x, dy = oy - p.y;
		const float along = dx * fx + dy * fy, side = dx * fy - dy * fx;
		if (along > 0.1f && along < 1.6f && fabsf(side) < 0.75f) steer += side >= 0.0f ? 0.7f : -0.7f;
	};
	if (!player::g.inVehicle) test(player::g.x, player::g.y);
	for (int i = 0; i < MAX; i++) {
		const Ped* o = pedAt(i);
		if (o && o != &p && alive(*o)) test(o->x, o->y);
	}
	p.steer = steer > 0.9f ? 0.9f : steer < -0.9f ? -0.9f : steer;
	return want + p.steer;
}

static void fireAt(Ped& p, float tx, float ty, float tz, float d) {
	aeweapon::fire(p.weapon, p.x, p.y, p.z + 1.2f);
	// CWeapon::FireInstantHit for a ped: worse with distance, blocked by the map
	float chance = 0.75f - d * 0.025f;
	chance = chance < 0.15f ? 0.15f : chance > 0.7f ? 0.7f : chance;
	if (frand() > chance) return;
	if (p.near && col::available()) {
		const float a[3] = { p.x, p.y, p.z + 1.3f }, b[3] = { tx, ty, tz + 1.1f };
		col::Hit h;
		if (col::castSegment(a, b, h)) return;
	}
	const float dmg = p.weapon == player::WEAPON_MICRO_UZI || p.weapon == player::WEAPON_TEC9 ? 5.0f
	                  : p.weapon == player::WEAPON_DESERT_EAGLE ? 14.0f : p.weapon == player::WEAPON_AK47 ? 9.0f : 8.0f;
	damageTarget(p, dmg, DMG_BULLET);
}

static void taskStep(Ped& p, int dt) {
	switch (p.task) {
	case TASK_WANDER: {
		const float dx = p.tx - p.x, dy = p.ty - p.y;
		if (dx * dx + dy * dy < 0.5f) {
			const int n = nextNode(p.from, p.to);
			if (n < 0) { setTask(p, TASK_STAND, 300); break; }
			p.from = p.to;
			p.to = n;
			setTarget(p);
		}
		locomote(p, avoid(p, headingTo(p.tx - p.x, p.ty - p.y), dt), 1.0f, false, dt);
		// CTaskComplexWanderStandard: now and then stop for a while
		if (p.t > 600 && irand(5000) < dt) setTask(p, TASK_STAND, 120 + irand(240));
		break;
	}
	case TASK_STAND:
		locomote(p, p.heading, 0.0f, false, dt);
		if (p.t >= p.dur) setTask(p, p.leader >= 0 ? TASK_FOLLOW : TASK_WANDER);
		break;
	case TASK_FOLLOW: {
		// CTaskComplexFollowLeaderInFormation: beside / behind the leader
		const Ped* l = pedAt(p.leader);
		if (!l || !alive(*l) || l->task > TASK_FOLLOW) { p.leader = -1; resumeWander(p); break; }
		const float side = (indexOf(p) & 1) ? 1.1f : -1.1f;
		const float lfx = -fsin(l->heading), lfy = fcos(l->heading);
		const float gx = l->x + lfy * side - lfx * 0.4f, gy = l->y - lfx * side - lfy * 0.4f;
		const float dx = gx - p.x, dy = gy - p.y, d = sqrtf(dx * dx + dy * dy);
		const float lb = l->move.moveBlend;
		locomote(p, d > 0.5f ? headingTo(dx, dy) : l->heading, d > 3.0f ? 1.8f : d > 0.5f ? fmaxf(lb, 1.0f) : lb, false, dt);
		p.from = l->from;
		p.to = l->to;
		break;
	}
	case TASK_FLEE: {
		const float ax = p.x - p.thx, ay = p.y - p.thy;
		float want = headingTo(ax, ay);
		const ds::PathNode* target = peddata::node(p.to);
		if (target && (p.x - p.tx) * (p.x - p.tx) + (p.y - p.ty) * (p.y - p.ty) < 2.0f * 2.0f) {
			p.from = p.to;
			p.to = -1;
			target = nullptr;
		}
		if (!target && p.from >= 0) {
			const ds::PathNode* n = peddata::node(p.from);
			int best = -1;
			float score = -1e10f;
			for (int k = 0; n && k < n->numLinks; k++) {
				const int to = peddata::link(n, k);
				const ds::PathNode* next = peddata::node(to);
				if (!next || (next->flags & ds::PATHNODE_SWITCHED_OFF)) continue;
				const float dx = peddata::nodeX(next) - p.x, dy = peddata::nodeY(next) - p.y;
				const float length = sqrtf(dx * dx + dy * dy);
				if (length < 1.0f) continue;
				const float heading = (dx * ax + dy * ay) / (length * fmaxf(1.0f, sqrtf(ax * ax + ay * ay)));
				const float forward = (-fsin(p.heading) * dx + fcos(p.heading) * dy) / length;
				const float rank = heading * 2.0f + forward + frand() * 0.3f;
				if (rank > score) { score = rank; best = to; }
			}
			p.to = best;
			if (best >= 0) setTarget(p);
			target = peddata::node(best);
		}
		if (target) want = headingTo(p.tx - p.x, p.ty - p.y);
		if (p.sub) want += p.sub == 1 ? 0.8f : -0.8f;
		locomote(p, avoid(p, want, dt), 2.0f, true, dt);
		const float flee = p.stat ? (float)p.stat->fleeDistance : 15.0f;
		if (p.t >= p.dur || ax * ax + ay * ay > (flee + 20.0f) * (flee + 20.0f)) resumeWander(p);
		break;
	}
	case TASK_ATTACK: {
		float tx, ty, tz;
		if (!targetPos(p, tx, ty, tz) || (p.target == TARGET_PLAYER && !hatesPlayer(p) && isCop(p))) { resumeWander(p); break; }
		const float dx = tx - p.x, dy = ty - p.y, d = sqrtf(dx * dx + dy * dy);
		if (d > 40.0f || (p.t > 900 && d > 25.0f)) { resumeWander(p); break; }
		const bool targetInCar = p.target == TARGET_PLAYER && player::g.inVehicle;
		const float want = headingTo(dx, dy);
		if (p.weapon >= player::WEAPON_PISTOL) {
			if (d > 18.0f) { locomote(p, want, 2.0f, false, dt); break; }
			pedmove::turn(p.heading, want, DT * (float)dt, true);
			oneShot(p, "Gun_stand", true, false, dt);
			if ((p.cooldown -= dt) <= 0 && fabsf(wrap(want - p.heading)) < 0.3f) {
				fireAt(p, tx, ty, tz, d);
				p.cooldown = 45 + irand(50);
			}
			break;
		}
		if (targetInCar) {   // fists vs a car: keep away, give up after a while
			if (p.t > 300) resumeWander(p);
			else locomote(p, want, 0.0f, false, dt);
			break;
		}
		// CTaskComplexKillPedOnFootMelee + CTaskSimpleFight::ChooseAttackAI with
		// melee.dat's UNARMED_1: close in to RANGES, then a FightA_1 -> 2 -> 3
		// combo (each move chained at its chain frame, the strike at its hit
		// frame), a FIGHTIDLE pause, again; a target on the floor gets FightA_G
		const bool down = p.target == TARGET_PLAYER ? player::isDown()
		                                            : pedAt(p.target) && (pedAt(p.target)->task == TASK_FALL || pedAt(p.target)->task == TASK_GETUP);
		if (p.sub == 0) {
			if (d > melee::RANGE - 0.4f) { locomote(p, avoid(p, want, dt), d > 4.0f ? 2.0f : 1.2f, false, dt); break; }
			p.sub = 1;
			p.combo = down ? melee::MOVE_GROUND : melee::MOVE_HIT_1;
			p.struck = false;
			p.t = 0;
		}
		if (p.sub == 1) {
			const melee::Info& mv = melee::k_unarmed[p.combo];
			pedmove::turn(p.heading, want, DT * (float)dt, true);
			oneShot(p, mv.clip, false, true, dt);
			const int at = pedanim::time(p.anim, mv.clip);
			if (!p.struck && at >= melee::hitTicks(p.combo)) {
				p.struck = true;
				aeped::swing(player::WEAPON_UNARMED, voicePos(p));
				const bool kick = p.combo == melee::MOVE_GROUND;
				if (d < melee::REACH + 0.15f && fabsf(wrap(want - p.heading)) < 0.8f && kick == down) {
					// GetStrikeDamage: the move's damage by the ped's attack strength
					const float strength = p.stat ? p.stat->attackStrength / 20.0f : 1.0f;
					damageTarget(p, (float)mv.damage * strength, DMG_MELEE, p.combo);
				}
			}
			const bool ended = clipDone(p, mv.clip);
			const bool more = p.combo < melee::MOVE_HIT_3 && !down && d < melee::RANGE;
			if (p.struck && more && (at >= melee::chainTicks(p.combo) || ended)) {
				p.combo++;
				p.struck = false;
				p.t = 0;
			} else if (ended) {
				p.sub = 2;
				p.t = 0;
				p.cooldown = 20 + irand(50);
			}
			break;
		}
		pedmove::turn(p.heading, want, DT * (float)dt, true);
		oneShot(p, "FIGHTIDLE", true, false, dt);
		if ((p.cooldown -= dt) <= 0) { p.sub = 0; p.t = 0; }
		break;
	}
	case TASK_HIT:
		oneShot(p, p.clip, false, false, dt);
		if (clipDone(p, p.clip) || p.t > 90) {
			if (p.after == TASK_ATTACK) setTask(p, TASK_ATTACK);
			else fleeNow(p, p.thx, p.thy, 480 + irand(360));
		}
		break;
	case TASK_FALL:
		oneShot(p, p.clip, false, false, dt);
		if (p.vz == 0.0f && clipDone(p, p.clip)) {
			if (p.health <= 0.0f) { setTask(p, TASK_DEAD); p.still = true; break; }
			if (p.t > 160 + irand(40)) setTask(p, TASK_GETUP);
		}
		break;
	case TASK_GETUP:
		oneShot(p, p.getup ? p.getup : "getup", false, false, dt);
		if (clipDone(p, p.getup ? p.getup : "getup")) {
			if (p.after == TASK_ATTACK) setTask(p, TASK_ATTACK);
			else if (p.after == TASK_FLEE) fleeNow(p, p.thx, p.thy, 480 + irand(360));
			else resumeWander(p);
		}
		break;
	case TASK_DIVE:
		oneShot(p, p.clip, false, true, dt);
		if (clipDone(p, p.clip)) {
			if (!strcmp(p.clip, "EV_dive")) { p.getup = "getup_front"; p.after = TASK_FLEE; setTask(p, TASK_GETUP); }
			else resumeWander(p);
		}
		break;
	case TASK_COWER:
		oneShot(p, "DUCK_cower", true, false, dt);
		if (p.t >= p.dur) fleeNow(p, p.thx, p.thy, 420 + irand(300));
		break;
	case TASK_HANDSUP:
		oneShot(p, "handsup", false, false, dt);
		if (p.t >= p.dur) fleeNow(p, p.thx, p.thy, 420 + irand(300));
		break;
	case TASK_DEAD:
		if (!p.still) {
			oneShot(p, p.clip, false, false, dt);
			p.still = pedanim::ended(p.anim, p.clip) || !pedanim::playing(p.anim, p.clip);
		}
		break;
	default: break;
	}
	p.t += dt;
}

// ----------------------------------------------------------------------------
// Events (CEventGunShot, CEventGunAimedAt, CEventAcquaintancePedHate,
// CEventVehicleThreat / potential walk into vehicle)
// ----------------------------------------------------------------------------
static bool calm(const Ped& p) { return p.task <= TASK_FOLLOW; }

static void gunShotFrom(float sx, float sy, int shooter) {
	for (int i = 0; i < MAX; i++) {
		Ped* p = pedAt(i);
		if (!p || !alive(*p) || i == shooter || p->task == TASK_ATTACK || p->task >= TASK_HIT) continue;
		const float dx = p->x - sx, dy = p->y - sy, d2 = dx * dx + dy * dy;
		if (d2 > 30.0f * 30.0f) continue;
		if (shooter == TARGET_PLAYER) {
			if (hatesPlayer(*p)) { startAttack(*p, TARGET_PLAYER); continue; }
			if (isCop(*p) && d2 < 20.0f * 20.0f) {   // gunfire next to a cop
				if (player::g.wantedLevel < 1) player::setWantedLevel(1);
				startAttack(*p, TARGET_PLAYER);
				continue;
			}
			if (friendOfPlayer(*p)) continue;
		} else if (p->gang != 0xFF || isCop(*p)) {
			continue;
		}
		if (p->task == TASK_FLEE || p->task == TASK_COWER) continue;
		if (d2 < 8.0f * 8.0f && p->stat && p->stat->fear > 60 && !irand(3)) {
			p->thx = sx;
			p->thy = sy;
			setTask(*p, TASK_COWER, 150 + irand(180));
		} else {
			startFlee(*p, sx, sy, 480 + irand(360));
		}
	}
}

void gunShot(float x, float y, float) { gunShotFrom(x, y, TARGET_PLAYER); }

// CJ aiming a gun at this ped (CEventGunAimedAt)
static bool aimedAt(const Ped& p) {
	const player::State& g = player::g;
	if (!g.aiming || g.weaponType < player::WEAPON_PISTOL || g.inVehicle) return false;
	const float dx = p.x - g.x, dy = p.y - g.y, d2 = dx * dx + dy * dy;
	return d2 < 20.0f * 20.0f && fabsf(wrap(headingTo(dx, dy) - g.heading)) < 0.25f;
}

static void scan(Ped& p, int i) {
	const player::State& g = player::g;
	const float dx = g.x - p.x, dy = g.y - p.y, d2 = dx * dx + dy * dy;
	if (aimedAt(p) && calm(p)) {
		if (hatesPlayer(p) || isCop(p)) {
			if (isCop(p)) player::registerCrime(player::CRIME_AIM_GUN);
			startAttack(p, TARGET_PLAYER);
		} else if (!friendOfPlayer(p)) {
			p.thx = g.x;
			p.thy = g.y;
			if (p.stat && p.stat->fear < 70 + irand(30)) setTask(p, TASK_HANDSUP, 120 + irand(100));
			else startFlee(p, g.x, g.y, 480 + irand(360));
		}
		return;
	}
	if (!calm(p)) return;
	// CPedScanner + acquaintances: CJ / a rival gang member in range
	if (g.health > 0.0f && hatesPlayer(p) && d2 < (isCop(p) ? 30.0f * 30.0f : 12.0f * 12.0f) &&
	    (!g.inVehicle || p.weapon >= player::WEAPON_PISTOL)) {
		startAttack(p, TARGET_PLAYER);
		return;
	}
	if (p.gang == 0xFF) return;
	for (int j = 0; j < MAX; j++) {
		const Ped* o = pedAt(j);
		if (!o || j == i || !alive(*o) || !hates(p, *o)) continue;
		const float ex = o->x - p.x, ey = o->y - p.y;
		if (ex * ex + ey * ey < 10.0f * 10.0f) { startAttack(p, j); return; }
	}
}

// CTaskComplexEvasiveDiveAndGetUp / EvasiveStep: a car coming at the ped
static void vehicleThreat(Ped& p) {
	if (!(calm(p) || p.task == TASK_FLEE)) return;
	for (int i = 0; i < veh::MAX; i++) {
		const veh::Vehicle* v = veh::get(i);
		if (!v || v->status == veh::STATUS_WRECKED) continue;
		const float mx = v->moveSpeed[0], my = v->moveSpeed[1];
		const float spd = sqrtf(mx * mx + my * my);
		if (spd < 0.12f) continue;   // ~22 km/h
		const float ux = mx / spd, uy = my / spd;
		const float dx = p.x - v->pos[0], dy = p.y - v->pos[1];
		const float along = dx * ux + dy * uy, side = dx * uy - dy * ux;
		if (along < 0.0f || along > 4.0f + spd * 50.0f || fabsf(side) > 1.8f) continue;
		// dive away from the car's line
		const float sx = side >= 0.0f ? uy : -uy, sy = side >= 0.0f ? -ux : ux;
		p.heading = headingTo(sx, sy);
		p.clip = spd > 0.25f ? "EV_dive" : "EV_step";
		p.leader = -1;
		setTask(p, TASK_DIVE);
		return;
	}
}

// ----------------------------------------------------------------------------
// Physics: ground, walls, vehicles, the other peds
// ----------------------------------------------------------------------------
static void physics(Ped& p, int dt) {
	const bool useCol = p.near && col::available() && dbg::opts.physics;
	const float fdt = DT * (float)dt;
	if (p.vx != 0.0f || p.vy != 0.0f || p.vz != 0.0f) {
		// thrown (CPhysical::ApplyMoveSpeed with gravity, reduced)
		p.x += p.vx * fdt;
		p.y += p.vy * fdt;
		const float zPrev = p.z;
		p.z += p.vz * fdt;
		p.vz -= 20.0f * fdt;
		p.vx *= 0.97f;
		p.vy *= 0.97f;
		float floor = pathZ(p);
		col::Hit h;
		if (useCol && col::groundAt(p.x, p.y, zPrev + 0.5f, p.z - 0.5f, h)) floor = h.z;
		if (p.vz < 0.0f && p.z <= floor) {
			p.z = floor;
			p.vx = p.vy = p.vz = 0.0f;
		}
		if (useCol) col::pushSphere(p.x, p.y, p.z + 1.0f, PED_R);
		return;
	}
	if (p.task == TASK_DEAD && p.still) return;
	if (useCol) {
		// off the path (fleeing, fighting): the walls; then the floor under the feet
		if (p.task != TASK_WANDER) {
			const float ox = p.x, oy = p.y;
			col::pushSphere(p.x, p.y, p.z + 1.0f, PED_R);
			if (p.task == TASK_FLEE) {
				const float mx = p.x - ox, my = p.y - oy;
				if (mx * mx + my * my > 0.0004f * dt * dt) p.sub = (uint8_t)(1 + irand(2));
				else if (p.t % 60 < dt) p.sub = 0;
			}
		}
		if (((s_frame + indexOf(p)) & 1) == 0) {
			col::Hit h;
			if (col::groundAt(p.x, p.y, p.z + 0.6f, p.z - 1.5f, h)) {
				p.z = h.z > p.z ? p.z + (h.z - p.z) * 0.5f : h.z;
				p.surface = h.surface;
			}
		}
		return;
	}
	if (p.task == TASK_WANDER || p.task == TASK_FOLLOW || p.task == TASK_FLEE) {
		const float z = pathZ(p);
		p.z += (z - p.z) * 0.25f;
	}
}

static void vehicles(Ped& p) {
	if (p.task == TASK_DEAD) return;
	veh::PedHit hit;
	veh::pushPed(p.x, p.y, p.z + 1.0f, PED_R, -1, &hit);
	if (hit.id < 0 || hit.speed < 0.08f || p.cooldown > 0 || p.task == TASK_FALL) return;
	// CPed::KillPedWithCar: the speed decides how bad it is
	const float kmh = hit.speed * 180.0f;
	const veh::Vehicle* v = veh::get(hit.id);
	const bool cjDriving = player::vehicleId() == hit.id && player::driving();
	const float dmg = kmh < 15.0f ? 0.0f : (kmh - 10.0f) * 2.2f;
	veh::pedHitSound(hit.id, p.x, p.y, p.z);
	p.cooldown = 30;
	if (dmg <= 0.0f) return;
	damage(p, dmg, v ? v->pos[0] : p.x, v ? v->pos[1] : p.y, DMG_CAR, 0, cjDriving ? TARGET_PLAYER : TARGET_NONE, true,
	       hit.vx * 50.0f * 0.8f, hit.vy * 50.0f * 0.8f);
}

static void separate() {
	for (int i = 0; i < MAX; i++) {
		Ped* a = pedAt(i);
		if (!a || a->task == TASK_DEAD) continue;
		for (int j = i + 1; j < MAX; j++) {
			Ped* b = pedAt(j);
			if (!b || b->task == TASK_DEAD) continue;
			const float dx = b->x - a->x, dy = b->y - a->y, d2 = dx * dx + dy * dy;
			const float r = PED_R * 2.0f;
			if (d2 >= r * r || d2 < 1e-6f || fabsf(a->z - b->z) > 1.5f) continue;
			const float d = sqrtf(d2), push = (r - d) * 0.5f / d;
			a->x -= dx * push; a->y -= dy * push;
			b->x += dx * push; b->y += dy * push;
		}
	}
}

bool pushPlayer(float& x, float& y, float z, float r) {
	bool touched = false;
	for (int i = 0; i < MAX; i++) {
		Ped* p = pedAt(i);
		if (!p || p->task == TASK_DEAD || fabsf(p->z - z) > 1.5f) continue;
		const float dx = x - p->x, dy = y - p->y, d2 = dx * dx + dy * dy, rr = r + PED_R;
		if (d2 >= rr * rr || d2 < 1e-6f) continue;
		const float d = sqrtf(d2), push = (rr - d) / d;
		// both give way (CJ is heavier: he moves 2/3 less)
		x += dx * push * 0.4f; y += dy * push * 0.4f;
		p->x -= dx * push * 0.6f; p->y -= dy * push * 0.6f;
		touched = true;
	}
	return touched;
}

bool copNear(float x, float y, float r) {
	for (int i = 0; i < MAX; i++) {
		const Ped* p = pedAt(i);
		if (!p || !alive(*p) || !isCop(*p)) continue;
		const float dx = p->x - x, dy = p->y - y;
		if (dx * dx + dy * dy < r * r) return true;
	}
	return false;
}

// CPed::PlayFootSteps for the peds close to the camera
static void footsteps(Ped& p) {
	if (p.task > TASK_ATTACK || !p.near) { p.lastFoot = -1.0f; return; }
	const pedmove::Cycle c = pedmove::dominantCycle(p.move);
	if (pedmove::cycleWeight(p.move, c) < 0.5f) { p.lastFoot = -1.0f; return; }
	const char* cyc = c == pedmove::CYCLE_SPRINT ? p.mo.sprint : c == pedmove::CYCLE_RUN ? p.mo.run : p.mo.walk;
	const float ph = pedanim::phase(p.anim, cyc), prev = p.lastFoot;
	p.lastFoot = ph;
	if (ph < 0.0f || prev < 0.0f) return;
	auto crossed = [&](float t) { return prev <= ph ? prev < t && ph >= t : prev < t || ph >= t; };
	const float vol = (c == pedmove::CYCLE_SPRINT ? 0.0f : c == pedmove::CYCLE_RUN ? -6.0f : -12.0f) - 3.0f;
	const float speed = c == pedmove::CYCLE_SPRINT ? 1.2f : c == pedmove::CYCLE_RUN ? 1.1f : 0.9f;
	const aeped::Pos pos = voicePos(p);
	if (crossed(1.0f / 15.0f)) aeped::footstep(true, vol, speed, p.surface, pos);
	if (crossed(0.5f + 1.0f / 15.0f)) aeped::footstep(false, vol, speed, p.surface, pos);
}

// ----------------------------------------------------------------------------
// CPopulation: add / remove
// ----------------------------------------------------------------------------
static void removePed(Ped& p) {
	const int i = indexOf(p);
	for (int k = 0; k < MAX; k++) {
		Ped* o = pedAt(k);
		if (!o || o == &p) continue;
		if (o->leader == i) o->leader = -1;
		if (o->target == i) o->target = TARGET_NONE;
	}
	pedanim::reset(p.anim);
	pedmodel::release(p.model);
	s_pool.destroy(&p);
	s_stats.removed++;
}

static cj::Motion motionOf(int group) {
	const ds::PedAnimGroup* g = peddata::animGroup(group);
	const ds::PedAnimGroup* def = peddata::animGroup(0);
	auto clip = [&](int k) -> const char* {
		if (g && pedanim::exists(g->clip[k])) return g->clip[k];
		return def ? def->clip[k] : "IDLE_stance";
	};
	cj::Motion m;
	m.idle = clip(ds::PEDANIM_IDLE);
	m.walk = clip(ds::PEDANIM_WALK);
	m.run = clip(ds::PEDANIM_RUN);
	m.sprint = clip(ds::PEDANIM_SPRINT);
	m.start = clip(ds::PEDANIM_WALKSTART);
	m.tired = m.idle;
	m.stop = "run_stop";
	m.stopR = "run_stopR";
	return m;
}

// CPopulation::AddPed
static Ped* addPed(int pedIndex, float x, float y, float z, float heading) {
	const ds::PedModel* pm = peddata::model(pedIndex);
	const int h = pedmodel::find(pedIndex);
	const pedmodel::Model* mdl = pedmodel::get(h);
	if (!pm || !mdl || mdl->state != pedmodel::STATE_READY || mdl->skel->numBones > MAX_BONES) return nullptr;
	Ped* p = s_pool.create();
	if (!p) return nullptr;
	memset((void*)p, 0, sizeof(Ped));
	pedmodel::request(pedIndex);   // the ped's own reference
	p->model = (int8_t)h;
	p->pedIndex = (int16_t)pedIndex;
	p->pedType = pm->pedType;
	p->female = pm->pedType == peddata::PT_CIVFEMALE || pm->pedType == peddata::PT_PROSTITUTE;
	p->gang = peddata::isGang(pm->pedType) ? (uint8_t)(pm->pedType - peddata::PT_GANG1) : 0xFF;
	p->stat = peddata::stat(pm->stat);
	p->mo = motionOf(pm->animGroup);
	p->x = x; p->y = y; p->z = z;
	p->heading = heading;
	p->health = 100.0f;
	p->target = TARGET_NONE;
	p->leader = -1;
	p->lastFoot = -1.0f;
	p->money = (uint16_t)(pm->pedType == peddata::PT_DEALER ? 100 + irand(200) : irand(35));
	p->needUntil = s_frame + 240;
	// CGangs::Gang[].GetRandomWeapon (33%), CCopPed's pistol
	static const uint8_t k_gangWeapons[10][2] = {
		{ player::WEAPON_PISTOL, player::WEAPON_MICRO_UZI }, { player::WEAPON_PISTOL, 0 }, { player::WEAPON_PISTOL, 0 },
		{ 0, 0 }, { player::WEAPON_PISTOL, player::WEAPON_MICRO_UZI }, { player::WEAPON_DESERT_EAGLE, 0 },
		{ player::WEAPON_PISTOL, player::WEAPON_AK47 }, { player::WEAPON_PISTOL, player::WEAPON_MICRO_UZI }, { 0, 0 }, { 0, 0 } };
	if (p->gang != 0xFF && !irand(3)) p->weapon = k_gangWeapons[p->gang][irand(2)];
	if (pm->pedType == peddata::PT_COP) p->weapon = player::WEAPON_PISTOL;
	pedmove::reset(p->move);
	s_stats.spawned++;
	return p;
}

// CPathFind::GeneratePedCreationCoors: a random point on a ped path link in
// the creation ring (on screen far, off screen nearer), clear of other peds.
static bool creationCoors(bool start, float& x, float& y, float& z, int& from, int& to, float& lane) {
	const float px = player::g.x, py = player::g.y, pz = player::g.z;
	int cand[48], nc = 0;
	const float outer = start ? SPAWN_ON_MAX : SPAWN_ON_MAX + 4.0f;
	peddata::forNodesNear(px, py, outer, [&](int i) {
		const ds::PathNode* n = peddata::node(i);
		if (!n->numLinks || (n->flags & ds::PATHNODE_SWITCHED_OFF)) return true;
		if (nc < 48) cand[nc++] = i;
		else cand[irand(48)] = i;
		return true;
	});
	for (int tr = 0; tr < 10 && nc; tr++) {
		const int a = cand[irand(nc)];
		const ds::PathNode* na = peddata::node(a);
		const int b = peddata::link(na, irand(na->numLinks));
		const ds::PathNode* nb = peddata::node(b);
		if (!nb || (nb->flags & ds::PATHNODE_SWITCHED_OFF)) continue;
		const float t = frand();
		const float ax = peddata::nodeX(na), ay = peddata::nodeY(na);
		const float sx = peddata::nodeX(nb) - ax, sy = peddata::nodeY(nb) - ay;
		const float l = sqrtf(sx * sx + sy * sy);
		const float width = fmaxf(1.0f, fminf(4.0f, (na->width + nb->width) * (0.5f / 16.0f)));
		lane = (frand() - 0.5f) * width * 0.8f;
		x = ax + sx * t + (l > 0.01f ? sy / l : 0.0f) * lane;
		y = ay + sy * t - (l > 0.01f ? sx / l : 0.0f) * lane;
		z = peddata::nodeZ(na) + (peddata::nodeZ(nb) - peddata::nodeZ(na)) * t;
		if (fabsf(z - pz) > 25.0f) continue;
		const float dx = x - px, dy = y - py, d = sqrtf(dx * dx + dy * dy);
		const bool vis = onScreen(x, y, z + 1.0f, 1.0f);
		const bool ok = start ? d >= 10.0f && d <= SPAWN_ON_MAX
		                      : vis ? d >= SPAWN_ON_MIN && d <= SPAWN_ON_MAX : d >= SPAWN_OFF_MIN && d <= SPAWN_OFF_MAX;
		if (!ok) continue;
		const ds::PedZone* zone = peddata::zoneAt(x, y);
		if (zone != s_pop.zone && (!zone || !s_pop.zone || zone->popType != s_pop.zone->popType ||
		                            zone->races != s_pop.zone->races || memcmp(zone->gang, s_pop.zone->gang, 10))) continue;
		bool clear = true;
		for (int i = 0; i < MAX && clear; i++) {
			const Ped* o = pedAt(i);
			if (o && (o->x - x) * (o->x - x) + (o->y - y) * (o->y - y) < 2.5f * 2.5f) clear = false;
		}
		float vx = x, vy = y;
		if (!clear || veh::pushPed(vx, vy, z + 1.0f, PED_R)) continue;
		// pick the walking direction: from -> to
		if (irand(2)) { from = a; to = b; }
		else { from = b; to = a; lane = -lane; }
		return true;
	}
	return false;
}

static int usersOf(int handle) {
	int n = 0;
	for (int i = 0; i < MAX; i++) {
		const Ped* p = pedAt(i);
		if (p && p->model == handle) n++;
	}
	return n;
}

// a resident model of this category (the least used one: variety)
static int residentModel(Cat cat, int gang) {
	int best = -1, bestUsers = 1 << 20;
	for (int h = 0; h < pedmodel::MAX; h++) {
		const pedmodel::Model* m = pedmodel::get(h);
		if (!m || m->state != pedmodel::STATE_READY) continue;
		const ds::PedModel* pm = peddata::model(m->pedIndex);
		if (!pm || catOf(m->pedIndex) != cat) continue;
		if (cat == CAT_GANG && pm->pedType - peddata::PT_GANG1 != gang) continue;
		if (cat == CAT_CIV && (!raceAllowed(pm->race) || !appropriate(m->pedIndex))) continue;
		const int u = usersOf(h) * 4 + irand(4);
		if (u < bestUsers) { bestUsers = u; best = m->pedIndex; }
	}
	return best;
}

// CPopCycle::FindNewPedType (+ PickGangToCreateMembersOf)
static int findNewPedType(Cat& cat) {
	float civ = s_pop.nOther - (float)s_numCiv;
	float cops = s_pop.nCops - (float)s_numCops + (float)player::g.wantedLevel * 1.5f;
	float dealers = s_pop.nDealers - (float)s_numDealers;
	float gangs = s_pop.nGang - (float)s_numGangTotal;
	for (float* c : { &civ, &cops, &dealers, &gangs })
		if (*c < 2.0f) *c *= frand();
	for (int tries = 0; tries < 4; tries++) {
		const float best = fmaxf(fmaxf(civ, cops), fmaxf(dealers, gangs));
		if (best <= 0.0f) return -1;
		if (best == gangs) {
			// the gang furthest under its share of the zone's strength
			const ds::PedZone* z = s_pop.zone;
			int g = -1, sum = 0;
			float bestScore = -1e9f;
			for (int k = 0; z && k < 10; k++) sum += z->gang[k];
			for (int k = 0; z && sum && k < 10; k++) {
				if (!z->gang[k]) continue;
				const float score = (float)z->gang[k] / sum - (s_pop.nGang > 0.0f ? s_numGang[k] / s_pop.nGang : 0.0f);
				if (score > bestScore) { bestScore = score; g = k; }
			}
			const int mi = g >= 0 ? residentModel(CAT_GANG, g) : -1;
			if (mi >= 0) { cat = CAT_GANG; return mi; }
			gangs = 0.0f;
		} else if (best == cops) {
			const int mi = residentModel(CAT_COP, 0);
			if (mi >= 0) { cat = CAT_COP; return mi; }
			cops = 0.0f;
		} else if (best == dealers) {
			const int mi = residentModel(CAT_DEALER, 0);
			if (mi >= 0) { cat = CAT_DEALER; return mi; }
			dealers = 0.0f;
		} else {
			const int mi = residentModel(CAT_CIV, 0);
			if (mi >= 0) { cat = CAT_CIV; return mi; }
			civ = 0.0f;
		}
	}
	return -1;
}

// CPopulation::AddToPopulation (+ PlaceGangMembers: gangs walk in twos and threes)
static bool addToPopulation(bool start) {
	if (s_pool.full() || s_live >= s_pop.maxPeds) return false;
	Cat cat;
	const int mi = findNewPedType(cat);
	if (mi < 0) return false;
	float x, y, z, lane;
	int from, to;
	if (!creationCoors(start, x, y, z, from, to, lane)) return false;
	const ds::PathNode* nt = peddata::node(to);
	const float heading = headingTo(peddata::nodeX(nt) - x, peddata::nodeY(nt) - y);
	Ped* p = addPed(mi, x, y, z, heading);
	if (!p) return false;
	p->from = from;
	p->to = to;
	p->lane = lane;
	setTarget(*p);
	p->task = TASK_WANDER;
	if (cat == CAT_GANG) {
		const int members = 1 + irand(2);
		for (int k = 0; k < members && !s_pool.full() && s_live + 1 + k < MAX; k++) {
			const float side = (k & 1) ? 1.1f : -1.1f;
			const int m2 = residentModel(CAT_GANG, p->gang);
			Ped* f = addPed(m2 >= 0 ? m2 : mi, x + fcos(heading) * side, y + fsin(heading) * side, z, heading);
			if (!f) break;
			f->leader = (int8_t)indexOf(*p);
			f->from = p->from;
			f->to = p->to;
			f->task = TASK_FOLLOW;
		}
	}
	return true;
}

static void countPeds() {
	s_numCiv = s_numCops = s_numDealers = s_numGangTotal = s_live = 0;
	memset(s_numGang, 0, sizeof s_numGang);
	for (int i = 0; i < MAX; i++) {
		const Ped* p = pedAt(i);
		if (!p || !alive(*p)) continue;
		s_live++;
		if (p->gang != 0xFF) { s_numGang[p->gang]++; s_numGangTotal++; }
		else if (isCop(*p)) s_numCops++;
		else if (p->pedType == peddata::PT_DEALER) s_numDealers++;
		else s_numCiv++;
	}
	s_stats.live = (uint16_t)s_live;
	s_stats.civ = (uint16_t)s_numCiv;
	s_stats.gang = (uint16_t)s_numGangTotal;
	s_stats.cops = (uint16_t)s_numCops;
	s_stats.dealers = (uint16_t)s_numDealers;
}

// CPopulation::ManagePed
static void manage(Ped& p) {
	const float dx = p.x - player::g.x, dy = p.y - player::g.y, d2 = dx * dx + dy * dy;
	if (p.task == TASK_DEAD || (p.health <= 0.0f && p.deathFrame)) {
		const uint32_t age = s_frame - p.deathFrame;
		if (!visible(p.x, p.y, p.z + 1.0f, 1.2f) && (age > 30 * 60 || d2 > REMOVE_FAR * REMOVE_FAR)) removePed(p);
		return;
	}
	if (d2 > REMOVE_DIST * REMOVE_DIST && !visible(p.x, p.y, p.z + 1.0f, 1.2f)) { removePed(p); return; }
	const bool chasing = p.task == TASK_ATTACK && p.target == TARGET_PLAYER;
	if (d2 <= NEED_DIST * NEED_DIST || p.onScreen || chasing) p.needUntil = s_frame + (isCop(p) ? 600 : 240);
	else if (s_frame > p.needUntil) removePed(p);
}

// ----------------------------------------------------------------------------
// Public
// ----------------------------------------------------------------------------
void init() {
	if (s_driverModel >= 0) pedmodel::release(s_driverModel);
	s_driverModel = -1;
	if (s_pool.capacity()) {
		for (int i = 0; i < MAX; i++)
			if (Ped* p = pedAt(i)) removePed(*p);
	} else {
		s_pool.init(MAX, mem::TAG_PED);
	}
	for (int k = 0; k < pedmodel::MAX; k++) {
		if (s_held[k] >= 0 && s_inited) pedmodel::release(s_held[k]);
		s_held[k] = -1;
	}
	s_inited = true;
	s_cam.valid = false;
	s_stats = {};
}

// Loading job: tables, the first ped models (read and compiled now, their
// textures go up in the loading screen's vblanks), then
// CPopulation::GeneratePedsAtStartOfGame.
bool initStep() {
	static int s_step, s_wait;
	if (s_step == 0) {
		if (!peddata::initStep()) return false;
		init();
		s_step = peddata::ready() ? 1 : 3;
		s_wait = 0;
		return s_step == 3 ? (s_step = 0, true) : false;
	}
	if (s_step == 1) {
		updatePopCycle();
		streamModels();
		pedmodel::update();
		pedmodel::update();
		if (++s_wait >= 4 * pedmodel::MAX) s_step = 2;
		return false;
	}
	if (s_step == 2) {
		pedmodel::update();
		bool busy = false;
		for (int h = 0; h < pedmodel::MAX; h++) {
			const pedmodel::Model* m = pedmodel::get(h);
			busy |= m && m->state != pedmodel::STATE_READY && m->state != pedmodel::STATE_FAILED;
		}
		if (busy && ++s_wait < 600) return false;
		countPeds();
		for (int i = 0; i < 40 && s_live < s_pop.maxPeds; i++) {
			addToPopulation(true);
			countPeds();
		}
		int driver = -1;
		for (int k = 0; k < pedmodel::MAX; k++) {
			const pedmodel::Model* m = pedmodel::get(s_held[k]);
			if (!m || !pedmodel::ready(s_held[k])) continue;
			if (driver < 0 || catOf(m->pedIndex) == CAT_CIV) driver = m->pedIndex;
			if (catOf(m->pedIndex) == CAT_CIV) break;
		}
		if (driver >= 0) {
			const int h = pedmodel::request(driver);
			const pedmodel::Model* m = pedmodel::get(h);
			if (m && m->skel && m->skel->numBones <= MAX_BONES) {
				pedanim::Set anim = {};
				const cj::Clip sit = { "CAR_sit", 1.0f, true, false, false, false, 0.0f };
				pedanim::pose(anim, m->skel, &sit, 1);
				if (pedanim::playing(anim, sit.name)) {
					pedanim::evaluate(anim, m->skel, s_driverWorld[0]);
					s_driverModel = h;
				}
				pedanim::reset(anim);
				if (s_driverModel >= 0) {
					const cj::Clip low = { "CAR_Lsit", 1.0f, true, false, false, false, 0.0f };
					pedanim::pose(anim, m->skel, &low, 1);
					s_driverLow = pedanim::playing(anim, low.name);
					if (s_driverLow) pedanim::evaluate(anim, m->skel, s_driverWorld[1]);
					pedanim::reset(anim);
				}
			}
			if (s_driverModel < 0 && h >= 0) pedmodel::release(h);
		}
		hud::log("peds: %d spawned, %u models, zone %d max %d", s_live, pedmodel::stats().ready, s_pop.zoneType, s_pop.maxPeds);
	}
	s_step = 0;
	return true;
}

void update(int dt) {
	if (!peddata::ready() || !s_pool.capacity()) return;
	const u32 t0 = timing::ticks();
	if (dt < 1) dt = 1;
	if (dt > 4) dt = 4;
	s_frame++;
	pedmodel::update();
	if (s_frame % 60 == 1) updatePopCycle();
	if (s_frame % 90 == 7) streamModels();
	countPeds();
	for (int i = 0; i < MAX; i++)
		if (Ped* p = pedAt(i)) manage(*p);
	if (dbg::opts.peds && s_frame % 6 == 0) addToPopulation(false);

	const player::State& g = player::g;
	u32 taskUs = 0, physUs = 0;
	for (int i = 0; i < MAX; i++) {
		Ped* p = pedAt(i);
		if (!p) continue;
		const float dx = p->x - g.x, dy = p->y - g.y;
		p->near = dx * dx + dy * dy < COL_RANGE * COL_RANGE;
		// AI LOD: peds near CJ or on screen think every frame, the rest every
		// 2nd / 4th frame with the ticks they missed
		const float d2p = dx * dx + dy * dy;
		const int every = p->near || p->onScreen || p->task >= TASK_HIT || p->vz != 0.0f ? 1 : d2p < 25.0f * 25.0f ? 2 : 4;
		p->acc = (uint8_t)(p->acc + dt);
		if (((s_frame + i) % every) != 0 && p->acc < 12) continue;
		const int pdt = p->acc > 12 ? 12 : p->acc;
		p->acc = 0;
		if (alive(*p)) {
			if (((s_frame + i) & 7) < (uint32_t)every) scan(*p, i);
			if (((s_frame + i) & 3) < (uint32_t)every) vehicleThreat(*p);
		}
		const u32 ta = timing::ticks();
		if (p->voiceCd > 0) p->voiceCd -= pdt;
		if (p->task != TASK_ATTACK && p->cooldown > 0) p->cooldown -= pdt;
		taskStep(*p, pdt);
		taskUs += timing::usSince(ta);
		const u32 tb = timing::ticks();
		physics(*p, pdt);
		vehicles(*p);
		physUs += timing::usSince(tb);
		footsteps(*p);
		// CPed::CreateDeadPedMoney, reduced: walk over the body to take it
		if (p->health <= 0.0f && !p->moneyGiven && !g.inVehicle && dx * dx + dy * dy < 1.6f * 1.6f && p->task == TASK_DEAD) {
			p->moneyGiven = true;
			if (p->money) {
				player::g.money += p->money;
				hud::log("picked up $%u", (unsigned)p->money);
			}
		}
	}
	separate();
	s_stats.models = pedmodel::stats().ready;
	s_stats.wanted = (uint16_t)g.wantedLevel;
	s_stats.updateUs = timing::usSince(t0);
	s_stats.taskUs = taskUs;
	s_stats.physUs = physUs;
}

void vblank() { pedmodel::vblank(); }

static inline void storeCam(const View& v) {
	s_cam.ex = v.eyeX; s_cam.ey = v.eyeY; s_cam.ez = v.eyeZ;
	const float sy = fsin(v.yaw), cy = fcos(v.yaw), sp = fsin(v.pitch), cp = fcos(v.pitch);
	s_cam.f[0] = -cp * sy; s_cam.f[1] = cp * cy; s_cam.f[2] = -sp;
	s_cam.r[0] = cy; s_cam.r[1] = sy; s_cam.r[2] = 0.0f;
	s_cam.u[0] = -sp * sy; s_cam.u[1] = sp * cy; s_cam.u[2] = cp;
	s_cam.valid = true;
}

int reservePolys(const View& v) {
	if (!s_pool.capacity()) return 0;
	storeCam(v);
	int reserve = 0;
	for (int i = 0; i < MAX; i++) {
		const Ped* p = pedAt(i);
		if (!p || !pedmodel::ready(p->model) || !onScreen(p->x, p->y, p->z + 1.0f, 1.2f)) continue;
		const float dx = p->x - v.eyeX, dy = p->y - v.eyeY, dz = p->z - v.eyeZ;
		if (dx * dx + dy * dy + dz * dz > DRAW_DIST * DRAW_DIST) continue;
		reserve += pedmodel::get(p->model)->tris / 2 + 8;
	}
	if (s_driverModel >= 0 && pedmodel::ready(s_driverModel)) {
		const int cost = pedmodel::get(s_driverModel)->tris / 2 + 8;
		int drivers = 0;
		for (int i = 0; i < veh::MAX && drivers < 2; i++) {
			const veh::Vehicle* car = veh::get(i);
			if (!car || !car->npcDriver || !vehmdl::ready(car->model) ||
			    !onScreen(car->pos[0], car->pos[1], car->pos[2] + 1.0f, 2.0f)) continue;
			const float dx = car->pos[0] - v.eyeX, dy = car->pos[1] - v.eyeY;
			if (dx * dx + dy * dy > 35.0f * 35.0f) continue;
			reserve += cost;
			drivers++;
		}
	}
	if (reserve > dbg::opts.pedPolys) reserve = dbg::opts.pedPolys;
	return reserve < 320 ? reserve : 320;
}

void draw(const View& v) {
	storeCam(v);
	if (!s_pool.capacity()) return;
	const u32 t0 = timing::ticks();
	struct Vis { Ped* p; float d; };
	Vis vis[MAX];
	int n = 0;
	for (int i = 0; i < MAX; i++) {
		Ped* p = pedAt(i);
		if (!p) continue;
		p->onScreen = onScreen(p->x, p->y, p->z + 1.0f, 1.2f);
		if (!p->onScreen || !pedmodel::ready(p->model)) continue;
		const float dx = p->x - v.eyeX, dy = p->y - v.eyeY, dz = p->z - v.eyeZ;
		const float d = sqrtf(dx * dx + dy * dy + dz * dz);
		if (d > DRAW_DIST) continue;
		int k = n++;
		while (k > 0 && vis[k - 1].d > d) { vis[k] = vis[k - 1]; k--; }
		vis[k] = { p, d };
	}
	int occupied = 0;
	glGetInt(GL_GET_POLYGON_RAM_COUNT, &occupied);
	const int available = 2048 - 32 - occupied;
	int polys = 0, drawn = 0, posed = 0;
	u32 poseUs = 0;
	MATRIX_STORE = pedmodel::SLOT_VIEW;
	int drivers = 0;
	if (s_driverModel >= 0 && pedmodel::ready(s_driverModel)) {
		const pedmodel::Model& m = *pedmodel::get(s_driverModel);
		const int est = m.tris / 2 + 8;
		for (int i = 0; i < veh::MAX && drivers < 2; i++) {
			const veh::Vehicle* car = veh::get(i);
			if (!car || !car->npcDriver || !vehmdl::ready(car->model) ||
			    !onScreen(car->pos[0], car->pos[1], car->pos[2] + 1.0f, 2.0f)) continue;
			const float dx = car->pos[0] - v.eyeX, dy = car->pos[1] - v.eyeY;
			if (dx * dx + dy * dy > 35.0f * 35.0f || polys + est > dbg::opts.pedPolys ||
			    polys + est > available) continue;
			const vehmdl::Model* carModel = vehmdl::get(car->model);
			s32 attached[12];
			veh::attachMatrix(*car, carModel->seat, v.originX, v.originY, v.originZ, attached);
			MATRIX_RESTORE = pedmodel::SLOT_VIEW;
			for (s32 x : attached) MATRIX_MULT4x3 = x;
			MATRIX_STORE = pedmodel::SLOT_ENTITY;
			pedmodel::draw(m, s_driverWorld[s_driverLow && (carModel->info.modelFlags & 4) ? 1 : 0]);
			polys += est;
			drivers++;
		}
	}
	for (int k = 0; k < n; k++) {
		Ped& p = *vis[k].p;
		const pedmodel::Model& m = *pedmodel::get(p.model);
		const int est = m.tris / 2 + 8;   // back faces culled: about half reach the polygon RAM
		if (polys + est > dbg::opts.pedPolys || polys + est > available) continue;
		// pose (~0.9 ms each, DEBUG): every frame near, every 2nd..4th frame further away
		const int rate = vis[k].d < 10.0f ? 1 : vis[k].d < 20.0f ? 2 : vis[k].d < 30.0f ? 3 : 4;
		if (!p.poseValid || (!p.still && ++p.poseAge >= rate)) {
			const u32 te = timing::ticks();
			pedanim::evaluate(p.anim, m.skel, p.world);
			const u32 one = timing::usSince(te);
			poseUs += one;
			if (one > s_stats.posePeakUs) s_stats.posePeakUs = one;
			s_stats.poseTotalUs += one;
			s_stats.poseCount++;
			p.poseValid = true;
			p.poseAge = 0;
			posed++;
		}
		MATRIX_RESTORE = pedmodel::SLOT_VIEW;
		glTranslatef(p.x - v.originX, p.z - v.originZ + m.lift, -(p.y - v.originY));
		glRotatef(p.heading * 57.2958f, 0.0f, 1.0f, 0.0f);
		glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
		MATRIX_STORE = pedmodel::SLOT_ENTITY;
		pedmodel::draw(m, p.world);
		polys += est;
		drawn++;
	}
	MATRIX_RESTORE = pedmodel::SLOT_VIEW;
	pedmodel::endDraw();
	s_stats.drawn = (uint16_t)drawn;
	s_stats.drivers = (uint16_t)drivers;
	s_stats.posed = (uint16_t)posed;
	s_stats.polys = (uint16_t)polys;
	s_stats.drawUs = timing::usSince(t0);
	s_stats.poseUs = poseUs;
}

// --- interactions -----------------------------------------------------------
static bool isDown(const Ped& p) { return p.health > 0.0f && (p.task == TASK_FALL || p.task == TASK_GETUP) && p.vz == 0.0f; }

// nearest ped in the strike's path: standing ones for the fist moves, lying ones for the kick
static Ped* strikeTarget(float x, float y, float z, float heading, float range, bool ground) {
	const float fx = -fsin(heading), fy = fcos(heading);
	Ped* best = nullptr;
	float bestD = range;
	for (int i = 0; i < MAX; i++) {
		Ped* p = pedAt(i);
		if (!p || !alive(*p) || isDown(*p) != ground || fabsf(p->z - z) > 1.2f) continue;
		// a body on the floor lies about 0.8 m from its origin along its fall
		const float dx = p->x - x, dy = p->y - y;
		const float along = dx * fx + dy * fy, side = fabsf(dx * fy - dy * fx);
		if (along < 0.0f || along > bestD || side > (ground ? 0.9f : 0.6f)) continue;
		best = p;
		bestD = along;
	}
	return best;
}

bool downedInFront(float x, float y, float z, float heading, float range) {
	return strikeTarget(x, y, z, heading, range + 0.3f, true) != nullptr;
}

bool meleeHit(float x, float y, float z, float heading, float range, int weaponType, int move) {
	const bool ground = move == melee::MOVE_GROUND;
	Ped* best = strikeTarget(x, y, z, heading, ground ? range + 0.3f : range, ground);
	if (!best) return false;
	// melee.dat damage for fists, weapon.dat's for melee weapons (their own
	// combos are not ported: the fist moves carry them)
	const bool fists = weaponType == player::WEAPON_UNARMED || weaponType == player::WEAPON_BRASSKNUCKLE;
	float dmg = fists ? (float)melee::k_unarmed[move].damage : weaponDamage(weaponType);
	if (weaponType == player::WEAPON_BRASSKNUCKLE) dmg *= 1.5f;
	const bool knock = melee::knocksDown(move) || (!fists && !ground && !irand(3));
	damage(*best, dmg, x, y, DMG_MELEE, weaponType, TARGET_PLAYER, knock, 0.0f, 0.0f, move);
	return true;
}

bool bulletHit(const float a[3], float b[3], int weaponType) {
	const float sx = b[0] - a[0], sy = b[1] - a[1], sz = b[2] - a[2];
	const float l2 = sx * sx + sy * sy + sz * sz;
	if (l2 < 1e-4f) return false;
	Ped* best = nullptr;
	float bestT = 1.0f;
	bool head = false;
	for (int i = 0; i < MAX; i++) {
		Ped* p = pedAt(i);
		if (!p || p->health <= 0.0f) continue;
		// closest approach of the shot to the ped's vertical axis (0.2 .. 1.8 m)
		const float px = p->x - a[0], py = p->y - a[1];
		const float sl2 = sx * sx + sy * sy;
		if (sl2 < 1e-6f) continue;
		float t = (px * sx + py * sy) / sl2;
		if (t < 0.0f || t > bestT) continue;
		const float cx = a[0] + sx * t - p->x, cy = a[1] + sy * t - p->y;
		if (cx * cx + cy * cy > 0.4f * 0.4f) continue;
		const float hz = a[2] + sz * t - p->z;
		if (hz < 0.1f || hz > 1.9f) continue;
		best = p;
		bestT = t;
		head = hz > 1.55f;
	}
	if (!best) return false;
	for (int k = 0; k < 3; k++) b[k] = a[k] + (b[k] - a[k]) * bestT;
	// headshots: x3 (the player's shots on peds)
	damage(*best, weaponDamage(weaponType) * (head ? 3.0f : 1.0f), a[0], a[1], DMG_BULLET, weaponType, TARGET_PLAYER, false);
	return true;
}

void explosion(float x, float y, float z, float radius, float dmg) {
	for (int i = 0; i < MAX; i++) {
		Ped* p = pedAt(i);
		if (!p || !alive(*p)) continue;
		const float dx = p->x - x, dy = p->y - y, dz = p->z - z, d = sqrtf(dx * dx + dy * dy + dz * dz);
		if (d > radius) continue;
		const float k = 1.0f - d / radius;
		const float inv = 1.0f / (d + 0.3f);
		damage(*p, dmg * k, x, y, DMG_EXPLOSION, 0, TARGET_NONE, true, dx * inv * 8.0f * k, dy * inv * 8.0f * k);
	}
	gunShotFrom(x, y, TARGET_NONE);
}

// --- debug ------------------------------------------------------------------
int count() { return s_pool.count(); }

bool info(int i, Info& out) {
	const Ped* p = pedAt(i);
	if (!p) { out = {}; return false; }
	const ds::PedModel* pm = peddata::model(p->pedIndex);
	out = { true, p->task, p->pedType, (int16_t)(pm ? pm->modelId : -1), p->x, p->y, p->z, p->heading, p->health, p->target };
	return true;
}

int spawnNear(float x, float y, float z, int modelId) {
	int mi = modelId >= 0 ? peddata::findModel(modelId) : -1;
	if (mi >= 0 && !pedmodel::ready(pedmodel::find(mi))) {
		const int h = pedmodel::request(mi);   // held by the population until it rotates out
		for (int k = 0; k < pedmodel::MAX && h >= 0; k++)
			if (s_held[k] < 0) { s_held[k] = (int8_t)h; s_heldFrame[k] = s_frame; break; }
		return -1;   // ask again once it streamed in
	}
	if (mi < 0) {
		for (int h = 0; h < pedmodel::MAX && mi < 0; h++)
			if (pedmodel::ready(h)) mi = pedmodel::get(h)->pedIndex;
	}
	const int n = peddata::nearestNode(x, y, z, 40.0f);
	if (mi < 0 || n < 0) return -1;
	const ds::PathNode* nd = peddata::node(n);
	Ped* p = addPed(mi, peddata::nodeX(nd), peddata::nodeY(nd), peddata::nodeZ(nd), 0.0f);
	if (!p) return -1;
	p->lane = 0.0f;
	resumeWander(*p);
	return indexOf(*p);
}

void removeAll() {
	for (int i = 0; i < MAX; i++)
		if (Ped* p = pedAt(i)) removePed(*p);
}

void suspendForCutscene() {
	removeAll();
	for (int8_t& h : s_held) {
		if (h >= 0) pedmodel::release(h);
		h = -1;
	}
}

void debugPlace(int i, float x, float y, float z, float heading) {
	Ped* p = pedAt(i);
	if (!p) return;
	p->x = x; p->y = y; p->z = z;
	p->heading = heading;
	setTask(*p, TASK_STAND, 600);
}

const Stats& stats() { return s_stats; }

}
