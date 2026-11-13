#include "game/world.h"

#include <algorithm>
#include <limits.h>
#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "assets/ds_formats.h"
#include "assets/model.h"
#include "assets/texture.h"
#include "game/debugopts.h"
#include "game/player.h"
#include "game/streaming.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/fs.h"
#include "platform/timing.h"
#include "platform/video.h"
#include "render/gxlist.h"

namespace world {

using namespace mem::budget;

// ----------------------------------------------------------------------------
// Data
// ----------------------------------------------------------------------------
static constexpr uint16_t NONE = 0xFFFF;
static constexpr uint16_t TEX_BUSY = 0xFFFE;
static constexpr int MAX_MODEL_TEX = 12;     // unique textures per map model (rest draws untextured)
static constexpr int VIEW_SLOT = 13;         // matrix stack slot holding the view (CJ uses 14..30)
static constexpr int SECTOR_MARGIN = 40;     // m added to the view distance for the sector window
static constexpr int MAX_WANTS = 8;
static constexpr int MAX_LOADING = 8;
static constexpr u32 MODEL_READ_BYTES = 16 * 1024;
static constexpr u32 UPDATE_US = 3000;       // streaming CPU per in-game frame
static constexpr u32 PRELOAD_US = 8000;      // per loading-screen slice
static constexpr u32 UPLOAD_BYTES = 24 * 1024; // texture bytes per vblank
static constexpr u32 PRELOAD_MAX_VBLANKS = 600;
// Constant normal for the lighting trick (see compile()): +y, 10.10.10
static constexpr u32 NORMAL_UP = 0x1FFu << 10;

struct ModelLite { uint16_t drawDist, radius; uint8_t on, off, loaded, detail; };

enum : uint8_t { DETAIL_WIRES = 1, DETAIL_CLUTTER = 2, DETAIL_POLES = 4 };

static bool starts(const char* name, const char* prefix) {
	return strncmp(name, prefix, strlen(prefix)) == 0;
}

static uint8_t detailKind(const char* name) {
	if (((strstr(name, "wire") && !strstr(name, "fence") &&
	      !starts(name, "scumwires") && !starts(name, "induswire") && !starts(name, "lawnwires")) ||
	     starts(name, "cables") || starts(name, "des_powercable_")))
		return DETAIL_WIRES;
	if (starts(name, "telgrphpole") || starts(name, "tlgraphpole") ||
	    starts(name, "vgstlgraphpole") || starts(name, "vgntelepole") || starts(name, "sftelepole"))
		return DETAIL_POLES;
	if (starts(name, "cardboardbox") || starts(name, "blackbag") || starts(name, "proc_rubbish") ||
	    starts(name, "rubbish_box") || starts(name, "cj_rubbish") || starts(name, "cj_skip_rubbish") ||
	    starts(name, "cj_dump") || starts(name, "dyn_dumpster") || starts(name, "dyn_box_pile") ||
	    starts(name, "dyn_crate") || !strcmp(name, "dump1") || !strcmp(name, "bin1") ||
	    !strcmp(name, "cj_bin1") || !strcmp(name, "wastebin") ||
	    !strcmp(name, "woodenbox") || !strcmp(name, "dyn_boxes"))
		return DETAIL_CLUTTER;
	return 0;
}

enum : uint8_t { SLOT_FREE, SLOT_READING, SLOT_LOADING, SLOT_READY, SLOT_FAILED };
struct Slot {
	uint16_t model;
	uint8_t state, numTex;
	int8_t scaleExp;          // list vertices are v16 * 2^scaleExp
	uint8_t hingeDoor;
	uint16_t tris;
	uint16_t txd;
	uint8_t interior;
	u32* list;
	uint32_t listBytes;
	void* blob;               // DMDL while LOADING
	mdl::Model* view;         // parsed view while LOADING
	uint32_t readOffset, readSize, readDone;
	uint32_t lastUse;
	int32_t priority;         // distance to closest visible instance while loading
	int32_t boundCenter[3];   // model-space SA, 20.12
	int32_t boundRadius;      // model-space, 1/16 m
	uint16_t tex[MAX_MODEL_TEX];
};

enum : uint8_t { TEX_FREE, TEX_QUEUED, TEX_STAGED, TEX_READY, TEX_FAILED };
struct TexEntry {
	uint32_t nameHash;
	uint16_t txd, refs;
	uint8_t state, hasPal, pad[2];
	tex::Texture* t;
	uint32_t param, pal;      // TEXIMAGE_PARAM (+ repeat) and PLTT_BASE for the lists
	void* staging;            // DTEX waiting for the vblank
	uint32_t lastUse;
	uint32_t offset, size;    // DSPK payload to read when this texture is needed
};

using Inst = Instance;
static_assert(sizeof(Inst) == sizeof(ds::WorldInstance));

struct Sector {
	int16_t sx, sy;
	uint16_t count;
	uint8_t used, pad;
	Inst* inst;
	int32_t c[3];             // bounding sphere of every instance + model radius, 1/16 m
	int32_t reach;
};

struct TxdDir {
	int16_t txd;
	uint16_t count;
	uint32_t base, stamp;
	ds::TxdEntry* e;
};

struct Want { uint16_t model; int32_t key; };

static FILE* s_file;
static ds::WorldHeader s_hdr;
static ModelLite* s_models;
static ds::WorldTxd* s_txds;
static uint16_t* s_slotOf;
static Slot* s_slots;
static TexEntry* s_tex;
static Sector s_sectors[WORLD_SECTOR_SLOTS];
static TxdDir s_dirs[4];
static uint32_t s_dirStamp;
static uint8_t s_lastDetailFilter = 0xFF;
static Want s_want[MAX_WANTS];
static int s_numWant;
static uint32_t s_frame;
static uint32_t s_listBytes, s_texVram;
static int s_centerX = -9999, s_centerY = -9999, s_windowR;
static bool s_windowDirty = true;
static bool s_nearDirty = true;      // a sector of the 3x3 block around the player is missing
static int s_boost;                  // frames of raised streaming budget left (after suspend)
static int s_preloadStep;
static u32 s_preloadT0;
static Stats s_stats;
static int s_area;
static const Inst* s_door;
static float s_doorAngle;
static float s_doorX, s_doorY;
static int s_doorSign;

// Nearby HD geometry wins polygon RAM over distant LODs and large skyline
// models; last frame's drawn set gets a smaller bonus to suppress toggling.
struct Vis { const Inst* inst; uint16_t slot; uint8_t wasDrawn, pad; int32_t key; };
static Vis* s_vis;
static int s_numVis;

static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

static void freeSlot(Slot& s);
static int lruSlot();

bool ready() { return s_file && s_slots; }
const Stats& stats() { return s_stats; }

int sectors(SectorView* out, int max) {
	int n = 0;
	for (const Sector& sc : s_sectors) {
		if (!sc.used || !sc.count || n >= max) continue;
		out[n++] = { sc.inst, sc.count, { sc.c[0], sc.c[1], sc.c[2] }, sc.reach };
	}
	return n;
}

bool readAt(uint32_t offset, void* dst, uint32_t bytes) {
	return s_file && !fseek(s_file, (long)offset, SEEK_SET) && fread(dst, 1, bytes, s_file) == bytes;
}

uint32_t colOffset() { return s_file ? s_hdr.colOffset : 0; }
bool sectorsPending() { return s_windowDirty; }
bool nearSectorsPending() { return s_nearDirty; }
int numModels() { return s_file ? s_hdr.numModels : 0; }

uint8_t detailFilter() {
	return (dbg::opts.worldWires ? 0 : DETAIL_WIRES) |
	       (dbg::opts.worldClutter ? 0 : DETAIL_CLUTTER) |
	       (dbg::opts.worldPoles ? 0 : DETAIL_POLES);
}

bool detailVisible(int model) {
	return !s_models || model < 0 || model >= s_hdr.numModels ||
	       !(s_models[model].detail & detailFilter());
}

// ----------------------------------------------------------------------------
// Textures (CTxdStore-lite): one entry per (txd, texture name), refcounted by
// the models that use it, LRU-evicted once unreferenced.
// ----------------------------------------------------------------------------
static void freeTex(TexEntry& t) {
	if (t.t) {
		s_texVram -= t.t->vramBytes;
		tex::unload(t.t);
	}
	mem::free(t.staging);
	t = {};
}

static void releaseTex(uint16_t i) {
	if (i == NONE) return;
	TexEntry& t = s_tex[i];
	if (t.refs && --t.refs == 0) {
		t.lastUse = s_frame;
		if (t.state == TEX_QUEUED || t.state == TEX_STAGED || t.state == TEX_FAILED) freeTex(t);
	}
}

// Oldest unreferenced texture (READY), or -1.
static int lruTex() {
	int best = -1;
	for (int i = 0; i < WORLD_TEX_ENTRIES; i++) {
		const TexEntry& t = s_tex[i];
		if (t.state == TEX_READY && !t.refs && (best < 0 || t.lastUse < s_tex[best].lastUse)) best = i;
	}
	return best;
}

bool evictTexture() {
	const int v = s_tex ? lruTex() : -1;
	if (v < 0) return false;
	freeTex(s_tex[v]);
	return true;
}

static int newTexEntry() {
	for (int i = 0; i < WORLD_TEX_ENTRIES; i++)
		if (s_tex[i].state == TEX_FREE) return i;
	int v = lruTex();
	if (v < 0) {
		const int slot = lruSlot();
		if (slot >= 0) freeSlot(s_slots[slot]);
		for (int i = 0; i < WORLD_TEX_ENTRIES; i++)
			if (s_tex[i].state == TEX_FREE) return i;
		v = lruTex();
	}
	if (v >= 0) freeTex(s_tex[v]);
	return v;
}

static const TxdDir* txdDir(int txd) {
	TxdDir* victim = &s_dirs[0];
	for (TxdDir& d : s_dirs) {
		if (d.e && d.txd == txd) {
			d.stamp = ++s_dirStamp;
			return &d;
		}
		if (!d.e || (victim->e && d.stamp < victim->stamp)) victim = &d;
	}
	char name[32];
	snprintf(name, sizeof name, "%s.txd", s_txds[txd].name);
	ds::PackEntry pe;
	ds::TxdHeader th;
	Pack& pack = s_txds[txd].pad ? streaming::g_interiors : streaming::g_models;
	if (!pack.find(name, pe) || !pack.read(pe.offset, &th, sizeof th) ||
	    th.magic != DS_FOURCC('D', 'T', 'X', 'D'))
		return nullptr;
	ds::TxdEntry* e = (ds::TxdEntry*)mem::alloc(sizeof(ds::TxdEntry) * (th.count ? th.count : 1), mem::TAG_WORLD);
	if (!e) return nullptr;
	if (!pack.read(pe.offset + sizeof th, e, sizeof(ds::TxdEntry) * th.count)) {
		mem::free(e);
		return nullptr;
	}
	mem::free(victim->e);
	*victim = { (int16_t)txd, th.count, pe.offset, ++s_dirStamp, e };
	return victim;
}

// Finds (or starts reading) texture `name` in the model's TXD or its txdp
// parents. The DTEX is staged in RAM; vblank() uploads it.
static uint16_t requestTex(int txd, const char* name) {
	const uint32_t h = ds::fnv1a(name);
	for (int t = txd, depth = 0; t >= 0 && t < s_hdr.numTxds && depth < 4; t = s_txds[t].parent, depth++) {
		for (int i = 0; i < WORLD_TEX_ENTRIES; i++)
			if (s_tex[i].state != TEX_FREE && s_tex[i].txd == t && s_tex[i].nameHash == h) {
				s_tex[i].refs++;
				return (uint16_t)i;
			}
		const TxdDir* d = txdDir(t);
		if (!d) continue;
		for (int k = 0; k < d->count; k++) {
			if (strcasecmp(d->e[k].name, name)) continue;
			const int i = newTexEntry();
			if (i < 0) return TEX_BUSY;
			TexEntry& te = s_tex[i];
			te = {};
			te.nameHash = h;
			te.txd = (uint16_t)t;
			te.refs = 1;
			te.state = TEX_QUEUED;
			te.offset = d->base + d->e[k].offset;
			te.size = d->e[k].size;
			return (uint16_t)i;
		}
	}
	return NONE;
}

void vblank() {
	if (!s_tex) return;
#ifdef GTASADS_DEBUG
	const u32 uploadT0 = timing::ticks();
#endif
	u32 spent = 0;
	for (int i = 0; i < WORLD_TEX_ENTRIES && spent < UPLOAD_BYTES; i++) {
		TexEntry& te = s_tex[i];
		if (te.state != TEX_STAGED) continue;
		if (!video::textureUploadWindow()) break;
		const ds::TexHeader* h = (const ds::TexHeader*)te.staging;
		const u32 bytes = h->texelBytes + h->numColors * 2u;
		while (s_texVram + bytes > WORLD_TEX_VRAM) {
			const int v = lruTex();
			if (v < 0) break;
			freeTex(s_tex[v]);
		}
		tex::Texture* t = tex::load(h, tex::GROUP_WORLD);
		for (int v; !t && (v = lruTex()) >= 0;) {   // VRAM fragmented/full: evict until it fits
			freeTex(s_tex[v]);
			t = tex::load(h, tex::GROUP_WORLD);
		}
		if (!t && bytes <= WORLD_TEX_VRAM) {
			const int v = lruSlot();
			if (v >= 0) freeSlot(s_slots[v]);
			if (s_frame - te.lastUse < 20) break;
		}
		const bool paletted = h->numColors != 0;   // h dies with the staging buffer
		mem::free(te.staging);
		te.staging = nullptr;
		spent += bytes;
		s_stats.uploads++;
		if (!t) {
			te.state = TEX_FAILED;
			continue;
		}
		te.t = t;
		te.state = TEX_READY;
		te.lastUse = s_frame;
		s_texVram += t->vramBytes;
		// the lists set the texture registers themselves
		glBindTexture(0, t->id);
		te.param = glGetTexParameter() | GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;
		int pal = -1;
		if (paletted) glGetColorTableParameterEXT(0, GL_COLOR_TABLE_FORMAT_EXT, &pal);
		te.hasPal = pal >= 0;
		te.pal = pal < 0 ? 0 : (uint32_t)pal;
	}
	if (spent) glBindTexture(0, 0);
#ifdef GTASADS_DEBUG
	s_stats.uploadUs = timing::usSince(uploadT0);
#endif
}

// ----------------------------------------------------------------------------
// Model slots (CStreaming::RequestModel / CModelInfo, reduced)
// ----------------------------------------------------------------------------
static void freeSlot(Slot& s) {
	if (s.state == SLOT_READY) s_stats.evictions++;
	for (int i = 0; i < s.numTex; i++) releaseTex(s.tex[i]);
	mem::free(s.list);
	mdl::free(s.view);
	mem::free(s.blob);
	s_listBytes -= s.listBytes;
	if (s.model < s_hdr.numModels) s_slotOf[s.model] = NONE;
	s = {};
}

// Oldest READY/FAILED slot not drawn this or last frame, or -1.
static int lruSlot() {
	int best = -1;
	for (int i = 0; i < WORLD_MODEL_SLOTS; i++) {
		const Slot& s = s_slots[i];
		if ((s.state == SLOT_READY || s.state == SLOT_FAILED) && s.lastUse + 1 < s_frame &&
		    (best < 0 || s.lastUse < s_slots[best].lastUse))
			best = i;
	}
	return best;
}

static bool startLoad(uint16_t model, int32_t priority) {
	int si = -1;
	for (int i = 0; i < WORLD_MODEL_SLOTS && si < 0; i++)
		if (s_slots[i].state == SLOT_FREE) si = i;
	if (si < 0 && (si = lruSlot()) >= 0) freeSlot(s_slots[si]);
	if (si < 0) return false;
	Slot& s = s_slots[si];
	s = {};
	s.model = model;
	s.state = SLOT_FAILED;
	s.lastUse = s_frame;
	s.priority = priority;
	s_slotOf[model] = (uint16_t)si;
	s_stats.loads++;
	if (s_models[model].loaded) s_stats.reloads++;
	s_models[model].loaded = 1;

	ds::WorldModel wm;
	if (fseek(s_file, (long)(s_hdr.modelsOffset + model * sizeof wm), SEEK_SET) || fread(&wm, sizeof wm, 1, s_file) != 1)
		return true;
	char name[32];
	snprintf(name, sizeof name, "%.24s.dff", wm.name);
	ds::PackEntry pe;
	Pack& pack = wm.flags & 0x80000000u ? streaming::g_interiors : streaming::g_models;
	if (!pack.find(name, pe)) {
		hud::log("\x1b[33mworld: %s missing\x1b[39m", name);
		return true;
	}
	s.blob = mem::alloc(pe.size, mem::TAG_MODEL);
	if (!s.blob) {
		freeSlot(s);
		return false;
	}
	s.readOffset = pe.offset;
	s.readSize = pe.size;
	s.txd = wm.txd;
	s.interior = (wm.flags & 0x80000000u) != 0;
	s.state = SLOT_READING;
	return true;
}

static bool finishRead(Slot& s) {
	s.view = mdl::load(s.blob);
	if (!s.view) {
		freeSlot(s);
		return false;
	}
	for (int g = 0; g < s.view->numGeoms; g++) {
		const mdl::Geometry& geo = s.view->geoms[g];
		for (int mi = 0; mi < geo.h->numMaterials; mi++) {
			const char* tn = geo.mats[mi].texture;
			if (!tn[0] || s.txd == NONE || s.numTex >= MAX_MODEL_TEX) continue;
			const uint32_t h = ds::fnv1a(tn);
			bool dup = false;
			for (int k = 0; k < s.numTex && !dup; k++) dup = s.tex[k] != NONE && s_tex[s.tex[k]].nameHash == h;
			if (!dup) {
				const uint16_t ti = requestTex(s.txd, tn);
				if (ti == TEX_BUSY) {
					freeSlot(s);
					return false;
				}
				s.tex[s.numTex++] = ti;
			}
		}
	}
	s.state = SLOT_LOADING;
	return true;
}

static inline u16 mul15(u16 a, u16 b) {
	auto ch = [&](int sh) { return (u16)(((((a >> sh) & 31) * (((b >> sh) & 31) + 1)) >> 5) << sh); };
	return ch(0) | ch(5) | ch(10);
}

static inline s32 posAt(const mdl::Geometry& g, int v, int k) {
	const int pe = g.h->posExp;
	return g.h->offset[k] + (pe >= 0 ? (s32)g.pos[v][k] * (1 << pe) : (s32)g.pos[v][k] >> -pe);
}

static void frameMatrix(const mdl::Model& m, int frame, s32 out[12]) {
	for (int k = 0; k < 12; k++) out[k] = k < 9 && k % 4 == 0 ? 4096 : 0;
	for (int depth = 0; frame >= 0 && frame < m.numFrames && depth < m.numFrames; depth++) {
		const mdl::Frame& f = m.frames[frame];
		s32 next[12];
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) {
				int64_t sum = 0;
				for (int k = 0; k < 3; k++) sum += (int64_t)out[r * 3 + k] * f.rot[k * 3 + c];
				next[r * 3 + c] = (s32)(sum >> 12);
			}
		for (int c = 0; c < 3; c++) {
			int64_t sum = 0;
			for (int k = 0; k < 3; k++) sum += (int64_t)out[9 + k] * f.rot[k * 3 + c];
			next[9 + c] = (s32)(sum >> 12) + f.pos[c];
		}
		memcpy(out, next, sizeof next);
		frame = f.parent;
	}
}

