#pragma once

#include <stdint.h>

#include "assets/ds_formats.h"

// DMDL (from tools/gtasa/ds_mesh.py) runtime view. The blob stays owned by the
// caller; the loader parses frames + geometry pointers into one malloc block.
// Geometry arrays are left inside the blob (already packed s16/u8).
//
// Skinning convention (validated against RW/gta-reversed): points are row
// vectors, p' = p * M. Node i's world matrix is local_i * world_parent, and a
// vertex bound to node i transforms by v * invBone[i] * world[i]. Vertices'
// bone indices index the hAnim node list (= invBone order), not frames.
namespace mdl {

struct Frame {
	int16_t parent;           // -1 = root
	int16_t boneId;           // HAnim bone id, -1 = not a bone
	ds::fx16 rot[9];          // bind rotation, 4.12
	ds::fx32 pos[3];          // bind position, 20.12
};

struct Geometry {
	const ds::GeometryHeader* h;
	const ds::GeometryMaterial* mats;
	const int16_t (*pos)[3];  // pos = offset + s16 * 2^posExp / 4096
	const int16_t (*uv)[2];   // uv  = s16 * 2^uvExp / 4096   (may be null)
	const uint32_t* normal;   // packed 10.10.10 (may be null)
	const uint16_t* color;    // RGB15 (may be null)
	const uint8_t* bone;      // dominant skin bone index (may be null)
	const uint16_t* idx;
};

struct Model {
	uint16_t flags;
	uint16_t numFrames, numGeoms, numAtomics, numBones;
	Frame* frames;
	const ds::ModelAtomic* atomics;
	const ds::fx32* invBone;      // [numBones][4][3] rows right/up/at/pos, 20.12
	const ds::ModelNode* nodes;   // [numBones] hAnim order (null if !MDL_SKINNED)
	const ds::fx32* bindLocal;    // [numBones][4][3] anim-less local matrix
	const ds::fx32* bonePos;      // [numBones][3] default local translation
	int32_t* bindQ;               // [numBones][4] bindLocal as quats (anim::prepare), or null
	Geometry* geoms;
};

// Parses a DMDL blob (one mem::alloc block for the parsed view; the blob
// stays owned by the caller). Returns nullptr on bad magic. Free with mdl::free.
Model* load(const void* blob);
void free(Model* m);

}
