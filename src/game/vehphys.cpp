// Vehicle physics in fixed point (fx::q = Q8.24, positions Q24 in s64).
// Ported from CPhysical / CAutomobile / CVehicle (ProcessWheel, FlyingControl,
// ProcessBoatControl) / cTransmission in gta-reversed, with every force kept
// per unit mass (ApplyMoveForce(f) -> move += f / m, so the f = x * m terms of
// the original lose their m; ApplyTurnForce divides by m_fTurnMass, which
// becomes `kTurn` = m / turnMass on a per-mass impulse). Floats only at the
// edges: spawn, publish() for the rest of the game, damage bookkeeping.
#include <initializer_list>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "game/collision.h"
#include "game/skidmarks.h"
#include "game/vehicledata.h"
#include "game/vehicleint.h"
#include "game/waterlevel.h"
#include "game/weather.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/timing.h"

namespace veh {

using fx::F;
using fx::ONE;
using fx::mul;
using fx::div;
using fx::dot;
using fx::cross;
using fx::normalise;

static constexpr q ELASTICITY = F(0.05);                // CAutomobile::m_fElasticity
static constexpr q FRICTION = F(0.25);                  // body vs map sliding friction
static constexpr q WS_TRAC_FRAC_LIMIT = F(0.3);
static constexpr q WS_ALREADY_SPINNING_LOSS = F(0.2);
static constexpr q DAMPING_LIMIT_OF_SPRING_FORCE = F(0.999);
static constexpr q TRANSMISSION_SMOOTHER_FRAC = F(0.85);
static constexpr q BIKE_HALF_TRACK = F(0.22);           // the two line pairs of a bike
static constexpr q HELI_ROTOR_FULL = F(0.22);           // CHeli rotor speed at take-off
static constexpr int32_t CEILING_M = 800;               // CVehicle::HeightAboveCeiling (planes / helis)
enum WheelState : uint8_t { WHEEL_NORMAL, WHEEL_SPINNING, WHEEL_SKIDDING, WHEEL_FIXED };

u32 g_colTicks;
static q s_adhesion[6];             // tyre (RUBBER) vs adhesion group
static bool s_tables;

// ---- conversions ----------------------------------------------------------------
static inline fx::wp wpOf(float x) { return (fx::wp)(x * 16777216.0f); }
static inline float floatOf(fx::wp w) { return (float)(int32_t)(w >> 12) * (1.0f / 4096.0f); }
// world cu (1/128 m) of pos + rel
static inline void cuOf(const Car& c, V rel, int32_t out[3]) {
	for (int k = 0; k < 3; k++) out[k] = (int32_t)((c.pos[k] + rel[k]) >> 17);
}
static inline q fromCu(int32_t v) { return v << 17; }

// ---- CPhysical ------------------------------------------------------------------
static inline V rot(const Car& c, V l) { return c.right * l.x + c.fwd * l.y + c.up * l.z; }
static inline V unrot(const Car& c, V w) { return { dot(w, c.right), dot(w, c.fwd), dot(w, c.up) }; }
// CPhysical::GetSpeed (p relative to the entity position, cw = the world COM offset)
static inline V speedAt(const Car& c, V p, V cw) { return cross(c.turn, p - cw) + c.move; }

// CPhysical::GetMass(pos, dir) / m_fMass = 1 / (|p x dir|^2 m / turnMass + 1)
static q massRel(const Car& c, V p, V dir) {
	const V cr = cross(p, dir);
	const int64_t s = fx::dot48(cr, cr) >> fx::SH;
	int64_t t = ((s * c.k.kTurn) >> fx::SH) + ONE;
	int64_t n = 1LL << 48;
	while (t >= (1LL << 31)) { t >>= 1; n >>= 1; }
	return div64(n, (s32)t);
}

static inline void applyMove(Car& c, V u) { c.move += u; }
// ApplyTurnForce of a per-mass force u at p
static inline void applyTurn(Car& c, V u, V p, V cw) { c.turn += cross(p - cw, u) * c.k.kTurn; }
// ApplyTurnForce(a * m_fTurnMass, p): FlyingControl's torques come per turn mass
static inline void applyTurnRaw(Car& c, V a, V p, V cw) { c.turn += cross(p - cw, a); }
static inline void applyForce(Car& c, V u, V p, V cw) { applyMove(c, u); applyTurn(c, u, p, cw); }

static void reorthogonalise(Car& c) {
	normalise(c.fwd);
	c.right = cross(c.fwd, c.up);
	normalise(c.right);
	c.up = cross(c.right, c.fwd);
	normalise(c.up);
}

static void buildTables() {
	if (s_tables) return;
	s_tables = true;
	for (int g = 0; g < 6; g++) s_adhesion[g] = fx::fromFloat(vehdata::groupAdhesion(g));
}

// SurfaceInfos_c::GetAdhesiveLimit * GetWetMultiplier
static q tyreAdhesion(uint8_t surface) {
	const q a = s_adhesion[vehdata::surfaceGroup(surface)];
	const int wet = vehdata::wetGrip(surface);
	if (!wet) return a;
	const q wr = fx::fromFloat(weather::g.wetRoads);
	return mul(a, ONE + mul(wr, wet * (ONE / 100)));
}

// ---- cTransmission ------------------------------------------------------------------
static void initGears(Car& c) {
	const int n = c.k.numGears;
	memset(c.gears, 0, sizeof c.gears);
	const q maxV = c.k.maxVel, avgHalf = maxV / (2 * n), maxGear = maxV - avgHalf;
	for (int i = 1; i <= n; i++) {
		Gear& g = c.gears[i];
		g.maxVel = (q)((int64_t)i * maxGear / n) + avgHalf;
		const q diff = g.maxVel - c.gears[i - 1].maxVel;
		if (i >= n) g.up = maxV;
		else {
			c.gears[i + 1].down = mul(F(0.42), diff) + c.gears[i - 1].maxVel;
			g.up = mul(F(0.6667), diff) + c.gears[i - 1].maxVel;
		}
	}
	c.gears[0].maxVel = c.k.maxRev;
	c.gears[0].up = -F(0.01);
	c.gears[0].down = c.k.maxRev;
	c.gears[1].down = -F(0.01);
}

// cTransmission::CalculateDriveAcceleration (no cheats / nitro)
static q driveAccel(Car& c, q gas, q vel) {
	const Hand& k = c.k;
	const int n = k.numGears;
	if (vel < k.maxRev) return 0;
	for (int guard = 0; vel <= k.maxVel && guard < 16; guard++) {
		const Gear& g = c.gears[c.gear];
		bool accelerate = false;
		if (vel > g.up) {
			if (c.gear == 0 && gas <= 0) accelerate = true;
			else if (c.gear < n) { c.gear++; continue; }
			else accelerate = true;
		} else {
			if (vel >= g.down || c.gear == 0 || (c.gear == 1 && gas >= 0)) accelerate = true;
			else { c.gear--; continue; }
		}
		if (!accelerate) break;
		q mult;
		if (n == 1) mult = ONE;
		else if (c.gear >= 1) {
			q gn = ONE - (c.gear - 1) * ONE / (n - 1);
			gn = mul(gn, gn);
			mult = gn * (k.hflags & 1 ? 5 : k.hflags & 2 ? 4 : 3) + ONE;
		} else mult = F(4.5);
		q a = mul(mul(mul(mult, k.engineAccel), F(0.4)), mul(gas, TS));
		if (c.driveOnGround) {
			const q maxChange = k.maxVel / (3 * n);
			q cur, span;
			if (c.gear == 0) { cur = maxChange - vel; span = maxChange - c.gears[0].down; }
			else if (c.gear == 1) { cur = maxChange + vel; span = maxChange + c.gears[1].up; }
			else { cur = vel - g.down; span = g.up - g.down; }
			const q ratio = span ? div(cur, span) : 0;
			const q inertia = ratio - c.inertia1;
			const q acc = fx::clamp(ONE - mul(inertia, k.engineInertia), F(0.1), ONE);
			c.inertia1 = ratio;
			c.inertia2 = mul(acc, ONE - TRANSMISSION_SMOOTHER_FRAC) + mul(TRANSMISSION_SMOOTHER_FRAC, c.inertia2);
			a = mul(a, c.inertia2);
		}
		return a;
	}
	return 0;
}

// ---- train tracks (CTrain::ReadAndInterpretTrackFile, loaded on the first train) ----
static ds::TrackInfo* s_tracks;
static ds::TrackNode* s_nodes;
static int s_numTracks;
static bool s_tracksTried;

static bool loadTracks() {
	if (s_tracksTried) return s_tracks != nullptr;
	s_tracksTried = true;
	const ds::VehHeader* h = vehdata::header();
	if (!h || !h->numTracks || !h->numTrackNodes) return false;
	s_tracks = (ds::TrackInfo*)mem::alloc(sizeof(ds::TrackInfo) * h->numTracks, mem::TAG_WORLD);
	s_nodes = (ds::TrackNode*)mem::alloc(sizeof(ds::TrackNode) * h->numTrackNodes, mem::TAG_WORLD);
	const uint32_t tb = sizeof(ds::TrackInfo) * h->numTracks;
	if (!s_tracks || !s_nodes || !vehdata::readAt(h->trackOffset, s_tracks, tb) ||
	    !vehdata::readAt(h->trackOffset + tb, s_nodes, sizeof(ds::TrackNode) * h->numTrackNodes)) {
		mem::free(s_tracks);
		mem::free(s_nodes);
		s_tracks = nullptr;
		return false;
	}
	s_numTracks = h->numTracks;
	hud::log("tracks: %d loops %lu nodes", s_numTracks, (unsigned long)h->numTrackNodes);
	return true;
}

// point at distance d (m) along track t
static void trackPoint(int t, float d, float out[3]) {
	const ds::TrackInfo& ti = s_tracks[t];
	if (ti.length <= 0.0f || !ti.count) { out[0] = out[1] = out[2] = 0.0f; return; }
	d = fmodf(d, ti.length);
	if (d < 0.0f) d += ti.length;
	const ds::TrackNode* n = s_nodes + ti.first;
	int lo = 0, hi = ti.count - 1;
	const uint32_t dq = (uint32_t)(d * 2.0f);
	while (lo < hi) {   // last node with dist <= d
		const int mid = (lo + hi + 1) / 2;
		if (n[mid].dist <= dq) lo = mid;
		else hi = mid - 1;
	}
	const ds::TrackNode& a = n[lo];
	const ds::TrackNode& b = n[lo + 1 < ti.count ? lo + 1 : 0];
	const float da = (float)a.dist * 0.5f;
	const float db = lo + 1 < ti.count ? (float)b.dist * 0.5f : ti.length;
	const float f = db > da ? (d - da) / (db - da) : 0.0f;
	out[0] = ((float)a.x + (float)(b.x - a.x) * f) * 0.125f;
	out[1] = ((float)a.y + (float)(b.y - a.y) * f) * 0.125f;
	out[2] = ((float)a.z + (float)(b.z - a.z) * f) * 0.125f;
}

static bool nearestTrack(float x, float y, int& track, float& dist) {
	if (!loadTracks()) return false;
	float best = 1e12f;
	for (int t = 0; t < s_numTracks; t++)
		for (int i = 0; i < s_tracks[t].count; i++) {
			const ds::TrackNode& n = s_nodes[s_tracks[t].first + i];
			const float dx = (float)n.x * 0.125f - x, dy = (float)n.y * 0.125f - y;
			if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; track = t; dist = (float)n.dist * 0.5f; }
		}
	return best < 1e12f;
}

