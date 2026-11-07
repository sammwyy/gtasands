#pragma once

#include <stdint.h>

namespace missions {

void init();
void update();
uint8_t completed();
uint8_t objective();
const char* title();
const char* instruction();
bool entryAllowed(const char* name, float x, float y);
bool pointAllowed(float x, float y);

}
