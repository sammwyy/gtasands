#include "audio/aeambience.h"

#include <math.h>

#include "audio/aeaudio.h"
#include "audio/aesoundmanager.h"
#include "audio/aestream.h"
#include "audio/audiozones.h"
#include "game/weather.h"

namespace aeambience {

static int s_track = -1;      // m_RequestedSettings.TrackID while starting/playing
static int s_want = -1;
static int s_failed = -1;
static float s_volume = ae::VOLUME_SILENCE;

void reset() {
	aestream::stop();
	s_track = s_want = -1;
	s_volume = ae::VOLUME_SILENCE;
}

int wantedTrack() { return s_want; }

// UpdateAmbienceTrackAndVolume's audio zone -> track table (radio zones and
// the special mission tracks are not ported)
static int trackForZone(int zone, float& vol) {
	float sx, sy, sz;
	switch (zone) {
	case 4: vol = -6.0f; return 143;
	case 5:
	case 10:
		if (audiozones::activeSphere(sx, sy, sz))
			vol = aesound::distanceAttenuation(aesound::distanceToListener(sx, sy, sz) * 0.2f) - 6.0f;
		return zone == 5 ? 140 : 139;
	case 8: return 165;
	case 12: return 168;
	case 13: return 157;
	case 15: return 164;
	case 17: return 146;
	case 19: return 138;
	case 20: return 136;
	case 21: return 135;
	case 23: return 148;
	case 24: return 159;
	case 25: return 158;
	case 26: return 154;
	case 28:
	case 29: return 147;
	case 34: return 162;
	case 36: return 155;
	case 37: return 144;
	case 39: return 163;
	case 41: return 169;
	case 44: return 152;
	case 48: return 137;
	case 50: return 173;
	case 51: return 156;
	case 64: return 151;
	case 66: return 170;
	case 67: return 171;
	default: return -1;
	}
}

void service(bool paused) {
	// CheckForPause: the track freezes (silent) while the menu is up
	aestream::setFreqFactor(paused ? 0.0f : 1.0f);
	if (paused) return;

	float vol = -6.0f;
	int want = -1;
	if (weather::g.sandstorm > 0.0f) {
		want = 167;   // the desert storm loop
		vol = log10f(2.0f) * log2f(weather::g.sandstorm);
	} else {
		want = trackForZone(audiozones::activeZone(), vol);
	}
	s_want = want;

	if (s_track != want) {
		// FadeOutAndStop, then STARTING at -34 dB for the new one
		if (aestream::playing()) {
			s_volume -= 0.4f;
			if (s_volume <= -40.0f) {
				aestream::stop();
				s_track = -1;
			}
		} else if (want >= 0 && want != s_failed) {
			s_volume = -34.0f;
			s_track = aestream::play((uint32_t)want, s_volume, true) ? want : -1;
			if (s_track < 0) s_failed = want;   // not in the build: don't retry every frame
		} else {
			s_track = -1;
		}
	} else if (want >= 0) {
		s_volume = s_volume < vol ? (s_volume + 1.0f < vol ? s_volume + 1.0f : vol)
		                          : (s_volume - 1.0f > vol ? s_volume - 1.0f : vol);
	}
	aestream::setVolume(s_volume);
}

}
