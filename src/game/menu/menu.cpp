#include "game/menu/menu.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include "assets/pack.h"
#include "assets/texture.h"
#include "game/text/font.h"
#include "game/text/gxt.h"
#include "platform/fs.h"
#include "platform/input.h"
#include "hud/hud.h"
#include "platform/timing.h"
#include "render/sprite2d.h"

CMenuManager FrontEndMenuManager;

// SA menu colours (MenuManager.h / HudColours)
static constexpr sprite::RGBA MENU_BG{ 0, 0, 0, 255 };
static constexpr sprite::RGBA MENU_TEXT_NORMAL{ 74, 90, 107, 255 };
static constexpr sprite::RGBA MENU_TEXT_SELECTED{ 172, 203, 241, 255 };
static constexpr sprite::RGBA MENU_SHADOW{ 0, 0, 0, 255 };
static constexpr sprite::RGBA TITLE_COLOR{ 172, 203, 241, 255 };
static constexpr sprite::RGBA DISABLED{ 14, 30, 47, 255 };

// Menu coords are virtual 640x480 (same numbers as the PC frontend); the ortho
// projection in sprite::beginFrame compresses them to the 256x192 DS screen.
static inline float StretchX(float x) { return x; }
static inline float StretchY(float y) { return y; }
static inline float ScreenR(float x) { return 640.0f - x; }

static Pack s_fe1, s_fe2, s_fe3, s_fepc;
static const tex::Texture* s_sprites[FRONTEND_SPRITE_COUNT];
static const tex::Texture* s_font2 = nullptr;
static const tex::Texture* s_font1 = nullptr;

static void* loadBlob(const char* name) {
	// loose DTXD files at nitro:/models/<name>.txd (converted by the pipeline)
	char path[96];
	snprintf(path, sizeof path, "nitro:/models/%s.txd", name);
	FILE* f = fopen(path, "rb");
	if (!f) return nullptr;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* b = malloc(n);
	if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = nullptr; }
	fclose(f);
	return b;
}

static const tex::Texture* spriteTex(eFrontend id) { return s_sprites[(int)id]; }

static const tex::Texture* loadSpriteFile(const char* file, int first, int last) {
	void* dtxd = loadBlob(file);
	if (!dtxd) return nullptr;
	const tex::Texture* firstTex = nullptr;
	for (int i = first; i < last; i++) {
		s_sprites[i] = tex::loadFromTxd(dtxd, FrontEndFilenames[i].name);
		if (!firstTex) firstTex = s_sprites[i];
	}
	return firstTex;
}

void CMenuManager::initialise() {
	m_nCurrentScreen = SCREEN_INITIAL;
	m_nCurrentScreenItem = 0;
	m_bMenuActive = true;
	m_bMainMenuSwitch = true;
	for (auto& s : m_Slots) s = eSlotState::SLOT_FREE;
	loadAllTextures();
	font::init();
	font::attachFontTextures(s_font2, s_font1);
}

void CMenuManager::loadAllTextures() {
	// Only the always-needed sprites up front; per-screen art loads lazily
	// on SwitchToNewScreen so VRAM is never exhausted.
	loadSpriteFile("fronten1", FRONTEND_SPRITE_ARROW, FRONTEND_SPRITE_ARROW + 1);
	loadSpriteFile("fronten2", FRONTEND_SPRITE_BACK8, FRONTEND_SPRITE_MAP + 1);
	loadSpriteFile("fronten3", FRONTEND3_START, FRONTEND4_START);
	loadSpriteFile("fronten_pc", FRONTEND4_START, FRONTEND_SPRITE_COUNT);
	void* fd = loadBlob("fonts");
	if (fd) {
		s_font2 = tex::loadFromTxd(fd, "font2");
		s_font1 = tex::loadFromTxd(fd, "font1");
	}
	m_bTexturesLoaded = s_font1 && s_font2;
	int f1 = s_font1 ? s_font1->id : 0, f2 = s_font2 ? s_font2->id : 0;
	hud::log("fe tex: %d loaded, font1=%d font2=%d", tex::loadedCount(), f1, f2);
}

void CMenuManager::unloadTextures() { tex::clear(); }

void CMenuManager::process() {
	if (m_bMenuActive) userInput();
}

uint32_t CMenuManager::getNumberOfMenuOptions() {
	if (m_nCurrentScreen == SCREEN_MAP || m_nCurrentScreen == SCREEN_BRIEF) return 2;
	uint8_t r = 0;
	for (auto& item : aScreens[m_nCurrentScreen].m_aItems) {
		if (!item.m_nActionType) break;
		if (item.m_nActionType != MENU_ACTION_TEXT && item.m_nActionType != MENU_ACTION_SKIP) r++;
	}
	return r;
}

