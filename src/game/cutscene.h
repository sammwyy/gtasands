#pragma once

#include <stdint.h>

namespace cutscene {

struct Shot {
	float eye[3], target[3];
	uint16_t frames;
	const char* caption;
};

void reset();
bool start(const Shot* shots, uint8_t count);
void update(uint32_t keysDown);
void stop();
bool active();
const char* caption();
bool view(float eye[3], float target[3]);
void drawOverlay();

}
