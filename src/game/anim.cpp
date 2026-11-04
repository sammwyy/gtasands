#include "game/anim.h"

#include <nds.h>
#include <string.h>

// All pose math is s32 (20.12) fixed point: the ARM9 has no FPU and this runs
// per skinned vertex. Quaternion keys come in as s16 4.12, widened to s32.
// Row-vector convention everywhere: p' = p * M, M = M_a * M_b means "a then b".
namespace anim {

// --- seq record helpers -----------------------------------------------------
// AnimEntry: name[24], u16 numSeqs, u16 pad  (28 B)
// AnimSeq:   name[24], i16 boneTag, u8 hasTrans, u8 pad, u16 numFrames, u16 pad2 (32 B)
// key:       i16 q[4], i16 time (10 B) [+ i16 t[3] (6 B) if hasTrans]

static const uint8_t* seqKeys(const ds::AnimSeq* s) { return (const uint8_t*)(s + 1); }

static uint8_t seqKeyStride(const ds::AnimSeq* s) { return s->hasTrans ? 16 : 10; }

static const ds::AnimSeq* nextSeq(const ds::AnimSeq* s) {
	const size_t keyBytes = (((size_t)s->numFrames * seqKeyStride(s)) + 3) & ~3u;
	return (const ds::AnimSeq*)(seqKeys(s) + keyBytes);
}

static const ds::AnimSeq* seqAt(const Player& p, int i) {
	const ds::AnimSeq* s = (const ds::AnimSeq*)p.seqs;
	while (i-- > 0) s = nextSeq(s);
	return s;
}

static const uint8_t* findEntry(const void* danm, const char* name) {
	const ds::AnimHeader* h = (const ds::AnimHeader*)danm;
	if (!danm || h->magic != DS_FOURCC('D', 'A', 'N', 'M')) return nullptr;
	const uint8_t* p = (const uint8_t*)(h + 1);
	for (int i = 0; i < h->numAnims; i++) {
		const ds::AnimEntry* e = (const ds::AnimEntry*)p;
		p += sizeof(ds::AnimEntry);
		const ds::AnimSeq* s = (const ds::AnimSeq*)p;
		if (!strcmp(e->name, name)) return (const uint8_t*)e;
		for (int j = 0; j < e->numSeqs; j++) s = nextSeq(s);
		p = (const uint8_t*)s;
	}
	return nullptr;
}

// --- fixed-point pose math --------------------------------------------------

static inline s32 qmul(s32 a, s32 b) { return mulf32(a, b); }

// q (s32, |q|~1) -> RW rotation matrix: rows right/up/at, row-vector use
// (the transpose of the usual column-vector quat-to-matrix).
static void quatToMat(const s32* q, s32* m) {
	const s32 x2 = q[0] << 1, y2 = q[1] << 1, z2 = q[2] << 1;
	const s32 xx = qmul(q[0], x2), xy = qmul(q[0], y2), xz = qmul(q[0], z2);
	const s32 yy = qmul(q[1], y2), yz = qmul(q[1], z2), zz = qmul(q[2], z2);
	const s32 wx = qmul(q[3], x2), wy = qmul(q[3], y2), wz = qmul(q[3], z2);
	const s32 one = 1 << 12;
	m[0] = one - (yy + zz); m[1] = xy + wz;         m[2] = xz - wy;
	m[3] = xy - wz;         m[4] = one - (xx + zz); m[5] = yz + wx;
	m[6] = xz + wy;         m[7] = yz - wx;         m[8] = one - (xx + yy);
}

// m = a * b, row-vector convention: R = Ra.Rb, t = ta.Rb + tb (all s32, 4x3).
static void matMul(const s32* a, const s32* b, s32* m) {
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			m[r * 3 + c] = qmul(a[r * 3], b[c]) + qmul(a[r * 3 + 1], b[3 + c]) +
			               qmul(a[r * 3 + 2], b[6 + c]);
	for (int r = 0; r < 3; r++)
		m[9 + r] = qmul(a[9], b[r]) + qmul(a[10], b[3 + r]) +
		           qmul(a[11], b[6 + r]) + b[9 + r];
}

// key[i] = q[4] (s16 4.12), time (s16, 1/60 s), [t[3] (s16, 1/1024 u)]
static void seqRot(const ds::AnimSeq* s, int keyStride, uint8_t& cursor, int32_t t, s32* q) {
	const uint8_t* k = seqKeys(s);
	int i = cursor < s->numFrames ? cursor : 0;
	// keys are sorted by time; walk the cursor forward while we're past the next key
	while (i + 1 < s->numFrames && (int32_t)(int16_t)(*(const int16_t*)(k + (i + 1) * keyStride + 8)) <= t) i++;
	cursor = (uint8_t)i;
	const int16_t* ka = (const int16_t*)(k + i * keyStride);
	const int16_t* kb = (const int16_t*)(k + (i + 1 < s->numFrames ? i + 1 : i) * keyStride);
	const int32_t t0 = ka[4], t1 = kb[4];
	s32 f = t1 > t0 ? (s32)(((t - t0) << 12) / (t1 - t0)) : 0;
	if (f < 0) f = 0;
	if (f > 1 << 12) f = 1 << 12;
	// nlerp with shortest-path sign fix; keys are already 4.12 -> widen, no shift
	s32 dot = 0;
	for (int c = 0; c < 4; c++) dot += (int32_t)ka[c] * kb[c];
	const int32_t sgn = dot < 0 ? -1 : 1;
	s32 l[4], d2 = 0;
	for (int c = 0; c < 4; c++) {
		l[c] = ka[c] + ((s32)(kb[c] * sgn - ka[c]) * f >> 12);
		d2 += qmul(l[c], l[c]);
	}
	const s32 inv = divf32(1 << 12, sqrtf32(d2 ? d2 : 1 << 12));
	for (int c = 0; c < 4; c++) q[c] = qmul(l[c], inv);
}

// trans keys are s16 units of 1/1024 -> s32 20.12 is *4
static void lerpTrans(const int16_t* ka, const int16_t* kb, int32_t t, s32* out) {
	const int32_t t0 = ka[4], t1 = kb[4];
	s32 f = t1 > t0 ? (s32)(((t - t0) << 12) / (t1 - t0)) : 0;
	if (f < 0) f = 0;
	if (f > 1 << 12) f = 1 << 12;
	for (int c = 0; c < 3; c++)
		out[c] = ((s32)ka[5 + c] << 2) + ((((s32)(kb[5 + c] - ka[5 + c]) << 2) * f) >> 12);
}

static void seqTrans(const ds::AnimSeq* s, int keyStride, uint8_t cursor, int32_t t, s32* out) {
	const uint8_t* k = seqKeys(s);
	int i = cursor;
	const int16_t* ka = (const int16_t*)(k + i * keyStride);
	const int16_t* kb = (const int16_t*)(k + (i + 1 < s->numFrames ? i + 1 : i) * keyStride);
	lerpTrans(ka, kb, t, out);
}

// Same interpolation as seqTrans, scanning keys itself (does not touch cursors).
static bool sampleTrans(const ds::AnimSeq* s, int32_t t, s32* out) {
	if (!s->hasTrans || s->numFrames < 1) return false;
	const int stride = seqKeyStride(s);
	const uint8_t* k = seqKeys(s);
	int i = 0;
	while (i + 1 < s->numFrames && (int32_t)(int16_t)(*(const int16_t*)(k + (i + 1) * stride + 8)) <= t) i++;
	const int16_t* ka = (const int16_t*)(k + i * stride);
	const int16_t* kb = (const int16_t*)(k + (i + 1 < s->numFrames ? i + 1 : i) * stride);
	lerpTrans(ka, kb, t, out);
	return true;
}

static const ds::AnimSeq* rootSeq(const Player& p, const mdl::Model* m) {
	if (!m || p.rootNode < 0 || p.rootNode >= m->numBones) return nullptr;
	const int16_t si = p.seqOfNode[p.rootNode];
	if (si < 0) return nullptr;
	return seqAt(p, si);
}

// --- public -----------------------------------------------------------------

bool play(Player& p, const mdl::Model* m, const void* danm, const char* name) {
	const uint8_t* e = findEntry(danm, name);
	if (!e || !m->nodes || m->numBones > MAX_NODES) return false;
	const ds::AnimEntry* ae = (const ds::AnimEntry*)e;
	p.seqs = e + sizeof(ds::AnimEntry);
	p.numSeqs = ae->numSeqs;
	p.time = 0;
	p.loop = 1;
	p.extractRoot = 0;
	p.rootNode = -1;
	memset(p.cursor, 0, sizeof p.cursor);
	for (int i = 0; i < MAX_NODES; i++) p.seqOfNode[i] = -1;
	for (int n = 0; n < m->numBones; n++)
		if (m->nodes[n].boneId == 0) p.rootNode = (int16_t)n;

	// seq boneTag <-> hAnim node id; also collects the anim duration (last key)
	p.duration = 0;
	const ds::AnimSeq* s = (const ds::AnimSeq*)p.seqs;
	for (int i = 0; i < p.numSeqs; i++) {
		const int16_t* last = (const int16_t*)(seqKeys(s) + (s->numFrames - 1) * seqKeyStride(s));
		if (last[4] > p.duration) p.duration = (uint16_t)last[4];
		if (s->boneTag >= 0)
			for (int n = 0; n < m->numBones; n++)
				if (m->nodes[n].boneId == s->boneTag) p.seqOfNode[n] = (int16_t)i;
		s = nextSeq(s);
	}
	if (!p.duration) p.duration = 1;
	return true;
}

bool advance(Player& p, int32_t dt60) {
	if (p.duration < 1) p.duration = 1;
	const int32_t t0 = p.time;
	p.time += dt60;
	if (p.time < p.duration) return false;
	if (p.loop) {
		p.time %= p.duration;
		memset(p.cursor, 0, p.numSeqs < MAX_NODES ? p.numSeqs : MAX_NODES);
		return true;
	}
	p.time = p.duration;
	return t0 < p.duration;
}

bool rootTranslation(const Player& p, const mdl::Model* m, int32_t time, s32 out[3]) {
	const ds::AnimSeq* s = rootSeq(p, m);
	if (!s) return false;
	return sampleTrans(s, time, out);
}

bool rootEndTranslation(const Player& p, const mdl::Model* m, s32 out[3]) {
	const ds::AnimSeq* s = rootSeq(p, m);
	if (!s || !s->hasTrans || s->numFrames < 1) return false;
	const int stride = seqKeyStride(s);
	const int16_t* k = (const int16_t*)(seqKeys(s) + (s->numFrames - 1) * stride);
	for (int c = 0; c < 3; c++) out[c] = (s32)k[5 + c] << 2;
	return true;
}

void evalWorld(const mdl::Model* m, Player& p, s32* world) {
	static s32 local[MAX_NODES * 12];
	for (int n = 0; n < m->numBones; n++) {
		s32* lm = local + n * 12;
		const int16_t si = p.seqOfNode[n];
		if (si >= 0) {
			const ds::AnimSeq* sq = seqAt(p, si);
			s32 q[4];
			seqRot(sq, seqKeyStride(sq), p.cursor[si], p.time, q);
			quatToMat(q, lm);
			if (sq->hasTrans) {
				seqTrans(sq, seqKeyStride(sq), p.cursor[si], p.time, lm + 9);
				// Locomotion lives on the root bone (boneId 0): walk/run/sprint
				// translate it by the whole stride, then the cycle snaps back.
				// Same rule as FrameUpdateCallBackSkinnedWithVelocityExtraction:
				// the XY delta is applied to the ped, and the bone keeps only
				// its vertical bob plus the bind position. Otherwise the mesh
				// walks out from under the camera and pops every loop.
				if (p.extractRoot && n == p.rootNode) {
					lm[9] = m->bonePos[n * 3];
					lm[10] = m->bonePos[n * 3 + 1];
				}
			} else {
				// bone-local rest position (same rule as RpSkin's update cb)
				lm[9] = m->bonePos[n * 3];
				lm[10] = m->bonePos[n * 3 + 1];
				lm[11] = m->bonePos[n * 3 + 2];
			}
		} else {
			memcpy(lm, m->bindLocal + n * 12, 48);
		}
	}
	// The node list is topologically sorted (parents precede children), so a
	// single pass composes local * world_parent into world space.
	for (int n = 0; n < m->numBones; n++) {
		s32* wm = world + n * 12;
		const int16_t par = m->nodes[n].parent;
		if (par >= 0) matMul(local + n * 12, world + par * 12, wm);
		else memcpy(wm, local + n * 12, 48);
	}
}

void evalBind(const mdl::Model* m, s32* world) {
	for (int n = 0; n < m->numBones; n++) {
		s32* wm = world + n * 12;
		const int16_t par = m->nodes[n].parent;
		if (par >= 0) matMul(m->bindLocal + n * 12, world + par * 12, wm);
		else memcpy(wm, m->bindLocal + n * 12, 48);
	}
}

void skinForPart(const mdl::Model* m, const s32* world, s32* skin) {
	if (!(m->flags & ds::MDL_SKINNED) || !m->nodes) return;
	// v' = v * invBone * LTM (row vectors); verts end up in hierarchy space
	// (the entity matrix places it in the world).
	for (int b = 0; b < m->numBones; b++)
		matMul(m->invBone + b * 12, world + b * 12, skin + b * 12);
}

}