void CMenuManager::switchToNewScreen(eMenuScreen screen) {
	m_nPrevScreen = m_nCurrentScreen;
	if (screen == SCREEN_GO_BACK) {
		if (m_bMainMenuSwitch) {
			switch (m_nCurrentScreen) {
			case SCREEN_START_GAME: m_nCurrentScreenItem = 0; break;
			case SCREEN_OPTIONS: m_nCurrentScreenItem = 1; break;
			case SCREEN_QUIT_GAME_ASK: m_nCurrentScreenItem = 2; break;
			default: m_nCurrentScreenItem = aScreens[m_nCurrentScreen].m_nStartEntry; break;
			}
		} else {
			m_nCurrentScreenItem = aScreens[m_nCurrentScreen].m_nStartEntry;
		}
		m_nCurrentScreen = aScreens[m_nCurrentScreen].m_nParentMenu;
	} else {
		if (screen) {
			m_nCurrentScreenItem = 0;
			m_nCurrentScreen = screen;
		} else {
			m_nCurrentScreenItem = 0;
			m_nCurrentScreen = aScreens[m_nCurrentScreen].m_aItems[m_nCurrentScreenItem].m_nTargetMenu;
		}
	}
	if (m_bMainMenuSwitch) {
		switch (m_nCurrentScreen) {
		case SCREEN_NEW_GAME_ASK:
			startNewGame();
			break;
		case SCREEN_LOAD_GAME_ASK:
			m_nCurrentScreen = SCREEN_LOAD_FIRST_SAVE;
			break;
		default: break;
		}
	}
}

void CMenuManager::scrollRadioStations(int8_t n) {
	static const char* stationGxt[] = {
		"AA", "ADVERTS", "AMBIENCE", "BEATS", "CH", "CO", "CR", "CUTSCENE", "DS", "HC",
		"MH", "MR", "NJ", "RE", "RG", "TK", "OFF"
	};
	m_nRadioStation += n;
	if (m_nRadioStation <= 0) m_nRadioStation = 16;
	if (m_nRadioStation > 16) m_nRadioStation = 1;
	(void)stationGxt;
}

void CMenuManager::setLanguage(eLanguage l) {
	m_nPrefsLanguage = l;
	TheText.load(l == eLanguage::SPANISH ? "nitro:/text/spanish.gxt" : "nitro:/text/american.gxt");
}

void CMenuManager::startNewGame() {
	m_WantsStartGame = true;
	m_bDontDrawFrontEnd = true;
	m_bMenuActive = false;
}

void CMenuManager::userInput() {
	static constexpr auto mainMenu = { SCREEN_MAIN_MENU, SCREEN_PAUSE_MENU, SCREEN_GAME_SAVE,
		SCREEN_GAME_WARNING_DONT_SAVE, SCREEN_SAVE_DONE_1, SCREEN_DELETE_FINISHED, SCREEN_EMPTY };
	static constexpr auto sliderActions = { MENU_ACTION_BRIGHTNESS, MENU_ACTION_RADIO_VOL, MENU_ACTION_SFX_VOL,
		MENU_ACTION_DRAW_DIST, MENU_ACTION_MOUSE_SENS };
	auto contains = [](const auto& v, auto x) {
		for (auto e : v) if (e == x) return true;
		return false;
	};
	const input::State& in = input::get();
	bool bEnter = false, bExit = false, bUp = false, bDown = false;
	int8_t sliderMove = 0;

	if (getNumberOfMenuOptions() > 1) {
		if (in.down & KEY_DOWN) bDown = true;
		else if (in.down & KEY_UP) bUp = true;
	}
	if (in.down & (KEY_A | KEY_START)) bEnter = true;
	if ((in.down & (KEY_B | KEY_TOUCH)) && !contains(mainMenu, (eMenuScreen)m_nCurrentScreen)) bExit = true;
	if (bDown || bUp || bEnter) bExit = false;

	static uint32_t lastMove;
	uint32_t now = timing::vblanks();
	int dir = (in.held & KEY_LEFT) ? -1 : (in.held & KEY_RIGHT) ? 1 : 0;
	if (dir && (now - lastMove > 14 || in.down & (KEY_LEFT | KEY_RIGHT))) {
		auto act = aScreens[m_nCurrentScreen].m_aItems[m_nCurrentScreenItem].m_nActionType;
		if (contains(sliderActions, act) || act == MENU_ACTION_STAT ||
		    aScreens[m_nCurrentScreen].m_aItems[m_nCurrentScreenItem].m_nType == TI_OPTION) {
			sliderMove = (int8_t)dir;
			lastMove = now;
		}
	}
	processUserInput(bDown, bUp, bEnter, bExit, sliderMove);
}

