#include "game/menu/menu.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include "assets/texture.h"
#include "assets/txdfile.h"
#include "audio/aeradio.h"
#include "game/text/font.h"
#include "game/text/gxt.h"
#include "game/hud/hudfont.h"
#include "game/hud/hudtex.h"
#include "game/hud/chud.h"
#include "game/hud/radar.h"
#include "game/hud/subfb.h"
#include "game/player.h"
#include "mem/mem.h"
#include "platform/fs.h"
#include "platform/input.h"
#include "game/sfx.h"
#include "hud/hud.h"
#include "platform/timing.h"
#include "platform/video.h"
#include "render/sprite2d.h"

CMenuManager FrontEndMenuManager;

// SA menu colours (MenuManager.h / HudColours)
static constexpr sprite::RGBA MENU_BG{ 0, 0, 0, 255 };
static constexpr sprite::RGBA MENU_TEXT_NORMAL{ 74, 90, 107, 255 };
static constexpr sprite::RGBA MENU_TEXT_SELECTED{ 172, 203, 241, 255 };
static constexpr sprite::RGBA MENU_SHADOW{ 0, 0, 0, 255 };
static constexpr sprite::RGBA TITLE_COLOR{ 172, 203, 241, 255 };
static constexpr sprite::RGBA DISABLED{ 14, 30, 47, 255 };

// action/screen sets used for frontend audio events (MenuManager_Input.cpp)
static constexpr auto mainMenu = { SCREEN_MAIN_MENU, SCREEN_PAUSE_MENU, SCREEN_GAME_SAVE,
	SCREEN_GAME_WARNING_DONT_SAVE, SCREEN_SAVE_DONE_1, SCREEN_DELETE_FINISHED, SCREEN_EMPTY };
static constexpr auto sliderActions = { MENU_ACTION_BRIGHTNESS, MENU_ACTION_RADIO_VOL, MENU_ACTION_SFX_VOL,
	MENU_ACTION_TOUCH_SENS };
static constexpr auto sliderSelect = { MENU_ACTION_BRIGHTNESS, MENU_ACTION_RADIO_VOL,
	MENU_ACTION_TOUCH_SENS };
static constexpr auto specialScreens = { SCREEN_AUDIO_SETTINGS, SCREEN_DISPLAY_SETTINGS, SCREEN_CONTROLLER_SETUP };
static constexpr auto specialMenuActions = { MENU_ACTION_BACK, MENU_ACTION_MENU, MENU_ACTION_BRIGHTNESS,
	MENU_ACTION_RADIO_VOL, MENU_ACTION_SFX_VOL, MENU_ACTION_RADIO_STATION, MENU_ACTION_TOUCH_SENS };

template<class C, class T>
static bool contains(const C& v, const T& x) {
	for (auto e : v) if (e == x) return true;
	return false;
}

// Menu coords are virtual 640x480 (same numbers as the PC frontend); the ortho
// projection in sprite::beginFrame compresses them to the 256x192 DS screen.
static inline float StretchX(float x) { return x; }
static inline float StretchY(float y) { return y; }
static inline float ScreenR(float x) { return 640.0f - x; }

static const tex::Texture* s_sprites[FRONTEND_SPRITE_COUNT];
static const tex::Texture* s_font2 = nullptr;
static const tex::Texture* s_font1 = nullptr;
static constexpr int LEGEND_ROWS = 6;
static const tex::Texture* s_legendTex[radar::MAP_TYPE_COUNT];
static int s_legendNext;
static FILE* s_globalFile;
static u8* s_globalMap;
static u32 s_globalSize, s_globalRead;

static void closeGlobalMap() {
	if (s_globalFile) fclose(s_globalFile);
	s_globalFile = nullptr;
	mem::free(s_globalMap);
	s_globalMap = nullptr;
	s_globalSize = s_globalRead = 0;
}

static bool openGlobalMap() {
	closeGlobalMap();
	FILE* f = fopen("nitro:/models/fronten2.txd", "rb");
	ds::TxdHeader h;
	if (!f || fread(&h, sizeof h, 1, f) != 1 || h.magic != DS_FOURCC('D', 'T', 'X', 'D') ||
	    h.count > 128) {
		if (f) fclose(f);
		return false;
	}
	for (int i = 0; i < h.count; i++) {
		ds::TxdEntry e;
		if (fread(&e, sizeof e, 1, f) != 1) break;
		if (strcmp(e.name, "map")) continue;
		if (e.size > 128 * 1024 || e.size < sizeof(ds::TexHeader)) break;
		u8* data = (u8*)mem::alloc(e.size, mem::TAG_HUD);
		if (!data) break;
		if (fseek(f, e.offset, SEEK_SET) != 0) { mem::free(data); break; }
		s_globalFile = f;
		s_globalMap = data;
		s_globalSize = e.size;
		return true;
	}
	fclose(f);
	return false;
}

