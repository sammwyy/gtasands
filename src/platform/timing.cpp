#include "platform/timing.h"

#include <nds.h>

namespace timing {

static volatile u32 s_vblanks;
static u32 s_frameStart, s_frameUs, s_frames, s_fps, s_lastSecond;

static void onVBlank() { s_vblanks++; }

void init() {
	irqSet(IRQ_VBLANK, onVBlank);
	irqEnable(IRQ_VBLANK);
	cpuStartTiming(0);
}

void frameStart() { s_frameStart = cpuGetTiming(); }

void frameEnd() {
	s_frameUs = timerTicks2usec(cpuGetTiming() - s_frameStart);
	s_frames++;
	if (s_vblanks - s_lastSecond >= 60) {
		s_fps = s_frames;
		s_frames = 0;
		s_lastSecond = s_vblanks;
	}
}

u32 frameUs() { return s_frameUs; }
u32 fps() { return s_fps; }
u32 vblanks() { return s_vblanks; }
static u32 s_section[SEC_COUNT];
void setSection(Section s, u32 us) { s_section[s] = us; }
u32 sectionUs(Section s) { return s_section[s]; }

u32 ticks() { return cpuGetTiming(); }
u32 usSince(u32 startTicks) { return timerTicks2usec(cpuGetTiming() - startTicks); }

}