bool spawnPoint(int vehIndex, float x, float y, float& ox, float& oy, float& oz) {
	const uint8_t t = vehdata::type(vehIndex);
	if (t == ds::VEH_TRAIN) {
		int track;
		float d, p[3];
		if (!nearestTrack(x, y, track, d)) return false;
		trackPoint(track, d, p);
		ox = p[0]; oy = p[1]; oz = p[2] + 1.0f;
		return true;
	}
	ds::VehInfo h;
	if (t == ds::VEH_BOAT || (t == ds::VEH_PLANE && vehdata::read(vehIndex, h) && h.extKind == ds::VEHEXT_BOAT)) {
		float wz;
		if (water::levelAt(x, y, wz)) { ox = x; oy = y; oz = wz; return true; }
		return water::nearest(x, y, ox, oy, oz);
	}
	return false;
}

// CTrain on rails: position / axes from the track under the bogies
static void trainPose(Car& c) {
	const float half = (fx::toFloat(c.bmax.y) - fx::toFloat(c.bmin.y)) * 0.35f;
	float p[3], f[3], b[3];
	trackPoint(c.track, c.trackDist, p);
	trackPoint(c.track, c.trackDist + half, f);
	trackPoint(c.track, c.trackDist - half, b);
	float d[3] = { f[0] - b[0], f[1] - b[1], f[2] - b[2] };
	const float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (l > 1e-3f) for (float& x : d) x /= l;
	else { d[0] = 0.0f; d[1] = 1.0f; d[2] = 0.0f; }
	c.fwd = fx::fromFloat3(d);
	c.up = { 0, 0, ONE };
	reorthogonalise(c);
	// rails under the wheel bottoms: wheel z = line start - suspension upper
	const float lift = fx::toFloat(c.wheelR[0]) - (fx::toFloat(c.lineStart[0].z) - fx::toFloat(c.k.suspUpper));
	c.pos[0] = wpOf(p[0]);
	c.pos[1] = wpOf(p[1]);
	c.pos[2] = wpOf(p[2] + lift);
}

// ---- setup ------------------------------------------------------------------------
static void convertHandling(Car& c) {
	const ds::VehInfo& h = c.h;
	Hand& k = c.k;
	k = {};
	k.traction = fx::fromFloat(h.tractionMult / 250.0f * 0.25f);
	k.tractionLoss = fx::fromFloat(h.tractionLoss);
	k.tractionBias = fx::fromFloat(h.tractionBias);
	k.maxVel = fx::fromFloat(h.maxVelocity);
	k.maxRev = fx::fromFloat(h.maxReverseVelocity);
	k.engineAccel = fx::fromFloat(h.engineAccel);
	k.engineInertia = fx::fromFloat(h.engineInertia);
	k.brakeDecel = fx::fromFloat(h.brakeDecel);
	k.brakeBias = fx::fromFloat(h.brakeBias);
	k.steerLock = fx::fromFloat(h.steeringLock * (3.14159265f / 180.0f));
	k.suspForce = fx::fromFloat(h.suspForce);
	k.suspDamp = fx::fromFloat(h.suspDamping);
	k.suspUpper = fx::fromFloat(h.suspUpper);
	k.suspLower = fx::fromFloat(h.suspLower);
	k.suspBias = fx::fromFloat(h.suspBias);
	k.antiDive = fx::fromFloat(h.suspAntiDive);
	k.idleBrake = fx::fromFloat(0.9f / h.mass * (h.mass < 500.0f ? 0.1f : 1.0f));
	const float kt = h.turnMass > 1.0f ? h.mass / h.turnMass : 1.0f;
	k.kTurn = fx::fromFloat(kt > 100.0f ? 100.0f : kt);
	k.airRes = fx::fromFloat(h.dragMult <= 0.01f ? h.dragMult : h.dragMult / 1000.0f / 2.0f);
	k.numGears = (uint8_t)(h.numGears < 1 ? 1 : h.numGears > 6 ? 6 : h.numGears);
	k.drive = (char)h.driveType;
	k.hflags = h.handlingFlags;
	k.mflags = h.modelFlags;
	if (h.extKind == ds::VEHEXT_BIKE) {
		k.maxLean = fx::fromFloat(asinf(fminf(h.ext[4], 0.99f)));
		k.desLean = fx::fromFloat(h.ext[6]);
		k.speedSteer = fx::fromFloat(h.ext[7]);
		k.slipSteer = fx::fromFloat(h.ext[8]);
	} else if (h.extKind == ds::VEHEXT_BOAT) {
		k.thrustY = fx::fromFloat(h.ext[0]);
		k.thrustZ = fx::fromFloat(h.ext[1]);
		k.thrustAppZ = fx::fromFloat(h.ext[2]);
		k.aqForce = fx::fromFloat(h.ext[3]);
		k.aqLimit = fx::fromFloat(h.ext[4]);
		k.aqOffset = fx::fromFloat(h.ext[5]);
		for (int i = 0; i < 3; i++) {
			k.boatMoveRes[i] = fx::fromFloat(powf(h.ext[7 + i], 50.0f / 60.0f));
			k.boatTurnRes[i] = fx::fromFloat(powf(h.ext[10 + i], 50.0f / 60.0f));
		}
	}
	if (h.hasFlying) {
		const float* f = h.fly;
		k.fly = true;
		k.thrust = fx::fromFloat(f[0]);
		k.thrustFallOff = fx::fromFloat(f[1]);
		k.yaw = fx::fromFloat(f[2]);
		k.yawStab = fx::fromFloat(f[3]);
		k.sideSlip = fx::fromFloat(f[4]);
		k.roll = fx::fromFloat(f[5]);
		k.rollStab = fx::fromFloat(f[6]);
		k.pitch = fx::fromFloat(f[7]);
		k.pitchStab = fx::fromFloat(f[8]);
		k.formLift = fx::fromFloat(f[9]);
		k.attackLift = fx::fromFloat(f[10]);
		k.gearDownL = fx::fromFloat(f[12]);
		k.windMult = fx::fromFloat(f[13]);
		k.moveResTS = fx::fromFloat(powf(f[14], 50.0f / 60.0f));
		for (int i = 0; i < 3; i++) {
			k.lnTurnRes[i] = fx::fromFloat(logf(fmaxf(f[15 + i], 1e-4f)) * (50.0f / 60.0f));
			k.speedRes[i] = fx::fromFloat(fminf(f[18 + i], 120.0f));
		}
	}
}

