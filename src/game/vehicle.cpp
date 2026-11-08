#include "game/vehicle.h"

#include <initializer_list>
#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/texture.h"
#include "audio/aevehicleaudio.h"
#include "game/collision.h"
#include "game/debugopts.h"
#include "game/peds.h"
#include "game/peddata.h"
#include "game/player.h"
#include "game/skidmarks.h"
#include "game/timecycle.h"
#include "game/vehicledata.h"
#include "game/vehicleint.h"
#include "game/waterlevel.h"
#include "hud/hud.h"
#include "platform/timing.h"
#include "render/gxlist.h"
#include "render/sky.h"

namespace veh {

using namespace vehmdl;

static constexpr float ACTIVE_RANGE = 10.0f;          // m from the player: the collision near list covers it
static constexpr int VIEW_SLOT = 11, BODY_SLOT = 10;   // matrix stack (map 13, overlay 12, CJ 14..30)
static constexpr float DOOR_OPEN_ANGLE = 1.25f;       // CDoor m_fOpenAngle ~ 0.4 PI

Car g_cars[MAX];

struct Debris { bool used; int8_t model; uint8_t part; float pos[3], r[9], vel[3], w[3], life; };
static Debris s_debris[8];
struct Fx { bool used; float pos[3], t; };
static Fx s_fx[4];
static Stats s_stats;

// ---- vector helpers (floats: damage, drawing, the player side) -----------------
static inline float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void cross(const float* a, const float* b, float* o) {
	const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
	o[0] = x; o[1] = y; o[2] = z;
}
static inline void madd(float* o, const float* a, float s) { o[0] += a[0] * s; o[1] += a[1] * s; o[2] += a[2] * s; }
static inline float len(const float* a) { return sqrtf(dot(a, a)); }
static inline void normalise(float* a) {
	const float l = len(a);
	if (l > 1e-6f) { a[0] /= l; a[1] /= l; a[2] /= l; }
}
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float frand(float a, float b) { return a + (b - a) * (float)(rand() & 1023) * (1.0f / 1023.0f); }

const Stats& stats() { return s_stats; }
Vehicle* get(int id) { return (unsigned)id < MAX && g_cars[id].v.status != STATUS_FREE ? &g_cars[id].v : nullptr; }
int count() {
	int n = 0;
	for (const Car& c : g_cars) n += c.v.status != STATUS_FREE;
	return n;
}

bool isRemote(int id) { return id == 441 || id == 464 || id == 465 || id == 501 || id == 564 || id == 594; }

static Class classOf(const ds::VehInfo& h) {
	switch (h.type) {
	case ds::VEH_BIKE: case ds::VEH_BMX: return CLS_BIKE;
	case ds::VEH_BOAT: return CLS_BOAT;
	case ds::VEH_HELI: case ds::VEH_FHELI: return CLS_HELI;
	case ds::VEH_PLANE: case ds::VEH_FPLANE: return h.extKind == ds::VEHEXT_BOAT ? CLS_BOAT : CLS_PLANE;   // Skimmer floats
	case ds::VEH_TRAIN: return CLS_TRAIN;
	default: return CLS_AUTOMOBILE;
	}
}

void toWorld(const Vehicle& v, const float l[3], float w[3]) {
	for (int k = 0; k < 3; k++) w[k] = v.pos[k] + l[0] * v.right[k] + l[1] * v.fwd[k] + l[2] * v.up[k];
}
static void dirToWorld(const Vehicle& v, const float l[3], float w[3]) {
	for (int k = 0; k < 3; k++) w[k] = l[0] * v.right[k] + l[1] * v.fwd[k] + l[2] * v.up[k];
}
void toLocal(const Vehicle& v, const float w[3], float l[3]) {
	const float d[3] = { w[0] - v.pos[0], w[1] - v.pos[1], w[2] - v.pos[2] };
	l[0] = dot(d, v.right);
	l[1] = dot(d, v.fwd);
	l[2] = dot(d, v.up);
}
float heading(const Vehicle& v) { return atan2f(-v.fwd[0], v.fwd[1]); }

// bike lean: model space rotation about the forward axis through the ground line
static float leanPivot(const Car& c) {
	const Model* m = vehmdl::get(c.v.model);
	return m ? m->wheelPos[WHEEL_RL][2] - m->wheelRadius[1] : 0.0f;
}
void attachMatrix(const Vehicle& v, const float l[3], float ox, float oy, float oz, s32 out[12]) {
	float right[3], up[3], w[3];
	const float* rows[3] = { v.right, v.fwd, v.up };
	if (v.cls == CLS_BIKE && v.lean != 0.0f) {
		const int id = (int)((const Car*)&v - g_cars);   // Car::v is the first member
		const float zg = leanPivot(g_cars[id]);
		// the axes and the point turn about (0, y, zg) with the body
		const float s = sinf(v.lean), c = cosf(v.lean), dz = l[2] - zg;
		for (int k = 0; k < 3; k++) {
			right[k] = v.right[k] * c - v.up[k] * s;
			up[k] = v.right[k] * s + v.up[k] * c;
		}
		const float lx = l[0] * c + dz * s, lz = -l[0] * s + dz * c + zg;
		for (int k = 0; k < 3; k++) w[k] = v.pos[k] + lx * v.right[k] + l[1] * v.fwd[k] + lz * v.up[k];
		rows[0] = right;
		rows[2] = up;
	} else {
		toWorld(v, l, w);
	}
	for (int r = 0; r < 3; r++) {
		out[r * 3] = (s32)(rows[r][0] * 4096.0f);
		out[r * 3 + 1] = (s32)(rows[r][2] * 4096.0f);
		out[r * 3 + 2] = (s32)(-rows[r][1] * 4096.0f);
	}
	out[9] = (s32)((w[0] - ox) * 4096.0f);
	out[10] = (s32)((w[2] - oz) * 4096.0f);
	out[11] = (s32)(-(w[1] - oy) * 4096.0f);
}

// ---- pool -------------------------------------------------------------------------
int spawn(int vehIndex, float x, float y, float z, float hdg, int colour, const char* cutsceneModel) {
	int id = -1;
	for (int i = 0; i < MAX && id < 0; i++)
		if (g_cars[i].v.status == STATUS_FREE) id = i;
	if (id < 0 || !vehdata::spawnable(vehIndex)) return -1;
	Car& c = g_cars[id];
	c = {};
	if (!vehdata::read(vehIndex, c.h)) return -1;
	Vehicle& v = c.v;
	v.model = (int8_t)vehmdl::request(vehIndex, cutsceneModel);
	if (v.model < 0) return -1;
	v.status = STATUS_PHYSICS;
	v.type = c.h.type;
	v.cls = classOf(c.h);
	v.vehIndex = (int16_t)vehIndex;
	v.modelId = c.h.modelId;
	v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
	const float s = sinf(hdg), co = cosf(hdg);
	v.fwd[0] = -s; v.fwd[1] = co; v.fwd[2] = 0.0f;
	v.right[0] = co; v.right[1] = s; v.right[2] = 0.0f;
	v.up[0] = 0.0f; v.up[1] = 0.0f; v.up[2] = 1.0f;
	v.health = 1000.0f;
	v.gear = 1;
	v.handbrake = true;
	const int nc = c.h.numColours ? c.h.numColours : 1;
	const int pick = colour >= 0 ? colour % nc : rand() % nc;
	v.colPrim = c.h.colours[pick][0];
	v.colSec = c.h.colours[pick][1];
	pull(c);
	c.gear = 1;
	c.inertia2 = fx::ONE;
	c.track = -1;
	c.roadFrom = c.roadTo = c.roadNext = -1;
	v.audio = cutsceneModel ? -1 : aeveh::create(v.modelId);
	hud::log("spawn %.12s #%d cls %d", c.h.model, id, v.cls);
	return id;
}

void remove(int id) {
	if (!get(id)) return;
	Car& c = g_cars[id];
	if (c.v.npcDriver) s_stats.removed++;
	if (player::vehicleId() == id) player::leaveVehicleNow();
	aeveh::destroy(c.v.audio);
	vehmdl::release(c.v.model);
	for (Debris& d : s_debris)
		if (d.used && d.model == c.v.model) d.used = false;
	c.v.status = STATUS_FREE;
	c.v.npcDriver = false;
}

void removeAll() {
	for (int i = 0; i < MAX; i++) remove(i);
}

void setControls(int id, const Controls& ctl) {
	if (!get(id)) return;
	g_cars[id].pad = ctl;
}

void setDriver(int id, bool isPlayer) {
	Vehicle* v = get(id);
	if (!v || v->status == STATUS_WRECKED) return;
	v->status = isPlayer ? STATUS_PLAYER : STATUS_PHYSICS;
	v->npcDriver = false;
	if (isPlayer) v->playerOwned = true;
	if (isPlayer && !v->engineOn) {
		v->engineOn = v->type != ds::VEH_BMX;   // CAEVehicleAudioEntity::JustGotInVehicleAsDriver -> AE_START_ENGINE
		if (v->engineOn) aeveh::event(v->audio, aeveh::EV_START_ENGINE, v->pos[0], v->pos[1], v->pos[2]);
		if (v->type == ds::VEH_BMX) v->engineOn = true;   // pedals: "engine" = the rider
	}
	Car& c = g_cars[id];
	if (!isPlayer) {
		c.pad = {};
		c.pad.handbrake = true;
		v->engineOn = false;
	}
	v->asleep = false;
	c.fakePhysics = 0;
}

// distance (xy) from a point to the body box, and the door point for cars
int findEnterable(float x, float y, float z, float range) {
	int best = -1;
	float bd = range * range;
	for (int i = 0; i < MAX; i++) {
		Car& c = g_cars[i];
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || v.status == STATUS_WRECKED || !c.placed || v.up[2] < 0.5f) continue;
		if (!vehdata::enterable(v.vehIndex)) continue;
		const Model* m = vehmdl::get(v.model);
		if (!m) continue;
		const float p[3] = { x, y, z };
		float l[3];
		toLocal(v, p, l);
		if (fabsf(l[2] - m->seat[2]) > 4.0f) continue;
		float d;
		if (v.cls == CLS_AUTOMOBILE && !isRemote(v.modelId)) {
			const float dl[3] = { m->bmin[0] - 0.4f, m->seat[1], 0.0f };
			float w[3];
			toWorld(v, dl, w);
			d = (w[0] - x) * (w[0] - x) + (w[1] - y) * (w[1] - y);
		} else {
			const float dx = l[0] < m->bmin[0] ? m->bmin[0] - l[0] : l[0] > m->bmax[0] ? l[0] - m->bmax[0] : 0.0f;
			const float dy = l[1] < m->bmin[1] ? m->bmin[1] - l[1] : l[1] > m->bmax[1] ? l[1] - m->bmax[1] : 0.0f;
			d = (dx + 1.0f) * (dx + 1.0f) + dy * dy;
		}
		if (d < bd) { bd = d; best = i; }
	}
	return best;
}