static inline s32 atomicPos(const mdl::Geometry& g, int v, int k, const s32 mat[12]) {
	int64_t sum = 0;
	for (int j = 0; j < 3; j++) sum += (int64_t)posAt(g, v, j) * mat[j * 3 + k];
	return (s32)(sum >> 12) + mat[9 + k];
}

// DMDL -> one packed GX list for the whole model. World lighting without a
// per-vertex CPU pass: each vertex colour (prelit * material colour) is sent
// as the AMBIENT material colour followed by a constant NORMAL; with diffuse,
// specular and emission at 0 the GE outputs ambient * light0 colour, and
// light0 = timecycle world light is set once per frame. DIF_AMB + NORMAL are
// only emitted when the colour changes. Texture registers are baked in
// (repeat on), so the textures must stay put while the list lives.
static ITCM_CODE bool compile(Slot& s) {
	const mdl::Model* m = s.view;
	s32 maxAbs = 1;
	s32 lo[3] = { INT_MAX, INT_MAX, INT_MAX }, hi[3] = { INT_MIN, INT_MIN, INT_MIN };
	uint32_t numIdx = 0, numMats = 0;
	for (int a = 0; a < m->numAtomics; a++) {
		const ds::ModelAtomic& atom = m->atomics[a];
		if (atom.geometry >= m->numGeoms) continue;
		const mdl::Geometry& geo = m->geoms[atom.geometry];
		s32 mat[12];
		frameMatrix(*m, atom.frame, mat);
		numIdx += geo.h->numIndices;
		numMats += geo.h->numMaterials;
		for (int v = 0; v < geo.h->numVerts; v++)
			for (int k = 0; k < 3; k++) {
				const s32 value = atomicPos(geo, v, k, mat);
				if (value < lo[k]) lo[k] = value;
				if (value > hi[k]) hi[k] = value;
				const s32 extent = iabs(value);
				if (extent > maxAbs) maxAbs = extent;
			}
	}
	s32 half[3] = {};
	for (int k = 0; k < 3; k++)
		if (lo[k] <= hi[k]) {
			s.boundCenter[k] = lo[k] + (hi[k] - lo[k]) / 2;
			half[k] = (hi[k] - lo[k] + 511) >> 9;
		}
	s.hingeDoor = lo[0] <= hi[0] && lo[1] <= hi[1] &&
	              hi[0] - lo[0] > 2048 && hi[1] - lo[1] < 2048 &&
	              (iabs(lo[0]) < 1024 || iabs(hi[0]) < 1024);
	s.boundRadius = (int32_t)sqrt32(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]) + 32;
	int e = 0;
	while ((maxAbs >> e) > 32767) e++;
	const u32 cap = numIdx * 6 + numMats * 8 + 8;
	mem::Scratch scratch;
	u32* buf = (u32*)scratch.alloc(cap * 4);
	if (!buf) {
		s.readDone = cap * 4;
#ifdef GTASADS_DEBUG
		s_stats.compileFailModel = s.model;
		s_stats.compileFailReason = 1;
		s_stats.compileFailBytes = cap * 4;
#endif
		return false;
	}
	gx::ListBuilder dl(buf, cap);
	dl.cmd(gx::CMD_SPE_EMI, 0);
	uint32_t tris = 0;
	struct CachedPos { int v; s32 p[3]; };
	CachedPos posCache[128];
	for (int a = 0; a < m->numAtomics; a++) {
		const ds::ModelAtomic& atom = m->atomics[a];
		if (atom.geometry >= m->numGeoms) continue;
		const mdl::Geometry& geo = m->geoms[atom.geometry];
		s32 frame[12];
		frameMatrix(*m, atom.frame, frame);
		for (CachedPos& c : posCache) c.v = -1;
		const int nv = geo.h->numVerts, ue = geo.h->uvExp;
		for (int mi = 0; mi < geo.h->numMaterials; mi++) {
			const ds::GeometryMaterial& mat = geo.mats[mi];
			const TexEntry* te = nullptr;
			if (mat.texture[0]) {
				const uint32_t h = ds::fnv1a(mat.texture);
				for (int k = 0; k < s.numTex && !te; k++)
					if (s.tex[k] != NONE && s_tex[s.tex[k]].state == TEX_READY && s_tex[s.tex[k]].nameHash == h)
						te = &s_tex[s.tex[k]];
			}
			const int alpha = mat.alpha >= 248 ? 31 : (mat.alpha >> 3) | 1;
			dl.cmd(gx::CMD_POLYGON_ATTR, POLY_ALPHA(alpha) | POLY_CULL_BACK | POLY_FOG | POLY_FORMAT_LIGHT0);
			dl.cmd(gx::CMD_TEXIMAGE_PARAM, te ? te->param : 0);
			if (te && te->hasPal) dl.cmd(gx::CMD_PLTT_BASE, te->pal);
			dl.cmd(gx::CMD_BEGIN_VTXS, GL_TRIANGLES);
			const bool uv = te && geo.uv;
			const int tw = te ? te->t->w : 0, th = te ? te->t->h : 0;
			u32 lastCol = 0xFFFFFFFF;
			const uint32_t end = mat.firstIndex + mat.numIndices;
			for (uint32_t i = mat.firstIndex; i + 2 < end; i += 3) {
				int vi[3];
				s32 tu[3] = {}, tv[3] = {};
				for (int k = 0; k < 3; k++) {
					vi[k] = geo.idx[i + k] < nv ? geo.idx[i + k] : 0;
					if (uv) {
						// t16 (12.4 texels) = uv * size * 16 = raw * size << ue >> 8
						tu[k] = ((s32)geo.uv[vi[k]][0] * (tw << ue)) >> 8;
						tv[k] = ((s32)geo.uv[vi[k]][1] * (th << ue)) >> 8;
					}
				}
				// UVs tile a lot (roads): shift each triangle by whole repeats so
				// the 12.4 texcoords stay inside +-2048 texels
				s32 su = 0, sv = 0;
				if (uv) {
					const s32 pu = tw * 16, pv = th * 16;
					s32 mu = tu[0], mv = tv[0];
					for (int k = 1; k < 3; k++) {
						if (tu[k] < mu) mu = tu[k];
						if (tv[k] < mv) mv = tv[k];
					}
					su = (mu >= 0 ? mu / pu : -((-mu + pu - 1) / pu)) * pu;
					sv = (mv >= 0 ? mv / pv : -((-mv + pv - 1) / pv)) * pv;
				}
				for (int k = 0; k < 3; k++) {
					const int v = vi[k];
					if (uv) {
						s32 u = tu[k] - su, t = tv[k] - sv;
						u = u > 32767 ? 32767 : u;
						t = t > 32767 ? 32767 : t;
						dl.cmd(gx::CMD_TEXCOORD, (u32)(u16)(s16)u | ((u32)(u16)(s16)t << 16));
					}
					const u16 c = mul15(geo.color ? geo.color[v] : 0x7FFF, mat.color);
					if (c != lastCol) {
						dl.cmd(gx::CMD_DIF_AMB, (u32)c << 16);
						dl.cmd(gx::CMD_NORMAL, NORMAL_UP);
						lastCol = c;
					}
					CachedPos& cp = posCache[(unsigned)v & 127];
					if (cp.v != v) {
						cp.v = v;
						for (int axis = 0; axis < 3; axis++) cp.p[axis] = atomicPos(geo, v, axis, frame);
					}
					const s32 x = cp.p[0] >> e, y = cp.p[1] >> e, z = cp.p[2] >> e;
					dl.cmd(gx::CMD_VTX_16, (u32)(u16)(s16)x | ((u32)(u16)(s16)y << 16), (u32)(u16)(s16)z);
				}
				tris++;
			}
		}
	}
	const u32 n = dl.finish();
	if (!n) {
		s.readDone = UINT_MAX;
#ifdef GTASADS_DEBUG
		s_stats.compileFailModel = s.model;
		s_stats.compileFailReason = 2;
		s_stats.compileFailBytes = cap * 4;
#endif
		return false;
	}
	u32* list = nullptr;
	for (;;) {
		if (s_listBytes + n * 4 <= WORLD_LIST_BYTES)
			list = (u32*)mem::alloc(n * 4, mem::TAG_MODEL);
		if (list) break;
		const int v = lruSlot();
		if (v < 0) {
			s.readDone = n * 4;
#ifdef GTASADS_DEBUG
			s_stats.compileFailModel = s.model;
			s_stats.compileFailReason = 3;
			s_stats.compileFailBytes = n * 4;
#endif
			return false;
		}
		freeSlot(s_slots[v]);
	}
	memcpy(list, buf, n * 4);
	s.list = list;
	gx::flushList(s.list);
	s.listBytes = n * 4;
	s_listBytes += s.listBytes;
	s.scaleExp = (int8_t)e;
	s.tris = (uint16_t)(tris > 0xFFFF ? 0xFFFF : tris);
	return true;
}