void physSetup(Car& c) {
	buildTables();
	Vehicle& v = c.v;
	const vehmdl::Model* m = vehmdl::get(v.model);
	convertHandling(c);
	initGears(c);
	c.bmin = fx::fromFloat3(m->bmin);
	c.bmax = fx::fromFloat3(m->bmax);
	c.wheelR[0] = fx::fromFloat(m->wheelRadius[0]);
	c.wheelR[1] = fx::fromFloat(m->wheelRadius[1]);
	c.com = fx::fromFloat3(c.h.com);
	// CAutomobile::SetupSuspensionLines (bikes: two narrow pairs)
	for (int i = 0; i < 4; i++) {
		const bool front = i == vehmdl::WHEEL_FL || i == vehmdl::WHEEL_FR;
		const q r = c.wheelR[front ? 0 : 1];
		V w = fx::fromFloat3(m->wheelPos[i]);
		if (v.cls == CLS_BIKE) w.x = i == vehmdl::WHEEL_FL || i == vehmdl::WHEEL_RL ? -BIKE_HALF_TRACK : BIKE_HALF_TRACK;
		c.lineStart[i] = c.lineEnd[i] = w;
		c.lineStart[i].z = w.z + c.k.suspUpper;
		c.lineEnd[i].z = w.z + c.k.suspLower - r;
		c.lineLen[i] = c.lineStart[i].z - c.lineEnd[i].z;
		c.susp[i] = c.frac[i] = ONE;
		c.skidMark[i] = -1;
	}
	c.springLen = c.k.suspUpper - c.k.suspLower;
	// body spheres: two rows along the length + the cabin
	const float W = m->bmax[0] - m->bmin[0], L = m->bmax[1] - m->bmin[1], H = m->bmax[2] - m->bmin[2];
	float r = fminf(W * 0.42f, H * 0.45f);
	r = r < 0.3f ? 0.3f : r > 1.4f ? 1.4f : r;
	const float xo = W * 0.5f - r > 0.1f ? W * 0.5f - r : 0.0f;
	const float zLow = m->bmin[2] + r + (v.cls == CLS_BOAT ? 0.0f : 0.2f);
	// long vehicles (buses, planes, trains) get more rows
	const int rows = L > 9.0f ? 4 : 3;
	c.numSph = 0;
	for (int k = 0; k < rows; k++) {
		const float y = m->bmin[1] + r + (L - 2.0f * r) * (float)k / (float)(rows - 1);
		for (int s = 0; s < (xo > 0.0f ? 2 : 1) && c.numSph < NUM_SPH; s++) {
			const float sp[3] = { s ? xo : -xo, y, zLow };
			c.sph[c.numSph] = fx::fromFloat3(sp);
			c.sphR[c.numSph++] = fx::fromFloat(r);
		}
	}
	const float rt = fminf(fmaxf(W * 0.35f, 0.3f), 0.9f);
	for (int k = 0; k < 2 && L > 2.5f && c.numSph < NUM_SPH && v.cls != CLS_BIKE; k++) {
		const float sp[3] = { 0.0f, (m->bmin[1] + m->bmax[1]) * 0.5f + (k ? -0.15f : 0.15f) * L, m->bmax[2] - rt };
		c.sph[c.numSph] = fx::fromFloat3(sp);
		c.sphR[c.numSph++] = fx::fromFloat(rt);
	}
	float rad = 0.0f;
	for (int k = 0; k < 3; k++) rad += fmaxf(m->bmax[k] * m->bmax[k], m->bmin[k] * m->bmin[k]);
	c.radius = fx::fromFloat(sqrtf(rad) + 0.5f);
	// boats: per-sample buoyancy so the hull floats at percentSubmerged of its height
	if (v.cls == CLS_BOAT) {
		const float sub = c.h.percentSubmerged > 5 ? c.h.percentSubmerged / 100.0f : 0.5f;
		const float hull = fmaxf(H * 0.6f, 0.4f);
		c.k.buoy = fx::fromFloat(0.008f * (50.0f / 60.0f) / (6.0f * sub * hull));
	}
	physPlace(c);
}

void physPlace(Car& c) {
	Vehicle& v = c.v;
	if (v.cls == CLS_TRAIN) {
		float dist = 0.0f;
		int track = 0;
		if (nearestTrack(v.pos[0], v.pos[1], track, dist)) {
			c.track = (int8_t)track;
			c.trackDist = dist;
			trainPose(c);
		} else {
			c.track = -1;
		}
		return;
	}
	float wz;
	if (v.cls == CLS_BOAT && water::levelAt(v.pos[0], v.pos[1], wz) && wz > v.pos[2] - 6.0f) {
		col::Hit hit;
		// on the water unless the ground under it is higher
		if (!col::groundAt(v.pos[0], v.pos[1], v.pos[2] + 3.0f, v.pos[2] - 20.0f, hit) || hit.z < wz) {
			c.pos[2] = wpOf(wz - fx::toFloat(c.bmin.z) * 0.6f);
			return;
		}
	}
	// CalculateHeightAboveRoad: drop the car onto the ground under it
	const float spring = fx::toFloat(c.springLen);
	const float hab = (1.0f - 1.0f / (c.h.suspForce * 4.0f)) * spring + fx::toFloat(c.wheelR[0]) - fx::toFloat(c.lineStart[0].z);
	col::Hit hit;
	if (col::groundAt(v.pos[0], v.pos[1], v.pos[2] + 3.0f, v.pos[2] - 20.0f, hit))
		c.pos[2] = wpOf(hit.z + (v.cls == CLS_BOAT ? -fx::toFloat(c.bmin.z) : hab));
}

void pull(Car& c) {
	const Vehicle& v = c.v;
	for (int k = 0; k < 3; k++) c.pos[k] = wpOf(v.pos[k]);
	c.right = fx::fromFloat3(v.right);
	c.fwd = fx::fromFloat3(v.fwd);
	c.up = fx::fromFloat3(v.up);
	c.move = fx::fromFloat3(v.moveSpeed);
	reorthogonalise(c);
}

void publish(Car& c) {
	Vehicle& v = c.v;
	for (int k = 0; k < 3; k++) v.pos[k] = floatOf(c.pos[k]);
	fx::toFloat3(c.right, v.right);
	fx::toFloat3(c.fwd, v.fwd);
	fx::toFloat3(c.up, v.up);
	fx::toFloat3(c.move, v.moveSpeed);
	for (int i = 0; i < 4; i++) v.susp[i] = fx::toFloat(c.susp[i]);
	v.steer = fx::toFloat(c.steer);
	v.rawSteer = fx::toFloat(c.rawSteer);
	v.gas = fx::toFloat(c.gas);
	v.brake = fx::toFloat(c.brake);
	v.gear = c.gear;
	v.numContact = c.numContact;
	v.speedFwd = fx::toFloat(c.speedFwd);
	v.lean = fx::toFloat(c.lean);
	if (c.impactJ > 0) {
		c.damageIntensity = fx::toFloat(c.impactJ) * c.h.mass;
		V p = c.impactRel;
		for (int k = 0; k < 3; k++) c.impactPos[k] = v.pos[k] + fx::toFloat(p[k]);
	}
}

