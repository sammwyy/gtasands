#pragma once

#include <stdint.h>

// Map collision (CCollision + CWorld::ProcessLineOfSight / ProcessVerticalLine,
// reduced to what an on-foot player and the chase camera need). The COL of
// every map model is baked into world.dwld (tools/gtasa/world.py: model
// space, 1/128 m s16 vertices, faces in a bbox tree: BVH, <= 6 per leaf).
//  - update(): every frame, the collision records of the instances within
//    STREAM_M of the player are streamed in (LRU over COL_CACHE_BYTES) and the
//    instances within GATHER_M are copied to a small "near" list,
//  - the queries only walk the near list, in each instance's model space
//    (integer math, int64 for the cross products),
//  - single-sided like CCollision::ProcessLineTriangle: a segment only hits a
//    face it crosses from the front.
namespace col {

// CTempColModels::ms_colModelPed1: three 0.35 m spheres at -0.2 / 0.2 / 0.6
// around the ped origin, which sits 1 m above the feet.
constexpr float PED_RADIUS = 0.35f;
constexpr float PED_SPHERE_Z[3] = { 0.8f, 1.2f, 1.6f };   // above the feet

struct Hit {
	float t;                  // 0..1 along the segment
	float x, y, z;            // SA world point
	float nx, ny, nz;         // world normal (nz 1 = flat floor)
	uint8_t surface, light;   // COL surface type, tColLighting (day | night << 4)
};

struct Stats {
	uint16_t models, nearCount, wants, missing;
	uint32_t bytes, loads;
	uint16_t updateUs, queryUs;  // queryUs: the previous frame's queries (pushUs of them in pushSphere)
	uint16_t pushUs;
	uint32_t faceTests;          // triangle bbox tests in the previous frame's queries
	uint16_t debugFaces;
};

// Loading-screen job (after world::initStep): reads the index, then the
// records around the player. True when done.
bool initStep();
bool available();
// Once per in-game frame, before the player moves.
void update(float x, float y, float z);
// True while a collision record needed near the player is still streaming:
// the player must not start falling through it.
bool pending();

// First front-facing hit from a to b (SA world).
bool castSegment(const float a[3], const float b[3], Hit& hit);
// CWorld::ProcessVerticalLine: highest surface below zTop down to zBottom.
bool groundAt(float x, float y, float zTop, float zBottom, Hit& hit);
// Sphere at (x, y, z) radius r vs walls (faces with |n.z| < 0.6), boxes and
// spheres: (x, y) is pushed out horizontally. True if anything was touched.
bool pushSphere(float& x, float& y, float z, float r);
// Vehicle body spheres (CCollision::ProcessColModels, reduced): sphere at c
// radius r vs every near face (front side), box and sphere. `out` = the 3D
// displacement (m) that resolves the overlap (its direction is the contact
// normal, its length the depth); surface = COL surface of the last face hit.
bool sphereContact(const float c[3], float r, float out[3], uint8_t& surface);

// REND "collision" overlay: near volumes within dbg::opts.colRange as
// outlined wireframe on top of the scene (+ the ped spheres and ground hit).
// Call with the view matrix loaded; origin = SA point at the GL origin.
void drawDebug(float originX, float originY, float originZ, float px, float py, float pz);

const Stats& stats();

}
