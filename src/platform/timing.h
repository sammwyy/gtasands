#pragma once

#include <nds/ndstypes.h>

// Frame timing / profiling. Uses hardware timers 0+1 (cpuStartTiming).
namespace timing {

void init();
void frameStart();
void frameEnd();

u32 frameUs();      // CPU time of the last frame, microseconds
u32 fps();          // presented frames during the last second
u32 vblanks();
u32 ticks();                 // free-running CPU timer (33.5 MHz)
u32 usSince(u32 startTicks); // microseconds elapsed since a ticks() value

// Per-subsystem CPU time of the last frame (debug HUD / smoke markers).
enum Section { SEC_SIM, SEC_3D, SEC_HUD, SEC_COUNT };
void setSection(Section s, u32 us);
u32 sectionUs(Section s);

}