// LOADING -> READY once every texture is uploaded (or failed).
static bool finishLoad(Slot& s) {
	for (int k = 0; k < s.numTex; k++)
		if (s.tex[k] != NONE && (s_tex[s.tex[k]].state == TEX_QUEUED ||
		                         s_tex[s.tex[k]].state == TEX_STAGED)) return false;
#ifdef GTASADS_DEBUG
	const u32 compileT0 = timing::ticks();
#endif
	const bool compiled = compile(s);
#ifdef GTASADS_DEBUG
	s_stats.compileUs += timing::usSince(compileT0);
#endif
	if (!compiled) {
		for (int k = 0; k < s.numTex; k++) releaseTex(s.tex[k]);
		s.numTex = 0;
		mdl::free(s.view);
		mem::free(s.blob);
		s.view = nullptr;
		s.blob = nullptr;
		s.lastUse = s_frame;
		s.state = SLOT_FAILED;
		return false;
	}
	mdl::free(s.view);
	mem::free(s.blob);
	s.view = nullptr;
	s.blob = nullptr;
	s.state = SLOT_READY;
	return true;
}

// ----------------------------------------------------------------------------
// Sectors (CWorld sector window)
// ----------------------------------------------------------------------------
static void freeSector(Sector& sc) {
	mem::free(sc.inst);
	sc = {};
}

