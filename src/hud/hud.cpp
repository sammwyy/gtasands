#include "hud/hud.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "assets/texture.h"
#include "game/hud/chud.h"
#include "game/hud/radar.h"
#include "game/menu/menu.h"
#include "game/hud/subfb.h"
#include "hud/debugui.h"
#include "mem/mem.h"
#include "platform/input.h"
#include "platform/timing.h"
#ifdef GTASADS_DEBUG
#include "audio/aehardware.h"
#include "audio/audioengine.h"
#include "audio/aeradio.h"
#include "game/cj.h"
#include "game/collision.h"
#include "game/cutscene.h"
#include "game/timecycle.h"
#include "game/weather.h"
#include "game/debugopts.h"
#include "game/peddata.h"
#include "game/pedmodel.h"
#include "game/peds.h"
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

#if defined(GTASADS_DEBUG) && defined(GTASADS_VEHSPAWNTEST)
// Headless spawn regression: the debug-UI spawn in front of CJ for a car and
// a bike, CJ facing 4 directions at the Grove spawn. Each run logs where it
// was placed, then CJ gets in and drives 100 frames: `moved` must be > 0
// (a vehicle wedged in a wall / kerb / shelter stays at ~0).
static void vehSpawnTest(int f) {
	static FILE* out;
	static int id = -1;
	static float sx, sy;
	if (f < 600) return;
	const int run = (f - 600) / 480, t = (f - 600) % 480;
	if (run >= 8) {
		if (out) {
			for (int i = 0; i < LOG_LINES; i++) {
				const char* l = s_log[(s_logHead + i) % LOG_LINES];
				if (*l) fprintf(out, "log: %s\n", l);
			}
			fclose(out);
			out = nullptr;
		}
		return;
	}
	const int model = run & 1 ? 522 : 492;
	const veh::Vehicle* v = veh::get(id);
	if (t == 0) {
		if (!out) out = fopen("fat:/gtasads_vehspawn.txt", "w");
		veh::removeAll();
		player::respawn();
		player::g.heading = (run >> 1) * 1.5708f;
		player::debugSetView(player::g.heading, 0.3f);
		id = veh::spawnInFront(vehdata::findModel(model));
	}
	if (t == 100 && out) {
		fprintf(out, "run %d model %d id %d hdg %.2f cj=%.1f,%.1f,%.2f ", run, model, id, player::g.heading, player::g.x, player::g.y, player::g.z);
		if (v) fprintf(out, "car=%.1f,%.1f,%.2f up=%.2f contact=%u sleep=%d ", v->pos[0], v->pos[1], v->pos[2], v->up[2], v->numContact, (int)v->asleep);
		else fprintf(out, "car=none ");
	}
	if (t == 110) input::debugHold(KEY_Y);
	if (t == 113) input::debugHold(0);
	if (t == 330 && v) { sx = v->pos[0]; sy = v->pos[1]; }
	if (t == 330) input::debugHold(KEY_UP);
	if (t == 430) {
		input::debugHold(0);
		if (out) {
			if (v) fprintf(out, "in=%d moved=%.1f spd=%.1fkmh up=%.2f\n", player::vehicleId() == id,
			               sqrtf((v->pos[0] - sx) * (v->pos[0] - sx) + (v->pos[1] - sy) * (v->pos[1] - sy)),
			               v->speedFwd * 180.0f, v->up[2]);
			else fprintf(out, "lost\n");
			fflush(out);
		}
	}
}
#endif

