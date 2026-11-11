#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mem/budget.h"

// Custom memory manager (see mem/budget.h for the region plan). Every
// allocation in src/ goes through here with a tag, so the debug HUD can show
// where the RAM is going. picolibc uses its own separate SYS heap.
namespace mem {

enum Tag : uint8_t {
	TAG_SYS,       // libc / libstdc++
	TAG_TEXT,      // GXT
	TAG_MODEL,     // DMDL blobs, display lists, skeletons
	TAG_TEXTURE,   // DTEX staging, composites
	TAG_ANIM,      // DANM clips + block directories
	TAG_AUDIO,     // DSND sample data
	TAG_RADAR,     // radar tiles
	TAG_HUD,       // decoded sprites, font coverage, glyph cache
	TAG_PACK,      // DSPK TOCs / page caches
	TAG_WORLD,     // zones, IDE/IPL tables, pools
	TAG_COL,       // map collision records + index (game/collision)
	TAG_PED,       // ped population: peds.dped tables, ped pool, poses (game/peds)
	TAG_TEMP,      // scratch overflow
	TAG_COUNT
};

// Carves the regions. Runs automatically on the first allocation (libc can
// allocate before main), calling it again is a no-op.
void init();

// STREAM heap: variable-size resources with a lifetime.
void* alloc(size_t n, Tag tag, size_t align = 8);
void* zalloc(size_t n, Tag tag);
void* realloc(void* p, size_t n, Tag tag);
void free(void* p);   // any pointer from alloc/zalloc/realloc (nullptr ok)

// PERM arena: loaded once, never freed.
void* perm(size_t n, Tag tag, size_t align = 4);

// SCRATCH stack: temporaries released when the Scratch goes out of scope.
// Nested scopes are fine; release order must be LIFO (it is, with RAII).
// If the stack is full the allocation falls back to STREAM and is still
// released by the scope.
class Scratch {
public:
	Scratch();
	~Scratch();
	Scratch(const Scratch&) = delete;
	Scratch& operator=(const Scratch&) = delete;
	void* alloc(size_t n, size_t align = 8);

private:
	uint8_t* m_mark;
	void* m_spill[4];
	int m_nspill;
};

struct RegionStats {
	size_t capacity, used, peak, largestFree;
	uint32_t blocks, failures;
};
enum Region : uint8_t { REGION_SYS, REGION_SCRATCH, REGION_PERM, REGION_STREAM, REGION_COUNT };
RegionStats stats(Region r);
uint32_t tagBytes(Tag t);           // live STREAM+PERM bytes per tag
const char* tagName(Tag t);
size_t totalBytes();                // everything mem manages
// One-line summary for logs/the debug HUD ("str 812/2304K lf 1400K ...").
void summary(char* buf, int n);
// Walks the heaps and checks invariants (slow, debug only).
bool check();

}
