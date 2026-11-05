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
#ifdef GTASADS_DEBUG
#include "audio/aehardware.h"
#include "audio/audioengine.h"
#include "game/collision.h"
#include "game/timecycle.h"
#include "game/weather.h"
#include "game/debugopts.h"
#include "game/player.h"
#include "game/world.h"
#include "game/vehicle.h"
#include "game/vehicledata.h"
#include "game/vehiclemodel.h"
#include "platform/video.h"
#include "render/renderer.h"
#endif

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

#if defined(GTASADS_DEBUG) && defined(GTASADS_COLTEST)
// Headless collision test (after the screenshot sequence): overlays on, run
// north then east from the spawn with a held d-pad (walls must stop CJ),
// drop him from 6 m (must land on the floor), log to gtasads_coltest.txt and
// capture shot6 / shot7 at the end of each run.
static void colTest(int f) {
	static FILE* out;
	auto pos = [&](const char* what) {
		if (!out) return;
		fprintf(out, "%4d %-6s pos=%.2f,%.2f,%.2f cam=%.2f near=%u miss=%u steps=%lu land=%lu\n", f, what, player::g.x,
		        player::g.y, player::g.z, player::camDist(), col::stats().nearCount, col::stats().missing,
		        (unsigned long)audio::stats().footsteps, (unsigned long)audio::stats().landings);
	};
	if (f == 1) dbg::opts.collision = dbg::opts.skeleton = true;
	if (f == 600) {
		player::respawn();
		player::debugSetView(0.0f, 0.42f);
		out = fopen("fat:/gtasads_coltest.txt", "w");
		pos("spawn");
	}
	if (f == 620 || f == 800) input::debugHold(KEY_UP);
	if (f == 790) {
		player::debugSetView(1.5708f, 0.42f);
		dbg::opts.collision = dbg::opts.skeleton = false;   // shot7: plain render (camera collision)
	}
	if (f > 620 && f <= 940 && f % 20 == 0) pos(f < 790 ? "north" : "east");
	if (f == 770 || f == 940) input::debugHold(0);
	if (f == 780) video::requestCapture("fat:/gtasads_shot6.dscr");
	if (f == 950) video::requestCapture("fat:/gtasads_shot7.dscr");
	if (f == 960) {
		pos("drop");
		player::g.z += 6.0f;
	}
	if (f > 960 && f <= 1040 && f % 8 == 0) pos("fall");
	if (f == 1041 && out) {
		char ab[320];
		audio::statsLine(ab, sizeof ab);
		fprintf(out, "%s\n", ab);
		fclose(out);
		out = nullptr;
	}
}
#endif

