#pragma once

#include <nds/ndstypes.h>
#include <stdint.h>

#include "assets/model.h"

// DANM (from tools/gtasa/ds_misc.py, an IFP ANP3 convert) playback. Pose is
// evaluated per hAnim node into 4x3 fixed-point matrices (20.12): rows
// right/up/at (m[0..8]) then translation (m[9..11]).
// All math follows the RW convention: points are row vectors (p' = p * M),
// so node world = local * world_parent and skin = invBone * world.
// Clips come from game/animstore (streamed, refcounted); a Player only
// points into a clip record.
namespace anim {

constexpr int MAX_NODES = 64; // hAnim node count cap for a skinned clump

// Playback state for one model on one clip. Plain POD; the caller owns the
// storage (static or member, no heap). Sequences are resolved per node once
// in play(), so sampling never walks the clip.
struct Player {
	const ds::AnimEntry* clip;
	const ds::AnimSeq* nodeSeq[MAX_NODES];   // per hAnim node, nullptr = bind pose
	uint8_t cursor[MAX_NODES];               // per node key cursor
	uint16_t duration;        // anim length in 1/60 s
	uint8_t loop;
	uint8_t extractRoot;      // locomotion: root XY delta moves the ped, not the mesh
	int32_t time;             // play head, 1/60 s (0..duration)
	int16_t rootNode;         // hAnim node with boneId 0, -1 if none
};

// Precomputes per-model data the sampler needs (bind-pose quaternions for
// nodes a clip does not animate). Call once after mdl::load.
bool prepare(mdl::Model* m);

// Binds a clip record (AnimEntry + seqs) to the model's nodes by HAnim id.
bool play(Player& p, const mdl::Model* m, const ds::AnimEntry* clip);

// Advances the play head by dt60 ticks (1/60 s units); wraps when looping.
// Returns true when the play head wrapped (or a non-looping anim clamped).
bool advance(Player& p, int32_t dt60);

// Root-bone translation (boneId 0) at an absolute time, in 20.12 model space
// (x right, y forward, z up). False when that node has no translation keys.
bool rootTranslation(const Player& p, const mdl::Model* m, int32_t time, s32 out[3]);

// Translation of the root bone's last key (the pose the cycle leaves on).
bool rootEndTranslation(const Player& p, const mdl::Model* m, s32 out[3]);

// One node's local pose: quaternion (4.12, xyzw, not necessarily unit: the
// blend normalizes once) and translation (20.12).
struct Local {
	s32 q[4];
	s32 t[3];
};

// Seeks the play head. Rewinding clears the key cursors.
void setTime(Player& p, int32_t t60);
float phase(const Player& p); // 0..1

// Samples the local pose at the current time into `out` (numBones entries).
// Nodes the clip does not animate stay on the bind pose. An extractRoot clip
// keeps only the root bone's vertical bob (XY replaced by the bind position).
// Advances the per-node key cursors.
void sampleLocal(const mdl::Model* m, Player& p, Local* out);

// Blends `n` local poses by 4.12 weights (normalized weighted sum, shortest
// path vs the first pose) and composes the hierarchy into world matrices
// (numBones entries of 12 fx32). n == 0 writes the bind pose.
void blendWorld(const mdl::Model* m, const Local* const* locals, const s32* w4096, int n, s32* world);

// Evaluates the static bind pose (bindLocal chains only).
void evalBind(const mdl::Model* m, s32* world);

// skin[node] = invBone[node] * world[node] (numBones entries of 12 fx32).
// Only used on the CPU for one-off measurements; drawing skins on the GX.
void skinForPart(const mdl::Model* m, const s32* world, s32* skin);

}