// ---- CAutomobile::ProcessControlInputs ------------------------------------------------
static void controlInputs(Car& c) {
	Vehicle& v = c.v;
	const Controls& p = c.pad;
	v.handbrake = p.handbrake;
	const q padSteer = fx::fromFloat(p.steer), padAccel = fx::fromFloat(p.accel), padBrake = fx::fromFloat(p.brake);
	c.rawSteer += mul((-padSteer - c.rawSteer) / 5, TS);
	c.rawSteer = fx::clamp(c.rawSteer, -ONE, ONE);
	const q fwd = dot(c.move, c.fwd);
	const q in = padAccel - padBrake;
	if (fx::abs(fwd) < F(0.01)) {
		if (padAccel > F(0.6) && padBrake > F(0.6)) { c.gas = padAccel; c.brake = padBrake; }
		else { c.gas = in; c.brake = 0; }
	} else if (fwd >= 0) {
		if (in < 0) { c.gas = 0; c.brake = -in; }
		else { c.gas = in; c.brake = 0; }
	} else if (in >= 0 && (c.gas <= F(0.5) || fwd <= -F(0.15))) {
		c.gas = 0;
		c.brake = in;
	} else {
		c.gas = in;
		c.brake = 0;
	}
	const q s = mul(c.rawSteer, fx::abs(c.rawSteer));
	q lock = c.k.steerLock;
	// CBike: the lock shrinks with speed (tBikeHandlingData::m_fSpeedSteer)
	if (v.cls == CLS_BIKE && fwd > 0) lock = mul(lock, fx::clamp(ONE - mul(fwd, c.k.speedSteer), F(0.25), ONE));
	c.steer = mul(lock, s);
	v.horn = p.horn;
	v.pitchIn = p.pitch;
	v.yawIn = p.yaw;
	v.rollIn = p.steer;
}

void controlsIdle(Car& c) {
	Vehicle& v = c.v;
	c.gas = 0;
	c.brake = v.status == STATUS_WRECKED ? ONE : F(0.3);
	v.handbrake = v.cls != CLS_BOAT;
	v.horn = false;
	c.rawSteer = mul(c.rawSteer, F(0.95));
	c.steer = mul(c.k.steerLock, mul(c.rawSteer, fx::abs(c.rawSteer)));
	v.pitchIn = v.yawIn = v.rollIn = 0.0f;
}

// ---- CVehicle::ProcessWheel -----------------------------------------------------------
static void processWheel(Car& c, int i, V wf, V wr, V cs, V cp, V cw, q thrust, q brake, q adhesion) {
	const Vehicle& v = c.v;
	bool alreadySkidding = false;
	q right = 0, fwd = 0;
	const q sFwd = dot(wf, cs), sRight = dot(wr, cs);
	const bool braking = brake != 0;
	const bool driving = !braking && thrust != 0;
	const int on = c.numContact ? c.numContact : 1;
	adhesion = mul(adhesion, TS);
	uint8_t& state = c.wheelState[i];
	const bool player = v.status == STATUS_PLAYER;
	if (state != WHEEL_NORMAL) {
		alreadySkidding = true;
		adhesion = mul(adhesion, c.k.tractionLoss);
		if (state == WHEEL_SPINNING && player) adhesion = mul(adhesion, ONE - mul(fx::abs(c.gas), WS_ALREADY_SPINNING_LOSS));
	}
	state = WHEEL_NORMAL;
	if (sRight) right = -(sRight / on);
	if (driving) {
		fwd = thrust;
		right = fx::clamp(right, -adhesion, adhesion);
	} else if (sFwd) {
		fwd = -(sFwd / on);
		if (!braking && fx::abs(c.gas) < F(0.01)) brake = c.k.idleBrake;
		if (brake > adhesion) {
			if (fx::abs(sFwd) > F(0.005)) state = WHEEL_FIXED;
		} else {
			fwd = fx::clamp(fwd, -brake, brake);
		}
	}
	const int64_t slipSq = (int64_t)right * right + (int64_t)fwd * fwd;
	c.slip[i] = 0;
	if (slipSq > (int64_t)adhesion * adhesion) {
		if (state != WHEEL_FIXED) {
			const q lim = sFwd > F(0.15) && (i == vehmdl::WHEEL_FL || i == vehmdl::WHEEL_FR) ? 2 * WS_TRAC_FRAC_LIMIT : WS_TRAC_FRAC_LIMIT;
			state = driving && mul(lim, adhesion) < fx::abs(fwd) ? WHEEL_SPINNING : WHEEL_SKIDDING;
		}
		q loss = ONE;
		if (!alreadySkidding && state == WHEEL_SPINNING && player)
			loss = mul(c.k.tractionLoss, ONE - mul(fx::abs(c.gas), WS_ALREADY_SPINNING_LOSS));
		const q sl = fx::sqrt48(slipSq);
		c.slip[i] = adhesion > 0 ? div(sl, adhesion) - ONE : 0;
		loss = mul(loss, div(adhesion, sl));
		fwd = mul(fwd, loss);
		right = mul(right, loss);
	} else if (state == WHEEL_FIXED) {
		c.slip[i] = ONE;
	}
	if (fwd || right) {
		V mv = wf * fwd + wr * right;
		V turn = mv;
		const bool sep = c.k.antiDive > 0 && (braking || driving);
		if (sep) turn = fx::madd(turn, wf, -mul(mul(braking ? ONE : F(0.5), c.k.antiDive), fwd));
		const q speed = normalise(mv);
		q tspeed = speed;
		if (sep) tspeed = normalise(turn);
		else turn = mv;
		applyMove(c, mv * speed);
		applyTurn(c, turn * mul(massRel(c, cp, turn), tspeed), cp, cw);
	}
}

// ---- CAutomobile::ProcessCarWheelPair ---------------------------------------------------
static void wheelPair(Car& c, int l, int r, q steer, bool canSteer, const V* cs, const V* cp, V cw, q traction, q accel,
                      q brake, bool front) {
	const Vehicle& v = c.v;
	const char dt = c.k.drive;
	const bool drive = dt == '4' || (front ? dt == 'F' : dt == 'R');
	q bias = 2 * c.k.suspBias;
	if (!front) {
		bias = 2 * ONE - bias;
		if (v.handbrake && !(c.k.hflags & 0x40)) brake = fx::fromInt(100);
	}
	if (c.wheelCounts[l] <= 0 && c.wheelCounts[r] <= 0) return;
	const q ss = canSteer ? fx::sin(steer) : 0, sc = canSteer ? fx::cos(steer) : ONE;
	const q brakeBias = front ? 2 * c.k.brakeBias : 2 * ONE - 2 * c.k.brakeBias;
	const q tracBias = front ? 2 * c.k.tractionBias : 2 * ONE - 2 * c.k.tractionBias;
	for (int w : { l, r }) {
		if (c.wheelCounts[w] <= 0) continue;
		const V n = c.contactN[w];
		V wf = fx::madd(c.fwd, n, -dot(c.fwd, n));
		normalise(wf);
		V wr = cross(wf, n);
		normalise(wr);
		if (canSteer) {
			const V t = wf * sc - wr * ss;
			wr = wf * ss + wr * sc;
			wf = t;
		}
		q adhesion = mul(tyreAdhesion(c.surf[w]), traction);
		adhesion = mul(adhesion, fx::min(mul(mul(bias, c.k.suspForce) * 4, ONE - c.susp[w]), 2 * ONE));
		if (v.burst & (1u << w)) adhesion = mul(adhesion, F(0.4));   // CAutomobile: burst tyre grip
		processWheel(c, w, wf, wr, cs[w], cp[w], cw, drive ? accel : 0, mul(brake, brakeBias), mul(adhesion, tracBias));
		if (drive && c.gas < 0 && c.wheelState[w] == WHEEL_SPINNING) c.wheelState[w] = WHEEL_NORMAL;
	}
}

// ---- suspension lines vs the map, springs ----------------------------------------------
static void castWheels(Car& c) {
	for (int i = 0; i < 4; i++) {
		V ls = c.lineStart[i], le = c.lineEnd[i];
		if (c.v.burst & (1u << i)) le.z += mul(c.wheelR[i == vehmdl::WHEEL_FL || i == vehmdl::WHEEL_FR ? 0 : 1], F(0.4));
		const V a = rot(c, ls), b = rot(c, le);
		int32_t wa[3], wb[3];
		cuOf(c, a, wa);
		cuOf(c, b, wb);
		col::HitCu h;
		if (col::castSegmentCu(wa, wb, h) && h.n[2] > 410) {
			const q t = h.t << 8;
			c.frac[i] = t;
			c.contactRel[i] = a + (b - a) * t;
			c.contactN[i] = { h.n[0] << 12, h.n[1] << 12, h.n[2] << 12 };
			c.surf[i] = h.surface;
			// CAutomobile::ProcessControl: line fraction -> spring compression
			const q len = le.z < ls.z ? ls.z - le.z : c.lineLen[i];
			const q wr = ONE - div(c.springLen, len);
			c.susp[i] = div(t - wr, ONE - wr);
		} else {
			c.frac[i] = ONE;
			c.susp[i] = ONE;
		}
	}
}

