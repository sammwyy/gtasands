#include "audio/aeaudio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "hud/hud.h"
#include "platform/timing.h"

namespace ae {

// EventVol.dat is one int8 per eAudioEvents (45401 of them, mostly script
// events). The game events the port raises are all below 256: keep those.
static constexpr int EVENT_VOLUMES = 256;
static int8_t s_eventVol[EVENT_VOLUMES];

void loadEventVolumes() {
	FILE* f = fopen("game:/audio/config/eventvol.dat", "rb");
	if (!f) {
		hud::log("\x1b[33maudio: eventvol.dat missing\x1b[39m");
		return;
	}
	if (fread(s_eventVol, 1, EVENT_VOLUMES, f) != EVENT_VOLUMES) hud::log("\x1b[33maudio: eventvol short\x1b[39m");
	fclose(f);
}

float defaultVolume(int event) {
	return event >= 0 && event < EVENT_VOLUMES ? (float)s_eventVol[event] : 0.0f;
}

float audioLog10(float p) { return p >= 0.00001f ? log10f(p) : -5.0f; }

int randInt(int lo, int hi) { return hi <= lo ? lo : lo + rand() % (hi - lo + 1); }
float randFloat(float lo, float hi) { return lo + (hi - lo) * (float)(rand() & 0x7FFF) / 32767.0f; }
bool chance(float p) { return p >= 1.0f || (float)(rand() & 0x7FFF) < p * 32768.0f; }
uint32_t timeMs() { return timing::vblanks() * 50u / 3u; }

}
