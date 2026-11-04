#pragma once

// CHud port for the bottom screen. Draws the real in-game HUD (clock, money,
// wanted stars, weapon icon + ammo, health/armor/breath bars, zone & vehicle
// names) plus the rotating radar, all into the software framebuffer.
namespace chud {

// Loads hud/fonts assets + radar, one slice per call (loading screen);
// true once done. ready() tells hud::draw whether the HUD can be shown.
bool initStep();
bool ready();
// Renders one frame into the fb (call once per frame). Only regions whose
// content changed are repainted; invalidate() forces a full repaint.
void draw();
void invalidate();

// Fills buf with a one-line subsystem summary (tile cache, sprites, zone).
// Used by the fat-backend smoke marker in DEBUG builds.
void stats(char* buf, int n);

}
