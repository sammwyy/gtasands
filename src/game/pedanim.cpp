#include "game/pedanim.h"

#include <string.h>

#include "game/animstore.h"
#include "mem/mem.h"

namespace pedanim {

static constexpr float BLEND_OUT_PER_TICK = 8.0f / 60.0f;

static int pedBlock() {
	static int s_block = animstore::NO_BLOCK;
	if (s_block == animstore::NO_BLOCK) s_block = animstore::findBlock("ped");
	return s_block;
}

bool exists(const char* name) { return name && name[0] && animstore::hasClip(pedBlock(), name); }

static void kill(Slot& sl) {
	animstore::release(sl.clip);
	sl.clip = nullptr;
	sl.alive = 0;
	sl.weight = 0.0f;
	sl.hash = 0;
}

void reset(Set& s) {
	for (Slot& sl : s.slots) kill(sl);
	s.rootX = s.rootY = 0.0f;
}

static int findSlot(const Set& s, uint32_t h) {
	for (int i = 0; i < SLOTS; i++)
		if (s.slots[i].alive && s.slots[i].hash == h) return i;
	return -1;
}

static bool play(Slot& sl, const mdl::Model* skel, const char* name) {
	const int b = pedBlock();
	if (!skel || !animstore::hasClip(b, name)) return false;
	animstore::Clip* c = animstore::acquire(b, name);
	if (c && anim::play(sl.anim, skel, animstore::entry(c))) {
		animstore::release(sl.clip);
		sl.clip = c;
		return true;
	}
	animstore::release(c);
	return false;
}

void pose(Set& s, const mdl::Model* skel, const cj::Clip* clips, int n, int dt60) {
	uint8_t keep[SLOTS] = {};
	for (int i = 0; i < n; i++) {
		const cj::Clip& c = clips[i];
		if (!c.name || c.weight <= 0.0f) continue;
		const uint32_t h = ds::fnv1a(c.name);
		int k = findSlot(s, h);
		if (k < 0) {
			for (int j = 0; j < SLOTS && k < 0; j++)
				if (!s.slots[j].alive) k = j;
			if (k < 0) {
				k = 0;
				for (int j = 1; j < SLOTS; j++)
					if (s.slots[j].weight < s.slots[k].weight) k = j;
			}
			if (!play(s.slots[k], skel, c.name)) continue;
			s.slots[k].hash = h;
			s.slots[k].alive = 1;
		}
		Slot& sl = s.slots[k];
		sl.weight = c.weight;
		sl.anim.loop = c.loop ? 1 : 0;
		sl.anim.extractRoot = c.extract ? 1 : 0;
		if (c.restart || c.sync) {
			int32_t t = (int32_t)(c.phase * (float)sl.anim.duration + 0.5f);
			if (t < 0) t = 0;
			if (t > sl.anim.duration) t = sl.anim.duration;
			if (c.restart || t + 1 < sl.anim.time || t > sl.anim.time + 1) anim::setTime(sl.anim, t);
		}
		keep[k] = 1;
	}
	for (int i = 0; i < SLOTS; i++) {
		Slot& sl = s.slots[i];
		if (!sl.alive || keep[i]) continue;
		sl.weight -= BLEND_OUT_PER_TICK * (float)dt60;
		if (sl.weight <= 0.0f) kill(sl);
	}
}

float phase(const Set& s, const char* name) {
	const int k = findSlot(s, ds::fnv1a(name));
	return k < 0 ? -1.0f : anim::phase(s.slots[k].anim);
}

bool ended(const Set& s, const char* name) {
	const int k = findSlot(s, ds::fnv1a(name));
	if (k < 0) return false;
	const anim::Player& p = s.slots[k].anim;
	return !p.loop && p.time >= p.duration;
}

bool playing(const Set& s, const char* name) { return findSlot(s, ds::fnv1a(name)) >= 0; }

int time(const Set& s, const char* name) {
	const int k = findSlot(s, ds::fnv1a(name));
	return k < 0 ? -1 : (int)s.slots[k].anim.time;
}

// cj::advance: weighted root deltas, the end key closes a looping seam
void advance(Set& s, const mdl::Model* skel, int dt60) {
	s.rootX = s.rootY = 0.0f;
	if (!skel) return;
	float dx = 0, dy = 0, wsum = 0;
	for (Slot& sl : s.slots) {
		if (!sl.alive) continue;
		s32 before[3], after[3], end[3];
		const int32_t t0 = sl.anim.time;
		const bool take = sl.anim.extractRoot && sl.weight > 0.0f && anim::rootTranslation(sl.anim, skel, t0, before);
		const bool wrapped = anim::advance(sl.anim, dt60);
		if (!take || !anim::rootTranslation(sl.anim, skel, sl.anim.time, after)) continue;
		s32 ddx = after[0] - before[0], ddy = after[1] - before[1];
		if (wrapped && sl.anim.loop && anim::rootEndTranslation(sl.anim, skel, end)) {
			ddx += end[0];
			ddy += end[1];
		}
		dx += sl.weight * (float)ddx;
		dy += sl.weight * (float)ddy;
		wsum += sl.weight;
	}
	if (wsum > 1.0f) {
		dx /= wsum;
		dy /= wsum;
	}
	s.rootX = dx / 4096.0f;
	s.rootY = dy / 4096.0f;
}

void evaluate(Set& s, const mdl::Model* skel, s32* world) {
	mem::Scratch scratch;
	anim::Local* locals = (anim::Local*)scratch.alloc(sizeof(anim::Local) * SLOTS * skel->numBones);
	const anim::Local* ptrs[SLOTS];
	s32 w[SLOTS];
	int n = 0;
	for (int i = 0; locals && i < SLOTS; i++) {
		Slot& sl = s.slots[i];
		if (!sl.alive || sl.weight <= 0.0f) continue;
		anim::Local* l = locals + n * skel->numBones;
		anim::sampleLocal(skel, sl.anim, l);
		ptrs[n] = l;
		w[n] = (s32)(sl.weight * 4096.0f + 0.5f);
		n++;
	}
	anim::blendWorld(skel, ptrs, w, n, world);
}

static Set* s_cur;
static const mdl::Model* s_curSkel;
static int s_curDt = 1;

void bind(Set* s, const mdl::Model* skel, int dt60) {
	s_cur = s;
	s_curSkel = skel;
	s_curDt = dt60;
}
void apiPose(const cj::Clip* clips, int n) { if (s_cur) pose(*s_cur, s_curSkel, clips, n, s_curDt); }
float apiPhase(const char* name) { return s_cur ? phase(*s_cur, name) : -1.0f; }
bool apiEnded(const char* name) { return s_cur && ended(*s_cur, name); }

}
