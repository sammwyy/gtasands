#pragma once

#include <nds.h>

// CRadar port: rotating minimap rendered by the CPU into the bottom-screen
// framebuffer. Map tiles (12x12 grid of 500x500 world units) are streamed
// from models/radar.dsrt into small pools around the player; sprites come
// from models/hud.txd.
namespace radar {

constexpr int MAP_TYPE_COUNT = 15;
bool init();
void prefetch();      // load every tile around the player now (loading screen)
void draw();          // reads player::g each frame, streams tiles within a byte budget
void drawPauseMap(float centerX, float centerY);
void drawFullPoints(int left, int top, int size);
int legendTypes(float centerX, float centerY, bool full, u8* out, int capacity);
int legendRadarTypes(u8* out, int capacity);
const char* typeIcon(int type);
const char* typeLabelKey(int type);
void drawLegendIcon(int type, int x, int y, int size);

// Blips (CRadar::SetCoordBlip-lite). sprite = texture name in hud.txd
// ("" = plain colored square). id 0..15.
void setBlip(int id, float x, float y, const char* sprite, u8 r, u8 g, u8 b);
void clearBlip(int id);

float range();        // current radar range (world units)
int tilesCached();    // map tiles currently decoded in the cache

}
