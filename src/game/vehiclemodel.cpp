#include "game/vehiclemodel.h"

#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "assets/model.h"
#include "assets/texture.h"
#include "game/streaming.h"
#include "game/vehicledata.h"
#include "game/world.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/timing.h"
#include "platform/video.h"
#include "render/gxlist.h"

namespace vehmdl {

static constexpr int MAX_TEX = 64;
static constexpr u32 UPLOAD_BYTES = 16 * 1024;   // per vblank, after world::vblank's share
static constexpr uint16_t NONE = 0xFFFF;

// CVehicleModelInfo::ms_vehicleColourTable placeholders (material colours the
// game replaces): primary / secondary body colour and the four light slots
static constexpr u16 COL_PRIMARY = 7 | 31 << 5;                 // 60,255,0
static constexpr u16 COL_SECONDARY = 31 | 21 << 10;             // 255,0,175
static constexpr u16 COL_LIGHT_FL = 31 | 21 << 5;               // 255,175,0
static constexpr u16 COL_LIGHT_FR = 31 << 5 | 25 << 10;         // 0,255,200
static constexpr u16 COL_LIGHT_RL = 23 | 31 << 5;               // 185,255,0
static constexpr u16 COL_LIGHT_RR = 31 | 7 << 5;                // 255,60,0

enum : uint8_t { TEX_FREE, TEX_STAGED, TEX_READY, TEX_FAILED };
struct TexEntry {
	uint32_t txdHash, nameHash;
	uint16_t refs;
	uint8_t state, hasPal;
	tex::Texture* t;
	u32 param, pal;
	void* staging;
};

struct Dir { FILE* file; const Pack* pack; uint32_t base; int count; ds::TxdEntry* e; };

static Model s_models[MAX_MODELS];
static TexEntry s_tex[MAX_TEX];
static Dir s_generic;
static bool s_genericTried;
static uint32_t s_frame;
static Stats s_stats;
static const uint32_t GENERIC_HASH = ds::fnv1a("vehicle");

const Stats& stats() { return s_stats; }
Model* get(int h) { return (unsigned)h < (unsigned)MAX_MODELS && s_models[h].state != STATE_FREE ? &s_models[h] : nullptr; }
bool ready(int h) { const Model* m = get(h); return m && m->state == STATE_READY; }

void init() {}

// ----------------------------------------------------------------------------
// Textures (model TXD -> models/generic/vehicle.txd parent)
// ----------------------------------------------------------------------------
static void freeTex(TexEntry& t) {
	if (t.t) tex::unload(t.t);
	mem::free(t.staging);
	t = {};
}

static void releaseTex(uint16_t i) {
	if (i == NONE || i >= MAX_TEX) return;
	TexEntry& t = s_tex[i];
	if (t.refs && --t.refs == 0) freeTex(t);
}

static void openGeneric() {
	if (s_genericTried) return;
	s_genericTried = true;
	FILE* f = fopen("game:/models/generic/vehicle.txd", "rb");
	ds::TxdHeader th;
	if (!f || fread(&th, sizeof th, 1, f) != 1 || th.magic != DS_FOURCC('D', 'T', 'X', 'D')) {
		if (f) fclose(f);
		hud::log("\x1b[33mvehicle.txd missing\x1b[39m");
		return;
	}
	ds::TxdEntry* e = (ds::TxdEntry*)mem::alloc(sizeof(ds::TxdEntry) * (th.count ? th.count : 1), mem::TAG_TEXTURE);
	if (!e || fread(e, sizeof(ds::TxdEntry), th.count, f) != th.count) {
		mem::free(e);
		fclose(f);
		return;
	}
	s_generic = { f, nullptr, 0, th.count, e };
}

static void* readEntry(const Dir& d, const ds::TxdEntry& e) {
	void* buf = mem::alloc(e.size, mem::TAG_TEXTURE);
	if (!buf) return nullptr;
	bool ok;
	if (d.file) ok = !fseek(d.file, (long)(d.base + e.offset), SEEK_SET) && fread(buf, 1, e.size, d.file) == e.size;
	else ok = (d.pack ? *d.pack : streaming::g_models).read(d.base + e.offset, buf, e.size);
	if (!ok || ((const ds::TexHeader*)buf)->magic != DS_FOURCC('D', 'T', 'E', 'X')) {
		mem::free(buf);
		return nullptr;
	}
	return buf;
}

static uint16_t requestTex(const Dir& model, uint32_t modelHash, const char* name) {
	const uint32_t h = ds::fnv1a(name);
	for (int i = 0; i < MAX_TEX; i++)
		if (s_tex[i].state != TEX_FREE && s_tex[i].nameHash == h &&
		    (s_tex[i].txdHash == modelHash || s_tex[i].txdHash == GENERIC_HASH)) {
			s_tex[i].refs++;
			return (uint16_t)i;
		}
	int slot = -1;
	for (int i = 0; i < MAX_TEX && slot < 0; i++)
		if (s_tex[i].state == TEX_FREE) slot = i;
	if (slot < 0) return NONE;
	const Dir* dirs[2] = { &model, &s_generic };
	for (int d = 0; d < 2; d++) {
		for (int k = 0; k < dirs[d]->count; k++) {
			if (strcasecmp(dirs[d]->e[k].name, name)) continue;
			void* buf = readEntry(*dirs[d], dirs[d]->e[k]);
			if (!buf) return NONE;
			TexEntry& te = s_tex[slot];
			te = {};
			te.txdHash = d ? GENERIC_HASH : modelHash;
			te.nameHash = h;
			te.refs = 1;
			te.state = TEX_STAGED;
			te.staging = buf;
			return (uint16_t)slot;
		}
	}
	return NONE;
}

void vblank() {
	u32 spent = 0;
	for (int i = 0; i < MAX_TEX && spent < UPLOAD_BYTES; i++) {
		TexEntry& te = s_tex[i];
		if (te.state != TEX_STAGED) continue;
		if (!video::textureUploadWindow()) break;
		const ds::TexHeader* h = (const ds::TexHeader*)te.staging;
		tex::Texture* t = tex::load(h, tex::GROUP_GAME);
		// VRAM full / fragmented: the map gives up its oldest unused textures
		for (int n = 0; !t && n < 16 && world::evictTexture(); n++) t = tex::load(h, tex::GROUP_GAME);
		spent += h->texelBytes + h->numColors * 2u;
		const bool paletted = h->numColors != 0;   // h dies with the staging buffer
		mem::free(te.staging);
		te.staging = nullptr;
		if (!t) {
			te.state = TEX_FAILED;
			continue;
		}
		te.t = t;
		te.state = TEX_READY;
		glBindTexture(0, t->id);
		te.param = glGetTexParameter() | GL_TEXTURE_WRAP_S | GL_TEXTURE_WRAP_T;
		int pal = -1;
		if (paletted) glGetColorTableParameterEXT(0, GL_COLOR_TABLE_FORMAT_EXT, &pal);
		te.hasPal = pal >= 0;
		te.pal = pal < 0 ? 0 : (uint32_t)pal;
	}
	if (spent) glBindTexture(0, 0);
}

// ----------------------------------------------------------------------------
// Frames: model-space matrices (row vectors, rows x / y / z images + pos)
// ----------------------------------------------------------------------------
static void mul(const float* a, const float* b, float* o) {   // o = a * b
	float r[12];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
	for (int j = 0; j < 3; j++) r[9 + j] = a[9] * b[j] + a[10] * b[3 + j] + a[11] * b[6 + j] + b[9 + j];
	memcpy(o, r, sizeof r);
}

static void toGx(const float* m, s32* o) {
	for (int k = 0; k < 12; k++) o[k] = (s32)(m[k] * 4096.0f);
}

static void local(const mdl::Frame& f, float* m) {
	for (int k = 0; k < 9; k++) m[k] = (float)f.rot[k] * (1.0f / 4096.0f);
	for (int k = 0; k < 3; k++) m[9 + k] = (float)f.pos[k] * (1.0f / 4096.0f);
}

static bool isIdentity(const float* m) {
	static const float I[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
	for (int k = 0; k < 12; k++)
		if (fabsf(m[k] - I[k]) > 0.0005f) return false;
	return true;
}

struct NameComp { const char* base; Comp comp; };
static const NameComp k_comps[] = {
	{ "door_lf", COMP_DOOR_LF }, { "door_rf", COMP_DOOR_RF }, { "door_lr", COMP_DOOR_LR }, { "door_rr", COMP_DOOR_RR },
	{ "bonnet", COMP_BONNET }, { "boot", COMP_BOOT }, { "bump_front", COMP_BUMP_FRONT }, { "bump_rear", COMP_BUMP_REAR },
	{ "windscreen", COMP_WINDSCREEN }, { "wing_lf", COMP_WING_LF }, { "wing_rf", COMP_WING_RF },
	{ "wing_lr", COMP_WING_LR }, { "wing_rr", COMP_WING_RR },
};

// "door_lf_ok" -> (DOOR_LF, OK), "door_lf_dummy" -> (DOOR_LF, ANY) + dummy
static Comp classify(const char* n, Variant& var, bool& dummy) {
	char b[24];
	int len = 0;
	for (; len < 23 && n[len]; len++) b[len] = (char)(n[len] >= 'A' && n[len] <= 'Z' ? n[len] + 32 : n[len]);
	b[len] = 0;
	var = VAR_ANY;
	dummy = false;
	auto strip = [&](const char* suf) {
		const int sl = (int)strlen(suf);
		if (len > sl && !strcmp(b + len - sl, suf)) { len -= sl; b[len] = 0; return true; }
		return false;
	};
	if (strip("_ok")) var = VAR_OK;
	else if (strip("_dam")) var = VAR_DAM;
	else if (strip("_dummy")) dummy = true;
	if (!strcmp(b, "chassis")) return COMP_CHASSIS;
	if (!strcmp(b, "wheel")) return COMP_WHEEL;
	for (const NameComp& c : k_comps)
		if (!strcmp(b, c.base)) return c.comp;
	return COMP_NONE;
}

// CBike::SetupModelNodes / CHeli / CPlane / CBoat frame names that animate
struct NameAnim { const char* name; Anim anim; Vis vis; };
static const NameAnim k_anims[] = {
	{ "forks_front", ANIM_STEER, VIS_ALWAYS }, { "handlebars", ANIM_STEER, VIS_ALWAYS },
	{ "wheel_front", ANIM_WHEEL_F, VIS_ALWAYS }, { "wheel_rear", ANIM_WHEEL_R, VIS_ALWAYS },
	{ "moving_rotor", ANIM_ROTOR, VIS_FAST }, { "static_rotor", ANIM_ROTOR, VIS_SLOW },
	{ "moving_rotor2", ANIM_ROTOR2, VIS_FAST }, { "static_rotor2", ANIM_ROTOR2, VIS_SLOW },
	{ "moving_prop", ANIM_PROP, VIS_FAST }, { "static_prop", ANIM_PROP, VIS_SLOW },
	{ "moving_prop2", ANIM_PROP, VIS_FAST }, { "static_prop2", ANIM_PROP, VIS_SLOW },
	{ "rudder", ANIM_RUDDER, VIS_ALWAYS }, { "elevator", ANIM_ELEVATOR, VIS_ALWAYS },
	{ "elevator_l", ANIM_ELEVATOR, VIS_ALWAYS }, { "elevator_r", ANIM_ELEVATOR, VIS_ALWAYS },
	{ "aileron_l", ANIM_AILERON_L, VIS_ALWAYS }, { "aileron_r", ANIM_AILERON_R, VIS_ALWAYS },
};

static Anim animOf(const char* n, Vis& vis) {
	vis = VIS_ALWAYS;
	for (const NameAnim& a : k_anims)
		if (!strncasecmp(n, a.name, 24)) { vis = a.vis; return a.anim; }
	return ANIM_NONE;
}

static inline s32 posAt(const mdl::Geometry& g, int v, int k) {
	const int pe = g.h->posExp;
	return g.h->offset[k] + (pe >= 0 ? (s32)g.pos[v][k] * (1 << pe) : (s32)g.pos[v][k] >> -pe);
}

// ----------------------------------------------------------------------------
// Compile (one list per atomic). The list sets the material registers itself:
// DIF_AMB (diffuse = material colour, ambient = 5/8 of it) is patchable for
// the body colours, every vertex carries its NORMAL (light0 = the car light).
// ----------------------------------------------------------------------------
static const TexEntry* texFor(const Model& m, const char* name) {
	if (!name[0]) return nullptr;
	const uint32_t h = ds::fnv1a(name);
	for (int k = 0; k < m.numTex; k++)
		if (m.tex[k] != NONE && s_tex[m.tex[k]].state == TEX_READY && s_tex[m.tex[k]].nameHash == h) return &s_tex[m.tex[k]];
	return nullptr;
}

static inline u32 difAmb(u16 c) {
	const u16 a = (u16)((((c & 31) * 5 >> 3)) | (((c >> 5 & 31) * 5 >> 3) << 5) | (((c >> 10 & 31) * 5 >> 3) << 10));
	return (u32)c | (u32)a << 16;
}

static bool compilePart(Model& m, Part& p, const mdl::Geometry& geo, int e) {
	const u32 cap = geo.h->numIndices * 8 + geo.h->numMaterials * 10 + 8;
	mem::Scratch scratch;
	u32* buf = (u32*)scratch.alloc(cap * 4);
	if (!buf) return false;
	gx::ListBuilder dl(buf, cap);
	dl.cmd(gx::CMD_SPE_EMI, 0);
	const int nv = geo.h->numVerts, ue = geo.h->uvExp;
	uint32_t tris = 0;
	p.numPatch = 0;
	for (int mi = 0; mi < geo.h->numMaterials; mi++) {
		const ds::GeometryMaterial& mat = geo.mats[mi];
		const TexEntry* te = texFor(m, mat.texture);
		const int alpha = mat.alpha >= 248 ? 31 : (mat.alpha >> 3) | 1;
		dl.cmd(gx::CMD_POLYGON_ATTR, POLY_ALPHA(alpha) | POLY_CULL_BACK | POLY_FOG | POLY_FORMAT_LIGHT0 |
		                                 (alpha < 31 ? POLY_ID(3) : 0));
		dl.cmd(gx::CMD_TEXIMAGE_PARAM, te ? te->param : 0);
		if (te && te->hasPal) dl.cmd(gx::CMD_PLTT_BASE, te->pal);
		u16 col = mat.color;
		uint8_t kind = 0;
		if (col == COL_PRIMARY) kind = PATCH_PRIMARY;
		else if (col == COL_SECONDARY) kind = PATCH_SECONDARY;
		else if (col == COL_LIGHT_FL || col == COL_LIGHT_FR) kind = PATCH_LIGHT_FRONT;
		else if (col == COL_LIGHT_RL || col == COL_LIGHT_RR) kind = PATCH_LIGHT_REAR;
		if (kind) col = 0x6318;   // grey until setColours
		dl.cmd(gx::CMD_DIF_AMB, difAmb(col));
		if (kind && p.numPatch < MAX_PATCH) {
			p.patchAt[p.numPatch] = (uint16_t)(dl.words() - 1);
			p.patchKind[p.numPatch] = kind;
			p.patchBase[p.numPatch++] = mat.color;
		}
		dl.cmd(gx::CMD_BEGIN_VTXS, GL_TRIANGLES);
		const bool uv = te && geo.uv;
		const int tw = te ? te->t->w : 0, th = te ? te->t->h : 0;
		const uint32_t end = mat.firstIndex + mat.numIndices;
		for (uint32_t i = mat.firstIndex; i + 2 < end; i += 3) {
			int vi[3];
			s32 tu[3] = {}, tv[3] = {};
			for (int k = 0; k < 3; k++) {
				vi[k] = geo.idx[i + k] < nv ? geo.idx[i + k] : 0;
				if (uv) {
					tu[k] = ((s32)geo.uv[vi[k]][0] * (tw << ue)) >> 8;
					tv[k] = ((s32)geo.uv[vi[k]][1] * (th << ue)) >> 8;
				}
			}
			s32 su = 0, sv = 0;   // whole-repeat shift so 12.4 texcoords stay in range
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
				dl.cmd(gx::CMD_NORMAL, geo.normal ? geo.normal[v] : 0x1FFu << 20);
				const s32 x = posAt(geo, v, 0) >> e, y = posAt(geo, v, 1) >> e, z = posAt(geo, v, 2) >> e;
				dl.cmd(gx::CMD_VTX_16, (u32)(u16)(s16)x | ((u32)(u16)(s16)y << 16), (u32)(u16)(s16)z);
			}
			tris++;
		}
	}
	const u32 n = dl.finish();
	if (!n) return false;
	p.list = (u32*)mem::alloc(n * 4, mem::TAG_MODEL);
	if (!p.list) return false;
	memcpy(p.list, buf, n * 4);
	gx::flushList(p.list);
	p.tris = (uint16_t)tris;
	m.listBytes += n * 4;
	return true;
}

// ----------------------------------------------------------------------------
// Load (CStreaming::RequestModel + CVehicleModelInfo::SetVehicleComponentFlags)
// ----------------------------------------------------------------------------
static void freeModel(Model& m) {
	for (int i = 0; i < m.numParts; i++) mem::free(m.parts[i].list);
	for (int k = 0; k < m.numTex; k++) releaseTex(m.tex[k]);
	mdl::free((mdl::Model*)m.view);
	mem::free(m.blob);
	s_stats.listBytes -= m.listBytes;
	m = {};
}

enum LoadResult { LOAD_OK, LOAD_RETRY, LOAD_FAIL };

static LoadResult startLoad(Model& m) {
	openGeneric();
	s_stats.loads++;
	const Pack& pack = m.cutsceneName[0] ? streaming::g_cutscene : streaming::g_models;
	const char* const base = m.cutsceneName[0] ? m.cutsceneName : m.info.model;
	char name[40];
	snprintf(name, sizeof name, "%.24s.dff", base);
	ds::PackEntry de;
	if (!pack.find(name, de)) {
		hud::log("\x1b[33mveh: %s missing\x1b[39m", name);
		return LOAD_FAIL;
	}
	// a ~40 KB DMDL in a full / fragmented STREAM heap (the map streams
	// into it too) is not a broken model: it failed the vehicle outright,
	// so spawns and traffic cars vanished at random. Try again shortly.
	m.blob = pack.load(de, mem::TAG_MODEL);
	if (!m.blob && world::releaseRam(de.size)) m.blob = pack.load(de, mem::TAG_MODEL);
	mdl::Model* view = m.blob ? mdl::load(m.blob) : nullptr;
	m.view = view;
	if (!view) {
		mem::free(m.blob);
		m.blob = nullptr;
		return LOAD_RETRY;
	}
	// model TXD directory (entries only)
	Dir dir = {};
	const char* const txd = m.cutsceneName[0] ? m.cutsceneName : m.info.txd;
	snprintf(name, sizeof name, "%.24s.txd", txd);
	ds::PackEntry pe;
	ds::TxdHeader th;
	mem::Scratch scratch;
	if (pack.find(name, pe) && pack.read(pe.offset, &th, sizeof th) &&
	    th.magic == DS_FOURCC('D', 'T', 'X', 'D') && th.count) {
		ds::TxdEntry* e = (ds::TxdEntry*)scratch.alloc(sizeof(ds::TxdEntry) * th.count);
		if (e && pack.read(pe.offset + sizeof th, e, sizeof(ds::TxdEntry) * th.count))
			dir = { nullptr, &pack, pe.offset, th.count, e };
	}
	const uint32_t txdHash = ds::fnv1a(txd);
	m.numTex = 0;
	for (int g = 0; g < view->numGeoms; g++) {
		const mdl::Geometry& geo = view->geoms[g];
		for (int mi = 0; mi < geo.h->numMaterials; mi++) {
			const char* tn = geo.mats[mi].texture;
			if (!tn[0] || m.numTex >= (int)(sizeof m.tex / sizeof m.tex[0])) continue;
			const uint32_t h = ds::fnv1a(tn);
			bool dup = false;
			for (int k = 0; k < m.numTex && !dup; k++) dup = m.tex[k] != NONE && s_tex[m.tex[k]].nameHash == h;
			if (!dup) m.tex[m.numTex++] = requestTex(dir, txdHash, tn);
		}
	}
	return LOAD_OK;
}

static bool finishLoad(Model& m) {
	mdl::Model* view = (mdl::Model*)m.view;
	const ds::ModelHeader* hdr = (const ds::ModelHeader*)m.blob;
	const ds::ModelFrame* sf = (const ds::ModelFrame*)(hdr + 1);
	const int nf = view->numFrames;
	mem::Scratch scratch;
	float* W = (float*)scratch.alloc(sizeof(float) * 12 * (nf ? nf : 1));
	float* L = (float*)scratch.alloc(sizeof(float) * 12 * (nf ? nf : 1));
	uint8_t* done = (uint8_t*)scratch.alloc(nf ? nf : 1);
	if (!W || !L || !done) return false;
	memset(done, 0, nf);
	for (int i = 0; i < nf; i++) local(view->frames[i], L + i * 12);
	// frames come parent-first in RW clumps; the loop tolerates either order
	for (int pass = 0; pass < 8; pass++)
		for (int i = 0; i < nf; i++) {
			if (done[i]) continue;
			const int par = view->frames[i].parent;
			if (par < 0 || par >= nf) { memcpy(W + i * 12, L + i * 12, 48); done[i] = 1; }
			else if (done[par]) { mul(L + i * 12, W + par * 12, W + i * 12); done[i] = 1; }
		}
	// dummies (CVehicleModelInfo::m_pVehicleStruct->m_avDummyPos, wheel posns)
	int wheelFrame[NUM_WHEELS] = { -1, -1, -1, -1 };
	for (int i = 0; i < nf; i++) {
		char n[25];
		memcpy(n, sf[i].name, 24);
		n[24] = 0;
		for (char* c = n; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
		const float* p = W + i * 12 + 9;
		if (!strcmp(n, "wheel_lf_dummy")) wheelFrame[WHEEL_FL] = i;
		else if (!strcmp(n, "wheel_lb_dummy")) wheelFrame[WHEEL_RL] = i;
		else if (!strcmp(n, "wheel_rf_dummy")) wheelFrame[WHEEL_FR] = i;
		else if (!strcmp(n, "wheel_rb_dummy")) wheelFrame[WHEEL_RR] = i;
		else if (!strcmp(n, "ped_frontseat")) { m.seat[0] = -p[0]; m.seat[1] = p[1]; m.seat[2] = p[2]; }
		else if (!strcmp(n, "exhaust")) memcpy(m.exhaust, p, 12);
		else if (!strcmp(n, "engine")) memcpy(m.engine, p, 12);
		else if (!strcmp(n, "headlights") || (!strcmp(n, "boat_lights") && !m.hasHead)) { memcpy(m.headlight, p, 12); m.hasHead = true; }
		else if (!strcmp(n, "taillights")) { memcpy(m.taillight, p, 12); m.hasTail = true; }
		else if (!strcmp(n, "wheel_front")) { wheelFrame[WHEEL_FL] = wheelFrame[WHEEL_FR] = i; m.bikeWheels = true; }
		else if (!strcmp(n, "wheel_rear")) { wheelFrame[WHEEL_RL] = wheelFrame[WHEEL_RR] = i; m.bikeWheels = true; }
		Variant v;
		bool dummy;
		const Comp c = classify(n, v, dummy);
		if (dummy && c != COMP_NONE) {
			memcpy(m.hinge[c], p, 12);
			toGx(W + i * 12, m.hingeM[c]);
		}
	}
	for (int w = 0; w < NUM_WHEELS; w++)
		if (wheelFrame[w] >= 0) memcpy(m.wheelPos[w], W + wheelFrame[w] * 12 + 9, 12);
	// missing rear dummies (some 4x4s name them differently): mirror the front
	for (int w = 0; w < NUM_WHEELS; w++)
		if (wheelFrame[w] < 0) {
			const int o = w == WHEEL_FL ? WHEEL_FR : w == WHEEL_FR ? WHEEL_FL : w == WHEEL_RL ? WHEEL_RR : WHEEL_RL;
			m.wheelPos[w][0] = -m.wheelPos[o][0];
			m.wheelPos[w][1] = m.wheelPos[o][1];
			m.wheelPos[w][2] = m.wheelPos[o][2];
		}
	m.wheelRadius[0] = m.info.wheelScaleF * 0.5f;
	m.wheelRadius[1] = m.info.wheelScaleR * 0.5f;

	// vertex scale for the whole model (buses / trucks exceed +-8 m)
	s32 maxAbs = 1;
	for (int g = 0; g < view->numGeoms; g++)
		for (int v = 0; v < view->geoms[g].h->numVerts; v++)
			for (int k = 0; k < 3; k++) {
				const s32 a = posAt(view->geoms[g], v, k);
				if ((a < 0 ? -a : a) > maxAbs) maxAbs = a < 0 ? -a : a;
			}
	int e = 0;
	while ((maxAbs >> e) > 32767) e++;
	m.scaleExp = (int8_t)e;

	for (int k = 0; k < 3; k++) { m.bmin[k] = 1e9f; m.bmax[k] = -1e9f; }
	m.numParts = 0;
	m.wheelPart = -1;
	float wheelGeomR = 0.0f;
	for (int a = 0; a < view->numAtomics && m.numParts < MAX_PARTS; a++) {
		const ds::ModelAtomic& at = view->atomics[a];
		if (at.geometry >= view->numGeoms || at.frame >= nf) continue;
		Part& p = m.parts[m.numParts];
		p = {};
		p.hinge = -1;
		// component + variant: the nearest named ancestor decides
		Comp comp = COMP_NONE;
		Variant var = VAR_ANY;
		int hinge = -1;
		for (int f = at.frame; f >= 0 && f < nf; f = view->frames[f].parent) {
			Variant v;
			bool dummy;
			const Comp c = classify(sf[f].name, v, dummy);
			if (dummy) {
				if (c != COMP_NONE && hinge < 0 && (comp == COMP_NONE || comp == c)) { hinge = f; if (comp == COMP_NONE) comp = c; }
				continue;
			}
			if (c != COMP_NONE && comp == COMP_NONE) comp = c;
			if (v != VAR_ANY && var == VAR_ANY) var = v;
		}
		if (comp == COMP_WHEEL) hinge = -1;
		const bool moving = comp >= COMP_DOOR_LF && comp <= COMP_BOOT && hinge >= 0;
		// animated frames on the way to the root: inner (nearest) and outer
		int animF[2] = { -1, -1 };
		Anim animK[2] = { ANIM_NONE, ANIM_NONE };
		Vis vis = VIS_ALWAYS;
		for (int f = at.frame, n = 0; !moving && comp != COMP_WHEEL && f >= 0 && f < nf && n < 2; f = view->frames[f].parent) {
			Vis fv;
			const Anim an = animOf(sf[f].name, fv);
			if (an == ANIM_NONE) continue;
			if (fv != VIS_ALWAYS) vis = fv;
			animF[n] = f;
			animK[n++] = an;
		}
		if (animF[1] < 0 && animF[0] >= 0) {   // one: it is the outer
			animF[1] = animF[0]; animK[1] = animK[0];
			animF[0] = -1; animK[0] = ANIM_NONE;
		}
		if (animF[1] >= 0 && m.numAnimM + 2 > MAX_ANIM_M) animF[0] = animF[1] = -1;
		static const float I[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };
		float below[12];
		p.vis = vis;
		if (animF[1] >= 0) {
			// outer: model space matrix; inner: its frame chain up to the outer;
			// below: part frame chain up to the innermost anim frame
			p.anim1 = animK[1];
			p.m1 = (int8_t)m.numAnimM;
			toGx(W + animF[1] * 12, m.animM[m.numAnimM++]);
			const int stop = animF[0] >= 0 ? animF[0] : animF[1];
			if (animF[0] >= 0) {
				float mid[12];
				memcpy(mid, I, sizeof mid);
				for (int f = animF[0]; f >= 0 && f != animF[1]; f = view->frames[f].parent) mul(mid, L + f * 12, mid);
				p.anim2 = animK[0];
				p.m2 = (int8_t)m.numAnimM;
				toGx(mid, m.animM[m.numAnimM++]);
			}
			memcpy(below, I, sizeof below);
			for (int f = at.frame; f >= 0 && f != stop; f = view->frames[f].parent) mul(below, L + f * 12, below);
		} else if (moving) {
			// part frame -> hinge, excluding the hinge's own transform
			memcpy(below, I, sizeof below);
			for (int f = at.frame; f >= 0 && f != hinge; f = view->frames[f].parent) mul(below, L + f * 12, below);
			p.hinge = (int8_t)hinge;
		} else if (comp == COMP_WHEEL) {
			memcpy(below, I, sizeof below);
		} else {
			memcpy(below, W + at.frame * 12, sizeof below);
		}
		p.identity = isIdentity(below);
		toGx(below, p.below);
		p.comp = comp;
		p.variant = var;
		const mdl::Geometry& geo = view->geoms[at.geometry];
		if (!compilePart(m, p, geo, e)) continue;
		// bounds: geometry box corners through the part's model-space matrix
		s32 lo[3] = { INT32_MAX, INT32_MAX, INT32_MAX }, hi[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
		for (int v = 0; v < geo.h->numVerts; v++)
			for (int k = 0; k < 3; k++) {
				const s32 q = posAt(geo, v, k);
				if (q < lo[k]) lo[k] = q;
				if (q > hi[k]) hi[k] = q;
			}
		if (comp == COMP_WHEEL) {
			m.wheelPart = m.numParts;
			wheelGeomR = (float)(hi[2] - lo[2]) * (0.5f / 4096.0f);
		} else if (var != VAR_DAM && geo.h->numVerts && p.anim1 != ANIM_ROTOR && p.anim1 != ANIM_ROTOR2 &&
		           p.anim1 != ANIM_PROP) {   // rotor discs / propellers would blow the body spheres up
			const float* M = W + at.frame * 12;
			for (int c = 0; c < 8; c++) {
				const float q[3] = { (float)(c & 1 ? hi[0] : lo[0]) / 4096.0f, (float)(c & 2 ? hi[1] : lo[1]) / 4096.0f,
				                     (float)(c & 4 ? hi[2] : lo[2]) / 4096.0f };
				for (int k = 0; k < 3; k++) {
					const float w = q[0] * M[k] + q[1] * M[3 + k] + q[2] * M[6 + k] + M[9 + k];
					if (w < m.bmin[k]) m.bmin[k] = w;
					if (w > m.bmax[k]) m.bmax[k] = w;
				}
			}
		}
		m.numParts++;
	}
	for (int k = 0; k < 2; k++) {
		float s = wheelGeomR > 0.05f ? m.wheelRadius[k] / wheelGeomR : 1.0f;
		m.wheelScale[k] = s < 0.5f ? 0.5f : s > 2.0f ? 2.0f : s;
	}
	if (m.bmin[0] > m.bmax[0])
		for (int k = 0; k < 3; k++) { m.bmin[k] = -1.0f; m.bmax[k] = 1.0f; }
	s_stats.listBytes += m.listBytes;
	mdl::free(view);
	mem::free(m.blob);
	m.view = nullptr;
	m.blob = nullptr;
	m.curPrim = m.curSec = 0xFFFF;
	m.curLights = 0xFF;
	hud::log("veh %.12s: %d parts %luK", m.info.model, m.numParts, (unsigned long)(m.listBytes >> 10));
	return m.numParts > 0;
}

int request(int vehIndex, const char* cutsceneName) {
	if (cutsceneName && strlen(cutsceneName) >= sizeof s_models[0].cutsceneName) return -1;
	const char* model = cutsceneName ? cutsceneName : "";
	for (int i = 0; i < MAX_MODELS; i++)
		if (s_models[i].state != STATE_FREE && s_models[i].vehIndex == vehIndex &&
		    !strcmp(s_models[i].cutsceneName, model) && s_models[i].state != STATE_FAILED) {
			s_models[i].refs++;
			return i;
		}
	int slot = -1;
	for (int i = 0; i < MAX_MODELS && slot < 0; i++)
		if (s_models[i].state == STATE_FREE) slot = i;
	// reuse the oldest unreferenced model
	for (int i = 0; i < MAX_MODELS && slot < 0; i++)
		if (!s_models[i].refs && (slot < 0 || s_models[i].lastUse < s_models[slot].lastUse)) slot = i;
	if (slot < 0) return -1;
	Model& m = s_models[slot];
	if (m.state != STATE_FREE) freeModel(m);
	m = {};
	if (!vehdata::read(vehIndex, m.info)) return -1;
	snprintf(m.cutsceneName, sizeof m.cutsceneName, "%s", model);
	m.vehIndex = (int16_t)vehIndex;
	m.refs = 1;
	m.state = STATE_PENDING;
	m.lastUse = s_frame;
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
			if (r == LOAD_RETRY && ++m.retries < 40) {
				m.retryAt = s_frame + 15;
			} else {
				if (r == LOAD_RETRY) hud::log("\x1b[33mveh: no RAM for %.12s\x1b[39m", m.info.model);
				m.state = r == LOAD_OK ? STATE_TEXTURES : STATE_FAILED;
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
	uint16_t n = 0, r = 0, t = 0, f = 0;
	u32 vram = 0;
	for (const Model& m : s_models) {
		n += m.state != STATE_FREE;
		r += m.state == STATE_READY;
	}
	for (const TexEntry& te : s_tex) {
		t += te.state == TEX_READY;
		f += te.state == TEX_FAILED;
		if (te.t) vram += te.t->vramBytes;
	}
	s_stats.models = n;
	s_stats.ready = r;
	s_stats.textures = t;
	s_stats.texFailed = f;
	s_stats.texVram = vram;
}

void setColours(Model& m, u16 prim, u16 sec, bool lightsOn) {
	const uint8_t lights = lightsOn ? 1 : 0;
	if (prim == m.curPrim && sec == m.curSec && lights == m.curLights) return;
	m.curPrim = prim;
	m.curSec = sec;
	m.curLights = lights;
	for (int i = 0; i < m.numParts; i++) {
		Part& p = m.parts[i];
		for (int k = 0; k < p.numPatch; k++) {
			u16 c;
			switch (p.patchKind[k]) {
			case PATCH_PRIMARY: c = prim; break;
			case PATCH_SECONDARY: c = sec; break;
			default: c = lightsOn ? 0x7FFF : 0x4210; break;
			}
			u32* w = p.list + p.patchAt[k];
			*w = difAmb(c);
			DC_FlushRange(w, 4);
		}
	}
}

}
