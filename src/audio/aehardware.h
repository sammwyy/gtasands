#pragma once

#include <stdint.h>

// CAEAudioHardware reduced to the DS mixer: 16 hardware channels driven
// through BlocksDS's libnds ARM7 sound driver. Samples are
// played as stored (IMA-ADPCM is the hardware format, see game/sfx history),
// volumes are linear 11-bit converted to the mixer's 7-bit hardware range.
namespace aehw {

constexpr int NUM_CHANNELS = 16;
constexpr int SFX_CHANNELS = 14;     // 0..13: CAESoundManager's physical channels
constexpr int STREAM_CHANNEL = 14;   // CAEStreamingChannel (ambience track)
constexpr unsigned VOL_MAX = 2047;

// A DSND sound resident in main RAM (data = after the 24-byte SoundHeader).
struct Sample {
	const void* data;
	uint32_t bytes;          // padded to a word
	uint32_t numSamples;
	int32_t loopStart;       // samples, -1 = one shot
	uint16_t rate;
	int16_t headroom;        // 1/100 dB (CAEMP3BankLoader::GetSoundHeadroom)
	uint8_t fmt;             // ds::SoundFormat
	uint32_t lengthMs() const { return rate ? numSamples * 1000u / rate : 0; }
};

void init();                 // power on + channel tracking
// Starts `s` on channel `ch` (anything playing there is replaced).
void play(int ch, const Sample& s, unsigned vol, unsigned pan, unsigned hz);
// Looped PCM16 ring (stream channel), restarts from its start.
void playRing(int ch, const void* pcm16, uint32_t bytes, unsigned vol, unsigned pan, unsigned hz);
void setVolume(int ch, unsigned vol);   // 0..VOL_MAX
void setPan(int ch, unsigned pan);      // 0..127, 64 = centre
void setFreq(int ch, unsigned hz);
void stop(int ch);
uint32_t activeMask();       // channels tracked as playing
void setMasterVolume(unsigned v);   // mixer 0..127
// SFX channels the sound manager may use (all of 0..13 unless a debug
// capture holds 1 and 3, the capture units' timer channels)
uint32_t sfxMask();

#ifdef GTASADS_DEBUG
// Headless listening test: the two capture units record the left / right
// mixer output (PCM16 at `hz`) into RAM; captureSave writes a stereo WAV.
// Reserves channels 1 and 3 from the sound manager while it runs.
bool captureStart(uint32_t samples, unsigned hz);
bool captureBusy();          // still recording (time based)
bool captureSave(const char* path);
#endif

}
