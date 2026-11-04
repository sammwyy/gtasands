#include "hud/hud.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "game/hud/chud.h"
#include "game/hud/subfb.h"
#include "platform/input.h"
#include "platform/timing.h"
#include "render/renderer.h"

namespace hud {

static PrintConsole s_console;
static Screen s_screen = Screen::DEBUG;

// last few log lines kept so they survive the debug<->game switch (the
// console is re-initialized, and cleared, on every switch)
static const int LOG_LINES = 14, LOG_LEN = 64;
static char s_log[LOG_LINES][LOG_LEN];
static int s_logHead; // next slot to write

static void applyDebug() {
	videoSetModeSub(MODE_0_2D);
	consoleInit(&s_console, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
	consoleSelect(&s_console);
	consoleSetWindow(&s_console, 0, 10, 32, 14);
	for (int i = 0; i < LOG_LINES; i++) {
		const char* l = s_log[(s_logHead + i) % LOG_LINES];
		if (*l) printf("%s\n", l);
	}
}

static void applyGame() {
	subfb::init();
}

// rows 0..9 = stats panel, rows 10..23 = scrolling log
void init() {
	applyDebug();
}

void log(const char* fmt, ...) {
	char buf[LOG_LEN];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	memcpy(s_log[s_logHead], buf, LOG_LEN); // vsnprintf already NUL-terminated
	s_logHead = (s_logHead + 1) % LOG_LINES;
	if (s_screen == Screen::DEBUG) printf("%s\n", buf);
}

void drawStats() {
	const input::State& in = input::get();
	PrintConsole* prev = consoleSelect(&s_console);
	consoleSetWindow(&s_console, 0, 0, 32, 10);
	printf("\x1b[0;0H\x1b[33mGTA:SA DS\x1b[39m  ---------------------");
	printf("\x1b[2;0Hfps %3lu  cpu %6lu us (%3lu%%)  ", timing::fps(), timing::frameUs(), timing::frameUs() * 100 / 16715);
	printf("\x1b[3;0Hpolys %4d/2048  verts %4d/6144 ", renderer::polyCount(), renderer::vertCount());
	printf("\x1b[5;0Htouch %s %3d,%3d   ", in.touching ? "on " : "off", in.touchX, in.touchY);
	printf("\x1b[7;0H\x1b[36mDPad orbit  L/R zoom  SELECT hud\x1b[39m");
	printf("\x1b[9;0H--------------------------------");
	consoleSetWindow(&s_console, 0, 10, 32, 14);
	consoleSelect(prev);
}

void draw() {
	if (s_screen == Screen::GAME) {
		if (chud::init()) {
			chud::draw();
#ifdef GTASADS_DEBUG
			// fat-backend smoke proof: after ~2s of game HUD, drop a marker on
			// the SD so headless test runs can confirm the full path rendered
			static int s_gameFrames;
			if (++s_gameFrames == 120) {
				char buf[160];
				chud::stats(buf, sizeof buf);
				FILE* f = fopen("fat:/gtasads_hudok.txt", "w");
				if (f) { fprintf(f, "%s\n", buf); fclose(f); }
			}
#endif
			return;
		}
		// assets unavailable yet (e.g. toggled before load): minimal note
		subfb::clear(subfb::rgb(10, 14, 22));
		return;
	}
	drawStats();
}

// After endFrame()'s vblank wait: DMA the software-rendered HUD into VRAM C.
// DEBUG mode writes console tiles directly (tiny, no present needed).
void present() {
	if (s_screen == Screen::GAME) subfb::present();
}

Screen screen() { return s_screen; }

void setScreen(Screen s) {
	if (s == s_screen) return;
	s_screen = s;
	if (s == Screen::DEBUG) applyDebug();
	else applyGame();
}

void toggleScreen() { setScreen(s_screen == Screen::DEBUG ? Screen::GAME : Screen::DEBUG); }

}
