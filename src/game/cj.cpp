#include "game/cj.h"

#include <math.h>
#include <nds.h>
#include <string.h>

#include "assets/texture.h"
#include "game/anim.h"
#include "game/animstore.h"
#include "game/debugopts.h"
#include "game/hud/hudtex.h"
#include "game/streaming.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "render/gxlist.h"

namespace cj {

// The default outfit from main.scm: (model, txd, texture) triples. Like
// CClothesBuilder::CreateSkinnedClump, the torso clothing model (vest.dff) IS
// the whole torso incl. bare arms and neck; torso.dff is only the fallback
// when no torso clothes are worn. Its "torso" texture is ConstructTextures'
// composite: player_torso skin with the vest placed on top wherever the vest
// texel is not transparent (PlaceTextureOnTopOfTexture). hands.dff samples
// the same composite (BODY_PART_TEX_NAMES[2] = "torso"). txd == nullptr marks
// a part that uses that composite.
struct PartDef { const char* dff; const char* txd; const char* tex; };
static constexpr const char* TORSO_BASE_TXD = "player_torso.txd";
static constexpr const char* TORSO_BASE_TEX = "torso";
static constexpr int NUM_PARTS = 5;

// Debug outfits (player.img items). Index 0 is the SCM default. A clothing
// txd holds one texture; when its name differs from `tex` the first texture
// is used. topTxd == nullptr = bare torso (player_torso skin only).
struct Outfit {
	const char* name;
	const char* torso; const char* topTxd; const char* topTex;
	const char* legs; const char* legsTxd; const char* legsTex;
	const char* feet; const char* feetTxd; const char* feetTex;
};
static const Outfit k_outfits[] = {
	{ "vest+jeans", "vest.dff", "vest.txd", "vest", "jeans.dff", "jeansdenim.txd", "legs",
	  "sneaker.dff", "sneakerbincblk.txd", "sneakerbincblk" },
	{ "tshirt", "tshirt.dff", "tshirtwhite.txd", "tshirtwhite", "chinos.dff", "chinoskhaki.txd", "chinoskhaki",
	  "shoe.dff", "shoedressblk.txd", "shoedressblk" },
	{ "hoodie", "hoodya.dff", "hoodyablack.txd", "hoodyablack", "tracktr.dff", "tracktrgang.txd", "tracktrgang",
	  "sneaker.dff", "sneakerheatwht.txd", "sneakerheatwht" },
	{ "suit", "suit1.dff", "suit1blk.txd", "suit1blk", "suit1tr.dff", "suit1trblk.txd", "suit1trblk",
	  "shoe.dff", "shoedressbrn.txd", "shoedressbrn" },
	{ "bare", "torso.dff", nullptr, nullptr, "legs.dff", "player_legs.txd", "legs",
	  "feet.dff", "player_feet.txd", "feet" },
};
static constexpr int NUM_OUTFITS = sizeof(k_outfits) / sizeof(k_outfits[0]);
static int s_outfit;
static PartDef s_defs[NUM_PARTS];

static void buildDefs() {
	const Outfit& o = k_outfits[s_outfit];
	s_defs[0] = { o.torso, nullptr, nullptr };              // torso composite
	s_defs[1] = { "head.dff", "player_face.txd", "face" };
	s_defs[2] = { "hands.dff", nullptr, nullptr };           // samples the torso composite
	s_defs[3] = { o.legs, o.legsTxd, o.legsTex };
	s_defs[4] = { o.feet, o.feetTxd, o.feetTex };
}

// Named texture, or the txd's first one.
static const ds::TexHeader* findOrFirst(const void* txd, const char* name) {
	const ds::TexHeader* t = hudtex::find(txd, name);
	if (t || !txd) return t;
	const ds::TxdHeader* th = (const ds::TxdHeader*)txd;
	if (th->magic != DS_FOURCC('D', 'T', 'X', 'D') || !th->count) return nullptr;
	return (const ds::TexHeader*)((const u8*)txd + ((const ds::TxdEntry*)(th + 1))->offset);
}

// GX skinning: every part is compiled once into a display list in which each
// vertex is preceded (when its bone changes) by MTX_RESTORE of that bone's
// matrix-stack slot. Per frame the CPU only evaluates the skeleton and stores
// one matrix per used bone (entity * world * invBone, multiplied by the GX
// itself); the list is DMA'd to the FIFO as-is. glPushMatrix uses the bottom
// of the 31-entry stack, so bones take the top slots down from SLOT_TOP and
// the entity matrix sits just below them.
static constexpr int MAX_PART_BONES = 16;
static constexpr int SLOT_TOP = 30;
static constexpr int SLOT_BASE = SLOT_TOP - MAX_PART_BONES;

struct Part {
	const PartDef* def;
	tex::Texture* tex;
	u32* list;                    // GX display list (STREAM, TAG_MODEL)
	s32 (*invBone)[12];           // per used bone, copied from the part's DMDL
	uint8_t node[MAX_PART_BONES]; // hAnim node per used bone (slot SLOT_TOP - k)
	uint8_t numBones;
	uint16_t verts;
};

static Part s_parts[NUM_PARTS];
static mdl::Model* s_skel;        // vest.dff parsed view: the shared skeleton
static void* s_skelBlob;
static tex::Texture* s_torsoTex;
static int s_loadStep;

static constexpr int SLOTS = 4;
// AnimBlendAssociation uses a short blend-out rather than deleting the old
// association when a new one is started. Locomotion already supplies its own
// weights; this is the safety net for one-shot actions (jump, fight and aim)
// that are submitted as a single clip by player.cpp.
static constexpr float BLEND_OUT_PER_TICK = 8.0f / 60.0f;
struct Slot {
	anim::Player anim;
	animstore::Clip* clip;
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
static int s_pedBlock = animstore::NO_BLOCK;
static int s_extraBlock[3] = { animstore::NO_BLOCK, animstore::NO_BLOCK, animstore::NO_BLOCK };
static uint8_t s_triedExtra[3];
static s32 s_scale[anim::MAX_NODES]; // 4.12 girth per bone, 4096 = unchanged

static s32 s_world[anim::MAX_NODES * 12];
static anim::Local s_locals[SLOTS][anim::MAX_NODES];

static void findShoulderBones(const mdl::Model* m);

// Reads a player.dspk entry into scratch (released with the caller's scope).
static void* loadTemp(mem::Scratch& scratch, const char* name) {
	ds::PackEntry e;
	if (!streaming::g_player.find(name, e)) return nullptr;
	void* buf = scratch.alloc(e.size);
	return buf && streaming::g_player.read(e.offset, buf, e.size) ? buf : nullptr;
}

// CClothesBuilder::ConstructTextures for the torso slot: copy the body skin,
// then PlaceTextureOnTopOfTexture the clothing (alpha != 0 wins). Uploaded as
// one direct-color texture shared by the torso and hands parts. Everything
// but the VRAM upload lives in scratch.
static tex::Texture* buildTorsoTexture() {
	mem::Scratch scratch;
	const Outfit& o = k_outfits[s_outfit];
	const ds::TexHeader* base = findOrFirst(loadTemp(scratch, TORSO_BASE_TXD), TORSO_BASE_TEX);
	const ds::TexHeader* top = o.topTxd ? findOrFirst(loadTemp(scratch, o.topTxd), o.topTex) : nullptr;
	if (!base && !top) return nullptr;
	const int w = base ? base->width : top->width, h = base ? base->height : top->height;
	const int tw = top ? top->width : 0, th = top ? top->height : 0;
	u16* topCol = top ? (u16*)scratch.alloc(tw * th * 2) : nullptr;
	u8* topA = top ? (u8*)scratch.alloc(tw * th) : nullptr;
	u8* blob = (u8*)scratch.alloc(sizeof(ds::TexHeader) + w * h * 2);
	if (!blob || (top && (!topCol || !topA))) return nullptr;
	if (top) hudtex::decodeInto(top, topCol, topA);
	ds::TexHeader* hd = (ds::TexHeader*)blob;
	*hd = { DS_FOURCC('D', 'T', 'E', 'X'), (uint16_t)w, (uint16_t)h, GL_RGBA, 0, 0, (uint32_t)(w * h * 2) };
	u16* px = (u16*)(hd + 1);
	if (base) hudtex::decodeInto(base, px, nullptr);
	else memset(px, 0, w * h * 2);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			u16& c = px[y * w + x];
			if (top) {
				const int i = (y * th / h) * tw + x * tw / w;
				if (topA[i]) c = topCol[i];
			}
			c |= BIT(15);
		}
	return tex::load(blob, tex::GROUP_GAME);
}

static int findSlotForNode(Part& p, int node) {
	for (int k = 0; k < p.numBones; k++)
		if (p.node[k] == node) return k;
	if (p.numBones >= MAX_PART_BONES) return -1;
	p.node[p.numBones] = (uint8_t)node;
	return p.numBones++;
}

static inline u32 vtxXY(s32 x, s32 y) { return (u32)(u16)(s16)x | ((u32)(u16)(s16)y << 16); }

// Compiles one part: used-bone table, inverse binds, and the display list.
// Returns the lowest bind-pose z (for the feet lift) through minZ.
static bool buildPart(Part& p, const mdl::Model* m, s32& minZ) {
	p.numBones = 0;
	p.verts = 0;
	size_t words = 16;
	for (int g = 0; g < m->numGeoms; g++) {
		const mdl::Geometry& geo = m->geoms[g];
		words += geo.h->numMaterials * 8 + geo.h->numIndices * 6;
		p.verts += geo.h->numVerts;
		if (geo.bone)
			for (int v = 0; v < geo.h->numVerts; v++)
				if (findSlotForNode(p, geo.bone[v]) < 0) {
					hud::log("\x1b[31mcj %s: >%d bones\x1b[39m", p.def->dff, MAX_PART_BONES);
					return false;
				}
	}
	p.invBone = (s32(*)[12])mem::alloc(sizeof(s32) * 12 * (p.numBones ? p.numBones : 1), mem::TAG_MODEL);
	if (!p.invBone) return false;
	for (int k = 0; k < p.numBones; k++) memcpy(p.invBone[k], m->invBone + p.node[k] * 12, 48);

	mem::Scratch scratch;
	u32* buf = (u32*)scratch.alloc(words * 4);
	if (!buf) return false;
	gx::ListBuilder dl(buf, (u32)words);
	const int tw = p.tex ? p.tex->w : 64, th = p.tex ? p.tex->h : 64;
	int lastSlot = -1;
	for (int g = 0; g < m->numGeoms; g++) {
		const mdl::Geometry& geo = m->geoms[g];
		const int pe = geo.h->posExp, ue = geo.h->uvExp;
		const int nv = geo.h->numVerts;
		for (int v = 0; v < nv; v++) {
			const s32 z = geo.h->offset[2] + (pe >= 0 ? (s32)geo.pos[v][2] << pe : (s32)geo.pos[v][2] >> -pe);
			if (z < minZ) minZ = z;
		}
		for (int mi = 0; mi < geo.h->numMaterials; mi++) {
			const ds::GeometryMaterial& mat = geo.mats[mi];
			dl.cmd(gx::CMD_POLYGON_ATTR, POLY_ALPHA(mat.alpha >= 248 ? 31 : (mat.alpha >> 3) | 1) | POLY_CULL_NONE);
			dl.cmd(gx::CMD_COLOR, mat.color);
			dl.cmd(gx::CMD_BEGIN_VTXS, GL_TRIANGLES);
			for (uint32_t i = mat.firstIndex; i < mat.firstIndex + mat.numIndices; i++) {
				const int v = geo.idx[i] < nv ? geo.idx[i] : 0;
				int slot = SLOT_BASE;
				if (geo.bone) {
					for (int k = 0; k < p.numBones; k++)
						if (p.node[k] == geo.bone[v]) { slot = SLOT_TOP - k; break; }
				}
				if (slot != lastSlot) {
					dl.cmd(gx::CMD_MTX_RESTORE, (u32)slot);
					lastSlot = slot;
				}
				if (geo.uv) {
					// t16 (12.4 texels) = uv * size * 16 = raw * size << ue >> 8
					const s32 u = ((s32)geo.uv[v][0] * (tw << ue)) >> 8;
					const s32 t = ((s32)geo.uv[v][1] * (th << ue)) >> 8;
					dl.cmd(gx::CMD_TEXCOORD, (u32)(u16)(s16)u | ((u32)(u16)(s16)t << 16));
				}
				const s32 x = geo.h->offset[0] + (pe >= 0 ? (s32)geo.pos[v][0] << pe : (s32)geo.pos[v][0] >> -pe);
				const s32 y = geo.h->offset[1] + (pe >= 0 ? (s32)geo.pos[v][1] << pe : (s32)geo.pos[v][1] >> -pe);
				const s32 z = geo.h->offset[2] + (pe >= 0 ? (s32)geo.pos[v][2] << pe : (s32)geo.pos[v][2] >> -pe);
				// f32 and v16 share the 12-bit fraction; CJ fits in +-8 units
				dl.cmd(gx::CMD_VTX_16, vtxXY(x, y), (u32)(u16)(s16)z);
			}
		}
	}
	const u32 n = dl.finish();
	if (!n) return false;
	p.list = (u32*)mem::alloc(n * 4, mem::TAG_MODEL);
	if (!p.list) return false;
	memcpy(p.list, buf, n * 4);
	gx::flushList(p.list);
	return true;
}

void shutdown() {
	for (Slot& s : s_slots) {
		animstore::release(s.clip);
		s = {};
	}
	for (Part& p : s_parts) {
		mem::free(p.list);
		mem::free(p.invBone);
		if (p.tex && p.tex != s_torsoTex) tex::unload(p.tex);
		p = {};
	}
	if (s_torsoTex) tex::unload(s_torsoTex);
	s_torsoTex = nullptr;
	mdl::free(s_skel);
	mem::free(s_skelBlob);
	s_skel = nullptr;
	s_skelBlob = nullptr;
	s_loadStep = 0;
}

static s32 s_minZ;

static bool loadPart(int i) {
	Part& p = s_parts[i];
	p.def = &s_defs[i];
	p.tex = p.def->txd ? nullptr : s_torsoTex;
	if (p.def->txd) {
		mem::Scratch scratch;
		void* txd = loadTemp(scratch, p.def->txd);
		if (txd) {
			p.tex = tex::loadFromTxd(txd, p.def->tex, tex::GROUP_GAME);
			if (!p.tex) p.tex = tex::loadFirst(txd, tex::GROUP_GAME);
		}
	}
	// the first part (vest) stays resident: its node table is the skeleton
	void* blob = streaming::g_player.load(p.def->dff, mem::TAG_MODEL);
	mdl::Model* m = mdl::load(blob);
	const bool ok = m && m->nodes && buildPart(p, m, s_minZ);
	hud::log("cj %s: %s %u verts %d bones tex %s", p.def->dff, ok ? "ok" : "FAILED",
	         (unsigned)p.verts, p.numBones, p.tex ? "ok" : "none");
	if (i == 0 && ok) {
		s_skel = m;
		s_skelBlob = blob;
		anim::prepare(s_skel);
	} else {
		mdl::free(m);
		mem::free(blob);
	}
	return ok;
}

// The on-foot control set, pinned in RAM at load: locomotion, jump, unarmed
// aim/fight and the armed aim shuffle (~105 KB of ped.ifp's ~900 KB). Every
// other clip streams from the cart the first time it plays. Do not trim
// JUMP_* or FIGHTIDLE: player.cpp switches to those immediately, and a miss
// there froze CJ in the locomotion pose while jumping or aiming.
static const char* const k_resident[] = {
	"idle_stance", "walk_player", "run_player", "sprint_civi",
	"walk_start", "idle_tired", "run_stop", "run_stopr",
	"jump_launch", "jump_launch_r", "jump_glide", "jump_land", "fall_land",
	"fightidle", "fighta_1", "fighta_2", "fighta_3",
	"fightsh_fwd", "fightsh_bwd", "fightsh_left", "fightsh_right",
	"idle_armed", "gun_stand",
	"gunmove_fwd", "gunmove_bwd", "gunmove_l", "gunmove_r",
};
static constexpr int NUM_RESIDENT = sizeof(k_resident) / sizeof(k_resident[0]);
static constexpr int STEP_PIN = NUM_PARTS + 1;
static constexpr int STEP_FINAL = STEP_PIN + NUM_RESIDENT;

bool initStep() {
	if (s_loadStep == 0) {
		shutdown();
		buildDefs();
		s_minZ = 0;
		s_torsoTex = buildTorsoTexture();
		hud::log("cj torso tex: %s", s_torsoTex ? "ok" : "FAILED");
	} else if (s_loadStep <= NUM_PARTS) {
		if (!loadPart(s_loadStep - 1) && s_loadStep == 1) {
			s_loadStep = STEP_FINAL + 1;   // no skeleton: give up, draw nothing
			return true;
		}
	} else if (s_loadStep < STEP_FINAL) {
		s_pedBlock = animstore::findBlock("ped");
		const char* name = k_resident[s_loadStep - STEP_PIN];
		if (!animstore::pin(s_pedBlock, name)) hud::log("\x1b[33mped clip %s missing\x1b[39m", name);
	} else if (s_loadStep == STEP_FINAL) {
		findShoulderBones(s_skel);
		s_pedBlock = animstore::findBlock("ped");
		hud::log("cj anims: %d clips %u KB resident", animstore::residentClips(),
		         (unsigned)(animstore::residentBytes() >> 10));
		s_miss[0] = 0;
		s_fat = s_muscle = 0;
		for (int i = 0; i < anim::MAX_NODES; i++) s_scale[i] = 1 << 12;
		const Clip idle = { "IDLE_stance", 1.0f, true, false, false, false, 0.0f };
		pose(&idle, 1);
		// SA ped models are authored pelvis-at-origin: the feet hang ~1 unit
		// below. Lift the entity matrix by the bind pose's lowest vertex so
		// the feet rest exactly on the draw position (bind skin = identity).
		s_lift = -(float)s_minZ / 4096.0f;
	}
	return ++s_loadStep > STEP_FINAL;
}

bool init() {
	s_loadStep = 0;
	while (!initStep()) {}
	return s_skel != nullptr;
}

int outfitCount() { return NUM_OUTFITS; }
int outfit() { return s_outfit; }
const char* outfitName(int i) { return (unsigned)i < (unsigned)NUM_OUTFITS ? k_outfits[i].name : "?"; }

void setOutfit(int i) {
	if ((unsigned)i >= (unsigned)NUM_OUTFITS) return;
	const float f = s_fat, m = s_muscle;
	s_outfit = i;
	init();   // a few small reads; pinned clips are cache hits
	setStats(f, m);
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

// fat.ifp / muscular.ifp: only the directory is opened; clips stream on use.
static int extraBlock(int which) {
	if (s_extraBlock[which] != animstore::NO_BLOCK || s_triedExtra[which] || !streaming::g_anim.isOpen())
		return s_extraBlock[which];
	s_triedExtra[which] = 1;
	const char* file = which == 1 ? "fat.ifp" : "muscular.ifp";
	s_extraBlock[which] = animstore::openInPack(streaming::g_anim, file);
	hud::log("cj %s %s", file, s_extraBlock[which] != animstore::NO_BLOCK ? "ok" : "MISSING");
	return s_extraBlock[which];
}

static bool playName(Slot& sl, const char* name) {
	if (!s_skel || !name) return false;
	const int g = groupId();
	const int blocks[2] = { g != 0 ? extraBlock(g) : animstore::NO_BLOCK, s_pedBlock };
	for (int b : blocks) {
		if (!animstore::hasClip(b, name)) continue;
		animstore::Clip* c = animstore::acquire(b, name);
		if (c && anim::play(sl.anim, s_skel, animstore::entry(c))) {
			animstore::release(sl.clip);
			sl.clip = c;
			return true;
		}
		animstore::release(c);
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

static void killSlot(Slot& sl) {
	sl.weight = 0.0f;
	sl.alive = 0;
	animstore::release(sl.clip);
	sl.clip = nullptr;
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
			if (!playName(s_slots[s], c.name)) continue;
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
	for (int i = 0; i < SLOTS; i++) {
		Slot& sl = s_slots[i];
		if (!sl.alive || keep[i]) continue;
		// Preserve the final pose of the outgoing association long enough to
		// blend it against the first pose of the incoming clip. This matters
		// most at launch -> glide -> land and at the fight/aim shuffles, which
		// do not have the locomotion state's explicit chained weights.
		sl.weight -= BLEND_OUT_PER_TICK;
		if (sl.weight <= 0.0f) killSlot(sl);
	}
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
	const mdl::Model* m = s_skel;
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
		// The end key closes the seam only when a loop wraps (I_NextKeyFrame's
		// `looped` is IsLooped-only). A non-looping clip clamped on its last key
		// makes advance() return true as well; adding `end` again there moved
		// the ped a whole clip-length on the final frame.
		if (wrapped && sl.anim.loop && anim::rootEndTranslation(sl.anim, m, end)) {
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

// CPed::ShoulderBoneRotation (MODEL_PLAYER only, after the anim update). On
// CJ the "breast" bones 301/302 are the deltoids: child of the upper arm,
// skinning the top of the shoulder. Anims key them with a fixed pose that only
// makes sense after this fix-up: the bone takes the upper arm's matrix, then
// its X (twist) Euler angle relative to the clavicle is halved. Without it the
// shoulder mesh sticks out as a box above the arm.
struct ShoulderBones { int breast, upperArm, clavicle; };
static ShoulderBones s_shoulders[2];

static int nodeOf(const mdl::Model* m, int16_t boneId) {
	for (int n = 0; n < m->numBones; n++)
		if (m->nodes[n].boneId == boneId) return n;
	return -1;
}

static void findShoulderBones(const mdl::Model* m) {
	static const int16_t k_ids[2][3] = { { 302, 32, 31 }, { 301, 22, 21 } };   // L, R
	for (int s = 0; s < 2; s++) {
		ShoulderBones& b = s_shoulders[s];
		b.breast = b.upperArm = b.clavicle = -1;
		if (!m || !m->nodes) continue;
		b.breast = nodeOf(m, k_ids[s][0]);
		b.upperArm = nodeOf(m, k_ids[s][1]);
		b.clavicle = nodeOf(m, k_ids[s][2]);
	}
}

// CMatrix::ConvertToEulerAngles / ConvertFromEulerAngles for the one flag set
// the original uses here: Tait-Bryan, ORDER_ZYX (index order 2,1,0, which
// negates the angles) | SWAP_XZ. f is a row matrix (right, up, at).
static constexpr int E1 = 2, E2 = 1, E3 = 0;
static void toEulerZYXSwap(const float (&f)[3][3], float& x, float& y, float& z) {
	const float cy = sqrtf(f[E1][E1] * f[E1][E1] + f[E2][E1] * f[E2][E1]);
	if (cy > 1.0f / 524288.0f) {
		x = atan2f(f[E3][E2], f[E3][E3]);
		y = atan2f(-f[E3][E1], cy);
		z = atan2f(f[E2][E1], f[E1][E1]);
	} else {
		x = atan2f(-f[E2][E3], f[E2][E2]);
		y = atan2f(-f[E3][E1], cy);
		z = 0.0f;
	}
	const float t = x; x = -z; y = -y; z = -t;
}

static void fromEulerZYXSwap(float x, float y, float z, float (&f)[3][3]) {
	const float t = x; x = -z; y = -y; z = -t;
	const float sx = sinf(x), cx = cosf(x), sy = sinf(y), cy = cosf(y), sz = sinf(z), cz = cosf(z);
	f[E1][E1] = cy * cz;
	f[E1][E2] = -(cx * sz) + cz * sx * sy;
	f[E1][E3] = sx * sz + cx * cz * sy;
	f[E2][E1] = cy * sz;
	f[E2][E2] = cx * cz + sx * sy * sz;
	f[E2][E3] = -(cz * sx) + cx * sy * sz;
	f[E3][E1] = -sy;
	f[E3][E2] = cy * sx;
	f[E3][E3] = cx * cy;
}

static void shoulderBoneRotation(s32* world) {
	for (const ShoulderBones& b : s_shoulders) {
		if (b.breast < 0 || b.upperArm < 0 || b.clavicle < 0) continue;
		const s32* U = world + b.upperArm * 12;
		const s32* C = world + b.clavicle * 12;
		s32* B = world + b.breast * 12;
		// rel = U * inverse(C) (row vectors); C's 3x3 is orthonormal here
		float rel[3][3], c[3][3];
		for (int r = 0; r < 3; r++)
			for (int k = 0; k < 3; k++) c[r][k] = (float)C[r * 3 + k] * (1.0f / 4096.0f);
		for (int r = 0; r < 3; r++)
			for (int k = 0; k < 3; k++)
				rel[r][k] = ((float)U[r * 3] * c[k][0] + (float)U[r * 3 + 1] * c[k][1] +
				             (float)U[r * 3 + 2] * c[k][2]) * (1.0f / 4096.0f);
		float x, y, z;
		toEulerZYXSwap(rel, x, y, z);
		fromEulerZYXSwap(x * 0.5f, y, z, rel);
		// back into hierarchy space: B = rel * C, position = the upper arm's
		for (int r = 0; r < 3; r++)
			for (int k = 0; k < 3; k++)
				B[r * 3 + k] = (s32)((rel[r][0] * c[0][k] + rel[r][1] * c[1][k] + rel[r][2] * c[2][k]) * 4096.0f);
		B[9] = U[9];
		B[10] = U[10];
		B[11] = U[11];
	}
}

static void rebuildScales() {
	for (int i = 0; i < anim::MAX_NODES; i++) s_scale[i] = 1 << 12;
	const mdl::Model* m = s_skel;
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
	if (g != 0 && extraBlock(g) == animstore::NO_BLOCK) return k_player;
	if (g == 1) return k_fat;
	if (g == 2) return k_muscle;
	return k_player;
}

void draw(float x, float y, float z, float heading) {
	const mdl::Model* skel = s_skel;
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
	shoulderBoneRotation(s_world);
	for (int b = 0; b < skel->numBones && b < anim::MAX_NODES; b++) {
		const s32 s = s_scale[b];
		if (s == (1 << 12)) continue;
		s32* m = s_world + b * 12;
		for (int k = 0; k < 9; k++) m[k] = mulf32(m[k], s);
	}

	// entity matrix: SA model space -> GL via rotX(-90), heading on the up
	// axis (SA heading is CCW-from-north; forward = (-sin h, cos h) so the
	// GL yaw is +heading), position in world (SA x,y,z -> GL x,z,-y)
	glPushMatrix();
	glTranslatef(x, z + s_lift, -y);
	glRotatef(heading * 57.2958f, 0.0f, 1.0f, 0.0f);
	glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
	glStoreMatrix(SLOT_BASE);
	s_vertCount = 0;
	for (int i = 0; i < NUM_PARTS && dbg::opts.drawCj; i++) {
		const Part& p = s_parts[i];
		if (!p.list) continue;
		// slot = entity * world[node] * invBone (GX: current = M * current)
		for (int k = 0; k < p.numBones; k++) {
			glRestoreMatrix(SLOT_BASE);
			glMultMatrix4x3((const m4x3*)(s_world + p.node[k] * 12));
			glMultMatrix4x3((const m4x3*)p.invBone[k]);
			glStoreMatrix(SLOT_TOP - k);
		}
		glBindTexture(GL_TEXTURE_2D, p.tex ? p.tex->id : 0);
		gx::callList(p.list);
		s_vertCount += p.verts;
	}
	if (dbg::opts.skeleton) {
		// bone overlay: one line per parent->child (a triangle with two equal
		// vertices is rasterized as a line by the GE), in entity space
		glRestoreMatrix(SLOT_BASE);
		glBindTexture(GL_TEXTURE_2D, 0);
		glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
		glBegin(GL_TRIANGLES);
		for (int b = 0; b < skel->numBones; b++) {
			const int par = skel->nodes[b].parent;
			if (par < 0) continue;
			const s32* a = s_world + par * 12 + 9;
			const s32* c = s_world + b * 12 + 9;
			glColor(skel->nodes[b].boneId >= 300 ? RGB15(31, 8, 8) : RGB15(31, 31, 0));
			glVertex3v16((v16)a[0], (v16)a[1], (v16)a[2]);
			glVertex3v16((v16)c[0], (v16)c[1], (v16)c[2]);
			glVertex3v16((v16)c[0], (v16)c[1], (v16)c[2]);
		}
		glEnd();
		glColor(RGB15(31, 31, 31));
	}
	glPopMatrix(1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

int vertCount() { return s_vertCount; }

}
