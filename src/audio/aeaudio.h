#pragma once

#include <stdint.h>

// Shared audio ids and helpers (Audio/Enums/eSoundBank.h, eAudioEvents.h,
// SoundIDs.h, CAEAudioEntity / CAEAudioUtility). Only the subset the port
// plays today; the values are the original ones so the tables can be grepped.
namespace ae {

// eSoundBank (global bank ids, banks.dsbl maps them to a pak + index)
enum Bank : uint16_t {
	BANK_FEET_GENERIC = 0,
	BANK_FEET_GRASS = 1,
	BANK_FEET_GRAVEL = 2,
	BANK_FEET_METAL = 3,
	BANK_FEET_SAND = 4,
	BANK_FEET_TILE = 5,
	BANK_FEET_WOOD = 6,
	BANK_GENRL_BULLET_HITS = 27,
	BANK_GENRL_COLLISIONS = 39,
	BANK_GENRL_EXPLOSIONS = 52,
	BANK_GENRL_FRONTEND_GAME = 59,
	BANK_GENRL_FRONTEND_MENU = 60,
	BANK_GENRL_LOADING = 82,
	BANK_GENRL_RAIN = 105,
	BANK_GENRL_SWIMMING = 128,
	BANK_GENRL_WEAPONS = 143,
	BANK_PAIN_A_CARL = 144,
	BANK_PAIN_A_FEMALE = 145,
	BANK_PAIN_A_MALE = 146,
};

// eAudioEvents (the index into EventVol.dat)
enum Event : int16_t {
	AE_UNDEFINED = -1,
	AE_FRONTEND_START = 0,
	AE_FRONTEND_SELECT = 1,
	AE_FRONTEND_BACK = 2,
	AE_FRONTEND_HIGHLIGHT = 3,
	AE_FRONTEND_ERROR = 4,
	AE_FRONTEND_NOISE_TEST = 5,
	AE_FRONTEND_LOADING_TUNE_START = 50,
	AE_PED_FOOTSTEP_LEFT = 54,
	AE_PED_FOOTSTEP_RIGHT = 55,
	AE_PED_LAND_ON_FEET_AFTER_FALL = 58,
	AE_PED_COLLAPSE_AFTER_FALL = 59,
	AE_PED_SWING = 60,
	AE_PED_HIT_HIGH_UNARMED = 65,
	AE_BULLET_HIT = 117,
	AE_THUNDER = 141,
	AE_RAIN = 143,
	AE_CITY_NOISE = 144,
	AE_WEAPON_FIRE = 145,
	AE_WEAPON_RELOAD_A = 146,
	AE_WEAPON_RELOAD_B = 147,
};

// SND_GENRL_WEAPONS (SoundIDs.h)
enum WeaponSound : int16_t {
	SND_WEAPONS_UNK = -1,
	SND_WEAPONS_9MM1SHOT_L = 0, SND_WEAPONS_9MM1SHOT_R = 1, SND_WEAPONS_9MM1TAIL = 2,
	SND_WEAPONS_AK_SHOT_L = 3, SND_WEAPONS_AK_SHOT_R = 4, SND_WEAPONS_AK_TAIL_L = 5,
	SND_WEAPONS_EAGLE_SHOT_L = 6, SND_WEAPONS_EAGLE_SHOT_R = 7, SND_WEAPONS_EAGLE_TAIL_L = 8,
	SND_WEAPONS_MP5SHOT_L = 17, SND_WEAPONS_MP5SHOT_R = 18,
	SND_WEAPONS_SHOTGUN_SHOT_L = 21, SND_WEAPONS_SHOTGUN_SHOT_R = 22, SND_WEAPONS_SHOTGUN_TAIL_R = 23,
	SND_WEAPONS_SILENCED_SHOT_L = 24,
	SND_WEAPONS_SNIPER_SHOT_L = 26, SND_WEAPONS_SNIPER_SHOT_R = 27,
	SND_WEAPONS_9MM1DRY = 29, SND_WEAPONS_9MM1LOWS = 30,
	SND_WEAPONS_AK_DRYFIRE = 33,
	SND_WEAPONS_BODY_PUNCH1 = 36,   // .. PUNCH4 = 39
	SND_WEAPONS_DETONATOR_BEEP = 49,
	SND_WEAPONS_EAGLE_DRYFIRE = 52, SND_WEAPONS_EAGLE_LOWS = 53,
	SND_WEAPONS_SHOTGUN_DRYFIRE = 73, SND_WEAPONS_SHOTGUN_LOWS = 74,
	SND_WEAPONS_SILENCED_DRYFIRE = 76, SND_WEAPONS_SILENCED_LOWS = 77,
	SND_WEAPONS_WEAPON_SWIPE = 86, SND_WEAPONS_WEAPON_SWIPE_FIST = 87, SND_WEAPONS_WEAPON_SWIPE_METAL = 88,
};

// SND_GENRL_EXPLOSIONS
enum ExplosionSound : int16_t { SND_EXPLOSIONS_DISTANT_L = 1, SND_EXPLOSIONS_NEAR_L = 4 };

constexpr float VOLUME_SILENCE = -100.0f;

// EventVol.dat (CAEAudioEntity::StaticInitialise): default dB per event.
void loadEventVolumes();
float defaultVolume(int event);   // CAEAudioEntity::GetDefaultVolume

float audioLog10(float p);        // CAEAudioUtility::AudioLog10 (-5 below 1e-5)
int randInt(int lo, int hi);      // CAEAudioUtility::GetRandomNumberInRange, inclusive
float randFloat(float lo, float hi);
bool chance(float p);             // CAEAudioUtility::ResolveProbability
uint32_t timeMs();                // CTimer::GetTimeInMS (vblank based, keeps running in the menu)

}