void setDoor(int id, int door, float ratio) {
	Vehicle* v = get(id);
	if (!v || (unsigned)door >= NUM_DOORS) return;
	Car& c = g_cars[id];
	ratio = clampf(ratio, 0.0f, 1.0f);
	const float old = v->door[door];
	v->door[door] = ratio;
	const uint32_t bit = 1u << door;
	if (ratio > 0.05f && !(c.doorSound & bit)) {
		c.doorSound |= bit;
		aeveh::event(v->audio, aeveh::EV_DOOR_OPEN, v->pos[0], v->pos[1], v->pos[2]);
	}
	if (ratio < 0.02f && old >= 0.02f && (c.doorSound & bit)) {
		c.doorSound &= ~bit;
		aeveh::event(v->audio, aeveh::EV_DOOR_CLOSE, v->pos[0], v->pos[1], v->pos[2]);
	}
}

// ---- damage (CAutomobile::VehicleDamage + CDamageManager, reduced) ----------------
static bool detachable(int comp) { return comp >= COMP_DOOR_LF && comp <= COMP_BUMP_REAR; }

static void spawnDebris(Car& c, int comp) {
	Model* m = vehmdl::get(c.v.model);
	if (!m) return;
	for (int i = 0; i < m->numParts; i++) {
		const Part& p = m->parts[i];
		if (p.comp != comp || p.variant == VAR_OK) continue;   // the damaged mesh flies off (or the only one)
		for (Debris& d : s_debris) {
			if (d.used) continue;
			d = {};
			d.used = true;
			d.model = c.v.model;
			d.part = (uint8_t)i;
			memcpy(d.pos, c.v.pos, sizeof d.pos);
			memcpy(d.r, c.v.right, 12);
			memcpy(d.r + 3, c.v.fwd, 12);
			memcpy(d.r + 6, c.v.up, 12);
			for (int k = 0; k < 3; k++) d.vel[k] = c.v.moveSpeed[k] * 50.0f;
			float side[3];
			const float l[3] = { m->hinge[comp][0], m->hinge[comp][1], m->hinge[comp][2] };
			dirToWorld(c.v, l, side);
			normalise(side);
			madd(d.vel, side, frand(2.0f, 4.0f));
			d.vel[2] += frand(2.0f, 5.0f);
			d.w[0] = frand(-6.0f, 6.0f); d.w[1] = frand(-6.0f, 6.0f); d.w[2] = frand(-3.0f, 3.0f);
			d.life = 8.0f;
			break;
		}
		if (p.variant == VAR_DAM || !(c.hasDam & (1u << comp))) break;
	}
}

static void damageComp(Car& c, int comp, float amount) {
	Vehicle& v = c.v;
	if (comp <= COMP_WHEEL || comp >= COMP_COUNT || v.dmg[comp] == DMG_MISSING) return;
	v.compDamage[comp] += amount;
	if (amount > 150.0f && v.dmg[comp] == DMG_OK) {
		v.dmg[comp] = DMG_DAMAGED;
		if (comp == COMP_WINDSCREEN && !c.windscreenSaid) {
			c.windscreenSaid = true;
			aeveh::event(v.audio, aeveh::EV_WINDSCREEN, v.pos[0], v.pos[1], v.pos[2]);
		}
	}
	if (detachable(comp) && v.compDamage[comp] > 2500.0f && v.dmg[comp] == DMG_DAMAGED) {
		spawnDebris(c, comp);
		v.dmg[comp] = DMG_MISSING;
		aeveh::event(v.audio, aeveh::EV_PART_OFF, v.pos[0], v.pos[1], v.pos[2]);
	}
}

void vehicleDamage(Car& c) {
	Vehicle& v = c.v;
	const float di = c.damageIntensity;
	c.damageIntensity = 0.0f;
	if (v.status == STATUS_WRECKED) return;
	const float minDmg = c.h.mass / 1500.0f * 25.0f;
	if (di <= minDmg) return;
	aeveh::event(v.audio, aeveh::EV_CRASH, c.impactPos[0], c.impactPos[1], c.impactPos[2], di);
	v.health -= (di - minDmg) * c.h.collisionDamageMult * 0.6f;
	if (v.health < 0.0f) v.health = 0.0f;
	const Model* m = vehmdl::get(v.model);
	if (!m) return;
	float l[3];
	toLocal(v, c.impactPos, l);
	const float amount = di * 4.0f * c.h.collisionDamageMult;
	const bool rearDoors = m->hinge[COMP_DOOR_LR][1] != 0.0f || m->hinge[COMP_DOOR_LR][0] != 0.0f;
	if (l[1] > m->bmax[1] * 0.6f) {
		damageComp(c, COMP_BUMP_FRONT, amount);
		if (v.dmg[COMP_BUMP_FRONT] != DMG_OK && di > c.h.mass * 0.2f) damageComp(c, COMP_BONNET, amount);
		if (v.dmg[COMP_BUMP_FRONT] != DMG_OK && di > c.h.mass * 0.25f) damageComp(c, COMP_WINDSCREEN, amount);
	} else if (l[1] < m->bmin[1] * 0.6f) {
		damageComp(c, COMP_BUMP_REAR, amount);
		if (v.dmg[COMP_BUMP_REAR] != DMG_OK) damageComp(c, COMP_BOOT, amount);
	} else if (l[2] > m->bmax[2] * 0.6f) {
		damageComp(c, COMP_WINDSCREEN, amount);
	} else {
		const bool left = l[0] < 0.0f, front = !rearDoors || l[1] > m->seat[1] - 0.5f;
		damageComp(c, left ? (front ? COMP_DOOR_LF : COMP_DOOR_LR) : (front ? COMP_DOOR_RF : COMP_DOOR_RR), amount);
	}
}

