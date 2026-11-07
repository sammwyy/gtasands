#include "game/anim.h"

#include <nds.h>
#include <string.h>

#include "mem/mem.h"

// All pose math is s32 (20.12) fixed point: the ARM9 has no FPU and this runs
// per bone per clip every frame. Quaternion keys come in as s16 4.12.
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

static inline int16_t keyTime(const uint8_t* k, int i, int stride) {
	return *(const int16_t*)(k + i * stride + 8);
}

// --- fixed-point pose math --------------------------------------------------

static inline s32 qmul(s32 a, s32 b) { return mulf32(a, b); }

// q (s32 unit, 4.12) -> RW rotation matrix: rows right/up/at, row-vector use
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

// Inverse of quatToMat. The stored matrix is the transpose of the usual
// column-vector quat matrix, so the off-diagonal terms are swapped.
static void matToQuat(const s32* m, s32* q) {
	const s32 one = 1 << 12;
	const s32 tr = m[0] + m[4] + m[8];
	s32 x, y, z, w, s;
	if (tr > 0) s = sqrtf32(tr + one) << 1;
	else if (m[0] > m[4] && m[0] > m[8]) s = sqrtf32(one + m[0] - m[4] - m[8]) << 1;
	else if (m[4] > m[8]) s = sqrtf32(one + m[4] - m[0] - m[8]) << 1;
	else s = sqrtf32(one + m[8] - m[0] - m[4]) << 1;
	if (s < 1) s = 1;
	if (tr > 0) {
		w = s >> 2;
		x = divf32(m[5] - m[7], s);
		y = divf32(m[6] - m[2], s);
		z = divf32(m[1] - m[3], s);
	} else if (m[0] > m[4] && m[0] > m[8]) {
		x = s >> 2;
		y = divf32(m[1] + m[3], s);
		z = divf32(m[2] + m[6], s);
		w = divf32(m[5] - m[7], s);
	} else if (m[4] > m[8]) {
		y = s >> 2;
		x = divf32(m[1] + m[3], s);
		z = divf32(m[5] + m[7], s);
		w = divf32(m[6] - m[2], s);
	} else {
		z = s >> 2;
		x = divf32(m[2] + m[6], s);
		y = divf32(m[5] + m[7], s);
		w = divf32(m[1] - m[3], s);
	}
	s32 d2 = qmul(x, x) + qmul(y, y) + qmul(z, z) + qmul(w, w);
	const s32 inv = divf32(one, sqrtf32(d2 ? d2 : one));
	q[0] = qmul(x, inv);
	q[1] = qmul(y, inv);
	q[2] = qmul(z, inv);
	q[3] = qmul(w, inv);
}

// Lerped / blended keys stay close to unit length: two Newton steps of
// 1/sqrt(d2) from 1 (x' = x (3 - d2 x^2) / 2) are exact to ~0.1% there and
// skip the divider + square root unit waits (32 bones x every pose). Far
// from 1 (opposed blends) the exact path.
static void normalize(s32* q) {
	const s32 d2 = qmul(q[0], q[0]) + qmul(q[1], q[1]) + qmul(q[2], q[2]) + qmul(q[3], q[3]);
	s32 inv;
	if (d2 > 3277 && d2 < 5120) {   // 0.8 .. 1.25
		inv = ((3 << 12) - d2) >> 1;
		inv = qmul(inv, ((3 << 12) - qmul(d2, qmul(inv, inv))) >> 1);
	} else {
		inv = divf32(1 << 12, sqrtf32(d2 > 0 ? d2 : 1 << 12));
	}
	for (int c = 0; c < 4; c++) q[c] = qmul(q[c], inv);
}

static inline s32 keyFrac(int32_t t, int32_t t0, int32_t t1) {
	if (t1 <= t0) return 0;
	s32 f = (s32)(((t - t0) << 12) / (t1 - t0));
	return f < 0 ? 0 : f > (1 << 12) ? (1 << 12) : f;
}

// Advances the node's cursor to the key at/before t, then lerps the rotation
// (shortest path). Left un-normalized: blendWorld normalizes once per bone.
static void seqRot(const ds::AnimSeq* s, uint8_t& cursor, int32_t t, s32* q) {
	const int stride = seqKeyStride(s);
	const uint8_t* k = seqKeys(s);
	int i = cursor < s->numFrames ? cursor : 0;
	while (i + 1 < s->numFrames && keyTime(k, i + 1, stride) <= t) i++;
	cursor = (uint8_t)i;
	const int16_t* ka = (const int16_t*)(k + i * stride);
	const int16_t* kb = (const int16_t*)(k + (i + 1 < s->numFrames ? i + 1 : i) * stride);
	const s32 f = keyFrac(t, ka[4], kb[4]);
	s32 dot = 0;
	for (int c = 0; c < 4; c++) dot += (int32_t)ka[c] * kb[c];
	const int32_t sgn = dot < 0 ? -1 : 1;
	for (int c = 0; c < 4; c++) q[c] = ka[c] + (((s32)(kb[c] * sgn - ka[c]) * f) >> 12);
}