#if defined(GTASADS_DEBUG) && defined(GTASADS_MENUAUDTEST)
// Headless pause-menu test: opens the pause menu, scrolls the items (menu
// beeps), records the mixer (gtasads_menu.wav) and the frame cost per 30
// frames into gtasads_menutest.txt, closes it and walks on.
static void menuAudTest(int f) {
	static FILE* out;
	if (f == 600) out = fopen("fat:/gtasads_menutest.txt", "w");
	if (f == 610 || f == 1100) input::debugHold(KEY_START);
	if (f == 612 || f == 1102) input::debugHold(0);
	if (f == 660) aehw::captureStart(16384 * 3 / 2, 16384);
	for (int k = 0; k < 10; k++) {
		if (f == 670 + k * 14) input::debugHold(k & 4 ? KEY_UP : KEY_DOWN);
		if (f == 672 + k * 14) input::debugHold(0);
	}
	if (f >= 860 && f < 1000 && !aehw::captureBusy()) aehw::captureSave("fat:/gtasads_menu.wav");
	if (out && f >= 600 && f <= 1300 && f % 30 == 0) {
		char ab[320];
		audio::statsLine(ab, sizeof ab);
		fprintf(out, "%d menu=%d scr=%d tl=%lu ready=%u evict=%lu drawn=%u wu=%u fps=%lu cpu=%lu (3d %lu sim %lu hud %lu) %s\n", f,
		        (int)FrontEndMenuManager.m_bMenuActive, (int)FrontEndMenuManager.m_nCurrentScreen, (unsigned long)radar::tileLoads(), world::stats().modelsReady, (unsigned long)world::stats().evictions,
		        world::stats().drawn, world::stats().updateUs,
		        (unsigned long)timing::fps(), (unsigned long)timing::frameUs(), (unsigned long)timing::sectionUs(timing::SEC_3D),
		        (unsigned long)timing::sectionUs(timing::SEC_SIM), (unsigned long)timing::sectionUs(timing::SEC_HUD), ab);
	}
	if (f == 1301 && out) { fclose(out); out = nullptr; }
}
#endif

#if defined(GTASADS_DEBUG) && defined(GTASADS_RADIOTEST)
static void radioTest(int f) {
	static FILE* out;
	if (f == 600) {
		player::respawn();
		dbg::opts.alwaysRadio = true;
		out = fopen("fat:/gtasads_radiotest.txt", "w");
	}
	if (f == 700 || f == 800) input::debugHold(KEY_L | (f == 800 ? KEY_UP : 0));
	if (f == 703) input::debugHold(0);
	if (f == 803) input::debugHold(KEY_UP);
	if (f == 830) input::debugHold(0);
	if (f == 850) dbg::opts.alwaysRadio = false;
	if (f == 710) aehw::captureStart(32768, 16384);
	if (f == 870 && !aehw::captureBusy()) aehw::captureSave("fat:/gtasads_radio.wav");
	if (out && f >= 620 && f <= 900 && f % 20 == 0) {
		char ab[320];
		audio::statsLine(ab, sizeof ab);
		fprintf(out, "%d car=%d station=%d fps=%lu %s\n", f, player::vehicleId(), aeradio::station(),
		        (unsigned long)timing::fps(), ab);
	}
	if (f == 901 && out) { fclose(out); out = nullptr; }
}
#endif

#if defined(GTASADS_DEBUG) && defined(GTASADS_SKIDTEST)
static void skidTest(int f) {
	static int id = -1;
	static float turnSkid, turnSpeed;
	static bool driving;
	if (f == 600) {
		player::respawn();
		id = veh::spawn(vehdata::findModel(492), player::g.x, player::g.y + 6.0f, player::g.z + 1.0f, 1.5708f, 0);
		veh::setDriver(id, true);
	}
	if (f == 625) player::enterVehicleNow(id);
	if (f == 700) input::debugHold(KEY_UP);
	if (f == 710) input::debugHold(KEY_UP | KEY_LEFT);
	if (f == 720) {
		const veh::Vehicle* v = veh::get(id);
		driving = player::vehicleId() == id;
		turnSkid = v ? v->skid : -1.0f;
		turnSpeed = v ? v->speedFwd * 180.0f : 0.0f;
	}
	if (f == 730) input::debugHold(KEY_UP);
	if (f == 740) {
		const veh::Vehicle* v = veh::get(id);
		FILE* out = fopen("fat:/gtasads_skidtest.txt", "w");
		if (out) {
			fprintf(out, "turn driving=%d speed=%.1f skid=%.2f\n", driving, turnSpeed, turnSkid);
			fprintf(out, "straight driving=%d speed=%.1f skid=%.2f\n", player::vehicleId() == id,
			        v ? v->speedFwd * 180.0f : 0.0f, v ? v->skid : -1.0f);
			fclose(out);
		}
	}
}
#endif

