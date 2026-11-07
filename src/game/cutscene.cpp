#include "game/cutscene.h"

#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audioengine.h"
#include "game/anim.h"
#include "game/animstore.h"
#include "game/cj.h"
#include "game/collision.h"
#include "game/hud/chud.h"
#include "game/peds.h"
#include "game/pedmodel.h"
#include "game/player.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/timing.h"
#include "render/sprite2d.h"

namespace cutscene {

static const Shot* s_shots;
static uint8_t s_count;
static uint32_t s_start, s_total;
static bool s_original, s_ready, s_placed;
static int s_animBlock;
static float s_offset[3], s_root[3], s_return[4];
static uint32_t s_animTick;
struct CamKey { uint16_t time; float xyz[3]; };
static CamKey s_camera[2][64];
static uint8_t s_camCount[2];
struct Actor {
	const char* model;
	const char* animation;
	int handle = -1;
	animstore::Clip* clip = nullptr;
	anim::Player player{};
	s32 world[anim::MAX_NODES * 12]{};
};
static Actor s_actors[] = { { "tenpen", "cstenpenny" }, { "hern", "cshernandez" } };
static int s_actorStage;

static void releaseActors() {
	for (Actor& a : s_actors) {
		animstore::release(a.clip);
		pedmodel::release(a.handle);
		a.clip = nullptr;
		a.handle = -1;
	}
}

void reset() {
	releaseActors();
	s_shots = nullptr;
	s_count = 0;
	s_start = s_total = 0;
	s_original = s_ready = s_placed = false;
	s_animBlock = animstore::NO_BLOCK;
	s_actorStage = 0;
}

static void begin(uint32_t total) {
	s_start = timing::vblanks();
	s_total = total;
	chud::invalidate();
	audio::setGameActive(false);
	hud::log("cutscene: start %lu frames", (unsigned long)total);
}

bool start(const Shot* shots, uint8_t count) {
	if (active() || !shots || !count || count > 16) return false;
	uint32_t total = 0;
	for (uint8_t i = 0; i < count; i++) {
		if (!shots[i].frames) return false;
		total += shots[i].frames;
	}
	if (total > 60 * 120) return false;
	s_shots = shots;
	s_count = count;
	begin(total);
	return true;
}

void stop() {
	if (active()) {
		const bool original = s_original;
		s_original = false;
		s_shots = nullptr;
		s_count = 0;
		s_total = 0;
		if (original) {
			releaseActors();
			cj::setCutsceneBlock(animstore::NO_BLOCK);
			player::teleport(s_return[0], s_return[1], s_return[2], s_return[3]);
			col::resetScene();
		}
		chud::invalidate();
		audio::setGameActive(true);
		hud::log("cutscene: finished");
	}
}
bool active() { return s_shots || s_original; }

void update(uint32_t keysDown) {
	if (!active()) return;
	const uint32_t elapsed = timing::vblanks() - s_start;
	if (s_original && elapsed < s_total) {
		const uint32_t dt = elapsed - s_animTick;
		if (dt) {
			cj::advance(dt);
			for (Actor& a : s_actors)
				if (a.clip) anim::advance(a.player, dt);
			float x, y;
			cj::rootDelta(x, y);
			player::g.x += x;
			player::g.y += y;
			s_animTick = elapsed;
		}
	}
	if (elapsed > 12 && (keysDown & (KEY_START | KEY_A | KEY_B))) {
		hud::log("cutscene: skipped");
		stop();
	} else if (elapsed >= s_total) stop();
}

static uint8_t shotAt(uint32_t& elapsed) {
	uint8_t i = 0;
	while (i + 1 < s_count && elapsed >= s_shots[i].frames) {
		elapsed -= s_shots[i].frames;
		i++;
	}
	return i;
}

const char* caption() {
	if (!active()) return nullptr;
	if (s_original) return nullptr;
	uint32_t elapsed = timing::vblanks() - s_start;
	return s_shots[shotAt(elapsed)].caption;
}

bool view(float eye[3], float target[3]) {
	if (!active()) return false;
	uint32_t elapsed = timing::vblanks() - s_start;
	if (s_original) {
		float* out[2] = { eye, target };
		for (int track = 0; track < 2; track++) {
			const CamKey* keys = s_camera[track];
			int k = 0;
			while (k + 1 < s_camCount[track] && elapsed > keys[k + 1].time) k++;
			const int next = k + 1 < s_camCount[track] ? k + 1 : k;
			const float t = next == k || keys[next].time == keys[k].time ? 0.0f :
				(float)(elapsed - keys[k].time) / (keys[next].time - keys[k].time);
			for (int j = 0; j < 3; j++)
				out[track][j] = s_offset[j] + keys[k].xyz[j] + (keys[next].xyz[j] - keys[k].xyz[j]) * t;
		}
		return true;
	}
	const uint8_t i = shotAt(elapsed);
	const Shot& a = s_shots[i];
	const Shot& b = s_shots[i + (i + 1 < s_count ? 1 : 0)];
	const float t = i + 1 < s_count ? (float)elapsed / a.frames : 0.0f;
	for (int j = 0; j < 3; j++) {
		eye[j] = a.eye[j] + (b.eye[j] - a.eye[j]) * t;
		target[j] = a.target[j] + (b.target[j] - a.target[j]) * t;
	}
	return true;
}

static bool readTrack(FILE* f, int section) {
	char line[256];
	if (!fgets(line, sizeof line, f)) return false;
	const int n = atoi(line);
	if (n < 2 || n > 64) return false;
	for (int i = 0; i < n; i++) {
		if (!fgets(line, sizeof line, f)) return false;
		float v[4];
		char* p = line;
		for (float& value : v) {
			char* end;
			char* start = p;
			value = strtof(start, &end);
			if (end == start) return false;
			p = strchr(end, ',');
			if (!p) return false;
			p++;
		}
		if (section >= 2) {
			CamKey& k = s_camera[section - 2][i];
			if (!isfinite(v[0]) || v[0] < 0.0f || v[0] > 120.0f) return false;
			k.time = (uint16_t)(v[0] * 60.0f + 0.5f);
			for (int j = 0; j < 3; j++) {
				if (!isfinite(v[j + 1]) || fabsf(v[j + 1]) > 500.0f) return false;
				k.xyz[j] = v[j + 1];
			}
			if (i && k.time < s_camera[section - 2][i - 1].time) return false;
		}
	}
	if (section >= 2) s_camCount[section - 2] = (uint8_t)n;
	return fgets(line, sizeof line, f) && line[0] == ';';
}

bool preloadOriginal(const char* name) {
	s_ready = false;
	if (!name || strlen(name) > 16) return false;
	for (const char* p = name; *p; p++)
		if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9')) return false;
	char path[64], line[256];
	snprintf(path, sizeof path, "nitro:/anim/cuts/%s.cut", name);
	FILE* f = fopen(path, "rb");
	bool offset = false;
	while (f && fgets(line, sizeof line, f)) {
		if (sscanf(line, "offset %f %f %f", &s_offset[0], &s_offset[1], &s_offset[2]) == 3) {
			offset = true;
			break;
		}
	}
	if (f) fclose(f);
	if (!offset) return false;
	snprintf(path, sizeof path, "nitro:/anim/cuts/%s.dat", name);
	f = fopen(path, "rb");
	bool camera = f != nullptr;
	for (int i = 0; camera && i < 4; i++) camera = readTrack(f, i);
	if (f) fclose(f);
	if (!camera || !s_camCount[0] || !s_camCount[1]) return false;
	s_total = s_camera[0][s_camCount[0] - 1].time;
	if (s_total < 60 || s_total > 60 * 120) return false;
	snprintf(path, sizeof path, "nitro:/anim/cuts/%s.ifp", name);
	s_animBlock = animstore::findBlock(name);
	if (s_animBlock == animstore::NO_BLOCK) s_animBlock = animstore::openFile(path);
	animstore::Clip* clip = animstore::acquire(s_animBlock, "csplay");
	if (!clip) return false;
	const ds::AnimEntry* anim = animstore::entry(clip);
	const ds::AnimSeq* root = (const ds::AnimSeq*)(anim + 1);
	const bool valid = anim->numSeqs && root->boneTag == 0 && root->hasTrans && root->numFrames;
	if (valid) {
		const int16_t* first = (const int16_t*)(root + 1);
		for (int i = 0; i < 3; i++) s_root[i] = first[5 + i] / 1024.0f;
	}
	animstore::release(clip);
	s_ready = valid;
	hud::log("cutscene: %s %s", name, valid ? "loaded" : "invalid");
	return valid;
}

bool preloadActorsStep() {
	if (!s_ready || !s_placed) return true;
	if (!s_actorStage) {
		peds::suspendForCutscene();
		for (Actor& a : s_actors) a.handle = pedmodel::requestCutscene(a.model);
		s_actorStage = 1;
		return false;
	}
	pedmodel::update();
	bool waiting = false;
	for (const Actor& a : s_actors) {
		const pedmodel::Model* m = pedmodel::get(a.handle);
		waiting |= m && (m->state == pedmodel::STATE_PENDING || m->state == pedmodel::STATE_TEXTURES);
	}
	if (waiting && ++s_actorStage < 180) return false;
	for (Actor& a : s_actors) {
		const pedmodel::Model* m = pedmodel::get(a.handle);
		a.clip = m && pedmodel::ready(a.handle) ? animstore::acquire(s_animBlock, a.animation) : nullptr;
		if (a.clip && anim::play(a.player, m->skel, animstore::entry(a.clip))) {
			a.player.loop = 0;
			hud::log("cutscene actor %s: ready", a.model);
		} else {
			hud::log("cutscene actor %s: missing", a.model);
			animstore::release(a.clip);
			pedmodel::release(a.handle);
			a.clip = nullptr;
			a.handle = -1;
		}
	}
	return true;
}

bool placeOriginal() {
	if (!s_ready || s_placed) return false;
	s_return[0] = player::g.x;
	s_return[1] = player::g.y;
	s_return[2] = player::g.z;
	s_return[3] = player::g.heading;
	player::teleport(s_offset[0] + s_root[0], s_offset[1] + s_root[1],
	                 s_offset[2] + s_root[2], 0.0f);
	s_placed = true;
	return true;
}

bool startOriginal() {
	if (!s_ready || !s_placed || active()) return false;
	s_original = true;
	s_animTick = 0;
	cj::setCutsceneBlock(s_animBlock);
	const cj::Clip pose{ "csplay", 1.0f, false, true, false, true, 0.0f };
	cj::pose(&pose, 1);
	peds::suspendForCutscene();
	begin(s_total);
	return true;
}

void drawActors(float x, float y, float z) {
	if (!s_original) return;
	MATRIX_STORE = pedmodel::SLOT_VIEW;
	for (Actor& a : s_actors) {
		const pedmodel::Model* m = pedmodel::get(a.handle);
		if (!a.clip || !m || !pedmodel::ready(a.handle)) continue;
		mem::Scratch scratch;
		anim::Local* local = (anim::Local*)scratch.alloc(sizeof(anim::Local) * m->skel->numBones);
		if (!local) continue;
		anim::sampleLocal(m->skel, a.player, local);
		const anim::Local* poses = local;
		const s32 weight = 4096;
		anim::blendWorld(m->skel, &poses, &weight, 1, a.world);
		MATRIX_RESTORE = pedmodel::SLOT_VIEW;
		glTranslatef(s_offset[0] - x, s_offset[2] - z + m->lift, -(s_offset[1] - y));
		glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
		MATRIX_STORE = pedmodel::SLOT_ENTITY;
		pedmodel::draw(*m, a.world);
	}
	MATRIX_RESTORE = pedmodel::SLOT_VIEW;
	pedmodel::endDraw();
}

void drawOverlay() {
	if (!active()) return;
	uint32_t elapsed = timing::vblanks() - s_start;
	sprite::beginFrame();
	sprite::rect({ 0, 0, 640, 46 }, { 0, 0, 0 });
	sprite::rect({ 0, 434, 640, 480 }, { 0, 0, 0 });
	const uint32_t remaining = elapsed < s_total ? s_total - elapsed : 0;
	uint32_t fade = elapsed < 15 ? 15 - elapsed : remaining < 15 ? 15 - remaining : 0;
	if (fade) {
		sprite::alpha((uint8_t)(fade * 31 / 15));
		sprite::rect({ 0, 0, 640, 480 }, { 0, 0, 0 });
	}
	sprite::endFrame();
}

}