static void startFx(const float p[3]) {
	Fx* f = &s_fx[0];
	for (Fx& x : s_fx)
		if (!x.used) { f = &x; break; }
	*f = { true, { p[0], p[1], p[2] }, 0.0f };
}

// CAutomobile::BlowUpCar
void blowUp(int id) {
	Vehicle* vp = get(id);
	if (!vp || vp->status == STATUS_WRECKED) return;
	Car& c = g_cars[id];
	Vehicle& v = *vp;
	const bool driven = player::vehicleId() == id;
	v.status = STATUS_WRECKED;
	v.npcDriver = false;
	v.health = 0.0f;
	v.engineOn = false;
	v.asleep = false;
	c.fakePhysics = 0;
	c.wreckedMs = 0;
	c.wreckBurn = 20000.0f;
	c.move.z += fx::F(0.13);
	c.turn.x += fx::fromFloat(frand(-0.04f, 0.04f));
	c.turn.y += fx::fromFloat(frand(-0.04f, 0.04f));
	for (int comp : { COMP_BONNET, COMP_BOOT, COMP_DOOR_LF, COMP_DOOR_RF, COMP_DOOR_LR, COMP_DOOR_RR })
		if (v.dmg[comp] != DMG_MISSING && (comp == COMP_BONNET || rand() % 3 == 0)) {
			if (v.dmg[comp] == DMG_OK) v.dmg[comp] = DMG_DAMAGED;
			spawnDebris(c, comp);
			v.dmg[comp] = DMG_MISSING;
		}
	if (c.hasDam & (1u << COMP_WINDSCREEN)) v.dmg[COMP_WINDSCREEN] = DMG_DAMAGED;
	startFx(v.pos);
	aeveh::event(v.audio, aeveh::EV_EXPLOSION, v.pos[0], v.pos[1], v.pos[2]);
	// CExplosion::AddExplosion (EXPLOSION_CAR): the player inside or near takes it
	const float dx = player::g.x - v.pos[0], dy = player::g.y - v.pos[1], dz = player::g.z - v.pos[2];
	const float d = sqrtf(dx * dx + dy * dy + dz * dz);
	if (driven) player::vehicleExploded(id);
	else if (d < 7.0f) player::explosionDamage(v.pos[0], v.pos[1], v.pos[2], (1.0f - d / 7.0f) * 80.0f);
	peds::explosion(v.pos[0], v.pos[1], v.pos[2], 7.0f, 120.0f);
	// wake every vehicle in the blast
	for (Car& o : g_cars) {
		if (o.v.status == STATUS_FREE || &o == &c) continue;
		const float ex = o.v.pos[0] - v.pos[0], ey = o.v.pos[1] - v.pos[1];
		const float e = sqrtf(ex * ex + ey * ey);
		if (e < 8.0f) {
			o.v.asleep = false;
			o.fakePhysics = 0;
			o.move.x += fx::fromFloat(ex / (e + 0.5f) * 0.05f);
			o.move.y += fx::fromFloat(ey / (e + 0.5f) * 0.05f);
			o.move.z += fx::F(0.06);
			o.v.health -= (1.0f - e / 8.0f) * 500.0f;
		}
	}
	hud::log("veh #%d blew up", id);
}

void repair(int id) {
	Vehicle* v = get(id);
	if (!v) return;
	v->health = 1000.0f;
	v->burnTimer = 0.0f;
	v->burst = 0;
	memset(v->dmg, 0, sizeof v->dmg);
	memset(v->compDamage, 0, sizeof v->compDamage);
	if (v->status == STATUS_WRECKED) v->status = player::vehicleId() == id ? STATUS_PLAYER : STATUS_PHYSICS;
	if (v->status == STATUS_PLAYER) v->engineOn = true;
	g_cars[id].windscreenSaid = false;
	g_cars[id].wreckBurn = 0.0f;
}

void flip(int id) {
	Vehicle* v = get(id);
	if (!v) return;
	v->up[0] = 0.0f; v->up[1] = 0.0f; v->up[2] = 1.0f;
	v->fwd[2] = 0.0f;
	if (len(v->fwd) < 0.1f) { v->fwd[0] = 0.0f; v->fwd[1] = 1.0f; }
	normalise(v->fwd);
	cross(v->fwd, v->up, v->right);
	v->pos[2] += 1.5f;
	memset(v->moveSpeed, 0, 12);
	Car& c = g_cars[id];
	pull(c);
	c.turn = { 0, 0, 0 };
	v->asleep = false;
	c.fakePhysics = 0;
}

void setHealth(int id, float h) {
	if (Vehicle* v = get(id)) v->health = h;
}

// CAutomobile::BurstTyre
void burstTyre(int id, int wheel) {
	Vehicle* v = get(id);
	if (!v || v->cls == CLS_BOAT || v->cls == CLS_TRAIN) return;
	const uint8_t bits = wheel < 0 ? 0x0F : (uint8_t)(1u << (wheel & 3));
	if ((v->burst & bits) == bits) return;
	v->burst |= bits;
	v->asleep = false;
	g_cars[id].fakePhysics = 0;
	aeveh::event(v->audio, aeveh::EV_TYRE_BURST, v->pos[0], v->pos[1], v->pos[2]);
}

// CWeapon::DoBulletImpact on a vehicle: the wheels first (spheres at the
// wheel centres), then the body box
bool bulletHit(const float a[3], const float b[3], float damage) {
	float bestT = 1.0f;
	int bestCar = -1, bestWheel = -1;
	for (int i = 0; i < MAX; i++) {
		Car& c = g_cars[i];
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || !c.placed || player::vehicleId() == i) continue;
		const Model* m = vehmdl::get(v.model);
		if (!m) continue;
		float la[3], lb[3];
		toLocal(v, a, la);
		toLocal(v, b, lb);
		const float d[3] = { lb[0] - la[0], lb[1] - la[1], lb[2] - la[2] };
		// slab test against the body box (+ the wheels)
		float t0 = 0.0f, t1 = bestT;
		bool miss = false;
		for (int k = 0; k < 3 && !miss; k++) {
			const float lo = m->bmin[k] - (k == 2 ? 0.5f : 0.1f), hi = m->bmax[k] + 0.1f;
			if (fabsf(d[k]) < 1e-6f) { miss = la[k] < lo || la[k] > hi; continue; }
			float ta = (lo - la[k]) / d[k], tb = (hi - la[k]) / d[k];
			if (ta > tb) { const float s = ta; ta = tb; tb = s; }
			if (ta > t0) t0 = ta;
			if (tb < t1) t1 = tb;
			miss = t0 > t1;
		}
		if (miss) continue;
		int wheel = -1;
		if (v.cls != CLS_BOAT && v.cls != CLS_TRAIN)
			for (int w = 0; w < 4; w++) {
				const float r = m->wheelRadius[w == WHEEL_FL || w == WHEEL_FR ? 0 : 1] + 0.08f;
				const float* wc = m->wheelPos[w];
				float best = 1e9f, tw = 0.0f;
				for (int s = 0; s <= 8; s++) {   // closest approach (a few samples are plenty)
					const float t = t0 + (t1 - t0) * (float)s / 8.0f;
					const float e[3] = { la[0] + d[0] * t - wc[0], la[1] + d[1] * t - wc[1], la[2] + d[2] * t - (wc[2] + m->wheelPos[w][2] * 0.0f) };
					const float dd = dot(e, e);
					if (dd < best) { best = dd; tw = t; }
				}
				if (best < r * r && tw <= t0 + (t1 - t0) && (wheel < 0 || tw < t0)) { wheel = w; t0 = tw; }
			}
		if (t0 < bestT) { bestT = t0; bestCar = i; bestWheel = wheel; }
	}
	if (bestCar < 0) return false;
	Vehicle& v = g_cars[bestCar].v;
	if (bestWheel >= 0) burstTyre(bestCar, bestWheel);
	if (v.status != STATUS_WRECKED) v.health -= damage * 0.5f;
	v.asleep = false;
	g_cars[bestCar].fakePhysics = 0;
	return true;
}

