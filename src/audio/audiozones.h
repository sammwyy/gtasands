#pragma once

#include <stdint.h>

// CAudioZones (Audio/AudioZones.cpp): the boxes / spheres of
// data/maps/audiozon.ipl ("auzo" section), each with an audio zone id the
// ambience track manager and the reverb lookup switch on.
namespace audiozones {

bool load();                                   // loading job (one pass over the IPL)
void update(bool force, float x, float y, float z);   // CAudioZones::Update (every 16 frames or 20 m)
int activeZone();                              // first active sphere, else first box, else -1
bool activeSphere(float& x, float& y, float& z);   // centre of the first active sphere
int count();
void switchZone(const char* name, bool enable); // CAudioZones::SwitchAudioZone

}
