#include "game/world.h"

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
#include "platform/timing.h"
#include "render/gxlist.h"

namespace world {

using namespace mem::budget;

// ----------------------------------------------------------------------------
// Data
// ----------------------------------------------------------------------------
static constexpr uint16_t NONE = 0xFFFF;
static constexpr int MAX_MODEL_TEX = 12;     // unique textures per map model (rest draws untextured)
static constexpr int VIEW_SLOT = 13;         // matrix stack slot holding the view (CJ uses 14..30)
static constexpr int SECTOR_MARGIN = 40;     // m added to the view distance for the sector window
static constexpr int MAX_WANTS = 8;
static constexpr u32 UPDATE_US = 3000;       // streaming CPU per in-game frame
static constexpr u32 PRELOAD_US = 8000;      // per loading-screen slice
static constexpr u32 UPLOAD_BYTES = 24 * 1024; // texture bytes per vblank
static constexpr u32 PRELOAD_MAX_VBLANKS = 600;
// Constant normal for the lighting trick (see compile()): +y, 10.10.10
static constexpr u32 NORMAL_UP = 0x1FFu << 10;

struct ModelLite { uint16_t drawDist, radius; uint8_t on, off, pad[2]; };

enum : uint8_t { SLOT_FREE, SLOT_LOADING, SLOT_READY, SLOT_FAILED };
struct Slot {
	uint16_t model;
	uint8_t state, numTex;
	int8_t scaleExp;          // list vertices are v16 * 2^scaleExp
	uint8_t pad;
	uint16_t tris;
	u32* list;
	uint32_t listBytes;
	void* blob;               // DMDL while LOADING
	mdl::Model* view;         // parsed view while LOADING
	uint32_t lastUse;
	uint16_t tex[MAX_MODEL_TEX];
};

enum : uint8_t { TEX_FREE, TEX_STAGED, TEX_READY, TEX_FAILED };
struct TexEntry {
	uint32_t nameHash;
	uint16_t txd, refs;
	uint8_t state, hasPal, pad[2];
	tex::Texture* t;
	uint32_t param, pal;      // TEXIMAGE_PARAM (+ repeat) and PLTT_BASE for the lists
	void* staging;            // DTEX waiting for the vblank
	uint32_t lastUse;
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
static Want s_want[MAX_WANTS];
static int s_numWant;
static uint32_t s_frame;
static uint32_t s_listBytes, s_texVram;
static int s_centerX = -9999, s_centerY = -9999, s_windowR;
static bool s_windowDirty = true;
static int s_preloadStep;
static u32 s_preloadT0;
static Stats s_stats;

struct Vis { const Inst* inst; uint16_t slot; uint8_t bucket, pad; };
static Vis* s_vis;
static int s_numVis;

static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

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
int numModels() { return s_file ? s_hdr.numModels : 0; }

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
		if (t.state == TEX_STAGED || t.state == TEX_FAILED) freeTex(t);
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
	const int v = lruTex();
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
	if (!streaming::g_models.find(name, pe) || !streaming::g_models.read(pe.offset, &th, sizeof th) ||
	    th.magic != DS_FOURCC('D', 'T', 'X', 'D'))
		return nullptr;
	ds::TxdEntry* e = (ds::TxdEntry*)mem::alloc(sizeof(ds::TxdEntry) * (th.count ? th.count : 1), mem::TAG_WORLD);
	if (!e) return nullptr;
	if (!streaming::g_models.read(pe.offset + sizeof th, e, sizeof(ds::TxdEntry) * th.count)) {
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
			if (i < 0) return NONE;
			void* buf = mem::alloc(d->e[k].size, mem::TAG_TEXTURE);
			if (!buf || !streaming::g_models.read(d->base + d->e[k].offset, buf, d->e[k].size)) {
				mem::free(buf);
				return NONE;
			}
			TexEntry& te = s_tex[i];
			te = {};
			te.nameHash = h;
			te.txd = (uint16_t)t;
			te.refs = 1;
			te.state = TEX_STAGED;
			te.staging = buf;
			return (uint16_t)i;
		}
	}
	return NONE;
}