static bool loadSector(int sx, int sy) {
	Sector* sc = nullptr;
	for (Sector& c : s_sectors)
		if (!c.used) { sc = &c; break; }
	if (!sc) return false;
	ds::WorldSector ws;
	const long off = (long)(s_hdr.sectorsOffset + (sy * s_hdr.gridW + sx) * sizeof ws);
	if (fseek(s_file, off, SEEK_SET) || fread(&ws, sizeof ws, 1, s_file) != 1) return false;
	Inst* inst = nullptr;
	uint16_t count = 0;
	if (ws.count) {
		mem::Scratch scratch;
		Inst* source = (Inst*)scratch.alloc(sizeof(Inst) * ws.count);
		if (!source) return false;
		if (fseek(s_file, (long)(s_hdr.instancesOffset + ws.first * sizeof(Inst)), SEEK_SET) ||
		    fread(source, sizeof(Inst), ws.count, s_file) != ws.count) return false;
		for (int i = 0; i < ws.count; i++)
			if (source[i].area == s_area || source[i].area == 13) count++;
		if (count) inst = (Inst*)mem::alloc(sizeof(Inst) * count, mem::TAG_WORLD);
		if (count && !inst) return false;
		int n = 0;
		for (int i = 0; i < ws.count; i++) {
			if (source[i].area != s_area && source[i].area != 13) continue;
			inst[n] = source[i];
			int16_t* r = inst[n].m;
			for (int row = 0; row < 3; row++) {      // SA (x,y,z) -> GL (x,z,-y)
				const int16_t y = r[row * 3 + 1], z = r[row * 3 + 2];
				r[row * 3 + 1] = z;
				r[row * 3 + 2] = (int16_t)-y;
			}
			if (inst[n].model >= s_hdr.numModels) inst[n].model = 0;
			n++;
		}
	}
	*sc = { (int16_t)sx, (int16_t)sy, count, 1, 0, inst, { 0, 0, 0 }, 0 };
	// sector sphere for the scan's early-out: centre = sector middle at the
	// mean height, reach = farthest instance surface
	const int size = s_hdr.sectorSize;
	int32_t zs = 0;
	for (int i = 0; i < count; i++) zs += inst[i].pos[2] >> 8;
	sc->c[0] = (s_hdr.originX + sx * size + size / 2) * 16;
	sc->c[1] = (s_hdr.originY + sy * size + size / 2) * 16;
	sc->c[2] = count ? zs / count : 0;
	for (int i = 0; i < count; i++) {
		const int32_t dx = (inst[i].pos[0] >> 8) - sc->c[0], dy = (inst[i].pos[1] >> 8) - sc->c[1],
		              dz = (inst[i].pos[2] >> 8) - sc->c[2];
		const int32_t r = (int32_t)sqrt32(dx * dx + dy * dy + dz * dz) + s_models[inst[i].model].radius * 16;
		if (r > sc->reach) sc->reach = r;
	}
	return true;
}

// Keeps every sector within the view distance (+ margin) of (x, y) loaded.
// `maxLoads` bounds the file reads per call.
static void updateSectors(float x, float y, int maxLoads) {
	const int size = s_hdr.sectorSize;
	const int cx = (int)floorf((x - s_hdr.originX) / size), cy = (int)floorf((y - s_hdr.originY) / size);
	const int reach = dbg::opts.fogView + SECTOR_MARGIN;
	const int r = (reach + size - 1) / size;
	// the window is decided for the centre of the current sector, so it only
	// changes when the player crosses a sector edge (no per-frame work)
	const int px = s_hdr.originX + cx * size + size / 2, py = s_hdr.originY + cy * size + size / 2;
	auto inWindow = [&](int sx, int sy) {
		const int x0 = s_hdr.originX + sx * size, y0 = s_hdr.originY + sy * size;
		const int dx = px < x0 ? x0 - px : px > x0 + size ? px - x0 - size : 0;
		const int dy = py < y0 ? y0 - py : py > y0 + size ? py - y0 - size : 0;
		return dx * dx + dy * dy <= reach * reach;
	};
	if (cx != s_centerX || cy != s_centerY || r != s_windowR) {
		if (s_centerX != -9999 && (iabs(cx - s_centerX) > r || iabs(cy - s_centerY) > r))
			suspend();
		for (Sector& sc : s_sectors)
			if (sc.used && !inWindow(sc.sx, sc.sy)) freeSector(sc);
		s_centerX = cx;
		s_centerY = cy;
		s_windowR = r;
		s_windowDirty = s_nearDirty = true;
	}
	if (!s_windowDirty) return;
	bool missing = false, nearMissing = false;
	for (int ring = 0; ring <= r; ring++) {
		for (int sy = cy - ring; sy <= cy + ring; sy++) {
			if (sy < 0 || sy >= s_hdr.gridH) continue;
			for (int sx = cx - ring; sx <= cx + ring; sx++) {
				if (sx < 0 || sx >= s_hdr.gridW || !inWindow(sx, sy) ||
				    (iabs(sx - cx) != ring && iabs(sy - cy) != ring)) continue;
				bool have = false;
				for (const Sector& sc : s_sectors)
					if (sc.used && sc.sx == sx && sc.sy == sy) { have = true; break; }
				if (have) continue;
				if (maxLoads > 0 && loadSector(sx, sy)) maxLoads--;
				else {
					missing = true;
					if (ring <= 1) nearMissing = true;
				}
			}
		}
	}
	s_windowDirty = missing;
	s_nearDirty = nearMissing;
	int n = 0, ni = 0;
	for (const Sector& sc : s_sectors)
		if (sc.used) { n++; ni += sc.count; }
	s_stats.sectors = (uint16_t)n;
	s_stats.instances = (uint16_t)ni;
}

// ----------------------------------------------------------------------------
// Scan: distance / time / frustum tests over the sector window (CRenderer::
// ScanWorld + SetupEntityVisibility, reduced). Fills the visible list and the
// nearest not-yet-loaded models.
// ----------------------------------------------------------------------------
struct Cam {
	int32_t eye[3];               // 1/16 m
	int16_t f[3], r[3], u[3];     // SA camera basis, 4.12
	bool frustum;
	int32_t view;                 // 1/16 m
	int hour;
};

