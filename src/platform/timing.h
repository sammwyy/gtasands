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

}