// trans keys are s16 units of 1/1024 -> s32 20.12 is *4
static void lerpTrans(const ds::AnimSeq* s, const int16_t* ka, const int16_t* kb, int32_t t, s32* out) {
	const s32 f = keyFrac(t, ka[4], kb[4]);
	for (int c = 0; c < 3; c++)
		out[c] = ((s32)ka[5 + c] << (2 + s->transShift)) +
		         ((((s32)(kb[5 + c] - ka[5 + c]) << (2 + s->transShift)) * f) >> 12);
}

static void seqTrans(const ds::AnimSeq* s, uint8_t cursor, int32_t t, s32* out) {
	const int stride = seqKeyStride(s);
	const uint8_t* k = seqKeys(s);
	const int16_t* ka = (const int16_t*)(k + cursor * stride);
	const int16_t* kb = (const int16_t*)(k + (cursor + 1 < s->numFrames ? cursor + 1 : cursor) * stride);
	lerpTrans(s, ka, kb, t, out);
}

// Same interpolation as seqTrans, scanning keys itself (does not touch cursors).
static bool sampleTrans(const ds::AnimSeq* s, int32_t t, s32* out) {
	if (!s->hasTrans || s->numFrames < 1) return false;
	const int stride = seqKeyStride(s);
	const uint8_t* k = seqKeys(s);
	int i = 0;
	while (i + 1 < s->numFrames && keyTime(k, i + 1, stride) <= t) i++;
	const int16_t* ka = (const int16_t*)(k + i * stride);
	const int16_t* kb = (const int16_t*)(k + (i + 1 < s->numFrames ? i + 1 : i) * stride);
	lerpTrans(s, ka, kb, t, out);
	return true;
}

static const ds::AnimSeq* rootSeq(const Player& p, const mdl::Model* m) {
	if (!m || p.rootNode < 0 || p.rootNode >= m->numBones) return nullptr;
	return p.nodeSeq[p.rootNode];
}

// --- public -----------------------------------------------------------------

bool clipRootPose(const ds::AnimEntry* clip, int32_t time, s32 pos[3], s32 quat[4]) {
	if (!clip || !clip->numSeqs) return false;
	const ds::AnimSeq* s = (const ds::AnimSeq*)(clip + 1);
	if (!sampleTrans(s, time, pos)) return false;
	uint8_t cursor = 0;
	seqRot(s, cursor, time, quat);
	return true;
}

bool prepare(mdl::Model* m) {
	if (!m || !m->nodes || m->bindQ) return m && m->bindQ;
	m->bindQ = (s32*)mem::alloc((size_t)m->numBones * 4 * sizeof(s32), mem::TAG_MODEL);
	if (!m->bindQ) return false;
	for (int n = 0; n < m->numBones; n++) matToQuat(m->bindLocal + n * 12, m->bindQ + n * 4);
	return true;
}

bool play(Player& p, const mdl::Model* m, const ds::AnimEntry* clip) {
	if (!clip || !m || !m->nodes || m->numBones > MAX_NODES) return false;
	p.clip = clip;
	p.time = 0;
	p.loop = 1;
	p.extractRoot = 0;
	p.rootNode = -1;
	memset(p.cursor, 0, sizeof p.cursor);
	memset(p.nodeSeq, 0, sizeof p.nodeSeq);
	for (int n = 0; n < m->numBones; n++)
		if (m->nodes[n].boneId == 0) p.rootNode = (int16_t)n;

	// seq boneTag <-> hAnim node id; also collects the anim duration (last key)
	p.duration = 0;
	const ds::AnimSeq* s = (const ds::AnimSeq*)(clip + 1);
	for (int i = 0; i < clip->numSeqs; i++) {
		if (s->numFrames) {
			const int16_t last = keyTime(seqKeys(s), s->numFrames - 1, seqKeyStride(s));
			if (last > p.duration) p.duration = (uint16_t)last;
			if (s->boneTag >= 0)
				for (int n = 0; n < m->numBones; n++)
					if (m->nodes[n].boneId == s->boneTag) p.nodeSeq[n] = s;
		}
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
		memset(p.cursor, 0, sizeof p.cursor);
		return true;
	}
	p.time = p.duration;
	return t0 < p.duration;
}

bool rootTranslation(const Player& p, const mdl::Model* m, int32_t time, s32 out[3]) {
	const ds::AnimSeq* s = rootSeq(p, m);
	return s && sampleTrans(s, time, out);
}

bool rootEndTranslation(const Player& p, const mdl::Model* m, s32 out[3]) {
	const ds::AnimSeq* s = rootSeq(p, m);
	if (!s || !s->hasTrans || s->numFrames < 1) return false;
	const int16_t* k = (const int16_t*)(seqKeys(s) + (s->numFrames - 1) * seqKeyStride(s));
	for (int c = 0; c < 3; c++) out[c] = (s32)k[5 + c] << (2 + s->transShift);
	return true;
}