static void processSuspension(Car& c, V cw, V* cp) {
	V dir[4];
	q limit[4] = {};
	for (int i = 0; i < 4; i++) {
		dir[i] = -c.up;
		if (c.susp[i] >= ONE) continue;
		cp[i] = c.contactRel[i];
		// ApplySpringCollisionAlt
		const q bias = i == vehmdl::WHEEL_FL || i == vehmdl::WHEEL_FR ? c.k.suspBias : ONE - c.k.suspBias;
		const q stress = ONE - c.susp[i];
		V n = c.contactN[i];
		if (dot(dir[i], n) > 0) n = -n;
		limit[i] = mul(mul(mul(mul(stress, TS), c.k.suspForce), bias), F(0.016));
		applyForce(c, n * limit[i], cp[i], cw);
	}
	for (int i = 0; i < 4; i++) {
		if (c.susp[i] >= ONE) continue;
		const V cs = speedAt(c, cp[i], cw);
		if (c.contactN[i].z > F(0.35)) dir[i] = -c.contactN[i];
		// ApplySpringDampening
		const q a = dot(cs, dir[i]);
		const q dts = fx::clamp(mul(TS, c.k.suspDamp), -F(0.25), F(0.25));
		const q ds = -mul(dts, a);
		q fd = mul(massRel(c, cp[i] - cw, dir[i]), ds);
		const q lim = mul(fx::abs(limit[i]), DAMPING_LIMIT_OF_SPRING_FORCE);
		if (fd > lim) fd = lim;
		applyForce(c, dir[i] * fd, cp[i], cw);
	}
}

// ---- body spheres vs the map (ApplyCollisionAlt, static entity) + push-out ------------
static void collideWorld(Car& c) {
	V cw = rot(c, c.com);
	for (int s = 0; s < c.numSph; s++) {
		const V rel = rot(c, c.sph[s]);
		int32_t wc[3], disp[3];
		cuOf(c, rel, wc);
		uint8_t surf;
		if (!col::sphereContactCu(wc, c.sphR[s] >> 17, disp, surf)) continue;
		V n = { fromCu(disp[0]), fromCu(disp[1]), fromCu(disp[2]) };
		const V D = n;
		if (!normalise(n)) continue;
		const V push = D * F(0.9);
		for (int k = 0; k < 3; k++) c.pos[k] += push[k];
		const V p = rel - n * c.sphR[s] - push;
		const V vp = speedAt(c, p, cw);
		const q vn = dot(vp, n);
		if (vn >= 0) continue;
		const V d = p - cw;
		const q j = mul(mul(-(ONE + ELASTICITY), vn), massRel(c, d, n));
		applyForce(c, n * j, p, cw);
		// friction along the surface
		V vt = vp - n * vn;
		const q tl = normalise(vt);
		if (tl > 0) {
			const q fj = fx::min(mul(FRICTION, j), mul(tl, massRel(c, d, vt)));
			applyForce(c, vt * -fj, p, cw);
		}
		if (j > c.impactJ) {
			c.impactJ = j;
			c.impactRel = p + push;
		}
	}
}

// vehicle vs vehicle: sphere pairs, impulse on both (CPhysical::ApplyCollision)
void physPair(Car& A, Car& B) {
	V dAB;
	for (int k = 0; k < 3; k++) {
		const fx::wp d = A.pos[k] - B.pos[k];
		if (d > ((fx::wp)12 << fx::SH) || d < -((fx::wp)12 << fx::SH)) return;
		dAB[k] = (q)d;
	}
	const q reach = A.radius + B.radius;
	if (fx::dot48(dAB, dAB) > (int64_t)reach * reach) return;
	const float ma = A.h.mass, mb = B.h.mass;
	float rab = ma / mb;
	rab = rab < 1.0f / 64.0f ? 1.0f / 64.0f : rab > 64.0f ? 64.0f : rab;
	const q rAB = fx::fromFloat(rab), rBA = fx::fromFloat(1.0f / rab), ta = fx::fromFloat(mb / (ma + mb));
	const V cwA = rot(A, A.com), cwB = rot(B, B.com);
	for (int i = 0; i < A.numSph; i++) {
		const V wa = rot(A, A.sph[i]);
		for (int k = 0; k < B.numSph; k++) {
			const V wb = rot(B, B.sph[k]);
			V n = dAB + wa - wb;
			const q rr = A.sphR[i] + B.sphR[k];
			if (fx::dot48(n, n) >= (int64_t)rr * rr) continue;
			const q d = normalise(n);
			if (!d) continue;
			const q depth = rr - d;
			const V pa_ = n * mul(depth, ta), pb_ = n * (depth - mul(depth, ta));
			for (int q3 = 0; q3 < 3; q3++) { A.pos[q3] += pa_[q3]; B.pos[q3] -= pb_[q3]; }
			dAB = dAB + pa_ + pb_;
			const V pa = wa - n * A.sphR[i];
			const V pb = pa + dAB;
			const V va = speedAt(A, pa, cwA), vb = speedAt(B, pb, cwB);
			const q vn = dot(va - vb, n);
			if (vn >= 0) continue;
			const q kA = massRel(A, pa - cwA, n), kB = massRel(B, pb - cwB, n);
			// j / m_a and j / m_b of j = -1.1 vn / (1 / (m_a kA) + 1 / (m_b kB))
			const q dvA = div(mul(-F(1.1), vn), fx::inv(kA) + div(rAB, kB));
			const q dvB = div(mul(-F(1.1), vn), div(rBA, kA) + fx::inv(kB));
			applyForce(A, n * dvA, pa, cwA);
			applyForce(B, n * -dvB, pb, cwB);
			if (dvA > A.impactJ) { A.impactJ = dvA; A.impactRel = pa; }
			if (dvB > B.impactJ) { B.impactJ = dvB; B.impactRel = pb; }
			A.v.asleep = B.v.asleep = false;
			A.fakePhysics = B.fakePhysics = 0;
		}
	}
}

// ---- CBike: upright + the visual lean ---------------------------------------------------
static void bikeStabilise(Car& c) {
	if (c.v.status == STATUS_WRECKED || c.up.z < F(0.3)) return;
	// roll about the forward axis: right.z -> 0 (a + rotation about fwd lowers right.z)
	const q roll = c.right.z;
	const q rate = dot(c.turn, c.fwd);
	const q kp = c.numContact ? F(0.03) : F(0.006), kd = c.numContact ? F(0.35) : F(0.08);
	c.turn += c.fwd * (mul(roll, kp) - mul(rate, kd));
	// lean into the turn: v^2 / r against gravity, capped by MaxLean
	const q spd = fx::clamp(div(c.speedFwd, F(0.35)), 0, ONE);
	const q target = fx::clamp(mul(mul(-c.rawSteer, c.k.maxLean), spd), -c.k.maxLean, c.k.maxLean);
	c.lean += mul(target - c.lean, F(0.12));
}

// ---- CVehicle::FlyingControl --------------------------------------------------------------
static q heightAboveCeiling(const Car& c) {
	const int32_t z = (int32_t)(c.pos[2] >> fx::SH);
	return z > CEILING_M ? fx::fromInt(z - CEILING_M > 120 ? 120 : z - CEILING_M) : -ONE;
}