static void streamGlobalMap() {
	if (!s_globalFile) return;
	const u32 n = std::min<u32>(4096, s_globalSize - s_globalRead);
	if (fread(s_globalMap + s_globalRead, 1, n, s_globalFile) != n) {
		closeGlobalMap();
		return;
	}
	s_globalRead += n;
	if (s_globalRead == s_globalSize) {
		fclose(s_globalFile);
		s_globalFile = nullptr;
	}
}

static bool openTxd(TxdFile& txd, const char* name) {
	char path[96];
	snprintf(path, sizeof path, "nitro:/models/%s.txd", name);
	if (txd.open(path)) return true;
	hud::log("fe: %s.txd missing", name);
	return false;
}

// Streams a single DTEX entry out of a loose TXD into scratch, uploads it and
// drops the staging copy. The whole file is never held in RAM: fronten2.txd
// is ~490KB, one entry is <=66KB.
static const tex::Texture* loadTxdEntry(const TxdFile& txd, const char* name) {
	mem::Scratch scratch;
	const ds::TexHeader* h = txd.load(name, scratch);
	if (!h) hud::log("fe: %s not in txd", name);
	return h ? tex::load(h, tex::GROUP_FRONTEND) : nullptr;
}

static const tex::Texture* spriteTex(eFrontend id) { return s_sprites[(int)id]; }

static const tex::Texture* loadSpriteFile(const char* file, int first, int last) {
	TxdFile txd;
	if (!openTxd(txd, file)) return nullptr;
	const tex::Texture* firstTex = nullptr;
	for (int i = first; i < last; i++) {
		s_sprites[i] = loadTxdEntry(txd, FrontEndFilenames[i].name);
		if (!firstTex) firstTex = s_sprites[i];
	}
	return firstTex;
}

static void loadLegendStep() {
	if (s_legendNext >= radar::MAP_TYPE_COUNT) return;
	TxdFile txd;
	if (openTxd(txd, "hud"))
		s_legendTex[s_legendNext] = loadTxdEntry(txd, radar::typeIcon(s_legendNext));
	s_legendNext++;
}

void CMenuManager::initialise() {
	m_nCurrentScreen = SCREEN_INITIAL;
	m_nCurrentScreenItem = 0;
	m_bMenuActive = true;
	m_bMainMenuSwitch = true;
	for (auto& s : m_Slots) s = eSlotState::SLOT_FREE;
	sfx::init();
	loadAllTextures();
	font::init();
	font::attachFontTextures(s_font2, s_font1);
	hudfont::init();
	loadBottomArt();
	sfx::start(); // whoosh when the title menu comes up
}

void CMenuManager::loadAllTextures() {
	// Only the always-needed sprites up front; per-screen art loads lazily
	// on SwitchToNewScreen so VRAM is never exhausted.
	// The DS layout draws only the arrow and the fonts on the top screen (the
	// map / back8 art goes to the bottom framebuffer, decoded on the CPU):
	// back8, map, back8_top/right, mouse and crosshair cost ~110 KB of VRAM
	// the pause menu loaded for nothing.
	loadSpriteFile("fronten1", FRONTEND_SPRITE_ARROW, FRONTEND_SPRITE_ARROW + 1);
	TxdFile fonts;
	if (openTxd(fonts, "fonts")) {
		s_font2 = loadTxdEntry(fonts, "font2");
		s_font1 = loadTxdEntry(fonts, "font1");
	}
	m_bTexturesLoaded = s_font1 && s_font2;
	int f1 = s_font1 ? s_font1->id : 0, f2 = s_font2 ? s_font2->id : 0;
	hud::log("fe tex: %d loaded, font1=%d font2=%d map=%d back8=%d", tex::loadedCount(), f1, f2,
	         s_sprites[FRONTEND_SPRITE_MAP] ? (int)s_sprites[FRONTEND_SPRITE_MAP]->id : -1,
	         s_sprites[FRONTEND_SPRITE_BACK8] ? (int)s_sprites[FRONTEND_SPRITE_BACK8]->id : -1);
}

void CMenuManager::unloadTextures() {
	closeGlobalMap();
	tex::clear(tex::GROUP_FRONTEND);
	mem::free(m_bottomArt);
	m_bottomArt = nullptr;
	for (auto& icon : s_legendTex) icon = nullptr;
	s_legendNext = 0;
}

