#pragma once

#include <math.h>
#ifdef ARM9
#include <nds.h>
#endif

namespace trig {

inline float atan2(float y, float x) {
#ifdef ARM9
	const int32_t fy = floattof32(y), fx = floattof32(x);
	if (fy || fx) return f32tofloat(atan2_f32(fy, fx));
	if (y == 0.0f && x == 0.0f) return 0.0f;
#endif
	return atan2f(y, x);
}

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
