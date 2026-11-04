#include "game/cj.h"

#include <nds.h>
#include <stdlib.h>
#include <string.h>

#include "assets/texture.h"
#include "game/anim.h"
#include "game/streaming.h"
#include "hud/hud.h"

namespace cj {

// The default outfit from main.scm: (model, txd, texture) triples.
// vest.dff and torso.dff share the exact same mesh (324 verts, same skinning):
// the vest is a reskin whose arm-region triangles sample fully transparent
// texels in vest.txd (the "cut" -- CJ's skin shows through there). So we split
// the shared topology: torso.dff draws only the triangles fully bound to the
// neck/arm bones (real skin from player_torso.txd), vest.dff draws the rest
// (opaque cloth). Every triangle is emitted exactly once -> no z-fighting.
// hands.dff is a separate model part covering the forearm/hand/fingers.
// legs/feet skin parts are not drawn (jeans+sneakers cover them completely).
static constexpr u64 SKIN_NODES = (1ull << 4) | 0x1FC00ull | 0x1FC0000ull;
struct PartDef { const char* dff; const char* txd; const char* tex; u64 onlyIn; u64 skipIn; };
static const PartDef k_parts[] = {
	{ "torso.dff",   "player_torso.txd",   "torso",          SKIN_NODES, 0 },
	{ "vest.dff",    "vest.txd",           "vest",           0, SKIN_NODES },
	{ "head.dff",    "player_face.txd",    "face",           0, 0 },
	{ "hands.dff",   "hands.txd",          "torso8bit",      0, 0 },
	{ "jeans.dff",   "jeansdenim.txd",     "legs",           0, 0 },
	{ "sneaker.dff", "sneakerbincblk.txd", "sneakerbincblk", 0, 0 },
};
static constexpr int NUM_PARTS = sizeof(k_parts) / sizeof(k_parts[0]);

struct Part {
	const PartDef* def;
	mdl::Model* m;
	void* blob;          // DMDL blob (owned)
	tex::Texture* tex;   // VRAM texture for all its materials
};

static Part s_parts[NUM_PARTS];
static constexpr int SLOTS = 4;
struct Slot {
	anim::Player anim;
	char name[24];
	float weight;
	uint8_t alive;
};
static Slot s_slots[SLOTS];
static char s_miss[24];
static int s_vertCount;
static float s_lift;        // +z entity offset so the feet rest at the origin
static float s_rootX, s_rootY; // locomotion extracted this frame (model metres)
static float s_fat, s_muscle;
static const void* s_fatIfp;
static const void* s_musIfp;
static uint8_t s_triedFat, s_triedMus;
static s32 s_scale[anim::MAX_NODES]; // 4.12 girth per bone, 4096 = unchanged

static s32 s_world[anim::MAX_NODES * 12];
static s32 s_skin[anim::MAX_NODES * 12];
static anim::Local s_locals[SLOTS][anim::MAX_NODES];

// scratch buffers for one part's transformed verts
static constexpr int MAX_VERTS = 1024;
static v16 s_xyz[MAX_VERTS][3];
static t16 s_uv[MAX_VERTS][2];

static void* loadEntry(Pack& pack, const char* name) {
	return pack.load(pack.find(name));
}

static int skinVerts(const Part& p, const mdl::Geometry& g);

bool init() {
	for (int i = 0; i < NUM_PARTS; i++) {
		Part& p = s_parts[i];
		p.def = &k_parts[i];
		p.blob = loadEntry(streaming::g_player, p.def->dff);
		p.m = mdl::load(p.blob);
		p.tex = nullptr;
		void* txd = loadEntry(streaming::g_player, p.def->txd);
		if (txd) {
			p.tex = tex::loadFromTxd(txd, p.def->tex);
			if (!p.tex) p.tex = tex::loadFirst(txd);
			::free(txd);
		}
		hud::log("cj %s: %s, %u verts, tex %s", p.def->dff,
		         p.m ? "ok" : "FAILED",
		         p.m ? (unsigned)p.m->geoms[0].h->numVerts : 0u,
		         p.tex ? "ok" : "none");
	}
	if (!s_parts[0].m) return false;
	for (int i = 0; i < SLOTS; i++) s_slots[i].alive = 0;
	s_miss[0] = 0;
	s_fat = s_muscle = 0;
	for (int i = 0; i < anim::MAX_NODES; i++) s_scale[i] = 1 << 12;
	const Clip idle = { "IDLE_stance", 1.0f, true, false, false, false, 0.0f };
	pose(&idle, 1);

	// SA ped models are authored pelvis-at-origin: the feet hang ~1 unit
	// below. Measure the bind pose's lowest vert once and lift the entity
	// matrix by it so the feet rest exactly on the draw position.
	s32 bindW[anim::MAX_NODES * 12];
	anim::evalBind(s_parts[0].m, bindW);
	s32 minz = 0;
	for (int i = 0; i < NUM_PARTS; i++) {
		Part& p = s_parts[i];
		if (!p.m) continue;
		anim::skinForPart(p.m, bindW, s_skin);
		for (int g = 0; g < p.m->numGeoms; g++) {
			const int nv = skinVerts(p, p.m->geoms[g]);
			for (int v = 0; v < nv; v++)
				if (s_xyz[v][2] < minz) minz = s_xyz[v][2];
		}
	}
	s_lift = -(float)minz / 4096.0f;
	return true;
}

static bool nameEq(const char* a, const char* b) {
	if (!a || !b) return false;
	for (int i = 0; i < 24; i++) {
		unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return false;
		if (!ca) return true;
	}
	return true;
}

static void copyName(char* d, const char* s) {
	int i = 0;
	for (; i < 23 && s && s[i]; i++) d[i] = s[i];
	d[i] = 0;
}

static int groupId() {
	// CClothes::GetPlayerMotionGroupToLoad. Fat wins only when strictly fatter.
	if (s_fat > 500.0f && s_fat > s_muscle) return 1;
	if (s_muscle <= 500.0f) return 0;
	return 2;
}

static const void* extraIfp(int which) {
	const void*& slot = which == 1 ? s_fatIfp : s_musIfp;
	uint8_t& tried = which == 1 ? s_triedFat : s_triedMus;
	if (slot || tried || !streaming::g_anim.isOpen()) return slot;
	tried = 1;
	const char* file = which == 1 ? "fat.ifp" : "muscular.ifp";
	slot = streaming::g_anim.load(streaming::g_anim.find(file));
	hud::log("cj %s %s", file, slot ? "ok" : "MISSING");
	return slot;
}

static bool playName(anim::Player& p, const char* name) {
	const mdl::Model* m = s_parts[0].m;
	if (!m || !name) return false;
	const int g = groupId();
	const void* blobs[2];
	int n = 0;
	if (g != 0) blobs[n++] = extraIfp(g);
	blobs[n++] = streaming::g_pedIfp;
	for (int i = 0; i < n; i++) {
		if (blobs[i] && anim::play(p, m, blobs[i], name)) return true;
	}
	if (!nameEq(s_miss, name)) {
		copyName(s_miss, name);
		hud::log("\x1b[33manim '%s' missing\x1b[39m", name);
	}
	return false;
}

static int findSlot(const char* name) {
	for (int i = 0; i < SLOTS; i++)
		if (s_slots[i].alive && nameEq(s_slots[i].name, name)) return i;
	return -1;
}

void pose(const Clip* clips, int n) {
	uint8_t keep[SLOTS] = {};
	for (int i = 0; i < n; i++) {
		const Clip& c = clips[i];
		if (!c.name || c.weight <= 0.0f) continue;
		int s = findSlot(c.name);
		if (s < 0) {
			for (int k = 0; k < SLOTS; k++)
				if (!s_slots[k].alive) { s = k; break; }
			if (s < 0) {
				s = 0;
				for (int k = 1; k < SLOTS; k++)
					if (s_slots[k].weight < s_slots[s].weight) s = k;
			}
			if (!playName(s_slots[s].anim, c.name)) continue;
			copyName(s_slots[s].name, c.name);
			s_slots[s].alive = 1;
		}
		Slot& sl = s_slots[s];
		sl.weight = c.weight;
		sl.anim.loop = c.loop ? 1 : 0;
		sl.anim.extractRoot = c.extract ? 1 : 0;
		if (c.restart || c.sync) {
			int32_t t = (int32_t)(c.phase * (float)sl.anim.duration + 0.5f);
			if (t < 0) t = 0;
			if (t > sl.anim.duration) t = sl.anim.duration;
			// Sync every frame quantises the follower a tick behind the leader
			// often enough to rewind. A one-tick lag is left for advance() to
			// close; a real seek (restart, or a drifted follower) still rewinds.
			if (c.restart || t + 1 < sl.anim.time || t > sl.anim.time + 1)
				anim::setTime(sl.anim, t);
		}
		keep[s] = 1;
	}
	for (int i = 0; i < SLOTS; i++)
		if (!keep[i]) s_slots[i].alive = 0;
}

float clipPhase(const char* name) {
	const int s = findSlot(name);
	if (s < 0) return -1.0f;
	return anim::phase(s_slots[s].anim);
}

bool clipEnded(const char* name) {
	const int s = findSlot(name);
	if (s < 0) return false;
	const anim::Player& p = s_slots[s].anim;
	return !p.loop && p.time >= p.duration;
}

const char* animName() {
	int best = -1;
	for (int i = 0; i < SLOTS; i++)
		if (s_slots[i].alive && (best < 0 || s_slots[i].weight > s_slots[best].weight)) best = i;
	return best >= 0 ? s_slots[best].name : "";
}

void rootDelta(float& x, float& y) { x = s_rootX; y = s_rootY; }

void advance(int dt60) {
	s_rootX = s_rootY = 0;
	const mdl::Model* m = s_parts[0].m;
	if (!m) return;
	float dx = 0, dy = 0, wsum = 0;
	for (int i = 0; i < SLOTS; i++) {
		Slot& sl = s_slots[i];
		if (!sl.alive) continue;
		s32 before[3], after[3], end[3];
		const int32_t t0 = sl.anim.time;
		const bool take = sl.anim.extractRoot && sl.weight > 0.0f &&
		                  anim::rootTranslation(sl.anim, m, t0, before);
		const bool wrapped = anim::advance(sl.anim, dt60);
		if (!take || !anim::rootTranslation(sl.anim, m, sl.anim.time, after)) continue;
		s32 ddx = after[0] - before[0];
		s32 ddy = after[1] - before[1];
		if (wrapped && anim::rootEndTranslation(sl.anim, m, end)) {
			ddx += end[0];
			ddy += end[1];
		}
		dx += sl.weight * (float)ddx;
		dy += sl.weight * (float)ddy;
		wsum += sl.weight;
	}
	// Weights already sum to about 1 during a crossfade. Only a sum past 1
	// (two full locomotion clips) would double the step.
	if (wsum > 1.0f) {
		dx /= wsum;
		dy /= wsum;
	}
	s_rootX = dx / 4096.0f;
	s_rootY = dy / 4096.0f;
}

// CClothesBuilder blend ratios, then a per-bone girth. Scaling the 3x3 of the
// bone's world matrix (translation kept) bulges the vertices bound to that
// bone around its origin. Children are already in world space, so they do not
// inherit the scale — a fat belly does not drag the head. Dominant-bone
// skinning means only the vertices owned by that bone move.
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static void girthFor(int16_t id, float& fatAdd, float& musAdd) {
	fatAdd = musAdd = 0;
	switch (id) {
	case 201: fatAdd = 0.85f; musAdd = -0.12f; break;             // belly
	case 2:   fatAdd = 0.35f; musAdd = 0.20f; break;              // spine
	case 3:   fatAdd = 0.35f; musAdd = 0.25f; break;              // spine1
	case 1:   fatAdd = 0.25f; musAdd = 0.10f; break;              // pelvis
	case 41: case 51: fatAdd = 0.30f; musAdd = 0.30f; break;      // thighs
	case 42: case 52: fatAdd = 0.12f; musAdd = 0.22f; break;      // calves
	case 22: case 32: fatAdd = 0.15f; musAdd = 0.55f; break;      // upper arms
	case 23: case 33: fatAdd = 0.05f; musAdd = 0.35f; break;      // forearms
	case 301: case 302: fatAdd = 0.55f; musAdd = 0.10f; break;    // breasts
	case 21: case 31: fatAdd = 0.12f; musAdd = 0.20f; break;      // clavicles
	default: break;
	}
}

static void rebuildScales() {
	for (int i = 0; i < anim::MAX_NODES; i++) s_scale[i] = 1 << 12;
	const mdl::Model* m = s_parts[0].m;
	if (!m || !m->nodes) return;
	float rM = clamp01(s_muscle / 1000.0f);
	float rF = clamp01((s_fat - 200.0f) / 800.0f);
	float rN = 1.0f - rM - rF;
	if (rN <= 0.0f) {
		const float t = rF + rM;
		if (t > 0.0f) { rM /= t; rF /= t; }
		rN = 0.0f;
	}
	(void)rN;
	for (int n = 0; n < m->numBones && n < anim::MAX_NODES; n++) {
		float fa, ma;
		girthFor(m->nodes[n].boneId, fa, ma);
		float s = 1.0f + fa * rF + ma * rM;
		if (s < 0.85f) s = 0.85f;
		if (s > 1.90f) s = 1.90f;
		s_scale[n] = (s32)(s * 4096.0f + 0.5f);
	}
}

void setStats(float fat, float muscle) {
	if (fat < 0.0f) fat = 0.0f;
	if (fat > 1000.0f) fat = 1000.0f;
	if (muscle < 0.0f) muscle = 0.0f;
	if (muscle > 1000.0f) muscle = 1000.0f;
	if (fat == s_fat && muscle == s_muscle) return;
	s_fat = fat;
	s_muscle = muscle;
	rebuildScales();
}

float fat() { return s_fat; }
float muscle() { return s_muscle; }

Motion motion() {
	static const Motion k_player = {
		"IDLE_stance", "WALK_player", "run_player", "sprint_civi",
		"walk_start", "idle_tired", "run_stop", "run_stopR"
	};
	static const Motion k_fat = {
		"FatIdle", "FatWalk", "FatRun", "FatSprint",
		"FatWalkstart", "IDLE_tired", "run_stop", "run_stopR"
	};
	static const Motion k_muscle = {
		"MuscleIdle", "MuscleWalk", "MuscleRun", "MuscleSprint",
		"MuscleWalkstart", "idle_tired", "run_stop", "run_stopR"
	};
	const int g = groupId();
	if (g != 0 && !extraIfp(g)) return k_player;
	if (g == 1) return k_fat;
	if (g == 2) return k_muscle;
	return k_player;
}

static const s32 s_ident[12] = { 1 << 12, 0, 0, 0, 1 << 12, 0, 0, 0, 1 << 12, 0, 0, 0 };

// Skins a geometry's verts into s_xyz (model space, SA axes) and converts the
// uvs to t16 for the part's texture size. Returns vertex count.
static int skinVerts(const Part& p, const mdl::Geometry& g) {
	const int nv = g.h->numVerts > MAX_VERTS ? MAX_VERTS : g.h->numVerts;
	const int pe = g.h->posExp;
	const int ue = g.h->uvExp;
	const int umul = (p.tex ? p.tex->w : 64) << ue;   // t16 = raw * umul >> 8
	for (int i = 0; i < nv; i++) {
		s32 x = g.h->offset[0] + (pe >= 0 ? (s32)g.pos[i][0] << pe : (s32)g.pos[i][0] >> -pe);
		s32 y = g.h->offset[1] + (pe >= 0 ? (s32)g.pos[i][1] << pe : (s32)g.pos[i][1] >> -pe);
		s32 z = g.h->offset[2] + (pe >= 0 ? (s32)g.pos[i][2] << pe : (s32)g.pos[i][2] >> -pe);
		const s32* s = g.bone ? s_skin + g.bone[i] * 12 : s_ident;
		// row-vector apply: v' = v * M (column c picks m[c][r])
		const s32 sx = mulf32(x, s[0]) + mulf32(y, s[3]) + mulf32(z, s[6]) + s[9];
		const s32 sy = mulf32(x, s[1]) + mulf32(y, s[4]) + mulf32(z, s[7]) + s[10];
		const s32 sz = mulf32(x, s[2]) + mulf32(y, s[5]) + mulf32(z, s[8]) + s[11];
		// f32 and v16 share the same 12-bit fraction; cast directly
		s_xyz[i][0] = (v16)sx;
		s_xyz[i][1] = (v16)sy;
		s_xyz[i][2] = (v16)sz;
		if (g.uv) {
			s_uv[i][0] = (t16)(((s32)g.uv[i][0] * umul) >> 8);
			s_uv[i][1] = (t16)(((s32)g.uv[i][1] * umul) >> 8);
		} else {
			s_uv[i][0] = s_uv[i][1] = 0;
		}
	}
	return nv;
}

static void drawPart(const Part& p) {
	for (int g = 0; g < p.m->numGeoms; g++) {
		const mdl::Geometry& geo = p.m->geoms[g];
		const int nv = skinVerts(p, geo);
		if (p.tex) glBindTexture(GL_TEXTURE_2D, p.tex->id);
		for (int mi = 0; mi < geo.h->numMaterials; mi++) {
			const ds::GeometryMaterial& mat = geo.mats[mi];
			glPolyFmt(POLY_ALPHA(mat.alpha >= 248 ? 31 : (mat.alpha >> 3) | 1) | POLY_CULL_NONE);
			glColor(mat.color);
			glBegin(GL_TRIANGLES);
			for (uint32_t i = mat.firstIndex; i < mat.firstIndex + mat.numIndices; i += 3) {
				const uint16_t a = geo.idx[i] < nv ? geo.idx[i] : 0;
				const uint16_t b = geo.idx[i + 1] < nv ? geo.idx[i + 1] : 0;
				const uint16_t c = geo.idx[i + 2] < nv ? geo.idx[i + 2] : 0;
				if (geo.bone && (p.def->onlyIn | p.def->skipIn)) {
					const u64 t = (1ull << geo.bone[a]) | (1ull << geo.bone[b]) | (1ull << geo.bone[c]);
					if (p.def->onlyIn && (t & ~p.def->onlyIn)) continue;
					if (!(t & ~p.def->skipIn)) continue;
				}
				glTexCoord2t16(s_uv[a][0], s_uv[a][1]);
				glVertex3v16(s_xyz[a][0], s_xyz[a][1], s_xyz[a][2]);
				glTexCoord2t16(s_uv[b][0], s_uv[b][1]);
				glVertex3v16(s_xyz[b][0], s_xyz[b][1], s_xyz[b][2]);
				glTexCoord2t16(s_uv[c][0], s_uv[c][1]);
				glVertex3v16(s_xyz[c][0], s_xyz[c][1], s_xyz[c][2]);
			}
			glEnd();
		}
	}
}

void draw(float x, float y, float z, float heading) {
	const mdl::Model* skel = s_parts[0].m;
	if (!skel) return;
	const anim::Local* ptrs[SLOTS];
	s32 w[SLOTS];
	int n = 0;
	for (int i = 0; i < SLOTS; i++) {
		if (!s_slots[i].alive || s_slots[i].weight <= 0.0f) continue;
		anim::sampleLocal(skel, s_slots[i].anim, s_locals[i]);
		ptrs[n] = s_locals[i];
		w[n] = (s32)(s_slots[i].weight * 4096.0f + 0.5f);
		n++;
	}
	anim::blendWorld(skel, ptrs, w, n, s_world);
	if (skel && skel->nodes) {
		for (int b = 0; b < skel->numBones && b < anim::MAX_NODES; b++) {
			const s32 s = s_scale[b];
			if (s == (1 << 12)) continue;
			s32* m = s_world + b * 12;
			for (int k = 0; k < 9; k++) m[k] = mulf32(m[k], s);
		}
	}

	// entity matrix: SA model space -> GL via rotX(-90), heading on the up
	// axis (SA heading is CCW-from-north; forward = (-sin h, cos h) so the
	// GL yaw is +heading), position in world (SA x,y,z -> GL x,z,-y)
	glPushMatrix();
	glTranslatef(x, z + s_lift, -y);
	glRotatef(heading * 57.2958f, 0.0f, 1.0f, 0.0f);
	glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
	s_vertCount = 0;
	for (int i = 0; i < NUM_PARTS; i++) {
		Part& p = s_parts[i];
		if (!p.m) continue;
		anim::skinForPart(p.m, s_world, s_skin);
		for (int g = 0; g < p.m->numGeoms; g++) s_vertCount += p.m->geoms[g].h->numVerts;
		drawPart(p);
	}
	glPopMatrix(1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

int vertCount() { return s_vertCount; }

}