static void flyingControl(Car& c, bool heli) {
	const Hand& k = c.k;
	Vehicle& v = c.v;
	const bool player = v.status == STATUS_PLAYER;
	const V cw = rot(c, c.com);
	const V wind = { fx::fromFloat(-weather::g.windDir[0]), fx::fromFloat(-weather::g.windDir[1]), 0 };
	const V air = c.move + wind * k.windMult;
	const q accel = player ? fx::fromFloat(c.pad.accel - c.pad.brake) : 0;
	const q upDown = player ? fx::fromFloat(c.pad.pitch) : 0;
	const q lr = player ? fx::fromFloat(c.pad.steer) : 0;
	const q skidIn = player ? fx::fromFloat(c.pad.yaw) : 0;
	const q g8 = mul(F(0.008), TS);
	if (!heli) {
		// FLIGHTMODEL_PLANE
		const q steeringUpDown = -upDown;
		const V tailOffset = c.fwd * c.bmin.y;
		const q fwdSpeed = dot(air, c.fwd);
		bool anyWheel = false;
		for (int i = 0; i < 4; i++) anyWheel |= c.susp[i] < ONE;
		q thrust = 0;
		if (accel <= 0 && anyWheel) {
			if (accel != 0 && dot(c.fwd, c.move) < F(0.02)) thrust = mul(fx::min(accel - mul(fwdSpeed, F(7.76)), 0), k.thrust);
		} else {
			thrust = mul(mul(ONE + accel, k.thrust), F(0.5));
			if (fwdSpeed > 0 && k.thrustFallOff < ONE) {
				const q fo = k.thrustFallOff;
				const q falloff = fo < 0 ? mul(mul(fwdSpeed + fo, fwdSpeed + fo), F(3.0)) : mul(mul(fwdSpeed - fo, fwdSpeed - fo), F(0.65));
				thrust = mul(thrust, fx::max(ONE - falloff, 0));
			}
		}
		applyMove(c, c.fwd * mul(thrust, g8));
		const q sideSpeed = -dot(air, c.right);
		applyMove(c, c.right * mul(mul(mul(sideSpeed, fx::abs(sideSpeed)), k.sideSlip), TS));
		const q tailSide = -dot(speedAt(c, tailOffset, cw), c.right);
		const q yawT = mul(mul(mul(tailSide, fx::abs(tailSide)), k.yawStab) + mul(mul(fwdSpeed, k.yaw), skidIn), TS);
		applyTurnRaw(c, c.right * yawT, tailOffset + cw, cw);
		const q rollT = mul(mul(mul(k.roll, fwdSpeed), lr), TS);
		applyTurnRaw(c, c.right * rollT, c.up + cw, cw);
		V horiz = cross(c.fwd, V{ 0, 0, ONE });
		if (c.up.z <= 0) horiz = -horiz;
		const q rollSide = c.right.z > 0 ? -ONE : ONE;
		q rst = mul(mul(ONE - fx::abs(c.fwd.z), ONE - dot(horiz, c.right)), rollSide);
		rst = mul(mul(mul(rst, k.rollStab), TS), F(0.5));
		applyTurnRaw(c, c.right * rst, c.up + cw, cw);
		const q tailUp = -dot(speedAt(c, tailOffset, cw), c.up);
		const q pitchT = mul(mul(mul(tailUp, fx::abs(tailUp)), k.pitchStab) + mul(mul(fwdSpeed, k.pitch), steeringUpDown), TS);
		applyTurnRaw(c, c.up * pitchT, tailOffset + cw, cw);
		const q airLen = fx::max(F(0.01), fx::len(air));
		const q attack = -fx::asin(fx::clamp(div(dot(air, c.up), airLen), -ONE, ONE));
		const q formLift = mul(k.formLift, k.gearDownL);
		q lift = mul(mul(mul(attack, k.attackLift) + formLift, TS), mul(fwdSpeed, fwdSpeed));
		if (lift > g8) {
			const q hc = heightAboveCeiling(c);
			if (hc > 0) lift = mul(fx::max(0, ONE - mul(hc, F(0.02))), g8);
		}
		applyMove(c, c.up * lift);
	} else {
		// FLIGHTMODEL_HELI
		c.move *= k.moveResTS;
		const V thrustDir = c.up;
		q thrustAxis = dot(air, thrustDir);
		if (thrustAxis > 0) thrustAxis *= 2;
		applyMove(c, V{ 0, 0, mul(F(0.5) - c.move.z, g8) });   // hover compensation
		q rotor = mul(accel, k.thrust) + F(0.45) - mul(thrustAxis, k.thrustFallOff);
		const q hc = heightAboveCeiling(c);
		if (hc > 0) rotor = div(rotor * 10, hc + fx::fromInt(10));
		applyMove(c, thrustDir * mul(rotor, g8));
		q pitchLevel;
		if (c.up.z <= 0) {
			const q rr = c.right.z < 0 ? k.formLift : -k.formLift;
			applyTurnRaw(c, c.up * mul(mul(rr, k.attackLift), TS), c.right + cw, cw);
			pitchLevel = mul(c.fwd.z < 0 ? k.formLift : -k.formLift, k.attackLift);
		} else {
			V wtu = V{ 0, 0, ONE } + V{ fx::fromFloat(weather::g.windDir[0]), fx::fromFloat(weather::g.windDir[1]), 0 } * k.windMult;
			normalise(wtu);
			const q rollTilt = fx::clamp(dot(wtu, c.right), -k.formLift, k.formLift);
			applyTurnRaw(c, c.up * -mul(mul(rollTilt, k.attackLift), TS), c.right + cw, cw);
			const q pitchTilt = fx::clamp(dot(wtu, c.fwd), -k.formLift, k.formLift);
			pitchLevel = -mul(pitchTilt, k.attackLift);
		}
		applyTurnRaw(c, c.up * mul(pitchLevel, TS), c.fwd + cw, cw);
		applyTurnRaw(c, c.up * mul(mul(upDown, k.pitch), TS), c.fwd + cw, cw);
		applyTurnRaw(c, c.up * mul(mul(-lr, k.roll), TS), c.right + cw, cw);
		const q sideSpeed = -dot(air, c.right);
		applyMove(c, c.right * mul(mul(mul(sideSpeed, fx::abs(sideSpeed)), k.sideSlip), TS));
		const q yawStabT = mul(mul(mul(sideSpeed, fx::abs(sideSpeed)), k.yawStab) + mul(skidIn, k.yaw), TS);
		applyTurnRaw(c, c.right * yawStabT, -c.fwd + cw, cw);
		applyTurnRaw(c, c.fwd * mul(mul(skidIn, k.yaw), TS), c.right + cw, cw);
	}
	// resistance
	V local = unrot(c, c.turn);
	const q moveLen = fx::len(c.move);
	const q dampExp = heli ? moveLen + ONE : moveLen * 2;
	V dampDelta = { 0, 0, 0 };
	for (int i = 0; i < 3; i++) {
		if (k.speedRes[i] <= 0) {
			local[i] = mul(local[i], fx::exp(mul(dampExp, k.lnTurnRes[i])));
		} else {
			const q base = ONE + mul(mul(local[i], local[i]), k.speedRes[i]);
			const q e = fx::exp(k.lnTurnRes[i] - mul(TS, fx::ln(base)));
			dampDelta[i] = mul(e, local[i]) - local[i];
		}
	}
	c.turn = rot(c, local);
	dampDelta *= dampExp;
	if (dampDelta.x) applyTurnRaw(c, c.fwd * -dampDelta.x, c.up + cw, cw);
	if (dampDelta.y) applyTurnRaw(c, c.up * -dampDelta.y, c.right + cw, cw);
	if (dampDelta.z) applyTurnRaw(c, c.right * -dampDelta.z, c.fwd + cw, cw);
	const q ms = fx::len(c.move);
	if (ms > F(1.5)) c.move *= div(F(1.5), ms);
	const q ts = fx::len(c.turn);
	if (ts > F(0.2)) c.turn *= div(F(0.2), ts);
}