#if defined(GTASADS_DEBUG) && defined(GTASADS_WORLDTEST)
static void worldTest(int f) {
	static FILE* out;
	static void* pressure;
	if (f == 600) {
		out = fopen("fat:/gtasads_worldtest.txt", "w");
		player::g.x = -2150.0f; player::g.y = 550.0f; player::g.z = 34.5f;
	}
	if (f == 760) {
		player::g.x = 1942.0f; player::g.y = 1073.0f; player::g.z = 10.0f;
	}
	if (f == 880) {
		player::g.x = 2515.0f; player::g.y = 1975.0f; player::g.z = 10.0f;
	}
	if (f == 1018) pressure = mem::alloc(24 * 1024, mem::TAG_TEMP);
	if (f == 1022) player::g.x += 50.0f;
	if (f == 1026 && out) {
		const world::Stats& s = world::stats();
		fprintf(out, "boundary: ready=%u drawn=%u sectors=%u kept=%d pressure=%d\n",
		        s.modelsReady, s.drawn, s.sectors, s.modelsReady >= 30, pressure != nullptr);
	}
	if (f == 1034) { mem::free(pressure); pressure = nullptr; }
	if (f == 1040) {
		player::g.x = -68.5f; player::g.y = 1020.75f; player::g.z = 19.2f;
	}
	if ((f == 700 || (f >= 780 && f <= 1180 && f % 20 == 0)) && out) {
		const world::Stats& s = world::stats();
		char memLine[128];
		mem::summary(memLine, sizeof memLine);
		fprintf(out, "%d %s: sectors=%u inst=%u cand=%u drawn=%u ready=%u loading=%u loads=%lu tex=%u fail=%u lists=%luK %s\n",
		        f, f == 700 ? "SF" : f < 880 ? "LV" : f < 1040 ? "LV north" : "countryside",
		        s.sectors, s.instances, s.candidates, s.drawn,
		        s.modelsReady, s.modelsLoading, (unsigned long)s.loads, s.textures, s.texFailed,
		        (unsigned long)(s.listBytes >> 10), memLine);
		int paths = 0, roads = 0;
		peddata::forNodesNear(player::g.x, player::g.y, 40.0f, [&](int) { paths++; return true; });
		peddata::forRoadNodesNear(player::g.x, player::g.y, 40.0f, [&](int) { roads++; return true; });
		fprintf(out, "  pos=%.1f,%.1f,%.1f path=%d road=%d nearest=%d peds=%u traffic=%u pathUs=%u peak=%u pathLoads=%lu\n",
		        player::g.x, player::g.y, player::g.z, paths, roads,
		        peddata::nearestNode(player::g.x, player::g.y, player::g.z, 40.0f),
		        peds::stats().live, veh::stats().traffic, peddata::stats().pathUs, peddata::stats().pathPeakUs,
		        (unsigned long)peddata::stats().pathLoads);
		if (f == 700 || f == 860 || f == 1020 || f == 1180)
			video::requestCapture(f == 700 ? "fat:/gtasads_world_sf.dscr" : f == 860 ? "fat:/gtasads_world_lv.dscr" :
			                      f == 1020 ? "fat:/gtasads_world_north.dscr" : "fat:/gtasads_world_country.dscr");
		if (f == 1180) { fprintf(out, "heapok=%d\n", mem::check()); fclose(out); out = nullptr; }
	}
}
#endif