// Instances drawn last frame (open addressing on the Inst pointer, rebuilt
// every draw): budget hysteresis per instance, not per model.
static constexpr int DRAWN_HASH = 1024;
static_assert(DRAWN_HASH >= 2 * WORLD_VISIBLE_MAX);
static const Inst* s_drawn[2][DRAWN_HASH];
static int s_drawnCur;
static constexpr int32_t NEW_MARGIN = 192;   // polys a newcomer needs on top of its estimate

static inline uint32_t drawnHash(const Inst* p) { return ((uint32_t)p >> 5) * 2654435761u >> 22; }

// scan() runs before draw() flips the tables: s_drawnCur still holds last frame
static bool wasDrawn(const Inst* p) {
	const Inst* const* t = s_drawn[s_drawnCur];
	for (uint32_t h = drawnHash(p);; h = (h + 1) & (DRAWN_HASH - 1)) {
		if (t[h] == p) return true;
		if (!t[h]) return false;
	}
}

static void markDrawn(const Inst* p) {
	const Inst** t = s_drawn[s_drawnCur];
	uint32_t h = drawnHash(p);
	while (t[h] && t[h] != p) h = (h + 1) & (DRAWN_HASH - 1);
	t[h] = p;
}

// HD instances with a LOD drawn this frame, keyed by (model, position): a LOD
// asks whether its own HD drew in O(1). (The old linear search over every
// visible instance for each LOD cost ~2-3 ms per frame with ~200 candidates.)
struct HdDrawn { int32_t pos[3]; uint16_t model, gen; };
static constexpr int HD_DRAWN = 512;
static HdDrawn s_hdDrawn[HD_DRAWN];
static uint16_t s_hdGen = 1;

static inline uint32_t hdHash(uint16_t model, const int32_t* p) {
	return ((uint32_t)p[0] * 73856093u ^ (uint32_t)p[1] * 19349663u ^ (uint32_t)p[2] * 83492791u ^
	        model * 2654435761u) >> 23;
}

static void markHdDrawn(const Inst& in) {
	uint32_t h = hdHash(in.model, in.pos);
	for (int n = 0; n < HD_DRAWN; n++, h = (h + 1) & (HD_DRAWN - 1)) {
		HdDrawn& e = s_hdDrawn[h];
		if (e.gen == s_hdGen && (e.model != in.model || memcmp(e.pos, in.pos, sizeof e.pos))) continue;
		e.gen = s_hdGen;
		e.model = in.model;
		memcpy(e.pos, in.pos, sizeof e.pos);
		return;
	}
}

static bool hdDrawnFor(const Inst& lod) {
	uint32_t h = hdHash(lod.linkedModel, lod.pos);
	for (int n = 0; n < HD_DRAWN; n++, h = (h + 1) & (HD_DRAWN - 1)) {
		const HdDrawn& e = s_hdDrawn[h];
		if (e.gen != s_hdGen) return false;
		if (e.model == lod.linkedModel && !memcmp(e.pos, lod.pos, sizeof e.pos)) return true;
	}
	return false;
}