void setTime(Player& p, int32_t t60) {
	if (p.duration < 1) p.duration = 1;
	if (t60 < 0) t60 = 0;
	if (t60 > p.duration) t60 = p.duration;
	// A seek backwards (loop wrap, or a stop/start clip restarted) invalidates
	// the forward-only key cursors.
	if (t60 < p.time) memset(p.cursor, 0, sizeof p.cursor);
	p.time = t60;
}

float phase(const Player& p) {
	if (p.duration < 1) return 0.0f;
	return (float)p.time / (float)p.duration;
}

void sampleLocal(const mdl::Model* m, Player& p, Local* out) {
	for (int n = 0; n < m->numBones; n++) {
		Local& L = out[n];
		const ds::AnimSeq* sq = p.nodeSeq[n];
		if (sq) {
			seqRot(sq, p.cursor[n], p.time, L.q);
			if (sq->hasTrans) {
				seqTrans(sq, p.cursor[n], p.time, L.t);
				// Locomotion lives on the root bone (boneId 0): walk/run/sprint
				// translate it by the whole stride. Same rule as
				// FrameUpdateCallBackSkinnedWithVelocityExtraction: the XY
				// delta moves the ped, the bone keeps only its vertical bob.
				if (p.extractRoot && n == p.rootNode) {
					L.t[0] = m->bonePos[n * 3];
					L.t[1] = m->bonePos[n * 3 + 1];
				}
			} else {
				// bone-local rest position (same rule as RpSkin's update cb)
				L.t[0] = m->bonePos[n * 3];
				L.t[1] = m->bonePos[n * 3 + 1];
				L.t[2] = m->bonePos[n * 3 + 2];
			}
		} else {
			memcpy(L.q, m->bindQ + n * 4, sizeof L.q);
			L.t[0] = m->bindLocal[n * 12 + 9];
			L.t[1] = m->bindLocal[n * 12 + 10];
			L.t[2] = m->bindLocal[n * 12 + 11];
		}
	}
}

static void compose(const mdl::Model* m, const s32* local, s32* world) {
	// node list is topologically sorted (parents precede children)
	for (int n = 0; n < m->numBones; n++) {
		s32* wm = world + n * 12;
		const int16_t par = m->nodes[n].parent;
		if (par >= 0) matMul(local + n * 12, world + par * 12, wm);
		else memcpy(wm, local + n * 12, 48);
	}
}

void blendWorld(const mdl::Model* m, const Local* const* locals, const s32* w4096, int n, s32* world) {
	static s32 mats[MAX_NODES * 12];
	if (n <= 0 || !m->nodes) {
		evalBind(m, world);
		return;
	}
	// weights -> one reciprocal for the translation average (no per-bone divides)
	s32 wsum = 0;
	for (int i = 0; i < n; i++) wsum += w4096[i] > 0 ? w4096[i] : 0;
	if (wsum <= 0) {
		evalBind(m, world);
		return;
	}
	if (n == 1) {   // one association: no weights, no sign tests
		for (int b = 0; b < m->numBones; b++) {
			const Local& L = locals[0][b];
			s32 q[4] = { L.q[0], L.q[1], L.q[2], L.q[3] };
			s32* lm = mats + b * 12;
			normalize(q);
			quatToMat(q, lm);
			lm[9] = L.t[0];
			lm[10] = L.t[1];
			lm[11] = L.t[2];
		}
		compose(m, mats, world);
		return;
	}
	const s64 invW = ((s64)1 << 36) / wsum;     // 2^36 / wsum, applied as >> 36 after * w
	for (int b = 0; b < m->numBones; b++) {
		s32 q[4] = { 0, 0, 0, 0 };
		s64 t[3] = { 0, 0, 0 };
		const s32* ref = nullptr;
		for (int i = 0; i < n; i++) {
			const s32 w = w4096[i];
			if (w <= 0) continue;
			const Local& L = locals[i][b];
			s32 sgn = 1;
			if (!ref) ref = L.q;
			else if ((s64)ref[0] * L.q[0] + (s64)ref[1] * L.q[1] + (s64)ref[2] * L.q[2] + (s64)ref[3] * L.q[3] < 0)
				sgn = -1;
			for (int c = 0; c < 4; c++) q[c] += (sgn * L.q[c] * w) >> 12;
			for (int c = 0; c < 3; c++) t[c] += (s64)L.t[c] * w;
		}
		s32* lm = mats + b * 12;
		normalize(q);
		quatToMat(q, lm);
		for (int c = 0; c < 3; c++) lm[9 + c] = (s32)((t[c] * invW) >> 36);
	}
	compose(m, mats, world);
}

void evalBind(const mdl::Model* m, s32* world) { compose(m, m->bindLocal, world); }

void skinForPart(const mdl::Model* m, const s32* world, s32* skin) {
	if (!(m->flags & ds::MDL_SKINNED) || !m->nodes) return;
	// v' = v * invBone * LTM (row vectors); verts end up in hierarchy space
	// (the entity matrix places it in the world).
	for (int b = 0; b < m->numBones; b++)
		matMul(m->invBone + b * 12, world + b * 12, skin + b * 12);
}

}
