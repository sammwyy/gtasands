#pragma once

#include <stddef.h>

// Memory plan for the 4 MB main RAM (DS mode). The heap the crt0 leaves after
// code + BSS (~3.4 MB) is carved once, at the first allocation, into regions
// split by LIFETIME so short-lived data never punches holes between
// long-lived blocks:
//
//   SYS      TLSF   libc/libstdc++ malloc+new (stdio buffers, libfat cache,
//                   NitroFS). Our own code never mallocs directly.
//   SCRATCH  stack  temporary staging inside one call (file reads before an
//                   upload/decode, display-list build). LIFO, mem::Scratch.
//   PERM     bump   loaded once, never freed (HUD fonts, glyph cache, SFX
//                   banks, zones). Zero overhead, zero fragmentation.
//   STREAM   TLSF   everything that comes and goes: models, display lists,
//                   anim clips, radar tiles, pack TOCs, GXT. Gets whatever is
//                   left (all of the extra 12 MB on DSi).
//
// Fixed-count objects (radar tiles, anim clips, textures, and later peds /
// vehicles / objects) live in mem::Pool<T> blocks sized by the counts below,
// allocated once from STREAM -> no per-object heap traffic at all.
namespace mem::budget {

constexpr size_t SYS_BYTES = 160 * 1024;
constexpr size_t SCRATCH_BYTES = 160 * 1024;
constexpr size_t PERM_BYTES = 320 * 1024;

// Category soft budgets inside STREAM (checked by the debug HUD/logs).
constexpr size_t ANIM_CACHE_BYTES = 192 * 1024;   // streamed anim clips (LRU over refcount 0)

// Pool capacities (CPool-style, gta-reversed Core/Pool.h). The PC game uses
// 140 peds / 110 vehicles / 350 objects; the DS draws at most 2048 polygons,
// so the live world is much smaller.
constexpr int MAX_PEDS = 12;
constexpr int MAX_VEHICLES = 8;
constexpr int MAX_OBJECTS = 48;
constexpr int MAX_TEX_SLOTS = 64;         // VRAM texture slots (banks A+B+D)
constexpr int MAX_ANIM_CLIPS = 64;        // resident clips across all blocks
constexpr int MAX_ANIM_BLOCKS = 8;        // ped.ifp + fat/muscle + future vehicle/weapon blocks
constexpr int RADAR_TILE_SLOTS = 16;      // 64px tiles (8 KB): worst-case 4x4 window at range ~460
constexpr int RADAR_MIP_SLOTS = 36;       // 32px tiles (2 KB): worst-case 6x6 window at range 900

}
