#include "game/collision.h"

#include <math.h>
#include <nds.h>
#include <string.h>

#include "assets/ds_formats.h"
#include "game/debugopts.h"
#include "game/player.h"
#include "game/world.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/timing.h"
#include "render/overlay.h"

namespace col {

using namespace mem::budget;

// cu = collision units, 1/128 m (the record vertices); positions here are
// SA world or model space in cu, int32.
static constexpr int32_t GATHER_M = 14;      // near list: volumes within this of the player
static constexpr int32_t STREAM_M = 48;      // records streamed within this
static constexpr int MAX_WANTS = 8;
static constexpr u32 UPDATE_US = 1500;       // streaming per in-game frame
static constexpr u32 PRELOAD_US = 8000;      // per loading-screen slice
static constexpr u32 PRELOAD_MAX_VBLANKS = 300;
static constexpr int32_t WALL_NZ = 2458;     // |n.z| below 0.6 (4.12) = wall for pushSphere
static constexpr int DBG_SLOT = 12;          // matrix slot with the view (world 13, CJ 14..30)
static constexpr uint32_t GATHER_FRAMES = 8; // near list rebuild period

struct Ref { ds::WorldCol* data; uint32_t lastUse; };

// A near instance: its transform (SA rows: world = pos + sum_k l_k * r[k])
// copied out of the sector window so it survives world::update.
struct Near {
	int32_t pos[3];           // SA, 20.12
	int16_t r[9];             // rows right / forward / up in SA axes, 4.12
	uint16_t model;
	const ds::WorldCol* c;
	int32_t dist;             // player -> volume sphere, 1/16 m
	int32_t lo[3], hi[3];     // world AABB of the volumes, SA cu (first test of every query)
};

static inline bool aabbHit(const Near& n, const int32_t lo[3], const int32_t hi[3]) {
	return lo[0] <= n.hi[0] && hi[0] >= n.lo[0] && lo[1] <= n.hi[1] && hi[1] >= n.lo[1] && lo[2] <= n.hi[2] &&
	       hi[2] >= n.lo[2];
}

struct Want { uint16_t model; int32_t key; };

struct View {
	const ds::WorldColSphere* sph;
	const ds::WorldColBox* box;
	const ds::WorldColNode* node;
	const ds::WorldColFace* face;
	const int16_t* vert;
};

static ds::WorldColIndex* s_index;
static Ref* s_refs;
static int s_numModels;
static Near s_near[COL_NEAR_MAX];
static int s_numNear;
static Want s_want[MAX_WANTS];
static int s_numWant;
static uint32_t s_frame, s_bytes;
static int s_step;
static u32 s_preloadT0;
static u32 s_queryUs, s_pushUs, s_faceTests;
static uint32_t s_gatherFrame;
static float s_gatherX, s_gatherY, s_gatherZ;
static bool s_nearDirty = true;
static Stats s_stats;
static world::SectorView s_sv[WORLD_SECTOR_SLOTS];

const Stats& stats() { return s_stats; }
bool available() { return s_index != nullptr; }
bool pending() { return s_stats.missing != 0 || world::sectorsPending(); }

static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }
// 16.16 fraction num / den for 0 <= num <= den, on the hardware divider
// (libgcc's 64-bit division costs ~1000 cycles on the ARM9)
static inline int32_t frac16(int64_t num, int64_t den) {
	while (den > 0x3FFFFFFF) {
		den >>= 1;
		num >>= 1;
	}
	return den > 0 ? div64(num << 16, (s32)den) : 0;
}

// (num << 16) / den clamped to +-2^30 (slab entry/exit parameters)
static inline int32_t slab16(int32_t num, int32_t den) {
	const int64_t n = (int64_t)num << 16;
	const int64_t lim = (int64_t)(den < 0 ? -den : den) << 30;
	if (n >= lim || -n >= lim) return (n < 0) != (den < 0) ? -(1 << 30) : (1 << 30);
	return div64(n, den);
}

// soft float: one multiply + convert (truncation, < 1 cu off)
static inline int32_t cu(float m) { return (int32_t)(m * 128.0f); }

static View view(const ds::WorldCol* c) {
	const uint8_t* p = (const uint8_t*)(c + 1);
	View v;
	v.sph = (const ds::WorldColSphere*)p;
	p += c->numSpheres * sizeof(ds::WorldColSphere);
	v.box = (const ds::WorldColBox*)p;
	p += c->numBoxes * sizeof(ds::WorldColBox);
	v.node = (const ds::WorldColNode*)p;
	p += c->numNodes * sizeof(ds::WorldColNode);
	v.face = (const ds::WorldColFace*)p;
	p += c->numFaces * sizeof(ds::WorldColFace);
	v.vert = (const int16_t*)p;
	return v;
}