#if defined(GTASADS_DEBUG) && defined(GTASADS_FLICKERTEST)
static void flickerTest(int f) {
	static FILE* out;
	if (f == 1) {
		cutscene::stop();
		if (world::area() != 0) world::setArea(0);
		player::respawn();
	}
	if (f == 100) {
		out = fopen("fat:/gtasads_flickertest.txt", "w");
		player::debugSetView(1.5708f, 0.15f);
		input::debugHold(KEY_UP);
	}
	if (f == 800) input::debugHold(0);
#ifdef GTASADS_GROVESHOT
	if (f == 100) {
		input::debugHold(0);
		player::teleport(2495.0f, -1664.0f, 12.35f, 1.5708f);
		player::debugSetView(1.5708f, 0.15f);
	}
	if (f == 180) video::requestCapture("fat:/gtasads_grove_west.dscr");
	if (f == 210) {
		player::teleport(2495.0f, -1664.0f, 12.35f, -1.5708f);
		player::debugSetView(-1.5708f, 0.15f);
	}
	if (f > 210 && f <= 300) player::debugSetView(-1.5708f, 0.15f);
	if (f == 290) video::requestCapture("fat:/gtasads_grove_east.dscr");
	if (f == 300 && out) fflush(out);
#endif
	if (out) {
		const world::Stats& s = world::stats();
		fprintf(out, "%d pos=%.1f,%.1f,%.1f sec=%u inst=%u cand=%u drawn=%u budget=%u pops=%lu ready=%u loading=%u lists=%luK free=%luK largest=%luK loads=%lu reloads=%lu evict=%lu polys=%u>%u upd=%u sector=%lu mdlread=%lu parse=%lu texread=%lu compile=%lu start=%lu upload=%lu fail=%u:%u:%lu veh=%u:%u:%u:%u peds=%u:%u trafficTry=%lu:%u:%lu:%lu:%lu\n",
		        f, player::g.x, player::g.y, player::g.z, s.sectors, s.instances, s.candidates, s.drawn, s.culledBudget,
		        (unsigned long)s.budgetPops,
		        s.modelsReady, s.modelsLoading, (unsigned long)(s.listBytes >> 10),
		        (unsigned long)((mem::stats(mem::REGION_STREAM).capacity - mem::stats(mem::REGION_STREAM).used) >> 10),
		        (unsigned long)(mem::stats(mem::REGION_STREAM).largestFree >> 10),
		        (unsigned long)s.loads, (unsigned long)s.reloads, (unsigned long)s.evictions,
		        s.polysBefore, s.polysAfter, s.updateUs, (unsigned long)s.sectorUs,
		        (unsigned long)s.modelReadUs, (unsigned long)s.modelParseUs,
		        (unsigned long)s.texReadUs, (unsigned long)s.compileUs,
		        (unsigned long)s.startUs, (unsigned long)s.uploadUs,
		        s.compileFailModel, s.compileFailReason, (unsigned long)s.compileFailBytes,
		        veh::stats().active, veh::stats().traffic, veh::stats().spawned, veh::stats().drawn,
		        peds::stats().live, peds::stats().drawn,
		        (unsigned long)veh::stats().trafficAttempts, veh::stats().trafficCandidates,
		        (unsigned long)veh::stats().trafficSpots, (unsigned long)veh::stats().trafficModelMiss,
		        (unsigned long)veh::stats().trafficSpawnFail);
		if (f == 850) {
			fclose(out);
			out = nullptr;
			video::requestCapture("fat:/gtasads_flickershot.dscr");
		}
	}
}
#endif

