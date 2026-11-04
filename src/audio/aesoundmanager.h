#pragma once

#include <stdint.h>

// CAESoundManager + CAESound (Audio/Managers/AESoundManager.cpp, AESound.cpp).
// A fixed pool of virtual sounds; every frame service() runs the owners'
// UpdateParameters, computes the listener volume (CalculateVolume: volume -
// headroom + distance attenuation + directional mike) and pan from the
// camera, and gives the 14 SFX hardware channels to the loudest ones (sounds
// that are not IS_CANCELLABLE keep theirs). A looped sound that loses its
// channel keeps running virtually and restarts when it gets one back; a
// one-shot can only start within its first 100 ms (ADPCM cannot be entered
// mid-stream), afterwards it plays out virtually and ends on time.
namespace aesound {

enum Flags : uint16_t {
	SOUND_FRONT_END = 0x1,          // position is listener-relative (x -1 left .. 1 right), no attenuation
	SOUND_IS_CANCELLABLE = 0x2,     // may lose its channel to a louder sound
	SOUND_REQUEST_UPDATES = 0x4,    // owner's update() runs every frame
	SOUND_UNPAUSABLE = 0x10,        // keeps playing while the game is paused (menu)
	SOUND_START_PERCENTAGE = 0x20,  // playTime = % of the length (loops: start delayed by that much)
	SOUND_ROLLED_OFF = 0x400,
	SOUND_FORCED_FRONT = 0x1000,
};

// CAEAudioEntity owners (AreSoundsOfThisEventPlayingForThisEntity & co).
enum Owner : uint8_t { OWNER_NONE, OWNER_FRONTEND, OWNER_PED, OWNER_WEAPON, OWNER_WEATHER, OWNER_TWINLOOP, OWNER_SPEECH, OWNER_VEHICLE };

struct Sound;
// CAEAudioEntity::UpdateParameters: playPosMs >= 0 every frame (with
// SOUND_REQUEST_UPDATES), -1 once when the sound ends (owner forgets it).
typedef void (*UpdateFn)(Sound& s, int playPosMs);

// tSoundPlayParams (positions are SA world metres unless SOUND_FRONT_END)
struct Params {
	uint16_t bank = 0, id = 0;
	float x = 0.0f, y = 0.0f, z = 0.0f;
	float volume = 0.0f;           // dB
	float rollOff = 1.0f;
	float speed = 1.0f;            // frequency factor
	float freqVariance = 0.0f;
	uint16_t flags = 0;
	int16_t playTime = 0;
	uint8_t frameDelay = 0;
	uint8_t owner = OWNER_NONE;
	int16_t event = -1;
	UpdateFn update = nullptr;
	void* client = nullptr;        // owner data for update()
	float clientVar = 0.0f;        // m_ClientVariable
};

struct Sound {
	Params p;
	float listenerVolume;          // dB after attenuation
	float listenerSpeed;           // speed with the random variance
	uint32_t startMs, lengthMs, hwStartMs;
	int16_t entry;                 // aebank handle
	int8_t channel;                // -1 = virtual
	bool active, loop, started;
	uint16_t hwVol, hwHz;
	uint8_t hwPan, sentPan;

	void setPosition(float x, float y, float z) { p.x = x; p.y = y; p.z = z; }
	void setVolume(float db) { p.volume = db; }
	void setSpeed(float s);        // CAESound::SetSpeed (+ UpdateFrequency)
	int playPos() const;           // ms since it started (loops wrap)
	void stop();                   // StopSoundAndForget: no more update() calls
};

void init();
// CAESoundManager::PlaySound. nullptr when the sample is not resident (its load
// is queued: the owner retries) or every virtual sound is busy.
Sound* play(const Params& p);
int playing(int16_t event, uint8_t owner);     // AreSoundsOfThisEventPlayingForThisEntity (count)
void cancelEvent(int16_t event, uint8_t owner);
void cancelOwner(uint8_t owner);
void cancelAll();
void setPaused(bool paused);                   // CTimer::m_UserPause: pausable sounds go silent
void setEffectsVolume(int vol64);              // menu SFX volume 0..64 (the effects fader)
// Listener = camera: eye + basis (right / forward / up, SA world axes) and
// the camera's distance to the player (GetPositionRelativeToCamera's offset).
void setListener(const float eye[3], const float right[3], const float fwd[3], const float up[3], float playerDist);
void service();

// CAEAudioEnvironment helpers (world point -> listener space).
float distanceAttenuation(float dist);   // GetDistanceAttenuation, dB
float distanceToListener(float x, float y, float z);
unsigned volumeFromDb(float db);         // 0..2047 linear

struct Stats {
	uint8_t active, physical;
	uint32_t started, dropped, stolen, missing;
	uint32_t serviceUs;
	uint16_t minGain = 4096;   // lowest limiter gain so far (1/4096)
};
const Stats& stats();

}
