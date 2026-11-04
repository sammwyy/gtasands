#include "game/vehicle.h"

#include <initializer_list>
#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/aevehicleaudio.h"
#include "game/collision.h"
#include "game/debugopts.h"
#include "game/player.h"
#include "game/timecycle.h"
#include "game/vehicledata.h"
#include "game/weather.h"
#include "hud/hud.h"
#include "platform/timing.h"
#include "render/gxlist.h"
#include "render/sky.h"
#include "assets/texture.h"

namespace veh {

using namespace vehmdl;

static constexpr float TS = 50.0f / 60.0f;            // CTimer::ms_fTimeStep per 60 Hz step
static constexpr float GRAVITY = 0.008f;              // per step^2
static constexpr float ELASTICITY = 0.05f;            // CAutomobile::m_fElasticity
static constexpr float FRICTION = 0.25f;              // body vs map sliding friction
static constexpr float WHEEL_FRICTION = 0.9f;         // cHandlingDataMgr::fWheelFriction
static constexpr float WS_TRAC_FRAC_LIMIT = 0.3f;
static constexpr float WS_ALREADY_SPINNING_LOSS = 0.2f;
static constexpr float DAMPING_LIMIT_IN_FRAME = 0.25f;
static constexpr float DAMPING_LIMIT_OF_SPRING_FORCE = 0.999f;
static constexpr float TRANSMISSION_SMOOTHER_FRAC = 0.85f;
static constexpr float ACTIVE_RANGE = 10.0f;          // m from the player: the collision near list covers it
static constexpr int VIEW_SLOT = 11, BODY_SLOT = 10;   // matrix stack (map 13, overlay 12, CJ 14..30)
static constexpr float DOOR_OPEN_ANGLE = 1.25f;       // CDoor m_fOpenAngle ~ 0.4 PI
enum WheelState : uint8_t { WHEEL_NORMAL, WHEEL_SPINNING, WHEEL_SKIDDING, WHEEL_FIXED };

struct Gear { float maxVel, up, down; };
struct Car {
	Vehicle v;
	ds::VehInfo h;
	Gear gears[7];
	float frac[4];              // raw collision line fraction (wheel drawing)
	float slip[4];
	float padSteer, padAccel, padBrake;
	bool padHandbrake, padHorn;
	bool placed;
	uint32_t hasDam;            // components with a *_dam variant
	uint32_t doorSound;         // door edges already played (bit per door: open)
	bool windscreenSaid;
	float wreckBurn;            // ms the wreck keeps burning
};
static Car s_cars[MAX];

struct Debris { bool used; int8_t model; uint8_t part; float pos[3], r[9], vel[3], w[3], life; };
static Debris s_debris[8];
struct Fx { bool used; float pos[3], t; };
static Fx s_fx[4];
static Stats s_stats;

// ---- vector helpers -----------------------------------------------------------
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
Vehicle* get(int id) { return (unsigned)id < MAX && s_cars[id].v.status != STATUS_FREE ? &s_cars[id].v : nullptr; }
int count() {
	int n = 0;
	for (const Car& c : s_cars) n += c.v.status != STATUS_FREE;
	return n;
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

// ---- CPhysical ----------------------------------------------------------------
static void comWorld(const Vehicle& v, float o[3]) { dirToWorld(v, v.com, o); }

// CPhysical::GetSpeed (point relative to the entity position)
static void speedAt(const Vehicle& v, const float p[3], float o[3]) {
	float c[3], d[3];
	comWorld(v, c);
	for (int k = 0; k < 3; k++) d[k] = p[k] - c[k];
	cross(v.turnSpeed, d, o);
	for (int k = 0; k < 3; k++) o[k] += v.moveSpeed[k];
}
// CPhysical::GetMass(pos, dir): effective mass at a point along a direction
static float massAt(const Vehicle& v, const float p[3], const float dir[3]) {
	float c[3];
	cross(p, dir, c);
	return 1.0f / (dot(c, c) / v.turnMass + 1.0f / v.mass);
}
static void applyMoveForce(Vehicle& v, const float f[3]) { madd(v.moveSpeed, f, 1.0f / v.mass); }
static void applyTurnForce(Vehicle& v, const float f[3], const float p[3]) {
	float c[3], d[3], t[3];
	comWorld(v, c);
	for (int k = 0; k < 3; k++) d[k] = p[k] - c[k];
	cross(d, f, t);
	madd(v.turnSpeed, t, 1.0f / v.turnMass);
}
static void applyForce(Vehicle& v, const float f[3], const float p[3]) {
	applyMoveForce(v, f);
	applyTurnForce(v, f, p);
}

static void reorthogonalise(Vehicle& v) {
	normalise(v.fwd);
	cross(v.fwd, v.up, v.right);
	normalise(v.right);
	cross(v.right, v.fwd, v.up);
	normalise(v.up);
}

// ---- cTransmission ------------------------------------------------------------
static void initGears(Car& c) {
	const ds::VehInfo& h = c.h;
	const int n = h.numGears < 1 ? 1 : h.numGears > 6 ? 6 : h.numGears;
	memset(c.gears, 0, sizeof c.gears);
	const float avgHalf = 0.5f * h.maxVelocity / n, maxGear = h.maxVelocity - avgHalf;
	for (int i = 1; i <= n; i++) {
		Gear& g = c.gears[i];
		g.maxVel = (float)i * maxGear / n + avgHalf;
		const float diff = g.maxVel - c.gears[i - 1].maxVel;
		if (i >= n) g.up = h.maxVelocity;
		else {
			c.gears[i + 1].down = 0.42f * diff + c.gears[i - 1].maxVel;
			g.up = 0.6667f * diff + c.gears[i - 1].maxVel;
		}
	}
	c.gears[0].maxVel = h.maxReverseVelocity;
	c.gears[0].up = -0.01f;
	c.gears[0].down = h.maxReverseVelocity;
	c.gears[1].down = -0.01f;
}

// cTransmission::CalculateDriveAcceleration (no cheats / nitro)
static float driveAccel(Car& c, float gas, float vel) {
	Vehicle& v = c.v;
	const ds::VehInfo& h = c.h;
	const int n = h.numGears < 1 ? 1 : h.numGears > 6 ? 6 : h.numGears;
	if (vel < h.maxReverseVelocity) return 0.0f;
	for (int guard = 0; vel <= h.maxVelocity && guard < 16; guard++) {
		const Gear& g = c.gears[v.gear];
		bool accelerate = false;
		if (vel > g.up) {
			if (v.gear == 0 && gas <= 0.0f) accelerate = true;
			else if (v.gear < n) { v.gear++; continue; }
			else accelerate = true;
		} else {
			if (vel >= g.down || v.gear == 0 || (v.gear == 1 && gas >= 0.0f)) accelerate = true;
			else { v.gear--; continue; }
		}
		if (!accelerate) break;
		float mult;
		if (n == 1) mult = 1.0f;
		else if (v.gear >= 1) {
			float gn = 1.0f - ((float)v.gear - 1.0f) / ((float)n - 1.0f);
			gn *= gn;
			mult = gn * (h.handlingFlags & 1 ? 5.0f : h.handlingFlags & 2 ? 4.0f : 3.0f) + 1.0f;
		} else mult = 4.5f;
		float a = mult * h.engineAccel * 0.4f * gas * TS;
		if (v.driveOnGround) {
			const float maxChange = h.maxVelocity / (float)n * (1.0f / 3.0f);
			float cur, span;
			if (v.gear == 0) { cur = maxChange - vel; span = maxChange - c.gears[0].down; }
			else if (v.gear == 1) { cur = maxChange + vel; span = maxChange + c.gears[1].up; }
			else { cur = vel - g.down; span = g.up - g.down; }
			const float ratio = span != 0.0f ? cur / span : 0.0f;
			const float inertia = ratio - v.inertia1;
			const float acc = clampf(1.0f - inertia * h.engineInertia, 0.1f, 1.0f);
			v.inertia1 = ratio;
			v.inertia2 = acc * (1.0f - TRANSMISSION_SMOOTHER_FRAC) + TRANSMISSION_SMOOTHER_FRAC * v.inertia2;
			a *= v.inertia2;
		}
		return a;
	}
	return 0.0f;
}

// ---- setup ----------------------------------------------------------------------
static void setupFromModel(Car& c) {
	Vehicle& v = c.v;
	const Model* m = vehmdl::get(v.model);
	// CAutomobile::SetupSuspensionLines
	for (int i = 0; i < 4; i++) {
		const float r = m->wheelRadius[i == WHEEL_FL || i == WHEEL_FR ? 0 : 1];
		const float* w = m->wheelPos[i];
		v.lineStart[i][0] = v.lineEnd[i][0] = w[0];
		v.lineStart[i][1] = v.lineEnd[i][1] = w[1];
		v.lineStart[i][2] = w[2] + c.h.suspUpper;
		v.lineEnd[i][2] = w[2] + c.h.suspLower - r;
		v.lineLen[i] = v.lineStart[i][2] - v.lineEnd[i][2];
		v.susp[i] = v.suspPrev[i] = 1.0f;
		c.frac[i] = 1.0f;
	}
	v.springLen = c.h.suspUpper - c.h.suspLower;
	// body spheres: two rows along the length + the cabin
	const float W = m->bmax[0] - m->bmin[0], L = m->bmax[1] - m->bmin[1], H = m->bmax[2] - m->bmin[2];
	const float r = clampf(fminf(W * 0.42f, H * 0.45f), 0.35f, 0.9f);
	const float xo = W * 0.5f - r > 0.1f ? W * 0.5f - r : 0.0f;
	const float zLow = m->bmin[2] + r + 0.2f;
	const float ys[3] = { m->bmin[1] + r, (m->bmin[1] + m->bmax[1]) * 0.5f, m->bmax[1] - r };
	v.numSpheres = 0;
	for (int k = 0; k < 3; k++)
		for (int s = 0; s < (xo > 0.0f ? 2 : 1); s++) {
			float* q = v.sph[v.numSpheres++];
			q[0] = s ? xo : -xo; q[1] = ys[k]; q[2] = zLow; q[3] = r;
		}
	const float rt = clampf(W * 0.35f, 0.3f, 0.7f);
	for (int k = 0; k < 2 && L > 2.5f; k++) {
		float* q = v.sph[v.numSpheres++];
		q[0] = 0.0f; q[1] = (m->bmin[1] + m->bmax[1]) * 0.5f + (k ? -0.15f : 0.15f) * L; q[2] = m->bmax[2] - rt; q[3] = rt;
	}
	c.hasDam = 0;
	for (int i = 0; i < m->numParts; i++)
		if (m->parts[i].variant == VAR_DAM) c.hasDam |= 1u << m->parts[i].comp;
	// CalculateHeightAboveRoad: drop the car onto the ground under it
	const float hab = (1.0f - 1.0f / (c.h.suspForce * 4.0f)) * v.springLen + m->wheelRadius[0] - v.lineStart[0][2];
	col::Hit hit;
	if (col::groundAt(v.pos[0], v.pos[1], v.pos[2] + 3.0f, v.pos[2] - 20.0f, hit)) v.pos[2] = hit.z + hab;
	vehmdl::setColours(*vehmdl::get(v.model), 0, 0, false);
	c.placed = true;
	v.asleep = false;
	v.fakePhysics = 0;
}

int spawn(int vehIndex, float x, float y, float z, float hdg, int colour) {
	int id = -1;
	for (int i = 0; i < MAX && id < 0; i++)
		if (s_cars[i].v.status == STATUS_FREE) id = i;
	if (id < 0 || !vehdata::drivable(vehIndex)) return -1;
	Car& c = s_cars[id];
	c = {};
	if (!vehdata::read(vehIndex, c.h)) return -1;
	Vehicle& v = c.v;
	v.model = (int8_t)vehmdl::request(vehIndex);
	if (v.model < 0) return -1;
	v.status = STATUS_PHYSICS;
	v.vehIndex = (int16_t)vehIndex;
	v.modelId = c.h.modelId;
	v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
	const float s = sinf(hdg), co = cosf(hdg);
	v.fwd[0] = -s; v.fwd[1] = co; v.fwd[2] = 0.0f;
	v.right[0] = co; v.right[1] = s; v.right[2] = 0.0f;
	v.up[0] = 0.0f; v.up[1] = 0.0f; v.up[2] = 1.0f;
	v.mass = c.h.mass;
	v.turnMass = c.h.turnMass;
	v.airRes = c.h.dragMult <= 0.01f ? c.h.dragMult : c.h.dragMult / 1000.0f / 2.0f;
	memcpy(v.com, c.h.com, sizeof v.com);
	v.health = 1000.0f;
	v.gear = 1;
	v.inertia2 = 1.0f;
	v.handbrake = true;
	const int nc = c.h.numColours ? c.h.numColours : 1;
	const int pick = colour >= 0 ? colour % nc : rand() % nc;
	v.colPrim = c.h.colours[pick][0];
	v.colSec = c.h.colours[pick][1];
	initGears(c);
	v.audio = aeveh::create(v.modelId);
	hud::log("spawn %.12s #%d", c.h.model, id);
	return id;
}

void remove(int id) {
	if (!get(id)) return;
	Car& c = s_cars[id];
	if (player::vehicleId() == id) player::leaveVehicleNow();
	aeveh::destroy(c.v.audio);
	vehmdl::release(c.v.model);
	for (Debris& d : s_debris)
		if (d.used && d.model == c.v.model) d.used = false;
	c.v.status = STATUS_FREE;
}

void removeAll() {
	for (int i = 0; i < MAX; i++) remove(i);
}

void setControls(int id, float steerLR, float accel, float brake, bool handbrake, bool horn) {
	if (!get(id)) return;
	Car& c = s_cars[id];
	c.padSteer = steerLR;
	c.padAccel = accel;
	c.padBrake = brake;
	c.padHandbrake = handbrake;
	c.padHorn = horn;
}

void setDriver(int id, bool isPlayer) {
	Vehicle* v = get(id);
	if (!v || v->status == STATUS_WRECKED) return;
	v->status = isPlayer ? STATUS_PLAYER : STATUS_PHYSICS;
	if (isPlayer && !v->engineOn) {
		v->engineOn = true;   // CAEVehicleAudioEntity::JustGotInVehicleAsDriver -> AE_START_ENGINE
		aeveh::event(v->audio, aeveh::EV_START_ENGINE, v->pos[0], v->pos[1], v->pos[2]);
	}
	if (!isPlayer) {
		Car& c = s_cars[id];
		c.padSteer = c.padAccel = c.padBrake = 0.0f;
		c.padHorn = false;
		c.padHandbrake = true;
		v->engineOn = false;
	}
	v->asleep = false;
	v->fakePhysics = 0;
}

void attachMatrix(const Vehicle& v, const float l[3], float ox, float oy, float oz, s32 out[12]) {
	float w[3];
	toWorld(v, l, w);
	const float* rows[3] = { v.right, v.fwd, v.up };
	for (int r = 0; r < 3; r++) {
		out[r * 3] = (s32)(rows[r][0] * 4096.0f);
		out[r * 3 + 1] = (s32)(rows[r][2] * 4096.0f);
		out[r * 3 + 2] = (s32)(-rows[r][1] * 4096.0f);
	}
	out[9] = (s32)((w[0] - ox) * 4096.0f);
	out[10] = (s32)((w[2] - oz) * 4096.0f);
	out[11] = (s32)(-(w[1] - oy) * 4096.0f);
}

int findEnterable(float x, float y, float z, float range) {
	int best = -1;
	float bd = range * range;
	for (int i = 0; i < MAX; i++) {
		Car& c = s_cars[i];
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || v.status == STATUS_WRECKED || !c.placed || v.up[2] < 0.5f) continue;
		const Model* m = vehmdl::get(v.model);
		if (!m) continue;
		const float l[3] = { m->bmin[0] - 0.4f, m->seat[1], 0.0f };
		float w[3];
		toWorld(v, l, w);
		const float dx = w[0] - x, dy = w[1] - y, dz = w[2] - z;
		const float d = dx * dx + dy * dy;
		if (d < bd && fabsf(dz) < 3.0f) { bd = d; best = i; }
	}
	return best;
}

void setDoor(int id, int door, float ratio) {
	Vehicle* v = get(id);
	if (!v || (unsigned)door >= NUM_DOORS) return;
	Car& c = s_cars[id];
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

static void vehicleDamage(Car& c) {
	Vehicle& v = c.v;
	const float di = v.damageIntensity;
	v.damageIntensity = 0.0f;
	if (v.status == STATUS_WRECKED) return;
	const float minDmg = v.mass / 1500.0f * 25.0f;
	if (di <= minDmg) return;
	aeveh::event(v.audio, aeveh::EV_CRASH, v.impactPos[0], v.impactPos[1], v.impactPos[2], di);
	v.health -= (di - minDmg) * c.h.collisionDamageMult * 0.6f;
	if (v.health < 0.0f) v.health = 0.0f;
	const Model* m = vehmdl::get(v.model);
	if (!m) return;
	float l[3];
	toLocal(v, v.impactPos, l);
	const float amount = di * 4.0f * c.h.collisionDamageMult;
	const bool rearDoors = m->hinge[COMP_DOOR_LR][1] != 0.0f || m->hinge[COMP_DOOR_LR][0] != 0.0f;
	if (l[1] > m->bmax[1] * 0.6f) {
		damageComp(c, COMP_BUMP_FRONT, amount);
		if (v.dmg[COMP_BUMP_FRONT] != DMG_OK && di > v.mass * 0.2f) damageComp(c, COMP_BONNET, amount);
		if (v.dmg[COMP_BUMP_FRONT] != DMG_OK && di > v.mass * 0.25f) damageComp(c, COMP_WINDSCREEN, amount);
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
	Car& c = s_cars[id];
	Vehicle& v = *vp;
	const bool driven = player::vehicleId() == id;
	v.status = STATUS_WRECKED;
	v.health = 0.0f;
	v.engineOn = false;
	v.asleep = false;
	v.fakePhysics = 0;
	v.wreckedMs = 0;
	c.wreckBurn = 20000.0f;
	v.moveSpeed[2] += 0.13f;
	v.turnSpeed[0] += frand(-0.04f, 0.04f);
	v.turnSpeed[1] += frand(-0.04f, 0.04f);
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
	// wake every car in the blast
	for (Car& o : s_cars) {
		if (o.v.status == STATUS_FREE || &o == &c) continue;
		const float ex = o.v.pos[0] - v.pos[0], ey = o.v.pos[1] - v.pos[1];
		const float e = sqrtf(ex * ex + ey * ey);
		if (e < 8.0f) {
			o.v.asleep = false;
			o.v.fakePhysics = 0;
			o.v.moveSpeed[0] += ex / (e + 0.5f) * 0.05f;
			o.v.moveSpeed[1] += ey / (e + 0.5f) * 0.05f;
			o.v.moveSpeed[2] += 0.06f;
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
	memset(v->dmg, 0, sizeof v->dmg);
	memset(v->compDamage, 0, sizeof v->compDamage);
	if (v->status == STATUS_WRECKED) v->status = player::vehicleId() == id ? STATUS_PLAYER : STATUS_PHYSICS;
	if (v->status == STATUS_PLAYER) v->engineOn = true;
	s_cars[id].windscreenSaid = false;
	s_cars[id].wreckBurn = 0.0f;
}

void flip(int id) {
	Vehicle* v = get(id);
	if (!v) return;
	v->up[0] = 0.0f; v->up[1] = 0.0f; v->up[2] = 1.0f;
	v->fwd[2] = 0.0f;
	if (len(v->fwd) < 0.1f) { v->fwd[0] = 0.0f; v->fwd[1] = 1.0f; }
	reorthogonalise(*v);
	v->pos[2] += 1.5f;
	memset(v->moveSpeed, 0, 12);
	memset(v->turnSpeed, 0, 12);
	v->asleep = false;
	v->fakePhysics = 0;
}

void setHealth(int id, float h) {
	if (Vehicle* v = get(id)) v->health = h;
}

// ---- simulation step ------------------------------------------------------------
// CAutomobile::ProcessControlInputs
static void controlInputs(Car& c) {
	Vehicle& v = c.v;
	v.handbrake = c.padHandbrake;
	v.rawSteer += (-c.padSteer - v.rawSteer) / 5.0f * TS;
	v.rawSteer = clampf(v.rawSteer, -1.0f, 1.0f);
	const float fwd = dot(v.moveSpeed, v.fwd);
	const float in = c.padAccel - c.padBrake;
	if (fabsf(fwd) < 0.01f) {
		if (c.padAccel > 0.6f && c.padBrake > 0.6f) { v.gas = c.padAccel; v.brake = c.padBrake; }
		else { v.gas = in; v.brake = 0.0f; }
	} else if (fwd >= 0.0f) {
		if (in < 0.0f) { v.gas = 0.0f; v.brake = -in; }
		else { v.gas = in; v.brake = 0.0f; }
	} else if (in >= 0.0f && (v.gas <= 0.5f || fwd <= -0.15f)) {
		v.gas = 0.0f;
		v.brake = in;
	} else {
		v.gas = in;
		v.brake = 0.0f;
	}
	const float s = v.rawSteer * v.rawSteer * (v.rawSteer < 0.0f ? -1.0f : 1.0f);
	v.steer = c.h.steeringLock * s * (3.14159265f / 180.0f);
	v.horn = c.padHorn;
}

// CVehicle::ProcessWheel
static void processWheel(Car& c, int i, const float wfwd[3], const float wright[3], const float cs[3], const float cp[3],
                         float thrust, float brake, float adhesion) {
	Vehicle& v = c.v;
	bool alreadySkidding = false;
	float right = 0.0f, fwd = 0.0f;
	const float sFwd = dot(wfwd, cs), sRight = dot(wright, cs);
	const bool braking = brake != 0.0f;
	const bool driving = !braking && thrust != 0.0f;
	const int on = v.numContact ? v.numContact : 1;
	adhesion *= TS;
	uint8_t& state = v.wheelState[i];
	if (state != WHEEL_NORMAL) {
		alreadySkidding = true;
		adhesion *= c.h.tractionLoss;
		if (state == WHEEL_SPINNING && v.status == STATUS_PLAYER) adhesion *= 1.0f - fabsf(v.gas) * WS_ALREADY_SPINNING_LOSS;
	}
	state = WHEEL_NORMAL;
	if (sRight != 0.0f) right = -(sRight / on);
	if (driving) {
		fwd = thrust;
		right = clampf(right, -adhesion, adhesion);
	} else if (sFwd != 0.0f) {
		fwd = -(sFwd / on);
		if (!braking && fabsf(v.gas) < 0.01f) brake = WHEEL_FRICTION / v.mass * (v.mass < 500.0f ? 0.1f : 1.0f);
		if (brake > adhesion) {
			if (fabsf(sFwd) > 0.005f) state = WHEEL_FIXED;
		} else {
			fwd = clampf(fwd, -brake, brake);
		}
	}
	const float slipSq = right * right + fwd * fwd;
	c.slip[i] = 0.0f;
	if (slipSq > adhesion * adhesion) {
		if (state != WHEEL_FIXED) {
			const float lim = sFwd > 0.15f && (i == WHEEL_FL || i == WHEEL_FR) ? 2.0f * WS_TRAC_FRAC_LIMIT : WS_TRAC_FRAC_LIMIT;
			state = driving && lim * adhesion < fabsf(fwd) ? WHEEL_SPINNING : WHEEL_SKIDDING;
		}
		float loss = 1.0f;
		if (!alreadySkidding && state == WHEEL_SPINNING && v.status == STATUS_PLAYER)
			loss = c.h.tractionLoss * (1.0f - fabsf(v.gas) * WS_ALREADY_SPINNING_LOSS);
		const float sl = sqrtf(slipSq);
		c.slip[i] = adhesion > 0.0f ? sl / adhesion - 1.0f : 0.0f;
		loss *= adhesion / sl;
		fwd *= loss;
		right *= loss;
	} else if (state == WHEEL_FIXED) {
		c.slip[i] = 1.0f;
	}
	if (fwd != 0.0f || right != 0.0f) {
		float mv[3] = { fwd * wfwd[0] + right * wright[0], fwd * wfwd[1] + right * wright[1], fwd * wfwd[2] + right * wright[2] };
		float turn[3] = { mv[0], mv[1], mv[2] };
		const bool sep = c.h.suspAntiDive > 0.0f && (braking || driving);
		if (sep) madd(turn, wfwd, -(braking ? 1.0f : 0.5f) * c.h.suspAntiDive * fwd);
		const float speed = len(mv);
		normalise(mv);
		const float tspeed = sep ? len(turn) : speed;
		if (sep) normalise(turn);
		else memcpy(turn, mv, sizeof turn);
		const float f[3] = { mv[0] * v.mass * speed, mv[1] * v.mass * speed, mv[2] * v.mass * speed };
		applyMoveForce(v, f);
		const float tm = massAt(v, cp, turn) * tspeed;
		const float tf[3] = { turn[0] * tm, turn[1] * tm, turn[2] * tm };
		applyTurnForce(v, tf, cp);
	}
}

// CAutomobile::ProcessCarWheelPair
static void wheelPair(Car& c, int l, int r, float steerAngle, float (*cs)[3], float (*cp)[3], float traction,
                      float accel, float brake, bool front) {
	Vehicle& v = c.v;
	const char dt = (char)c.h.driveType;
	const bool drive = dt == '4' || (front ? dt == 'F' : dt == 'R');
	float bias = 2.0f * c.h.suspBias;
	if (!front) {
		bias = 2.0f - bias;
		if (v.handbrake && !(c.h.handlingFlags & 0x40)) brake = 20000.0f;
	}
	if (v.wheelCounts[l] <= 0.0f && v.wheelCounts[r] <= 0.0f) return;
	const bool canSteer = steerAngle > -100.0f;
	const float ss = canSteer ? sinf(steerAngle) : 0.0f, sc = canSteer ? cosf(steerAngle) : 1.0f;
	const float brakeBias = front ? 2.0f * c.h.brakeBias : 2.0f - 2.0f * c.h.brakeBias;
	const float tracBias = front ? 2.0f * c.h.tractionBias : 2.0f - 2.0f * c.h.tractionBias;
	for (int w : { l, r }) {
		if (v.wheelCounts[w] <= 0.0f) continue;
		const float* n = v.contactN[w];
		float wf[3] = { v.fwd[0], v.fwd[1], v.fwd[2] };
		madd(wf, n, -dot(wf, n));
		normalise(wf);
		float wr[3];
		cross(wf, n, wr);
		normalise(wr);
		if (canSteer) {
			float t[3];
			for (int k = 0; k < 3; k++) t[k] = sc * wf[k] - ss * wr[k];
			for (int k = 0; k < 3; k++) wr[k] = ss * wf[k] + sc * wr[k];
			memcpy(wf, t, sizeof wf);
		}
		float adhesion = vehdata::tyreAdhesion(v.contactSurf[w]) * traction;
		adhesion *= vehdata::wetMultiplier(v.contactSurf[w], weather::g.wetRoads);
		adhesion *= fminf(bias * c.h.suspForce * 4.0f * (1.0f - v.susp[w]), 2.0f);
		processWheel(c, w, wf, wr, cs[w], cp[w], drive ? accel : 0.0f, brake * brakeBias, adhesion * tracBias);
		if (drive && v.gas < 0.0f && v.wheelState[w] == WHEEL_SPINNING) v.wheelState[w] = WHEEL_NORMAL;
	}
}

static void processSuspension(Car& c) {
	Vehicle& v = c.v;
	float cp[4][3] = {}, dir[4][3], limit[4] = {};
	for (int i = 0; i < 4; i++) {
		for (int k = 0; k < 3; k++) dir[i][k] = -v.up[k];
		if (v.susp[i] >= 1.0f) continue;
		for (int k = 0; k < 3; k++) cp[i][k] = v.contact[i][k] - v.pos[k];
		// ApplySpringCollisionAlt
		const float bias = i == WHEEL_FL || i == WHEEL_FR ? c.h.suspBias : 1.0f - c.h.suspBias;
		const float stress = 1.0f - v.susp[i];
		float n[3] = { v.contactN[i][0], v.contactN[i][1], v.contactN[i][2] };
		if (dot(dir[i], n) > 0.0f) for (float& x : n) x = -x;
		limit[i] = stress * TS * v.mass * c.h.suspForce * bias * 0.016f;
		const float f[3] = { n[0] * limit[i], n[1] * limit[i], n[2] * limit[i] };
		applyForce(v, f, cp[i]);
	}
	for (int i = 0; i < 4; i++) {
		if (v.susp[i] >= 1.0f) continue;
		float cs[3];
		speedAt(v, cp[i], cs);
		if (v.contactN[i][2] > 0.35f) for (int k = 0; k < 3; k++) dir[i][k] = -v.contactN[i][k];
		// ApplySpringDampening
		const float a = dot(cs, dir[i]);
		float ps[3];
		speedAt(v, cp[i], ps);
		const float b = dot(ps, dir[i]);
		const float dts = clampf(TS * c.h.suspDamping, -DAMPING_LIMIT_IN_FRAME, DAMPING_LIMIT_IN_FRAME);
		float ds = -(dts * a);
		if (ds > 0.0f && ds + b > 0.0f) ds = b >= 0.0f ? 0.0f : -b;
		else if (ds < 0.0f && ds + b < 0.0f) ds = b <= 0.0f ? 0.0f : -b;
		float cw[3], d[3];
		comWorld(v, cw);
		for (int k = 0; k < 3; k++) d[k] = cp[i][k] - cw[k];
		float fd = massAt(v, d, dir[i]) * ds;
		const float lim = fabsf(limit[i]) * DAMPING_LIMIT_OF_SPRING_FORCE;
		if (fd > lim) fd = lim;
		const float f[3] = { dir[i][0] * fd, dir[i][1] * fd, dir[i][2] * fd };
		applyForce(v, f, cp[i]);
	}
}

// suspension lines vs the map (CAutomobile::ProcessEntityCollision's wheel part)
static void castWheels(Car& c) {
	Vehicle& v = c.v;
	for (int i = 0; i < 4; i++) {
		float a[3], b[3];
		toWorld(v, v.lineStart[i], a);
		toWorld(v, v.lineEnd[i], b);
		col::Hit h;
		v.suspPrev[i] = v.susp[i];
		if (col::castSegment(a, b, h) && h.nz > 0.1f) {
			c.frac[i] = h.t;
			v.contact[i][0] = h.x; v.contact[i][1] = h.y; v.contact[i][2] = h.z;
			v.contactN[i][0] = h.nx; v.contactN[i][1] = h.ny; v.contactN[i][2] = h.nz;
			v.contactSurf[i] = h.surface;
			// CAutomobile::ProcessControl: line fraction -> spring compression
			const float wr = 1.0f - v.springLen / v.lineLen[i];
			v.susp[i] = (h.t - wr) / (1.0f - wr);
		} else {
			c.frac[i] = 1.0f;
			v.susp[i] = 1.0f;
		}
	}
}

// body spheres vs the map: ApplyCollisionAlt (static entity) + push-out
static void collideWorld(Car& c) {
	Vehicle& v = c.v;
	for (int s = 0; s < v.numSpheres; s++) {
		float w[3], disp[3];
		toWorld(v, v.sph[s], w);
		uint8_t surf;
		if (!col::sphereContact(w, v.sph[s][3], disp, surf)) continue;
		const float depth = len(disp);
		if (depth < 1e-4f) continue;
		const float n[3] = { disp[0] / depth, disp[1] / depth, disp[2] / depth };
		madd(v.pos, disp, 0.9f);
		float p[3];
		for (int k = 0; k < 3; k++) p[k] = w[k] - n[k] * v.sph[s][3] - v.pos[k];
		float vp[3];
		speedAt(v, p, vp);
		const float vn = dot(vp, n);
		if (vn >= 0.0f) continue;
		float cw[3], d[3];
		comWorld(v, cw);
		for (int k = 0; k < 3; k++) d[k] = p[k] - cw[k];
		const float j = -(1.0f + ELASTICITY) * vn * massAt(v, d, n);
		const float f[3] = { n[0] * j, n[1] * j, n[2] * j };
		applyForce(v, f, p);
		// friction along the surface
		float vt[3] = { vp[0] - n[0] * vn, vp[1] - n[1] * vn, vp[2] - n[2] * vn };
		const float tl = len(vt);
		if (tl > 1e-4f) {
			for (float& x : vt) x /= tl;
			const float fj = fminf(FRICTION * j, tl * massAt(v, d, vt));
			const float ff[3] = { -vt[0] * fj, -vt[1] * fj, -vt[2] * fj };
			applyForce(v, ff, p);
		}
		if (j > v.damageIntensity) {
			v.damageIntensity = j;
			for (int k = 0; k < 3; k++) { v.impactPos[k] = w[k] - n[k] * v.sph[s][3]; v.impactDir[k] = n[k]; }
		}
	}
}

// car vs car: sphere pairs, impulse on both (CPhysical::ApplyCollision)
static void collideCars(int ia, int ib) {
	Vehicle& a = s_cars[ia].v;
	Vehicle& b = s_cars[ib].v;
	const float dx = a.pos[0] - b.pos[0], dy = a.pos[1] - b.pos[1], dz = a.pos[2] - b.pos[2];
	if (dx * dx + dy * dy + dz * dz > 64.0f) return;
	for (int i = 0; i < a.numSpheres; i++) {
		float wa[3];
		toWorld(a, a.sph[i], wa);
		for (int k = 0; k < b.numSpheres; k++) {
			float wb[3];
			toWorld(b, b.sph[k], wb);
			float n[3] = { wa[0] - wb[0], wa[1] - wb[1], wa[2] - wb[2] };
			const float d = len(n), rr = a.sph[i][3] + b.sph[k][3];
			if (d >= rr || d < 1e-4f) continue;
			for (float& x : n) x /= d;
			const float depth = rr - d;
			const float ta = b.mass / (a.mass + b.mass);
			madd(a.pos, n, depth * ta);
			madd(b.pos, n, -depth * (1.0f - ta));
			float pa[3], pb[3], va[3], vb[3];
			for (int q = 0; q < 3; q++) {
				pa[q] = wa[q] - n[q] * a.sph[i][3] - a.pos[q];
				pb[q] = wa[q] - n[q] * a.sph[i][3] - b.pos[q];
			}
			speedAt(a, pa, va);
			speedAt(b, pb, vb);
			const float vn = (va[0] - vb[0]) * n[0] + (va[1] - vb[1]) * n[1] + (va[2] - vb[2]) * n[2];
			if (vn >= 0.0f) continue;
			const float ma = massAt(a, pa, n), mb = massAt(b, pb, n);
			const float j = -(1.0f + 0.1f) * vn / (1.0f / ma + 1.0f / mb);
			const float f[3] = { n[0] * j, n[1] * j, n[2] * j }, g[3] = { -f[0], -f[1], -f[2] };
			applyForce(a, f, pa);
			applyForce(b, g, pb);
			for (Vehicle* v : { &a, &b })
				if (j > v->damageIntensity) {
					v->damageIntensity = j;
					for (int q = 0; q < 3; q++) v->impactPos[q] = wa[q] - n[q] * a.sph[i][3];
				}
			a.asleep = b.asleep = false;
			a.fakePhysics = b.fakePhysics = 0;
		}
	}
}

static bool inRange(const Vehicle& v) {
	const float dx = v.pos[0] - player::g.x, dy = v.pos[1] - player::g.y;
	return dx * dx + dy * dy < ACTIVE_RANGE * ACTIVE_RANGE;
}

static void stepCar(Car& c) {
	Vehicle& v = c.v;
	const Model* m = vehmdl::get(v.model);
	if (v.status == STATUS_PLAYER) controlInputs(c);
	else {
		v.gas = 0.0f;
		v.brake = v.status == STATUS_WRECKED ? 1.0f : 0.3f;
		v.handbrake = true;
		v.horn = false;
		v.rawSteer *= 0.95f;
		v.steer = c.h.steeringLock * v.rawSteer * fabsf(v.rawSteer) * (3.14159265f / 180.0f);
	}
	if (!v.engineOn) v.gas = 0.0f;
	// CPhysical::ApplyGravity
	v.moveSpeed[2] -= GRAVITY * TS;
	castWheels(c);
	processSuspension(c);
	// contact points / speeds for the tyres
	float cp[4][3], cs[4][3];
	for (int i = 0; i < 4; i++) {
		if (v.susp[i] >= 1.0f) {
			float e[3];
			dirToWorld(v, v.lineEnd[i], e);
			memcpy(cp[i], e, sizeof e);
		} else {
			for (int k = 0; k < 3; k++) cp[i][k] = v.contact[i][k] - v.pos[k];
		}
		speedAt(v, cp[i], cs[i]);
	}
	v.speedFwd = dot(v.moveSpeed, v.fwd);
	const float brake = c.h.brakeDecel * v.brake * TS;
	v.numContact = 0;
	v.driveOnGround = 0;
	const char dt = (char)c.h.driveType;
	for (int i = 0; i < 4; i++) {
		if (v.susp[i] >= 1.0f) {
			v.wheelCounts[i] = fmaxf(v.wheelCounts[i] - TS, 0.0f);
			if (v.wheelCounts[i] <= 0.0f) continue;
		} else {
			v.wheelCounts[i] = 4.0f;
		}
		v.numContact++;
		const bool front = i == WHEEL_FL || i == WHEEL_FR;
		if (dt == '4' || (dt == 'F' && front) || (dt == 'R' && !front)) v.driveOnGround++;
	}
	const float accel = v.engineOn ? driveAccel(c, v.gas, v.speedFwd) : 0.0f;
	const float traction = c.h.tractionMult / 250.0f * 0.25f;
	float steerAngle = 1.0f;
	if (v.speedFwd > 0.01f && v.status == STATUS_PLAYER && (v.wheelCounts[WHEEL_FL] > 0.0f || v.wheelCounts[WHEEL_RL] > 0.0f)) {
		const float sr = dot(v.moveSpeed, v.right);
		float sa = fminf(4.5f * traction * 16.0f / (v.speedFwd * v.speedFwd), 1.0f);
		sa = asinf(sa) / (c.h.steeringLock * (3.14159265f / 180.0f));
		if ((v.steer < 0.0f && sr > 0.05f) || (v.steer > 0.0f && sr < -0.05f) || v.handbrake || sa > 1.0f) sa = 1.0f;
		steerAngle = sa;
	}
	const float steer = v.steer * steerAngle;
	wheelPair(c, WHEEL_FL, WHEEL_FR, steer, cs, cp, traction, accel, brake, true);
	wheelPair(c, WHEEL_RL, WHEEL_RR, -999.0f, cs, cp, traction, accel, brake, false);
	// wheel spin (drawing + audio)
	v.skid = 0.0f;
	for (int i = 0; i < 4; i++) {
		const float r = m ? m->wheelRadius[i == WHEEL_FL || i == WHEEL_FR ? 0 : 1] : 0.35f;
		const bool front = i == WHEEL_FL || i == WHEEL_FR;
		const bool driveW = dt == '4' || (dt == 'F' && front) || (dt == 'R' && !front);
		if (v.wheelState[i] == WHEEL_FIXED) v.wheelSpin[i] = 0.0f;
		else if (v.wheelState[i] == WHEEL_SPINNING || (v.wheelCounts[i] <= 0.0f && driveW && v.gas != 0.0f))
			v.wheelSpin[i] += (v.gas * 0.6f - v.wheelSpin[i]) * 0.1f;
		else if (v.wheelCounts[i] > 0.0f) v.wheelSpin[i] = dot(cs[i], v.fwd) / r;
		else v.wheelSpin[i] *= 0.95f;
		v.wheelRot[i] += v.wheelSpin[i] * TS;
		if (v.wheelRot[i] > 6.2831853f) v.wheelRot[i] -= 6.2831853f;
		if (v.wheelRot[i] < -6.2831853f) v.wheelRot[i] += 6.2831853f;
		if (v.wheelCounts[i] > 0.0f && v.wheelState[i] != WHEEL_NORMAL) v.skid += fminf(c.slip[i] + 0.3f, 1.5f);
	}
	// CPhysical::ApplyAirResistance
	const float sp = len(v.moveSpeed) * v.airRes;
	const float k = 1.0f - sp * TS;
	for (float& x : v.moveSpeed) x *= k;
	for (float& x : v.turnSpeed) x *= 0.99f;
	// ApplyMoveSpeed + ApplyTurnSpeed
	madd(v.pos, v.moveSpeed, TS);
	{
		const float w[3] = { v.turnSpeed[0] * TS, v.turnSpeed[1] * TS, v.turnSpeed[2] * TS };
		float t[3];
		for (float* axis : { v.right, v.fwd, v.up }) {
			cross(w, axis, t);
			for (int q = 0; q < 3; q++) axis[q] += t[q];
		}
		float cw[3], nc[3];
		comWorld(v, cw);
		for (float& x : cw) x = -x;
		cross(w, cw, nc);
		for (int q = 0; q < 3; q++) v.pos[q] += nc[q];
		reorthogonalise(v);
	}
	collideWorld(c);
	// fake physics: a resting car without a driver stops being simulated
	const float vs = dot(v.moveSpeed, v.moveSpeed), ws = dot(v.turnSpeed, v.turnSpeed);
	if (v.status != STATUS_PLAYER && vs < 0.005f * 0.005f && ws < 0.0015f * 0.0015f && v.numContact >= 3 &&
	    v.damageIntensity <= 0.0f) {
		if (++v.fakePhysics > 10) {
			v.asleep = true;
			memset(v.moveSpeed, 0, 12);
			memset(v.turnSpeed, 0, 12);
		}
	} else {
		v.fakePhysics = 0;
	}
	// upside down: CVehicle damage drain, then the fire
	if (v.up[2] < 0.3f && vs < 0.02f * 0.02f && v.status != STATUS_WRECKED) v.health -= 4.0f * TS;
	vehicleDamage(c);
}

void step() {
	const u32 t0 = timing::ticks();
	uint16_t awake = 0;
	for (int i = 0; i < MAX; i++) {
		Car& c = s_cars[i];
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || !c.placed) continue;
		if (v.status != STATUS_PLAYER && (!inRange(v) || !col::available())) continue;
		if (v.asleep && v.status != STATUS_PLAYER) continue;
		stepCar(c);
		awake++;
	}
	for (int a = 0; a < MAX; a++)
		for (int b = a + 1; b < MAX; b++) {
			Car& A = s_cars[a];
			Car& B = s_cars[b];
			if (A.v.status == STATUS_FREE || B.v.status == STATUS_FREE || !A.placed || !B.placed) continue;
			if ((A.v.asleep || !inRange(A.v)) && (B.v.asleep || !inRange(B.v))) continue;
			collideCars(a, b);
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
			for (int q = 0; q < 3; q++) d.r[r * 3 + q] += t[q];
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
}

void update() {
	vehmdl::update();
	static uint32_t s_lastVbl;
	const uint32_t vb = timing::vblanks();
	const float frameMs = (float)((vb - s_lastVbl) > 6 ? 6 : vb - s_lastVbl) * (1000.0f / 60.0f) * (float)dbg::opts.simSpeedQ / 4.0f;
	s_lastVbl = vb;
	uint16_t active = 0;
	for (int i = 0; i < MAX; i++) {
		Car& c = s_cars[i];
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE) continue;
		active++;
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
		const Gear& g = c.gears[v.gear];
		aeveh::Params p;
		p.x = v.pos[0]; p.y = v.pos[1]; p.z = v.pos[2];
		p.speed = v.speedFwd;
		p.maxVelocity = c.h.maxVelocity;
		p.revs = v.gear ? clampf((fabsf(v.speedFwd) - g.down) / (g.maxVel - g.down + 1e-4f), 0.0f, 1.0f) : 0.0f;
		p.gas = v.gas;
		p.gear = v.gear;
		p.numGears = c.h.numGears;
		p.engineOn = v.engineOn;
		p.player = v.status == STATUS_PLAYER;
		p.wrecked = v.status == STATUS_WRECKED;
		p.horn = v.horn;
		p.onFire = (v.status != STATUS_WRECKED && v.health < 250.0f) || (v.status == STATUS_WRECKED && c.wreckBurn > 0.0f);
		p.skid = v.skid;
		p.surface = v.contactSurf[WHEEL_RL];
		aeveh::update(v.audio, p);
	}
	for (Fx& f : s_fx)
		if (f.used && (f.t += frameMs / 1000.0f) > 3.0f) f.used = false;
	s_stats.active = active;
}

// Ped vs the car bodies (CJ on foot): box in car space, horizontal push.
bool pushPed(float& x, float& y, float z, float r) {
	bool touched = false;
	for (int i = 0; i < MAX; i++) {
		Car& c = s_cars[i];
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || !c.placed || player::vehicleId() == i) continue;
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
		// hit by a moving car: knocked back and hurt (CPed::KillPedWithCar, reduced)
		const float spd = len(v.moveSpeed);
		if (spd > 0.08f) {
			static uint32_t s_lastHit;
			if (timing::vblanks() - s_lastHit > 30) {
				s_lastHit = timing::vblanks();
				player::hitByVehicle(v.moveSpeed[0] * 60.0f, v.moveSpeed[1] * 60.0f, spd * 120.0f);
				aeveh::event(v.audio, aeveh::EV_PED_HIT, x, y, z);
			}
		}
	}
	return touched;
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
	if (p.comp <= COMP_WHEEL) return p.comp != COMP_WHEEL;
	const uint8_t st = c.v.dmg[p.comp];
	if (st == DMG_MISSING) return false;
	if (p.variant == VAR_ANY) return true;
	const bool dam = st == DMG_DAMAGED && (c.hasDam & (1u << p.comp));
	return p.variant == (dam ? VAR_DAM : VAR_OK);
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
	vehmdl::setColours(m, prim, sec, v.engineOn && (player::g_clockH >= 20 || player::g_clockH < 7));
	s32 body[12];
	bodyMatrix(v.right, v.fwd, v.up, v.pos, w, body);
	MATRIX_RESTORE = VIEW_SLOT;
	mult4x3(body);
	glStoreMatrix(BODY_SLOT);
	int tris = 0;
	for (int i = 0; i < m.numParts; i++) {
		const Part& p = m.parts[i];
		if (!partVisible(c, p)) continue;
		MATRIX_RESTORE = BODY_SLOT;
		if (p.hinge >= 0) {
			mult4x3(m.hingeM[p.comp]);
			float a = 0.0f;
			if (p.comp >= COMP_DOOR_LF && p.comp <= COMP_DOOR_RR) {
				const int d = p.comp - COMP_DOOR_LF;
				a = v.door[d] * DOOR_OPEN_ANGLE * (d == DOOR_LF || d == DOOR_LR ? -1.0f : 1.0f);
			}
			if (a != 0.0f) {
				const s16 ang = (s16)((int)(a * 5215.19f) & 0x7FFF);
				const s32 s = sinLerp(ang), co = cosLerp(ang);
				MATRIX_MULT3x3 = co; MATRIX_MULT3x3 = s; MATRIX_MULT3x3 = 0;
				MATRIX_MULT3x3 = -s; MATRIX_MULT3x3 = co; MATRIX_MULT3x3 = 0;
				MATRIX_MULT3x3 = 0; MATRIX_MULT3x3 = 0; MATRIX_MULT3x3 = 4096;
			}
		}
		if (!p.identity) mult4x3(p.below);
		callPart(m, p);
		tris += p.tris;
	}
	// wheels: spin about x, steer about z, left side turned round (hubcap out)
	if (m.wheelPart >= 0) {
		const Part& p = m.parts[m.wheelPart];
		for (int i = 0; i < 4; i++) {
			const bool front = i == WHEEL_FL || i == WHEEL_FR, left = i == WHEEL_FL || i == WHEEL_RL;
			const float sc = m.wheelScale[front ? 0 : 1];
			const float spin = left ? v.wheelRot[i] : -v.wheelRot[i];
			const float st = (front ? v.steer : 0.0f) + (left ? 3.14159265f : 0.0f);
			const float cx = cosf(spin), sx = sinf(spin), cz = cosf(st), sz = sinf(st);
			// rows: Rx(spin) * Rz(st), then scale
			const float R[9] = { cz, sz, 0.0f, -cx * sz, cx * cz, sx, sx * sz, -sx * cz, cx };
			float z = m.wheelPos[i][2] + c.h.suspLower;
			if (c.frac[i] < 1.0f) z = v.lineStart[i][2] - c.frac[i] * v.lineLen[i] + m.wheelRadius[front ? 0 : 1];
			s32 M[12];
			for (int k = 0; k < 9; k++) M[k] = (s32)(R[k] * sc * 4096.0f);
			M[9] = (s32)(m.wheelPos[i][0] * 4096.0f);
			M[10] = (s32)(m.wheelPos[i][1] * 4096.0f);
			M[11] = (s32)(z * 4096.0f);
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
	glPolyFmt(POLY_ALPHA(alpha > 31 ? 31 : alpha) | POLY_CULL_NONE | POLY_ID(id) | POLY_FOG);
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

void draw(const View& w) {
	const u32 t0 = timing::ticks();
	uint16_t drawn = 0;
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
	for (Car& c : s_cars) {
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || !c.placed || !vehmdl::ready(v.model)) continue;
		const float dx = v.pos[0] - w.eyeX, dy = v.pos[1] - w.eyeY;
		if (dx * dx + dy * dy > (float)(dbg::opts.fogView * dbg::opts.fogView)) continue;
		// behind the camera (and not right next to it)
		const float fx = -sinf(w.yaw), fy = cosf(w.yaw);
		if (dx * fx + dy * fy < -6.0f) continue;
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
	// fire on burning cars, smoke on damaged ones, explosion fireballs
	const tex::Texture* glow = sky::glowTexture();
	const tex::Texture* puff = sky::puffTexture();
	int id = 40;
	const uint32_t now = timing::vblanks();
	for (Car& c : s_cars) {
		Vehicle& v = c.v;
		if (v.status == STATUS_FREE || !c.placed) continue;
		const Model* m = vehmdl::get(v.model);
		if (!m) continue;
		const bool fire = (v.status != STATUS_WRECKED && v.health < 250.0f) || (v.status == STATUS_WRECKED && c.wreckBurn > 0.0f);
		float e[3];
		const float el[3] = { m->engine[0], m->engine[1], m->bmax[2] * 0.6f };
		toWorld(v, el, e);
		if (fire) {
			for (int k = 0; k < 3; k++) {
				float q[3] = { e[0], e[1], e[2] + 0.25f + 0.3f * k };
				const float flick = 0.45f + 0.15f * (float)((now * 7 + k * 13) % 5) / 4.0f;
				sprite(w, q, flick * (v.status == STATUS_WRECKED ? 1.4f : 1.0f), RGB15(31, 18 - k * 5, 4), 26 - k * 5, glow, id++);
			}
		}
		if (v.health < 390.0f || fire) {
			const float t = (float)(now % 90) / 90.0f;
			float q[3] = { e[0], e[1], e[2] + 0.6f + t * 2.0f };
			const int shade = v.health < 250.0f || v.status == STATUS_WRECKED ? 6 : 16;
			sprite(w, q, 0.5f + t * 0.8f, RGB15(shade, shade, shade), (int)(18.0f * (1.0f - t)) + 2, puff, id++);
		}
	}
	for (Fx& f : s_fx) {
		if (!f.used) continue;
		const float t = f.t;
		if (t < 1.2f) {
			const float s = 1.0f + 3.5f * fminf(t / 0.35f, 1.0f);
			const int a = (int)(31.0f * (1.0f - t / 1.2f));
			sprite(w, f.pos, s, RGB15(31, 26, 10), a, glow, id++);
			const float q[3] = { f.pos[0], f.pos[1], f.pos[2] + 0.8f };
			sprite(w, q, s * 0.8f, RGB15(31, 12, 2), a, glow, id++);
		}
		const float q[3] = { f.pos[0], f.pos[1], f.pos[2] + 1.0f + t * 2.5f };
		sprite(w, q, 1.5f + t * 1.5f, RGB15(4, 4, 4), (int)(24.0f * (1.0f - t / 3.0f)), puff, id++);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
	s_stats.drawn = drawn;
	s_stats.polys = (uint16_t)tris;
	s_stats.drawUs = timing::usSince(t0);
}

void init() {
	removeAll();
	for (Debris& d : s_debris) d.used = false;
	for (Fx& f : s_fx) f.used = false;
}

}