// SA world cu -> model space cu
static inline void toLocal(const Near& n, const int32_t w[3], int32_t l[3]) {
	const int32_t d0 = w[0] - (n.pos[0] >> 5), d1 = w[1] - (n.pos[1] >> 5), d2 = w[2] - (n.pos[2] >> 5);
	for (int k = 0; k < 3; k++) l[k] = (d0 * n.r[k * 3] + d1 * n.r[k * 3 + 1] + d2 * n.r[k * 3 + 2]) >> 12;
}

// model-space vector -> SA world (no translation)
static inline void toWorldDir(const Near& n, const int32_t l[3], int32_t w[3]) {
	for (int j = 0; j < 3; j++) w[j] = (l[0] * n.r[j] + l[1] * n.r[3 + j] + l[2] * n.r[6 + j]) >> 12;
}

// world z of a model-space unit normal (4.12)
static inline int32_t worldNz(const Near& n, const int16_t nl[3]) {
	return (nl[0] * n.r[2] + nl[1] * n.r[5] + nl[2] * n.r[8]) >> 12;
}

static inline bool overlaps(const int16_t mn[3], const int16_t mx[3], const int32_t lo[3], const int32_t hi[3]) {
	return lo[0] <= mx[0] && hi[0] >= mn[0] && lo[1] <= mx[1] && hi[1] >= mn[1] && lo[2] <= mx[2] && hi[2] >= mn[2];
}

// Triangle bbox vs [lo, hi]: rejects most faces of an overlapping BVH leaf
// before any multiply.
static inline bool faceOverlaps(const int16_t* const v[3], const int32_t lo[3], const int32_t hi[3]) {
	s_faceTests++;
	for (int k = 0; k < 3; k++) {
		const int32_t a = v[0][k], b = v[1][k], c = v[2][k];
		if ((a < lo[k] && b < lo[k] && c < lo[k]) || (a > hi[k] && b > hi[k] && c > hi[k])) return false;
	}
	return true;
}

// Faces whose bbox overlaps [lo, hi], down the face BVH.
template <class F>
static inline void forFaces(const ds::WorldCol* c, const View& v, const int32_t lo[3], const int32_t hi[3], F&& fn) {
	if (!c->numNodes) return;
	uint16_t stack[48];
	int sp = 0;
	stack[sp++] = 0;
	while (sp) {
		const ds::WorldColNode& nd = v.node[stack[--sp]];
		if (!overlaps(nd.min, nd.max, lo, hi)) continue;
		if (nd.b & 0x8000) {
			for (int fi = nd.a, end = nd.a + (nd.b & 0x7FFF); fi < end; fi++) {
				const ds::WorldColFace& f = v.face[fi];
				const int16_t* const tri[3] = { v.vert + f.a * 3, v.vert + f.b * 3, v.vert + f.c * 3 };
				if (faceOverlaps(tri, lo, hi)) fn(f, tri);
			}
		} else if (sp + 2 <= (int)(sizeof stack / sizeof stack[0])) {
			stack[sp++] = nd.b;
			stack[sp++] = nd.a;
		}
	}
}

// Point h on the plane of CCW triangle (v0, v1, v2) with normal n: inside
// every edge, with ~1 cu of slack so shared edges never leak.
static bool inside(const int16_t* const v[3], const int16_t n[3], const int32_t h[3]) {
	for (int e = 0; e < 3; e++) {
		const int16_t* p = v[e];
		const int16_t* q = v[e == 2 ? 0 : e + 1];
		const int32_t ex = q[0] - p[0], ey = q[1] - p[1], ez = q[2] - p[2];
		const int32_t hx = h[0] - p[0], hy = h[1] - p[1], hz = h[2] - p[2];
		const int64_t cx = (int64_t)ey * hz - (int64_t)ez * hy;
		const int64_t cy = (int64_t)ez * hx - (int64_t)ex * hz;
		const int64_t cz = (int64_t)ex * hy - (int64_t)ey * hx;
		int32_t len = iabs(ex);
		if (iabs(ey) > len) len = iabs(ey);
		if (iabs(ez) > len) len = iabs(ez);
		if (cx * n[0] + cy * n[1] + cz * n[2] < -(int64_t)len * 4096) return false;
	}
	return true;
}

// ----------------------------------------------------------------------------
// Segment cast (CCollision::ProcessLineOfSight: ProcessLineTriangle /
// ProcessLineBox / ProcessLineSphere), model space. best = 16.16 fraction.
// ----------------------------------------------------------------------------
struct LocalHit { int32_t t; int16_t n[3]; uint8_t surface, light; };

