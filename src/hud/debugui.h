#pragma once

#include "platform/input.h"

// Bottom-screen debug UI (SELECT toggles it with the game HUD). A row of
// touch tabs on top, one page each:
//   LOG   the hud::log ring, wrapped, newest at the bottom
//   PERF  fps, CPU per subsystem with bars, frame history, player/anim state
//   MEM   every mem region (used/cap/peak/largest free/fails), tags, VRAM, caches
//   GAME  quick actions: wanted, health, armour, money, weapons, fat/muscle,
//         sprint, clock, outfit, respawn
//   REND  render/sim switches: CJ mesh, skeleton, ground, fixed step, sim
//         speed/pause, camera distance, HUD repaint
// Immediate-mode: pages are reprinted every frame on the text console and
// buttons ([label]) react to a stylus tap on their characters.
namespace dbgui {

void enter();                          // console just (re)initialised: full repaint
void frame(const input::State& in, bool inGame);

}
