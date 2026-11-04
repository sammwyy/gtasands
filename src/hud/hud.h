#pragma once

// Bottom screen (sub engine). Two modes toggled with SELECT:
//  - DEBUG: the original stats/log text console
//  - GAME:  the real GTA SA HUD (radar, money, weapon, wanted level, bars)
// rendered into a software framebuffer (see game/hud/).
namespace hud {

enum class Screen { DEBUG, GAME };

void init();
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void drawStats();          // stats rows on the debug console
void draw();               // per-frame dispatch on the active screen
void present();            // vblank: DMA the game-screen backbuffer to VRAM

Screen screen();
void setScreen(Screen s);
void toggleScreen();

}
