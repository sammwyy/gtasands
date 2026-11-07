#include "game/cutscene.h"

#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audioengine.h"
#include "audio/aestream.h"
#include "game/anim.h"
#include "game/animstore.h"
#include "game/cj.h"
#include "game/collision.h"
#include "game/hud/chud.h"
#include "game/peds.h"
#include "game/pedmodel.h"
#include "game/player.h"
#include "game/text/gxt.h"
#include "game/vehicle.h"
#include "game/vehicledata.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/timing.h"
#include "render/sprite2d.h"

namespace cutscene {

static const Shot* s_shots;
static uint8_t s_count;
static uint32_t s_start, s_total;
static bool s_original, s_ready, s_placed, s_haveReturn, s_next;
static uint8_t s_stage;
static int s_animBlock;
static float s_offset[3], s_root[3], s_return[4];
static uint32_t s_animTick;
struct CamKey { uint16_t time; float xyz[3], incoming[3], outgoing[3]; bool inValid, outValid; };
static CamKey s_camera[2][64];
static uint8_t s_camCount[2];
struct Subtitle { uint16_t from, to; char key[16]; };
static Subtitle s_subtitles[32];
static uint8_t s_subCount;
struct ActorDef { const char* model; const char* animation; bool cutsceneModel; };
static const ActorDef s_airportActors[] = {
	{ "csstew", "csstew", true }, { "csbogman", "csbogman", true },
	{ "cssuitcase", "cssuitcase-1", true }, { "cssuitcase", "cssuitcase-2", true }
};
static const ActorDef s_groveActors[] = {
	{ "tenpen", "cstenpenny", false }, { "hern", "cshernandez", false },
	{ "pulaski", "cspulaski", false }, { "cs9mm", "cs9mm-1", true },
	{ "cs9mm", "cs9mm-2", true }, { "cshandcuff", "cshandcuff", true },
	{ "csmoneystck", "csmoneystck", true }, { "csgizmo", "csgizmo", true }
};
struct Actor {
	const char* model;
	const char* animation;
	bool cutsceneModel;
	int handle = -1;
	animstore::Clip* clip = nullptr;
	anim::Player player{};
	s32 world[anim::MAX_NODES * 12]{};
};
static Actor s_actors[8];
static uint8_t s_numActors;
static int s_actorStage;
static uint32_t s_actorWaitAt;
struct CarActor {
	const char* model;
	const char* animation;
	int id = -1;
	animstore::Clip* clip = nullptr;
};
static CarActor s_cars[2];
static uint8_t s_numCars;
static int s_carStage;
static uint32_t s_carWaitAt;

static void positionCar(CarActor& car, uint32_t time) {
	if (car.id < 0 || !car.clip) return;
	s32 pos[3], quat[4];
	if (!anim::clipRootPose(animstore::entry(car.clip), time, pos, quat)) return;
	veh::positionCutscene(car.id, s_offset[0] + pos[0] / 4096.0f,
	                      s_offset[1] + pos[1] / 4096.0f,
	                      s_offset[2] + pos[2] / 4096.0f,
	                      2.0f * atan2f((float)quat[2], (float)quat[3]));
}

static void releaseCars() {
	for (CarActor& car : s_cars) {
		if (car.id >= 0) veh::remove(car.id);
		animstore::release(car.clip);
		car.id = -1;
		car.clip = nullptr;
	}
	s_carStage = 0;
	s_numCars = 0;
}

static void releaseActors() {
	for (Actor& a : s_actors) {
		animstore::release(a.clip);
		pedmodel::release(a.handle);
		a.clip = nullptr;
		a.handle = -1;
	}
}

void reset() {
	releaseCars();
	releaseActors();
	s_shots = nullptr;
	s_count = 0;
	s_start = s_total = 0;
	s_original = s_ready = s_placed = false;
	s_haveReturn = s_next = false;
	s_stage = s_subCount = 0;
	s_numActors = 0;
	s_animBlock = animstore::NO_BLOCK;
	s_actorStage = 0;
}

static void begin(uint32_t total) {
	s_start = timing::vblanks();
	s_total = total;
	chud::invalidate();
	audio::setGameActive(false);
	if (s_original) aestream::play(s_stage == 0 ? 706 : 708, 0.0f, false, false);
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
		const bool follow = original && s_stage == 0 && timing::vblanks() - s_start >= s_total;
		s_original = false;
		s_shots = nullptr;
		s_count = 0;
		s_total = 0;
		if (original) {
			aestream::stop();
			releaseCars();
			releaseActors();
			s_actorStage = 0;
			cj::setCutsceneBlock(animstore::NO_BLOCK);
			player::teleport(s_return[0], s_return[1], s_return[2], s_return[3]);
			col::resetScene();
			s_ready = s_placed = false;
			s_next = follow;
		}
		chud::invalidate();
		audio::setGameActive(true);
		hud::log("cutscene: finished");
	}
}
bool active() { return s_shots || s_original; }
bool takeNextOriginal() { const bool next = s_next; s_next = false; return next; }
bool originalReady() { return s_ready && s_placed; }

