#include "game/missions.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game/collision.h"
#include "game/cutscene.h"
#include "game/hud/radar.h"
#include "game/player.h"
#include "game/vehicle.h"
#include "game/vehicledata.h"
#include "game/world.h"
#include "hud/hud.h"
#include "platform/timing.h"
#include "render/sprite2d.h"

namespace missions {

static constexpr uint32_t MAGIC = 0x53494d47;
static constexpr uint16_t VERSION = 2;
static constexpr int BLIP = 15, BMX_MODEL = 481;
static constexpr float HOUSE_X = 2495.33f, HOUSE_Y = -1689.183f;

struct Save {
	uint32_t magic;
	uint16_t version;
	uint8_t passed, reserved;
	int32_t money;
	uint32_t check;
};

static uint8_t s_passed, s_objective;
static bool s_ready, s_warping, s_active, s_failed;
static int s_bike = -1;
static uint32_t s_retryAt, s_warpAt;
static const cutscene::Shot FUNERAL[] = {
	{ { 947.9119f, -1103.912f, 23.4476f }, { 946.9478f, -1103.822f, 23.6977f }, 90, "SWEET & KENDL" },
	{ { 952.0f, -1110.0f, 29.0f }, { 970.0f, -1108.0f, 23.0f }, 150, "GET BACK TO GROVE STREET" },
	{ { 967.0f, -1114.0f, 25.5f }, { 971.0f, -1108.0f, 23.0f }, 60, "GET ON THE BIKE" }
};

static uint32_t checksum(const Save& s) {
	return s.magic ^ ((uint32_t)s.version << 16 | (uint32_t)s.passed << 8 | s.reserved) ^
	       (uint32_t)s.money ^ 0xa62b317du;
}

static FILE* openSave(const char* mode) {
	FILE* f = fopen("sd:/gtasads_missions.dat", mode);
	if (!f) f = fopen("fat:/gtasads_missions.dat", mode);
	return f;
}

static bool scriptPresent(FILE* f, uint32_t offset, const char* name) {
	uint8_t op[11];
	return fseek(f, offset, SEEK_SET) == 0 && fread(op, 1, sizeof op, f) == sizeof op &&
	       op[0] == 0xa4 && op[1] == 0x03 && op[2] == 9 &&
	       !strncmp((const char*)op + 3, name, 8);
}

static bool checkScripts() {
	FILE* f = fopen("nitro:/data/script/main.scm", "rb");
	if (!f) return false;
	uint8_t chunk[24];
	bool ok = fread(chunk, 1, 8, f) == 8 &&
	          chunk[0] == 2 && chunk[1] == 0 && chunk[2] == 1 && chunk[7] == 's';
	uint32_t pos = 0;
	for (int i = 0; ok && i < 2; i++) {
		pos = (uint32_t)chunk[3] | (uint32_t)chunk[4] << 8 |
		      (uint32_t)chunk[5] << 16 | (uint32_t)chunk[6] << 24;
		ok = fseek(f, pos, SEEK_SET) == 0 && fread(chunk, 1, 8, f) == 8 &&
		     chunk[0] == 2 && chunk[1] == 0 && chunk[2] == 1 && chunk[7] == i;
	}
	if (ok) {
		ok = fread(chunk + 8, 1, sizeof chunk - 8, f) == sizeof chunk - 8 &&
		     chunk[7] == 1 && chunk[16] >= 12;
	}
	uint32_t offsets[12];
	if (ok) ok = fread(offsets, sizeof offsets, 1, f) == 1 &&
	             scriptPresent(f, offsets[2], "INTRO") &&
	             scriptPresent(f, offsets[11], "INTRO1");
	fclose(f);
	return ok;
}

static void save() {
	Save s{ MAGIC, VERSION, s_passed, 0, player::g.money, 0 };
	s.check = checksum(s);
	FILE* f = openSave("wb");
	if (!f) {
		hud::log("missions: no writable SD; progress in RAM");
		return;
	}
	const bool ok = fwrite(&s, sizeof s, 1, f) == 1 && fflush(f) == 0;
	fclose(f);
	if (!ok) hud::log("missions: SD write failed");
}

static void target() {
	if (!s_ready || s_failed || s_warping || s_passed == 2) {
		radar::clearBlip(BLIP);
		return;
	}
	if (!s_active) {
		radar::setBlip(BLIP, HOUSE_X, HOUSE_Y, "", 255, 220, 40);
		return;
	}
	static const float route[][2] = {
		{ 971.8f, -1108.0f }, { 1644.773f, -1051.334f },
		{ 1891.0f, -1347.78f }, { 2493.82f, -1669.91f }
	};
	radar::setBlip(BLIP, route[s_objective][0], route[s_objective][1], "", 255, 220, 40);
}

void init() {
	s_passed = s_objective = 0;
	s_bike = -1;
	s_warping = s_active = s_failed = false;
	s_ready = checkScripts();
	if (!s_ready) {
		hud::log("missions: stock INTRO/INTRO1 SCM missing");
		radar::clearBlip(BLIP);
		return;
	}
	FILE* f = openSave("rb");
	if (f) {
		Save s{};
		const bool valid = fread(&s, sizeof s, 1, f) == 1 && fgetc(f) == EOF &&
		                   s.magic == MAGIC && s.version == VERSION && s.passed <= 2 &&
		                   s.reserved == 0 && s.money >= 0 && s.money <= 99999999 &&
		                   s.check == checksum(s);
		fclose(f);
		if (valid) {
			s_passed = s.passed;
			player::g.money = s.money;
		} else hud::log("missions: invalid save; starting prologue");
	} else {
		player::g.money = 350;
#ifndef GTASADS_QUICKTEST
		player::teleport(2239.367f, -1261.939f, 23.9f, 4.76f);
		player::setClock(6, 30);
#endif
	}
	target();
	hud::log("SCM mission 2 INTRO, mission 11 INTRO1: %u passed", s_passed);
}

uint8_t completed() { return s_passed; }
uint8_t objective() { return s_objective; }
const char* title() {
	if (!s_ready) return "SCM NOT AVAILABLE";
	return s_passed == 0 ? "IN THE BEGINNING" : s_passed == 1 ? "BIG SMOKE" : "RYDER - NOT PORTED";
}
const char* instruction() {
	if (!s_ready) return "Check main.scm";
	if (s_failed) return "Mission failed - retrying";
	if (s_warping) return "Loading Jefferson";
	if (s_passed == 0) return "Get to Grove Street";
	if (s_passed == 2) return "Next SCM mission: INTRO2";
	if (!s_active) return "Visit CJ's house";
	static const char* const text[] = {
		"Get on the BMX (Y)", "Ride to East LS", "Follow the gang home", "Reach Grove Street"
	};
	return text[s_objective];
}

bool entryAllowed(const char* name, float, float) {
	return s_ready && s_passed >= 2 && !strncmp(name, "carls", 8);
}

bool pointAllowed(float x, float y) {
	if (s_passed < 2) return false;
	const float dx = x - HOUSE_X, dy = y - HOUSE_Y;
	return dx * dx + dy * dy < 20.0f * 20.0f;
}

static void clearBike() {
	if (s_bike >= 0) {
		if (player::vehicleId() == s_bike) player::leaveVehicleNow();
		veh::remove(s_bike);
		s_bike = -1;
	}
}

static void fail() {
	hud::log("mission failed: Big Smoke");
	s_failed = true;
	s_retryAt = timing::vblanks() + 180;
	target();
}

static void beginSmoke() {
	s_active = s_warping = true;
	s_objective = 0;
	s_warpAt = timing::vblanks();
	player::setClock(9, 0);
	player::teleport(970.0873f, -1107.776f, 23.9f, 4.44f);
	col::resetScene();
	hud::log("SCM mission 11 INTRO1: Big Smoke");
	target();
}

void update() {
	if (!s_ready || s_passed >= 2) return;
	if (s_failed) {
		if (player::g.health <= 0.0f || (int32_t)(timing::vblanks() - s_retryAt) < 0) return;
		clearBike();
		s_failed = s_warping = s_active = false;
		s_objective = 0;
		player::respawn();
		target();
		return;
	}
	if (player::g.health <= 0.0f) {
		if (s_active && !s_warping) fail();
		return;
	}
	if (s_warping) {
		const bool ready = world::preloadAreaStep(player::g.x, player::g.y, player::g.z) &&
		                   col::preloadAreaStep(player::g.x, player::g.y, player::g.z);
		if (!ready && timing::vblanks() - s_warpAt < 600) return;
		if (!ready) {
			hud::log("Big Smoke: Jefferson failed to stream");
			s_warping = false;
			fail();
			return;
		}
		player::placeOnGround();
		s_warping = false;
		cutscene::start(FUNERAL, sizeof FUNERAL / sizeof FUNERAL[0]);
		target();
		return;
	}
	if (world::area() != 0) return;
	const float dx = player::g.x - HOUSE_X, dy = player::g.y - HOUSE_Y;
	if (s_passed == 0) {
		if (dx * dx + dy * dy > 10.0f * 10.0f) {
			if (s_bike < 0 && vehdata::ready() && fabsf(player::g.x - 2246.508f) < 100.0f)
				s_bike = veh::spawn(vehdata::findModel(BMX_MODEL), 2246.508f, -1263.087f, 23.9f, 4.97f);
			return;
		}
		s_passed = 1;
		clearBike();
		save();
		hud::log("SCM INTRO completed; Big Smoke at CJ's house");
		target();
		return;
	}
	if (!s_active && dx * dx + dy * dy < 2.5f * 2.5f && !player::g.inVehicle) {
		beginSmoke();
		return;
	}
	if (!s_active) return;
	if (s_bike < 0 && vehdata::ready() && fabsf(player::g.x - 970.0873f) < 100.0f)
		s_bike = veh::spawn(vehdata::findModel(BMX_MODEL), 971.8f, -1108.0f, 23.9f, 2.44f);
	const veh::Vehicle* bike = veh::get(s_bike);
	if (bike && bike->status == veh::STATUS_WRECKED) {
		fail();
		return;
	}
	if (!bike) return;
	if (s_objective == 0) {
		if (!player::driving() || player::vehicleId() != s_bike) return;
		s_objective = 1;
	} else {
		static const float route[][2] = {
			{ 0, 0 }, { 1644.773f, -1051.334f },
			{ 1891.0f, -1347.78f }, { 2493.82f, -1669.91f }
		};
		if (!player::driving() || player::vehicleId() != s_bike) return;
		const float rx = player::g.x - route[s_objective][0];
		const float ry = player::g.y - route[s_objective][1];
		if (rx * rx + ry * ry > 25.0f * 25.0f) return;
		if (s_objective < 3) s_objective++;
		else {
			s_passed = 2;
			s_active = false;
			s_objective = 0;
			player::setWantedLevel(0);
			clearBike();
			save();
			hud::log("SCM INTRO1 passed: CJ's house unlocked");
		}
	}
	target();
}

bool busy() { return s_warping; }

void drawFade() {
	if (!s_warping) return;
	sprite::beginFrame();
	sprite::rect({ 0, 0, 640, 480 }, { 0, 0, 0 });
	sprite::endFrame();
}

}
