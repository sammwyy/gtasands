#pragma once

#include <nds/ndstypes.h>
#include <stdint.h>

#include "game/anim.h"
#include "game/cj.h"

namespace animstore { struct Clip; }

// CAnimBlendClumpData for an AI ped: up to SLOTS associations (clips from
// ped.ifp through animstore, shared and refcounted), the same pose / advance /
// root-extraction rules as cj.cpp's slots, keyed by a name hash so a slot
// costs no string. A ped calls pose() with this step's clip list (or
// pedmove::setRealMoveAnim through bind()), advance() with the elapsed ticks,
// and evaluate() only when it is drawn.
namespace pedanim {

constexpr int SLOTS = 4;

struct Slot {
	anim::Player anim;
	animstore::Clip* clip;
	uint32_t hash;
	float weight;
	uint8_t alive;
};

struct Set {
	Slot slots[SLOTS];
	float rootX, rootY;   // extracted root motion of the last advance (model metres: +x right, +y forward)
};

void reset(Set& s);       // releases every clip
// cj::pose semantics. `dt60` scales the blend-out of the clips not listed.
void pose(Set& s, const mdl::Model* skel, const cj::Clip* clips, int n, int dt60 = 1);
float phase(const Set& s, const char* name);   // -1 when not playing
bool ended(const Set& s, const char* name);
bool playing(const Set& s, const char* name);
void advance(Set& s, const mdl::Model* skel, int dt60);
// Blends the live slots into bone world matrices (numBones x 12).
void evaluate(Set& s, const mdl::Model* skel, s32* world);
// True when the clip exists in the ped block (fallbacks for missing groups).
bool exists(const char* name);

// pedmove::PoseApi callbacks for one ped: bind() before calling
// pedmove::setRealMoveAnim (the PoseApi has no context pointer).
void bind(Set* s, const mdl::Model* skel, int dt60);
void apiPose(const cj::Clip* clips, int n);
float apiPhase(const char* name);
bool apiEnded(const char* name);

}
