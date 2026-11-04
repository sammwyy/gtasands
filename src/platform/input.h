#pragma once

#include <nds/ndstypes.h>

namespace input {

struct State {
	u32 held, down, up;
	bool touching;
	bool tapped;          // touch went down this frame (survives consumeEdges, for UI)
	int touchX, touchY;
};

void poll();
const State& get();
// Clears this frame's down/up edges (fixed-step loops: edges fire once).
void consumeEdges();
#ifdef GTASADS_DEBUG
// Headless tests: these keys read as held (edges included) until changed.
void debugHold(u32 keys);
#endif

}