// ---- simulation ---------------------------------------------------------------------
static bool inRange(const Vehicle& v) {
	const float dx = v.pos[0] - player::g.x, dy = v.pos[1] - player::g.y;
	return dx * dx + dy * dy < ACTIVE_RANGE * ACTIVE_RANGE;
}

static float roadX(const ds::PathNode* n) { return n->x * 0.125f; }
static float roadY(const ds::PathNode* n) { return n->y * 0.125f; }
static bool roadOpen(const ds::PathNode* n) { return n && !(n->flags & (ds::ROAD_SWITCHED_OFF | ds::ROAD_BLOCK | ds::ROAD_WATER)); }

static int trafficModel(const ds::PedZone* zone) {
	if (!zone) return -1;
	ds::PopRow row;
	const int wd = player::g_weekday, hour = (int)player::g_clockH;
	const int weekend = wd == 7 || wd == 0 ? 1 : wd == 1 ? hour < 20 : wd == 6 ? hour >= 20 : 0;
	if (!peddata::popRow(zone->popType, weekend, hour / 2, row) || !row.maxCars) return -1;
	int gangSum = 0;
	for (int g = 0; g < 10; g++) gangSum += zone->gang[g];
	const int gangWeight = (int)fminf(50.0f, (float)gangSum * 0.5f) * row.percGang / 100;
	const int dealerWeight = zone->dealer * row.percDealers / 100;
	for (int attempt = 0; attempt < 12; attempt++) {
		int group = -1;
		const int category = rand() % 100;
		if (category < gangWeight && gangSum) {
			int pick = rand() % gangSum;
			for (int g = 0; g < 10; g++) {
				if (pick < zone->gang[g]) { group = 18 + g; break; }
				pick -= zone->gang[g];
			}
		} else if (category < gangWeight + dealerWeight) group = 28;
		if (group < 0) {
			int total = 0;
			for (int g = 0; g < peddata::NUM_PERC_GROUPS; g++) total += row.group[g];
			if (!total) return -1;
			int pick = rand() % total;
			for (int g = 0; g < peddata::NUM_PERC_GROUPS; g++) {
				if (pick < row.group[g]) { group = g; break; }
				pick -= row.group[g];
			}
		}
		const int n = peddata::carGroupSize(group);
		if (!n) continue;
		const int index = peddata::carGroupMember(group, rand() % n);
		if (vehdata::type(index) == ds::VEH_CAR || vehdata::type(index) == ds::VEH_MTRUCK) return index;
	}
	return -1;
}

static int chooseRoad(int from, int at) {
	const ds::PathNode* n = peddata::roadNode(at);
	if (!roadOpen(n)) return -1;
	int pick = -1, choices = 0;
	for (int k = 0; k < n->numLinks; k++) {
		const int b = peddata::roadLink(n, k);
		const ds::PathNode* to = peddata::roadNode(b);
		if (!roadOpen(to) || b == from) continue;
		const float dx = roadX(to) - roadX(n), dy = roadY(to) - roadY(n);
		if (dx * dx + dy * dy < 1.0f || fabsf(to->z - n->z) > sqrtf(dx * dx + dy * dy) * 4.0f) continue;
		if (!(rand() % ++choices)) pick = b;
	}
	return pick >= 0 ? pick : roadOpen(peddata::roadNode(from)) ? from : -1;
}

static void nextRoad(Car& c) {
	const int pick = c.roadNext >= 0 ? c.roadNext : chooseRoad(c.roadFrom, c.roadTo);
	c.roadFrom = c.roadTo;
	c.roadTo = (int16_t)pick;
	c.roadNext = -1;
	if (pick < 0) c.v.npcDriver = false;
}

static void driveTraffic(Car& c, bool physics) {
	Vehicle& v = c.v;
	const ds::PathNode* a = peddata::roadNode(c.roadFrom);
	const ds::PathNode* b = peddata::roadNode(c.roadTo);
	if (!roadOpen(a) || !roadOpen(b)) { v.npcDriver = false; return; }
	float sx = roadX(b) - roadX(a), sy = roadY(b) - roadY(a);
	const float length = sqrtf(sx * sx + sy * sy);
	if (length < 1.0f) { nextRoad(c); return; }
	const float laneX = sy * 1.35f / length, laneY = -sx * 1.35f / length;
	float tx = roadX(b) + laneX - v.pos[0], ty = roadY(b) + laneY - v.pos[1];
	if (tx * tx + ty * ty < 9.0f) { nextRoad(c); return; }
	if (c.roadNext < 0) c.roadNext = (int16_t)chooseRoad(c.roadFrom, c.roadTo);
	float corner = 0.0f;
	const ds::PathNode* next = peddata::roadNode(c.roadNext);
	if (next) {
		const float nx = roadX(next) - roadX(b), ny = roadY(next) - roadY(b);
		const float nl = sqrtf(nx * nx + ny * ny);
		if (nl > 1.0f) {
			corner = clampf(1.0f - (sx * nx + sy * ny) / (length * nl), 0.0f, 2.0f);
			const float dist = sqrtf(tx * tx + ty * ty);
			const float blend = clampf((12.0f - dist) / 9.0f, 0.0f, 1.0f);
			tx += ((nx * 4.0f + ny * 1.35f) / nl - laneX) * blend;
			ty += ((ny * 4.0f - nx * 1.35f) / nl - laneY) * blend;
		}
	}
	const float cross = v.fwd[0] * ty - v.fwd[1] * tx;
	const float along = v.fwd[0] * tx + v.fwd[1] * ty;
	const float angle = atan2f(-cross, along);
	v.handbrake = false;
	v.engineOn = true;
	v.asleep = false;
	c.pad = { clampf(angle * 1.2f, -0.7f, 0.7f), 0.35f, 0.0f, 0.0f, 0.0f, false, false };
	float gap = 100.0f;
	for (const Car& other : g_cars) {
		if (&other == &c || other.v.status == STATUS_FREE || !other.placed) continue;
		const float dx = other.v.pos[0] - v.pos[0], dy = other.v.pos[1] - v.pos[1];
		const float ahead = dx * v.fwd[0] + dy * v.fwd[1];
		const float side = fabsf(dx * v.right[0] + dy * v.right[1]);
		if (ahead > 0.0f && ahead < gap && side < 3.2f) gap = ahead;
	}
	const float dist = sqrtf(tx * tx + ty * ty);
	const float turnSpeed = dist < 14.0f ? 0.09f - 0.055f * fminf(corner, 1.0f) : 0.09f;
	const float gapSpeed = gap < 5.0f ? 0.0f : gap < 9.0f ? 0.025f : 0.09f;
	const float targetSpeed = fminf(turnSpeed, gapSpeed);
	if (gap < 7.0f || (physics && v.speedFwd > targetSpeed + 0.01f)) {
		c.pad.accel = 0.0f;
		c.pad.brake = gap < 4.0f ? 0.8f : 0.2f;
	}
	if (physics) return;
	const float speed = clampf(v.speedFwd + clampf(targetSpeed - v.speedFwd, -0.005f, 0.003f), 0.0f, 0.09f);
	const float turn = clampf(angle, -0.025f, 0.025f);
	const float h = heading(v) - turn;
	const float sn = sinf(h), cs = cosf(h);
	v.fwd[0] = -sn; v.fwd[1] = cs;
	v.right[0] = cs; v.right[1] = sn;
	v.pos[0] += v.fwd[0] * speed;
	v.pos[1] += v.fwd[1] * speed;
	v.moveSpeed[0] = v.fwd[0] * speed;
	v.moveSpeed[1] = v.fwd[1] * speed;
	v.moveSpeed[2] = 0.0f;
	v.speedFwd = speed;
	v.skid = 0.0f;
	pull(c);
}

