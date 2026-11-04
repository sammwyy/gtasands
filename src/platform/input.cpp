#include "platform/input.h"

#include <nds.h>

namespace input {

static State s;
#ifdef GTASADS_DEBUG
static u32 s_fake, s_fakePrev;
void debugHold(u32 keys) { s_fake = keys; }
#endif

void poll() {
	scanKeys();
	s.held = keysHeld();
	s.down = keysDown();
	s.up = keysUp();
#ifdef GTASADS_DEBUG
	s.held |= s_fake;
	s.down |= s_fake & ~s_fakePrev;
	s.up |= s_fakePrev & ~s_fake;
	s_fakePrev = s_fake;
#endif
	s.touching = s.held & KEY_TOUCH;
	s.tapped = s.down & KEY_TOUCH;
	if (s.touching) {
		touchPosition t;
		touchRead(&t);
		s.touchX = t.px;
		s.touchY = t.py;
	}
}

const State& get() { return s; }

void consumeEdges() {
	s.down = 0;
	s.up = 0;
}

}
