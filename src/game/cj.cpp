#include "game/cj.h"

#include <nds.h>
#include <stdlib.h>
#include <string.h>

#include "assets/texture.h"
#include "game/anim.h"
#include "game/streaming.h"
#include "hud/hud.h"

namespace cj {

// The default outfit from main.scm: (texture, model) pairs. Hands get no own
// texture part - they share the torso's (BODY_PART_TEX_NAMES in ClothesBuilder).
static const struct { const char* dff; const char* txd; } k_parts[] = {
	{ "vest.dff",    "vest.txd" },
	{ "head.dff",    "player_face.txd" },
	{ "hands.dff",   "vest.txd" },
	{ "jeans.dff",   "jeansdenim.txd" },
	{ "sneaker.dff", "sneakerbincblk.txd" },
};
static constexpr int NUM_PARTS = sizeof(k_parts) / sizeof(k_parts[0]);

struct Part {
	mdl::Model* m;
	void* blob;          // DMDL blob (owned)
	tex::Texture* tex;   // VRAM texture for all its materials
};

static Part s_parts[NUM_PARTS];
static anim::Player s_anim;
static char s_animName[24];
static int s_vertCount;
static float s_lift;        // +z entity offset so the feet rest at the origin

static s32 s_world[anim::MAX_NODES * 12];
static s32 s_skin[anim::MAX_NODES * 12];

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
		p.blob = loadEntry(streaming::g_player, k_parts[i].dff);
		p.m = mdl::load(p.blob);
		p.tex = nullptr;
		void* txd = loadEntry(streaming::g_player, k_parts[i].txd);
		if (txd) {
			p.tex = tex::loadFirst(txd);
			::free(txd);
		}
		hud::log("cj %s: %s, %u verts, tex %s", k_parts[i].dff,
		         p.m ? "ok" : "FAILED",
		         p.m ? (unsigned)p.m->geoms[0].h->numVerts : 0u,
		         p.tex ? "ok" : "none");
	}
	if (!s_parts[0].m) return false;
	// safe fallback pose (bind) if the anim below fails to load
	s_anim.seqs = nullptr; s_anim.numSeqs = 0; s_anim.duration = 1;
	s_anim.loop = 1; s_anim.time = 0;
	for (int i = 0; i < anim::MAX_NODES; i++) s_anim.seqOfNode[i] = -1;
	setAnim("IDLE_stance");

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

void setAnim(const char* name) {
	if (!strncmp(name, s_animName, sizeof s_animName)) return;
	if (!streaming::g_pedIfp) return;
	if (anim::play(s_anim, s_parts[0].m, streaming::g_pedIfp, name))
		strncpy(s_animName, name, sizeof s_animName - 1);
	else
		hud::log("\x1b[33manim '%s' not in ped.ifp\x1b[39m", name);
}

const char* animName() { return s_animName; }

void advance(int dt60) { anim::advance(s_anim, dt60); }

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
			for (uint32_t i = mat.firstIndex; i < mat.firstIndex + mat.numIndices; i++) {
				const uint16_t v = geo.idx[i] < nv ? geo.idx[i] : 0;
				glTexCoord2t16(s_uv[v][0], s_uv[v][1]);
				glVertex3v16(s_xyz[v][0], s_xyz[v][1], s_xyz[v][2]);
			}
			glEnd();
		}
	}
}

void draw(float x, float y, float z, float heading) {
	anim::evalWorld(s_parts[0].m, s_anim, s_world);

	// entity matrix: SA model space -> GL via rotX(-90), heading on the up
	// axis (SA heading is counterclockwise-from-north -> negative GL yaw),
	// position in world (SA x,y,z -> GL x,z,-y)
	glPushMatrix();
	glTranslatef(x, z + s_lift, -y);
	glRotatef(-heading * 57.2958f, 0.0f, 1.0f, 0.0f);
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
