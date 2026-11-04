#include "audio/aeweatheraudio.h"

#include <math.h>

#include "audio/aeaudio.h"
#include "audio/aebankloader.h"
#include "audio/aesoundmanager.h"
#include "audio/aetwinloop.h"
#include "audio/audioengine.h"
#include "game/weather.h"

namespace aeweather {

using aesound::Params;
using aesound::Sound;

// eWeatherEvent (the event ids of the entity's own sounds)
enum WeatherEvent : int16_t { WE_THUNDER = 1, WE_THUNDER_DIRECT = 2, WE_CITY_NOISE = 3, WE_WIND_A = 4, WE_WIND_B = 5,
                              WE_CITY_NOISE_R = 6 };

// DEFAULT_POS = (-0.906, 0, 0.423): to the side and a little up
static constexpr float SIDE = 0.906f, UP = 0.423f;
static constexpr uint16_t SND_CITY_NOISE = 0, SND_WIND_A = 29, SND_WIND_B = 30;
static constexpr uint16_t SND_RAIN_LOOP_A = 1, SND_RAIN_LOOP_B = 0, SND_RAIN_DROP_FIRST = 2, SND_RAIN_DROP_LAST = 11;

static TwinLoop s_rainL, s_rainR;
static float s_rainVolume = ae::VOLUME_SILENCE;   // m_sfRainVolume
static int s_lastDrop = SND_RAIN_DROP_LAST;        // m_snLastRainDropSoundID
static int s_thunderVar;                          // m_nThunderFrequencyVariationCounter
static bool s_windOffset;                         // sbWindOffset
static float s_windFreq = 1.0f;                   // sfOldFreqLeft
static uint32_t s_thunderPendingMs;
static uint32_t s_rainAskedAt = ~0u;
static uint32_t s_windFrame = ~0u;               // the gust state is shared by both wind loops
static float s_windOffsetDb;

static float stepTo(float v, float to, float step) {
	return v < to ? (v + step < to ? v + step : to) : (v - step > to ? v - step : to);
}

void preload() {
	aebank::pin(ae::BANK_GENRL_FRONTEND_GAME, SND_CITY_NOISE);
	aebank::pin(ae::BANK_GENRL_FRONTEND_GAME, SND_WIND_A);
	aebank::pin(ae::BANK_GENRL_FRONTEND_GAME, SND_WIND_B);
}

void reset() {
	s_rainVolume = ae::VOLUME_SILENCE;
	s_rainL.stop();
	s_rainR.stop();
	aesound::cancelOwner(aesound::OWNER_WEATHER);
}

float rainVolume() { return s_rainVolume; }

// UpdateParameters, CITY_NOISE: with no moving entity next to the camera
// (the line-of-sight boost needs traffic) it settles at the event volume
static void cityNoiseUpdate(Sound& s, int pos) {
	if (pos < 0) return;
	const float def = ae::defaultVolume(ae::AE_CITY_NOISE);
	if (s.p.volume < def) s.setVolume(s.p.volume + 0.3f < def ? s.p.volume + 0.3f : def);
	else if (s.p.volume > def) s.setVolume(s.p.volume - 0.3f > def ? s.p.volume - 0.3f : def);
	s.setSpeed(1.0f);
}

static float piecewise(const float (*pts)[2], int n, float x) {
	if (x <= pts[0][0]) return pts[0][1];
	for (int i = 1; i < n; i++)
		if (x <= pts[i][0])
			return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (x - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
	return pts[n - 1][1];
}

// UpdateParameters, UNK_4 / UNK_5: gusts. A 7% chance per update flips the
// gust on/off; volume steps toward -33 dB (+21 * ratio in a gust), the
// pitch rises with it. The gust state is worked out once per frame for both
// loops (the PC runs it per sound: twice the coin flips, same on average).
static void windFrame() {
	const uint32_t frame = ae::timeMs() / 16;
	if (frame == s_windFrame) return;
	s_windFrame = frame;
	float z = audio::listenerZ() / 500.0f;
	z = z < 0.0f ? 0.0f : z > 1.0f ? 1.0f : z;
	static const float k_low[3][2] = { { 0.0f, 0.0f }, { 0.3f, 0.4f }, { 1.0f, 0.5f } };
	static const float k_high[3][2] = { { 0.0f, 0.5f }, { 0.3f, 1.0f }, { 1.0f, 1.0f } };
	float w = weather::g.windClipped;
	w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
	const float lo = piecewise(k_low, 3, z), hi = piecewise(k_high, 3, z);
	const float ratio = lo + (hi - lo) * w;
	if (ae::chance(0.07f)) s_windOffset = !s_windOffset;
	s_windOffsetDb = s_windOffset ? 21.0f * ratio : 0.0f;
	const float freq = s_windOffset ? 1.2f * ratio : 0.0f;
	s_windFreq = stepTo(s_windFreq, freq > 1.0f ? freq : 1.0f, 0.1f);
}

static void windUpdate(Sound& s, int pos) {
	if (pos < 0) return;
	windFrame();
	s.setVolume(stepTo(s.p.volume, s_windOffsetDb - 33.0f, 0.6f));
	s.setSpeed(s_windFreq);
}

static float thunderVolume() {
	return ae::audioLog10((float)weather::g.lightningDuration * 0.0375f + 0.25f) * 20.0f + ae::defaultVolume(ae::AE_THUNDER);
}

// UpdateParameters, THUNDER: the near crack swells in after the flash
static void thunderUpdate(Sound& s, int pos) {
	if (pos <= 0 || s.p.event != WE_THUNDER) return;
	const float req = thunderVolume();
	const int reqPos = 600 - 500 * (int)(weather::g.lightningDuration / 20);
	if (pos > reqPos) {
		if (pos > reqPos + 200 || req <= 0.0f) {
			s.setVolume(req < 0.0f ? req : 0.0f);
		} else {
			const float v = powf(10.0f, req / 20.0f);
			s.setVolume(ae::audioLog10((float)(pos - reqPos) * (1.0f - v) / 200.0f + v) * 20.0f);
		}
	} else {
		s.setVolume(ae::audioLog10((float)pos / (float)reqPos) * 20.0f + req);
	}
}

static void ensure(uint16_t id, int16_t event, int16_t playTime, float x, aesound::UpdateFn fn) {
	if (aesound::playing(event, aesound::OWNER_WEATHER)) return;
	if (aebank::find(ae::BANK_GENRL_FRONTEND_GAME, id) < 0) return;   // EnsureSoundBankIsLoaded
	Params p;
	p.bank = ae::BANK_GENRL_FRONTEND_GAME;
	p.id = id;
	p.x = x;
	p.z = UP;
	p.volume = -50.0f;
	p.flags = aesound::SOUND_FORCED_FRONT | aesound::SOUND_START_PERCENTAGE | aesound::SOUND_REQUEST_UPDATES |
	          aesound::SOUND_FRONT_END;
	p.playTime = playTime;
	p.owner = aesound::OWNER_WEATHER;
	p.event = event;
	p.update = fn;
	aesound::play(p);
}

void thunder() {
	// the claps are only requested while it rains: a flash out of the blue
	// (debug) waits up to 2 s for them to stream in
	const bool near = aebank::find(ae::BANK_GENRL_EXPLOSIONS, ae::SND_EXPLOSIONS_NEAR_L) >= 0;
	const bool distant = aebank::find(ae::BANK_GENRL_EXPLOSIONS, ae::SND_EXPLOSIONS_DISTANT_L) >= 0;
	if (!near || !distant) {
		if (!s_thunderPendingMs) s_thunderPendingMs = ae::timeMs() + 2000;
		return;
	}
	s_thunderPendingMs = 0;
	audio::stats().thunders++;
	static const float k_freq[3] = { 1.15f, 1.0f, 0.85f };
	s_thunderVar = (s_thunderVar + 1) % 3;
	const float freq = k_freq[s_thunderVar];
	const float vol = thunderVolume();
	auto play = [&](uint16_t id, float x, float volume, float speed, uint16_t flags, int16_t event) {
		Params p;
		p.bank = ae::BANK_GENRL_EXPLOSIONS;
		p.id = id;
		p.x = x;
		p.y = UP;
		p.volume = volume;
		p.speed = speed;
		p.flags = flags;
		p.owner = aesound::OWNER_WEATHER;
		p.event = event;
		p.update = thunderUpdate;
		aesound::play(p);
	};
	using namespace aesound;
	const uint16_t f = SOUND_FRONT_END | SOUND_IS_CANCELLABLE | SOUND_REQUEST_UPDATES;
	play(ae::SND_EXPLOSIONS_NEAR_L, -SIDE, ae::VOLUME_SILENCE, freq * 0.35636002f, f, WE_THUNDER);
	play(ae::SND_EXPLOSIONS_NEAR_L, SIDE, ae::VOLUME_SILENCE, freq * 0.4f, f, WE_THUNDER);
	play(ae::SND_EXPLOSIONS_DISTANT_L, -SIDE, vol < 0.0f ? vol : 0.0f, freq * 0.35636002f, f | SOUND_ROLLED_OFF, WE_THUNDER_DIRECT);
	play(ae::SND_EXPLOSIONS_DISTANT_L, SIDE, vol < 0.0f ? vol : 0.0f, freq * 0.4f, f | SOUND_ROLLED_OFF, WE_THUNDER_DIRECT);
}

static void rainLoop(TwinLoop& t, float x) {
	if (t.active()) {
		t.update(x, 0.0f, UP, s_rainVolume, 1.0f);
		if (t.switchesThisFrame()) {
			s_lastDrop = s_lastDrop + 1 <= SND_RAIN_DROP_LAST ? s_lastDrop + 1 : SND_RAIN_DROP_FIRST;
			Params p;
			p.bank = ae::BANK_GENRL_RAIN;
			p.id = (uint16_t)s_lastDrop;
			p.x = x;
			p.y = ae::chance(0.5f) ? UP : -UP;
			p.volume = ae::randFloat(-6.0f, 6.0f) + s_rainVolume - 15.0f;
			p.flags = aesound::SOUND_IS_CANCELLABLE | aesound::SOUND_FRONT_END;
			p.owner = aesound::OWNER_WEATHER;
			aesound::play(p);
		}
	} else if (aebank::find(ae::BANK_GENRL_RAIN, SND_RAIN_LOOP_A) >= 0 && aebank::find(ae::BANK_GENRL_RAIN, SND_RAIN_LOOP_B) >= 0) {
		t.init(ae::BANK_GENRL_RAIN, SND_RAIN_LOOP_A, SND_RAIN_LOOP_B, 65, 350);
		t.play(x, UP, 0.0f, s_rainVolume, 1.0f, aesound::SOUND_IS_CANCELLABLE | aesound::SOUND_FRONT_END);
	}
}

void service() {
	// CGame::CanSeeOutSideFromCurrArea: no interiors yet, always outside.
	// Two city noise copies (left / right, half a loop apart): the reversed
	// code asks for the same event twice, so only one would ever start
	ensure(SND_CITY_NOISE, WE_CITY_NOISE, 0, -SIDE, cityNoiseUpdate);
	ensure(SND_CITY_NOISE, WE_CITY_NOISE_R, 50, SIDE, cityNoiseUpdate);
	ensure(SND_WIND_A, WE_WIND_A, 0, -SIDE, windUpdate);
	ensure(SND_WIND_B, WE_WIND_B, 50, SIDE, windUpdate);
	if (s_thunderPendingMs) {
		if (ae::timeMs() > s_thunderPendingMs) s_thunderPendingMs = 0;
		else thunder();
	}

	const float rain = weather::g.rain;
	float target = ae::VOLUME_SILENCE;
	if (rain > 0.0f) {
		if (s_rainVolume < -50.0f) s_rainVolume = -50.0f;
		target = ae::defaultVolume(ae::AE_RAIN) + ae::audioLog10((rain - 0.2f > 0.0f ? rain - 0.2f : 0.0f) / 0.8f) * 20.0f;
		// the drops / loops stream in when it starts raining (and the
		// thunder with them); LRU drops them again once it is dry. Asked
		// twice a second: a cache hit keeps them from going stale
		if ((ae::timeMs() >> 9) != s_rainAskedAt) {
			s_rainAskedAt = ae::timeMs() >> 9;
			for (uint16_t id = 0; id <= SND_RAIN_DROP_LAST; id++) aebank::find(ae::BANK_GENRL_RAIN, id);
			aebank::find(ae::BANK_GENRL_EXPLOSIONS, ae::SND_EXPLOSIONS_NEAR_L);
			aebank::find(ae::BANK_GENRL_EXPLOSIONS, ae::SND_EXPLOSIONS_DISTANT_L);
		}
	}
	s_rainVolume = stepTo(s_rainVolume, target, 0.5f);
	if (target == ae::VOLUME_SILENCE && s_rainVolume <= -50.0f) {
		s_rainVolume = ae::VOLUME_SILENCE;
		s_rainL.stop();
		s_rainR.stop();
	} else {
		rainLoop(s_rainL, -SIDE);
		rainLoop(s_rainR, SIDE);
	}
}

}
