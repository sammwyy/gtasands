#pragma once

#include <nds.h>

#include "assets/ds_formats.h"
#include "game/hud/subfb.h"

// DTEX decoding helpers for the software-rendered bottom HUD. Everything the
// software path touches lives in main RAM: sprites are (rgb15 color + u8
// alpha) planes, font glyphs are single u8 coverage maps, radar tiles are
// decoded once into flat rgb15 arrays.
namespace hudtex {

// Reads a whole nitro:/fat: file into a malloc'd buffer (caller frees).
void* readFile(const char* path);

// Finds a texture inside a DTXD blob; returns nullptr if missing/not a DTXD.
const ds::TexHeader* find(const void* dtxd, const char* name);

// Decodes a DTEX into a sprite (mallocs both planes; freeSprite releases).
bool decodeSprite(const ds::TexHeader* t, subfb::Sprite& out);
void freeSprite(subfb::Sprite& s);

// Decodes a DTEX into a coverage map (u8, 0..255): alpha * intensity. Used for
// font glyphs (a3i5/a5i3). Caller frees the returned buffer.
u8* decodeCoverage(const ds::TexHeader* t);

// Decodes a DTEX into a flat rgb15 array w*h (used for radar tiles).
u16* decodeRgb(const ds::TexHeader* t);
// Same, decoding into a caller-provided w*h buffer (hot-path friendly).
bool decodeRgbInto(const ds::TexHeader* t, u16* out);

}
