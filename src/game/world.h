#pragma once

#include <stdint.h>

// Streamed map (CWorld + CStreaming + CRenderer, heavily reduced). The map
// placement is baked offline into data/world.dwld (tools/gtasa/world.py):
// a sector grid of fixed-point instances. At runtime:
//  - a window of sectors around the player is kept in RAM (instances only),
//  - map models (gta3.dspk DMDL) are compiled into GX display lists on
//    demand, nearest wanted first, inside a per-frame time budget, and evicted
//    LRU (budget::WORLD_LIST_BYTES / WORLD_MODEL_SLOTS),
//  - their textures are read into RAM, uploaded right after the vblank
//    (world::vblank) and shared per (txd, name) with refcounts + LRU
//    (budget::WORLD_TEX_VRAM),
//  - draw() culls by draw distance / view frustum, draws near -> far and stops
//    at the per-frame polygon budget.
namespace world {

struct DrawView {
	float eyeX, eyeY, eyeZ;      // camera, SA world
	float yaw, pitch;            // renderer chase camera (yaw 0 = north, pitch > 0 looks down)
	float originX, originY, originZ; // SA point that maps to the GL origin (player-local space)
};

struct Stats {
	uint16_t sectors, instances;    // loaded window
	uint16_t candidates, drawn;     // in range / submitted last frame
	uint16_t culledFrustum, culledBudget;
	uint16_t modelsReady, modelsLoading;
	uint16_t textures, texFailed;
	uint32_t listBytes, texVram;
	uint32_t loads, uploads;        // totals since boot
	uint16_t polysBefore, polysAfter; // hardware polygon count around draw()
	uint16_t updateUs, drawUs, scanUs; // scanUs is part of drawUs
};

// Loading-screen job: opens the world file, then preloads the models around
// the player (bounded slices). True when done.
bool initStep();
bool ready();
// Sector window + model streaming (time-boxed). Once per in-game frame.
void update(float x, float y);
// Texture uploads; call right after the vblank (VRAM is free for ~20 lines).
void vblank();
// Inside renderer::draw with the view matrix loaded (player-local space).
void draw(const DrawView& v);
// Drops every display list and world texture (the pause menu needs the
// VRAM); streaming rebuilds them when drawing resumes.
void suspend();
const Stats& stats();

}
