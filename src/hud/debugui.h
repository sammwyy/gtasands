#pragma once

#include <stdio.h>

#include "platform/input.h"

// Bottom-screen debug UI (SELECT toggles it with the game HUD). A row of
// touch tabs on top, one page each:
//   LOG   the hud::log ring, wrapped, newest at the bottom
//   PERF  fps, CPU per subsystem with bars, frame history, player/anim state
//   MEM   every mem region (used/cap/peak/largest free/fails), tags, VRAM, caches
//   GAME  read-only state: CJ, the current vehicle, the population
//   REND  render/sim switches: CJ mesh, skeleton, ground, fixed step, sim
//         speed/pause, camera, map / ped poly budgets, collision overlay,
//         physics, water, vehicle lights, skidmarks
//   SKY   time cycle / weather state, sky / fog / weather fx switches
//   CHTS  every action (player, vehicles, peds, time + weather, world, warps),
//         one list per category; pages only when it overflows rows 1..21
// Immediate-mode into a RAM text buffer, rebuilt every 3rd frame (or on a
// tap); only the cells that changed are written to the console, so nothing
// flickers and no text of another tab is left behind. Buttons ([label])
// react to a stylus tap on their characters.
namespace dbgui {

void enter();                          // console just (re)initialised: full repaint
void frame(const input::State& in, bool inGame);
#ifdef GTASADS_DEBUG
void debugShow(int tab, int cheatCat);   // headless tests: tab 0..6, CHTS category 0..5
void debugDump(FILE* f);                 // what the console shows, one line per row
#endif

}
