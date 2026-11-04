#pragma once

// CJ: the player model assembled from player.dspk clothing parts, animated by
// ped.ifp plus the fat/muscular motion groups (clips streamed by animstore).
// Skinned on the geometry engine: each part is a prebuilt display list that
// restores one matrix-stack slot per dominant bone, so the CPU never touches
// a vertex after load.
namespace cj {

// One clip in this frame's pose. Up to four play at once; their weights are
// applied as given (a crossfade is just two weights moving in opposite
// directions). `phase` is 0..1.
struct Clip {
	const char* name;
	float weight;
	bool loop;
	bool extract;   // root XY delta moves the ped; the mesh stays planted
	bool sync;      // seek to `phase` before this frame's advance
	bool restart;   // rewind to `phase` even if this clip is already playing
	float phase;
};

// Names of the on-foot motion group selected by the fat/muscle stats
// (CClothes::GetPlayerMotionGroupToLoad). stop/stopR always come from ped.ifp.
struct Motion {
	const char* idle;
	const char* walk;
	const char* run;
	const char* sprint;
	const char* start;   // walk_start, played once when leaving idle
	const char* tired;   // idle_tired, after a sprint empties the bar
	const char* stop;    // run_stop  (foot phase < 0.5)
	const char* stopR;   // run_stopR
};

// Loads the default outfit (SCM defaults: vest/jeansdenim/sneakerbincblk +
// player_face head). vest.dff is the whole torso; it and the hands sample the
// player_torso+vest composite, like BODY_PART_TEX_NAMES / ConstructTextures.
// initStep() does one part per call (loading-screen friendly) and returns
// true when done; init() runs all of them. Requires the "ped" anim block.
bool initStep();
bool init();
void shutdown();

// Replaces the pose targets with these clips. A clip already playing keeps its
// time unless `restart` or `sync` is set. Clips not listed blend out over a
// few frames, so action transitions do not snap through the bind pose.
void pose(const Clip* clips, int n);

// 0..1 of a playing clip, or -1 when that name is not in the pose.
float clipPhase(const char* name);
// A non-looping clip has reached its last tick.
bool clipEnded(const char* name);

// Advances every playing clip by dt60 ticks (1/60 s) and accumulates the
// extracted root motion, weighted by each clip's blend.
void advance(int dt60);
void rootDelta(float& x, float& y); // model metres this frame: +x right, +y forward

// STAT_FAT / STAT_MUSCLE, 0..1000. Picks the motion group (threshold 500, fat
// wins ties only when strictly fatter) and the per-bone girth. The original
// rebuilds three blended body meshes; this port has one outfit and one bone
// per vertex, so the same ratios scale the bone matrices instead.
void setStats(float fat, float muscle);
float fat();
float muscle();
Motion motion();

// Heaviest clip currently playing (debug / HUD).
const char* animName();

// Draws CJ at SA coords (x east, y north, z up) facing `heading` radians
// (0 = north, clockwise). Assumes the view matrix is already set.
void draw(float x, float y, float z, float heading);

// Total skinned vertices emitted per draw (for the HUD stats row).
int vertCount();

}
