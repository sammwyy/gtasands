#pragma once

#include <stdint.h>

// CSkidmarks / CSkidmark: 24 marks of up to 16 parts (a new part every
// 100 ms of skidding), drawn as translucent quad strips on the road, fading
// out 2.5..20 s after the wheel stops skidding depending on the length.
namespace skid {

enum Type : uint8_t { DEFAULT, SANDY, MUDDY, BLOODY };

void clear();
// CSkidmarks::RegisterOne: `id` = one per vehicle wheel, (x, y, z) the
// contact point, (dx, dy) the wheel's forward direction, width in m.
void registerOne(uint32_t id, float x, float y, float z, float dx, float dy, float width, Type type);
void update();                                   // once per frame
// Inside renderer::draw with the view loaded (player-local GL space).
void draw(float originX, float originY, float originZ);
int active();
int drawnPolys();

}