void CMenuManager::processUserInput(bool down, bool up, bool enter, bool back, int8_t lr) {
	if (m_nCurrentScreen == SCREEN_EMPTY) return;
	auto& scr = aScreens[m_nCurrentScreen];

	if (getNumberOfMenuOptions() > 1 && down) {
		m_nCurrentScreenItem++;
		for (; scr.m_aItems[m_nCurrentScreenItem].m_nActionType == MENU_ACTION_SKIP; m_nCurrentScreenItem++);
		if (m_nCurrentScreenItem >= TI_OPTION || !scr.m_aItems[m_nCurrentScreenItem].m_nActionType)
			m_nCurrentScreenItem = scr.m_aItems[0].m_nActionType == MENU_ACTION_TEXT ? 1 : 0;
	}
	if (getNumberOfMenuOptions() > 1 && up) {
		auto firstSpecial = scr.m_aItems[0].m_nActionType == 1;
		if (m_nCurrentScreenItem <= (firstSpecial ? 1 : 0)) {
			for (; m_nCurrentScreenItem < TI_ENTER && scr.m_aItems[m_nCurrentScreenItem + 1].m_nActionType; m_nCurrentScreenItem++);
			for (; scr.m_aItems[m_nCurrentScreenItem].m_nActionType == MENU_ACTION_SKIP; m_nCurrentScreenItem--);
		} else {
			m_nCurrentScreenItem--;
			for (; scr.m_aItems[m_nCurrentScreenItem].m_nActionType == MENU_ACTION_SKIP; m_nCurrentScreenItem--);
		}
	}
	if (enter) {
		processMenuOptions(0, back, enter);
	}
	if (lr && scr.m_aItems[m_nCurrentScreenItem].m_nType == TI_OPTION) {
		processMenuOptions(lr, back, 0);
		checkSliderMovement(lr);
	}
	if (back && m_RadioAvailable) switchToNewScreen(SCREEN_GO_BACK);
}

void CMenuManager::checkSliderMovement(int8_t lr) {
	auto& item = aScreens[m_nCurrentScreen].m_aItems[m_nCurrentScreenItem];
	switch (item.m_nActionType) {
	case MENU_ACTION_BRIGHTNESS:
		m_PrefsBrightness = std::clamp<int32_t>(lr * (363 / 15) + m_PrefsBrightness, 0, 384);
		break;
	case MENU_ACTION_RADIO_VOL:
		m_nRadioVolume = std::clamp<int8_t>(4 * lr + m_nRadioVolume, 0, 64);
		break;
	case MENU_ACTION_SFX_VOL:
		m_nSfxVolume = std::clamp<int8_t>(4 * lr + m_nSfxVolume, 0, 64);
		break;
	case MENU_ACTION_DRAW_DIST:
		m_fDrawDistance = std::clamp(m_fDrawDistance + (7.0f / 128.0f) * lr, 0.925f, 1.8f);
		break;
	case MENU_ACTION_MOUSE_SENS:
		m_fMouseSens = std::clamp(m_fMouseSens + (lr * (1.0f / 200.0f)) / 15.0f, 1.0f / 3200.0f, 1.0f / 200.0f);
		break;
	default: break;
	}
}

