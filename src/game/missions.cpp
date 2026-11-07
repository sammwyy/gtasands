#include "game/missions.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game/collision.h"
#include "game/hud/radar.h"
#include "game/player.h"
#include "game/world.h"
#include "hud/hud.h"
#include "platform/timing.h"

namespace missions {

static constexpr uint32_t MAGIC = 0x53494d47;
static constexpr uint16_t VERSION = 1;
static constexpr uint8_t COUNT = 3;
static constexpr int BLIP = 15;

struct Save {
	uint32_t magic;
	uint16_t version;
	uint8_t completed, objective;
	int32_t money;
	uint32_t check;
};

static uint8_t s_completed, s_objective;
static bool s_failed, s_loaded;
static uint32_t s_retryAt;

static uint32_t checksum(const Save& s) {
	return s.magic ^ ((uint32_t)s.version << 16 | (uint32_t)s.completed << 8 | s.objective) ^
	       (uint32_t)s.money ^ 0xa62b317du;
}

static FILE* openSave(const char* mode) {
	FILE* f = fopen("sd:/gtasads_missions.dat", mode);
	if (!f) f = fopen("fat:/gtasads_missions.dat", mode);
	return f;
}

static void save() {
	Save s{ MAGIC, VERSION, s_completed, s_objective, player::g.money, 0 };
	s.check = checksum(s);
	FILE* f = openSave("wb");
	if (!f) {
		hud::log("missions: SD unavailable, progress in RAM");
		return;
	}
	const bool ok = fwrite(&s, sizeof s, 1, f) == 1 && fflush(f) == 0;
	fclose(f);
	if (!ok) hud::log("missions: SD write failed");
}

static const char* const TITLES[COUNT] = {
	"GROVE STREET", "RYDER'S ERRAND", "LOS SANTOS"
};
static const char* const TEXT[COUNT][3] = {
	{ "Go to CJ's house", nullptr, nullptr },
	{ "Enter a vehicle (Y)", "Drive to Cluckin' Bell", "Drive back to Grove St." },
	{ "Go to the Ganton gym", "Return to CJ's house", nullptr },
};
static const float TARGETS[COUNT][3][2] = {
	{ {2495.3f, -1690.8f} },
	{ {0, 0}, {2398.0f, -1899.0f}, {2495.3f, -1690.8f} },
	{ {2238.0f, -1722.0f}, {2502.0f, -1690.8f} },
};

static void target() {
	if (s_failed || s_completed == COUNT || (s_completed == 1 && s_objective == 0)) {
		radar::clearBlip(BLIP);
		return;
	}
	radar::setBlip(BLIP, TARGETS[s_completed][s_objective][0],
	               TARGETS[s_completed][s_objective][1], "", 255, 220, 40);
}

void init() {
	s_completed = s_objective = 0;
	s_failed = s_loaded = false;
	FILE* f = openSave("rb");
	if (f) {
		Save s{};
		const bool valid = fread(&s, sizeof s, 1, f) == 1 &&
		                   fgetc(f) == EOF && s.magic == MAGIC && s.version == VERSION &&
		                   s.completed <= COUNT && s.objective <= (s.completed == 1 ? 2 : 1) &&
		                   (s.completed != COUNT || s.objective == 0) &&
		                   s.money >= 0 && s.money <= 99999999 && s.check == checksum(s);
		fclose(f);
		if (valid) {
			s_completed = s.completed;
			s_objective = s.objective;
			player::g.money = s.money;
			s_loaded = true;
		} else hud::log("missions: invalid save, starting new");
	}
	target();
	hud::log("missions: %u/%u %s", s_completed, COUNT, s_loaded ? "restored" : "new");
}

uint8_t completed() { return s_completed; }
uint8_t objective() { return s_objective; }
const char* title() { return s_completed < COUNT ? TITLES[s_completed] : "FREE ROAM"; }
const char* instruction() {
	if (s_completed == COUNT) return "Story slice complete";
	return s_failed ? "Mission failed - retrying" : TEXT[s_completed][s_objective];
}

bool entryAllowed(const char* name, float x, float y) {
	if (s_completed == COUNT) return true;
	if (!strncmp(name, "carls", 8)) return s_completed >= 1;
	if (s_completed < 2) return false;
	if (x > 1800.0f && x < 3000.0f && y > -2300.0f && y < -950.0f)
		return true;
	return false;
}

bool pointAllowed(float x, float y) {
	if (s_completed == COUNT) return true;
	if (s_completed == 1) {
		const float dx = x - 2495.3f, dy = y + 1690.8f;
		return dx * dx + dy * dy < 20.0f * 20.0f;
	}
	return s_completed == 2 && x > 1800.0f && x < 3000.0f &&
	       y > -2300.0f && y < -950.0f;
}

void update() {
	if (s_completed == COUNT) return;
	if (player::g.health <= 0.0f) {
		if (!s_failed) {
			s_failed = true;
			s_retryAt = timing::vblanks() + 180;
			hud::log("mission failed: %s", title());
			target();
		}
		return;
	}
	if (s_failed) {
		if ((int32_t)(timing::vblanks() - s_retryAt) < 0) return;
		if (world::area() != 0) {
			world::setArea(0);
			col::resetScene();
			player::respawn();
			player::g.health = player::g.maxHealth;
			player::setWantedLevel(0);
		}
		s_failed = false;
		s_objective = 0;
		target();
		save();
		return;
	}
	if (world::area() != 0) return;
	if (s_completed == 1 && s_objective == 0) {
		if (!player::driving()) return;
	} else {
		if (s_completed == 1 && !player::driving()) return;
		const float dx = player::g.x - TARGETS[s_completed][s_objective][0];
		const float dy = player::g.y - TARGETS[s_completed][s_objective][1];
		if (dx * dx + dy * dy > 9.0f * 9.0f) return;
	}
	const uint8_t last = s_completed == 1 ? 2 : s_completed == 2 ? 1 : 0;
	if (s_objective < last) {
		s_objective++;
		hud::log("%s: %s", title(), instruction());
	} else {
		hud::log("mission passed: %s", title());
		if (s_completed == 1) player::g.money += 500;
		if (s_completed == 2) player::g.money += 1000;
		s_completed++;
		s_objective = 0;
		if (s_completed == 1) hud::log("CJ's house unlocked");
		if (s_completed == 2) hud::log("Los Santos interiors unlocked");
		if (s_completed == COUNT) hud::log("All IPL interiors unlocked");
	}
	target();
	save();
}

}
