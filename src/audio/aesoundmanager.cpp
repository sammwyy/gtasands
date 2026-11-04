#include "audio/aesoundmanager.h"

#include <math.h>
#include <string.h>

#include "audio/aeaudio.h"
#include "audio/aebankloader.h"
#include "audio/aehardware.h"
#include "mem/budget.h"
#include "platform/timing.h"

namespace aesound {

using mem::budget::AUDIO_SOUNDS;

static constexpr int16_t k_attenuation[] = {
#include "audio/aeattenuation.inc"
};
static constexpr int ATT_ENTRIES = sizeof k_attenuation / sizeof k_attenuation[0];

// dB -> 11-bit linear volume, quarter-dB steps from -100 to +12 dB
static constexpr int DB_MIN = -100, DB_MAX = 12, DB_STEPS = (DB_MAX - DB_MIN) * 4 + 1;
static uint16_t s_dbVol[DB_STEPS];

// PC levels are mixed for desktop speakers (volume - headroom puts the
// city bed near -45 dBFS and steps at -20): the DS speakers want it hotter.
// Loud events (gun fire +14 dB) are held back by the limiter below.
static constexpr float MIX_GAIN_DB = 12.0f;
// Sum of the channel gains the limiter lets through (1.5 full-scale
// channels: decorrelated sounds rarely peak together)
static constexpr uint32_t LIMIT_SUM = 2047 * 3 / 2;
static uint32_t s_limitGain = 4096;   // 1/4096

static Sound s_sounds[AUDIO_SOUNDS];
static int8_t s_chanSound[aehw::SFX_CHANNELS];   // sound index per hardware channel, -1 free
static float s_eye[3], s_right[3] = { 1, 0, 0 }, s_fwd[3] = { 0, 1, 0 }, s_up[3] = { 0, 0, 1 };
static bool s_paused;
static float s_effectsDb;
static Stats s_stats;

void init() {
	for (int i = 0; i < DB_STEPS; i++) {
		const float db = DB_MIN + i * 0.25f;
		const float v = 2047.0f * powf(10.0f, db / 20.0f);
		s_dbVol[i] = (uint16_t)(v > 2047.0f ? 2047.0f : v);
	}
	s_dbVol[0] = 0;   // -100 dB = VOLUME_SILENCE
	for (Sound& s : s_sounds) s = {};
	for (int8_t& c : s_chanSound) c = -1;
	setEffectsVolume(40);
}

unsigned volumeFromDb(float db) {
	if (db <= (float)DB_MIN) return 0;
	if (db >= (float)DB_MAX) return s_dbVol[DB_STEPS - 1];
	return s_dbVol[(int)((db - DB_MIN) * 4.0f + 0.5f)];
}

float distanceAttenuation(float dist) {
	const int i = (int)(dist * 10.0f);
	return i < 0 ? 0.0f : i >= ATT_ENTRIES ? ae::VOLUME_SILENCE : (float)k_attenuation[i] * 0.01f;
}

void setEffectsVolume(int vol64) {
	// CAEAudioHardware::SetEffectsMasterVolume: slider 0..64 -> fader dB
	s_effectsDb = vol64 <= 0 ? ae::VOLUME_SILENCE : 20.0f * log10f((float)vol64 / 64.0f);
}

void setPaused(bool paused) { s_paused = paused; }

void setListener(const float eye[3], const float right[3], const float fwd[3], const float up[3], float playerDist) {
	// GetPositionRelativeToCamera: the ear sits up to 0.5 m in front of the
	// lens, toward the player
	float k = playerDist - 0.5f;
	k = k < 0.0f ? 0.0f : k > 0.5f ? 0.5f : k;
	for (int i = 0; i < 3; i++) {
		s_eye[i] = eye[i] + fwd[i] * k;
		s_right[i] = right[i];
		s_fwd[i] = fwd[i];
		s_up[i] = up[i];
	}
}

static void relative(float x, float y, float z, float& rx, float& ry, float& rz) {
	const float dx = x - s_eye[0], dy = y - s_eye[1], dz = z - s_eye[2];
	rx = dx * s_right[0] + dy * s_right[1] + dz * s_right[2];
	ry = dx * s_fwd[0] + dy * s_fwd[1] + dz * s_fwd[2];
	rz = dx * s_up[0] + dy * s_up[1] + dz * s_up[2];
}

float distanceToListener(float x, float y, float z) {
	float rx, ry, rz;
	relative(x, y, z, rx, ry, rz);
	return sqrtf(rx * rx + ry * ry + rz * rz);
}

void Sound::setSpeed(float s) {
	if (s == p.speed) return;
	p.speed = s;
	// UpdateFrequency keeps the random offset picked at the start
	listenerSpeed = s;
}

int Sound::playPos() const {
	const uint32_t t = ae::timeMs() - startMs;
	return (int)(loop && lengthMs ? t % lengthMs : t);
}

static void freeChannel(Sound& s) {
	if (s.channel < 0) return;
	aehw::stop(s.channel);
	s_chanSound[s.channel] = -1;
	s.channel = -1;
	aebank::release(s.entry);
}

void Sound::stop() {
	if (!active) return;
	freeChannel(*this);
	active = false;
	p.update = nullptr;
}

static void finish(Sound& s) {
	freeChannel(s);
	s.active = false;
	if (s.p.update) {
		UpdateFn fn = s.p.update;
		s.p.update = nullptr;
		fn(s, -1);
	}
}

// Pool full (automatic fire stacks tails): the quietest one-shot that is
// quieter than the new sound makes room, the way a busy PC mix would never
// have heard it anyway.
static Sound* stealSlot(float volume) {
	Sound* v = nullptr;
	for (Sound& s : s_sounds)
		if (s.active && !s.loop && !(s.p.flags & SOUND_UNPAUSABLE) && (!v || s.listenerVolume < v->listenerVolume)) v = &s;
	if (!v || v->listenerVolume >= volume) return nullptr;
	finish(*v);
	s_stats.stolen++;
	return v;
}

Sound* play(const Params& p) {
	const int h = aebank::find(p.bank, p.id, true);
	const aehw::Sample* smp = aebank::sample(h);
	if (!smp) {
		s_stats.missing++;
		return nullptr;
	}
	Sound* slot = nullptr;
	for (Sound& s : s_sounds)
		if (!s.active) { slot = &s; break; }
	if (!slot) slot = stealSlot(p.volume);
	if (slot) {
		Sound& s = *slot;
		s = {};
		s.p = p;
		s.active = true;
		s.channel = -1;
		s.entry = (int16_t)h;
		s.loop = smp->loopStart >= 0;
		s.lengthMs = smp->lengthMs();
		if (!s.lengthMs) s.lengthMs = 1;
		// CalculateFrequency: a random offset within the variance, once
		s.listenerSpeed = p.freqVariance > 0.0f && p.freqVariance < p.speed
		                      ? p.speed + ae::randFloat(-p.freqVariance, p.freqVariance) : p.speed;
		// SOUND_START_PERCENTAGE on a loop = the same phase offset by starting
		// later (ADPCM data cannot be entered in the middle)
		if ((p.flags & SOUND_START_PERCENTAGE) && s.loop && p.playTime > 0) {
			const uint32_t delayMs = s.lengthMs * (uint32_t)(p.playTime % 100) / 100u;
			const uint32_t frames = delayMs * 60u / 1000u;
			s.p.frameDelay = (uint8_t)(frames > 255 ? 255 : frames);
		}
		s.startMs = ae::timeMs();
		s.listenerVolume = p.volume;   // until the first service(): what stealSlot compares
		return &s;
	}
	s_stats.dropped++;
	return nullptr;
}

int playing(int16_t event, uint8_t owner) {
	int n = 0;
	for (const Sound& s : s_sounds)
		if (s.active && s.p.event == event && s.p.owner == owner) n++;
	return n;
}

void cancelEvent(int16_t event, uint8_t owner) {
	for (Sound& s : s_sounds)
		if (s.active && s.p.event == event && s.p.owner == owner) s.stop();
}

void cancelOwner(uint8_t owner) {
	for (Sound& s : s_sounds)
		if (s.active && s.p.owner == owner) s.stop();
}

void cancelAll() {
	for (Sound& s : s_sounds) s.stop();
}


// CAESound::CalculateVolume + the channel position (pan)
static void calculate(Sound& s, int headroom) {
	float vol = s.p.volume - (float)headroom * 0.01f;
	float pan = 64.0f;
	if (s.p.flags & SOUND_FRONT_END) {
		// listener-relative x: the side copies of the ambient beds sit at
		// +-0.906, stereo halves (gun shots) at +-1; never fully hard-panned
		pan += 48.0f * (s.p.x < -1.0f ? -1.0f : s.p.x > 1.0f ? 1.0f : s.p.x);
	} else {
		float rx, ry, rz;
		relative(s.p.x, s.p.y, s.p.z, rx, ry, rz);
		const float d = sqrtf(rx * rx + ry * ry + rz * rz);
		// GetDirectionalMikeAttenuation: -6 dB behind, cut-off at 45 degrees
		const float c = d > 0.001f ? ry / d : 1.0f;
		constexpr float CUT = 0.70710678f;
		vol += c >= CUT ? 0.0f : c <= -CUT ? -6.0f : -6.0f * (1.0f - (c + CUT) / (2.0f * CUT));
		vol += distanceAttenuation(d / (s.p.rollOff > 0.01f ? s.p.rollOff : 0.01f));
		if (d > 0.5f) pan += 56.0f * rx / (d > 1.0f ? d : 1.0f);
	}
	s.listenerVolume = vol;
	s.hwPan = (uint8_t)(pan < 0.0f ? 0.0f : pan > 127.0f ? 127.0f : pan);
}

void service() {
	const u32 t0 = timing::ticks();
	const uint32_t now = ae::timeMs();
	const uint32_t hwActive = aehw::activeMask();

	// ended sounds: a physical one-shot whose channel went idle (after the
	// driver had time to report it started), a virtual one past its length,
	// a one-shot that never got a channel in time
	for (Sound& s : s_sounds) {
		if (!s.active) continue;
		bool done = false;
		if (s.channel >= 0) {
			if (now - s.hwStartMs > 50 && !(hwActive & (1u << s.channel))) {
				if (s.loop) freeChannel(s);   // stolen behind our back: restart later
				else done = true;
			}
		} else if (!s.loop && s.p.frameDelay == 0) {
			if (s.started ? now - s.startMs > s.lengthMs : now - s.startMs > 100) done = true;
		}
		if (done) {
			finish(s);
			continue;
		}
		if (s.p.frameDelay) {
			s.p.frameDelay--;
			s.startMs = now;
			continue;
		}
		if ((s.p.flags & SOUND_REQUEST_UPDATES) && s.p.update) {
			s.p.update(s, s.playPos());
			if (!s.active) continue;
		}
		const aehw::Sample* smp = aebank::sample(s.entry);
		if (!smp) {   // evicted while virtual (cannot happen with a ref, but be safe)
			finish(s);
			continue;
		}
		calculate(s, smp->headroom);
	}

	// priorities: uncancellable sounds keep their channel, then the loudest
	// (integer scores in 1/16 dB: soft-float compares add up on the ARM9)
	int cand[AUDIO_SOUNDS], score[AUDIO_SOUNDS];
	int nc = 0;
	for (int i = 0; i < AUDIO_SOUNDS; i++) {
		const Sound& s = s_sounds[i];
		if (!s.active || s.p.frameDelay) continue;
		const bool canStart = s.channel >= 0 || s.loop || !s.started;
		if (!canStart || (s.channel < 0 && s.listenerVolume <= -60.0f)) continue;
		int sc = (int)(s.listenerVolume * 16.0f) + (s.channel >= 0 ? 48 : 0);
		if (s.channel >= 0 && !(s.p.flags & SOUND_IS_CANCELLABLE)) sc += 16000;
		if (s.p.flags & SOUND_UNPAUSABLE) sc += 8000;   // frontend beeps
		cand[nc] = i;
		score[nc++] = sc;
	}
	// partial selection sort: the best candidates first, one per usable channel
	const uint32_t mask = aehw::sfxMask();
	const int usable = __builtin_popcount(mask);
	const int n = nc < usable ? nc : usable;
	for (int k = 0; k < n; k++) {
		int best = k;
		for (int j = k + 1; j < nc; j++)
			if (score[j] > score[best]) best = j;
		const int ti = cand[k], ts = score[k];
		cand[k] = cand[best];
		score[k] = score[best];
		cand[best] = ti;
		score[best] = ts;
	}
	const int* order = cand;
	for (int j = n; j < nc; j++) {
		Sound& s = s_sounds[cand[j]];
		if (s.channel >= 0) {
			freeChannel(s);
			s_stats.stolen++;
		}
	}
	// sounds that did not even qualify (silent, delayed) give theirs up too
	for (int c = 0; c < aehw::SFX_CHANNELS; c++) {
		const int si = s_chanSound[c];
		if (si < 0) continue;
		bool kept = false;   // a channel taken away (debug capture) frees its sound too
		if (mask & (1u << c))
			for (int k = 0; k < n && !kept; k++) kept = order[k] == si;
		if (!kept) freeChannel(s_sounds[si]);
	}

	// mix limiter: the channel gains are summed and scaled down when they
	// could clip the mixer together (gun fire, thunder over the rain);
	// instant attack, ~0.5 dB per frame release
	const float master = s_effectsDb + MIX_GAIN_DB;
	unsigned raw[aehw::SFX_CHANNELS];
	uint32_t sum = 0;
	for (int k = 0; k < n; k++) {
		const Sound& s = s_sounds[order[k]];
		const bool silent = s_paused && !(s.p.flags & SOUND_UNPAUSABLE);
		raw[k] = silent ? 0 : volumeFromDb(s.listenerVolume + master);
		sum += raw[k];
	}
	const uint32_t want = sum > LIMIT_SUM ? LIMIT_SUM * 4096u / sum : 4096u;
	s_limitGain = want < s_limitGain ? want : (s_limitGain * 4337u >> 12) < want ? s_limitGain * 4337u >> 12 : want;
	if (s_limitGain < s_stats.minGain) s_stats.minGain = (uint16_t)s_limitGain;
	uint8_t physical = 0, active = 0;
	for (int k = 0; k < n; k++) {
		Sound& s = s_sounds[order[k]];
		const aehw::Sample* smp = aebank::sample(s.entry);
		const unsigned vol = raw[k] * s_limitGain >> 12;
		unsigned hz = (unsigned)((float)smp->rate * s.listenerSpeed);
		if (s.channel < 0) {
			int ch = -1;
			for (int c = 0; c < aehw::SFX_CHANNELS; c++)
				if (s_chanSound[c] < 0 && (mask & (1u << c))) { ch = c; break; }
			if (ch < 0) break;
			s.channel = (int8_t)ch;
			s_chanSound[ch] = (int8_t)order[k];
			aebank::addRef(s.entry);
			aehw::play(ch, *smp, vol, s.hwPan, hz);
			s.sentPan = s.hwPan;
			s.hwVol = (uint16_t)vol;
			s.hwHz = (uint16_t)hz;
			s.hwStartMs = now;
			if (!s.started) {
				s.started = true;
				s.startMs = now;
				s_stats.started++;
			}
			continue;
		}
		if (vol != s.hwVol) { aehw::setVolume(s.channel, vol); s.hwVol = (uint16_t)vol; }
		if (hz != s.hwHz) { aehw::setFreq(s.channel, hz); s.hwHz = (uint16_t)hz; }
		// pan moves in small steps as the camera turns: only send real changes
		if (s.hwPan > s.sentPan + 1 || s.hwPan + 1 < s.sentPan) { aehw::setPan(s.channel, s.hwPan); s.sentPan = s.hwPan; }
	}
	for (const Sound& s : s_sounds) {
		active += s.active;
		physical += s.active && s.channel >= 0;
	}
	s_stats.active = active;
	s_stats.physical = physical;
	s_stats.serviceUs = timing::usSince(t0);
}

const Stats& stats() { return s_stats; }

}
