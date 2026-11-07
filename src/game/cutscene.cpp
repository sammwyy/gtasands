#include "game/cutscene.h"

#include <nds.h>

#include "audio/audioengine.h"
#include "game/hud/chud.h"
#include "hud/hud.h"
#include "platform/timing.h"
#include "render/sprite2d.h"

namespace cutscene {

static const Shot* s_shots;
static uint8_t s_count;
static uint32_t s_start, s_total;

void reset() {
	s_shots = nullptr;
	s_count = 0;
	s_start = s_total = 0;
}

bool start(const Shot* shots, uint8_t count) {
	if (s_shots || !shots || !count || count > 16) return false;
	uint32_t total = 0;
	for (uint8_t i = 0; i < count; i++) {
		if (!shots[i].frames) return false;
		total += shots[i].frames;
	}
	if (total > 60 * 120) return false;
	s_shots = shots;
	s_count = count;
	s_start = timing::vblanks();
	s_total = total;
	chud::invalidate();
	audio::setGameActive(false);
	hud::log("cutscene: start %lu frames", (unsigned long)total);
	return true;
}

void stop() {
	if (active()) {
		reset();
		chud::invalidate();
		audio::setGameActive(true);
		hud::log("cutscene: finished");
	}
}
bool active() { return s_shots != nullptr; }

void update(uint32_t keysDown) {
	if (!active()) return;
	const uint32_t elapsed = timing::vblanks() - s_start;
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
	return s_shots[shotAt(elapsed)].caption;
}

bool view(float eye[3], float target[3]) {
	if (!active()) return false;
	uint32_t elapsed = timing::vblanks() - s_start;
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
