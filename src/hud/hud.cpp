#include "hud/hud.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>

#include "platform/input.h"
#include "platform/timing.h"
#include "render/renderer.h"

namespace hud {

static PrintConsole s_console;

// rows 0..9 = stats panel, rows 10..23 = scrolling log
void init() {
	consoleInit(&s_console, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
	consoleSetWindow(&s_console, 0, 10, 32, 14);
	consoleSelect(&s_console);
}

void log(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

void drawStats() {
	const input::State& in = input::get();
	PrintConsole* prev = consoleSelect(&s_console);
	consoleSetWindow(&s_console, 0, 0, 32, 10);
	printf("\x1b[0;0H\x1b[33mGTA:SA DS\x1b[39m  ---------------------");
	printf("\x1b[2;0Hfps %3lu  cpu %6lu us (%3lu%%)  ", timing::fps(), timing::frameUs(), timing::frameUs() * 100 / 16715);
	printf("\x1b[3;0Hpolys %4d/2048  verts %4d/6144 ", renderer::polyCount(), renderer::vertCount());
	printf("\x1b[5;0Htouch %s %3d,%3d   ", in.touching ? "on " : "off", in.touchX, in.touchY);
	printf("\x1b[7;0H\x1b[36mDPad orbit  L/R zoom  START quit\x1b[39m");
	printf("\x1b[9;0H--------------------------------");
	consoleSetWindow(&s_console, 0, 10, 32, 14);
	consoleSelect(prev);
}

}