#if defined(GTASADS_DEBUG) && defined(GTASADS_AUDTEST)
// Headless audio test (after the screenshot sequence): run, jump, punch,
// rain + a lightning flash (thunder), M4 bursts, then a sandstorm (the
// ambience stream). Writes the audio stats line per phase to
// gtasads_audtest.txt and records the mixer output (both capture units) of
// each phase to gtasads_aud_<phase>.wav (tools/audwav.py prints levels).
static void audTest(int f) {
	static FILE* out;
	static char capPath[40];
	auto log = [&](const char* what) {
		if (!out) return;
		char ab[320];
		audio::statsLine(ab, sizeof ab);
		fprintf(out, "%4d %-8s %s\n", f, what, ab);
	};
	auto cap = [&](const char* what, float seconds) {
		if (capPath[0] || !aehw::captureStart((uint32_t)(seconds * 16384.0f), 16384)) return;
		snprintf(capPath, sizeof capPath, "fat:/gtasads_aud_%s.wav", what);
	};
	if (capPath[0] && !aehw::captureBusy()) {
		aehw::captureSave(capPath);
		capPath[0] = 0;
	}
	if (f == 600) {
		player::respawn();
		player::debugSetView(0.0f, 0.42f);
		out = fopen("fat:/gtasads_audtest.txt", "w");
		log("spawn");
	}
	if (f == 605) cap("run", 2.5f);
	if (f == 610) input::debugHold(KEY_UP);
	if (f == 680) log("run");
	if (f == 690) input::debugHold(0);
	if (f == 700) input::debugHold(KEY_X);
	if (f == 704) input::debugHold(0);
	if (f == 770) log("jump");
	if (f == 780) input::debugHold(KEY_A);
	if (f == 784) input::debugHold(0);
	if (f == 820) log("punch");
	if (f == 830) weather::forceWeatherNow(weather::RAINY_SF);
	if (f == 940) cap("rain", 2.0f);
	if (f == 960) log("rain");
	if (f == 970) weather::debugFlash();
	if (f == 1005) cap("thunder", 3.0f);
	if (f == 1090) log("thunder");
	if (f == 1095) player::giveWeapon(player::WEAPON_M4, 500);
	if (f == 1110) input::debugHold(KEY_A);
	if (f == 1112) cap("m4", 2.0f);
	if (f == 1170) input::debugHold(0);
	if (f == 1172) log("m4");
	if (f == 1180) weather::forceWeatherNow(weather::SANDSTORM_DESERT);
	if (f == 1340) cap("sand", 2.0f);
	if (f == 1420) log("sand");
	if (f == 1421 && out) {
		fclose(out);
		out = nullptr;
	}
}
#endif