void CMenuManager::processMenuOptions(int8_t lr, bool& cancel, bool accept) {
	(void)accept;
	tMenuScreen* screen = &aScreens[m_nCurrentScreen];
	tMenuScreenItem* item = &screen->m_aItems[m_nCurrentScreenItem];

	switch (item->m_nActionType) {
	case MENU_ACTION_BACK: cancel = true; return;
	case MENU_ACTION_YES:
	case MENU_ACTION_NO:
	case MENU_ACTION_MENU: switchToNewScreen(item->m_nTargetMenu); return;
	case MENU_ACTION_NEW_GAME: switchToNewScreen(SCREEN_SELECT_GAME); return;
	case MENU_ACTION_STANDARD_GAME:
		startNewGame();
		return;
	case MENU_ACTION_15:
		m_bDontDrawFrontEnd = true;
		m_bMenuActive = false;
		return;
	case MENU_ACTION_SAVE_SLOT:
	case MENU_ACTION_SAVE_GAME:
		// all slots are free on DS for now; nothing happens (AUDIO error in OG)
		m_SelectedSlot = m_nCurrentScreenItem - 1;
		return;
	case MENU_ACTION_QUIT: m_WantsQuit = true; return;
	case MENU_ACTION_MENU_CLOSE:
	case MENU_ACTION_PAUSE:
		m_bMenuActive = false;
		return;
	case MENU_ACTION_SUBTITLES: m_bShowSubtitles ^= true; return;
	case MENU_ACTION_WIDESCREEN: m_bWidescreenOn ^= true; return;
	case MENU_ACTION_FRAME_LIMITER: m_bPrefsFrameLimiter ^= true; return;
	case MENU_ACTION_RADIO_EQ: m_bRadioEq ^= true; return;
	case MENU_ACTION_RADIO_RETUNE: m_bRadioAutoSelect ^= true; return;
	case MENU_ACTION_RADIO_STATION: scrollRadioStations(lr); return;
	case MENU_ACTION_SHOW_LEGEND: m_bMapLegend ^= true; return;
	case MENU_ACTION_RADAR_MODE:
		m_nRadarMode = (eRadarMode)(((int)m_nRadarMode + (int)lr + (int)eRadarMode::RADAR_MODE_COUNT) % (int)eRadarMode::RADAR_MODE_COUNT);
		return;
	case MENU_ACTION_HUD_MODE: m_bHudOn = !m_bHudOn; return;
	case MENU_ACTION_INVERT_PAD: return;
	case MENU_ACTION_23: return;
	case MENU_ACTION_MIP_MAPPING: m_bPrefsMipMapping ^= true; return;
	case MENU_ACTION_ANTIALIASING:
		m_nPrefsAntialiasing += lr;
		if (m_nPrefsAntialiasing < 1) m_nPrefsAntialiasing = 1;
		if (m_nPrefsAntialiasing > 4) m_nPrefsAntialiasing = 4;
		m_nDisplayAntialiasing = m_nPrefsAntialiasing;
		return;
	case MENU_ACTION_RESOLUTION: m_nPrefsVideoMode ^= lr ? 1 : 0; return;
	case MENU_ACTION_FX_QUALITY:
		m_nPrefsFxQuality += lr;
		if (m_nPrefsFxQuality < 0) m_nPrefsFxQuality = 0;
		if (m_nPrefsFxQuality > 3) m_nPrefsFxQuality = 3;
		return;
	case MENU_ACTION_CONTROL_TYPE: m_ControlMethod = eController((int(m_ControlMethod) + 1) % 2); return;
	case MENU_ACTION_CTRLS_JOYPAD: m_ControlMethod = eController((int(m_ControlMethod) + 1) % 2); return;
	case MENU_ACTION_MOUSE_STEERING: m_bEnableMouseSteering ^= true; return;
	case MENU_ACTION_MOUSE_FLY: m_bEnableMouseFlying ^= true; return;
	case MENU_ACTION_CONTROLS_MOUSE_INVERT_Y: m_bInvertMouseY ^= true; return;
	case MENU_ACTION_CONTROLS_JOY_INVERT_X: m_bInvertPadX1 ^= true; return;
	case MENU_ACTION_CONTROLS_JOY_INVERT_Y: m_bInvertPadY1 ^= true; return;
	case MENU_ACTION_CONTROLS_JOY_INVERT_X2: m_bInvertPadX2 ^= true; return;
	case MENU_ACTION_CONTROLS_JOY_INVERT_Y2: m_bInvertPadY2 ^= true; return;
	case MENU_ACTION_CONTROLS_JOY_SWAP_AXIS1: m_bSwapPadAxis1 ^= true; return;
	case MENU_ACTION_CONTROLS_JOY_SWAP_AXIS2: m_bSwapPadAxis2 ^= true; return;
	case MENU_ACTION_USER_TRACKS_PLAY_MODE: m_RadioMode = (m_RadioMode + lr + 3) % 3; return;
	case MENU_ACTION_USER_TRACKS_AUTO_SCAN: m_bTracksAutoScan ^= true; return;
	case MENU_ACTION_STORE_PHOTOS: m_bSavePhotos ^= true; return;
	case MENU_ACTION_LANG_ENGLISH: setLanguage(eLanguage::AMERICAN); return;
	case MENU_ACTION_LANG_SPANISH: setLanguage(eLanguage::SPANISH); return;
	case MENU_ACTION_LANGUAGE: setLanguage(m_nPrefsLanguage == eLanguage::AMERICAN ? eLanguage::SPANISH : eLanguage::AMERICAN); return;
	case MENU_ACTION_RESET_CFG:
		// per-screen defaults
		return;
	default: return;
	}
}

