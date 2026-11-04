#pragma once

#include <nds.h>

// CRadar port: rotating minimap rendered by the CPU into the bottom-screen
// framebuffer. Map tiles come from gta3.dspk (radarNN.txd, 12x12 grid of
// 500x500 world units), sprites from models/hud.txd.
namespace radar {

void init();
void draw();          // reads player::g each frame

// Blips (CRadar::SetCoordBlip-lite). sprite = texture name in hud.txd
// ("" = plain colored square). id 0..15.
void setBlip(int id, float x, float y, const char* sprite, u8 r, u8 g, u8 b);
void clearBlip(int id);

float range();        // current radar range (world units)
int tilesCached();    // map tiles currently decoded in the cache

}