#if defined(GTASADS_DEBUG) && defined(GTASADS_VEHTEST)
// Headless vehicle test (after the screenshot sequence): spawn a Greenwood
// north of CJ (driver door facing him), Y to get in (go to door, open, getin,
// close), drive west with a held d-pad up, brake, Y to get out, then set the
// health under 250 (engine fire) until it blows up. gtasads_vehtest.txt gets
// one line per 20 frames, shot6..shot10 the stages.
static void vehTest(int f) {
	static FILE* out;
	static int id = -1;
	auto log = [&](const char* what) {
		if (!out) return;
		const veh::Vehicle* v = veh::get(id);
		fprintf(out, "%4d %-7s cj=%.2f,%.2f,%.2f hp=%d task=%d", f, what, player::g.x, player::g.y, player::g.z,
		        (int)player::g.health, player::vehicleId());
		if (v)
			fprintf(out, " car=%.2f,%.2f,%.2f up=%.2f spd=%.1fkmh g=%u hp=%d st=%u susp=%.2f,%.2f,%.2f,%.2f contact=%u sleep=%d door=%.2f gas=%.2f brk=%.2f hb=%d skid=%.2f",
			        v->pos[0], v->pos[1], v->pos[2], v->up[2], v->speedFwd * 180.0f, v->gear, (int)v->health, v->status,
			        v->susp[0], v->susp[1], v->susp[2], v->susp[3], v->numContact, (int)v->asleep, v->door[0], v->gas, v->brake,
			        (int)v->handbrake, v->skid);
		const veh::Stats& vs = veh::stats();
		const vehmdl::Stats& ms = vehmdl::stats();
		fprintf(out, " | mdl=%u/%u tex=%u fail=%u vram=%luK lists=%luK tris=%u step=%luus(q%lu) draw=%luus fps=%lu polys=%d",
		        ms.ready, ms.models, ms.textures, ms.texFailed, (unsigned long)(ms.texVram >> 10),
		        (unsigned long)(ms.listBytes >> 10), vs.polys, (unsigned long)vs.stepUs, (unsigned long)vs.queryUs,
		        (unsigned long)vs.drawUs, timing::fps(), renderer::polyCount());
		const world::Stats& ws = world::stats();
		const col::Stats& cs = col::stats();
		fprintf(out, " | 3d=%lu sim=%lu hud=%lu wu=%u wd=%u cu=%u cq=%u\n", (unsigned long)timing::sectionUs(timing::SEC_3D),
		        (unsigned long)timing::sectionUs(timing::SEC_SIM), (unsigned long)timing::sectionUs(timing::SEC_HUD),
		        ws.updateUs, ws.drawUs, cs.updateUs, cs.queryUs);
	};
	if (f == 600) {
		player::respawn();
		player::debugSetView(0.0f, 0.42f);
		out = fopen("fat:/gtasads_vehtest.txt", "w");
		id = veh::spawn(vehdata::findModel(492), player::g.x, player::g.y + 6.0f, player::g.z + 1.0f, 1.5708f, 0);
		log("spawn");
	}
	if (f > 600 && f < 1900 && f % 20 == 0) log(f < 700 ? "park" : f < 900 ? "enter" : f < 1160 ? "drive" : f < 1420 ? "exit" : "fire");
	if (f == 690) video::requestCapture("fat:/gtasads_shot6.dscr");
	if (f == 700) input::debugHold(KEY_Y);
	if (f == 703) input::debugHold(0);
	if (f == 760) video::requestCapture("fat:/gtasads_shot7.dscr");
	static int id2 = -1;
	if (f == 905) {   // a second car across the road 30 m ahead: rammed at ~70 km/h
		const veh::Vehicle* v = veh::get(id);
		if (v) id2 = veh::spawn(vehdata::findModel(492), v->pos[0] - 30.0f, v->pos[1], v->pos[2] + 0.5f, 0.0f, 1);
	}
	if (f > 905 && f < 1160 && f % 20 == 10 && out) {
		const veh::Vehicle* v = veh::get(id);
		const veh::Vehicle* w = veh::get(id2);
		if (v && w) {
			fprintf(out, "%4d crash   car1 hp=%d dmg=", f, (int)v->health);
			for (int k = vehmdl::COMP_DOOR_LF; k < vehmdl::COMP_COUNT; k++) fputc("o~x"[v->dmg[k]], out);
			fprintf(out, " car2 pos=%.2f,%.2f hp=%d spd=%.1f sleep=%d dmg=", w->pos[0], w->pos[1], (int)w->health,
			        w->speedFwd * 180.0f, (int)w->asleep);
			for (int k = vehmdl::COMP_DOOR_LF; k < vehmdl::COMP_COUNT; k++) fputc("o~x"[w->dmg[k]], out);
			fputc('\n', out);
		}
	}
	if (f == 910) input::debugHold(KEY_UP);
	if (f == 990) video::requestCapture("fat:/gtasads_shot8.dscr");
	if (f == 1100) input::debugHold(KEY_DOWN);
	if (f == 1160) input::debugHold(0);
	if (f == 1180) input::debugHold(KEY_Y);
	if (f == 1183) input::debugHold(0);
	if (f == 1300) video::requestCapture("fat:/gtasads_shot9.dscr");
	if (f == 1420) veh::setHealth(id, 249.0f);
	if (f == 1585) video::requestCapture("fat:/gtasads_shot10.dscr");
	if (f == 1910 && out) {
		log("end");
		char ab[320];
		audio::statsLine(ab, sizeof ab);
		fprintf(out, "%s\n", ab);
		fclose(out);
		out = nullptr;
	}
}
#endif

