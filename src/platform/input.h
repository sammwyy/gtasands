#pragma once

#include <nds/ndstypes.h>

namespace input {

struct State {
	u32 held, down, up;
	bool touching;
	int touchX, touchY;
};

void poll();
const State& get();

}