void CMenuManager::drawFrontEnd() {
	if (m_bDontDrawFrontEnd) return;

	if (m_nCurrentScreen == SCREEN_INITIAL)
		m_nCurrentScreen = m_bMainMenuSwitch ? SCREEN_MAIN_MENU : SCREEN_PAUSE_MENU;
	if (m_nCurrentScreenItem == 0 && aScreens[m_nCurrentScreen].m_aItems[0].m_nActionType == MENU_ACTION_TEXT)
		m_nCurrentScreenItem = 1;

	sprite::beginFrame();
	drawBackground();
	sprite::endFrame();
}

void CMenuManager::drawBackground() {
	if (!m_bTexturesLoaded) return;

	int8_t bg;
	switch (m_nCurrentScreen) {
	case SCREEN_STATS:
	case SCREEN_LANGUAGE:
	case SCREEN_QUIT_GAME_ASK: bg = FRONTEND_SPRITE_BACK4; break;
	case SCREEN_START_GAME:
	case SCREEN_USER_TRACKS_OPTIONS:
	case SCREEN_OPTIONS: bg = FRONTEND_SPRITE_BACK3; break;
	case SCREEN_BRIEF:
	case SCREEN_LOAD_GAME:
	case SCREEN_DELETE_GAME:
	case SCREEN_GAME_SAVE: bg = FRONTEND_SPRITE_BACK2; break;
	case SCREEN_AUDIO_SETTINGS: bg = FRONTEND_SPRITE_BACK5; break;
	case SCREEN_DISPLAY_SETTINGS:
	case SCREEN_DISPLAY_ADVANCED: bg = FRONTEND_SPRITE_BACK7; break;
	case SCREEN_MAP:
	case SCREEN_CONTROLS_DEFINITION: bg = FRONTEND_SPRITE_ARROW; break;
	case SCREEN_CONTROLLER_SETUP:
	case SCREEN_MOUSE_SETTINGS: bg = FRONTEND_SPRITE_BACK6; break;
	default: bg = FRONTEND_SPRITE_BACK8; break;
	}

	sprite::rect({ 0, 0, 640, 480 }, MENU_BG);
	// on PC the back sprites only show on save dialogs; keep it faithful but
	// also draw the panorama strip on the title screen (console-style)
	if (m_nCurrentScreen == SCREEN_MAIN_MENU && spriteTex(FRONTEND_SPRITE_BACK8)) {
		sprite::draw(spriteTex(FRONTEND_SPRITE_BACK8), { 0, 300, 640, 480 }, { 255, 255, 255, 255 });
		if (spriteTex(FRONTEND_SPRITE_BACK8_TOP))
			sprite::draw(spriteTex(FRONTEND_SPRITE_BACK8_TOP), { 0, 268, 640, 300 }, { 255, 255, 255, 255 });
	}
	if (m_nCurrentScreen == SCREEN_MAP) {
		drawMap();
		return;
	}
	switch (m_nCurrentScreen) {
	case SCREEN_LOAD_FIRST_SAVE:
	case SCREEN_SAVE_DONE_1:
	case SCREEN_DELETE_FINISHED:
	case SCREEN_DELETE_SUCCESSFUL:
	case SCREEN_SAVE_WRITE_ASK:
		if (spriteTex((eFrontend)bg))
			sprite::draw(spriteTex((eFrontend)bg), { 384, 0, 640, 256 }, { 255, 255, 255, 255 });
		break;
	case SCREEN_GAME_SAVE:
		if (spriteTex((eFrontend)bg))
			sprite::draw(spriteTex((eFrontend)bg), { 192, 0, 448, 256 }, { 255, 255, 255, 255 });
		break;
	default: break;
	}

	drawStandardMenus(true);
}

