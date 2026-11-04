#pragma once

#include <nds/ndstypes.h>
#include <stdint.h>

#include "assets/ds_formats.h"

// CVehicleModelInfo + the render side of a vehicle model (DMDL from
// gta3.dspk, textures from the model TXD with models/generic/vehicle.txd as
// its txd parent). A model is requested by a spawned vehicle and streamed over
// a few frames like a map model: the DMDL is read and its textures staged in
// RAM (update), uploaded right after the vblank (vblank), then every atomic is
// compiled into its own GX display list (lit: material DIF_AMB + per-vertex
// NORMAL, light0 = the car light). Refcounted; unreferenced models stay
// cached until a slot is needed.
//
// Atomics are classified by frame name like CVehicleModelInfo::
// SetVehicleComponentFlags: chassis, wheel (one geometry drawn at the four
// wheel dummies), and the CDamageManager components with an *_ok / *_dam pair
// under their *_dummy hinge frame (doors, bonnet, boot, bumpers, windscreen,
// wings). Children (plates) follow their parent component.
namespace vehmdl {

constexpr int MAX_MODELS = 4;
constexpr int MAX_PARTS = 40;
constexpr int MAX_PATCH = 8;

// eCarWheel order (CAR_WHEEL_FRONT_LEFT, REAR_LEFT, FRONT_RIGHT, REAR_RIGHT)
enum Wheel : uint8_t { WHEEL_FL, WHEEL_RL, WHEEL_FR, WHEEL_RR, NUM_WHEELS };

// tComponent-ish: everything that can move or take damage on its own
enum Comp : uint8_t {
	COMP_NONE, COMP_CHASSIS, COMP_WHEEL,
	COMP_DOOR_LF, COMP_DOOR_RF, COMP_DOOR_LR, COMP_DOOR_RR,
	COMP_BONNET, COMP_BOOT, COMP_BUMP_FRONT, COMP_BUMP_REAR, COMP_WINDSCREEN,
	COMP_WING_LF, COMP_WING_RF, COMP_WING_LR, COMP_WING_RR,
	COMP_COUNT
};
enum Variant : uint8_t { VAR_ANY, VAR_OK, VAR_DAM };
enum Patch : uint8_t { PATCH_PRIMARY = 1, PATCH_SECONDARY, PATCH_LIGHT_FRONT, PATCH_LIGHT_REAR };

struct Part {
	u32* list;
	uint16_t tris;
	uint8_t comp, variant;
	int8_t hinge;               // frame index of the component's *_dummy (rotation pivot), -1 = static
	uint8_t numPatch, wheel, pad;
	s32 below[12];              // part frame -> hinge (or model space when hinge < 0), GX 4x3 rows
	bool identity;              // `below` is the identity
	uint16_t patchAt[MAX_PATCH];
	uint8_t patchKind[MAX_PATCH];
	uint16_t patchBase[MAX_PATCH];   // material colour (RGB15) for the light slots
};

enum State : uint8_t { STATE_FREE, STATE_PENDING, STATE_TEXTURES, STATE_READY, STATE_FAILED };

struct Model {
	uint8_t state, refs;
	int16_t vehIndex;           // vehdata index
	ds::VehInfo info;
	Part parts[MAX_PARTS];
	int numParts;
	int wheelPart;              // part index of the wheel geometry, -1 = none
	int8_t scaleExp;            // list vertices are v16 * 2^scaleExp
	// model space (SA axes: x right, y forward, z up), metres
	float wheelPos[NUM_WHEELS][3];
	float wheelRadius[2];       // front, rear (IDE wheel scale / 2)
	float wheelScale[2];        // geometry scale so the wheel mesh matches the IDE size
	float seat[3];              // driver seat (ped_frontseat mirrored to the left)
	float exhaust[3], engine[3];
	float hinge[COMP_COUNT][3]; // *_dummy positions (doors: hinge line)
	s32 hingeM[COMP_COUNT][12]; // *_dummy model-space matrices, GX 4x3
	float bmin[3], bmax[3];     // body bounds (ok parts, not the wheels)
	uint32_t listBytes;
	uint32_t lastUse;
	// colours currently patched into the lists
	u16 curPrim, curSec;
	uint8_t curLights;
	uint16_t numTex;
	uint16_t tex[24];           // texture cache entries this model references
	void* blob;                 // DMDL while loading
	void* view;                 // mdl::Model* while loading
};

void init();
// Starts (or references) the model of vehdata record `vehIndex`. -1 if no slot.
int request(int vehIndex);
void release(int handle);
Model* get(int handle);
bool ready(int handle);
// Streaming step (once per frame, time-boxed) and texture uploads (vblank).
void update();
void vblank();

// Writes the instance colours into the lists (only when they change).
void setColours(Model& m, u16 prim, u16 sec, bool lightsOn);

struct Stats { uint16_t models, ready, textures, texFailed; uint32_t listBytes, texVram, loads; };
const Stats& stats();

}
