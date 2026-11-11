#pragma once

#include <stdint.h>

#include "assets/ds_formats.h"

class Pack;

// CAnimManager-lite over DANM v2 blocks. A block (ped.ifp, fat.ifp, ...)
// keeps only its directory resident (12 bytes per clip); a clip record is
// read from the cart the first time someone plays it (one ~2-8 KB read) and
// shared by every player of that clip. Clips are refcounted; unreferenced
// clips stay cached until the ANIM budget (mem/budget.h) needs the room,
// then go least-recently-used first. Pinned clips (the player's on-foot set,
// preloaded under the loading bar) never leave.
namespace animstore {

constexpr int NO_BLOCK = -1;

void init();
// Loose DANM file ("game:/anim/ped.ifp"); the file stays open.
int openFile(const char* path);
// DANM entry inside an open pack (anim.dspk: "fat.ifp"). The pack must
// outlive the block.
int openInPack(const Pack& pack, const char* entry);
int findBlock(const char* name);     // by DANM block name ("ped", "fat")
bool hasClip(int block, const char* name);

struct Clip;
// Loads (if needed) and references a clip. nullptr when missing/out of RAM.
Clip* acquire(int block, const char* name);
void release(Clip* c);
// Loads and keeps one permanent reference (resident set).
bool pin(int block, const char* name);
const ds::AnimEntry* entry(const Clip* c);

// Drops unreferenced clips until the cache fits `budget` bytes.
void trim(uint32_t budget);
uint32_t residentBytes();
int residentClips();

}
