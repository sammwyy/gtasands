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

// An instance as kept in RAM: same 36 bytes as ds::WorldInstance, but the
// rotation rows are already converted to GL axes (SA (x,y,z) -> GL (x,z,-y)).
struct Instance {
	int32_t pos[3];           // SA, 20.12
	int16_t m[9];             // GL basis rows right/forward/up, 4.12
	uint16_t model;
	uint16_t lodSwitch, linkedModel;
};

// Loaded sector window, for game/collision (instances stay valid until the
// next world::update).
struct SectorView {
	const Instance* inst;
	int count;
	int32_t c[3], reach;      // bounding sphere (render radii), 1/16 m
};
int sectors(SectorView* out, int max);
// Raw access to world.dwld (collision records live in it).
bool readAt(uint32_t offset, void* dst, uint32_t bytes);
uint32_t colOffset();         // ds::WorldHeader::colOffset (0 = no file)
bool sectorsPending();        // the window around the player is still loading sectors
int numModels();

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
// Frees the least recently used unreferenced map texture (vehicles ask when
// VRAM is full). False when nothing can go.
bool evictTexture();
const Stats& stats();

}
