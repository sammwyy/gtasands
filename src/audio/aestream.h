#pragma once

#include <stdint.h>

// CAEStreamingChannel + CAEAudioHardware::PlayTrack reduced to one mono
// track at a time on aehw::STREAM_CHANNEL (ambience; radio would be next).
// Tracks live in audio/streams/*.dsst as one long DSND each. The hardware
// ADPCM decoder cannot be fed a ring (its loop restores the state saved at
// the loop point), so the track is decoded in software (IMA-ADPCM, same
// nibble order and clamping as the mixer) into a looped PCM16 ring; the
// write head is kept ahead of the play position, which is derived from the
// CPU timer and the channel's sample period (no position register on DS).
namespace aestream {

// Starts trackId (TrakLkup.dat index). Resuming the track that was stopped
// last continues where it left off (m_PrevAmbiencePlayTimeMs); otherwise
// it starts at the beginning. Returns false if the track is not in the build.
bool play(uint32_t trackId, float volumeDb, bool loop, bool resume = true);
void stop();
bool playing();
uint32_t track();                 // current track id (~0u when stopped)
void setVolume(float db);         // channel volume (the music fader is applied on top)
void setFreqFactor(float f);      // SetChannelFrequencyScalingFactor (0 = paused)
void setMusicVolume(int vol64);   // menu radio volume 0..64 -> music fader
int playTimeMs();                 // -1 when stopped
void service();                   // refills the ring (call every frame)

struct Stats { uint32_t decoded, underruns, serviceUs; uint32_t trackId; };
const Stats& stats();

}