#if defined(GTASADS_DEBUG) && defined(GTASADS_PEDTEST)
// Headless ped test (after the screenshot sequence): a ped placed 1 m in
// front of CJ takes a punch combo (hit / knock-down / response), then CJ gets
// a pistol and shoots a second one (death, the others flee). One line per 20
// frames to gtasads_pedtest.txt (the nearest peds' task / health), shot6..8.
static void pedTest(int f) {
	static FILE* out;
	static int a = -1, b = -1;
	auto log = [&](const char* what) {
		if (!out) return;
		const peds::Stats& ps = peds::stats();
		fprintf(out, "%4d %-6s cj=%.2f,%.2f hp=%d anim=%s $%d wanted=%d live=%u drawn=%u upd=%lu draw=%lu fps=%lu |", f, what,
		        player::g.x, player::g.y, (int)player::g.health, cj::animName(), player::g.money, player::g.wantedLevel, ps.live, ps.drawn,
		        (unsigned long)ps.updateUs, (unsigned long)ps.drawUs, timing::fps());
		for (int i = 0; i < peds::MAX; i++) {
			peds::Info in;
			if (!peds::info(i, in)) continue;
			const float dx = in.x - player::g.x, dy = in.y - player::g.y;
			fprintf(out, " #%d%s id%d %s hp%d %.1fm t%d", i, i == a ? "A" : i == b ? "B" : "", in.modelId,
			        peds::taskName(in.task), (int)in.health, sqrtf(dx * dx + dy * dy), in.target);
		}
		fputc('\n', out);
	};
	if (f == 90) {   // shot0 (frame 120, before any capture borrows VRAM D) shows a ped up close
		const int c = peds::spawnNear(player::g.x, player::g.y, player::g.z);
		peds::debugPlace(c, player::g.x + 0.6f, player::g.y + 2.2f, player::g.z, 3.1416f);
	}
	if (f == 600) {
		player::respawn();
		player::debugSetView(0.0f, 0.3f);
		out = fopen("fat:/gtasads_pedtest.txt", "w");
		a = peds::spawnNear(player::g.x, player::g.y, player::g.z);
		b = peds::spawnNear(player::g.x, player::g.y, player::g.z);
		log("spawn");
	}
	if (f == 610) {
		peds::debugPlace(a, player::g.x, player::g.y + 1.0f, player::g.z, 3.1416f);
		peds::debugPlace(b, player::g.x + 2.0f, player::g.y + 7.0f, player::g.z, 3.1416f);
	}
	if (f > 600 && f < 1200 && (f % 20 == 0 || (f > 640 && f < 900 && f % 5 == 0))) log(f < 760 ? "punch" : f < 900 ? "after" : "shoot");
	if (f == 640) video::requestCapture("fat:/gtasads_shot6.dscr");
	for (int k = 0; k < 3; k++) {
		if (f == 650 + k * 22) input::debugHold(KEY_A);
		if (f == 653 + k * 22) input::debugHold(0);
	}
	if (f == 700) video::requestCapture("fat:/gtasads_shot7.dscr");
	if (f == 900) {
		player::giveWeapon(player::WEAPON_PISTOL, 50);
		peds::Info in;
		if (peds::info(b, in)) player::debugSetView(atan2f(-(in.x - player::g.x), in.y - player::g.y), 0.3f);
	}
	if (f >= 930 && f < 1060 && f % 16 == 0) input::debugHold(KEY_R | KEY_A);
	if (f >= 930 && f < 1060 && f % 16 == 4) input::debugHold(KEY_R);
	if (f == 1060) input::debugHold(0);
	if (f == 1000) video::requestCapture("fat:/gtasads_shot8.dscr");
	// escape: a ped goes for CJ, CJ sprints away; it must lose him and walk on
	if (f == 1200) {
		player::respawn();
		peds::removeAll();
	}
	if (f >= 1210 && f < 1260 && a < 0 && f % 10 == 0) a = peds::spawnNear(player::g.x, player::g.y, player::g.z);
	if (f == 1200) a = -1;
	if (f == 1260 && a >= 0) {
		// east of CJ; he sprints west (open street, the mission house is south)
		peds::debugPlace(a, player::g.x + 3.0f, player::g.y, player::g.z, 1.5708f);
		peds::debugAttackPlayer(a);
		player::debugSetView(1.5708f, 0.3f);
	}
	if (f == 1290) input::debugHold(KEY_UP | KEY_B);
	if (f == 1650) input::debugHold(0);
	if (f > 1200 && f < 2100 && f % 20 == 0) log(f < 1290 ? "chase" : f < 1650 ? "flee" : "lost");
	if (f == 2100 && out) {
		log("end");
		fclose(out);
		out = nullptr;
	}
}
#endif

