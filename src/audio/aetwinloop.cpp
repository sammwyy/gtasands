#include "audio/aetwinloop.h"

#include "audio/aeaudio.h"

using aesound::Sound;

void TwinLoop::init(uint16_t b, uint16_t a, uint16_t bb, uint16_t mn, uint16_t mx) {
	stop();
	bank = b;
	ids[0] = a;
	ids[1] = bb;
	swapMin = mn;
	swapMax = mx;
	inUse = true;
	first = true;
}

static void recalc(TwinLoop& t) { t.swapAtMs = ae::timeMs() + (uint32_t)ae::randInt(t.swapMin, t.swapMax); }

bool TwinLoop::switchesThisFrame() const {
	const Sound* next = sounds[first ? 1 : 0];
	return ae::timeMs() > swapAtMs && next && !next->p.frameDelay;
}

// CAETwinLoopSoundEntity::UpdateParameters (+ SwapSounds)
static void onUpdate(Sound& s, int pos) {
	TwinLoop& t = *(TwinLoop*)s.p.client;
	if (pos < 0) {
		for (Sound*& p : t.sounds)
			if (p == &s) p = nullptr;
		return;
	}
	if (t.switchesThisFrame() && &s == t.sounds[0]) {
		Sound* cur = t.sounds[t.first ? 0 : 1];
		Sound* next = t.sounds[t.first ? 1 : 0];
		if (!cur || !next || next->p.frameDelay) return;   // the other one is still waiting for its phase
		next->p.volume = cur->p.volume;
		cur->p.volume = ae::VOLUME_SILENCE;
		t.first = !t.first;
		recalc(t);
	}
}

void TwinLoop::play(float x, float y, float z, float volume, float speed, uint16_t flags) {
	stop();
	inUse = true;
	for (int i = 0; i < 2; i++) {
		aesound::Params p;
		p.bank = bank;
		p.id = ids[i];
		p.x = x; p.y = y; p.z = z;
		p.volume = i == 0 ? volume : ae::VOLUME_SILENCE;
		p.speed = speed;
		p.flags = (uint16_t)(flags | aesound::SOUND_START_PERCENTAGE | aesound::SOUND_REQUEST_UPDATES);
		// random start phases; ours are start delays, so the audible one starts now
		p.playTime = i == 0 ? 0 : (int16_t)ae::randInt(0, 99);
		p.owner = aesound::OWNER_TWINLOOP;
		p.update = onUpdate;
		p.client = this;
		sounds[i] = aesound::play(p);
	}
	first = true;
	recalc(*this);
}

void TwinLoop::update(float x, float y, float z, float volume, float speed) {
	for (int i = 0; i < 2; i++) {
		Sound* s = sounds[i];
		if (!s) continue;
		s->setPosition(x, y, z);
		if (i == (first ? 0 : 1)) s->setVolume(volume);
		s->setSpeed(speed);
	}
}

void TwinLoop::stop() {
	for (Sound*& s : sounds) {
		if (s) s->stop();
		s = nullptr;
	}
	inUse = false;
}