void CMenuManager::drawMap() {
	// full map image like the PC frontend (map sprite = whole SA map)
	if (spriteTex(FRONTEND_SPRITE_MAP))
		sprite::draw(spriteTex(FRONTEND_SPRITE_MAP), { 60, 24, 580, 384 }, { 255, 255, 255, 255 });
	font::setFontStyle(FONT_GOTHIC);
	font::setScale(StretchX(1.3f), StretchY(2.1f));
	font::setEdge(1);
	font::setColor(TITLE_COLOR.r, TITLE_COLOR.g, TITLE_COLOR.b, TITLE_COLOR.a);
	font::setDropColor(0, 0, 0, 255);
	font::setOrientation(ALIGN_LEFT);
	font::printString(StretchX(40.0f), StretchY(28.0f), TheText.get(aScreens[m_nCurrentScreen].m_szTitleName));
	font::setFontStyle(FONT_MENU);
	font::setEdge(1);
	font::setScale(StretchX(0.7f), StretchY(1.0f));
	font::setColor(m_nCurrentScreenItem == 0 ? MENU_TEXT_SELECTED.r : MENU_TEXT_NORMAL.r,
	               m_nCurrentScreenItem == 0 ? MENU_TEXT_SELECTED.g : MENU_TEXT_NORMAL.g,
	               m_nCurrentScreenItem == 0 ? MENU_TEXT_SELECTED.b : MENU_TEXT_NORMAL.b, 255);
	font::printString(StretchX(57.0f), StretchY(410.0f), TheText.get("FEDS_TB"));
}