// Frontend art is freed while in-game (VRAM budget); reload it lazily the
// first time the pause menu is opened again.
void CMenuManager::ensureTextures() {
	if (m_bTexturesLoaded) return;
	loadAllTextures();
	font::attachFontTextures(s_font2, s_font1);
	loadBottomArt();
}

void CMenuManager::process() {
	if (!m_bMenuActive) return;
	userInput();
	if (m_nCurrentScreen == SCREEN_MAP) {
		streamGlobalMap();
		if (m_mapFull && !s_globalFile && !s_globalMap) m_mapFull = false;
		loadLegendStep();
	}
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
	const bool leavingMap = m_nCurrentScreen == SCREEN_MAP;
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
	if (m_nCurrentScreen == SCREEN_MAP && !leavingMap) {
		m_mapX = player::g.x;
		m_mapY = player::g.y;
		m_legendPage = 0;
		m_mapFull = m_mapTouch = false;
	} else if (leavingMap && m_nCurrentScreen != SCREEN_MAP) {
		closeGlobalMap();
		m_mapFull = m_mapTouch = false;
	}
}

void CMenuManager::scrollRadioStations(int8_t n) {
	m_nRadioStation += n;
	if (m_nRadioStation <= 0) m_nRadioStation = 12;
	if (m_nRadioStation > 12) m_nRadioStation = 1;
	aeradio::selectStation(m_nRadioStation);
}

bool CMenuManager::radioPreviewActive() const {
	if (!m_bMenuActive || m_bMainMenuSwitch || m_nCurrentScreen != SCREEN_AUDIO_SETTINGS) return false;
	const auto action = aScreens[m_nCurrentScreen].m_aItems[m_nCurrentScreenItem].m_nActionType;
	return action == MENU_ACTION_RADIO_VOL || action == MENU_ACTION_RADIO_STATION;
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
	const input::State& in = input::get();
	if (m_nCurrentScreen == SCREEN_MAP) {
		if (in.down & KEY_B) {
			sfx::back();
			switchToNewScreen(SCREEN_GO_BACK);
			return;
		}
		if (in.down & KEY_X) {
			if (m_mapFull) {
				closeGlobalMap();
				m_mapFull = false;
			} else if (openGlobalMap()) {
				m_mapFull = true;
			} else {
				sfx::error();
			}
		}
		u8 types[radar::MAP_TYPE_COUNT];
		const int count = radar::legendTypes(m_mapX, m_mapY, m_mapFull, types, radar::MAP_TYPE_COUNT);
		const int pages = std::max(1, (count + LEGEND_ROWS - 1) / LEGEND_ROWS);
		if (in.down & KEY_L) m_legendPage = (m_legendPage + pages - 1) % pages;
		if (in.down & KEY_R) m_legendPage = (m_legendPage + 1) % pages;
		const float step = m_mapFull ? 50.0f : 12.0f;
		if (in.held & KEY_LEFT) m_mapX -= step;
		if (in.held & KEY_RIGHT) m_mapX += step;
		if (in.held & KEY_UP) m_mapY += step;
		if (in.held & KEY_DOWN) m_mapY -= step;
		if (in.touching) {
			if (m_mapFull) {
				if (in.touchX >= 44 && in.touchX < 212 && in.touchY < 168) {
					m_mapX = -3000.0f + (in.touchX - 44) * (6000.0f / 168.0f);
					m_mapY = 3000.0f - in.touchY * (6000.0f / 168.0f);
				}
			} else if (m_mapTouch) {
				m_mapX -= (in.touchX - m_touchX) * 4.0f;
				m_mapY += (in.touchY - m_touchY) * 4.0f;
			}
			m_touchX = in.touchX;
			m_touchY = in.touchY;
		}
		m_mapTouch = in.touching;
		m_mapX = std::clamp(m_mapX, -2488.0f, 2488.0f);
		m_mapY = std::clamp(m_mapY, -2616.0f, 2616.0f);
		return;
	}
	bool bEnter = false, bExit = false, bUp = false, bDown = false;
	int8_t sliderMove = 0;

	if (getNumberOfMenuOptions() > 1) {
		if (in.down & KEY_DOWN) bDown = true;
		else if (in.down & KEY_UP) bUp = true;
	}
	if (in.down & KEY_A) bEnter = true;
	if ((in.down & KEY_B) && !contains(mainMenu, (eMenuScreen)m_nCurrentScreen)) bExit = true;
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
		if (m_nCurrentScreen != SCREEN_MAP) sfx::highlight();
		m_nCurrentScreenItem++;
		for (; scr.m_aItems[m_nCurrentScreenItem].m_nActionType == MENU_ACTION_SKIP; m_nCurrentScreenItem++);
		if (m_nCurrentScreenItem >= TI_OPTION || !scr.m_aItems[m_nCurrentScreenItem].m_nActionType)
			m_nCurrentScreenItem = scr.m_aItems[0].m_nActionType == MENU_ACTION_TEXT ? 1 : 0;
	}
	if (getNumberOfMenuOptions() > 1 && up) {
		if (m_nCurrentScreen != SCREEN_MAP) sfx::highlight();
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
		if (!back) {
			auto type = aScreens[m_nCurrentScreen].m_aItems[m_nCurrentScreenItem].m_nType;
			if (type >= FIRST_SAVE_SLOT && type <= MAX_SAVE_SLOT &&
			    m_Slots[m_nCurrentScreenItem - 1] != eSlotState::SLOT_FILLED)
				sfx::error();
			else
				sfx::select_();
		}
	}
	if (lr && scr.m_aItems[m_nCurrentScreenItem].m_nType == TI_OPTION) {
		auto act = scr.m_aItems[m_nCurrentScreenItem].m_nActionType;
		if (act != MENU_ACTION_SFX_VOL && (contains(sliderSelect, act) ||
		         (contains(specialScreens, (eMenuScreen)m_nCurrentScreen) && !contains(specialMenuActions, act))))
			sfx::select_();
		processMenuOptions(lr, back, 0);
		checkSliderMovement(lr);
		if (act == MENU_ACTION_SFX_VOL) sfx::noiseTest();
	}
	if (back && m_RadioAvailable) {
		sfx::back();
		switchToNewScreen(SCREEN_GO_BACK);
	}
}

