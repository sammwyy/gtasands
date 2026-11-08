#pragma once

#include <nds/ndstypes.h>
#include <stdint.h>

#include "assets/model.h"
#include "mem/budget.h"

// CPedModelInfo + the render side of a ped model (one skinned DMDL from
// gta3.dspk, its TXD), streamed like a vehicle model: the DMDL is read and its
// textures staged (update), uploaded after the vblank (vblank), then compiled
// into GX lists. A ped mesh binds ~32 bones but the matrix stack only has room
// for SEG_BONES of them next to the view / entity slots, so the triangles are
// grouped into segments of at most SEG_BONES dominant bones; per draw each
// segment stores its bone matrices (entity * world * invBone, multiplied by the
// GE) and DMAs its list. Only a compact skeleton (nodes, inverse binds, bind
// pose, bind quaternions) stays resident with the lists: the DMDL is freed.
// Refcounted by the peds using it; CPopulation-style streaming decides which
// models stay (game/peds).
namespace pedmodel {

constexpr int MAX = mem::budget::PED_MODEL_SLOTS;
constexpr int SEG_BONES = 24;
constexpr int MAX_SEGS = 4;
constexpr int MAX_TEX = 4;
constexpr int MAX_PATCH = 8;
// matrix stack slots while peds draw (CJ: 14..30, vehicles 10..11, map 12..13
// are restored by their own draws)
constexpr int SLOT_VIEW = 5;
constexpr int SLOT_ENTITY = 6;
constexpr int SLOT_TOP = 30;   // bone k of a segment = SLOT_TOP - k

struct Segment {
	u32* list;
	uint8_t numBones;
	uint8_t node[SEG_BONES];   // hAnim node per slot
	uint16_t tris;
};

enum State : uint8_t { STATE_FREE, STATE_PENDING, STATE_TEXTURES, STATE_READY, STATE_FAILED };

struct Model {
	uint8_t state, refs;
	uint8_t retries;           // DMDL reads that found no STREAM block for it
	uint32_t retryAt;          // frame of the next attempt
	int16_t pedIndex;          // peddata model index
	char cutsceneName[24];
	bool cutsceneSource;
	mdl::Model* skel;          // compact skeleton (one STREAM block, never mdl::free'd)
	Segment segs[MAX_SEGS];
	uint8_t numSegs;
	uint16_t tris;
	float lift;                // feet below the ped origin in the bind pose (m)
	uint32_t listBytes, lastUse;
	uint8_t numPatch;
	uint8_t patchSeg[MAX_PATCH];
	uint16_t patchAt[MAX_PATCH], patchBase[MAX_PATCH];   // COLOR words tinted by the ped light
	uint8_t numTex;
	uint8_t tex[MAX_TEX];      // texture entries
	void* blob;                // DMDL while loading
	mdl::Model* view;
};

void init();
// References (or starts loading) peddata model `pedIndex`. -1 if no slot is free.
int request(int pedIndex);
int requestCutscene(const char* name, bool source = false);
void release(int h);
Model* get(int h);
bool ready(int h);
int find(int pedIndex);        // handle of a resident / loading model, -1 if none
// Streaming step (once per frame) and texture uploads (right after the vblank).
void update();
void vblank();
// Ped lighting (timecycle::pedLight): tints every material colour, patched in
// place only when it changes.
void setLight(u8 r, u8 g, u8 b);
// Draws model h with the bone world matrices `world` (numBones x 12, 20.12).
// The entity matrix must be in SLOT_ENTITY. Returns the triangles submitted.
int draw(const Model& m, const s32* world);
// After a batch of draws: the lists wrote the texture registers behind libnds.
void endDraw();

struct Stats { uint16_t models, ready, textures; uint32_t listBytes, texVram, loads; };
const Stats& stats();

}