// ---- CBoat: mod_Buoyancy + CVehicle::ProcessBoatControl (reduced) -----------------------
static void boatControl(Car& c) {
	Vehicle& v = c.v;
	const Hand& k = c.k;
	V cw = rot(c, c.com);
	const q W = mul(c.bmax.x - c.bmin.x, F(0.35));
	const q y0 = mul(c.bmin.y, F(0.75)), y1 = mul(c.bmax.y, F(0.75));
	const q zk = c.bmin.z + F(0.1);
	const q hull = fx::max(mul(c.bmax.z - c.bmin.z, F(0.6)), F(0.4));
	const V samples[6] = { { -W, y0, zk }, { W, y0, zk }, { -W, 0, zk }, { W, 0, zk }, { -W, y1, zk }, { W, y1, zk } };
	q total = 0;
	V turnPoint = { 0, 0, 0 };
	int wet = 0;
	for (const V& s : samples) {
		const V rel = rot(c, s);
		int32_t w[3];
		cuOf(c, rel, w);
		int32_t wz;
		if (!water::levelCu(w[0], w[1], wz)) continue;
		const q depth = fx::clamp(fromCu(wz - w[2]), 0, hull);
		if (depth <= 0) continue;
		const q b = mul(depth, k.buoy);
		applyForce(c, V{ 0, 0, b }, rel, cw);
		total += b;
		turnPoint += rel;
		wet++;
	}
	c.immersion = wet * ONE / 6;
	v.inWater = wet > 0;
	if (!wet) return;
	turnPoint = turnPoint * (ONE / wet);
	const q imm = c.immersion;
	// water resistance (tBoatHandlingData MoveRes / TurnRes, the keel against sideways drift)
	V lv = unrot(c, c.move);
	lv.x = mul(lv.x, ONE - mul(F(0.08), imm));
	lv.y = mul(lv.y, ONE - mul(ONE - k.boatMoveRes[1], imm));
	lv.y -= mul(mul(mul(lv.y, fx::abs(lv.y)), F(0.004)), imm);
	lv.z = mul(lv.z, ONE - mul(F(0.06), imm));
	c.move = rot(c, lv);
	V lt = unrot(c, c.turn);
	for (int i = 0; i < 3; i++) lt[i] = mul(lt[i], ONE - mul(ONE - k.boatTurnRes[i], imm * 2) - mul(F(0.02), imm));
	c.turn = rot(c, lt);
	const bool onWater = total > mul(F(0.0008), TS);
	// aquaplane: the hull lifts at speed
	if (onWater && c.up.z > 0 && c.gas > F(0.05)) {
		q f = mul(mul(mul(fx::len2(c.move), k.aqForce), TS), mul(total, F(0.5)));
		f = mul(f, c.gas);
		f = fx::min(f, mul(mul(TS, k.aqLimit), F(1.0 / 125.0)));
		const V u = c.up * f;
		applyMove(c, u);
		applyTurn(c, u, turnPoint - c.fwd * k.aqOffset, cw);
	}
	if (c.up.z <= -F(0.6)) return;
	// thrust / rudder at the propeller
	q traction = ONE;
	if (v.status == STATUS_PLAYER) {
		q tl = mul(dot(c.move, c.fwd), k.tractionBias);
		if (v.handbrake) tl = mul(tl, F(0.5));
		traction = fx::clamp(ONE - tl, 0, ONE);
	}
	const q steerChange = -mul(traction, c.steer);
	const q ss = fx::sin(steerChange), sc = fx::cos(steerChange);
	const V tpl = { 0, mul(c.bmin.y, k.thrustY), mul(c.bmin.z, k.thrustZ) };
	const V tp = rot(c, tpl);
	int32_t tw[3], twz;
	cuOf(c, tp, tw);
	if (!water::levelCu(tw[0], tw[1], twz)) return;
	const q tz = fromCu(tw[2]) - F(0.5), wz = fromCu(twz);
	if (tz >= wz) return;
	// (the reduced hull floats higher than mod_Buoyancy's: keep the prop's bite)
	const q thrustDepth = fx::clamp(mul(wz - tz, wz - tz), F(0.6), ONE);
	const q gasAbs = fx::abs(c.gas);
	const bool slowing = gasAbs < F(0.5);
	if (gasAbs >= F(0.01)) {
		V f = rot(c, V{ -ss, sc, -fx::abs(c.steer) }) * mul(mul(mul(thrustDepth, c.gas), F(40.0)), k.engineAccel);
		if (f.z > F(0.2)) f.z = mul(F(1.2) - f.z, F(1.2) - f.z) + F(0.2);
		f *= TS;
		applyMove(c, f);
		applyTurn(c, f, tp - c.up * k.thrustAppZ, cw);
		const q side = mul(-dot(f, c.right), fx::fromFloat(c.h.tractionMult));
		applyTurn(c, c.right * side, c.up, cw);
	}
	if (slowing) {
		// fTractionLoss is a force in the original (ApplyMoveForce divides it by the mass)
		q tl = mul(mul(dot(c.move, c.fwd), k.tractionLoss), fx::fromFloat(1.0f / c.h.mass));
		tl = fx::min(tl, div(F(0.01), k.kTurn));
		if (gasAbs > F(0.01)) tl = mul(mul(tl, F(0.55) - gasAbs), v.status == STATUS_PLAYER ? F(2.6) : F(5.0));
		if ((c.gas < 0 && tl > 0) || (c.gas > 0 && tl < 0)) tl = -tl;
		const V u = rot(c, V{ -ss, 0, 0 }) * mul(mul(tl, thrustDepth), TS);
		applyMove(c, u);
		applyTurn(c, u, tp, cw);
		const V t = c.right * mul(mul(div(TS, fx::max(traction, F(0.05))), mul(tl, ss)), -F(0.75));
		applyTurn(c, t, c.up, cw);
	}
}

// ---- step -------------------------------------------------------------------------------
static void trainStep(Car& c) {
	if (c.track < 0) return;
	Vehicle& v = c.v;
	const float maxV = fmaxf(c.h.maxVelocity, 0.3f);
	if (v.status == STATUS_PLAYER && v.engineOn) {
		const float in = c.pad.accel - c.pad.brake;
		c.trackSpeed += in * c.h.engineAccel * 3.0f * (50.0f / 60.0f);
	}
	c.trackSpeed *= v.status == STATUS_PLAYER && c.pad.accel > 0.0f ? 0.9995f : 0.997f;
	c.trackSpeed = fminf(fmaxf(c.trackSpeed, -maxV * 0.5f), maxV);
	const fx::wp old[3] = { c.pos[0], c.pos[1], c.pos[2] };
	c.trackDist += c.trackSpeed * (50.0f / 60.0f);
	trainPose(c);
	for (int k = 0; k < 3; k++) c.move[k] = (q)((c.pos[k] - old[k]) * 6 / 5);   // per 1/50 s step
	c.speedFwd = c.trackSpeed >= 0.0f ? fx::len(c.move) : -fx::len(c.move);
	c.gas = fx::fromFloat(c.pad.accel);
	c.brake = fx::fromFloat(c.pad.brake);
	c.numContact = 4;
	for (int i = 0; i < 4; i++) {
		c.wheelSpin[i] = div(c.speedFwd, c.wheelR[0] ? c.wheelR[0] : F(0.5));
		c.wheelRot[i] += mul(c.wheelSpin[i], TS);
	}
}