static void castLocal(const ds::WorldCol* c, const int32_t O[3], const int32_t E[3], LocalHit& best) {
	int32_t lo[3], hi[3];
	for (int k = 0; k < 3; k++) {
		lo[k] = O[k] < E[k] ? O[k] : E[k];
		hi[k] = O[k] < E[k] ? E[k] : O[k];
	}
	if (!overlaps(c->bmin, c->bmax, lo, hi)) return;
	const View v = view(c);
	const int32_t D[3] = { E[0] - O[0], E[1] - O[1], E[2] - O[2] };
	forFaces(c, v, lo, hi, [&](const ds::WorldColFace& f, const int16_t* const tri[3]) {
		const int16_t* a = tri[0];
		const int64_t s0 = (int64_t)(O[0] - a[0]) * f.n[0] + (int64_t)(O[1] - a[1]) * f.n[1] + (int64_t)(O[2] - a[2]) * f.n[2];
		if (s0 <= 0) return;
		const int64_t s1 = (int64_t)(E[0] - a[0]) * f.n[0] + (int64_t)(E[1] - a[1]) * f.n[1] + (int64_t)(E[2] - a[2]) * f.n[2];
		if (s1 > 0) return;
		const int32_t t = frac16(s0, s0 - s1);
		if (t >= best.t) return;
		const int32_t h[3] = { O[0] + (int32_t)(((int64_t)D[0] * t) >> 16), O[1] + (int32_t)(((int64_t)D[1] * t) >> 16),
		                       O[2] + (int32_t)(((int64_t)D[2] * t) >> 16) };
		if (!inside(tri, f.n, h)) return;
		best = { t, { f.n[0], f.n[1], f.n[2] }, f.surface, f.light };
	});
	for (int bi = 0; bi < c->numBoxes; bi++) {
		const ds::WorldColBox& b = v.box[bi];
		if (!overlaps(b.min, b.max, lo, hi)) continue;
		int32_t tin = INT32_MIN, tout = 1 << 16;
		int axis = -1;
		bool miss = false;
		for (int k = 0; k < 3 && !miss; k++) {
			if (!D[k]) {
				miss = O[k] < b.min[k] || O[k] > b.max[k];
				continue;
			}
			int32_t t0 = slab16(b.min[k] - O[k], D[k]);
			int32_t t1 = slab16(b.max[k] - O[k], D[k]);
			if (t0 > t1) { const int32_t s = t0; t0 = t1; t1 = s; }
			if (t0 > tin) { tin = t0; axis = k; }
			if (t1 < tout) tout = t1;
		}
		// starting inside the box is not a hit (single sided)
		if (miss || axis < 0 || tin > tout || tin < 0 || tin >= best.t) continue;
		LocalHit h = { tin, { 0, 0, 0 }, b.surface, b.light };
		h.n[axis] = D[axis] > 0 ? -4096 : 4096;
		best = h;
	}
	for (int si = 0; si < c->numSpheres; si++) {
		const ds::WorldColSphere& s = v.sph[si];
		if (lo[0] > s.c[0] + s.r || hi[0] < s.c[0] - s.r || lo[1] > s.c[1] + s.r || hi[1] < s.c[1] - s.r ||
		    lo[2] > s.c[2] + s.r || hi[2] < s.c[2] - s.r)
			continue;
		const float ox = (float)(O[0] - s.c[0]), oy = (float)(O[1] - s.c[1]), oz = (float)(O[2] - s.c[2]);
		const float dx = (float)D[0], dy = (float)D[1], dz = (float)D[2], r = (float)s.r;
		const float qa = dx * dx + dy * dy + dz * dz, qb = 2.0f * (dx * ox + dy * oy + dz * oz);
		const float qc = ox * ox + oy * oy + oz * oz - r * r;
		const float disc = qb * qb - 4.0f * qa * qc;
		if (qc <= 0.0f || qa <= 0.0f || disc < 0.0f) continue;
		const float t = (-qb - sqrtf(disc)) / (2.0f * qa);
		if (t < 0.0f || t >= 1.0f || (int32_t)(t * 65536.0f) >= best.t) continue;
		const float inv = 4096.0f / r;
		best = { (int32_t)(t * 65536.0f),
		         { (int16_t)((ox + dx * t) * inv), (int16_t)((oy + dy * t) * inv), (int16_t)((oz + dz * t) * inv) },
		         s.surface, s.light };
	}
}

bool castSegment(const float a[3], const float b[3], Hit& hit) {
	const u32 t0 = timing::ticks();
	const int32_t wa[3] = { cu(a[0]), cu(a[1]), cu(a[2]) }, wb[3] = { cu(b[0]), cu(b[1]), cu(b[2]) };
	LocalHit best = { 1 << 16, { 0, 0, 0 }, 0, 0 };
	const Near* bestN = nullptr;
	int32_t lo[3], hi[3];
	for (int k = 0; k < 3; k++) {
		lo[k] = wa[k] < wb[k] ? wa[k] : wb[k];
		hi[k] = wa[k] < wb[k] ? wb[k] : wa[k];
	}
	for (int i = 0; i < s_numNear; i++) {
		const Near& n = s_near[i];
		if (!aabbHit(n, lo, hi)) continue;
		int32_t O[3], E[3];
		toLocal(n, wa, O);
		toLocal(n, wb, E);
		const int32_t before = best.t;
		castLocal(n.c, O, E, best);
		if (best.t != before) bestN = &n;
	}
	s_queryUs += timing::usSince(t0);
	if (!bestN) return false;
	const float t = (float)best.t * (1.0f / 65536.0f);
	hit.t = t;
	hit.x = a[0] + (b[0] - a[0]) * t;
	hit.y = a[1] + (b[1] - a[1]) * t;
	hit.z = a[2] + (b[2] - a[2]) * t;
	hit.nz = (float)worldNz(*bestN, best.n) * (1.0f / 4096.0f);
	hit.surface = best.surface;
	hit.light = best.light;
	return true;
}