void step() {
	const u32 t0 = timing::ticks();
	uint16_t awake = 0;
	for (int i = 0; i < MAX; i++) {
		Car& c = g_cars[i];
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || !c.placed) continue;
		const bool remote = player::vehicleId() == i;   // driven / remote-controlled: always simulated
		if (v.npcDriver && !remote) {
			const bool physics = inRange(v) && col::available();
			driveTraffic(c, physics);
			if (!v.npcDriver || !physics) continue;
		}
		if (!remote && v.cls != CLS_TRAIN && (!inRange(v) || !col::available())) continue;
		if (v.asleep && !remote) continue;
		physStep(c);
		awake++;
	}
	for (int a = 0; a < MAX; a++)
		for (int b = a + 1; b < MAX; b++) {
			Car& A = g_cars[a];
			Car& B = g_cars[b];
			if (A.v.status == STATUS_FREE || B.v.status == STATUS_FREE || !A.placed || !B.placed) continue;
			if (A.v.cls == CLS_TRAIN || B.v.cls == CLS_TRAIN) continue;
			if ((A.v.asleep || !inRange(A.v)) && (B.v.asleep || !inRange(B.v))) continue;
			physPair(A, B);
		}
	// debris (CObject flying panels): gravity, tumble, bounce on the ground
	for (Debris& d : s_debris) {
		if (!d.used) continue;
		const float dt = 1.0f / 60.0f;
		d.life -= dt;
		if (d.life <= 0.0f || !vehmdl::ready(d.model)) { d.used = false; continue; }
		d.vel[2] -= 9.8f * dt * 2.0f;
		const float z0 = d.pos[2];
		madd(d.pos, d.vel, dt);
		const float w[3] = { d.w[0] * dt, d.w[1] * dt, d.w[2] * dt };
		for (int r = 0; r < 3; r++) {
			float t[3];
			cross(w, d.r + r * 3, t);
			for (int q3 = 0; q3 < 3; q3++) d.r[r * 3 + q3] += t[q3];
		}
		normalise(d.r + 3);
		cross(d.r + 3, d.r + 6, d.r);
		normalise(d.r);
		cross(d.r, d.r + 3, d.r + 6);
		col::Hit h;
		if (d.vel[2] < 0.0f && col::groundAt(d.pos[0], d.pos[1], z0 + 0.3f, d.pos[2] - 0.05f, h)) {
			d.pos[2] = h.z + 0.05f;
			d.vel[2] = -d.vel[2] * 0.3f;
			d.vel[0] *= 0.6f;
			d.vel[1] *= 0.6f;
			for (float& x : d.w) x *= 0.5f;
		}
	}
	s_stats.awake = awake;
	s_stats.stepUs = timing::usSince(t0);
	s_stats.queryUs = timerTicks2usec(g_colTicks);
	g_colTicks = 0;
}

static uint32_t s_trafficFrame;

static void spawnTraffic() {
	if (!peddata::numRoadNodes() || count() >= MAX - 2) return;
	int traffic = 0;
	for (const Car& c : g_cars) traffic += c.v.status != STATUS_FREE && c.v.npcDriver;
	if (traffic >= 2) return;
	s_stats.trafficAttempts++;
	int cand[64], nc = 0;
	peddata::forRoadNodesNear(player::g.x, player::g.y, 72.0f, [&](int i) {
		const ds::PathNode* a = peddata::roadNode(i);
		if (!roadOpen(a) || !a->numLinks || !(a->flags >> 4)) return true;
		if (nc < 64) cand[nc] = i;
		else {
			const int j = rand() % (nc + 1);
			if (j < 64) cand[j] = i;
		}
		nc++;
		return true;
	});
	s_stats.trafficCandidates = (uint16_t)nc;
	const int attempts = nc < 24 ? nc : 24;
	for (int pass = 0; pass < 2; pass++)
	for (int tryNode = 0; tryNode < attempts; tryNode++) {
		const int from = cand[rand() % (nc < 64 ? nc : 64)];
		const ds::PathNode* a = peddata::roadNode(from);
		const int to = peddata::roadLink(a, rand() % a->numLinks);
		const ds::PathNode* b = peddata::roadNode(to);
		if (!roadOpen(b) || !(b->flags >> 4)) continue;
		const float sx = roadX(b) - roadX(a), sy = roadY(b) - roadY(a);
		const float length = sqrtf(sx * sx + sy * sy);
		if (length < 8.0f || fabsf(b->z - a->z) > length * 4.0f) continue;
		const float t = frand(0.2f, 0.8f);
		const float x = roadX(a) + sx * t + sy * 1.35f / length;
		const float y = roadY(a) + sy * t - sx * 1.35f / length;
		const float z = (a->z + (b->z - a->z) * t) * 0.125f;
		const float dx = x - player::g.x, dy = y - player::g.y;
		const float d2 = dx * dx + dy * dy;
		if (d2 < 22.0f * 22.0f || d2 > 65.0f * 65.0f || fabsf(z - player::g.z) > 12.0f) continue;
		const bool seen = peds::visible(x, y, z + 1.0f, 2.5f);
		if (pass == 0 ? seen : !seen || d2 < 48.0f * 48.0f) continue;
		bool clear = true;
		for (const Car& c : g_cars) {
			if (c.v.status == STATUS_FREE) continue;
			const float cx = x - c.v.pos[0], cy = y - c.v.pos[1];
			if (cx * cx + cy * cy < 9.0f * 9.0f) { clear = false; break; }
		}
		if (!clear) continue;
		s_stats.trafficSpots++;
		const int index = trafficModel(peddata::zoneAt(x, y));
		if (index < 0) { s_stats.trafficModelMiss++; continue; }
		const int id = spawn(index, x, y, z, atan2f(-sx, sy));
		if (id < 0) { s_stats.trafficSpawnFail++; return; }
		Car& c = g_cars[id];
		c.roadFrom = (int16_t)from;
		c.roadTo = (int16_t)to;
		c.roadNext = -1;
		c.v.npcDriver = true;
		c.trafficBorn = s_trafficFrame;
		c.trafficSeen = false;
		s_stats.spawned++;
		return;
	}
}

static void setupFromModel(Car& c) {
	Vehicle& v = c.v;
	const Model* m = vehmdl::get(v.model);
	c.hasDam = 0;
	for (int i = 0; i < m->numParts; i++)
		if (m->parts[i].variant == VAR_DAM) c.hasDam |= 1u << m->parts[i].comp;
	physSetup(c);
	publish(c);
	vehmdl::setColours(*vehmdl::get(v.model), 0, 0, false);
	c.placed = true;
	v.asleep = false;
	c.fakePhysics = 0;
}

bool prepareCutscene(int id) {
	Vehicle* v = get(id);
	if (!v) return false;
	Car& c = g_cars[id];
	if (!c.placed && vehmdl::ready(v->model)) setupFromModel(c);
	return c.placed;
}

void positionCutscene(int id, float x, float y, float z, float heading) {
	Vehicle* v = get(id);
	if (!v) return;
	v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
	const float s = sinf(heading), c = cosf(heading);
	v->fwd[0] = -s; v->fwd[1] = c; v->fwd[2] = 0.0f;
	v->right[0] = c; v->right[1] = s; v->right[2] = 0.0f;
	pull(g_cars[id]);
}

