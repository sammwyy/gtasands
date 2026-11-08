#include "game/pedmodel.h"

#include <nds.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "assets/texture.h"
#include "game/anim.h"
#include "game/animstore.h"
#include "game/peddata.h"
#include "game/streaming.h"
#include "game/world.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/video.h"
#include "render/gxlist.h"

namespace pedmodel {

static constexpr int TEX_SLOTS = MAX * MAX_TEX;
static constexpr u32 UPLOAD_BYTES = 8 * 1024;   // per vblank, after the map's and the vehicles' share
static constexpr uint8_t NONE = 0xFF;

enum : uint8_t { TEX_FREE, TEX_STAGED, TEX_READY, TEX_FAILED };
struct TexEntry {
	uint32_t nameHash;
	uint8_t state, hasPal;
	tex::Texture* t;
	u32 param, pal;
	void* staging;
};

static Model s_models[MAX];
static TexEntry s_tex[TEX_SLOTS];
static uint32_t s_frame;
static Stats s_stats;
static u16 s_light = 0x7FFF;

const Stats& stats() { return s_stats; }
Model* get(int h) { return (unsigned)h < (unsigned)MAX && s_models[h].state != STATE_FREE ? &s_models[h] : nullptr; }
bool ready(int h) { const Model* m = get(h); return m && m->state == STATE_READY; }
void init() {}

int find(int pedIndex) {
	for (int i = 0; i < MAX; i++)
		if (s_models[i].state != STATE_FREE && s_models[i].state != STATE_FAILED && s_models[i].pedIndex == pedIndex) return i;
	return -1;
}

static inline u16 tint(u16 c, u16 t) {
	auto ch = [&](int sh) { return (u16)(((((c >> sh) & 31) * (((t >> sh) & 31) + 1)) >> 5) << sh); };
	return ch(0) | ch(5) | ch(10);
}

// ----------------------------------------------------------------------------
// Textures (the model's own TXD; a ped TXD holds one or two)
// ----------------------------------------------------------------------------
static void freeTex(uint8_t i) {
	if (i == NONE || i >= TEX_SLOTS) return;
	TexEntry& t = s_tex[i];
	if (t.t) tex::unload(t.t);
	mem::free(t.staging);
	t = {};
}

static uint8_t stageTex(const Pack& pack, const ds::TxdEntry* dir, int count, uint32_t base, const char* name) {
	int slot = -1;
	for (int i = 0; i < TEX_SLOTS && slot < 0; i++)
		if (s_tex[i].state == TEX_FREE) slot = i;
	if (slot < 0 || !count) return NONE;
	int k = 0;
	for (int j = 0; j < count; j++)
		if (!strcasecmp(dir[j].name, name)) { k = j; break; }   // else the first (one-texture TXDs)
	void* buf = mem::alloc(dir[k].size, mem::TAG_TEXTURE);
	if (!buf) return NONE;
	if (!pack.read(base + dir[k].offset, buf, dir[k].size) ||
	    ((const ds::TexHeader*)buf)->magic != DS_FOURCC('D', 'T', 'E', 'X')) {
		mem::free(buf);
		return NONE;
	}
	TexEntry& te = s_tex[slot];
	te = {};
	te.nameHash = ds::fnv1a(name);
	te.state = TEX_STAGED;
	te.staging = buf;
	return (uint8_t)slot;
}

void vblank() {
	u32 spent = 0;
	for (int i = 0; i < TEX_SLOTS && spent < UPLOAD_BYTES; i++) {
		TexEntry& te = s_tex[i];
		if (te.state != TEX_STAGED) continue;
		if (!video::textureUploadWindow()) break;
		const ds::TexHeader* h = (const ds::TexHeader*)te.staging;
		tex::Texture* t = tex::load(h, tex::GROUP_GAME);
		for (int n = 0; !t && n < 16 && world::evictTexture(); n++) t = tex::load(h, tex::GROUP_GAME);
		spent += h->texelBytes + h->numColors * 2u;
		const bool paletted = h->numColors != 0;
		mem::free(te.staging);
		te.staging = nullptr;
		if (!t) {
			te.state = TEX_FAILED;
			continue;
		}
		te.t = t;
		te.state = TEX_READY;
		glBindTexture(0, t->id);
		te.param = glGetTexParameter() | GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;   // RW UVs repeat
		int pal = -1;
		if (paletted) glGetColorTableParameterEXT(0, GL_COLOR_TABLE_FORMAT_EXT, &pal);
		te.hasPal = pal >= 0;
		te.pal = pal < 0 ? 0 : (uint32_t)pal;
	}
	if (spent) glBindTexture(0, 0);
}

// ----------------------------------------------------------------------------
// Load
// ----------------------------------------------------------------------------
static void freeModel(Model& m) {
	for (int i = 0; i < m.numSegs; i++) mem::free(m.segs[i].list);
	for (int k = 0; k < m.numTex; k++) freeTex(m.tex[k]);
	mem::free(m.skel);
	mdl::free(m.view);
	mem::free(m.blob);
	s_stats.listBytes -= m.listBytes;
	m = {};
}

enum LoadResult { LOAD_FAIL, LOAD_OK, LOAD_RETRY };

static LoadResult startLoad(Model& m) {
	s_stats.loads++;
	const Pack& pack = m.cutsceneSource ? streaming::g_cutscene : streaming::g_models;
	ds::PedName nm{};
	if (m.cutsceneName[0]) {
		snprintf(nm.model, sizeof nm.model, "%s", m.cutsceneName);
		snprintf(nm.txd, sizeof nm.txd, "%s", m.cutsceneName);
	} else if (!peddata::name(m.pedIndex, nm)) return LOAD_FAIL;
	char file[40];
	snprintf(file, sizeof file, "%.23s.dff", nm.model);
	ds::PackEntry de;
	if (!pack.find(file, de)) {
		hud::log("\x1b[33mped %s missing\x1b[39m", file);
		return LOAD_FAIL;
	}
	m.blob = pack.load(de, mem::TAG_MODEL);
	if (!m.blob && world::releaseRam(de.size)) m.blob = pack.load(de, mem::TAG_MODEL);
	m.view = m.blob ? mdl::load(m.blob) : nullptr;
	if (!m.view) return LOAD_RETRY;   // no STREAM block right now (not a broken model)
	if (!m.view->nodes || m.view->numBones > anim::MAX_NODES || !anim::prepare(m.view)) {
		hud::log("\x1b[33mped %s unusable\x1b[39m", file);
		return LOAD_FAIL;
	}
	snprintf(file, sizeof file, "%.23s.txd", nm.txd);
	ds::PackEntry pe;
	ds::TxdHeader th;
	if (!pack.find(file, pe) || !pack.read(pe.offset, &th, sizeof th) ||
	    th.magic != DS_FOURCC('D', 'T', 'X', 'D') || !th.count)
		return LOAD_OK;   // untextured
	mem::Scratch scratch;
	ds::TxdEntry* dir = (ds::TxdEntry*)scratch.alloc(sizeof(ds::TxdEntry) * th.count);
	if (!dir || !pack.read(pe.offset + sizeof th, dir, sizeof(ds::TxdEntry) * th.count)) return LOAD_OK;
	for (int g = 0; g < m.view->numGeoms; g++) {
		const mdl::Geometry& geo = m.view->geoms[g];
		for (int mi = 0; mi < geo.h->numMaterials; mi++) {
			const char* tn = geo.mats[mi].texture;
			if (!tn[0] || m.numTex >= MAX_TEX) continue;
			const uint32_t h = ds::fnv1a(tn);
			bool dup = false;
			for (int k = 0; k < m.numTex && !dup; k++) dup = m.tex[k] != NONE && s_tex[m.tex[k]].nameHash == h;
			if (!dup) m.tex[m.numTex++] = stageTex(pack, dir, th.count, pe.offset, tn);
		}
	}
	return LOAD_OK;
}

static const TexEntry* texFor(const Model& m, const char* name) {
	if (!name[0]) return nullptr;
	const uint32_t h = ds::fnv1a(name);
	for (int k = 0; k < m.numTex; k++)
		if (m.tex[k] != NONE && s_tex[m.tex[k]].state == TEX_READY && s_tex[m.tex[k]].nameHash == h) return &s_tex[m.tex[k]];
	return nullptr;
}

// The skeleton the anim sampler and draw() need, copied out of the DMDL so
// the blob (geometry) can go: Model header + nodes + invBone + bindLocal +
// bonePos + bindQ in one block.
static mdl::Model* compactSkeleton(Model& owner, const mdl::Model* v) {
	const int nb = v->numBones;
	const size_t bytes = sizeof(mdl::Model) + nb * (sizeof(ds::ModelNode) + 48 + 48 + 12 + 16);
	uint8_t* p = (uint8_t*)mem::alloc(bytes, mem::TAG_MODEL);
	if (!p) return nullptr;
	mdl::Model* m = (mdl::Model*)p;
	*m = {};
	m->flags = v->flags;
	m->numBones = (uint16_t)nb;
	uint8_t* q = p + sizeof(mdl::Model);
	auto take = [&](const void* src, size_t n) { memcpy(q, src, n); void* r = q; q += n; return r; };
	m->invBone = (const ds::fx32*)take(v->invBone, nb * 48);
	m->bindLocal = (const ds::fx32*)take(v->bindLocal, nb * 48);
	m->bonePos = (const ds::fx32*)take(v->bonePos, nb * 12);
	m->bindQ = (int32_t*)take(v->bindQ, nb * 16);
	m->nodes = (const ds::ModelNode*)take(v->nodes, nb * sizeof(ds::ModelNode));
	owner.listBytes += (uint32_t)bytes;
	m->frames = nullptr;
	m->geoms = nullptr;
	return m;
}

static inline s32 posAt(const mdl::Geometry& g, int v, int k) {
	const int pe = g.h->posExp;
	return g.h->offset[k] + (pe >= 0 ? (s32)g.pos[v][k] * (1 << pe) : (s32)g.pos[v][k] >> -pe);
}

// invBone * world for IDLE_stance (bind pose when the clip is missing)
static void standingSkin(const mdl::Model* sk, s32* skin) {
	mem::Scratch scratch;
	s32* world = (s32*)scratch.alloc(sizeof(s32) * 12 * sk->numBones);
	anim::Local* loc = (anim::Local*)scratch.alloc(sizeof(anim::Local) * sk->numBones);
	anim::Player* pl = (anim::Player*)scratch.alloc(sizeof(anim::Player));
	if (!world || !loc || !pl) return;
	const int block = animstore::findBlock("ped");
	animstore::Clip* c = animstore::acquire(block, "IDLE_stance");
	if (c && anim::play(*pl, sk, animstore::entry(c))) {
		anim::sampleLocal(sk, *pl, loc);
		const anim::Local* ptr = loc;
		const s32 w = 4096;
		anim::blendWorld(sk, &ptr, &w, 1, world);
	} else {
		anim::evalBind(sk, world);
	}
	animstore::release(c);
	anim::skinForPart(sk, world, skin);
}

// Segments: triangles in material order (sorted by bone inside a material so
// limbs stay together), a new segment whenever the next triangle would need
// more than SEG_BONES bone slots.
static bool compile(Model& m) {
	const mdl::Model* v = m.view;
	uint32_t maxIdx = 0;
	for (int g = 0; g < v->numGeoms; g++)
		if (v->geoms[g].h->numIndices > maxIdx) maxIdx = v->geoms[g].h->numIndices;
	const u32 cap = maxIdx * 6 + 96;
	mem::Scratch scratch;
	u32* buf = (u32*)scratch.alloc(cap * 4);
	uint16_t* order = (uint16_t*)scratch.alloc(sizeof(uint16_t) * (maxIdx / 3 + 1));
	// the feet come from the skinned mesh in IDLE_stance (the ped anims put the
	// pelvis at the origin; a ped DFF's bind pose is neither upright nor
	// pelvis-relative like CJ's parts, its raw vertices sank the legs)
	s32* skin = (s32*)scratch.alloc(sizeof(s32) * 12 * v->numBones);
	if (!buf || !order || !skin) return false;
	standingSkin(m.skel, skin);
	int8_t slotOf[anim::MAX_NODES];
	gx::ListBuilder* dl = nullptr;
	alignas(gx::ListBuilder) uint8_t dlMem[sizeof(gx::ListBuilder)];
	Segment* seg = nullptr;
	int lastSlot = -1;
	s32 minZ = 0;
	bool ok = true;

	auto closeSeg = [&]() {
		if (!seg) return;
		const u32 n = dl->finish();
		seg->list = n ? (u32*)mem::alloc(n * 4, mem::TAG_MODEL) : nullptr;
		if (!seg->list) { ok = false; return; }
		memcpy(seg->list, buf, n * 4);
		gx::flushList(seg->list);
		m.listBytes += n * 4;
		m.numSegs++;
		seg = nullptr;
	};
	auto openSeg = [&]() {
		if (m.numSegs >= MAX_SEGS) { ok = false; return; }
		seg = &m.segs[m.numSegs];
		*seg = {};
		memset(slotOf, -1, sizeof slotOf);
		dl = new (dlMem) gx::ListBuilder(buf, cap);
		lastSlot = -1;
	};

	for (int g = 0; g < v->numGeoms && ok; g++) {
		const mdl::Geometry& geo = v->geoms[g];
		const int nv = geo.h->numVerts, ue = geo.h->uvExp;
		for (int i = 0; i < nv; i++) {
			const s32* S = skin + (geo.bone && geo.bone[i] < v->numBones ? geo.bone[i] : 0) * 12;
			const s32 z = mulf32(posAt(geo, i, 0), S[2]) + mulf32(posAt(geo, i, 1), S[5]) + mulf32(posAt(geo, i, 2), S[8]) + S[11];
			if (z < minZ) minZ = z;
		}
		auto boneOf = [&](int vi) { return geo.bone && vi < nv && geo.bone[vi] < anim::MAX_NODES ? geo.bone[vi] : 0; };
		for (int mi = 0; mi < geo.h->numMaterials && ok; mi++) {
			const ds::GeometryMaterial& mat = geo.mats[mi];
			const int nt = (int)(mat.numIndices / 3);
			// counting sort of the material's triangles by their first vertex's bone
			int counts[anim::MAX_NODES + 1] = {};
			for (int t = 0; t < nt; t++) counts[boneOf(geo.idx[mat.firstIndex + t * 3]) + 1]++;
			for (int b = 1; b <= anim::MAX_NODES; b++) counts[b] += counts[b - 1];
			for (int t = 0; t < nt; t++) order[counts[boneOf(geo.idx[mat.firstIndex + t * 3])]++] = (uint16_t)t;
			const TexEntry* te = texFor(m, mat.texture);
			const int tw = te ? te->t->w : 0, th = te ? te->t->h : 0;
			bool matOpen = false;
			for (int oi = 0; oi < nt && ok; oi++) {
				const uint32_t i0 = mat.firstIndex + order[oi] * 3;
				int need = 0;
				int b3[3];
				for (int k = 0; k < 3; k++) {
					b3[k] = boneOf(geo.idx[i0 + k]);
					bool dup = false;
					for (int j = 0; j < k; j++) dup |= b3[j] == b3[k];
					if (!dup && (!seg || slotOf[b3[k]] < 0)) need++;
				}
				if (!seg || seg->numBones + need > SEG_BONES) {
					closeSeg();
					if (!ok) break;
					openSeg();
					if (!ok) break;
					matOpen = false;
				}
				for (int k = 0; k < 3; k++)
					if (slotOf[b3[k]] < 0) {
						slotOf[b3[k]] = (int8_t)seg->numBones;
						seg->node[seg->numBones++] = (uint8_t)b3[k];
					}
				if (!matOpen) {
					matOpen = true;
					const int alpha = mat.alpha >= 248 ? 31 : (mat.alpha >> 3) | 1;
					dl->cmd(gx::CMD_POLYGON_ATTR, POLY_ALPHA(alpha) | POLY_CULL_BACK | POLY_FOG | (alpha < 31 ? POLY_ID(4) : 0));
					dl->cmd(gx::CMD_TEXIMAGE_PARAM, te ? te->param : 0);
					if (te && te->hasPal) dl->cmd(gx::CMD_PLTT_BASE, te->pal);
					dl->cmd(gx::CMD_COLOR, tint(mat.color, s_light));
					if (m.numPatch < MAX_PATCH) {
						m.patchSeg[m.numPatch] = m.numSegs;
						m.patchAt[m.numPatch] = (uint16_t)(dl->words() - 1);
						m.patchBase[m.numPatch++] = mat.color;
					}
					dl->cmd(gx::CMD_BEGIN_VTXS, GL_TRIANGLES);
					lastSlot = -1;
				}
				for (int k = 0; k < 3; k++) {
					const int vi = geo.idx[i0 + k] < nv ? geo.idx[i0 + k] : 0;
					const int slot = SLOT_TOP - slotOf[b3[k]];
					if (slot != lastSlot) {
						dl->cmd(gx::CMD_MTX_RESTORE, (u32)slot);
						lastSlot = slot;
					}
					if (te && geo.uv) {
						// t16 = uv * size * 16 = raw * size * 2^ue / 256 (ue < 0 when the UVs stay in 0..1)
						const int sh = 8 - ue;
						s32 u = sh >= 0 ? ((s32)geo.uv[vi][0] * tw) >> sh : ((s32)geo.uv[vi][0] * tw) << -sh;
						s32 t = sh >= 0 ? ((s32)geo.uv[vi][1] * th) >> sh : ((s32)geo.uv[vi][1] * th) << -sh;
						u = u > 32767 ? 32767 : u < -32768 ? -32768 : u;
						t = t > 32767 ? 32767 : t < -32768 ? -32768 : t;
						dl->cmd(gx::CMD_TEXCOORD, (u32)(u16)(s16)u | ((u32)(u16)(s16)t << 16));
					}
					const s32 x = posAt(geo, vi, 0), y = posAt(geo, vi, 1), z = posAt(geo, vi, 2);
					dl->cmd(gx::CMD_VTX_16, (u32)(u16)(s16)x | ((u32)(u16)(s16)y << 16), (u32)(u16)(s16)z);
				}
				seg->tris++;
				m.tris++;
			}
		}
	}
	if (ok) closeSeg();
	m.lift = -(float)minZ / 4096.0f;
	return ok && m.numSegs > 0;
}

static bool finishLoad(Model& m) {
	m.skel = compactSkeleton(m, m.view);
	const bool ok = m.skel && compile(m);
	mdl::free(m.view);
	mem::free(m.blob);
	m.view = nullptr;
	m.blob = nullptr;
	s_stats.listBytes += m.listBytes;
	return ok;
}

int request(int pedIndex) {
	const int have = find(pedIndex);
	if (have >= 0) {
		s_models[have].refs++;
		return have;
	}
	int slot = -1;
	for (int i = 0; i < MAX && slot < 0; i++)
		if (s_models[i].state == STATE_FREE) slot = i;
	if (slot < 0)   // else the oldest unreferenced one
		for (int i = 0; i < MAX; i++)
			if (!s_models[i].refs && (slot < 0 || s_models[i].lastUse < s_models[slot].lastUse)) slot = i;
	if (slot < 0) return -1;
	Model& m = s_models[slot];
	if (m.state != STATE_FREE) freeModel(m);
	m = {};
	m.pedIndex = (int16_t)pedIndex;
	m.refs = 1;
	m.state = STATE_PENDING;
	m.lastUse = s_frame;
	for (uint8_t& t : m.tex) t = NONE;
	return slot;
}

int requestCutscene(const char* name, bool source) {
	if (!name || !name[0] || strlen(name) >= sizeof(s_models[0].cutsceneName)) return -1;
	for (int i = 0; i < MAX; i++) {
		Model& m = s_models[i];
		if (m.state != STATE_FREE && m.state != STATE_FAILED &&
		    m.cutsceneSource == source && !strcmp(m.cutsceneName, name)) {
			m.refs++;
			return i;
		}
	}
	int slot = -1;
	for (int i = 0; i < MAX; i++)
		if (s_models[i].state == STATE_FREE) { slot = i; break; }
	if (slot < 0)
		for (int i = 0; i < MAX; i++)
			if (!s_models[i].refs && (slot < 0 || s_models[i].lastUse < s_models[slot].lastUse)) slot = i;
	if (slot < 0) return -1;
	Model& m = s_models[slot];
	if (m.state != STATE_FREE) freeModel(m);
	m = {};
	memcpy(m.cutsceneName, name, strlen(name) + 1);
	m.cutsceneSource = source;
	m.pedIndex = -1;
	m.refs = 1;
	m.state = STATE_PENDING;
	m.lastUse = s_frame;
	for (uint8_t& t : m.tex) t = NONE;
	return slot;
}

void release(int h) {
	Model* m = get(h);
	if (m && m->refs) {
		m->refs--;
		m->lastUse = s_frame;
	}
}

void update() {
	s_frame++;
	for (Model& m : s_models) {
		if (m.state == STATE_PENDING) {
			if ((int32_t)(s_frame - m.retryAt) < 0) continue;
			const LoadResult r = startLoad(m);
			m.state = r == LOAD_OK ? STATE_TEXTURES : STATE_FAILED;
			if (m.state == STATE_FAILED) {
				mdl::free(m.view);
				mem::free(m.blob);
				m.view = nullptr;
				m.blob = nullptr;
				// out of STREAM: wait and read it again instead of failing the ped
				if (r == LOAD_RETRY && ++m.retries < 40) {
					m.state = STATE_PENDING;
					m.retryAt = s_frame + 15;
				}
			}
			break;   // one DMDL read per frame
		}
		if (m.state == STATE_TEXTURES) {
			bool staged = false;
			for (int k = 0; k < m.numTex && !staged; k++) staged = m.tex[k] != NONE && s_tex[m.tex[k]].state == TEX_STAGED;
			if (staged) continue;
			m.state = finishLoad(m) ? STATE_READY : STATE_FAILED;
			break;
		}
	}
	uint16_t n = 0, r = 0, t = 0;
	u32 vram = 0;
	for (const Model& m : s_models) {
		n += m.state != STATE_FREE;
		r += m.state == STATE_READY;
	}
	for (const TexEntry& te : s_tex) {
		t += te.state == TEX_READY;
		if (te.t) vram += te.t->vramBytes;
	}
	s_stats.models = n;
	s_stats.ready = r;
	s_stats.textures = t;
	s_stats.texVram = vram;
}

void setLight(u8 r, u8 g, u8 b) {
	const u16 t = RGB15(r >> 3, g >> 3, b >> 3);
	if (t == s_light) return;
	s_light = t;
	for (Model& m : s_models) {
		if (m.state != STATE_READY) continue;
		for (int i = 0; i < m.numPatch; i++) {
			u32* w = m.segs[m.patchSeg[i]].list + m.patchAt[i];
			*w = tint(m.patchBase[i], t);
			DC_FlushRange(w, 4);
		}
	}
}

static inline void mult4x3(const s32* m) {
	for (int k = 0; k < 12; k++) MATRIX_MULT4x3 = m[k];
}

int draw(const Model& m, const s32* world) {
	int tris = 0;
	for (int s = 0; s < m.numSegs; s++) {
		const Segment& seg = m.segs[s];
		// slot = entity * world[node] * invBone (GE: current = M * current)
		for (int k = 0; k < seg.numBones; k++) {
			MATRIX_RESTORE = SLOT_ENTITY;
			mult4x3(world + seg.node[k] * 12);
			mult4x3(m.skel->invBone + seg.node[k] * 12);
			MATRIX_STORE = SLOT_TOP - k;
		}
		gx::callList(seg.list);
		tris += seg.tris;
	}
	return tris;
}

void endDraw() {
	GFX_TEX_FORMAT = 0;
	GFX_PAL_FORMAT = 0;
	glBindTexture(0, 0);
}

}