bool groundAt(float x, float y, float zTop, float zBottom, Hit& hit) {
	const float a[3] = { x, y, zTop }, b[3] = { x, y, zBottom };
	return castSegment(a, b, hit);
}

// ----------------------------------------------------------------------------
// Sphere push-out (CCollision::ProcessSphereTriangle / SphereBox / SphereSphere
// response, horizontal only: the feet are handled by groundAt)
// ----------------------------------------------------------------------------
static void closestOnSegment(const int16_t* p, const int16_t* q, const int32_t c[3], int32_t out[3]) {
	const int32_t e[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
	const int64_t num = (int64_t)(c[0] - p[0]) * e[0] + (int64_t)(c[1] - p[1]) * e[1] + (int64_t)(c[2] - p[2]) * e[2];
	const int64_t den = (int64_t)e[0] * e[0] + (int64_t)e[1] * e[1] + (int64_t)e[2] * e[2];
	if (num <= 0 || den <= 0) {
		for (int k = 0; k < 3; k++) out[k] = p[k];
	} else if (num >= den) {
		for (int k = 0; k < 3; k++) out[k] = q[k];
	} else {
		const int32_t t = frac16(num, den);
		for (int k = 0; k < 3; k++) out[k] = p[k] + (int32_t)(((int64_t)e[k] * t) >> 16);
	}
}

bool pushSphere(float& x, float& y, float z, float r) {
	if (!s_numNear) return false;
	const u32 t0 = timing::ticks();
	int32_t w[3] = { cu(x), cu(y), cu(z) };
	const int32_t rc = (int32_t)(r * 128.0f);
	bool touched = false;
	for (int i = 0; i < s_numNear; i++) {
		const Near& n = s_near[i];
		{
			const int32_t wl[3] = { w[0] - rc, w[1] - rc, w[2] - rc }, wh[3] = { w[0] + rc, w[1] + rc, w[2] + rc };
			if (!aabbHit(n, wl, wh)) continue;
		}
		const ds::WorldCol* c = n.c;
		int32_t C[3];
		toLocal(n, w, C);
		const int32_t lo[3] = { C[0] - rc, C[1] - rc, C[2] - rc }, hi[3] = { C[0] + rc, C[1] + rc, C[2] + rc };
		if (!overlaps(c->bmin, c->bmax, lo, hi)) continue;
		const View v = view(c);
		// moves the sphere along `dir` (model space, any length) by `amount` cu,
		// horizontally in the world
		auto push = [&](const int32_t dir[3], int32_t len, int32_t amount) {
			if (len <= 0 || amount <= 0) return;
			// |dir| <= 4096 and amount < 2^15 here: fits the 32-bit hardware divide
			const int32_t pl[3] = { div32(dir[0] * amount, len), div32(dir[1] * amount, len), div32(dir[2] * amount, len) };
			int32_t pw[3];
			toWorldDir(n, pl, pw);
			w[0] += pw[0];
			w[1] += pw[1];
			toLocal(n, w, C);
			touched = true;
		};
		auto pushFrom = [&](const int32_t q[3], int32_t reach) {
			const int32_t d[3] = { C[0] - q[0], C[1] - q[1], C[2] - q[2] };
			const int64_t d2 = (int64_t)d[0] * d[0] + (int64_t)d[1] * d[1] + (int64_t)d[2] * d[2];
			if (d2 >= (int64_t)reach * reach || d2 == 0) return;
			const int32_t len = (int32_t)sqrt32((int)d2);
			push(d, len, reach - len);
		};
		forFaces(c, v, lo, hi, [&](const ds::WorldColFace& f, const int16_t* const tri[3]) {
			const int32_t nz = worldNz(n, f.n);
			if (nz > WALL_NZ || nz < -WALL_NZ) return;
			const int16_t* a = tri[0];
			const int32_t s = (int32_t)(((int64_t)(C[0] - a[0]) * f.n[0] + (int64_t)(C[1] - a[1]) * f.n[1] +
			                             (int64_t)(C[2] - a[2]) * f.n[2]) >> 12);
			if (s < 0 || s >= rc) return;
			const int32_t q[3] = { C[0] - ((f.n[0] * s) >> 12), C[1] - ((f.n[1] * s) >> 12), C[2] - ((f.n[2] * s) >> 12) };
			if (inside(tri, f.n, q)) {
				const int32_t nd[3] = { f.n[0], f.n[1], f.n[2] };
				push(nd, 4096, rc - s);
				return;
			}
			int32_t best[3], e[3];
			int64_t bd = INT64_MAX;
			for (int k = 0; k < 3; k++) {
				closestOnSegment(tri[k], tri[k == 2 ? 0 : k + 1], C, e);
				const int64_t d2 = (int64_t)(C[0] - e[0]) * (C[0] - e[0]) + (int64_t)(C[1] - e[1]) * (C[1] - e[1]) +
				                   (int64_t)(C[2] - e[2]) * (C[2] - e[2]);
				if (d2 < bd) { bd = d2; memcpy(best, e, sizeof best); }
			}
			pushFrom(best, rc);
		});
		for (int bi = 0; bi < c->numBoxes; bi++) {
			const ds::WorldColBox& b = v.box[bi];
			if (!overlaps(b.min, b.max, lo, hi)) continue;
			int32_t q[3];
			bool in = true;
			for (int k = 0; k < 3; k++) {
				q[k] = C[k] < b.min[k] ? b.min[k] : C[k] > b.max[k] ? b.max[k] : C[k];
				in &= q[k] == C[k];
			}
			if (!in) {
				pushFrom(q, rc);
				continue;
			}
			// centre inside: out through the nearest side face
			int axis = 0, sign = 1;
			int32_t pen = INT32_MAX;
			for (int k = 0; k < 3; k++) {
				if (C[k] - b.min[k] < pen) { pen = C[k] - b.min[k]; axis = k; sign = -1; }
				if (b.max[k] - C[k] < pen) { pen = b.max[k] - C[k]; axis = k; sign = 1; }
			}
			int32_t d[3] = { 0, 0, 0 };
			d[axis] = sign;
			push(d, 1, pen + rc);
		}
		for (int si = 0; si < c->numSpheres; si++) {
			const ds::WorldColSphere& s = v.sph[si];
			if (lo[0] > s.c[0] + s.r || hi[0] < s.c[0] - s.r || lo[1] > s.c[1] + s.r || hi[1] < s.c[1] - s.r ||
			    lo[2] > s.c[2] + s.r || hi[2] < s.c[2] - s.r)
				continue;
			const int32_t q[3] = { s.c[0], s.c[1], s.c[2] };
			pushFrom(q, rc + s.r);
		}
	}
	x = (float)w[0] * (1.0f / 128.0f);
	y = (float)w[1] * (1.0f / 128.0f);
	const u32 us = timing::usSince(t0);
	s_queryUs += us;
	s_pushUs += us;
	return touched;
}

// ----------------------------------------------------------------------------
// Streaming (CColStore, per model instead of per area)
// ----------------------------------------------------------------------------
static void addWant(uint16_t model, int32_t key) {
	for (int i = 0; i < s_numWant; i++)
		if (s_want[i].model == model) {
			if (key < s_want[i].key) s_want[i].key = key;
			return;
		}
	int pos = s_numWant;
	if (pos == MAX_WANTS) {
		if (key >= s_want[MAX_WANTS - 1].key) return;
		pos = MAX_WANTS - 1;
	} else {
		s_numWant++;
	}
	while (pos > 0 && s_want[pos - 1].key > key) {
		s_want[pos] = s_want[pos - 1];
		pos--;
	}
	s_want[pos] = { model, key };
}

static bool evictOne() {
	int best = -1;
	for (int i = 0; i < s_numModels; i++)
		if (s_refs[i].data && s_refs[i].lastUse + 2 * GATHER_FRAMES < s_frame &&
		    (best < 0 || s_refs[i].lastUse < s_refs[best].lastUse))
			best = i;
	if (best < 0) return false;
	s_nearDirty = true;
	s_bytes -= s_index[best].size;
	mem::free(s_refs[best].data);
	s_refs[best].data = nullptr;
	s_stats.models--;
	return true;
}

static bool load(uint16_t model) {
	const ds::WorldColIndex& ix = s_index[model];
	if (s_refs[model].data || !ix.offset) return true;
	while (s_bytes + ix.size > COL_CACHE_BYTES)
		if (!evictOne()) return false;
	ds::WorldCol* c = (ds::WorldCol*)mem::alloc(ix.size, mem::TAG_COL);
	if (!c) {
		hud::log("\x1b[33mcol: no RAM for model %u (%luB)\x1b[39m", model, (unsigned long)ix.size);
		return false;
	}
	if (!world::readAt(ix.offset, c, ix.size)) {
		mem::free(c);
		s_index[model].offset = 0;   // broken record: never ask again
		return true;
	}
	s_refs[model] = { c, s_frame };
	s_nearDirty = true;
	s_bytes += ix.size;
	s_stats.models++;
	s_stats.loads++;
	return true;
}

static void gather(float x, float y, float z) {
	const int32_t p[3] = { (int32_t)(x * 16.0f), (int32_t)(y * 16.0f), (int32_t)(z * 16.0f) };
	s_numNear = 0;
	s_numWant = 0;
	uint16_t missing = 0;
	const int ns = world::sectors(s_sv, WORLD_SECTOR_SLOTS);
	for (int si = 0; si < ns; si++) {
		const world::SectorView& sv = s_sv[si];
		const int32_t dx = sv.c[0] - p[0], dy = sv.c[1] - p[1], dz = sv.c[2] - p[2];
		if (iabs(dx) > 0x4000 || iabs(dy) > 0x4000 || iabs(dz) > 0x4000) continue;
		// sector reach is from the render radii: collision can stick out a bit
		if ((int32_t)sqrt32(dx * dx + dy * dy + dz * dz) - sv.reach > (STREAM_M + 32) * 16) continue;
		for (int i = 0; i < sv.count; i++) {
			const world::Instance& in = sv.inst[i];
			const ds::WorldColIndex& ix = s_index[in.model];
			if (!ix.offset) continue;
			const int32_t ex = (in.pos[0] >> 8) - p[0], ey = (in.pos[1] >> 8) - p[1], ez = (in.pos[2] >> 8) - p[2];
			const int32_t reach = STREAM_M * 16 + ix.radius;
			if (iabs(ex) > reach || iabs(ey) > reach || iabs(ez) > reach || reach > 0x6000) continue;
			const int32_t d = (int32_t)sqrt32(ex * ex + ey * ey + ez * ez) - ix.radius;
			if (d >= STREAM_M * 16) continue;
			Ref& rf = s_refs[in.model];
			const bool close = d < GATHER_M * 16;
			if (!rf.data) {
				addWant(in.model, d);
				missing += close;
				continue;
			}
			rf.lastUse = s_frame;
			if (!close || s_numNear >= COL_NEAR_MAX) continue;
			Near& n = s_near[s_numNear++];
			memcpy(n.pos, in.pos, sizeof n.pos);
			for (int k = 0; k < 3; k++) {     // GL rows (x, z, -y) -> SA rows
				n.r[k * 3] = in.m[k * 3];
				n.r[k * 3 + 1] = (int16_t)-in.m[k * 3 + 2];
				n.r[k * 3 + 2] = in.m[k * 3 + 1];
			}
			n.model = in.model;
			n.c = rf.data;
			n.dist = d;
			// world AABB of the model-space bbox: centre + |R|^T * half extents
			const ds::WorldCol* c = rf.data;
			int32_t cl[3], el[3], cw[3];
			for (int k = 0; k < 3; k++) {
				cl[k] = (c->bmin[k] + c->bmax[k]) / 2;
				el[k] = (c->bmax[k] - c->bmin[k]) / 2 + 2;
			}
			toWorldDir(n, cl, cw);
			for (int j = 0; j < 3; j++) {
				const int32_t e = (el[0] * iabs(n.r[j]) + el[1] * iabs(n.r[3 + j]) + el[2] * iabs(n.r[6 + j])) / 4096 + 1;
				n.lo[j] = (n.pos[j] >> 5) + cw[j] - e;
				n.hi[j] = (n.pos[j] >> 5) + cw[j] + e;
			}
		}
	}
	s_stats.missing = missing;
	s_stats.wants = (uint16_t)s_numWant;
}

static void stream(u32 budgetUs) {
	const u32 t0 = timing::ticks();
	for (int i = 0; i < s_numWant; i++) {
		// anything the player can touch right now loads even past the budget
		if (timing::usSince(t0) >= budgetUs && s_want[i].key >= GATHER_M * 16) break;
		if (!load(s_want[i].model)) break;
	}
	s_stats.wants = (uint16_t)s_numWant;
}

static void step(float x, float y, float z, u32 budgetUs) {
	const u32 t0 = timing::ticks();
	s_frame++;
	s_stats.queryUs = (uint16_t)(s_queryUs > 0xFFFF ? 0xFFFF : s_queryUs);
	s_stats.pushUs = (uint16_t)(s_pushUs > 0xFFFF ? 0xFFFF : s_pushUs);
	s_stats.faceTests = s_faceTests;
	s_queryUs = s_pushUs = s_faceTests = 0;
	// the near list has GATHER_M of slack: rebuilt every few frames or after
	// a couple of metres, and whenever the cache changed under it
	const float mx = x - s_gatherX, my = y - s_gatherY, mz = z - s_gatherZ;
	if (s_frame - s_gatherFrame >= GATHER_FRAMES || mx * mx + my * my + mz * mz > 4.0f) s_nearDirty = true;
	if (s_nearDirty) {
		gather(x, y, z);
		s_gatherFrame = s_frame;
		s_gatherX = x;
		s_gatherY = y;
		s_gatherZ = z;
		s_nearDirty = false;
	}
	if (s_numWant) {
		stream(budgetUs);
		s_numWant = 0;
		if (s_nearDirty) gather(x, y, z);   // picks up what just arrived (and drops what left)
		s_nearDirty = false;
	}
	s_stats.nearCount = (uint16_t)s_numNear;
	s_stats.bytes = s_bytes;
	s_stats.updateUs = (uint16_t)timing::usSince(t0);
}

void update(float x, float y, float z) {
	if (s_index) step(x, y, z, UPDATE_US);
}

bool initStep() {
	if (s_step == 0) {
		s_step = 2;
		const uint32_t off = world::colOffset();
		const int nm = world::numModels();
		if (!off || !nm) {
			hud::log("\x1b[33mcol: no collision in world.dwld\x1b[39m");
			return true;
		}
		s_index = (ds::WorldColIndex*)mem::alloc(sizeof(ds::WorldColIndex) * nm, mem::TAG_COL);
		s_refs = (Ref*)mem::zalloc(sizeof(Ref) * nm, mem::TAG_COL);
		if (!s_index || !s_refs || !world::readAt(off, s_index, sizeof(ds::WorldColIndex) * nm)) {
			mem::free(s_index);
			mem::free(s_refs);
			s_index = nullptr;
			s_refs = nullptr;
			hud::log("\x1b[31mcol: index read failed\x1b[39m");
			return true;
		}
		s_numModels = nm;
		s_step = 1;
		s_preloadT0 = timing::vblanks();
		return false;
	}
	if (s_step == 2) return true;
	// CColStore::LoadCollision around the player before the first frame
	s_nearDirty = true;
	s_stats.wants = 0;
	step(player::g.x, player::g.y, player::g.z, PRELOAD_US);
	if (!s_stats.wants || timing::vblanks() - s_preloadT0 > PRELOAD_MAX_VBLANKS) {
		hud::log("col preload: %u models %uK, near %u", s_stats.models, (unsigned)(s_bytes >> 10), s_stats.nearCount);
		s_step = 2;
		return true;
	}
	return false;
}

// ----------------------------------------------------------------------------
// Debug overlay
// ----------------------------------------------------------------------------
struct DbgCtx {
	int32_t o[3];             // GL origin, SA 20.12
	float px, py, pz;         // player, SA
	int order[COL_NEAR_MAX];
	int count;
	int maxFaces;
	bool ground;
	float gx, gy, gz;
};

static void instanceMatrix(const DbgCtx& d, const Near& n) {
	// GL basis rows (x, z, -y) of the SA rows, x32: a raw cu vertex as v16 is
	// cu / 4096, the matrix brings it to metres
	MATRIX_RESTORE = DBG_SLOT;
	for (int k = 0; k < 3; k++) {
		MATRIX_MULT4x3 = (s32)n.r[k * 3] * 32;
		MATRIX_MULT4x3 = (s32)n.r[k * 3 + 2] * 32;
		MATRIX_MULT4x3 = -(s32)n.r[k * 3 + 1] * 32;
	}
	MATRIX_MULT4x3 = n.pos[0] - d.o[0];
	MATRIX_MULT4x3 = n.pos[2] - d.o[2];
	MATRIX_MULT4x3 = d.o[1] - n.pos[1];
}

static inline void vtx(int32_t x, int32_t y, int32_t z) {
	GFX_VERTEX16 = (u32)(u16)(s16)x | ((u32)(u16)(s16)y << 16);
	GFX_VERTEX16 = (u32)(u16)(s16)z;
}

static void emitDebug(void* ctx) {
	DbgCtx& d = *(DbgCtx*)ctx;
	const int32_t range = dbg::opts.colRange * 128;
	const int32_t pw[3] = { cu(d.px), cu(d.py), cu(d.pz + 1.0f) };
	int faces = 0;
	overlay::polyFmt(0);   // wireframe
	for (int oi = 0; oi < d.count && faces < d.maxFaces; oi++) {
		const Near& n = s_near[d.order[oi]];
		const ds::WorldCol* c = n.c;
		int32_t P[3];
		toLocal(n, pw, P);
		const int32_t lo[3] = { P[0] - range, P[1] - range, P[2] - range }, hi[3] = { P[0] + range, P[1] + range, P[2] + range };
		if (!overlaps(c->bmin, c->bmax, lo, hi)) continue;
		const View v = view(c);
		instanceMatrix(d, n);
		glBegin(GL_TRIANGLES);
		forFaces(c, v, lo, hi, [&](const ds::WorldColFace& f, const int16_t* const tri[3]) {
			if (faces >= d.maxFaces) return;
			const int32_t nz = worldNz(n, f.n);
			overlay::color(nz > WALL_NZ ? RGB15(6, 31, 8) : nz < -WALL_NZ ? RGB15(31, 6, 31) : RGB15(31, 18, 4));
			for (int k = 0; k < 3; k++) vtx(tri[k][0], tri[k][1], tri[k][2]);
			faces++;
		});
		glEnd();
		glBegin(GL_QUADS);
		overlay::color(RGB15(4, 28, 31));
		for (int bi = 0; bi < c->numBoxes && faces + 6 <= d.maxFaces; bi++) {
			const ds::WorldColBox& b = v.box[bi];
			if (!overlaps(b.min, b.max, lo, hi)) continue;
			const int16_t* m0 = b.min;
			const int16_t* m1 = b.max;
			for (int k = 0; k < 3; k++) {          // two faces per axis
				const int u = (k + 1) % 3, w = (k + 2) % 3;
				const int16_t sides[2] = { m0[k], m1[k] };
				for (const int16_t side : sides) {
					int32_t q[4][3];
					const int16_t uu[4] = { m0[u], m1[u], m1[u], m0[u] }, ww[4] = { m0[w], m0[w], m1[w], m1[w] };
					for (int j = 0; j < 4; j++) {
						q[j][k] = side;
						q[j][u] = uu[j];
						q[j][w] = ww[j];
						vtx(q[j][0], q[j][1], q[j][2]);
					}
				}
			}
			faces += 6;
		}
		overlay::color(RGB15(8, 12, 31));
		for (int si = 0; si < c->numSpheres && faces + 3 <= d.maxFaces; si++) {
			const ds::WorldColSphere& s = v.sph[si];
			const int32_t r = s.r;
			for (int k = 0; k < 3; k++) {          // three axis diamonds
				const int u = (k + 1) % 3;
				int32_t q[4][3];
				for (int j = 0; j < 4; j++) {
					q[j][0] = s.c[0]; q[j][1] = s.c[1]; q[j][2] = s.c[2];
				}
				q[0][k] += r; q[1][u] += r; q[2][k] -= r; q[3][u] -= r;
				for (int j = 0; j < 4; j++) vtx(q[j][0], q[j][1], q[j][2]);
			}
			faces += 3;
		}
		glEnd();
	}
	s_stats.debugFaces = (uint16_t)faces;

	// ped spheres + ground hit, player-local metres (v16 = m * 4096)
	MATRIX_RESTORE = DBG_SLOT;
	const float bx = d.px - (float)d.o[0] / 4096.0f, by = d.py - (float)d.o[1] / 4096.0f,
	            bz = d.pz - (float)d.o[2] / 4096.0f;
	auto gl = [&](float x, float y, float z) { vtx(floattov16(x), floattov16(z), floattov16(-y)); };
	glBegin(GL_QUADS);
	overlay::color(RGB15(22, 16, 31));   // not yellow: the skeleton overlay is
	const float r = PED_RADIUS;
	for (const float hz : PED_SPHERE_Z) {
		const float cz = bz + hz;
		gl(bx + r, by, cz); gl(bx, by + r, cz); gl(bx - r, by, cz); gl(bx, by - r, cz);
		gl(bx + r, by, cz); gl(bx, by, cz + r); gl(bx - r, by, cz); gl(bx, by, cz - r);
	}
	glEnd();
	if (d.ground) {
		const float gx = d.gx - (float)d.o[0] / 4096.0f, gy = d.gy - (float)d.o[1] / 4096.0f,
		            gz = d.gz - (float)d.o[2] / 4096.0f, k = 0.25f;
		overlay::polyFmt(31);
		glBegin(GL_TRIANGLES);
		overlay::color(RGB15(31, 31, 31));
		gl(gx - k, gy, gz); gl(gx + k, gy, gz); gl(gx + k, gy, gz);
		gl(gx, gy - k, gz); gl(gx, gy + k, gz); gl(gx, gy + k, gz);
		glEnd();
	}
}

void drawDebug(float originX, float originY, float originZ, float px, float py, float pz) {
	if (!dbg::opts.collision || !s_index) return;
	static DbgCtx d;
	d.o[0] = (int32_t)(originX * 4096.0f);
	d.o[1] = (int32_t)(originY * 4096.0f);
	d.o[2] = (int32_t)(originZ * 4096.0f);
	d.px = px;
	d.py = py;
	d.pz = pz;
	// nearest first, so the face budget drops the far ones
	d.count = s_numNear;
	for (int i = 0; i < s_numNear; i++) {
		int j = i;
		while (j > 0 && s_near[d.order[j - 1]].dist > s_near[i].dist) {
			d.order[j] = d.order[j - 1];
			j--;
		}
		d.order[j] = i;
	}
	d.maxFaces = dbg::opts.colPolys / (dbg::opts.colOutline ? 5 : 1);
	Hit h;
	d.ground = groundAt(px, py, pz + 0.5f, pz - 30.0f, h);
	d.gx = h.x;
	d.gy = h.y;
	d.gz = h.z;
	glStoreMatrix(DBG_SLOT);
	overlay::draw(emitDebug, &d, dbg::opts.colOutline);
	MATRIX_RESTORE = DBG_SLOT;
}

}