void CMenuManager::drawStandardMenus(bool drawTitle) {
	constexpr uint16_t DEF_X = 320 / 2 + 160, DEF_Y = 480 / 4 + 10;
	auto& scr = aScreens[m_nCurrentScreen];

	font::setProportional(true);
	font::setOrientation(ALIGN_LEFT);
	font::setWrapx(ScreenR(10));
	font::setRightJustifyWrap(StretchX(10));
	font::setCentreSize(640);

	if (drawTitle && scr.m_szTitleName[0]) {
		font::setOrientation(ALIGN_LEFT);
		font::setFontStyle(FONT_GOTHIC);
		font::setScale(StretchX(1.3f), StretchY(2.1f));
		font::setEdge(1);
		font::setColor(TITLE_COLOR.r, TITLE_COLOR.g, TITLE_COLOR.b, TITLE_COLOR.a);
		font::setDropColor(0, 0, 0, 255);
		font::printString(StretchX(40.0f), StretchY(28.0f), TheText.get(scr.m_szTitleName));
	}

	const bool hasLabel = scr.m_aItems[0].m_nActionType == MENU_ACTION_TEXT;
	if (hasLabel) {
		font::setWrapx(ScreenR(40));
		font::setFontStyle(FONT_SUBTITLES);
		font::setScale(StretchX(0.49f), StretchY(1.2f));
		font::setOrientation(ALIGN_LEFT);
		font::setEdge(2);
		font::setDropColor(MENU_BG.r, MENU_BG.g, MENU_BG.b, MENU_BG.a);
		font::setColor(MENU_TEXT_NORMAL.r, MENU_TEXT_NORMAL.g, MENU_TEXT_NORMAL.b, MENU_TEXT_NORMAL.a);
		font::printString(StretchX(60.0f), StretchY(97.0f), TheText.get(scr.m_aItems[0].m_szName));
		font::setWrapx(ScreenR(10));
	}

	for (int i = 0; i < 12; i++) {
		auto& item = scr.m_aItems[i];
		if (!item.m_nActionType) break;
		const uint16_t lineHeight = item.m_nType == TI_MPACK ? 20 : 30;

		font::setFontStyle(FONT_MENU);
		if (item.m_nType < TI_SLOT1 || item.m_nType > TI_SLOT8) {
			font::setScale(StretchX(0.7f), StretchY(1.0f));
			font::setEdge(2);
		} else {
			font::setEdge(1);
			font::setScale(StretchX(0.42f), StretchY(0.95f));
		}
		font::setDropColor(0, 0, 0, 255);
		const bool sel = i == m_nCurrentScreenItem && m_bMapLoaded;
		sprite::RGBA tc = sel ? MENU_TEXT_SELECTED : MENU_TEXT_NORMAL;
		font::setColor(tc.r, tc.g, tc.b, tc.a);
		font::setOrientation(item.m_nAlign == MENU_ALIGN_LEFT ? ALIGN_LEFT
		                   : item.m_nAlign == MENU_ALIGN_RIGHT ? ALIGN_RIGHT : ALIGN_CENTER);

		if (!item.m_X && !item.m_Y) {
			if (i == 0 || (i == 1 && hasLabel)) {
				item.m_X = DEF_X;
				item.m_Y = DEF_Y;
			} else {
				item.m_X = scr.m_aItems[i - 1].m_X;
				item.m_Y = scr.m_aItems[i - 1].m_Y + lineHeight;
			}
		}
		if (item.m_nActionType == MENU_ACTION_TEXT || !item.m_szName[0]) continue;

		const bool isSlot = item.m_nType >= FIRST_SAVE_SLOT && item.m_nType <= MAX_SAVE_SLOT;
		const GxtChar* displayText = nullptr;
		const GxtChar* rightText = nullptr;
		float xOffset = 0;

		if (item.m_nType == TI_MOUSEJOYPAD) {
			displayText = TheText.get(m_ControlMethod == eController::JOYPAD ? "FEJ_TIT" : "FEC_MOU");
		} else if (isSlot) {
			item.m_X = 80;
			font::setOrientation(ALIGN_LEFT);
			if (m_Slots[i - 1] == eSlotState::SLOT_FREE) {
				char key[16];
				snprintf(key, sizeof key, "FEM_SL%d", i);
				font::setOrientation(ALIGN_CENTER);
				item.m_X = DEF_X;
				displayText = TheText.get(key);
				xOffset = StretchX(40.0f);
			}
		} else {
			displayText = TheText.get(item.m_szName);
		}

		const auto onOff = [](bool c) { return TheText.get(c ? "FEM_ON" : "FEM_OFF"); };
		switch (item.m_nActionType) {
		case MENU_ACTION_FRAME_LIMITER: rightText = onOff(m_bPrefsFrameLimiter); break;
		case MENU_ACTION_SUBTITLES: rightText = onOff(m_bShowSubtitles); break;
		case MENU_ACTION_WIDESCREEN: rightText = onOff(m_bWidescreenOn); break;
		case MENU_ACTION_RADIO_EQ: rightText = onOff(m_bRadioEq); break;
		case MENU_ACTION_RADIO_RETUNE: rightText = onOff(m_bRadioAutoSelect); break;
		case MENU_ACTION_SHOW_LEGEND: rightText = onOff(m_bMapLegend); break;
		case MENU_ACTION_HUD_MODE: rightText = onOff(m_bHudOn); break;
		case MENU_ACTION_CONTROLS_JOY_INVERT_X: rightText = onOff(m_bInvertPadX1); break;
		case MENU_ACTION_CONTROLS_JOY_INVERT_Y: rightText = onOff(m_bInvertPadY1); break;
		case MENU_ACTION_CONTROLS_JOY_INVERT_X2: rightText = onOff(m_bInvertPadX2); break;
		case MENU_ACTION_CONTROLS_JOY_INVERT_Y2: rightText = onOff(m_bInvertPadY2); break;
		case MENU_ACTION_CONTROLS_JOY_SWAP_AXIS1: rightText = onOff(m_bSwapPadAxis1); break;
		case MENU_ACTION_CONTROLS_JOY_SWAP_AXIS2: rightText = onOff(m_bSwapPadAxis2); break;
		case MENU_ACTION_USER_TRACKS_AUTO_SCAN: rightText = onOff(m_bTracksAutoScan); break;
		case MENU_ACTION_STORE_PHOTOS: rightText = onOff(m_bSavePhotos); break;
		case MENU_ACTION_MIP_MAPPING: rightText = onOff(m_bPrefsMipMapping); break;
		case MENU_ACTION_CONTROLS_MOUSE_INVERT_Y: rightText = onOff(!m_bInvertMouseY); break;
		case MENU_ACTION_MOUSE_STEERING: rightText = onOff(m_bEnableMouseSteering); break;
		case MENU_ACTION_MOUSE_FLY: rightText = onOff(m_bEnableMouseFlying); break;
		case MENU_ACTION_RADAR_MODE:
			rightText = TheText.get(m_nRadarMode == eRadarMode::RADAR_MODE_MAPS_AND_BLIPS ? "FED_RDM"
			          : m_nRadarMode == eRadarMode::RADAR_MODE_BLIPS_ONLY ? "FED_RDB" : "FEM_OFF");
			break;
		case MENU_ACTION_FX_QUALITY:
			rightText = TheText.get(m_nPrefsFxQuality == 0 ? "FED_FXL" : m_nPrefsFxQuality == 1 ? "FED_FXM"
			          : m_nPrefsFxQuality == 2 ? "FED_FXH" : "FED_FXV");
			break;
		case MENU_ACTION_ANTIALIASING: {
			if (m_nDisplayAntialiasing <= 1) {
				rightText = TheText.get("FEM_OFF");
			} else {
				static GxtChar tmp[16];
				snprintf((char*)tmp, sizeof tmp, "%d", (int)m_nDisplayAntialiasing - 1);
				rightText = tmp;
			}
			break;
		}
		case MENU_ACTION_RESOLUTION: rightText = (const GxtChar*)"256x192"; break;
		case MENU_ACTION_CONTROL_TYPE:
			rightText = TheText.get(m_ControlMethod == eController::JOYPAD ? "FET_CCN" : "FET_SCN");
			break;
		case MENU_ACTION_USER_TRACKS_PLAY_MODE: {
			char key[16];
			snprintf(key, sizeof key, "FEA_PR%d", m_RadioMode + 1);
			rightText = TheText.get(key);
			break;
		}
		case MENU_ACTION_RADIO_STATION: rightText = getRadioStationName(); break;
		default: break;
		}

		const float sx = StretchX(item.m_X), sy = StretchY(item.m_Y);
		if (displayText) {
			if (isSlot && xOffset) font::setOrientation(ALIGN_CENTER);
			font::printString(sx, sy, displayText);
		}
		if (rightText) {
			font::setFontStyle(FONT_MENU);
			font::setEdge(1);
			font::setOrientation(ALIGN_RIGHT);
			font::setRightJustifyWrap(640);
			font::setScale(StretchX(0.7f), StretchY(1.0f));
			font::printString(ScreenR(40), sy, rightText);
		}

		if (displayText && i == m_nCurrentScreenItem && m_nCurrentScreen != SCREEN_MAP && m_nCurrentScreen != SCREEN_BRIEF) {
			if (!xOffset) {
				switch (item.m_nAlign) {
				case MENU_ALIGN_LEFT: xOffset = sx - StretchX(40.0f); break;
				case MENU_ALIGN_RIGHT: xOffset = StretchX(40.0f) + sx; break;
				default: xOffset = sx - StretchX(40.0f) - font::getStringWidth(displayText) * 0.5f; break;
				}
			}
			if (m_bMapLoaded && spriteTex(FRONTEND_SPRITE_ARROW)) {
				switch (m_nCurrentScreen) {
				case SCREEN_LOAD_FIRST_SAVE:
				case SCREEN_DELETE_FINISHED:
				case SCREEN_SAVE_DONE_1: break;
				default:
					sprite::draw(spriteTex(FRONTEND_SPRITE_ARROW),
					             { xOffset, sy - 5.0f, xOffset + StretchX(32.0f), sy + StretchX(47.0f) }, { 255, 255, 255, 255 });
					break;
				}
			}
		}

		const auto slider = [&](float value, float vy) {
			displaySlider(500.0f, 125.0f - vy, StretchY(4), StretchY(20), StretchX(100), value, (int)StretchX(3));
		};
		switch (item.m_nActionType) {
		case MENU_ACTION_BRIGHTNESS: slider(m_PrefsBrightness / 384.0f, 0); break;
		case MENU_ACTION_RADIO_VOL: slider(m_nRadioVolume / 64.0f, 30); break;
		case MENU_ACTION_SFX_VOL: slider(m_nSfxVolume / 64.0f, 0); break;
		case MENU_ACTION_DRAW_DIST: slider((m_fDrawDistance - 0.925f) / 0.875f, 0); break;
		case MENU_ACTION_MOUSE_SENS: slider(m_fMouseSens / 0.005f, 0); break;
		default: break;
		}
	}
}

