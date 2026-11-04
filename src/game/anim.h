#pragma once

#include <nds/ndstypes.h>
#include <stdint.h>

#include "assets/model.h"

// DANM (from tools/gtasa/ds_misc.py, an IFP ANP3 convert) playback. Pose is
// evaluated per hAnim node into 4x3 fixed-point matrices (20.12): rows
// right/up/at (m[0..8]) then translation (m[9..11]).
// All math follows the RW convention: points are row vectors (p' = p * M),
// so node world = local * world_parent and skin = invBone * world.
namespace anim {

constexpr int MAX_NODES = 64; // hAnim node count cap for a skinned clump

// Playback state for one model on one DANM animation. Plain POD; the caller
// owns the storage (static or member, no heap).
struct Player {
	const uint8_t* seqs;      // first AnimSeq inside the DANM blob
	uint16_t numSeqs;
	uint16_t duration;        // anim length in 1/60 s
	uint8_t loop;
	uint8_t extractRoot;      // locomotion: root XY delta moves the ped, not the mesh
	int32_t time;             // play head, 1/60 s (0..duration)
	int16_t rootNode;         // hAnim node with boneId 0, -1 if none
	uint8_t cursor[MAX_NODES];       // per-seq key cursor
	int16_t seqOfNode[MAX_NODES];    // hAnim node -> seq index, -1 = bind pose
};

// Points the player at `name` inside a DANM blob and binds model nodes to
// anim sequences by HAnim bone id. Returns false if the anim is missing.
bool play(Player& p, const mdl::Model* m, const void* danm, const char* name);

// Advances the play head by dt60 ticks (1/60 s units); wraps when looping.
// Returns true when the play head wrapped (or a non-looping anim clamped).
bool advance(Player& p, int32_t dt60);

// Root-bone translation (boneId 0) at an absolute time, in 20.12 model space
// (x right, y forward, z up). False when that node has no translation keys.
bool rootTranslation(const Player& p, const mdl::Model* m, int32_t time, s32 out[3]);

// Translation of the root bone's last key (the pose the cycle leaves on).
bool rootEndTranslation(const Player& p, const mdl::Model* m, s32 out[3]);

// Evaluates the pose at the current time: world[node] gets each node's
// model-space matrix (numBones entries of 12 fx32 each). Nodes with no
// sequence use the skin bind local baked into the model.
void evalWorld(const mdl::Model* m, Player& p, s32* world);

// Evaluates the static bind pose (bindLocal chains only).
void evalBind(const mdl::Model* m, s32* world);

// skin[node] = invBone[node] * world[node] (numBones entries of 12 fx32).
// With several parts sharing one skeleton, evalWorld once and call this per
// part (invBone is per-part data; the parts all use the same node table).
void skinForPart(const mdl::Model* m, const s32* world, s32* skin);

}
