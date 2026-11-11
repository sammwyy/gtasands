#include <nds.h>
#include <stdlib.h>

#include "audio/audioengine.h"
#include "game/collision.h"
#include "game/cutscene.h"
#include "game/entryexits.h"
#include "game/intro.h"
#include "game/loadscreen.h"
#include "game/missions.h"
#include "game/peddata.h"
#include "game/peds.h"
#include "game/player.h"
#include "game/sfx.h"
#include "game/timecycle.h"
#include "game/vehicle.h"
#include "game/vehiclemodel.h"
#include "game/weather.h"
#include "game/world.h"
#include "game/menu/menu.h"
#include "game/text/gxt.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/fs.h"
#include "platform/input.h"
#include "platform/timing.h"
#include "platform/video.h"
#include "render/renderer.h"

enum class AppState { TITLE, MENU, LOADING, INGAME };

static AppState s_state = AppState::TITLE;

// Pause menu closed in-game: its art goes back out of VRAM (and the menu
// reloads it lazily next time), so the world keeps the whole texture budget.
static void closePauseMenu() {
	FrontEndMenuManager.m_bMenuActive = false;
	FrontEndMenuManager.unloadTextures();
	FrontEndMenuManager.m_bTexturesLoaded = false;
	hud::setGameActive(true);
}

static void boot() {
#ifdef GTASADS_DSI
	if (isDSiMode()) setCpuClock(true);
#endif
	mem::init();   // carve the RAM regions before anything allocates
	video::init();
	hud::init();
	timing::init();
	renderer::init();
#ifdef GTASADS_DSI
	if (!isDSiMode()) {
		hud::log("DSi mode required (16 MB RAM)");
		return;
	}
#endif
	hud::log("RAM heap: %u KB", (unsigned)(mem::totalBytes() >> 10));

	if (!fs::init()) {
		hud::log("\x1b[31mfs init failed (no nitrofs, no SD)\x1b[39m");
		return;
	}
	hud::log("fs: %s", fs::backend());
#ifndef GTASADS_QUICKTEST
	// boot sequence: rockstar logo + SA intro cinematic (skippable)
	intro::play("game:/movies/logo.dvid");
	intro::play("game:/movies/gtatitles.dvid");
#endif
	if (!TheText.load("game:/text/american.gxt")) {
		hud::log("\x1b[31mamerican.gxt missing (make assets)\x1b[39m");
		return;
	}
	hud::log("gxt ok: %.12s", (const char*)TheText.get("FEP_STG"));
	sfx::init(); // before the title screen: it starts the loading tune
	loadscreen::beginTitle();
}