void vblank() {
	if (!s_tex) return;
	u32 spent = 0;
	for (int i = 0; i < WORLD_TEX_ENTRIES && spent < UPLOAD_BYTES; i++) {
		TexEntry& te = s_tex[i];
		if (te.state != TEX_STAGED) continue;
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
		if (h->numColors) glGetColorTableParameterEXT(0, GL_COLOR_TABLE_FORMAT_EXT, &pal);
		te.hasPal = pal >= 0;
		te.pal = pal < 0 ? 0 : (uint32_t)pal;
	}
	if (spent) glBindTexture(0, 0);
}

// ----------------------------------------------------------------------------
// Model slots (CStreaming::RequestModel / CModelInfo, reduced)
// ----------------------------------------------------------------------------
static void freeSlot(Slot& s) {
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

static bool startLoad(uint16_t model) {
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
	s_slotOf[model] = (uint16_t)si;
	s_stats.loads++;

	ds::WorldModel wm;
	if (fseek(s_file, (long)(s_hdr.modelsOffset + model * sizeof wm), SEEK_SET) || fread(&wm, sizeof wm, 1, s_file) != 1)
		return true;
	char name[32];
	snprintf(name, sizeof name, "%.24s.dff", wm.name);
	s.blob = streaming::g_models.load(name, mem::TAG_MODEL);
	s.view = mdl::load(s.blob);
	if (!s.view) {
		hud::log("\x1b[33mworld: %s missing\x1b[39m", name);
		mem::free(s.blob);
		s.blob = nullptr;
		return true;
	}
	for (int g = 0; g < s.view->numGeoms; g++) {
		const mdl::Geometry& geo = s.view->geoms[g];
		for (int mi = 0; mi < geo.h->numMaterials; mi++) {
			const char* tn = geo.mats[mi].texture;
			if (!tn[0] || wm.txd == NONE || s.numTex >= MAX_MODEL_TEX) continue;
			const uint32_t h = ds::fnv1a(tn);
			bool dup = false;
			for (int k = 0; k < s.numTex && !dup; k++) dup = s.tex[k] != NONE && s_tex[s.tex[k]].nameHash == h;
			if (!dup) s.tex[s.numTex++] = requestTex(wm.txd, tn);
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

// DMDL -> one packed GX list for the whole model. World lighting without a
// per-vertex CPU pass: each vertex colour (prelit * material colour) is sent
// as the AMBIENT material colour followed by a constant NORMAL; with diffuse,
// specular and emission at 0 the GE outputs ambient * light0 colour, and
// light0 = timecycle world light is set once per frame. DIF_AMB + NORMAL are
// only emitted when the colour changes. Texture registers are baked in
// (repeat on), so the textures must stay put while the list lives.
static bool compile(Slot& s) {
	const mdl::Model* m = s.view;
	s32 maxAbs = 1;
	uint32_t numIdx = 0, numMats = 0;
	for (int g = 0; g < m->numGeoms; g++) {
		const mdl::Geometry& geo = m->geoms[g];
		numIdx += geo.h->numIndices;
		numMats += geo.h->numMaterials;
		for (int v = 0; v < geo.h->numVerts; v++)
			for (int k = 0; k < 3; k++) {
				const s32 a = iabs(posAt(geo, v, k));
				if (a > maxAbs) maxAbs = a;
			}
	}
	int e = 0;
	while ((maxAbs >> e) > 32767) e++;
	const u32 cap = numIdx * 6 + numMats * 8 + 8;
	const u32 est = cap * 4;
	while (s_listBytes + est > WORLD_LIST_BYTES) {
		const int v = lruSlot();
		if (v < 0) return false;   // over budget with everything in view: retry later
		freeSlot(s_slots[v]);
	}
	u32* buf = (u32*)mem::alloc(est, mem::TAG_MODEL);
	if (!buf) return false;
	gx::ListBuilder dl(buf, cap);
	dl.cmd(gx::CMD_SPE_EMI, 0);
	uint32_t tris = 0;
	for (int g = 0; g < m->numGeoms; g++) {
		const mdl::Geometry& geo = m->geoms[g];
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
					const s32 x = posAt(geo, v, 0) >> e, y = posAt(geo, v, 1) >> e, z = posAt(geo, v, 2) >> e;
					dl.cmd(gx::CMD_VTX_16, (u32)(u16)(s16)x | ((u32)(u16)(s16)y << 16), (u32)(u16)(s16)z);
				}
				tris++;
			}
		}
	}
	const u32 n = dl.finish();
	if (!n) {
		mem::free(buf);
		return false;
	}
	s.list = (u32*)mem::realloc(buf, n * 4, mem::TAG_MODEL);
	if (!s.list) s.list = buf;
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
		if (s.tex[k] != NONE && s_tex[s.tex[k]].state == TEX_STAGED) return false;
	if (!compile(s)) return false;
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
	if (ws.count) {
		inst = (Inst*)mem::alloc(sizeof(Inst) * ws.count, mem::TAG_WORLD);
		if (!inst) return false;
		if (fseek(s_file, (long)(s_hdr.instancesOffset + ws.first * sizeof(Inst)), SEEK_SET) ||
		    fread(inst, sizeof(Inst), ws.count, s_file) != ws.count) {
			mem::free(inst);
			return false;
		}
		for (int i = 0; i < ws.count; i++) {
			int16_t* r = inst[i].m;
			for (int row = 0; row < 3; row++) {      // SA (x,y,z) -> GL (x,z,-y)
				const int16_t y = r[row * 3 + 1], z = r[row * 3 + 2];
				r[row * 3 + 1] = z;
				r[row * 3 + 2] = (int16_t)-y;
			}
			if (inst[i].model >= s_hdr.numModels) inst[i].model = 0;
		}
	}
	*sc = { (int16_t)sx, (int16_t)sy, ws.count, 1, 0, inst, { 0, 0, 0 }, 0 };
	// sector sphere for the scan's early-out: centre = sector middle at the
	// mean height, reach = farthest instance surface
	const int size = s_hdr.sectorSize;
	int32_t zs = 0;
	for (int i = 0; i < ws.count; i++) zs += inst[i].pos[2] >> 8;
	sc->c[0] = (s_hdr.originX + sx * size + size / 2) * 16;
	sc->c[1] = (s_hdr.originY + sy * size + size / 2) * 16;
	sc->c[2] = ws.count ? zs / ws.count : 0;
	for (int i = 0; i < ws.count; i++) {
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
		for (Sector& sc : s_sectors)
			if (sc.used && !inWindow(sc.sx, sc.sy)) freeSector(sc);
		s_centerX = cx;
		s_centerY = cy;
		s_windowR = r;
		s_windowDirty = true;
	}
	if (!s_windowDirty) return;
	bool missing = false;
	for (int sy = cy - r; sy <= cy + r; sy++) {
		if (sy < 0 || sy >= s_hdr.gridH) continue;
		for (int sx = cx - r; sx <= cx + r; sx++) {
			if (sx < 0 || sx >= s_hdr.gridW || !inWindow(sx, sy)) continue;
			bool have = false;
			for (const Sector& sc : s_sectors)
				if (sc.used && sc.sx == sx && sc.sy == sy) { have = true; break; }
			if (have) continue;
			if (maxLoads > 0 && loadSector(sx, sy)) maxLoads--;
			else missing = true;
		}
	}
	s_windowDirty = missing;
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

static constexpr int BUCKETS = 8;
static const int32_t k_bucketEdge[BUCKETS - 1] = { 10 * 16, 20 * 16, 32 * 16, 48 * 16, 64 * 16, 88 * 16, 120 * 16 };

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
	for (const Sector& sc : s_sectors) {
		if (!sc.used || !sc.count) continue;
		{   // whole sector out of range / behind the camera (and not close)
			const int32_t dx = sc.c[0] - c.eye[0], dy = sc.c[1] - c.eye[1], dz = sc.c[2] - c.eye[2];
			if (iabs(dx) > 0x4000 || iabs(dy) > 0x4000 || iabs(dz) > 0x4000) continue;
			const int32_t d = (int32_t)sqrt32(dx * dx + dy * dy + dz * dz);
			if (d - sc.reach > c.view) continue;
			if (c.frustum && d - sc.reach > 40 * 16 && !inFrustum(c, dx, dy, dz, sc.reach)) continue;
		}
		for (int i = 0; i < sc.count; i++) {
			const Inst& in = sc.inst[i];
			const ModelLite& ml = s_models[in.model];
			if (ml.on != ml.off) {
				const bool vis = ml.on < ml.off ? c.hour >= ml.on && c.hour < ml.off : c.hour >= ml.on || c.hour < ml.off;
				if (!vis) continue;
			}
			const int32_t dx = (in.pos[0] >> 8) - c.eye[0], dy = (in.pos[1] >> 8) - c.eye[1],
			              dz = (in.pos[2] >> 8) - c.eye[2];
			const int32_t rad = ml.radius * 16;
			const int32_t reach = c.view + rad;
			if (iabs(dx) > reach || iabs(dy) > reach || iabs(dz) > reach || reach > 0x6000) continue;   // d^2 fits s32
			// no LODs yet: HD models stay up to the view distance (nearest
			// surface), small props still respect their IDE draw distance
			const int32_t d2 = dx * dx + dy * dy + dz * dz;
			int32_t dd = ml.drawDist * 16 + rad;
			if (dd > 0x6000) dd = 0x6000;
			if (d2 > reach * reach || d2 > dd * dd) continue;
			candidates++;
			const bool inView = !c.frustum || inFrustum(c, dx, dy, dz, rad);
			const uint16_t si = s_slotOf[in.model];
			if (!inView && si != NONE) {
				// protected from LRU eviction while close enough to turn around to
				if (d2 < (40 * 16 + rad) * (40 * 16 + rad)) s_slots[si].lastUse = s_frame;
				culled++;
				continue;
			}
			const int32_t d = (int32_t)sqrt32(d2);
			const int32_t near = d - rad > 0 ? d - rad : 0;
			if (si == NONE) {
				addWant(in.model, inView ? near : near * 2 + 32 * 16);
				continue;
			}
			Slot& s = s_slots[si];
			s.lastUse = s_frame;
			if (s.state != SLOT_READY || !c.frustum || s_numVis >= WORLD_VISIBLE_MAX) continue;
			uint8_t b = 0;
			while (b < BUCKETS - 1 && near > k_bucketEdge[b]) b++;
			s_vis[s_numVis++] = { &in, si, b, 0 };
		}
	}
	s_stats.candidates = candidates;
	s_stats.culledFrustum = culled;
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
	for (int i = 0; i < WORLD_MODEL_SLOTS; i++) {
		if (s_slots[i].state != SLOT_LOADING) continue;
		worked |= finishLoad(s_slots[i]);
		if (timing::usSince(t0) >= budgetUs) break;
	}
	for (int i = 0; i < s_numWant && maxStarts > 0 && timing::usSince(t0) < budgetUs; i++) {
		if (s_slotOf[s_want[i].model] != NONE) continue;
		if (!startLoad(s_want[i].model)) break;
		maxStarts--;
		worked = true;
	}
	s_numWant = 0;
	if (!worked && (s_frame & 15)) return;   // stats only when something changed
	uint16_t ready = 0, loading = 0, texN = 0, texF = 0;
	for (int i = 0; i < WORLD_MODEL_SLOTS; i++) {
		ready += s_slots[i].state == SLOT_READY;
		loading += s_slots[i].state == SLOT_LOADING;
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
	updateSectors(x, y, 2);
	stream(UPDATE_US, 2);
	s_stats.updateUs = (uint16_t)timing::usSince(t0);
}

// ----------------------------------------------------------------------------
// Init / preload
// ----------------------------------------------------------------------------
static bool open() {
	s_file = fopen("nitro:/data/world.dwld", "rb");
	if (!s_file) return false;
	if (fread(&s_hdr, sizeof s_hdr, 1, s_file) != 1 || s_hdr.magic != DS_FOURCC('D', 'W', 'L', 'D') ||
	    s_hdr.version != 4 || !s_hdr.numModels) {
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
			for (int k = 0; k < n; k++)
				s_models[i + k] = { wm[k].drawDist, wm[k].radius, wm[k].timeOn, wm[k].timeOff, { 0, 0 } };
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
	// CStreaming::LoadScene: everything within the view around the player,
	// without frustum culling, before the first in-game frame
	const float px = player::g.x, py = player::g.y;
	s_frame++;
	updateSectors(px, py, 64);
	scan(makeCam(px, py, player::g.z + 2.0f, 0.0f, 0.0f, false));
	const bool idle = s_numWant == 0;
	stream(PRELOAD_US, 4);
	bool pending = false;
	for (int i = 0; i < WORLD_MODEL_SLOTS && !pending; i++) pending = s_slots[i].state == SLOT_LOADING;
	for (int i = 0; i < WORLD_TEX_ENTRIES && !pending; i++) pending = s_tex[i].state == TEX_STAGED;
	if ((idle && !pending) || timing::vblanks() - s_preloadT0 > PRELOAD_MAX_VBLANKS) {
		hud::log("world preload: %u models, %u tex %uK, lists %uK", s_stats.modelsReady, s_stats.textures,
		         (unsigned)(s_texVram >> 10), (unsigned)(s_listBytes >> 10));
		s_preloadStep = 2;
		return true;
	}
	return false;
}

void suspend() {
	if (!ready()) return;
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
	s_stats.scanUs = (uint16_t)timing::usSince(t0);

	// near -> far by bucket (the polygon budget drops the far ones)
	int count[BUCKETS] = {}, start[BUCKETS];
	for (int i = 0; i < s_numVis; i++) count[s_vis[i].bucket]++;
	for (int b = 0, acc = 0; b < BUCKETS; b++) {
		start[b] = acc;
		acc += count[b];
	}
	mem::Scratch scratch;
	const Vis** order = (const Vis**)scratch.alloc(sizeof(Vis*) * (s_numVis ? s_numVis : 1));
	if (!order) return;
	for (int i = 0; i < s_numVis; i++) order[start[s_vis[i].bucket]++] = &s_vis[i];

	const int32_t ox = (int32_t)(v.originX * 4096.0f), oy = (int32_t)(v.originY * 4096.0f),
	              oz = (int32_t)(v.originZ * 4096.0f);
	int polys = 0;
	glGetInt(GL_GET_POLYGON_RAM_COUNT, &polys);
	s_stats.polysBefore = (uint16_t)polys;
	const int budget = dbg::opts.worldPolys;
	uint16_t drawn = 0, dropped = 0;
	glStoreMatrix(VIEW_SLOT);
	for (int i = 0; i < s_numVis; i++) {
		const Vis& vi = *order[i];
		Slot& s = s_slots[vi.slot];
		s.lastUse = s_frame;
		// backface culling removes roughly half of a closed model
		if (polys + s.tris / 2 > budget) {
			dropped++;
			continue;
		}
		const Inst& in = *vi.inst;
		const int e = s.scaleExp;
		MATRIX_RESTORE = VIEW_SLOT;
		for (int k = 0; k < 9; k++) MATRIX_MULT4x3 = (s32)in.m[k] * (1 << e);
		MATRIX_MULT4x3 = in.pos[0] - ox;
		MATRIX_MULT4x3 = in.pos[2] - oz;
		MATRIX_MULT4x3 = oy - in.pos[1];
		gx::callList(s.list);
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
	s_stats.drawUs = (uint16_t)timing::usSince(t0);
}

}
