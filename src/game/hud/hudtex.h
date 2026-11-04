#pragma once

#include <nds.h>

#include "assets/ds_formats.h"
#include "game/hud/subfb.h"

// DTEX decoding helpers for the software-rendered bottom HUD. Everything the
// software path touches lives in main RAM: sprites are (rgb15 color + u8
// alpha) planes, font glyphs are single u8 coverage maps.
namespace hudtex {

// Finds a texture inside a DTXD blob; returns nullptr if missing/not a DTXD.
const ds::TexHeader* find(const void* dtxd, const char* name);

// Decodes a DTEX into a sprite (both planes in one mem block, TAG_HUD;
// freeSprite releases).
bool decodeSprite(const ds::TexHeader* t, subfb::Sprite& out);
void freeSprite(subfb::Sprite& s);

// Decodes a DTEX into a coverage map (u8, 0..255): alpha * intensity, in the
// PERM arena (font glyph atlases, loaded once).
u8* decodeCoverage(const ds::TexHeader* t);

// Decodes into caller buffers (w*h each). alpha may be null.
bool decodeInto(const ds::TexHeader* t, u16* col, u8* alpha);

}