void update() {
	vehmdl::update();
	++s_trafficFrame;
	if (s_trafficFrame % 90 == 0) spawnTraffic();
	skid::update();
	static uint32_t s_lastVbl;
	const uint32_t vb = timing::vblanks();
	const float frameMs = (float)((vb - s_lastVbl) > 6 ? 6 : vb - s_lastVbl) * (1000.0f / 60.0f) * (float)dbg::opts.simSpeedQ / 4.0f;
	s_lastVbl = vb;
	uint16_t active = 0, traffic = 0;
	for (int i = 0; i < MAX; i++) {
		Car& c = g_cars[i];
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE) continue;
		const bool seen = peds::visible(v.pos[0], v.pos[1], v.pos[2] + 1.0f, 3.5f);
		if (v.npcDriver && seen) { c.trafficSeen = true; c.trafficOffscreen = 0; }
		else if (v.npcDriver && c.trafficOffscreen < 600) c.trafficOffscreen++;
		if (v.status != STATUS_PLAYER && !v.playerOwned &&
		    !seen) {
			const float dx = v.pos[0] - player::g.x, dy = v.pos[1] - player::g.y;
			if ((v.npcDriver && ((c.trafficSeen && c.trafficOffscreen > 120 &&
			                      dx * dx + dy * dy > 48.0f * 48.0f) ||
			                     (!c.trafficSeen && s_trafficFrame - c.trafficBorn > 360))) ||
			    dx * dx + dy * dy > (v.npcDriver ? 80.0f * 80.0f : 45.0f * 45.0f)) { remove(i); continue; }
		}
		active++;
		traffic += v.npcDriver;
		if (!c.placed) {
			const Model* m = vehmdl::get(v.model);
			if (!m || m->state == STATE_FAILED) { hud::log("\x1b[33mveh #%d: model failed\x1b[39m", i); remove(i); continue; }
			if (m->state == STATE_READY && !col::pending()) setupFromModel(c);
			continue;
		}
		if (v.status == STATUS_WRECKED && c.wreckBurn > 0.0f) c.wreckBurn -= frameMs;
		// CAutomobile::ProcessCarOnFireAndExplode (per frame: a parked car sleeps)
		if (v.status != STATUS_WRECKED && v.health < 250.0f) {
			v.burnTimer += frameMs;
			if (v.burnTimer > 5000.0f) {
				blowUp(i);
				continue;
			}
		}
		const Gear& g = c.gears[c.gear];
		const float gd = fx::toFloat(g.down), gm = fx::toFloat(g.maxVel);
		aeveh::Params p;
		p.x = v.pos[0]; p.y = v.pos[1]; p.z = v.pos[2];
		p.speed = v.cls == CLS_HELI || v.cls == CLS_PLANE ? v.rotor * c.h.maxVelocity : v.speedFwd;
		p.maxVelocity = c.h.maxVelocity;
		p.revs = v.cls == CLS_HELI || v.cls == CLS_PLANE ? v.rotor
		         : v.gear ? clampf((fabsf(v.speedFwd) - gd) / (gm - gd + 1e-4f), 0.0f, 1.0f) : 0.0f;
		p.gas = v.cls == CLS_HELI || v.cls == CLS_PLANE ? clampf(c.pad.accel - c.pad.brake, -1.0f, 1.0f) * (v.engineOn ? 1.0f : 0.0f) : v.gas;
		p.gear = v.gear;
		p.numGears = c.h.numGears;
		p.engineOn = v.engineOn && v.type != ds::VEH_BMX;
		p.player = v.status == STATUS_PLAYER;
		p.wrecked = v.status == STATUS_WRECKED;
		p.horn = v.horn && v.cls != CLS_HELI && v.cls != CLS_PLANE;
		p.onFire = (v.status != STATUS_WRECKED && v.health < 250.0f) || (v.status == STATUS_WRECKED && c.wreckBurn > 0.0f);
		p.skid = v.skid;
		p.surface = c.surf[WHEEL_RL];
		aeveh::update(v.audio, p);
	}
	for (Fx& f : s_fx)
		if (f.used && (f.t += frameMs / 1000.0f) > 3.0f) f.used = false;
	s_stats.active = active;
	s_stats.traffic = traffic;
	s_stats.skids = (uint16_t)skid::active();
}

// Ped vs the vehicle bodies: box in vehicle space, horizontal push.
bool pushPed(float& x, float& y, float z, float r, int skip, PedHit* hit) {
	bool touched = false;
	if (hit) hit->id = -1;
	for (int i = 0; i < MAX; i++) {
		Car& c = g_cars[i];
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || !c.placed || i == skip) continue;
		const Model* m = vehmdl::get(v.model);
		if (!m) continue;
		const float w[3] = { x, y, z };
		float l[3];
		toLocal(v, w, l);
		if (l[2] < m->bmin[2] - 0.3f || l[2] > m->bmax[2] + 0.2f) continue;
		const float x0 = m->bmin[0] - r, x1 = m->bmax[0] + r, y0 = m->bmin[1] - r, y1 = m->bmax[1] + r;
		if (l[0] <= x0 || l[0] >= x1 || l[1] <= y0 || l[1] >= y1) continue;
		const float px[4] = { x0 - l[0], x1 - l[0], y0 - l[1], y1 - l[1] };
		int best = 0;
		for (int k = 1; k < 4; k++)
			if (fabsf(px[k]) < fabsf(px[best])) best = k;
		const float d[3] = { best < 2 ? px[best] : 0.0f, best >= 2 ? px[best] : 0.0f, 0.0f };
		float dw[3];
		dirToWorld(v, d, dw);
		x += dw[0];
		y += dw[1];
		touched = true;
		const float spd = len(v.moveSpeed);
		if (hit && spd > (hit->id < 0 ? 0.0f : hit->speed)) *hit = { i, v.moveSpeed[0], v.moveSpeed[1], spd };
	}
	return touched;
}

void pedHitSound(int id, float x, float y, float z) {
	if (const Vehicle* v = get(id)) aeveh::event(v->audio, aeveh::EV_PED_HIT, x, y, z);
}

// ---- drawing ------------------------------------------------------------------------
static inline void mult4x3(const s32* m) {
	for (int k = 0; k < 12; k++) MATRIX_MULT4x3 = m[k];
}

static void bodyMatrix(const float* right, const float* fwd, const float* up, const float* pos, const View& w, s32* o) {
	const float* rows[3] = { right, fwd, up };
	for (int r = 0; r < 3; r++) {
		o[r * 3] = (s32)(rows[r][0] * 4096.0f);
		o[r * 3 + 1] = (s32)(rows[r][2] * 4096.0f);
		o[r * 3 + 2] = (s32)(-rows[r][1] * 4096.0f);
	}
	o[9] = (s32)((pos[0] - w.originX) * 4096.0f);
	o[10] = (s32)((pos[2] - w.originZ) * 4096.0f);
	o[11] = (s32)(-(pos[1] - w.originY) * 4096.0f);
}

static void callPart(const Model& m, const Part& p) {
	if (m.scaleExp) {
		MATRIX_CONTROL = GL_POSITION;   // scale the positions only, normals stay unit
		const s32 s = 4096 << m.scaleExp;
		MATRIX_SCALE = s; MATRIX_SCALE = s; MATRIX_SCALE = s;
		MATRIX_CONTROL = GL_MODELVIEW;
	}
	gx::callList(p.list);
}

static bool partVisible(const Car& c, const Part& p) {
	if (p.vis == VIS_FAST && c.v.rotor < 0.6f) return false;
	if (p.vis == VIS_SLOW && c.v.rotor >= 0.6f) return false;
	if (p.comp <= COMP_WHEEL) return p.comp != COMP_WHEEL;
	const uint8_t st = c.v.dmg[p.comp];
	if (st == DMG_MISSING) return false;
	if (p.variant == VAR_ANY) return true;
	const bool dam = st == DMG_DAMAGED && (c.hasDam & (1u << p.comp));
	return p.variant == (dam ? VAR_DAM : VAR_OK);
}

// rotation about a model axis (0 x, 1 y, 2 z), angle in radians
static void rotAxis(int axis, float a) {
	if (a == 0.0f) return;
	const s16 ang = (s16)((int)(a * 5215.19f) & 0x7FFF);
	const s32 s = sinLerp(ang), co = cosLerp(ang);
	s32 m[9] = { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 };
	const int i = (axis + 1) % 3, j = (axis + 2) % 3;
	m[i * 3 + i] = co; m[i * 3 + j] = s;
	m[j * 3 + i] = -s; m[j * 3 + j] = co;
	for (int k = 0; k < 9; k++) MATRIX_MULT3x3 = m[k];
}

