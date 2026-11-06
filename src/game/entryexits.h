#pragma once

#include <stdint.h>

namespace entryexits {

bool init();
void reset();
bool unlockAll();
bool unlocked();
int count();
int exteriorCount();
bool update();
bool busy();
void drawMarker(float originX, float originY, float originZ);
void drawFade();

}
