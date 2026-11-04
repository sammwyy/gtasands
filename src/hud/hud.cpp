#include "hud/hud.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "assets/texture.h"
#include "game/hud/chud.h"
#include "game/hud/subfb.h"
#include "hud/debugui.h"
#include "mem/mem.h"
#include "platform/input.h"
#include "platform/timing.h"

namespace hud {

static PrintConsole s_console;
static Screen s_screen = Screen::GAME;
static bool s_gameActive, s_cleared;

// log ring: survives screen switches, shown by the debug UI's LOG tab
static const int LOG_LINES = 32, LOG_LEN = 64;
static char s_log[LOG_LINES][LOG_LEN];
static int s_logHead;   // next slot to write
static int s_logCount;
static unsigned s_logSeq;

static void applyDebug() {
	videoSetModeSub(MODE_0_2D);
	consoleInit(&s_console, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
	consoleSelect(&s_console);
	consoleSetWindow(&s_console, 0, 0, 32, 24);
	dbgui::enter();
}

static void applyGame() {
	subfb::init();
}

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
	if (s_logCount < LOG_LINES) s_logCount++;
	s_logSeq++;
}

int logCount() { return s_logCount; }
const char* logLine(int newest) {
	if (newest < 0 || newest >= s_logCount) return "";
	return s_log[(s_logHead - 1 - newest + LOG_LINES) % LOG_LINES];
}
unsigned logSeq() { return s_logSeq; }
void logClear() {
	s_logCount = 0;
	s_logSeq++;
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
	dbgui::frame(input::get(), s_gameActive);
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