static void animRot(const Car& c, uint8_t anim) {
	const Vehicle& v = c.v;
	const float rotor = fx::toFloat(c.rotorAngle);
	switch (anim) {
	case ANIM_STEER: rotAxis(2, v.steer); break;
	case ANIM_WHEEL_F: rotAxis(0, -fx::toFloat(c.wheelRot[WHEEL_FL])); break;
	case ANIM_WHEEL_R: rotAxis(0, -fx::toFloat(c.wheelRot[WHEEL_RL])); break;
	case ANIM_ROTOR: rotAxis(2, rotor); break;
	case ANIM_ROTOR2: rotAxis(0, rotor * 2.0f); break;
	case ANIM_PROP: rotAxis(1, rotor * 1.5f); break;
	case ANIM_RUDDER: rotAxis(2, -v.yawIn * 0.4f); break;
	case ANIM_ELEVATOR: rotAxis(0, v.pitchIn * 0.35f); break;
	case ANIM_AILERON_L: rotAxis(0, v.rollIn * 0.35f); break;
	case ANIM_AILERON_R: rotAxis(0, -v.rollIn * 0.35f); break;
	default: break;
	}
}

static int drawCar(Car& c, const View& w) {
	Vehicle& v = c.v;
	Model& m = *vehmdl::get(v.model);
	m.lastUse = timing::vblanks();
	u8 r, g, b;
	vehdata::colour(v.colPrim, r, g, b);
	const u16 prim = RGB15(r >> 3, g >> 3, b >> 3);
	vehdata::colour(v.colSec, r, g, b);
	const u16 sec = RGB15(r >> 3, g >> 3, b >> 3);
	vehmdl::setColours(m, prim, sec, v.lights);
	s32 body[12];
	bodyMatrix(v.right, v.fwd, v.up, v.pos, w, body);
	MATRIX_RESTORE = VIEW_SLOT;
	mult4x3(body);
	if (v.cls == CLS_BIKE && v.lean != 0.0f) {
		// lean about the ground line: T(0, 0, zg) Ry(lean) T(0, 0, -zg)
		const float zg = leanPivot(c), s = sinf(v.lean), co = cosf(v.lean);
		const s32 L[12] = { (s32)(co * 4096.0f), 0, (s32)(-s * 4096.0f), 0, 4096, 0, (s32)(s * 4096.0f), 0, (s32)(co * 4096.0f),
		                    (s32)(-zg * s * 4096.0f), 0, (s32)((zg - zg * co) * 4096.0f) };
		mult4x3(L);
	}
	glStoreMatrix(BODY_SLOT);
	int tris = 0;
	for (int i = 0; i < m.numParts; i++) {
		const Part& p = m.parts[i];
		if (!partVisible(c, p)) continue;
		MATRIX_RESTORE = BODY_SLOT;
		if (p.hinge >= 0) {
			mult4x3(m.hingeM[p.comp]);
			if (p.comp >= COMP_DOOR_LF && p.comp <= COMP_DOOR_RR) {
				const int d = p.comp - COMP_DOOR_LF;
				rotAxis(2, v.door[d] * DOOR_OPEN_ANGLE * (d == DOOR_LF || d == DOOR_LR ? -1.0f : 1.0f));
			}
		} else if (p.anim1) {
			mult4x3(m.animM[p.m1]);
			animRot(c, p.anim1);
			if (p.anim2) {
				mult4x3(m.animM[p.m2]);
				animRot(c, p.anim2);
			}
		}
		if (!p.identity) mult4x3(p.below);
		callPart(m, p);
		tris += p.tris;
	}
	// car wheels: spin about x, steer about z, left side turned round (hubcap out)
	if (m.wheelPart >= 0 && !m.bikeWheels) {
		const Part& p = m.parts[m.wheelPart];
		for (int i = 0; i < 4; i++) {
			const bool front = i == WHEEL_FL || i == WHEEL_FR, left = i == WHEEL_FL || i == WHEEL_RL;
			const float sc = m.wheelScale[front ? 0 : 1];
			const float wr = fx::toFloat(c.wheelRot[i]);
			const float spin = left ? wr : -wr;
			const float st = (front ? v.steer : 0.0f) + (left ? 3.14159265f : 0.0f);
			const float cx = cosf(spin), sx = sinf(spin), cz = cosf(st), sz = sinf(st);
			// rows: Rx(spin) * Rz(st), then scale; a burst tyre is squashed flat
			const float R[9] = { cz, sz, 0.0f, -cx * sz, cx * cz, sx, sx * sz, -sx * cz, cx };
			const float rad = m.wheelRadius[front ? 0 : 1];
			const float lz = fx::toFloat(c.lineStart[i].z), ll = fx::toFloat(c.lineLen[i]);
			float z = m.wheelPos[i][2] + fx::toFloat(c.k.suspLower);
			if (c.frac[i] < fx::ONE) z = lz - fx::toFloat(c.frac[i]) * ll + rad;
			const bool burst = v.burst & (1u << i);
			s32 M[12];
			for (int k = 0; k < 9; k++) M[k] = (s32)(R[k] * sc * (burst && (k % 3) == 2 ? 0.75f : 1.0f) * 4096.0f);
			M[9] = (s32)(m.wheelPos[i][0] * 4096.0f);
			M[10] = (s32)(m.wheelPos[i][1] * 4096.0f);
			M[11] = (s32)((z - (burst ? rad * 0.25f : 0.0f)) * 4096.0f);
			MATRIX_RESTORE = BODY_SLOT;
			mult4x3(M);
			callPart(m, p);
			tris += p.tris;
		}
	}
	return tris;
}

