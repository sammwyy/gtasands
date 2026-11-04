#include "hud/hud.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "assets/texture.h"
#include "game/hud/chud.h"
#include "game/hud/subfb.h"
#include "mem/mem.h"
#include "platform/input.h"
#include "platform/timing.h"
#include "render/renderer.h"

namespace hud {

static PrintConsole s_console;
static Screen s_screen = Screen::GAME;
static bool s_gameActive, s_cleared;

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
	applyGame();
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
	printf("\x1b[1;0Hfps %3lu  cpu %6lu us (%3lu%%)  ", timing::fps(), timing::frameUs(), timing::frameUs() * 100 / 16715);
	printf("\x1b[2;0H3d %5lu sim %5lu hud %5lu us ", timing::sectionUs(timing::SEC_3D),
	       timing::sectionUs(timing::SEC_SIM), timing::sectionUs(timing::SEC_HUD));
	printf("\x1b[3;0Hpolys %4d/2048  verts %4d/6144 ", renderer::polyCount(), renderer::vertCount());
	printf("\x1b[4;0Htouch %s %3d,%3d   ", in.touching ? "on " : "off", in.touchX, in.touchY);
	// memory: STREAM used/capacity + largest free block (fragmentation
	// shows as lf << free), PERM, SYS (libc), SCRATCH peak, VRAM textures
	const mem::RegionStats st = mem::stats(mem::REGION_STREAM), pm = mem::stats(mem::REGION_PERM);
	const mem::RegionStats sy = mem::stats(mem::REGION_SYS), sc = mem::stats(mem::REGION_SCRATCH);
	printf("\x1b[5;0Hstr %4u/%4uK lf %4uK pk %4uK", (unsigned)(st.used >> 10), (unsigned)(st.capacity >> 10),
	       (unsigned)(st.largestFree >> 10), (unsigned)(st.peak >> 10));
	printf("\x1b[6;0Hperm %3u/%3uK sys %3u/%3uK scr %3uK", (unsigned)(pm.used >> 10), (unsigned)(pm.capacity >> 10),
	       (unsigned)(sy.used >> 10), (unsigned)(sy.capacity >> 10), (unsigned)(sc.peak >> 10));
	printf("\x1b[7;0Hmdl %3uK anim %3uK rad %3uK vram %3uK ", (unsigned)(mem::tagBytes(mem::TAG_MODEL) >> 10),
	       (unsigned)(mem::tagBytes(mem::TAG_ANIM) >> 10), (unsigned)(mem::tagBytes(mem::TAG_RADAR) >> 10),
	       (unsigned)(tex::vramBytes() >> 10));
	printf("\x1b[8;0H\x1b[36mDPad move B sprint X jump SELECT hud\x1b[39m");
	printf("\x1b[9;0H--------------------------------");
	consoleSetWindow(&s_console, 0, 10, 32, 14);
	consoleSelect(prev);
}

void draw() {
	if (s_screen == Screen::GAME) {
		if (s_gameActive && chud::ready()) {
			chud::draw();
#ifdef GTASADS_DEBUG
			// fat-backend smoke proof: after ~2s of game HUD, drop a marker on
			// the SD so headless test runs can confirm the full path rendered
			static int s_gameFrames;
			if (++s_gameFrames == 120) {
				char buf[160], mb[128];
				chud::stats(buf, sizeof buf);
				mem::summary(mb, sizeof mb);
				FILE* f = fopen("fat:/gtasads_hudok.txt", "w");
				if (f) {
					fprintf(f, "%s\n%s fps=%lu cpu=%luus (3d %lu sim %lu hud %lu) heapok=%d\n", buf, mb, timing::fps(),
					        timing::frameUs(), timing::sectionUs(timing::SEC_3D), timing::sectionUs(timing::SEC_SIM),
					        timing::sectionUs(timing::SEC_HUD), (int)mem::check());
					fprintf(f, "tags:");
					for (int t = 0; t < mem::TAG_COUNT; t++)
						fprintf(f, " %s=%uK", mem::tagName((mem::Tag)t), (unsigned)(mem::tagBytes((mem::Tag)t) >> 10));
					fprintf(f, " vram=%uK\n", (unsigned)(tex::vramBytes() >> 10));
					for (int i = 0; i < LOG_LINES; i++) {
						const char* l = s_log[(s_logHead + i) % LOG_LINES];
						if (*l) fprintf(f, "log: %s\n", l);
					}
					fclose(f);
				}
			}
#endif
			return;
		}
		// HUD not loaded yet (frontend / loading screen): flat sky, drawn
		// once — the backbuffer keeps it, no need to refill it every frame
		if (!s_cleared) subfb::clear(RGB15(10, 14, 22) | BIT(15));
		s_cleared = true;
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

void setGameActive(bool on) {
	s_gameActive = on;
	s_cleared = false;
	if (on) chud::invalidate();
}

}
