#include "platform/input.h"

#include <nds.h>

namespace input {

static State s;

void poll() {
	scanKeys();
	s.held = keysHeld();
	s.down = keysDown();
	s.up = keysUp();
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