// camera-facing sprite at SA point p (size m), glow / puff textures
static void sprite(const View& w, const float p[3], float size, u16 col, int alpha, const tex::Texture* t, int id) {
	if (!t || alpha <= 0) return;
	const float sy = sinf(w.yaw), cy = cosf(w.yaw), sp = sinf(w.pitch), cp = cosf(w.pitch);
	const float R[3] = { cy, 0.0f, -sy }, U[3] = { -sp * sy, cp, -sp * cy };
	glPushMatrix();
	glTranslatef(p[0] - w.originX, p[2] - w.originZ, -(p[1] - w.originY));
	glScalef(size, size, size);
	glPolyFmt(POLY_ALPHA(alpha > 31 ? 31 : alpha) | POLY_CULL_NONE | POLY_ID(id & 63) | POLY_FOG);
	glBindTexture(GL_TEXTURE_2D, t->id);
	glColor(col);
	const t16 tw = inttot16(t->w), th = inttot16(t->h);
	static const s8 k[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	glBegin(GL_QUADS);
	for (int i = 0; i < 4; i++) {
		glTexCoord2t16(k[i][0] > 0 ? tw : 0, k[i][1] > 0 ? 0 : th);
		glVertex3v16(floattov16(R[0] * k[i][0] + U[0] * k[i][1]), floattov16(R[1] * k[i][0] + U[1] * k[i][1]),
		             floattov16(R[2] * k[i][0] + U[2] * k[i][1]));
	}
	glEnd();
	glPopMatrix(1);
}

// CVehicle::DoHeadLightBeam / CShadows::StoreCarLightShadow: the pool of light
// on the road ahead, a flat glow quad at the front wheels' contact height
static void headlightPool(const Car& c, const View& w, const tex::Texture* t, int id) {
	const Vehicle& v = c.v;
	const Model* m = vehmdl::get(v.model);
	if (!t || !m || v.up[2] < 0.7f || c.susp[WHEEL_FL] >= fx::ONE) return;
	const float gz = v.pos[2] + fx::toFloat(c.contactRel[WHEEL_FL].z) + 0.08f;
	const float reach = v.cls == CLS_BIKE ? 3.0f : 4.0f, half = v.cls == CLS_BIKE ? 1.1f : 1.7f;
	float ctr[3];
	const float l[3] = { 0.0f, m->bmax[1] + reach, 0.0f };
	toWorld(v, l, ctr);
	const float f[2] = { v.fwd[0], v.fwd[1] }, rgt[2] = { v.right[0], v.right[1] };
	glPushMatrix();
	glTranslatef(ctr[0] - w.originX, gz - w.originZ, -(ctr[1] - w.originY));
	glScalef(4.0f, 4.0f, 4.0f);
	glPolyFmt(POLY_ALPHA(9) | POLY_CULL_NONE | POLY_ID(id & 63) | POLY_FOG);
	glBindTexture(GL_TEXTURE_2D, t->id);
	glColor(RGB15(31, 30, 24));
	const t16 tw = inttot16(t->w), th = inttot16(t->h);
	static const s8 k[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	glBegin(GL_QUADS);
	for (int i = 0; i < 4; i++) {
		const float x = rgt[0] * half * k[i][0] + f[0] * reach * 0.7f * k[i][1];
		const float y = rgt[1] * half * k[i][0] + f[1] * reach * 0.7f * k[i][1];
		glTexCoord2t16(k[i][0] > 0 ? tw : 0, k[i][1] > 0 ? 0 : th);
		glVertex3v16(floattov16(x * 0.25f), 0, floattov16(-y * 0.25f));
	}
	glEnd();
	glPopMatrix(1);
}

void draw(const View& w) {
	const u32 t0 = timing::ticks();
	uint16_t drawn = 0, lights = 0;
	int tris = 0;
	// car light: the sun direction (moon / overhead at night) in the timecyc ped light colour
	const float* sun = timecycle::vectorToSun();
	float ld[3] = { -sun[0], -sun[2], sun[1] };   // SA -> GL, pointing away from the sun
	if (sun[2] < 0.2f) { ld[0] = 0.3f; ld[1] = -0.9f; ld[2] = 0.3f; }
	normalise(ld);
	u8 lr = 255, lg = 255, lb = 255;
	if (dbg::opts.pedLight) timecycle::pedLight(lr, lg, lb);
	const u16 light = RGB15(lr >> 3, lg >> 3, lb >> 3);
	glStoreMatrix(VIEW_SLOT);
	glLight(0, light, floattov10(ld[0] * 0.98f), floattov10(ld[1] * 0.98f), floattov10(ld[2] * 0.98f));
	bool wreckLight = false;
	const bool night = player::g_clockH >= 20 || player::g_clockH < 7;
	const float fx_ = -sinf(w.yaw), fy_ = cosf(w.yaw);
	for (Car& c : g_cars) {
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || !c.placed || !vehmdl::ready(v.model)) continue;
		v.lights = v.engineOn && night && v.status != STATUS_WRECKED;
		const float dx = v.pos[0] - w.eyeX, dy = v.pos[1] - w.eyeY;
		if (dx * dx + dy * dy > (float)(dbg::opts.fogView * dbg::opts.fogView)) continue;
		// behind the camera (and not right next to it)
		const float rad = fx::toFloat(c.radius);
		const float front = dx * fx_ + dy * fy_;
		if (front < -6.0f - rad || fabsf(dx * fy_ - dy * fx_) > fmaxf(front, 0.0f) * 1.15f + rad + 5.0f)
			continue;
		const bool wreck = v.status == STATUS_WRECKED;
		if (wreck != wreckLight) {
			wreckLight = wreck;
			MATRIX_RESTORE = VIEW_SLOT;
			glLight(0, wreck ? RGB15(5, 5, 5) : light, floattov10(ld[0] * 0.98f), floattov10(ld[1] * 0.98f),
			        floattov10(ld[2] * 0.98f));
		}
		tris += drawCar(c, w);
		drawn++;
	}
	for (Debris& d : s_debris) {
		if (!d.used || !vehmdl::ready(d.model)) continue;
		const Model& m = *vehmdl::get(d.model);
		const Part& p = m.parts[d.part];
		s32 body[12];
		bodyMatrix(d.r, d.r + 3, d.r + 6, d.pos, w, body);
		MATRIX_RESTORE = VIEW_SLOT;
		mult4x3(body);
		if (p.hinge >= 0) mult4x3(m.hingeM[p.comp]);
		if (!p.identity) mult4x3(p.below);
		callPart(m, p);
	}
	MATRIX_RESTORE = VIEW_SLOT;
	GFX_TEX_FORMAT = 0;
	GFX_PAL_FORMAT = 0;
	glBindTexture(0, 0);
	skid::draw(w.originX, w.originY, w.originZ);
	MATRIX_RESTORE = VIEW_SLOT;
	water::draw(w.originX, w.originY, w.originZ, w.eyeX, w.eyeY);
	MATRIX_RESTORE = VIEW_SLOT;
	// fire on burning cars, smoke on damaged ones, explosion fireballs, lights
	const tex::Texture* glow = sky::glowTexture();
	const tex::Texture* puff = sky::puffTexture();
	int id = 40;
	const uint32_t now = timing::vblanks();
	for (Car& c : g_cars) {
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || !c.placed) continue;
		const Model* m = vehmdl::get(v.model);
		if (!m) continue;
		const float dx = v.pos[0] - w.eyeX, dy = v.pos[1] - w.eyeY;
		const bool near = dx * dx + dy * dy < 70.0f * 70.0f;
		// CCoronas::RegisterCorona for the head / tail lights (+ brake lights)
		if (dbg::opts.vehLights && near && v.status != STATUS_WRECKED && (v.lights || (v.engineOn && v.brake > 0.1f))) {
			auto lamp = [&](const float* p, bool front, float size, u16 col, int a) {
				const bool pair = fabsf(p[0]) > 0.15f;
				for (int s = 0; s < (pair ? 2 : 1); s++) {
					const float l[3] = { s ? -p[0] : p[0], p[1] + (front ? 0.05f : -0.05f), p[2] };
					float q3[3];
					toWorld(v, l, q3);
					sprite(w, q3, size, col, a, glow, id++);
					lights++;
				}
			};
			if (v.lights && m->hasHead) lamp(m->headlight, true, 0.55f, RGB15(31, 31, 26), 24);
			if (m->hasTail) lamp(m->taillight, false, v.brake > 0.1f ? 0.45f : 0.3f, RGB15(31, 4, 2), v.brake > 0.1f ? 28 : 18);
			if (v.lights && m->hasHead && v.cls != CLS_BOAT && v.cls != CLS_HELI && v.cls != CLS_PLANE) headlightPool(c, w, glow, id++);
		}
		const bool fire = (v.status != STATUS_WRECKED && v.health < 250.0f) || (v.status == STATUS_WRECKED && c.wreckBurn > 0.0f);
		float e[3];
		const float el[3] = { m->engine[0], m->engine[1], m->bmax[2] * 0.6f };
		toWorld(v, el, e);
		if (fire) {
			for (int k = 0; k < 3; k++) {
				float q3[3] = { e[0], e[1], e[2] + 0.25f + 0.3f * k };
				const float flick = 0.45f + 0.15f * (float)((now * 7 + k * 13) % 5) / 4.0f;
				sprite(w, q3, flick * (v.status == STATUS_WRECKED ? 1.4f : 1.0f), RGB15(31, 18 - k * 5, 4), 26 - k * 5, glow, id++);
			}
		}
		if (v.health < 390.0f || fire) {
			const float t = (float)(now % 90) / 90.0f;
			float q3[3] = { e[0], e[1], e[2] + 0.6f + t * 2.0f };
			const int shade = v.health < 250.0f || v.status == STATUS_WRECKED ? 6 : 16;
			sprite(w, q3, 0.5f + t * 0.8f, RGB15(shade, shade, shade), (int)(18.0f * (1.0f - t)) + 2, puff, id++);
		}
	}
	for (Fx& f : s_fx) {
		if (!f.used) continue;
		const float t = f.t;
		if (t < 1.2f) {
			const float s = 1.0f + 3.5f * fminf(t / 0.35f, 1.0f);
			const int a = (int)(31.0f * (1.0f - t / 1.2f));
			sprite(w, f.pos, s, RGB15(31, 26, 10), a, glow, id++);
			const float q3[3] = { f.pos[0], f.pos[1], f.pos[2] + 0.8f };
			sprite(w, q3, s * 0.8f, RGB15(31, 12, 2), a, glow, id++);
		}
		const float q3[3] = { f.pos[0], f.pos[1], f.pos[2] + 1.0f + t * 2.5f };
		sprite(w, q3, 1.5f + t * 1.5f, RGB15(4, 4, 4), (int)(24.0f * (1.0f - t / 3.0f)), puff, id++);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
	s_stats.drawn = drawn;
	s_stats.lights = lights;
	s_stats.polys = (uint16_t)(tris + skid::drawnPolys());
	s_stats.drawUs = timing::usSince(t0);
}

void init() {
	removeAll();
	s_stats = {};
	skid::clear();
	for (Debris& d : s_debris) d.used = false;
	for (Fx& f : s_fx) f.used = false;
}

}
