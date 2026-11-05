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
constexpr size_t ANIM_CACHE_BYTES = 256 * 1024;   // streamed anim clips (LRU over refcount 0); CJ pins ~100K, peds share the rest
constexpr size_t WORLD_LIST_BYTES = 900 * 1024;   // compiled map-object display lists (LRU)
constexpr size_t WORLD_TEX_VRAM = 280 * 1024;     // map textures in VRAM (LRU over refcount 0); 384K total, ~20K left for vehicles
constexpr size_t COL_CACHE_BYTES = 192 * 1024;    // map collision records around the player (LRU)
constexpr int COL_NEAR_MAX = 48;                  // collision instances queried by the player / camera
// Audio (src/audio): resident DSND sounds, LRU over unused + unpinned ones.
// In game ~110 KB (feet, weather loops, weapon, pain); the loading tune adds
// ~100 KB while it plays (frontend + preload overlap stays under this).
constexpr size_t AUDIO_CACHE_BYTES = 256 * 1024;
constexpr int AUDIO_CACHE_ENTRIES = 128;          // (bank, sound) entries
constexpr int AUDIO_SOUNDS = 32;                  // CAESoundManager virtual sounds (PC: 300)
constexpr size_t AUDIO_STREAM_RING = 16 * 1024;   // ambience track PCM16 ring (0.5 s at 16 kHz)

// Streamed world (game/world.cpp)
constexpr int WORLD_MODEL_SLOTS = 192;    // map models resident (display list + textures)
constexpr int WORLD_TEX_ENTRIES = 320;    // map textures (txd, name) -> VRAM texture
constexpr int WORLD_SECTOR_SLOTS = 96;    // loaded sectors (window around the camera, ~55 at fogView 140)
constexpr int WORLD_VISIBLE_MAX = 384;    // instances drawn per frame (after culling)

// Pool capacities (CPool-style, gta-reversed Core/Pool.h). The PC game uses
// 140 peds / 110 vehicles / 350 objects; the DS draws at most 2048 polygons,
// so the live world is much smaller.
constexpr int MAX_PEDS = 12;
// Ped population (game/peds, game/pedmodel): CStreaming::ms_pedsLoaded is 8 on
// PC; each resident ped model is one ~270-tri skinned list (~12 KB) + skeleton.
constexpr int PED_MODEL_SLOTS = 6;
constexpr int MAX_VEHICLES = 8;
constexpr int MAX_OBJECTS = 48;
constexpr int MAX_TEX_SLOTS = 384;        // VRAM texture slots (banks A+B+D), mostly map textures
constexpr int MAX_ANIM_CLIPS = 96;        // resident clips across all blocks (CJ pins 27, peds ~30 more)
constexpr int MAX_ANIM_BLOCKS = 16;       // ped.ifp + fat/muscle + future vehicle/weapon blocks
constexpr int RADAR_TILE_SLOTS = 16;      // 64px tiles (8 KB): worst-case 4x4 window at range ~460
constexpr int RADAR_MIP_SLOTS = 36;       // 32px tiles (2 KB): worst-case 6x6 window at range 900

}