void physStep(Car& c) {
	Vehicle& v = c.v;
	c.impactJ = 0;
	if (v.cls == CLS_TRAIN) {
		if (v.status == STATUS_PLAYER || v.npcDriver) controlInputs(c);
		trainStep(c);
		publish(c);
		return;
	}
	if (v.status == STATUS_PLAYER || v.npcDriver) controlInputs(c);
	else controlsIdle(c);
	if (!v.engineOn) c.gas = 0;
	// broad phase once for the whole step: the box swept by the body this step
	{
		int32_t lo[3], hi[3];
		const q r = c.radius + fx::abs(c.move.x) + fx::abs(c.move.y) + fx::abs(c.move.z) + F(1.0);
		for (int k = 0; k < 3; k++) {
			lo[k] = (int32_t)((c.pos[k] - r) >> 17);
			hi[k] = (int32_t)((c.pos[k] + r) >> 17);
		}
		col::focus(lo, hi);
	}
	// CPhysical::ApplyGravity
	c.move.z -= mul(GRAVITY, TS);
	const bool wheels = v.cls != CLS_BOAT;
	V cw = rot(c, c.com);
	V cp[4], cs[4];
	c.numContact = 0;
	c.driveOnGround = 0;
	c.speedFwd = dot(c.move, c.fwd);
	if (wheels) {
		const u32 tq = timing::ticks();
		castWheels(c);
		g_colTicks += timing::ticks() - tq;
		processSuspension(c, cw, cp);
		// contact points / speeds for the tyres
		for (int i = 0; i < 4; i++) {
			if (c.susp[i] >= ONE) cp[i] = rot(c, c.lineEnd[i]);
			cs[i] = speedAt(c, cp[i], cw);
		}
		c.speedFwd = dot(c.move, c.fwd);
		const q brake = mul(mul(c.k.brakeDecel, c.brake), TS);
		const char dt = c.k.drive;
		for (int i = 0; i < 4; i++) {
			if (c.susp[i] >= ONE) {
				c.wheelCounts[i] = fx::max(c.wheelCounts[i] - TS, 0);
				if (c.wheelCounts[i] <= 0) continue;
			} else {
				c.wheelCounts[i] = fx::fromInt(4);
			}
			c.numContact++;
			const bool front = i == vehmdl::WHEEL_FL || i == vehmdl::WHEEL_FR;
			if (dt == '4' || (dt == 'F' && front) || (dt == 'R' && !front)) c.driveOnGround++;
		}
		// aircraft wheels roll free: thrust comes from the props / rotor
		const bool driven = v.cls == CLS_AUTOMOBILE || v.cls == CLS_BIKE;
		const q accel = v.engineOn && driven ? driveAccel(c, c.gas, c.speedFwd) : 0;
		const q traction = c.k.traction;
		q steerAngle = ONE;
		if (c.speedFwd > F(0.01) && v.status == STATUS_PLAYER && v.cls != CLS_BIKE &&
		    (c.wheelCounts[vehmdl::WHEEL_FL] > 0 || c.wheelCounts[vehmdl::WHEEL_RL] > 0)) {
			const q sr = dot(c.move, c.right);
			q sa = fx::min(div(mul(F(4.5 * 16.0), traction), mul(c.speedFwd, c.speedFwd)), ONE);
			sa = div(fx::asin(sa), c.k.steerLock);
			if ((c.steer < 0 && sr > F(0.05)) || (c.steer > 0 && sr < -F(0.05)) || v.handbrake || sa > ONE) sa = ONE;
			steerAngle = sa;
		}
		const q steer = mul(c.steer, steerAngle);
		wheelPair(c, vehmdl::WHEEL_FL, vehmdl::WHEEL_FR, steer, true, cs, cp, cw, traction, accel, brake, true);
		wheelPair(c, vehmdl::WHEEL_RL, vehmdl::WHEEL_RR, 0, false, cs, cp, cw, traction, accel, brake, false);
		// wheel spin (drawing + audio) and the skidmarks
		v.skid = 0.0f;
		const q sideSpeed = fx::abs(dot(c.move, c.right));
		for (int i = 0; i < 4; i++) {
			const bool front = i == vehmdl::WHEEL_FL || i == vehmdl::WHEEL_FR;
			const q r = c.wheelR[front ? 0 : 1] > F(0.05) ? c.wheelR[front ? 0 : 1] : F(0.35);
			const bool driveW = driven && (dt == '4' || (dt == 'F' && front) || (dt == 'R' && !front));
			if (c.wheelState[i] == WHEEL_FIXED) c.wheelSpin[i] = 0;
			else if (c.wheelState[i] == WHEEL_SPINNING || (c.wheelCounts[i] <= 0 && driveW && c.gas != 0))
				c.wheelSpin[i] += mul(mul(c.gas, F(0.6)) - c.wheelSpin[i], F(0.1));
			else if (c.wheelCounts[i] > 0) c.wheelSpin[i] = div(dot(cs[i], c.fwd), r);
			else c.wheelSpin[i] = mul(c.wheelSpin[i], F(0.95));
			c.wheelRot[i] += mul(c.wheelSpin[i], TS);
			if (c.wheelRot[i] > F(6.2831853)) c.wheelRot[i] -= F(6.2831853);
			if (c.wheelRot[i] < -F(6.2831853)) c.wheelRot[i] += F(6.2831853);
			const bool skidding = c.susp[i] < ONE && c.wheelState[i] != WHEEL_NORMAL;
			const bool audible = skidding &&
			    ((c.wheelState[i] == WHEEL_FIXED && (c.brake > F(0.3) || v.handbrake)) ||
			     (c.slip[i] > F(0.2) && sideSpeed > F(0.08)) ||
			     (c.wheelState[i] == WHEEL_SPINNING && fx::abs(c.speedFwd) < F(0.03) && c.gas > F(0.5)));
			if (audible && (v.status == STATUS_PLAYER || fx::abs(c.speedFwd) > F(0.03)))
				v.skid += fminf(fx::toFloat(c.slip[i]) + 0.3f, 1.5f);
			// CVehicle::AddSingleWheelParticles -> CSkidmarks::RegisterOne
			if (audible && fx::abs(c.speedFwd) > F(0.03)) {
				const int vid = (int)(&c - g_cars);
				const float x = floatOf(c.pos[0] + c.contactRel[i].x), y = floatOf(c.pos[1] + c.contactRel[i].y),
				            z = floatOf(c.pos[2] + c.contactRel[i].z);
				const uint8_t sk = vehdata::skidmark(c.surf[i]);
				skid::registerOne((uint32_t)(vid * 4 + i), x, y, z, fx::toFloat(c.fwd.x), fx::toFloat(c.fwd.y),
				                  v.cls == CLS_BIKE ? 0.18f : fx::toFloat(r) * 0.6f, (skid::Type)(sk < 3 ? sk : 0));
			}
		}
	}
	if (v.cls == CLS_BOAT) {
		boatControl(c);
		c.speedFwd = dot(c.move, c.fwd);
		for (int i = 0; i < 4; i++) c.susp[i] = ONE;
	}
	// CHeli rotor / CPlane propeller
	if (v.cls == CLS_HELI || v.cls == CLS_PLANE || (v.cls == CLS_BOAT && c.k.fly)) {   // + the Skimmer
		const bool heli = v.cls == CLS_HELI;
		const q full = heli ? HELI_ROTOR_FULL : F(0.3);
		const q want = v.engineOn ? (heli ? full : mul(full, F(0.6) + mul(F(0.4), fx::clamp(c.gas, 0, ONE)))) : 0;
		c.rotorSpeed += fx::clamp(want - c.rotorSpeed, -F(0.0008), F(heli ? 0.0012 : 0.004));
		c.rotorAngle += mul(c.rotorSpeed, F(heli ? 6.0 : 9.0));
		if (c.rotorAngle > F(6.2831853)) c.rotorAngle -= F(6.2831853);
		v.rotor = fx::toFloat(div(c.rotorSpeed, full));
		if (c.k.fly && v.status != STATUS_WRECKED && (heli ? c.rotorSpeed >= mul(full, F(0.95)) : v.engineOn))
			flyingControl(c, heli);
	}
	// CPhysical::ApplyAirResistance (boats: the water handles it)
	if (v.cls != CLS_BOAT || !v.inWater) {
		const q sp = mul(fx::len(c.move), c.k.airRes);
		c.move *= ONE - mul(sp, TS);
	}
	c.turn *= F(0.99);
	if (v.cls == CLS_BIKE) bikeStabilise(c);
	// ApplyMoveSpeed + ApplyTurnSpeed
	for (int k = 0; k < 3; k++) c.pos[k] += mul(c.move[k], TS);
	{
		const V w = c.turn * TS;
		c.right += cross(w, c.right);
		c.fwd += cross(w, c.fwd);
		c.up += cross(w, c.up);
		const V nc = cross(w, -cw);
		for (int k = 0; k < 3; k++) c.pos[k] += nc[k];
		reorthogonalise(c);
	}
	{
		const u32 tq = timing::ticks();
		collideWorld(c);
		g_colTicks += timing::ticks() - tq;
	}
	col::unfocus();
	// cars in deep water drown (no buoyancy): they sink, slow down and stall
	if (v.cls != CLS_BOAT) {
		int32_t pc[3], wz;
		cuOf(c, rot(c, c.com), pc);
		v.inWater = water::levelCu(pc[0], pc[1], wz) && wz > pc[2] + 64;
		if (v.inWater) {
			c.move *= F(0.95);
			c.turn *= F(0.95);
			c.move.z += mul(GRAVITY, F(0.6));
			if (v.cls != CLS_HELI && v.cls != CLS_PLANE) v.engineOn = false;
		}
	}
	// fake physics: a resting vehicle without a driver stops being simulated
	const int64_t vs = fx::dot48(c.move, c.move), ws = fx::dot48(c.turn, c.turn);
	const int64_t vsLim = (int64_t)F(0.005) * F(0.005), wsLim = (int64_t)F(0.0015) * F(0.0015);
	const bool resting = v.cls == CLS_BOAT ? v.inWater : c.numContact >= 3;
	if (v.status != STATUS_PLAYER && !v.npcDriver && vs < vsLim && ws < wsLim && resting && c.impactJ <= 0 && c.rotorSpeed <= 0) {
		if (++c.fakePhysics > 10 && v.cls != CLS_BOAT) {
			v.asleep = true;
			c.move = c.turn = V{ 0, 0, 0 };
		}
	} else {
		c.fakePhysics = 0;
	}
	publish(c);
	// upside down: CVehicle damage drain, then the fire
	if (v.up[2] < 0.3f && vs < (int64_t)F(0.02) * F(0.02) && v.status != STATUS_WRECKED && v.cls != CLS_BIKE)
		v.health -= 4.0f * (50.0f / 60.0f);
	vehicleDamage(c);
}

}
