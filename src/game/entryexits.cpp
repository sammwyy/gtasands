#include "game/entryexits.h"

#include <math.h>
#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "assets/ds_formats.h"
#include "game/cj.h"
#include "game/collision.h"
#include "game/missions.h"
#include "game/pad.h"
#include "game/player.h"
#include "game/streaming.h"
#include "game/world.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/input.h"
#include "platform/timing.h"
#include "render/sprite2d.h"

namespace entryexits {

static ds::EntryExit* s_entries;
static uint16_t s_count;
static bool s_unlocked;
enum Phase : uint8_t { IDLE, ENTER, FADE_OUT, PRELOAD, FADE_IN };
static Phase s_phase;
static int s_outside = -1, s_wait;
static bool s_armed = true;
static bool s_reverting;
static uint8_t s_failed[128];
static bool s_virtual;
static float s_backPos[3], s_backDest[4];
static int s_backArea, s_virtualArea;
static uint32_t s_lastVbl;
static float s_dst[4];
static float s_src[4];
static float s_rearmPos[3];
static int s_dstArea, s_srcArea, s_prevOutside, s_from;
static bool s_hasDoor;
static float s_enterHeading;
static constexpr int FADE_FRAMES = 18;
static constexpr int WALK_TICKS = 12, ENTER_TICKS = 40;

static float wrap(float angle) {
	while (angle > 3.14159265f) angle -= 6.2831853f;
	while (angle < -3.14159265f) angle += 6.2831853f;
	return angle;
}

static bool inRange(const ds::EntryExit& e, float x, float y, float z) {
	const float dx = x - e.entrance[0], dy = y - e.entrance[1];
	if (fabsf(dx) > 5.0f || fabsf(dy) > 5.0f || fabsf(z - e.entrance[2]) >= 1.0f) return false;
	const float c = cosf(e.entranceAngle), s = sinf(e.entranceAngle);
	const float u = dx * c + dy * s, v = dy * c - dx * s;
	return fabsf(u) <= e.range[0] * 0.5f && fabsf(v) <= e.range[1] * 0.5f;
}

static int closest(float x, float y, float z, bool inside) {
	int best = -1;
	float dist = inside ? 625.0f : 36.0f;
	for (int i = 0; i < s_count; i++) {
		const ds::EntryExit& e = s_entries[i];
		if (e.sourceArea != world::area() || (s_failed[i >> 3] & (1u << (i & 7)))) continue;
		if (!s_unlocked && !missions::entryAllowed(e.name, e.entrance[0], e.entrance[1])) continue;
		if (!inside && !inRange(e, x, y, z)) continue;
		const float dx = x - e.entrance[0], dy = y - e.entrance[1], dz = z - e.entrance[2];
		const float d = dx * dx + dy * dy + dz * dz;
		if (d < dist) { dist = d; best = i; }
	}
	if (s_virtual && world::area() == s_virtualArea) {
		const float dx = x - s_backPos[0], dy = y - s_backPos[1], dz = z - s_backPos[2];
		const float d = dx * dx + dy * dy + dz * dz;
		if (fabsf(dz) < 1.0f && d < dist && (inside || (fabsf(dx) < 1.0f && fabsf(dy) < 1.0f)))
			best = s_count;
	}
	return best;
}

static void setPose(const char* clip, bool restart, int ticks) {
	const cj::Clip pose{ clip, 1.0f, false, false, false, restart, 0.0f };
	cj::pose(&pose, 1);
	cj::advance(ticks > 0 ? ticks : 1);
}

static void start(int i) {
	s_src[0] = player::g.x; s_src[1] = player::g.y; s_src[2] = player::g.z; s_src[3] = player::g.heading;
	s_srcArea = world::area();
	s_prevOutside = s_outside;
	s_reverting = false;
	if (i == s_count) {
		s_from = -1;
		s_dstArea = s_backArea;
		memcpy(s_dst, s_backDest, sizeof s_dst);
		memcpy(s_rearmPos, s_dst, sizeof s_rearmPos);
		s_hasDoor = false;
		s_virtual = false;
		s_phase = ENTER;
		s_wait = 0;
		s_lastVbl = timing::vblanks();
		s_armed = false;
		return;
	}
	const ds::EntryExit& e = s_entries[i];
	s_from = i;
	int link = e.link < s_count ? e.link : i;
	if (e.sourceArea != 0 && s_outside >= 0 &&
	    !strncmp(e.name, s_entries[s_outside].name, sizeof e.name))
		link = s_outside;
	const ds::EntryExit& target = s_entries[link];
	s_dstArea = target.area;
	s_dst[0] = target.exit[0];
	s_dst[1] = target.exit[1];
	s_dst[2] = target.exit[2];
	s_dst[3] = target.exitAngle * (3.14159265f / 180.0f);
	memcpy(s_rearmPos, s_dst, sizeof s_rearmPos);
	s_virtual = link == i && (s_dstArea != s_srcArea ||
	            fabsf(s_dst[0] - e.entrance[0]) + fabsf(s_dst[1] - e.entrance[1]) +
	            fabsf(s_dst[2] - e.entrance[2]) > 6.0f);
	if (s_virtual) {
		memcpy(s_backPos, s_dst, sizeof s_backPos);
		memcpy(s_backDest, s_src, sizeof s_backDest);
		s_virtualArea = s_dstArea;
		s_backArea = s_srcArea;
	}
	if (e.sourceArea == 0 && s_dstArea != 0) s_outside = i;
	s_hasDoor = world::animateDoorNear(e.entrance[0], e.entrance[1], e.entrance[2], 0.0f);
	float tx, ty;
	if (s_hasDoor && world::doorTarget(tx, ty))
		s_enterHeading = atan2f(player::g.x - tx, ty - player::g.y);
	s_phase = ENTER;
	s_wait = 0;
	s_lastVbl = timing::vblanks();
	s_armed = false;
}

bool init() {
	s_unlocked = false;
	s_phase = IDLE;
	s_outside = -1;
	s_armed = true;
	s_virtual = false;
	memset(s_failed, 0, sizeof s_failed);
	if (s_entries) return true;
	FILE* f = fopen("nitro:/data/entry_exits.denx", "rb");
	ds::EntryExitHeader h;
	if (!f) return false;
	const bool valid = fread(&h, sizeof h, 1, f) == 1 &&
	                   h.magic == DS_FOURCC('D', 'E', 'N', 'X') && h.version == 3 && h.count <= 1024;
	if (valid) {
		ds::EntryExit* entries = (ds::EntryExit*)mem::alloc(sizeof(ds::EntryExit) * h.count, mem::TAG_WORLD);
		if (entries && fread(entries, sizeof(ds::EntryExit), h.count, f) == h.count) {
			s_entries = entries;
			s_count = h.count;
		} else mem::free(entries);
	}
	fclose(f);
	return s_entries != nullptr;
}

void reset() {
	s_unlocked = false;
	s_phase = IDLE;
	s_armed = true;
	s_virtual = false;
	s_outside = -1;
	memset(s_failed, 0, sizeof s_failed);
	world::clearDoorAnimation();
}
bool unlockAll() {
	if (!s_entries || !world::ready() || !col::available() || !streaming::g_interiors.isOpen()) return false;
	s_unlocked = true;
	return true;
}
bool unlocked() { return s_unlocked; }
int count() { return s_count; }
int exteriorCount() {
	int n = 0;
	for (int i = 0; i < s_count; i++) n += s_entries[i].sourceArea == 0;
	return n;
}

bool busy() { return s_phase != IDLE; }

bool update() {
	if (s_phase == IDLE) {
		if (!world::ready() || (!s_unlocked && !missions::completed())) return true;
		const player::State& p = player::g;
		if (!s_armed) {
			const float dx = p.x - s_rearmPos[0], dy = p.y - s_rearmPos[1];
			s_armed = dx * dx + dy * dy >= 3.5f * 3.5f || fabsf(p.z - s_rearmPos[2]) >= 2.5f;
			if (!s_armed) return true;
		}
		const int i = closest(p.x, p.y, p.z, false);
		if (i >= 0 && !p.inVehicle && p.health > 0.0f) start(i);
		return s_phase == IDLE;
	}
	input::consumeEdges();
	pad::consumeEdges();
	const uint32_t now = timing::vblanks();
	const int elapsed = (int)(now - s_lastVbl);
	s_lastVbl = now;
	if (s_phase == ENTER) {
		const int ticks = elapsed > 4 ? 4 : elapsed;
		if (s_hasDoor && s_wait < WALK_TICKS) {
			const float t = ticks < WALK_TICKS - s_wait ? (float)ticks / (float)(WALK_TICKS - s_wait) : 1.0f;
			player::faceEntry(player::g.heading + wrap(s_enterHeading - player::g.heading) * t);
		}
		setPose(s_wait < WALK_TICKS ? cj::motion().walk : "Walk_DoorPartial", s_wait == 0, ticks);
		if (s_hasDoor) {
			const int open = s_wait - WALK_TICKS;
			world::animateDoorNear(0.0f, 0.0f, 0.0f, open <= 0 ? 0.0f : open >= 28 ? 90.0f : open * (90.0f / 28.0f));
		}
		s_wait += ticks;
		if (s_wait >= ENTER_TICKS) { s_phase = FADE_OUT; s_wait = 0; }
	} else if (s_phase == FADE_OUT) {
		s_wait += elapsed > 4 ? 4 : elapsed;
		if (s_wait >= FADE_FRAMES) {
			world::clearDoorAnimation();
			world::setArea(s_dstArea);
			col::resetScene();
			player::teleport(s_dst[0], s_dst[1], s_dst[2], s_dst[3]);
			s_phase = PRELOAD;
			s_wait = 0;
		}
	} else if (s_phase == PRELOAD) {
		const player::State& p = player::g;
		const bool worldReady = world::preloadAreaStep(p.x, p.y, p.z);
		const bool colReady = col::preloadAreaStep(p.x, p.y, p.z);
		s_wait += elapsed > 4 ? 4 : elapsed;
		col::Hit hit;
		const bool floorReady = worldReady && colReady &&
		                        col::groundAt(p.x, p.y, p.z + 4.0f, p.z - 30.0f, hit);
		if (floorReady || (s_reverting && worldReady && colReady)) {
			if (floorReady) player::placeOnGround();
			s_phase = FADE_IN;
			s_wait = FADE_FRAMES;
		} else if (s_wait > 600 && !s_reverting) {
			hud::log("Entry %d: area %d not ready; returning", s_from, s_dstArea);
			if (s_from >= 0) s_failed[s_from >> 3] |= 1u << (s_from & 7);
			s_outside = s_prevOutside;
			s_virtual = false;
			s_reverting = true;
			world::setArea(s_srcArea);
			col::resetScene();
			player::teleport(s_src[0], s_src[1], s_src[2], s_src[3]);
			s_wait = 0;
		}
	} else {
		s_wait -= elapsed > 4 ? 4 : elapsed;
		if (s_wait <= 0) {
			s_phase = IDLE;
			s_wait = 0;
			if (s_dstArea == 0) s_outside = -1;
		}
	}
	return false;
}

void drawMarker(float originX, float originY, float originZ) {
	if ((!s_unlocked && !missions::completed()) || s_phase != IDLE || !s_armed) return;
	const player::State& p = player::g;
	const int i = closest(p.x, p.y, p.z, true);
	if (i < 0) return;
	ds::EntryExit proxy{};
	if (i == s_count) {
		memcpy(proxy.entrance, s_backPos, sizeof s_backPos);
		proxy.range[0] = proxy.range[1] = 2.0f;
	}
	const ds::EntryExit& e = i == s_count ? proxy : s_entries[i];
	glPushMatrix();
	glTranslatef(e.entrance[0] - originX, e.entrance[2] - originZ, originY - e.entrance[1]);
	glBindTexture(0, 0);
	glPolyFmt(POLY_ALPHA(11) | POLY_CULL_BACK);
	glColor(RGB15(31, 27, 0));
	glBegin(GL_QUADS);
	for (int a = 0; a < 8; a++) {
		const int t0 = a * 4096, t1 = (a + 1) * 4096;
		const v16 x0 = (v16)(sinLerp(t0) / 2), z0 = (v16)(cosLerp(t0) / 2);
		const v16 x1 = (v16)(sinLerp(t1) / 2), z1 = (v16)(cosLerp(t1) / 2);
		glVertex3v16(x0, 0, z0);
		glVertex3v16(x1, 0, z1);
		glVertex3v16(x1, floattov16(1.2f), z1);
		glVertex3v16(x0, floattov16(1.2f), z0);
	}
	glEnd();
	glEnable(GL_TEXTURE_2D);
	glPopMatrix(1);
}

void drawFade() {
	if (s_phase != FADE_OUT && s_phase != PRELOAD && s_phase != FADE_IN) return;
	sprite::beginFrame();
	const int alpha = s_phase == PRELOAD ? 31 : s_wait * 31 / FADE_FRAMES;
	sprite::alpha(alpha);
	sprite::rect({ 0, 0, 640, 480 }, { 0, 0, 0 });
	sprite::endFrame();
}


}