void update(uint32_t keysDown) {
	if (!active()) return;
	const uint32_t elapsed = timing::vblanks() - s_start;
	if (s_original && elapsed < s_total) {
		const uint32_t dt = elapsed - s_animTick;
		if (dt) {
			cj::advance(dt);
			for (Actor& a : s_actors)
				if (a.clip) anim::advance(a.player, dt);
			for (int i = 0; i < s_numCars; i++) positionCar(s_cars[i], elapsed);
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
	uint32_t elapsed = timing::vblanks() - s_start;
	if (s_original) {
		for (int i = 0; i < s_subCount; i++)
			if (elapsed >= s_subtitles[i].from && elapsed < s_subtitles[i].to)
				return (const char*)TheText.get(s_subtitles[i].key);
		return nullptr;
	}
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
			const bool spline = next != k && keys[k].outValid && keys[next].inValid;
			const float u = 1.0f - t;
			for (int j = 0; j < 3; j++)
				out[track][j] = s_offset[j] + (spline ?
					u * u * u * keys[k].xyz[j] + 3.0f * u * u * t * keys[k].outgoing[j] +
					3.0f * u * t * t * keys[next].incoming[j] + t * t * t * keys[next].xyz[j] :
					keys[k].xyz[j] + (keys[next].xyz[j] - keys[k].xyz[j]) * t);
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
		float v[10];
		char* p = line;
		for (int j = 0; j < (section >= 2 ? 10 : 4); j++) {
			char* end;
			char* start = p;
			v[j] = strtof(start, &end);
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
			k.inValid = k.outValid = true;
			for (int j = 0; j < 3; j++) {
				k.incoming[j] = v[j + 4];
				k.outgoing[j] = v[j + 7];
				k.inValid &= isfinite(k.incoming[j]) && fabsf(k.incoming[j]) <= 500.0f;
				k.outValid &= isfinite(k.outgoing[j]) && fabsf(k.outgoing[j]) <= 500.0f;
			}
			if (i && k.time < s_camera[section - 2][i - 1].time) return false;
		}
	}
	if (section >= 2) s_camCount[section - 2] = (uint8_t)n;
	return fgets(line, sizeof line, f) && line[0] == ';';
}

bool preloadOriginal(const char* name) {
	s_ready = false;
	s_camCount[0] = s_camCount[1] = s_subCount = 0;
	if (!name || strlen(name) > 16) return false;
	for (const char* p = name; *p; p++)
		if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9')) return false;
	char path[64], line[256];
	s_stage = strcmp(name, "prolog1") == 0 ? 0 : 1;
	snprintf(path, sizeof path, "nitro:/anim/cuts/%s.cut", name);
	FILE* f = fopen(path, "rb");
	bool offset = false;
	bool inText = false;
	while (f && fgets(line, sizeof line, f)) {
		if (sscanf(line, "offset %f %f %f", &s_offset[0], &s_offset[1], &s_offset[2]) == 3)
			offset = true;
		if (!strncmp(line, "text", 4)) inText = true;
		else if (inText && !strncmp(line, "end", 3)) inText = false;
		else if (inText && s_subCount < 32) {
			unsigned from, duration;
			char key[16];
			if (sscanf(line, "%u,%u,%15[A-Za-z0-9@_]", &from, &duration, key) == 3 &&
			    from < 120000 && duration < 120000 && from + duration <= 120000) {
				Subtitle& sub = s_subtitles[s_subCount++];
				sub.from = (uint16_t)(from * 60 / 1000);
				sub.to = (uint16_t)((from + duration) * 60 / 1000);
				snprintf(sub.key, sizeof sub.key, "%s", key);
			}
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
		for (int i = 0; i < 3; i++) s_root[i] = ((int32_t)first[5 + i] << root->transShift) / 1024.0f;
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
		const ActorDef* defs = s_stage == 0 ? s_airportActors : s_groveActors;
		s_numActors = s_stage == 0 ? 4 : 8;
		for (int i = 0; i < s_numActors; i++) {
			Actor& a = s_actors[i];
			a.model = defs[i].model;
			a.animation = defs[i].animation;
			a.cutsceneModel = defs[i].cutsceneModel;
			a.handle = pedmodel::requestCutscene(a.model, a.cutsceneModel);
		}
		s_actorStage = 1;
		s_actorWaitAt = timing::vblanks();
		return false;
	}
	pedmodel::update();
	bool waiting = false;
	for (int i = 0; i < s_numActors; i++) {
		const Actor& a = s_actors[i];
		const pedmodel::Model* m = pedmodel::get(a.handle);
		waiting |= m && (m->state == pedmodel::STATE_PENDING || m->state == pedmodel::STATE_TEXTURES);
	}
	if (waiting && timing::vblanks() - s_actorWaitAt < 180) return false;
	for (int i = 0; i < s_numActors; i++) {
		Actor& a = s_actors[i];
		const pedmodel::Model* m = pedmodel::get(a.handle);
		a.clip = m && pedmodel::ready(a.handle) ? animstore::acquire(s_animBlock, a.animation) : nullptr;
		if (a.clip && anim::play(a.player, m->skel, animstore::entry(a.clip))) {
			a.player.loop = 0;
			hud::log("cutscene actor %s: ready", a.model);
		} else {
			hud::log("cutscene actor %s: missing h=%d state=%d clip=%d",
			         a.model, a.handle, m ? m->state : -1, a.clip != nullptr);
			animstore::release(a.clip);
			pedmodel::release(a.handle);
			a.clip = nullptr;
			a.handle = -1;
		}
	}
	return true;
}

bool preloadVehiclesStep() {
	if (!s_ready || !s_placed) return true;
	if (!s_carStage) {
		static const CarActor airport[] = { { "cstaxi92", "cstaxi92" } };
		static const CarActor grove[] = {
			{ "cstaxi92", "cstaxi92" }, { "cscopcarla92", "cscopcarla92" }
		};
		const CarActor* defs = s_stage == 0 ? airport : grove;
		s_numCars = s_stage == 0 ? 1 : 2;
		for (int i = 0; i < s_numCars; i++) {
			CarActor& car = s_cars[i];
			car.model = defs[i].model;
			car.animation = defs[i].animation;
			car.clip = animstore::acquire(s_animBlock, car.animation);
			const int model = !strcmp(car.model, "cstaxi92") ? 420 : 596;
			const int index = vehdata::findModel(model);
			if (car.clip && index >= 0)
				car.id = veh::spawn(index, s_offset[0], s_offset[1], s_offset[2], 0.0f, -1, car.model);
			positionCar(car, 0);
		}
		s_carStage = 1;
		s_carWaitAt = timing::vblanks();
		return false;
	}
	vehmdl::update();
	bool waiting = false;
	for (int i = 0; i < s_numCars; i++) {
		CarActor& car = s_cars[i];
		if (car.id < 0) continue;
		const veh::Vehicle* v = veh::get(car.id);
		const vehmdl::Model* m = v ? vehmdl::get(v->model) : nullptr;
		if (m && (m->state == vehmdl::STATE_PENDING || m->state == vehmdl::STATE_TEXTURES)) waiting = true;
		else if (!veh::prepareCutscene(car.id)) {
			hud::log("cutscene car %s: missing", car.model);
			veh::remove(car.id);
			car.id = -1;
		}
	}
	if (waiting && timing::vblanks() - s_carWaitAt < 180) return false;
	if (waiting)
		for (int i = 0; i < s_numCars; i++) {
			CarActor& car = s_cars[i];
			const veh::Vehicle* v = veh::get(car.id);
			if (!v || vehmdl::ready(v->model)) continue;
			hud::log("cutscene car %s: timed out", car.model);
			veh::remove(car.id);
			car.id = -1;
		}
	for (int i = 0; i < s_numCars; i++)
		if (s_cars[i].id >= 0) hud::log("cutscene car %s: ready", s_cars[i].model);
	return true;
}

bool placeOriginal() {
	if (!s_ready || s_placed) return false;
	if (!s_haveReturn) {
		s_return[0] = player::g.x;
		s_return[1] = player::g.y;
		s_return[2] = player::g.z;
		s_return[3] = player::g.heading;
		s_haveReturn = true;
	}
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
	float eye[3], target[3];
	if (!view(eye, target)) return;
	const float dir[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
	const float dirLength = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
	MATRIX_STORE = pedmodel::SLOT_VIEW;
	for (Actor& a : s_actors) {
		const pedmodel::Model* m = pedmodel::get(a.handle);
		if (!a.clip || !m || !pedmodel::ready(a.handle)) continue;
		s32 root[3];
		if (dirLength > 0.001f && anim::rootTranslation(a.player, m->skel, a.player.time, root)) {
			const float dx = s_offset[0] + root[0] / 4096.0f - eye[0];
			const float dy = s_offset[1] + root[1] / 4096.0f - eye[1];
			const float dz = s_offset[2] + root[2] / 4096.0f - eye[2];
			const float forward = (dx * dir[0] + dy * dir[1] + dz * dir[2]) / dirLength;
			const float radius = forward * 1.4f + 3.0f;
			if (forward < -3.0f || forward > 100.0f ||
			    dx * dx + dy * dy + dz * dz - forward * forward > radius * radius) continue;
		}
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