void CMenuManager::checkSliderMovement(int8_t lr) {
	auto& item = aScreens[m_nCurrentScreen].m_aItems[m_nCurrentScreenItem];
	switch (item.m_nActionType) {
	case MENU_ACTION_BRIGHTNESS:
		m_PrefsBrightness = std::clamp<int32_t>(m_PrefsBrightness + 2 * lr, -8, 8);
		video::setScreenBrightness(m_PrefsBrightness);
		break;
	case MENU_ACTION_RADIO_VOL:
		m_nRadioVolume = std::clamp<int8_t>(4 * lr + m_nRadioVolume, 0, 64);
		break;
	case MENU_ACTION_SFX_VOL:
		m_nSfxVolume = std::clamp<int8_t>(4 * lr + m_nSfxVolume, 0, 64);
		break;
	case MENU_ACTION_TOUCH_SENS:
		m_touchSensitivity = std::clamp(m_touchSensitivity + lr * 0.125f, 0.25f, 2.0f);
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
	// ProcessMissionPackNewGame: no mission packs on DS -> always the
	// standard-game confirmation screen
	case MENU_ACTION_NEW_GAME: switchToNewScreen(SCREEN_NEW_GAME_ASK); return;
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
	case MENU_ACTION_SHOW_LEGEND:
		m_bMapLegend ^= true;
		chud::invalidate();
		return;
	case MENU_ACTION_CAMERA_FOLLOW: m_cameraFollowDpad ^= true; return;
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

void CMenuManager::loadBottomArt() {
	if (!m_bMainMenuSwitch || m_bottomArt) return;
	TxdFile txd;
	if (!openTxd(txd, "fronten2")) return;
	mem::Scratch scratch;
	const ds::TexHeader* h = txd.load("back8", scratch);
	if (!h || !h->width || !h->height) return;
	const int n = h->width * h->height;
	u16* src = (u16*)mem::alloc(n * sizeof(u16), mem::TAG_HUD);
	if (!src) return;
	m_bottomArt = (u16*)mem::alloc(subfb::W * subfb::H * sizeof(u16), mem::TAG_HUD);
	if (m_bottomArt && hudtex::decodeInto(h, src, nullptr)) {
		const int height = 128;
		memset(m_bottomArt, 0, subfb::W * subfb::H * sizeof(u16));
		for (int y = 0; y < height; y++)
			for (int x = 0; x < subfb::W; x++)
				m_bottomArt[y * subfb::W + x] = src[(y * h->height / height) * h->width + x * h->width / subfb::W];
	} else {
		mem::free(m_bottomArt);
		m_bottomArt = nullptr;
	}
	mem::free(src);
}

static u32 s_bottomSig;
static bool s_bottomValid;

void CMenuManager::invalidateBottom() { s_bottomValid = false; }

void CMenuManager::drawBottom() {
	loadBottomArt();
	// the bottom screen is static between inputs: repaint (a full clear, the
	// art copy or the pause map, text) only when what it shows changes
	const bool map = !m_bMainMenuSwitch;
	const bool pan = map && m_nCurrentScreen == SCREEN_MAP;
	u32 sig = 0x811C9DC5u;
	auto mix = [&](u32 v) { sig = (sig ^ v) * 0x01000193u; };
	mix((u32)m_bMainMenuSwitch | (u32)m_mapFull << 1 | (u32)(s_globalMap != nullptr) << 2 | (u32)(s_globalFile != nullptr) << 3);
	mix((u32)m_nCurrentScreen);
	mix((u32)(uintptr_t)m_bottomArt);
	mix((u32)m_nPrefsLanguage);
	const bool global = m_nCurrentScreen == SCREEN_MAP && m_mapFull && s_globalMap && !s_globalFile;
	if (map && !global) {
		const float cx = pan ? m_mapX : player::g.x, cy = pan ? m_mapY : player::g.y;
		radar::streamPauseMap(cx, cy);
		mix((u32)(int)cx);
		mix((u32)(int)cy);
		mix(radar::tileLoads());
	}
	if (s_bottomValid && sig == s_bottomSig) return;
	s_bottomValid = true;
	s_bottomSig = sig;
	subfb::clear(subfb::rgb(0, 0, 0));
	if (m_bMainMenuSwitch) {
		if (m_bottomArt) memcpy(subfb::frame(), m_bottomArt, subfb::W * subfb::H * sizeof(u16));
	} else if (m_nCurrentScreen == SCREEN_MAP && m_mapFull && s_globalMap && !s_globalFile) {
		drawGlobalMap();
	} else {
		radar::drawPauseMap(m_nCurrentScreen == SCREEN_MAP ? m_mapX : player::g.x,
		                    m_nCurrentScreen == SCREEN_MAP ? m_mapY : player::g.y);
	}
	const u16 black = subfb::rgb(0, 0, 0);
	const auto& scr = aScreens[m_nCurrentScreen == SCREEN_INITIAL ? (int)SCREEN_MAIN_MENU : m_nCurrentScreen];
	const u16 ink = subfb::rgb(224, 236, 255);
	if (m_bMainMenuSwitch) {
		subfb::rect(0, 124, 256, 68, black);
		hudfont::print(hudfont::STYLE_GOTHIC, 128, 127, TheText.get(scr.m_szTitleName),
		               0.75f, 1.35f, ink, hudfont::CENTER, 1, black);
		hudfont::print(hudfont::STYLE_GOTHIC, 128, 163, (const GxtChar*)"PAD  A OK  B Back",
		               0.55f, 1.0f, ink, hudfont::CENTER, 1, black);
	} else {
		if (m_nCurrentScreen == SCREEN_MAP) {
			if (m_mapFull && s_globalMap && !s_globalFile) {
				subfb::rect(0, 169, 256, 23, black);
				hudfont::print(hudfont::STYLE_GOTHIC, 128, 171,
				               (const GxtChar*)"X Region  L/R Page  B Back",
				               0.5f, 0.9f, ink, hudfont::CENTER, 1, black);
			} else {
				subfb::rect(0, 149, 256, 43, black);
				hudfont::print(hudfont::STYLE_GOTHIC, 128, 151,
				               (const GxtChar*)(m_mapFull ? "Loading map" : "PAD Pan  X Full"),
				               0.5f, 0.9f, ink, hudfont::CENTER, 1, black);
				hudfont::print(hudfont::STYLE_GOTHIC, 128, 172, (const GxtChar*)"L/R Page  B Back  START",
				               0.5f, 0.9f, ink, hudfont::CENTER, 1, black);
			}
		} else {
			subfb::rect(0, 169, 256, 23, black);
			hudfont::print(hudfont::STYLE_GOTHIC, 128, 171, (const GxtChar*)"A OK  B Back  START Resume",
			               0.5f, 0.9f, ink, hudfont::CENTER, 1, black);
		}
	}
}

void CMenuManager::drawGlobalMap() {
	const ds::TexHeader* h = (const ds::TexHeader*)s_globalMap;
	const u32 paletteOff = sizeof(*h) + ((h->texelBytes + 3) & ~3u);
	if (h->magic != DS_FOURCC('D', 'T', 'E', 'X') || h->format != 4 ||
	    h->texelBytes < (u32)h->width * h->height || h->numColors == 0 ||
	    paletteOff + (u32)h->numColors * 2 > s_globalSize) return;
	const u8* texels = s_globalMap + sizeof(*h);
	const u16* palette = (const u16*)(s_globalMap + paletteOff);
	constexpr int size = 168, left = (subfb::W - size) / 2;
	u16* dst = subfb::frame();
	for (int y = 0; y < size; y++) {
		const u8* src = texels + (y * h->height / size) * h->width;
		for (int x = 0; x < size; x++) {
			const u8 c = src[x * h->width / size];
			dst[y * subfb::STRIDE + left + x] = c < h->numColors ? palette[c] | BIT(15) : subfb::rgb(0, 0, 0);
		}
	}
	radar::drawFullPoints(left, 0, size);
	const int px = left + std::clamp((int)((player::g.x + 3000.0f) * size / 6000.0f), 0, size - 1);
	const int py = std::clamp((int)((3000.0f - player::g.y) * size / 6000.0f), 0, size - 1);
	const int mx = left + std::clamp((int)((m_mapX + 3000.0f) * size / 6000.0f), 0, size - 1);
	const int my = std::clamp((int)((3000.0f - m_mapY) * size / 6000.0f), 0, size - 1);
	subfb::circle(px, py, 4, subfb::rgb(0, 0, 0));
	subfb::circle(px, py, 2, subfb::rgb(255, 255, 255));
	subfb::circle(mx, my, 5, subfb::rgb(0, 0, 0));
	subfb::circle(mx, my, 3, subfb::rgb(255, 202, 80));
}

void CMenuManager::drawBackground() {
	if (!m_bTexturesLoaded) return;
	sprite::rect({ 0, 0, 640, 480 }, MENU_BG);
	if (m_nCurrentScreen == SCREEN_MAP) {
		drawLegend();
		return;
	}
	drawDsMenus();
}

void CMenuManager::drawLegend() {
	u8 types[radar::MAP_TYPE_COUNT];
	const int count = radar::legendTypes(m_mapX, m_mapY, m_mapFull, types, radar::MAP_TYPE_COUNT);
	const int pages = std::max(1, (count + LEGEND_ROWS - 1) / LEGEND_ROWS);
	m_legendPage = std::min<int>(m_legendPage, pages - 1);
	font::setProportional(true);
	font::setOrientation(ALIGN_CENTER);
	font::setDropColor(0, 0, 0);
	font::setEdge(1);
	font::setFontStyle(FONT_GOTHIC);
	font::setScale(1.4f, 2.2f);
	font::setColor(TITLE_COLOR.r, TITLE_COLOR.g, TITLE_COLOR.b);
	font::printString(320, 14, TheText.get("MAP_LEG"));
	for (int row = 0; row < LEGEND_ROWS; row++) {
		const int i = m_legendPage * LEGEND_ROWS + row;
		if (i >= count) break;
		const int type = types[i];
		const float y = 85.0f + row * 58.0f;
		if (s_legendTex[type])
			sprite::draw(s_legendTex[type], { 68, y, 108, y + 40 }, { 255, 255, 255, 255 });
		font::setFontStyle(FONT_SUBTITLES);
		font::setOrientation(ALIGN_LEFT);
		font::setScale(1.15f, 1.8f);
		font::setColor(220, 233, 249);
		font::printString(132, y + 2, TheText.get(radar::typeLabelKey(type)));
	}
	char page[24];
	snprintf(page, sizeof page, "%d / %d", (int)m_legendPage + 1, pages);
	font::setFontStyle(FONT_SUBTITLES);
	font::setOrientation(ALIGN_CENTER);
	font::setScale(1.05f, 1.6f);
	font::setColor(TITLE_COLOR.r, TITLE_COLOR.g, TITLE_COLOR.b);
	font::printString(320, 423, (const GxtChar*)page);
}

void CMenuManager::drawDsMenus() {
	const auto& scr = aScreens[m_nCurrentScreen];
	int rows[12], count = 0, selected = 0;
	for (int i = 0; i < 12 && scr.m_aItems[i].m_nActionType; i++) {
		const auto action = scr.m_aItems[i].m_nActionType;
		if (action == MENU_ACTION_TEXT || action == MENU_ACTION_SKIP || !scr.m_aItems[i].m_szName[0]) continue;
		if (i == m_nCurrentScreenItem) selected = count;
		rows[count++] = i;
	}
	const bool hasLabel = scr.m_aItems[0].m_nActionType == MENU_ACTION_TEXT;
	const float top = (m_bMainMenuSwitch ? 112.0f : 95.0f) + (hasLabel ? 48.0f : 0.0f);
	constexpr float pitch = 57, bottom = 410;
	const float maxScroll = std::max(0.0f, top + (count - 1) * pitch - bottom);
	float target = 0;
	if (maxScroll > 0) {
		target = std::clamp(top + (selected + (selected < count - 1 ? 1 : 0)) * pitch - bottom,
		                    0.0f, maxScroll);
	}
	if (m_scrollScreen != m_nCurrentScreen) {
		m_scrollScreen = m_nCurrentScreen;
		m_scroll = 0;
	}
	m_scroll += (target - m_scroll) * 0.27f;
	if (target > m_scroll && target - m_scroll < 0.5f) m_scroll = target;
	if (target < m_scroll && m_scroll - target < 0.5f) m_scroll = target;

	font::setProportional(true);
	font::setWrapx(620);
	font::setRightJustifyWrap(20);
	font::setCentreSize(600);
	font::setDropColor(0, 0, 0, 255);
	font::setEdge(0);
	font::setFontStyle(FONT_GOTHIC);
	font::setOrientation(ALIGN_CENTER);
	font::setScale(1.55f, 2.5f);
	font::setColor(TITLE_COLOR.r, TITLE_COLOR.g, TITLE_COLOR.b);
	for (int r = 0; r < count; r++) {
		const int i = rows[r];
		const auto& item = scr.m_aItems[i];
		const float y = top + r * pitch - m_scroll;
		if (y < 65 || y > 427) continue;
		const bool chosen = i == m_nCurrentScreenItem && m_bMapLoaded;
		const sprite::RGBA color = chosen ? MENU_TEXT_SELECTED : sprite::RGBA{ 182, 195, 210, 255 };
		if (chosen) sprite::rect({ 18, y - 6, 622, y + 47 }, { 19, 36, 54, 255 });
		font::setFontStyle(FONT_SUBTITLES);
		font::setScale(1.15f, 1.9f);
		font::setColor(color.r, color.g, color.b);
		font::setOrientation(ALIGN_LEFT);
		const bool slot = item.m_nType >= FIRST_SAVE_SLOT && item.m_nType <= MAX_SAVE_SLOT;
		const bool spanish = m_nPrefsLanguage == eLanguage::SPANISH;
		const GxtChar* label = item.m_nActionType == MENU_ACTION_CAMERA_FOLLOW
			? (const GxtChar*)(spanish ? "Camara sigue cruceta" : "Follow D-pad turns")
			: item.m_nActionType == MENU_ACTION_TOUCH_SENS
			? (const GxtChar*)(spanish ? "Sensibilidad tactil" : "Touch sensitivity")
			: item.m_nType == TI_MOUSEJOYPAD
			? TheText.get("FEJ_TIT") : TheText.get(item.m_szName);
		font::printString(58, y, label);
		const GxtChar* value = nullptr;
		char number[24];
		const auto onOff = [](bool v) { return TheText.get(v ? "FEM_ON" : "FEM_OFF"); };
		switch (item.m_nActionType) {
		case MENU_ACTION_FRAME_LIMITER: value = onOff(m_bPrefsFrameLimiter); break;
		case MENU_ACTION_SUBTITLES: value = onOff(m_bShowSubtitles); break;
		case MENU_ACTION_WIDESCREEN: value = onOff(m_bWidescreenOn); break;
		case MENU_ACTION_RADIO_EQ: value = onOff(m_bRadioEq); break;
		case MENU_ACTION_RADIO_RETUNE: value = onOff(m_bRadioAutoSelect); break;
		case MENU_ACTION_SHOW_LEGEND: value = onOff(m_bMapLegend); break;
		case MENU_ACTION_CAMERA_FOLLOW: value = onOff(m_cameraFollowDpad); break;
		case MENU_ACTION_HUD_MODE: value = onOff(m_bHudOn); break;
		case MENU_ACTION_MIP_MAPPING: value = onOff(m_bPrefsMipMapping); break;
		case MENU_ACTION_CONTROLS_JOY_INVERT_X: value = onOff(m_bInvertPadX1); break;
		case MENU_ACTION_CONTROLS_JOY_INVERT_Y: value = onOff(m_bInvertPadY1); break;
		case MENU_ACTION_CONTROLS_JOY_INVERT_X2: value = onOff(m_bInvertPadX2); break;
		case MENU_ACTION_CONTROLS_JOY_INVERT_Y2: value = onOff(m_bInvertPadY2); break;
		case MENU_ACTION_CONTROLS_JOY_SWAP_AXIS1: value = onOff(m_bSwapPadAxis1); break;
		case MENU_ACTION_CONTROLS_JOY_SWAP_AXIS2: value = onOff(m_bSwapPadAxis2); break;
		case MENU_ACTION_CONTROLS_MOUSE_INVERT_Y: value = onOff(!m_bInvertMouseY); break;
		case MENU_ACTION_MOUSE_STEERING: value = onOff(m_bEnableMouseSteering); break;
		case MENU_ACTION_MOUSE_FLY: value = onOff(m_bEnableMouseFlying); break;
		case MENU_ACTION_USER_TRACKS_AUTO_SCAN: value = onOff(m_bTracksAutoScan); break;
		case MENU_ACTION_STORE_PHOTOS: value = onOff(m_bSavePhotos); break;
		case MENU_ACTION_RADAR_MODE: value = TheText.get(m_nRadarMode == eRadarMode::RADAR_MODE_MAPS_AND_BLIPS ? "FED_RDM"
			: m_nRadarMode == eRadarMode::RADAR_MODE_BLIPS_ONLY ? "FED_RDB" : "FEM_OFF"); break;
		case MENU_ACTION_SFX_VOL:
			snprintf(number, sizeof number, "< %d >", (int)m_nSfxVolume * 100 / 64);
			value = (const GxtChar*)number; break;
		case MENU_ACTION_RADIO_VOL:
			snprintf(number, sizeof number, "< %d >", (int)m_nRadioVolume * 100 / 64);
			value = (const GxtChar*)number; break;
		case MENU_ACTION_BRIGHTNESS:
			snprintf(number, sizeof number, "< %ld >", (long)m_PrefsBrightness);
			value = (const GxtChar*)number; break;
		case MENU_ACTION_TOUCH_SENS:
			snprintf(number, sizeof number, "< %d%% >", (int)(m_touchSensitivity * 100.0f));
			value = (const GxtChar*)number; break;
		case MENU_ACTION_DRAW_DIST:
			snprintf(number, sizeof number, "< %d >", (int)(m_fDrawDistance * 100));
			value = (const GxtChar*)number; break;
		case MENU_ACTION_MOUSE_SENS:
			snprintf(number, sizeof number, "< %d >", (int)(m_fMouseSens * 100000));
			value = (const GxtChar*)number; break;
		case MENU_ACTION_FX_QUALITY: value = TheText.get(m_nPrefsFxQuality == 0 ? "FED_FXL" :
			m_nPrefsFxQuality == 1 ? "FED_FXM" : m_nPrefsFxQuality == 2 ? "FED_FXH" : "FED_FXV"); break;
		case MENU_ACTION_RESOLUTION: value = (const GxtChar*)"256x192"; break;
		case MENU_ACTION_CONTROL_TYPE: value = TheText.get(m_ControlMethod == eController::JOYPAD ? "FET_CCN" : "FET_SCN"); break;
		case MENU_ACTION_RADIO_STATION: value = getRadioStationName(); break;
		case MENU_ACTION_ANTIALIASING:
			snprintf(number, sizeof number, "%d", (int)m_nDisplayAntialiasing - 1);
			value = m_nDisplayAntialiasing <= 1 ? TheText.get("FEM_OFF") : (const GxtChar*)number;
			break;
		case MENU_ACTION_USER_TRACKS_PLAY_MODE: {
			char key[16];
			snprintf(key, sizeof key, "FEA_PR%d", m_RadioMode + 1);
			value = TheText.get(key);
			break;
		}
		default: break;
		}
		if (slot && m_Slots[i - 1] == eSlotState::SLOT_FREE) value = (const GxtChar*)"-";
		if (value) {
			font::setFontStyle(FONT_SUBTITLES);
			font::setScale(1.05f, 1.75f);
			font::setOrientation(ALIGN_RIGHT);
			font::printString(605, y + 2, value);
		}
	}
	{
		sprite::rect({ 0, 0, 640, m_bMainMenuSwitch ? 82.0f : 72.0f }, MENU_BG);
		font::setFontStyle(FONT_GOTHIC);
		font::setEdge(1);
		font::setOrientation(ALIGN_CENTER);
		font::setScale(1.5f, 2.3f);
		font::setColor(TITLE_COLOR.r, TITLE_COLOR.g, TITLE_COLOR.b);
		font::printString(320, m_bMainMenuSwitch ? 24 : 12, TheText.get(scr.m_szTitleName));
		if (hasLabel) {
			font::setFontStyle(FONT_SUBTITLES);
			font::setScale(1.0f, 1.55f);
			font::setOrientation(ALIGN_CENTER);
			font::printString(320, m_bMainMenuSwitch ? 83 : 74, TheText.get(scr.m_aItems[0].m_szName));
		}
		sprite::rect({ 0, 445, 640, 480 }, MENU_BG);
		if (maxScroll > 0 && m_scroll < maxScroll - 2)
			sprite::rect({ 302, 442, 338, 447 }, MENU_TEXT_SELECTED);
	}
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
