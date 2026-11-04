#pragma once

#include <nds/ndstypes.h>

// CTimeCycle + CColourSet port (TimeCycle.cpp, Collision/ColourSet.cpp).
// timecyc.dat holds 8 time samples x 23 weathers; every frame the two
// samples around the clock are blended for the old and the new weather, and
// the result is blended by CWeather::InterpolationValue. All blends are 8-bit
// integer lerps over a flat byte layout (no soft-float per field).
// Not ported: timecycle boxes (IPL "tcyc"), extra colours (scripts),
// underwater/tunnel sets, post-FX colour correction (no DS equivalent).
namespace timecycle {

constexpr int NUM_HOURS = 8;   // eTimeType: midnight 5 6 7 noon 19 20 22

// CColourSet subset. Sizes keep the original x10 fixed point (int8 storage);
// illumination is m_nDirectionalMult (dirMult * 100).
struct Colours {
	u8 ambR, ambG, ambB;
	u8 ambObjR, ambObjG, ambObjB;
	u8 skyTopR, skyTopG, skyTopB;
	u8 skyBotR, skyBotG, skyBotB;
	u8 sunCoreR, sunCoreG, sunCoreB;
	u8 sunCoronaR, sunCoronaG, sunCoronaB;
	u8 sunSize, spriteSize, spriteBrightness;
	u8 shadow, lightShadow, poleShadow;
	u8 lightOnGround;
	u8 lowCloudR, lowCloudG, lowCloudB;
	u8 fluffyR, fluffyG, fluffyB;
	u8 waterR, waterG, waterB, waterA;
	u8 postFx1R, postFx1G, postFx1B, postFx1A;
	u8 postFx2R, postFx2G, postFx2B, postFx2A;
	u8 cloudAlpha, highLightMin, waterFogAlpha, illumination;
	u8 pad;
	s16 farClip, fogStart;
};

// Resumable timecyc.dat parser (one weather = 8 lines per call); true when
// done. The table lives in PERM and is parsed once per boot.
bool loadStep();
bool loaded();

// CTimeCycle::Update -> CalcColoursForPoint at the camera. Reads the game
// clock (player::) and the weather blend (weather::).
void update(float camX, float camY, float camZ);

const Colours& current();
// m_VectorToSun (SA axes: x east, y north, z up), normalised.
const float* vectorToSun();
// m_BelowHorizonGrey.
void belowHorizonGrey(u8& r, u8& g, u8& b);
// GetDayNightBalance: 0 = day (07-20h), 1 = night (21-06h), linear between.
float dayNightBalance();
// Light for peds/objects (SetLightColoursForPedsCarsAndObjects with
// CPhysical::GetLightingFromCol), folded to one RGB multiplier 0..255 since
// CJ has no normals on DS: fMult * (ambient_obj + illumination * k).
void pedLight(u8& r, u8& g, u8& b);
// tColLighting (day low nibble, night high nibble) of the floor under the
// player (game/collision), for pedLight's contact surface brightness.
void setSurfaceLight(u8 light);
// Brightness for prelit world geometry (day/night prelit blend + ambient).
void worldLight(u8& r, u8& g, u8& b);

}
