#pragma once

// Bottom screen HUD (sub engine). Currently a text console used for stats and
// logs; radar/weapon/money widgets will be BG/sprite layers on the same engine.
namespace hud {

void init();
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void drawStats();

}
