#pragma once

#ifdef ARM9
#include <nds.h>
#else
#include <math.h>
#endif

namespace trig {

inline void sincos(float radians, float& s, float& c) {
#ifdef ARM9
	const int angle = (int)(radians * 5215.19f) & 0x7FFF;
	s = (float)sinLerp(angle) * (1.0f / 4096.0f);
	c = (float)cosLerp(angle) * (1.0f / 4096.0f);
#else
	s = sinf(radians);
	c = cosf(radians);
#endif
}

}