static void addWant(uint16_t model, int32_t key) {
	for (int i = 0; i < s_numWant; i++)
		if (s_want[i].model == model) {
			if (key >= s_want[i].key) return;
			while (i > 0 && s_want[i - 1].key > key) {
				s_want[i] = s_want[i - 1];
				i--;
			}
			s_want[i] = { model, key };
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

// Sphere vs the view frustum, camera-relative 1/16 m. tan(hfov) 0.7698 /
// 0.5774 and sec 1.262 / 1.155 for the 60 deg 4:3 projection (4.12).
static inline bool inFrustum(const Cam& c, int32_t dx, int32_t dy, int32_t dz, int32_t rad) {
	const int32_t z = (dx * c.f[0] + dy * c.f[1] + dz * c.f[2]) >> 12;
	if (z <= -rad) return false;
	const int32_t x = (dx * c.r[0] + dy * c.r[1] + dz * c.r[2]) >> 12;
	if (iabs(x) > ((z * 3153) >> 12) + ((rad * 5170) >> 12)) return false;
	const int32_t y = (dx * c.u[0] + dy * c.u[1] + dz * c.u[2]) >> 12;
	return iabs(y) <= ((z * 2365) >> 12) + ((rad * 4730) >> 12);
}

static void scan(const Cam& c) {
	s_numVis = 0;
	s_numWant = 0;
	uint16_t candidates = 0, culled = 0;
	uint16_t hiddenWires = 0, hiddenClutter = 0, hiddenPoles = 0;
	const uint8_t hidden = detailFilter();
	for (const Sector& sc : s_sectors) {
		if (!sc.used || !sc.count) continue;
		{   // whole sector out of range / behind the camera (and not close)
			const int32_t dx = sc.c[0] - c.eye[0], dy = sc.c[1] - c.eye[1], dz = sc.c[2] - c.eye[2];
			if (iabs(dx) > 0x4000 || iabs(dy) > 0x4000 || iabs(dz) > 0x4000) continue;
			const int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
			const int64_t range = c.view + sc.reach, close = 40 * 16 + sc.reach;
			if (d2 > range * range) continue;
			if (c.frustum && d2 > close * close && !inFrustum(c, dx, dy, dz, sc.reach)) continue;
		}
		for (int i = 0; i < sc.count; i++) {
			const Inst& in = sc.inst[i];
			const ModelLite& ml = s_models[in.model];
			const uint8_t skip = ml.detail & hidden;
			if (skip) {
				hiddenWires += (skip & DETAIL_WIRES) != 0;
				hiddenClutter += (skip & DETAIL_CLUTTER) != 0;
				hiddenPoles += (skip & DETAIL_POLES) != 0;
				continue;
			}
			if (ml.on != ml.off) {
				const bool vis = ml.on < ml.off ? c.hour >= ml.on && c.hour < ml.off : c.hour >= ml.on || c.hour < ml.off;
				if (!vis) continue;
			}
			const int32_t dx = (in.pos[0] >> 8) - c.eye[0], dy = (in.pos[1] >> 8) - c.eye[1],
			              dz = (in.pos[2] >> 8) - c.eye[2];
			const int32_t rad = ml.radius * 16;
			const int32_t reach = c.view + rad;
			if (iabs(dx) > reach || iabs(dy) > reach || iabs(dz) > reach || reach > 0x6000) continue;   // d^2 fits s32
			// HD/LOD counterparts switch when both are ready; keep the other
			// visible during streaming, and respect the IDE draw distance
			const int32_t d2 = dx * dx + dy * dy + dz * dz;
			const int32_t sw = (in.lodSwitch & 0x7FFF) * 16;
			if (sw && in.linkedModel != NONE && in.linkedModel < s_hdr.numModels) {
				const uint16_t linked = s_slotOf[in.linkedModel];
				if (!(in.lodSwitch & 0x8000) && d2 >= sw * sw &&
				    (linked == NONE || s_slots[linked].state == SLOT_READY)) continue;
			}
			int32_t dd = ml.drawDist * 16 + rad;
			if (!(in.lodSwitch & 0x8000) && sw && in.linkedModel < s_hdr.numModels &&
			    dd < sw + rad) dd = sw + rad;
			if (dd > 0x6000) dd = 0x6000;
			if (d2 > reach * reach || d2 > dd * dd) continue;
			candidates++;
			const uint16_t si = s_slotOf[in.model];
			bool inView = !c.frustum || inFrustum(c, dx, dy, dz, rad);
			int32_t visDx = dx, visDy = dy, visDz = dz, visRad = rad;
			if (inView && c.frustum && si != NONE && s_slots[si].state == SLOT_READY &&
			    rad > s_slots[si].boundRadius + 8 * 16) {
				const Slot& s = s_slots[si];
				int64_t px = 0, py = 0, pz = 0;
				for (int k = 0; k < 3; k++) {
					px += (int64_t)s.boundCenter[k] * in.m[k * 3];
					py -= (int64_t)s.boundCenter[k] * in.m[k * 3 + 2];
					pz += (int64_t)s.boundCenter[k] * in.m[k * 3 + 1];
				}
				visDx += (int32_t)(px >> 20);
				visDy += (int32_t)(py >> 20);
				visDz += (int32_t)(pz >> 20);
				visRad = s.boundRadius;
				inView = inFrustum(c, visDx, visDy, visDz, visRad);
			}
			if (!inView && si != NONE) {
				// protected from LRU eviction while close enough to turn around to
				if (d2 < (40 * 16 + rad) * (40 * 16 + rad)) s_slots[si].lastUse = s_frame;
				culled++;
				continue;
			}
			const int64_t visD2 = (int64_t)visDx * visDx + (int64_t)visDy * visDy + (int64_t)visDz * visDz;
			const int32_t d = visD2 > INT_MAX ? c.view + visRad : (int32_t)sqrt32((uint32_t)visD2);
			const int32_t near = d - visRad > 0 ? d - visRad : 0;
			if (si == NONE) {
				int32_t want = near;
				if ((in.lodSwitch & 0x8000) && in.linkedModel < s_hdr.numModels && d2 < sw * sw &&
				    (s_slotOf[in.linkedModel] == NONE ||
				     (s_slots[s_slotOf[in.linkedModel]].state != SLOT_READY &&
				      s_slots[s_slotOf[in.linkedModel]].state != SLOT_FAILED))) {
					const ModelLite& hd = s_models[in.linkedModel];
					const int32_t hdDistance = (hd.drawDist * 16 > sw ? hd.drawDist * 16 : sw) + hd.radius * 16;
					const int32_t hdReach = hdDistance < 0x6000 ? hdDistance : 0x6000;
					if (d2 < hdReach * hdReach) want += 48 * 16;
				}
				if (inView) addWant(in.model, want);
				continue;
			}
			Slot& s = s_slots[si];
			if (s.state == SLOT_FAILED && s.readSize && inView && s_frame - s.lastUse >= 120) {
				if (s.readDone <= mem::stats(mem::REGION_STREAM).largestFree) {
					freeSlot(s);
					addWant(in.model, near);
					continue;
				}
				s.lastUse = s_frame;
			}
			if (near < 64 * 16 && s.state != SLOT_FAILED) s.lastUse = s_frame;
			if ((s.state == SLOT_READING || s.state == SLOT_LOADING) && near < s.priority) s.priority = near;
			if (s.state != SLOT_READY || !c.frustum) continue;
			// Surface distance keeps wide ground and nearby houses ahead of
			// huge models behind them. The distance bands keep the foreground
			// ahead even when a distant object was drawn in the last frame.
			const uint8_t was = wasDrawn(&in);
			if (was) s.lastUse = s_frame;
			const int tier = near < (was ? 40 : 32) * 16 ? 3 :
			                 near < (was ? 88 : 80) * 16 ? 2 :
			                 near < (was ? 128 : 120) * 16 ? 1 : 0;
			const int32_t apparent = (visRad << 10) / (near + 64);
			const int32_t distance = near < c.view ? c.view - near : 0;
			const int32_t key = tier * 16384 + (was ? 1024 : 0) +
			                    ((in.lodSwitch & 0x8000) ? 0 : 256) +
			                    distance * 3 + (apparent < 1024 ? apparent : 1023);
			if (s_numVis < WORLD_VISIBLE_MAX) {
				s_vis[s_numVis++] = { &in, si, was, 0, key };
			} else {
				int smallest = 0;
				for (int j = 1; j < s_numVis; j++)
					if (s_vis[j].key < s_vis[smallest].key) smallest = j;
				if (key > s_vis[smallest].key) s_vis[smallest] = { &in, si, was, 0, key };
			}
		}
	}
	s_stats.candidates = candidates;
	s_stats.culledFrustum = culled;
	s_stats.hiddenWires = hiddenWires;
	s_stats.hiddenClutter = hiddenClutter;
	s_stats.hiddenPoles = hiddenPoles;
}

static Cam makeCam(float ex, float ey, float ez, float yaw, float pitch, bool frustum) {
	Cam c;
	c.eye[0] = (int32_t)(ex * 16.0f);
	c.eye[1] = (int32_t)(ey * 16.0f);
	c.eye[2] = (int32_t)(ez * 16.0f);
	// sky::draw basis converted to SA axes: GL (x, y, z) -> SA (x, -z, y)
	// libnds angles: 32768 = full circle
	const s16 ay = (s16)((int)(yaw * 5215.19f) & 0x7FFF), ap = (s16)((int)(pitch * 5215.19f) & 0x7FFF);
	const s32 sy = sinLerp(ay), cy = cosLerp(ay), sp = sinLerp(ap), cp = cosLerp(ap);
	c.f[0] = (int16_t)(-(cp * sy) >> 12); c.f[1] = (int16_t)((cp * cy) >> 12); c.f[2] = (int16_t)-sp;
	c.r[0] = (int16_t)cy;                 c.r[1] = (int16_t)sy;                c.r[2] = 0;
	c.u[0] = (int16_t)(-(sp * sy) >> 12); c.u[1] = (int16_t)((sp * cy) >> 12); c.u[2] = (int16_t)cp;
	c.frustum = frustum;
	c.view = (dbg::opts.fogView + 10) * 16;
	c.hour = (int)(player::minutesToday() / 60.0f);
	return c;
}

// ----------------------------------------------------------------------------
// Streaming step: finish loads whose textures arrived, then start the nearest
// wanted models, until the time budget is spent.
// ----------------------------------------------------------------------------
static void stream(u32 budgetUs, int maxStarts) {
	const u32 t0 = timing::ticks();
	bool worked = false;
	for (int n = 0; n < 2 && timing::usSince(t0) < budgetUs; n++) {
		int parse = -1;
		for (int i = 0; i < WORLD_MODEL_SLOTS; i++)
			if (s_slots[i].state == SLOT_READING && s_slots[i].readDone == s_slots[i].readSize &&
			    (parse < 0 || s_slots[i].priority < s_slots[parse].priority)) parse = i;
		if (parse < 0) break;
#ifdef GTASADS_DEBUG
		const u32 parseT0 = timing::ticks();
#endif
		finishRead(s_slots[parse]);
#ifdef GTASADS_DEBUG
		s_stats.modelParseUs += timing::usSince(parseT0);
#endif
		worked = true;
	}
	for (int reads = 0; reads < 2 && timing::usSince(t0) < budgetUs; reads++) {
		int best = -1;
		for (int i = 0; i < WORLD_MODEL_SLOTS; i++)
			if (s_slots[i].state == SLOT_READING && s_slots[i].readDone < s_slots[i].readSize &&
			    (best < 0 || s_slots[i].priority < s_slots[best].priority)) best = i;
		if (best < 0) break;
		Slot& s = s_slots[best];
		const u32 left = s.readSize - s.readDone;
		const u32 bytes = left < MODEL_READ_BYTES ? left : MODEL_READ_BYTES;
		Pack& pack = s.interior ? streaming::g_interiors : streaming::g_models;
#ifdef GTASADS_DEBUG
		const u32 readT0 = timing::ticks();
#endif
		const bool readOk = pack.read(s.readOffset + s.readDone, (uint8_t*)s.blob + s.readDone, bytes);
#ifdef GTASADS_DEBUG
		s_stats.modelReadUs += timing::usSince(readT0);
		s_stats.modelSeekUs += pack.lastSeekUs();
		s_stats.modelDataUs += pack.lastDataUs();
#endif
		if (!readOk) {
			mem::free(s.blob);
			s.blob = nullptr;
			s.state = SLOT_FAILED;
			continue;
		}
		s.readDone += bytes;
		worked = true;
		if (s.readDone == s.readSize && timing::usSince(t0) < budgetUs) {
#ifdef GTASADS_DEBUG
			const u32 parseT0 = timing::ticks();
#endif
			finishRead(s);
#ifdef GTASADS_DEBUG
			s_stats.modelParseUs += timing::usSince(parseT0);
#endif
		}
	}
	u32 staged = 0;
	for (int i = 0; i < WORLD_TEX_ENTRIES; i++)
		if (s_tex[i].state == TEX_STAGED) staged += s_tex[i].size;
	for (int reads = 0; reads < 2 && timing::usSince(t0) < budgetUs; reads++) {
		if (staged >= 32 * 1024) break;
		int best = -1, priority = 0x7FFFFFFF;
		for (int i = 0; i < WORLD_MODEL_SLOTS; i++) {
			const Slot& s = s_slots[i];
			if (s.state != SLOT_LOADING) continue;
			for (int k = 0; k < s.numTex; k++)
				if (s.tex[k] != NONE && s_tex[s.tex[k]].state == TEX_QUEUED && s.priority < priority) {
					priority = s.priority;
					best = s.tex[k];
				}
		}
		if (best < 0) break;
		TexEntry& t = s_tex[best];
#ifdef GTASADS_DEBUG
		const u32 readT0 = timing::ticks();
#endif
		void* buf = mem::alloc(t.size, mem::TAG_TEXTURE);
		if (!buf) break;
		Pack& pack = s_txds[t.txd].pad ? streaming::g_interiors : streaming::g_models;
		const bool readOk = pack.read(t.offset, buf, t.size);
#ifdef GTASADS_DEBUG
		s_stats.texReadUs += timing::usSince(readT0);
		s_stats.texSeekUs += pack.lastSeekUs();
		s_stats.texDataUs += pack.lastDataUs();
#endif
		if (readOk &&
		    t.size >= sizeof(ds::TexHeader) && ((const ds::TexHeader*)buf)->magic == DS_FOURCC('D', 'T', 'E', 'X')) {
			t.staging = buf;
			t.state = TEX_STAGED;
			t.lastUse = s_frame;
			staged += t.size;
		} else {
			mem::free(buf);
			t.state = TEX_FAILED;
		}
		worked = true;
	}
	int pendingLoads = 0;
	for (int i = 0; i < WORLD_MODEL_SLOTS; i++) {
		if (s_slots[i].state == SLOT_LOADING && timing::usSince(t0) < budgetUs) worked |= finishLoad(s_slots[i]);
		pendingLoads += s_slots[i].state == SLOT_LOADING || s_slots[i].state == SLOT_READING;
	}
	for (int i = 0; i < s_numWant && maxStarts > 0 && pendingLoads < MAX_LOADING && timing::usSince(t0) < budgetUs; i++) {
		if (s_slotOf[s_want[i].model] != NONE) continue;
#ifdef GTASADS_DEBUG
		const u32 startT0 = timing::ticks();
#endif
		const bool started = startLoad(s_want[i].model, s_want[i].key);
#ifdef GTASADS_DEBUG
		s_stats.startUs += timing::usSince(startT0);
#endif
		if (!started) continue;
		maxStarts--;
		pendingLoads++;
		worked = true;
	}
	s_numWant = 0;
	if (!worked && (s_frame & 15)) return;   // stats only when something changed
	uint16_t ready = 0, loading = 0, texN = 0, texF = 0;
	for (int i = 0; i < WORLD_MODEL_SLOTS; i++) {
		ready += s_slots[i].state == SLOT_READY;
		loading += s_slots[i].state == SLOT_LOADING || s_slots[i].state == SLOT_READING;
	}
	for (int i = 0; i < WORLD_TEX_ENTRIES; i++) {
		texN += s_tex[i].state == TEX_READY;
		texF += s_tex[i].state == TEX_FAILED;
	}
	s_stats.modelsReady = ready;
	s_stats.modelsLoading = loading;
	s_stats.textures = texN;
	s_stats.texFailed = texF;
	s_stats.listBytes = s_listBytes;
	s_stats.texVram = s_texVram;
}

void update(float x, float y) {
	if (!ready()) return;
	const u32 t0 = timing::ticks();
	s_frame++;
	const uint8_t hidden = detailFilter();
	if (hidden != s_lastDetailFilter) {
		s_lastDetailFilter = hidden;
		for (int i = 0; i < WORLD_MODEL_SLOTS; i++) {
			Slot& s = s_slots[i];
			if (s.state != SLOT_FREE && (s_models[s.model].detail & hidden)) freeSlot(s);
		}
	}
#ifdef GTASADS_DEBUG
	s_stats.sectorUs = s_stats.modelReadUs = s_stats.modelSeekUs = s_stats.modelDataUs =
		s_stats.modelParseUs = s_stats.texReadUs = s_stats.texSeekUs = s_stats.texDataUs =
		s_stats.compileUs = s_stats.startUs = 0;
	const u32 sectorT0 = timing::ticks();
#endif
	updateSectors(x, y, s_boost ? 4 : 2);
#ifdef GTASADS_DEBUG
	s_stats.sectorUs = timing::usSince(sectorT0);
#endif
	// after a suspend (pause menu) the visible set is rebuilt with a bigger
	// budget for a moment instead of trickling back in over ~10 s
	stream(s_boost ? UPDATE_US * 3 : UPDATE_US, 2);
	if (s_boost) s_boost--;
	s_stats.updateUs = timing::usSince(t0);
}

int area() { return s_area; }
void setArea(int area) {
	if (area == s_area || area < 0 || area > 18) return;
	s_door = nullptr;
	suspend();
	for (Sector& sc : s_sectors)
		if (sc.used) freeSector(sc);
	s_area = area;
	s_centerX = s_centerY = -9999;
	s_windowDirty = s_nearDirty = true;
	s_stats.drawn = s_stats.modelsReady = s_stats.modelsLoading = 0;
	memset(s_drawn, 0, sizeof s_drawn);
}

bool animateDoorNear(float x, float y, float z, float angle) {
	s_doorAngle = angle * s_doorSign;
	if (s_door) return true;
	float best = 64.0f;
	for (const Sector& sc : s_sectors)
		for (int i = 0; sc.used && i < sc.count; i++) {
			const Inst& in = sc.inst[i];
			const float dx = in.pos[0] * (1.0f / 4096.0f) - x;
			const float dy = in.pos[1] * (1.0f / 4096.0f) - y;
			const float dz = in.pos[2] * (1.0f / 4096.0f) - z;
			const float d = dx * dx + dy * dy + dz * dz;
			if (d >= best) continue;
			const int slot = s_slotOf[in.model];
			if (slot == NONE || s_slots[slot].state != SLOT_READY || !s_slots[slot].hingeDoor) continue;
			ds::WorldModel m;
			if (!readAt(s_hdr.modelsOffset + in.model * sizeof m, &m, sizeof m)) continue;
			bool door = false;
			for (int k = 0; k < 20 && m.name[k + 3]; k++)
				if ((m.name[k] | 32) == 'd' && (m.name[k + 1] | 32) == 'o' &&
				    (m.name[k + 2] | 32) == 'o' && (m.name[k + 3] | 32) == 'r') door = true;
			if (door) {
				best = d;
				s_door = &in;
				const s32* center = s_slots[slot].boundCenter;
				const int64_t cx = (int64_t)in.m[0] * center[0] + (int64_t)in.m[3] * center[1] + (int64_t)in.m[6] * center[2];
				const int64_t cy = (int64_t)in.m[2] * center[0] + (int64_t)in.m[5] * center[1] + (int64_t)in.m[8] * center[2];
				s_doorX = (in.pos[0] + (cx >> 12)) * (1.0f / 4096.0f);
				s_doorY = (in.pos[1] - (cy >> 12)) * (1.0f / 4096.0f);
				const float vx = s_doorX - in.pos[0] * (1.0f / 4096.0f);
				const float vy = s_doorY - in.pos[1] * (1.0f / 4096.0f);
				s_doorSign = ((x - s_doorX) * -vy + (y - s_doorY) * vx > 0) ? -1 : 1;
				s_doorAngle = angle * s_doorSign;
			}
		}
	return s_door != nullptr;
}

bool doorTarget(float& x, float& y) {
	if (!s_door) return false;
	x = s_doorX;
	y = s_doorY;
	return true;
}

void clearDoorAnimation() { s_door = nullptr; s_doorAngle = 0.0f; s_doorSign = 1; }

bool preloadAreaStep(float x, float y, float z) {
	if (!ready()) return false;
	s_frame++;
	updateSectors(x, y, 8);
	scan(makeCam(x, y, z + 1.5f, 0.0f, 0.0f, false));
	stream(PRELOAD_US, 4);
	int near = 0, loaded = 0;
	for (const Sector& sc : s_sectors)
		for (int i = 0; sc.used && i < sc.count; i++) {
			const Inst& in = sc.inst[i];
			const int32_t dx = (in.pos[0] >> 12) - (int)x;
			const int32_t dy = (in.pos[1] >> 12) - (int)y;
			const int32_t dz = (in.pos[2] >> 12) - (int)z;
			const int reach = 60 + (s_models[in.model].radius / 16 > 120 ? 120 : s_models[in.model].radius / 16);
			if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
			near++;
			const int slot = s_slotOf[in.model];
			if (slot != NONE && s_slots[slot].state == SLOT_READY) loaded++;
		}
	const int required = near < 4 ? near : near * 3 / 4 > 20 ? 20 : near * 3 / 4 > 4 ? near * 3 / 4 : 4;
	return !s_windowDirty && ((near > 0 && loaded >= required) ||
	                         (s_stats.modelsReady >= 8 && s_stats.drawn > 0));
}

// ----------------------------------------------------------------------------
// Init / preload
// ----------------------------------------------------------------------------
static bool open() {
	s_file = fopen("game:/data/world.dwld", "rb");
	if (!s_file) return false;
	if (fread(&s_hdr, sizeof s_hdr, 1, s_file) != 1 || s_hdr.magic != DS_FOURCC('D', 'W', 'L', 'D') ||
	    s_hdr.version != 6 || !s_hdr.numModels) {
		fclose(s_file);
		s_file = nullptr;
		return false;
	}
	s_models = (ModelLite*)mem::alloc(sizeof(ModelLite) * s_hdr.numModels, mem::TAG_WORLD);
	s_txds = (ds::WorldTxd*)mem::alloc(sizeof(ds::WorldTxd) * (s_hdr.numTxds ? s_hdr.numTxds : 1), mem::TAG_WORLD);
	s_slotOf = (uint16_t*)mem::alloc(2 * s_hdr.numModels, mem::TAG_WORLD);
	s_slots = (Slot*)mem::zalloc(sizeof(Slot) * WORLD_MODEL_SLOTS, mem::TAG_WORLD);
	s_tex = (TexEntry*)mem::zalloc(sizeof(TexEntry) * WORLD_TEX_ENTRIES, mem::TAG_WORLD);
	s_vis = (Vis*)mem::alloc(sizeof(Vis) * WORLD_VISIBLE_MAX, mem::TAG_WORLD);
	if (!s_models || !s_txds || !s_slotOf || !s_slots || !s_tex || !s_vis) return false;
	// model records stream in 1 KB chunks; only draw distance/radius/time stay
	{
		mem::Scratch scratch;
		constexpr int CHUNK = 32;
		ds::WorldModel* wm = (ds::WorldModel*)scratch.alloc(sizeof(ds::WorldModel) * CHUNK);
		if (!wm || fseek(s_file, (long)s_hdr.modelsOffset, SEEK_SET)) return false;
		for (int i = 0; i < s_hdr.numModels; i += CHUNK) {
			const int n = s_hdr.numModels - i < CHUNK ? s_hdr.numModels - i : CHUNK;
			if (fread(wm, sizeof *wm, n, s_file) != (size_t)n) return false;
			for (int k = 0; k < n; k++) {
				char name[sizeof wm[k].name + 1];
				memcpy(name, wm[k].name, sizeof wm[k].name);
				name[sizeof wm[k].name] = 0;
				s_models[i + k] = { wm[k].drawDist, wm[k].radius, wm[k].timeOn, wm[k].timeOff,
				                    0, detailKind(name) };
			}
		}
	}
	if (s_hdr.numTxds && fread(s_txds, sizeof(ds::WorldTxd), s_hdr.numTxds, s_file) != s_hdr.numTxds) return false;
	memset(s_slotOf, 0xFF, 2 * s_hdr.numModels);
	for (int i = 0; i < WORLD_MODEL_SLOTS; i++) s_slots[i].model = NONE;
	return true;
}

bool initStep() {
	if (s_preloadStep == 0) {
		s_preloadStep = 1;
		s_preloadT0 = timing::vblanks();
		if (!open()) {
			hud::log("\x1b[33mworld.dwld missing: placeholder ground\x1b[39m");
			s_preloadStep = 2;
			return true;
		}
		hud::log("world: %u models %u inst %ux%u sectors", s_hdr.numModels, (unsigned)s_hdr.numInstances,
		         s_hdr.gridW, s_hdr.gridH);
		return false;
	}
	if (s_preloadStep == 2 || !ready()) return true;
	if (s_preloadStep == 1) {
		s_preloadStep = 3;
		if (fs::initLookupCache(s_file, 8 * 1024) != 0)
			hud::log("world.dwld: seek cache unavailable");
		s_preloadT0 = timing::vblanks();
		return false;
	}
	// CStreaming::LoadScene: everything within the view around the player,
	// without frustum culling, before the first in-game frame
	const float px = player::g.x, py = player::g.y;
	s_frame++;
	updateSectors(px, py, 64);
	scan(makeCam(px, py, player::g.z + 2.0f, 0.0f, 0.0f, false));
	const bool idle = s_numWant == 0;
	stream(PRELOAD_US, 4);
	bool pending = false;
	for (int i = 0; i < WORLD_MODEL_SLOTS && !pending; i++)
		pending = s_slots[i].state == SLOT_LOADING || s_slots[i].state == SLOT_READING;
	for (int i = 0; i < WORLD_TEX_ENTRIES && !pending; i++) pending = s_tex[i].state == TEX_STAGED;
	if ((idle && !pending) || timing::vblanks() - s_preloadT0 > PRELOAD_MAX_VBLANKS) {
		hud::log("world preload: %u models, %u tex %uK, lists %uK", s_stats.modelsReady, s_stats.textures,
		         (unsigned)(s_texVram >> 10), (unsigned)(s_listBytes >> 10));
		s_preloadStep = 2;
		return true;
	}
	return false;
}

bool releaseRam(uint32_t bytes) {
	if (!ready()) return false;
	bool freed = false;
	while (mem::stats(mem::REGION_STREAM).largestFree < bytes + 64) {
		const int slot = lruSlot();   // never one drawn this or last frame
		if (slot < 0) break;
		freeSlot(s_slots[slot]);
		freed = true;
	}
	return freed;
}

void suspend() {
	if (!ready()) return;
	s_boost = 90;
	for (int i = 0; i < WORLD_MODEL_SLOTS; i++)
		if (s_slots[i].state != SLOT_FREE) freeSlot(s_slots[i]);
	for (int i = 0; i < WORLD_TEX_ENTRIES; i++)
		if (s_tex[i].state != TEX_FREE) freeTex(s_tex[i]);
	for (TxdDir& d : s_dirs) {
		mem::free(d.e);
		d = {};
	}
}

// ----------------------------------------------------------------------------
// Draw
// ----------------------------------------------------------------------------
void draw(const DrawView& v) {
	if (!ready() || !dbg::opts.world) return;
	const u32 t0 = timing::ticks();
	scan(makeCam(v.eyeX, v.eyeY, v.eyeZ, v.yaw, v.pitch, true));
	s_stats.scanUs = timing::usSince(t0);

	// Foreground first; apparent size only breaks ties within a distance band.
	std::sort(s_vis, s_vis + s_numVis, [](const Vis& a, const Vis& b) { return a.key > b.key; });
	s_drawnCur ^= 1;
	memset(s_drawn[s_drawnCur], 0, sizeof s_drawn[0]);
	if (++s_hdGen == 0) {   // wrapped: stale entries could match generation 1 again
		memset(s_hdDrawn, 0, sizeof s_hdDrawn);
		s_hdGen = 1;
	}

	const int32_t ox = (int32_t)(v.originX * 4096.0f), oy = (int32_t)(v.originY * 4096.0f),
	              oz = (int32_t)(v.originZ * 4096.0f);
	int polys = 0;
	glGetInt(GL_GET_POLYGON_RAM_COUNT, &polys);
	s_stats.polysBefore = (uint16_t)polys;
	const int budget = dbg::opts.worldPolys < 2048 - 32 - v.pedReserve ?
	                   dbg::opts.worldPolys : 2048 - 32 - v.pedReserve;
	uint16_t drawn = 0, dropped = 0;
	glStoreMatrix(VIEW_SLOT);
	for (int tier = 3; tier >= 0; tier--)
	for (int pass = 0; pass < 2; pass++)
	for (int i = 0; i < s_numVis; i++) {
		const Vis& vi = s_vis[i];
		if (vi.key / 16384 != tier) continue;
		const Inst& in = *vi.inst;
		const bool lod = (in.lodSwitch & 0x8000) != 0;
		if (lod != (pass == 1)) continue;
		if (lod && in.linkedModel != NONE && hdDrawnFor(in)) continue;
		Slot& s = s_slots[vi.slot];
		const uint16_t hd = lod && in.linkedModel < s_hdr.numModels ? s_slotOf[in.linkedModel] : NONE;
		const bool fallback = hd != NONE && s_slots[hd].state == SLOT_READY;
		// backface culling removes roughly half of a closed model; a model that
		// was not on screen last frame only comes in with room to spare, so the
		// set at the edge of the budget does not flip every frame
		if (polys + s.tris / 2 + (vi.wasDrawn ? 0 : fallback ? 96 : NEW_MARGIN) > budget ||
		    polys + s.tris > 2048 - 64) {
			dropped++;
			if (vi.wasDrawn) s_stats.budgetPops++;
			continue;
		}
		s.lastUse = s_frame;
		const int e = s.scaleExp;
		MATRIX_RESTORE = VIEW_SLOT;
		if (&in == s_door && s_doorAngle != 0.0f) {
			const int32_t cs = cosLerp((int)(s_doorAngle * (32768.0f / 360.0f)));
			const int32_t sn = sinLerp((int)(s_doorAngle * (32768.0f / 360.0f)));
			for (int k = 0; k < 3; k++) {
				const int32_t x = in.m[k * 3], z = in.m[k * 3 + 2];
				MATRIX_MULT4x3 = ((x * cs + z * sn) >> 12) * (1 << e);
				MATRIX_MULT4x3 = (s32)in.m[k * 3 + 1] * (1 << e);
				MATRIX_MULT4x3 = ((z * cs - x * sn) >> 12) * (1 << e);
			}
		} else {
			for (int k = 0; k < 9; k++) MATRIX_MULT4x3 = (s32)in.m[k] * (1 << e);
		}
		MATRIX_MULT4x3 = in.pos[0] - ox;
		MATRIX_MULT4x3 = in.pos[2] - oz;
		MATRIX_MULT4x3 = oy - in.pos[1];
		gx::callList(s.list);
		markDrawn(vi.inst);
		if (!lod && in.linkedModel != NONE) markHdDrawn(in);
		drawn++;
		glGetInt(GL_GET_POLYGON_RAM_COUNT, &polys);
	}
	MATRIX_RESTORE = VIEW_SLOT;
	// the lists wrote the texture registers behind libnds' back: leave both in
	// the "no texture" state glBindTexture expects
	GFX_TEX_FORMAT = 0;
	GFX_PAL_FORMAT = 0;
	glBindTexture(0, 0);
	s_stats.drawn = drawn;
	s_stats.culledBudget = dropped;
	s_stats.polysAfter = (uint16_t)polys;
	s_stats.drawUs = timing::usSince(t0);
}

}
