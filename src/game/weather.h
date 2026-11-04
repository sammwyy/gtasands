#pragma once

#include <nds/ndstypes.h>

// CWeather port (Weather.cpp, Data/Weather.def.h). The weather moves one step
// through its region's 64-entry list every game hour; the old->new blend is
// the fraction of the current hour (InterpolationValue). Everything the sky,
// the time cycle and the effects need is derived from those two types.
// Not ported: rain splashes / haze particles (no FX system or collision yet),
// heat-haze post FX, tunnels, underwater, weather audio (thunder) and pad shake.
namespace weather {

enum Type : s16 {
	UNDEFINED = -1,
	EXTRASUNNY_LA = 0,
	SUNNY_LA = 1,
	EXTRASUNNY_SMOG_LA = 2,
	SUNNY_SMOG_LA = 3,
	CLOUDY_LA = 4,
	SUNNY_SF = 5,
	EXTRASUNNY_SF = 6,
	CLOUDY_SF = 7,
	RAINY_SF = 8,
	FOGGY_SF = 9,
	SUNNY_VEGAS = 10,
	EXTRASUNNY_VEGAS = 11,
	CLOUDY_VEGAS = 12,
	EXTRASUNNY_COUNTRYSIDE = 13,
	SUNNY_COUNTRYSIDE = 14,
	CLOUDY_COUNTRYSIDE = 15,
	RAINY_COUNTRYSIDE = 16,
	EXTRASUNNY_DESERT = 17,
	SUNNY_DESERT = 18,
	SANDSTORM_DESERT = 19,
	UNDERWATER = 20,
	EXTRACOLOURS_1 = 21,
	EXTRACOLOURS_2 = 22,
	NUM_WEATHERS,
	NUM_PLAYABLE = UNDERWATER   // the types a list (or the debug UI) can pick
};

enum Region : s16 { REGION_DEFAULT, REGION_LA, REGION_SF, REGION_LV, REGION_DESERT };

// The CWeather statics this port keeps.
struct State {
	Type oldType, newType, forcedType;
	Region region;
	u32 typeInList;
	float interpolation;      // InterpolationValue, 0..1 through the hour
	float rain, sandstorm;
	float cloudCoverage, foggyness, foggynessSF, extraSunnyness;
	float rainbow, sunGlare, heatHaze, wetRoads;
	float wind, windClipped, windDir[3];
	float trafficLightsBrightness, headLightsSpectrum;
	u32 currentRainParticleStrength;
	bool lightningFlash, lightningBurst;
	u32 lightningStart, lightningDuration, lightningFlashLastChange;
};

extern State g;

void init();                              // CWeather::Init (new game)
void update(float camX, float camY, float camZ);  // once per frame in game
void forceWeather(Type t);                // next hour switches to t
void forceWeatherNow(Type t);             // old = new = t immediately
void releaseWeather();                    // back to the region's list
void setToAppropriateTypeNow(float x, float y);
bool isRainy();                           // Rain >= 0.2
bool isExtraSunny(Type t);
Region findRegion(float x, float y);
const char* name(Type t);                 // short debug names
const char* regionName(Region r);
// LightningFlash forced on for a few frames (debug UI test button).
void debugFlash();

}
