#pragma once

#include <stdint.h>

// CAudioEngine (Audio/AudioEngine.cpp): owns the audio subsystems and runs
// them once per frame. Boot: init() (hardware, banks.dsbl, EventVol.dat, the
// sound manager) so the frontend can beep; loading screen: initGameStep()
// (surfaces, audio zones, the in-game preload = InitialisePostLoading);
// in game: the listener follows the chase camera, the weather entity and
// the ambience track manager are serviced, the pause menu pauses the mix.
namespace audio {

void init();
bool initGameStep();          // resumable loading job, true when done
void setGameActive(bool on);  // in game (listener + entities) vs frontend only
void service();               // every frame, any state (after the simulation)
float listenerZ();            // camera height (wind)

struct Stats {
	uint32_t serviceUs;       // whole audio frame
	uint32_t footsteps, landings, swings, shots, thunders;
};
Stats& stats();
// One-line summary (hudok marker, debug UI): sounds, cache, events, stream.
void statsLine(char* buf, int n);

}
