#pragma once

// Bottom screen (sub engine). Two modes toggled with SELECT:
//  - DEBUG: tabbed touch debug UI on the text console (hud/debugui)
//  - GAME:  the real GTA SA HUD (radar, money, weapon, wanted level, bars)
// rendered into a software framebuffer (see game/hud/).
namespace hud {

enum class Screen { DEBUG, GAME };

void init();
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Log ring (last 32 lines) for the debug UI. 0 = newest.
int logCount();
const char* logLine(int newest);
unsigned logSeq();         // bumps on every log/clear
void logClear();
void draw();               // per-frame dispatch on the active screen
void present();            // vblank: DMA the game-screen backbuffer to VRAM

Screen screen();
void setScreen(Screen s);
void toggleScreen();
// The game HUD is drawn only while playing (set by main on INGAME); before
// that the GAME screen stays a flat backdrop.
void setGameActive(bool on);

}