int32_t CMenuManager::displaySlider(float x, float y, float h1, float h2, float len, float value, int32_t spacing) {
	constexpr int N = 16;
	int32_t lastX = 0;
	for (int i = 0; i < N; i++) {
		float bx = x + i * len / N;
		float bh = (h1 * (N - i) + h2 * i) / N;
		sprite::RGBA c = i / (float)N + 1.0f / (2 * N) < value ? MENU_TEXT_SELECTED : MENU_TEXT_NORMAL;
		if (c.r == MENU_TEXT_SELECTED.r) lastX = (int32_t)bx;
		sprite::rect({ bx + StretchX(2), y + bh + StretchY(2), bx + spacing + StretchX(2), y + StretchY(2) }, MENU_SHADOW);
		sprite::rect({ bx, y + bh, bx + spacing, y }, c);
	}
	return lastX;
}

const GxtChar* CMenuManager::getRadioStationName() const {
	static const char* keys[] = { "AA", "FEA_RSS" };
	static const struct { const char* key; } names[] = {
		{ "FEA_RSS" }
	};
	static const char* station[] = {
		"Playback FM", "K-Rose", "K-DST", "Bounce FM", "SF-UR", "Radio Los Santos", "Radio X",
		"CSR 103.9", "K-Jah West", "Master Sounds 98.3", "WCTR", "RADIO OFF"
	};
	(void)keys; (void)names;
	if (m_nRadioStation >= 1 && m_nRadioStation <= 12)
		return (const GxtChar*)station[m_nRadioStation - 1];
	return TheText.get("FEM_OFF");
}
