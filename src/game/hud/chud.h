#pragma once

// CHud port for the bottom screen. Draws the real in-game HUD (clock, money,
// wanted stars, weapon icon + ammo, health/armor/breath bars, zone & vehicle
// names) plus the rotating radar, all into the software framebuffer.
namespace chud {

bool init();   // loads hud/fonts assets + radar; false if assets unavailable
void draw();   // renders one frame into the fb (call once per frame)

// Fills buf with a one-line subsystem summary (tile cache, sprites, zone).
// Used by the fat-backend smoke marker in DEBUG builds.
void stats(char* buf, int n);

}
