#include "game/sfx.h"

#include <stdlib.h>

#include "audio/aeaudio.h"
#include "audio/aebankloader.h"
#include "audio/aesoundmanager.h"
#include "audio/audioengine.h"
#include "hud/hud.h"

// CAEFrontendAudioEntity on the audio engine (src/audio). The menu beeps
// (SND_BANK_GENRL_FRONTEND_MENU 60) and the start whoosh (FRONTEND_GAME
// 25/26) are stereo pairs, pinned at boot; the loading tune is one random
// stereo pair of SND_BANK_GENRL_LOADING (82), resident only while it plays.
namespace sfx {

using aesound::Params;

static constexpr uint16_t MENU_SOUNDS = 9;
static constexpr uint16_t SND_WHOOSH_L = 25, SND_WHOOSH_R = 26;
static bool s_ok;
static int s_tunePair = -1, s_tuneLoaded;
static aesound::Sound* s_tune[2];

void init() {
	if (s_ok) return;
	audio::init();
	int n = 0;
	for (uint16_t i = 0; i < MENU_SOUNDS; i++) n += aebank::loadNow(ae::BANK_GENRL_FRONTEND_MENU, i, true);
	aebank::loadNow(ae::BANK_GENRL_FRONTEND_GAME, SND_WHOOSH_L, true);
	aebank::loadNow(ae::BANK_GENRL_FRONTEND_GAME, SND_WHOOSH_R, true);
	s_ok = n > 0;
	if (!s_ok) { hud::log("\x1b[33msfx: frontend bank missing\x1b[39m"); return; }
	hud::log("sfx: %d menu sounds (adpcm)", n);
}

static aesound::Sound* play(uint16_t bank, uint16_t id, int16_t event, float x) {
	if (!s_ok) return nullptr;
	Params p;
	p.bank = bank;
	p.id = id;
	p.x = x;
	p.volume = ae::defaultVolume(event);
	// frontend: no position, plays through the pause menu, never stolen
	p.flags = aesound::SOUND_FRONT_END | aesound::SOUND_UNPAUSABLE;
	p.owner = aesound::OWNER_FRONTEND;
	p.event = event;
	return aesound::play(p);
}

static void pair(uint16_t a, uint16_t b, int16_t event, uint16_t bank = ae::BANK_GENRL_FRONTEND_MENU) {
	play(bank, a, event, -1.0f);
	play(bank, b, event, 1.0f);
}

// AEFrontendAudioEntity::AddAudioEvent
void highlight() { pair(4, 5, ae::AE_FRONTEND_HIGHLIGHT); }
void select_() { pair(6, 7, ae::AE_FRONTEND_SELECT); }
void back() { pair(0, 1, ae::AE_FRONTEND_BACK); }
void error() { pair(2, 3, ae::AE_FRONTEND_ERROR); }
void noiseTest() { play(ae::BANK_GENRL_FRONTEND_MENU, 8, ae::AE_FRONTEND_NOISE_TEST, 0.0f); }
void start() { pair(SND_WHOOSH_L, SND_WHOOSH_R, ae::AE_FRONTEND_START, ae::BANK_GENRL_FRONTEND_GAME); }

static void forgetTune(aesound::Sound& s, int pos) {
	if (pos >= 0) return;
	for (aesound::Sound*& t : s_tune)
		if (t == &s) t = nullptr;
}

// AEFrontendAudioEntity::ServiceLoadingTune: one random stereo pair from the
// LOADING bank, looped. One channel is read per call so the loading screen
// spreads the ~100 KB over two frames; both sides start together.
bool tuneStep() {
	if (!s_ok || s_tune[0] || s_tune[1]) return true;
	if (s_tunePair < 0) {
		s_tunePair = (rand() % 4) * 2;
		s_tuneLoaded = 0;
	}
	if (s_tuneLoaded < 2) {
		aebank::loadNow(ae::BANK_GENRL_LOADING, (uint16_t)(s_tunePair + s_tuneLoaded), true);
		if (++s_tuneLoaded < 2) return false;
	}
	for (int ch = 0; ch < 2; ch++) {
		s_tune[ch] = play(ae::BANK_GENRL_LOADING, (uint16_t)(s_tunePair + ch), ae::AE_FRONTEND_LOADING_TUNE_START,
		                  ch ? 1.0f : -1.0f);
		if (s_tune[ch]) s_tune[ch]->p.update = forgetTune;
	}
	hud::log("sfx: loading tune %d", s_tunePair / 2);
	return true;
}

void tuneStart() { while (!tuneStep()) {} }

void tuneStop() {
	for (aesound::Sound*& t : s_tune) {
		if (t) t->stop();
		t = nullptr;
	}
	if (s_tunePair >= 0) {
		aebank::drop(ae::BANK_GENRL_LOADING, (uint16_t)s_tunePair);
		aebank::drop(ae::BANK_GENRL_LOADING, (uint16_t)(s_tunePair + 1));
	}
	s_tunePair = -1;
}

}