void draw() {
#if defined(GTASADS_DEBUG) && defined(GTASADS_MENUAUDTEST)
	static int s_testFrames;   // counts menu frames too (the in-game hooks stop in the menu)
	if (s_gameActive || FrontEndMenuManager.m_bMenuActive) menuAudTest(++s_testFrames);
#endif
	if (s_screen == Screen::GAME) {
		static bool s_menuLast;   // the backbuffer holds the menu's bottom screen
		if (FrontEndMenuManager.m_bMenuActive) {
			if (!s_menuLast) FrontEndMenuManager.invalidateBottom();
			s_menuLast = true;
			FrontEndMenuManager.drawBottom();
			return;
		}
		s_menuLast = false;
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
#ifdef GTASADS_RADIOTEST
			radioTest(s_gameFrames);
#endif
#ifdef GTASADS_VEHSPAWNTEST
			vehSpawnTest(s_gameFrames);
#endif
#ifdef GTASADS_SKIDTEST
			skidTest(s_gameFrames);
#endif
#ifdef GTASADS_WORLDTEST
			worldTest(s_gameFrames);
#endif
#ifdef GTASADS_FLICKERTEST
			flickerTest(s_gameFrames);
#endif
#ifdef GTASADS_PEDTEST
			pedTest(s_gameFrames);
#endif
			// headless screenshots (tools/dsshot.py): one per compass direction
			// (shot0 at the marker frame, then turn, give streaming 60 frames,
			// capture), a bird's eye view (shot4), then a teleport 150 m west to
			// exercise sector streaming + eviction (shot5, gtasads_hudok2.txt)
#if defined(GTASADS_SCREENSHOTTEST) || defined(GTASADS_COLTEST) || defined(GTASADS_VEHTEST) || \
    defined(GTASADS_PEDTEST) || defined(GTASADS_WORLDTEST)
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
#endif
			const world::Stats& ws = world::stats();
			auto worldLine = [&](FILE* f) {
				fprintf(f, "world: sec=%u inst=%u cand=%u drawn=%u cullF=%u cullB=%u ready=%u loading=%u "
				        "tex=%u fail=%u lists=%luK texvram=%luK loads=%lu reloads=%lu evict=%lu pops=%lu up=%lu polys=%u>%u upd=%uus draw=%uus scan=%uus\n",
				        ws.sectors, ws.instances, ws.candidates, ws.drawn, ws.culledFrustum, ws.culledBudget,
				        ws.modelsReady, ws.modelsLoading, ws.textures, ws.texFailed,
				        (unsigned long)(ws.listBytes >> 10), (unsigned long)(ws.texVram >> 10),
				        (unsigned long)ws.loads, (unsigned long)ws.reloads, (unsigned long)ws.evictions,
				        (unsigned long)ws.budgetPops,
				        (unsigned long)ws.uploads, ws.polysBefore, ws.polysAfter,
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
				const veh::Stats& vs = veh::stats();
				fprintf(f, "veh: active=%u traffic=%u spawned=%u removed=%u drawn=%u step=%luus draw=%luus\n",
				        vs.active, vs.traffic, vs.spawned, vs.removed, vs.drawn,
				        (unsigned long)vs.stepUs, (unsigned long)vs.drawUs);
				const peds::Stats& ps = peds::stats();
				const pedmodel::Stats& pm = pedmodel::stats();
				fprintf(f, "peds: live=%u max=%u civ=%u gang=%u cops=%u drawn=%u drivers=%u posed=%u polys=%u spawned=%u removed=%u "
				        "zone=%u slot=%u models=%u/%u lists=%luK texvram=%luK upd=%luus(task %lu phys %lu) draw=%luus(pose %lu peak %lu avg %lu) wanted=%u\n",
				        ps.live, ps.maxPeds, ps.civ, ps.gang, ps.cops, ps.drawn, ps.drivers, ps.posed, ps.polys, ps.spawned, ps.removed,
				        ps.zoneType, ps.slot, pm.ready, pm.models, (unsigned long)(pm.listBytes >> 10),
				        (unsigned long)(pm.texVram >> 10), (unsigned long)ps.updateUs, (unsigned long)ps.taskUs, (unsigned long)ps.physUs,
				        (unsigned long)ps.drawUs, (unsigned long)ps.poseUs, (unsigned long)ps.posePeakUs,
				        (unsigned long)(ps.poseCount ? ps.poseTotalUs / ps.poseCount : 0), ps.wanted);
			};
#if defined(GTASADS_SCREENSHOTTEST) || defined(GTASADS_COLTEST) || defined(GTASADS_VEHTEST) || \
    defined(GTASADS_PEDTEST) || defined(GTASADS_WORLDTEST)
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
#endif
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