void draw() {
	if (s_screen == Screen::GAME) {
		if (s_gameActive && chud::ready()) {
			chud::draw();
#ifdef GTASADS_DEBUG
			// fat-backend smoke proof: after ~2s of game HUD, drop a marker on
			// the SD so headless test runs can confirm the full path rendered
			static int s_gameFrames;
			++s_gameFrames;
#ifdef GTASADS_COLTEST
			colTest(s_gameFrames);
#endif
#ifdef GTASADS_AUDTEST
			audTest(s_gameFrames);
#endif
#ifdef GTASADS_VEHTEST
			vehTest(s_gameFrames);
#endif
			// headless screenshots (tools/dsshot.py): one per compass direction
			// (shot0 at the marker frame, then turn, give streaming 60 frames,
			// capture), a bird's eye view (shot4), then a teleport 150 m west to
			// exercise sector streaming + eviction (shot5, gtasads_hudok2.txt)
			static const float k_yaw[6] = { 0.0f, 1.5708f, 3.1416f, -1.5708f, 0.0f, 3.1416f };
			for (int k = 0; k < 6; k++) {
				const int at = 120 + k * 90;
				if (k == 4 && s_gameFrames == at - 60) dbg::opts.camDist = 40.0f;
				if (k == 5 && s_gameFrames == at - 60) {
					dbg::opts.camDist = dbg::Options().camDist;
					player::g.x -= 150.0f;
				}
				if (k && s_gameFrames == at - 60) player::debugSetView(k_yaw[k], k == 4 ? 1.2f : 0.15f);
				if (s_gameFrames == at) {
					char path[40];
					snprintf(path, sizeof path, "fat:/gtasads_shot%d.dscr", k);
					video::requestCapture(path);
				}
			}
			const world::Stats& ws = world::stats();
			auto worldLine = [&](FILE* f) {
				fprintf(f, "world: sec=%u inst=%u cand=%u drawn=%u cullF=%u cullB=%u ready=%u loading=%u "
				        "tex=%u fail=%u lists=%luK texvram=%luK loads=%lu up=%lu polys=%u>%u upd=%uus draw=%uus scan=%uus\n",
				        ws.sectors, ws.instances, ws.candidates, ws.drawn, ws.culledFrustum, ws.culledBudget,
				        ws.modelsReady, ws.modelsLoading, ws.textures, ws.texFailed,
				        (unsigned long)(ws.listBytes >> 10), (unsigned long)(ws.texVram >> 10),
				        (unsigned long)ws.loads, (unsigned long)ws.uploads, ws.polysBefore, ws.polysAfter,
				        ws.updateUs, ws.drawUs, ws.scanUs);
				const col::Stats& cs = col::stats();
				fprintf(f, "col: models=%u bytes=%luK near=%u wants=%u miss=%u loads=%lu upd=%uus query=%uus push=%uus "
				        "ftests=%lu dbgfaces=%u pos=%.2f,%.2f,%.2f viewz=%.2f camdist=%.2f\n",
				        cs.models, (unsigned long)(cs.bytes >> 10), cs.nearCount, cs.wants, cs.missing,
				        (unsigned long)cs.loads, cs.updateUs, cs.queryUs, cs.pushUs, (unsigned long)cs.faceTests,
				        cs.debugFaces, player::g.x, player::g.y,
				        player::g.z, player::viewZ(), player::camDist());
				char ab[320];
				audio::statsLine(ab, sizeof ab);
				fprintf(f, "%s\n", ab);
			};
			if (s_gameFrames == 120 + 5 * 90 + 2) {
				char mb[128];
				mem::summary(mb, sizeof mb);
				FILE* f = fopen("fat:/gtasads_hudok2.txt", "w");
				if (f) {
					fprintf(f, "%s fps=%lu cpu=%luus heapok=%d\n", mb, timing::fps(), timing::frameUs(), (int)mem::check());
					worldLine(f);
					fclose(f);
				}
			}
			if (s_gameFrames == 120) {
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
					const timecycle::Colours& tc = timecycle::current();
					fprintf(f, "sky: w=%s>%s t=%.2f top=%d,%d,%d bot=%d,%d,%d far=%d fog=%d sunz=%.2f polys=%d\n",
					        weather::name(weather::g.oldType), weather::name(weather::g.newType),
					        weather::g.interpolation, tc.skyTopR, tc.skyTopG, tc.skyTopB, tc.skyBotR, tc.skyBotG,
					        tc.skyBotB, tc.farClip, tc.fogStart, timecycle::vectorToSun()[2], renderer::polyCount());
					worldLine(f);
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
