#pragma once

// CJ: the player model assembled from player.dspk clothing parts, animated by
// ped.ifp (DANM) and CPU-skinned on the ARM9 (the DS GE has no per-vertex
// matrices; dominant-bone skinning is done in fixed point per frame).
namespace cj {

// Loads the default outfit (SCM defaults: vest/jeansdenim/sneakerbincblk +
// player_face head; hands reuse the torso texture like BODY_PART_TEX_NAMES).
bool init();

// Switches the shared animation (ped.ifp entry name, e.g. "IDLE_stance").
void setAnim(const char* name);
const char* animName();

// Advances the play head by dt60 ticks (1/60 s). Call once per frame.
void advance(int dt60);

// Draws CJ at SA coords (x east, y north, z up) facing `heading` radians
// (0 = north, clockwise). Assumes the view matrix is already set.
void draw(float x, float y, float z, float heading);

// Total skinned vertices emitted per draw (for the HUD stats row).
int vertCount();

}