int main() {
	defaultExceptionHandler();
	boot();
#ifdef GTASADS_QUICKTEST
	FrontEndMenuManager.m_WantsStartGame = true;
	hud::setScreen(hud::Screen::GAME);
#endif

	while (true) {
		timing::frameStart();
		input::poll();
		const input::State& in = input::get();

		if (FrontEndMenuManager.m_WantsQuit) {
			powerOff(POWER_ALL);
		}
		if (FrontEndMenuManager.m_WantsStartGame) {
			FrontEndMenuManager.m_WantsStartGame = false;
			hud::setGameActive(false);
			audio::setGameActive(false);   // the weather bed / ambience stop while the new game loads
			loadscreen::begin();
			s_state = AppState::LOADING;
		}
		// boot title -> frontend -> loading -> ingame transitions
		if (s_state == AppState::TITLE && loadscreen::update()) {
			loadscreen::end();
			FrontEndMenuManager.initialise(); // menu art loads once the title frees VRAM
			hud::log("frontend up: %u screens", (unsigned)SCREEN_COUNT);
			s_state = AppState::MENU;
		}
		if (s_state == AppState::LOADING && loadscreen::update()) {
			loadscreen::end();   // the loading jobs already set up CJ, the HUD and player state
			hud::setGameActive(true);
			audio::setGameActive(true);
			s_state = AppState::INGAME;
			if (missions::newGame()) cutscene::startOriginal();
		}
		// SELECT toggles the bottom screen: debug UI <-> real game HUD (any state)
		if ((in.down & KEY_SELECT) && !FrontEndMenuManager.m_bMenuActive && !cutscene::active()) hud::toggleScreen();
		if (s_state == AppState::INGAME) {
			if ((in.down & KEY_START) && !entryexits::busy() && !cutscene::active()) {
				sfx::start(); // AE_FRONTEND_START: whoosh on menu open/close
				if (FrontEndMenuManager.m_bMenuActive) {
					closePauseMenu();
				} else {
					world::suspend();   // the menu art needs the VRAM; the map streams back in after
					FrontEndMenuManager.m_bMenuActive = true;
					hud::setScreen(hud::Screen::GAME);
					FrontEndMenuManager.m_bMainMenuSwitch = false;
					FrontEndMenuManager.ensureTextures();
					FrontEndMenuManager.m_bDontDrawFrontEnd = false;
					FrontEndMenuManager.m_nCurrentScreen = SCREEN_INITIAL;
					FrontEndMenuManager.m_nCurrentScreenItem = 0;
				}
			}
			if (in.down & KEY_B && FrontEndMenuManager.m_bMenuActive &&
			    FrontEndMenuManager.m_nCurrentScreen == SCREEN_PAUSE_MENU) {
				FrontEndMenuManager.m_bDontDrawFrontEnd = false;
				closePauseMenu();
			}
		}
		if (FrontEndMenuManager.m_bMenuActive) FrontEndMenuManager.process();
		if (s_state == AppState::INGAME && !FrontEndMenuManager.m_bMenuActive &&
		    FrontEndMenuManager.m_bTexturesLoaded) closePauseMenu();

		video::beginFrame();
		u32 t = timing::ticks();
		if (FrontEndMenuManager.m_bMenuActive) {
			FrontEndMenuManager.drawFrontEnd();
		} else if (s_state == AppState::TITLE || s_state == AppState::LOADING) {
			loadscreen::draw();
		} else {
			renderer::draw();
			entryexits::drawFade();
			missions::drawFade();
			cutscene::drawOverlay();
		}
		timing::setSection(timing::SEC_3D, timing::usSince(t));
		t = timing::ticks();
		if (s_state == AppState::INGAME && !FrontEndMenuManager.m_bMenuActive) {
			const bool playing = entryexits::update();
			static u32 s_pedVbl;
			if (playing) {
				const bool scene = cutscene::active();
				if (scene) {
#if defined(GTASADS_DEBUG) && defined(GTASADS_CUTSKIPTEST)
					static int sceneFrames;
					cutscene::update(++sceneFrames == 40 ? (u32)KEY_B : in.down);
#else
					cutscene::update(in.down);
#endif
				}
				if (cutscene::takeNextOriginal()) {
					hud::setGameActive(false);
					audio::setGameActive(false);
					loadscreen::beginPrologueTransition();
					s_state = AppState::LOADING;
				}
				if (!scene) peddata::update(player::g.x, player::g.y);
				col::update(player::g.x, player::g.y, player::g.z);        // collision near list + streaming
				if (!scene && !missions::busy()) player::update();
				if (!scene) missions::update();
				weather::update(player::g.x, player::g.y, player::g.z);     // CWeather::Update
				timecycle::update(player::g.x, player::g.y, player::g.z);   // CTimeCycle::Update
				world::update(player::g.x, player::g.y);                    // sectors + model streaming
				if (!scene && world::area() == 0) veh::update();             // vehicle models, timers, audio
				const u32 vb = timing::vblanks();
				if (!scene && world::area() == 0) peds::update(s_pedVbl ? (int)(vb - s_pedVbl) : 1); // CPopulation + ped tasks
				s_pedVbl = vb;
			} else s_pedVbl = timing::vblanks();
		}
		audio::service();   // CAudioEngine::Service: entities, bank reads, channels, ambience stream
		timing::setSection(timing::SEC_SIM, timing::usSince(t));
		// bottom screen renders into a RAM backbuffer (safe mid-frame), then
		// a ~0.6ms DMA pushes it to VRAM C inside vblank -> no tearing
		t = timing::ticks();
		hud::draw();
		timing::setSection(timing::SEC_HUD, timing::usSince(t));
		timing::frameEnd();   // cpu = everything the frame did (3D + sim + HUD)
		video::endFrame();
		world::vblank();      // texture uploads while the 3D engine is idle
		vehmdl::vblank();
		peds::vblank();       // ped model textures
		hud::present();
	}
	return 0;
}
