#pragma once

#include "game/menu/screens.h"
#include "game/text/gxt.h"

// CMenuManager port for DS. Same aScreens data, navigation and options logic as
// the PC frontend; rendering goes through sprite:: + font:: on the top screen.
enum class eController : int8_t { MOUSE_PLUS_KEYS, JOYPAD };
enum class eRadarMode : int8_t { RADAR_MODE_MAPS_AND_BLIPS, RADAR_MODE_BLIPS_ONLY, RADAR_MODE_OFF, RADAR_MODE_COUNT };
enum class eLanguage : int8_t { AMERICAN, SPANISH };
enum class eSlotState : int8_t { SLOT_FREE, SLOT_FILLED, SLOT_CORRUPTED };

class CMenuManager {
public:
	bool m_bMenuActive = false;
	bool m_bDontDrawFrontEnd = false;
	bool m_bMapLoaded = true;     // original sets it at initialise; enables text + highlight
	bool m_bMainMenuSwitch = true; // title menu vs pause menu
	bool m_bTexturesLoaded = false;
	bool m_RadioAvailable = true;
	int8_t m_nCurrentScreen = SCREEN_INITIAL;
	int8_t m_nPrevScreen = SCREEN_NONE;
	int8_t m_nCurrentScreenItem = 0;
	uint8_t m_nNumberOfMenuOptions = 0;
	int8_t m_SelectedSlot = 0;
	int8_t m_nRadioStation = 1;
	int8_t m_nRadioVolume = 40, m_nSfxVolume = 40;
	int8_t m_RadioMode = 0;
	int32_t m_PrefsBrightness = 256;
	float m_fDrawDistance = 1.2f;
	bool m_bPrefsFrameLimiter = true, m_bShowSubtitles = true, m_bWidescreenOn = false;
	bool m_bRadioEq = true, m_bRadioAutoSelect = true, m_bMapLegend = false, m_bHudOn = true;
	bool m_bPrefsMipMapping = true, m_bTracksAutoScan = false, m_bSavePhotos = false;
	bool m_bInvertPadX1 = false, m_bInvertPadY1 = false, m_bInvertPadX2 = false, m_bInvertPadY2 = false;
	bool m_bSwapPadAxis1 = false, m_bSwapPadAxis2 = false;
	bool m_bInvertMouseY = false, m_bEnableMouseSteering = false, m_bEnableMouseFlying = false;
	eRadarMode m_nRadarMode = eRadarMode::RADAR_MODE_MAPS_AND_BLIPS;
	int32_t m_nPrefsAntialiasing = 1, m_nDisplayAntialiasing = 1;
	int32_t m_nPrefsVideoMode = 0, m_nDisplayVideoMode = 0;
	int32_t m_nPrefsFxQuality = 2; // FX_QUALITY_MEDIUM-ish: low/medium/high
	eController m_ControlMethod = eController::JOYPAD;
	eLanguage m_nPrefsLanguage = eLanguage::AMERICAN;
	eSlotState m_Slots[8] = {};
	float m_fMouseSens = 1.0f / 2000.0f;
	bool m_WantsQuit = false;
	bool m_WantsStartGame = false;

public:
	void initialise();
	void loadAllTextures();
	void unloadTextures();
	void process();
	void drawFrontEnd();

	uint32_t getNumberOfMenuOptions();
	void switchToNewScreen(eMenuScreen screen);
	void userInput();
	void processUserInput(bool down, bool up, bool enter, bool back, int8_t lr);
	void processMenuOptions(int8_t lr, bool& cancel, bool accept);
	void checkSliderMovement(int8_t lr);
	void scrollRadioStations(int8_t n);
	void startNewGame();

private:
	void drawBackground();
	void drawStandardMenus(bool drawTitle);
	void drawMap();
	int32_t displaySlider(float x, float y, float h1, float h2, float len, float value, int32_t spacing);
	const GxtChar* getRadioStationName() const;
	void setLanguage(eLanguage l);
};

extern CMenuManager FrontEndMenuManager;
