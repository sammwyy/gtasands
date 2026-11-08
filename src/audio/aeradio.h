#pragma once

#include <stdint.h>

namespace aeradio {
constexpr int OFF = 12;
bool initStep();
void reset();
void nextStation();
void selectStation(int station);
bool service(bool paused, bool preview = false);
int station();
}
