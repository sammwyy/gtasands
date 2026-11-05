#pragma once

#include <stdint.h>

// CWaterLevel, reduced: the water.dat polygons (baked into vehicles.dveh by
// tools/gtasa/vehicles.py, ~8.6 KB resident) for GetWaterLevel and a flat
// translucent surface per polygon in view (no waves, no reflections).
// Outside the map square (|x| or |y| > 3000) the sea is at z = 0 like the
// original's out-of-map level.
namespace water {

// Lazy: reads the table from the open vehicles.dveh on first use.
bool available();
// CWaterLevel::GetWaterLevelNoWaves: surface z at (x, y), false = no water.
bool levelAt(float x, float y, float& z);
bool levelCu(int32_t x, int32_t y, int32_t& z);   // SA world in cu (1/128 m)
// Debug: the visible water polygon nearest to (x, y), its centre. False if none.
bool nearest(float x, float y, float& wx, float& wy, float& wz);
// Inside renderer::draw with the view loaded (player-local GL space).
void draw(float originX, float originY, float originZ, float eyeX, float eyeY);

}
