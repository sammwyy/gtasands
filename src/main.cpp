#include <nds.h>
#include <stdlib.h>

#include "assets/pack.h"
#include "game/menu/menu.h"
#include "game/text/gxt.h"
#include "hud/hud.h"
#include "platform/fs.h"
#include "platform/input.h"
#include "platform/timing.h"
#include "platform/video.h"
#include "render/renderer.h"

enum class AppState { MENU, INGAME };

static AppState s_state = AppState::MENU;
static float yaw = 30, pitch = 25, dist = 22;

static void boot() {
	video::init();
	hud::init();
	timing::init();
	renderer::init();

	if (!fs::init()) {
		hud::log("\x1b[31mnitroFS init failed\x1b[39m");
		return;
	}
	if (!TheText.load("nitro:/text/american.gxt")) {
		hud::log("\x1b[31mamerican.gxt missing (make assets)\x1b[39m");
		return;
	}
	FrontEndMenuManager.initialise();
	hud::log("frontend up: %u screens", (unsigned)SCREEN_COUNT);
}

int main() {
	boot();

	while (pmMainLoop()) {
		timing::frameStart();
		input::poll();
		const input::State& in = input::get();

		if (FrontEndMenuManager.m_WantsQuit) {
			powerOff(POWER_ALL);
		}
		if (FrontEndMenuManager.m_WantsStartGame) {
			FrontEndMenuManager.m_WantsStartGame = false;
			s_state = AppState::INGAME;
		}
		if (s_state == AppState::INGAME) {
			if (in.down & KEY_START) {
				FrontEndMenuManager.m_bMenuActive = !FrontEndMenuManager.m_bMenuActive;
				if (FrontEndMenuManager.m_bMenuActive) {
					FrontEndMenuManager.m_bMainMenuSwitch = false;
					FrontEndMenuManager.m_nCurrentScreen = SCREEN_INITIAL;
					FrontEndMenuManager.m_nCurrentScreenItem = 0;
				}
			}
			if (in.down & KEY_B && FrontEndMenuManager.m_bMenuActive &&
			    FrontEndMenuManager.m_nCurrentScreen == SCREEN_PAUSE_MENU)
				FrontEndMenuManager.m_bMenuActive = false;
		}
		if (FrontEndMenuManager.m_bMenuActive) FrontEndMenuManager.process();

		video::beginFrame();
		if (FrontEndMenuManager.m_bMenuActive) {
			FrontEndMenuManager.drawFrontEnd();
		} else {
			if (in.held & KEY_LEFT) yaw -= 2;
			if (in.held & KEY_RIGHT) yaw += 2;
			if (in.held & KEY_UP && pitch < 85) pitch += 1;
			if (in.held & KEY_DOWN && pitch > 5) pitch -= 1;
			if (in.held & KEY_L && dist > 5) dist -= 0.5f;
			if (in.held & KEY_R && dist < 60) dist += 0.5f;
			renderer::draw(yaw, pitch, dist);
		}
		timing::frameEnd();
		hud::drawStats();
		video::endFrame();
	}
	return 0;
}
