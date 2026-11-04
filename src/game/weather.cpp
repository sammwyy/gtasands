#include "game/weather.h"

#include <math.h>
#include <stdlib.h>

#include "audio/aeweatheraudio.h"
#include "game/debugopts.h"
#include "game/player.h"
#include "game/timecycle.h"
#include "platform/timing.h"

namespace weather {

State g;

// ---- Data/Weather.def.h (stored as bytes: 320 B of rodata) -------------------
constexpr u32 LIST_SIZE = 64;
#define XS EXTRASUNNY_COUNTRYSIDE
#define SC SUNNY_COUNTRYSIDE
#define CC CLOUDY_COUNTRYSIDE
#define RC RAINY_COUNTRYSIDE
#define FS FOGGY_SF
static const u8 k_listDefault[LIST_SIZE] = {
	XS, XS, XS, XS, XS, XS, XS, XS, SC, CC, FS, FS, CC, SC, XS, SC,
	SC, XS, XS, XS, XS, SC, SC, XS, XS, XS, XS, XS, XS, XS, XS, XS,
	XS, XS, SC, SC, SC, SC, SC, SC, CC, FS, FS, CC, RC, RC, RC, RC,
	CC, SC, SC, SC, SC, XS, XS, XS, XS, XS, XS, XS, XS, XS, XS, XS,
};
#undef XS
#undef SC
#undef CC
#undef FS
#define XL EXTRASUNNY_LA
#define SL SUNNY_LA
#define XM EXTRASUNNY_SMOG_LA
#define SM SUNNY_SMOG_LA
#define CL CLOUDY_LA
static const u8 k_listLA[LIST_SIZE] = {
	XM, XM, XL, XL, XL, XM, XM, XM, SM, XL, SM, XM, XL, XL, SM, SM,
	SM, CL, CL, XL, XL, SL, SL, XL, XM, XM, XM, XM, XM, XM, XM, XM,
	XM, XM, SM, SM, SM, SM, SM, SM, XM, XM, SM, SM, XL, XL, RC, XL,
	CL, SL, SL, SL, SL, XL, XL, XM, XM, XM, XM, XM, XM, XM, XM, XM,
};
#undef XL
#undef SL
#undef XM
#undef SM
#undef CL
#undef RC
#define S SUNNY_SF
#define X EXTRASUNNY_SF
#define C CLOUDY_SF
#define R RAINY_SF
#define F FOGGY_SF
static const u8 k_listSF[LIST_SIZE] = {
	S, S, S, S, S, C, C, F, F, C, S, S, S, S, S, C,
	C, C, R, R, C, C, S, S, S, S, X, X, S, C, F, F,
	C, S, S, C, S, S, X, X, S, S, S, C, C, R, R, C,
	C, F, F, S, S, X, X, X, S, S, X, C, C, X, X, S,
};
#undef S
#undef X
#undef C
#undef R
#undef F
#define X EXTRASUNNY_VEGAS
#define S SUNNY_VEGAS
#define C CLOUDY_VEGAS
static const u8 k_listVegas[LIST_SIZE] = {
	X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
	X, X, X, S, S, S, S, S, X, X, X, X, X, X, X, X,
	X, S, X, X, X, X, S, X, X, X, X, X, X, X, X, X,
	X, X, X, X, X, X, X, X, S, S, C, C, S, C, S, S,
};
#undef X
#undef S
#undef C
#define X EXTRASUNNY_DESERT
#define S SUNNY_DESERT
#define D SANDSTORM_DESERT
static const u8 k_listDesert[LIST_SIZE] = {
	X, X, X, D, D, X, X, X, X, X, X, X, X, X, X, X,
	X, X, X, S, S, S, S, S, X, X, X, X, X, X, X, X,
	X, X, X, X, X, D, D, D, X, X, X, X, X, X, X, X,
	X, X, S, S, S, X, X, X, X, X, X, X, D, D, D, X,
};
#undef X
#undef S
#undef D

static const u8* findList() {
	switch (g.region) {
	case REGION_LA: return k_listLA;
	case REGION_SF: return k_listSF;
	case REGION_LV: return k_listVegas;
	case REGION_DESERT: return k_listDesert;
	default: return k_listDefault;
	}
}

// 0x8D5E50
static const float k_windForType[NUM_WEATHERS] = {
	0.0f, 0.25f, 0.0f, 0.2f, 0.7f, 0.25f, 0.0f, 0.7f, 1.0f, 0.0f, 0.2f, 0.0f,
	0.4f, 0.0f, 0.3f, 0.7f, 1.0f, 0.0f, 0.3f, 1.5f, 0.0f, 0.0f, 0.0f };
static const float k_windDirScales[16] = {
	1.0f, 0.5f, 1.0f, 0.2f, 0.4f, 1.0f, 1.0f, 1.0f, 1.0f, 0.8f, 0.0f, 1.0f, 1.0f, 0.7f, 1.0f, 1.0f };
static const float k_windDirOffsets[16] = {
	0.5f, -0.3f, 0.8f, 0.0f, -0.4f, -0.8f, 0.3f, -0.1f, -0.9f, -0.5f, 0.7f, 0.7f, 0.3f, 0.7f, 0.0f, -0.5f };

// ---- predicates ---------------------------------------------------------------

bool isExtraSunny(Type t) {
	return t == EXTRASUNNY_LA || t == EXTRASUNNY_SMOG_LA || t == EXTRASUNNY_COUNTRYSIDE ||
	       t == EXTRASUNNY_SF || t == EXTRASUNNY_VEGAS || t == EXTRASUNNY_DESERT;
}
static bool isSunny(Type t) {
	return t == SUNNY_LA || t == SUNNY_SMOG_LA || t == SUNNY_COUNTRYSIDE || t == SUNNY_SF ||
	       t == SUNNY_VEGAS || t == SUNNY_DESERT;
}
static bool isCloudy(Type t) { return t == CLOUDY_LA || t == CLOUDY_COUNTRYSIDE || t == CLOUDY_VEGAS || t == CLOUDY_SF; }
static bool isRainyType(Type t) { return t == RAINY_COUNTRYSIDE || t == RAINY_SF; }
static bool isFoggy(Type t) { return t == FOGGY_SF || t == SANDSTORM_DESERT; }
static bool isHeatHazy(Type t) {
	return t == SUNNY_DESERT || t == EXTRASUNNY_DESERT || t == EXTRASUNNY_VEGAS || t == EXTRASUNNY_LA;
}

template <class P> static float lerpOldToNew(P pred) {
	const float from = pred(g.oldType) ? 1.0f : 0.0f, to = pred(g.newType) ? 1.0f : 0.0f;
	return from == to ? to : from + (to - from) * g.interpolation;
}

static float stepTo(float v, float target, float step) {
	return v < target ? (v + step > target ? target : v + step) : (v - step < target ? target : v - step);
}
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static bool randomBool(float pct) { return (float)(rand() % 10000) < pct * 100.0f; }

// ---- API ----------------------------------------------------------------------

Region findRegion(float x, float y) {
	if (x > 1000.0f && y > 910.0f) return REGION_LV;
	if (x > -850.0f && x < 1000.0f && y > 1280.0f) return REGION_DESERT;
	if (x < -1430.0f && y > -580.0f && y < 1430.0f) return REGION_SF;
	if (x > 250.0f && x < 3000.0f && y > -3000.0f && y < -850.0f) return REGION_LA;
	return REGION_DEFAULT;
}

void init() {
	g = {};
	g.newType = g.oldType = EXTRASUNNY_LA;
	g.forcedType = UNDEFINED;
	g.region = REGION_DEFAULT;
}

void forceWeather(Type t) { g.forcedType = t; }
void forceWeatherNow(Type t) { g.forcedType = g.oldType = g.newType = t; }
void releaseWeather() { g.forcedType = UNDEFINED; }

void setToAppropriateTypeNow(float x, float y) {
	g.region = findRegion(x, y);
	g.forcedType = UNDEFINED;
	g.oldType = g.newType = (Type)findList()[0];
}

bool isRainy() { return g.rain >= 0.2f; }

static u32 s_debugFlash;
static u32 s_thunderAtMs;   // WhenToPlayLightningSound
void debugFlash() {
	s_debugFlash = timing::vblanks() + 10;
	g.lightningDuration = 10;
	s_thunderAtMs = timing::vblanks() * 50u / 3u + (20 - 10) * 150;
}

static u32 s_lastVbl;

void update(float camX, float camY, float camZ) {
	const u32 vbl = timing::vblanks();
	u32 dv = vbl - s_lastVbl;
	s_lastVbl = vbl;
	if (dv > 6) dv = 6;
	const float timeStep = dv * (50.0f / 60.0f);   // CTimer::ms_fTimeStep is in 1/50 s
	const u32 timeMs = vbl * 50u / 3u;

	if ((vbl & 15) == 0) g.region = findRegion(camX, camY);

	// one list step per game hour; the blend is the fraction of the hour
	const float prev = g.interpolation;
	g.interpolation = ((float)player::clockSeconds() / 60.0f + (float)player::g_clockM) / 60.0f;
	if (g.interpolation < prev) {
		g.region = findRegion(camX, camY);
		g.oldType = g.newType;
		if (g.forcedType >= 0) {
			g.newType = g.forcedType;
		} else if (camZ < 950.0f) {
			g.typeInList = (g.typeInList + 1) % LIST_SIZE;
			g.newType = (Type)findList()[g.typeInList];
		}
	}

	// lightning bursts while both weathers are rainy
	if (isRainyType(g.newType) && isRainyType(g.oldType)) {
		if (g.lightningBurst) {
			if (randomBool(9.4f)) {
				g.lightningBurst = false;
				g.lightningDuration = vbl - g.lightningStart < 20 ? vbl - g.lightningStart : 20;
				s_thunderAtMs = (20 - g.lightningDuration) * 150 + timeMs;   // light travels faster
				g.lightningFlash = false;
			} else if (timeMs - g.lightningFlashLastChange > 50) {
				const bool was = g.lightningFlash;
				g.lightningFlash = rand() & 1;
				if (g.lightningFlash != was) g.lightningFlashLastChange = timeMs;
			}
		} else if (randomBool(0.6f)) {
			g.lightningStart = vbl;
			g.lightningFlashLastChange = timeMs;
			g.lightningBurst = g.lightningFlash = true;
		} else {
			g.lightningFlash = false;
		}
	} else {
		g.lightningFlash = g.lightningBurst = false;
	}
	if (s_thunderAtMs && timeMs > s_thunderAtMs) {
		aeweather::thunder();   // m_WeatherAudioEntity.AddAudioEvent(AE_THUNDER)
		s_thunderAtMs = 0;
	}
	if (vbl < s_debugFlash) g.lightningFlash = (vbl >> 1) & 1;
	if (!dbg::opts.weatherFx) g.lightningFlash = false;

	g.wetRoads = isRainyType(g.oldType) ? (isRainyType(g.newType) ? 1.0f : 1.0f - g.interpolation)
	                                    : (isRainyType(g.newType) ? g.interpolation : 0.0f);

	const float step = timeStep * 0.005f;
	const float intensity = (float)((timeMs >> 13) % 4) * 0.1f + 0.7f;
	g.rain = stepTo(g.rain, intensity * lerpOldToNew(isRainyType), step);
	g.sandstorm = stepTo(g.sandstorm, intensity * lerpOldToNew([](Type t) { return t == SANDSTORM_DESERT; }), step);
	g.cloudCoverage = lerpOldToNew([](Type t) { return !isSunny(t) && !isExtraSunny(t); });
	g.foggyness = lerpOldToNew(isFoggy);
	g.foggynessSF = lerpOldToNew([](Type t) { return t == FOGGY_SF; });
	g.extraSunnyness = lerpOldToNew(isExtraSunny);
	g.rainbow = isCloudy(g.oldType) && isSunny(g.newType) && g.interpolation < 0.5f &&
	            player::g_clockH > 6 && player::g_clockH < 21
	            ? 1.0f - fabsf(g.interpolation - 0.25f) * 4.0f : 0.0f;
	g.sunGlare = lerpOldToNew([](Type t) { return isExtraSunny(t) || isSunny(t); });
	if (g.sunGlare > 0.0f) {
		float s = timecycle::vectorToSun()[2] * 7.0f;
		g.sunGlare *= s < 1.0f ? s : 1.0f;
		g.sunGlare = g.sunGlare < 0.0f ? 0.0f : g.sunGlare > 1.0f ? 1.0f : g.sunGlare;
		g.sunGlare *= 1.0f - (float)(rand() % 32) * 0.007f;
	}
	g.heatHaze = lerpOldToNew(isHeatHazy);

	g.wind = lerpf(k_windForType[g.oldType], k_windForType[g.newType], g.interpolation);
	g.windClipped = g.wind < 1.0f ? g.wind : 1.0f;
	g.windDir[0] = g.windDir[1] = g.windClipped * 0.7f;
	auto offs = [](u32 idx, float t) { return lerpf(k_windDirOffsets[idx % 16], k_windDirOffsets[(idx + 1) % 16], t); };
	const u32 slowIdx = (timeMs / 1024) % 16;
	const float slowT = 0.5f - cosf((float)(timeMs % 1024) / 1024.0f * 3.14159265f) * 0.5f;
	float wx = offs(slowIdx, slowT) * g.windClipped * 0.4f + g.windDir[0];
	float wy = offs(slowIdx + 3, slowT) * g.windClipped * 0.4f + g.windDir[1];
	g.windDir[2] = offs(slowIdx + 6, slowT) * g.windClipped * 0.2f;
	const float gust = (g.windClipped - 0.5f) * 0.4f;
	if (gust > 0.0f) {
		const u32 fastIdx = (timeMs / 256) % 16;
		const float fastT = (float)(timeMs % 256) / 256.0f;
		wx += offs(fastIdx, fastT) * gust;
		wy += offs(fastIdx + 3, fastT) * gust;
		g.windDir[2] += offs(fastIdx + 6, fastT) * gust;
	}
	const u32 scaleIdx = (timeMs / 2048) % 16;
	const float scaleT = 0.5f - cosf((float)(timeMs % 2048) / 2048.0f * 3.14159265f) * 0.5f;
	const float scale = lerpf(k_windDirScales[scaleIdx], k_windDirScales[(scaleIdx + 1) % 16], scaleT);
	g.windDir[0] = scale * wx;
	g.windDir[1] = scale * wy;
	g.windDir[2] *= scale;

	const unsigned h = player::g_clockH;
	g.trafficLightsBrightness = h > 20 ? 1.0f : h > 19 ? player::g_clockM / 60.0f : h > 6 ? 0.0f
	                          : h > 5 ? 1.0f - player::g_clockM / 60.0f : 1.0f;
	g.headLightsSpectrum = g.rain > g.foggyness ? g.rain : g.foggyness;
	if (g.headLightsSpectrum > 1.0f) g.headLightsSpectrum = 1.0f;

	// RenderRainStreaks' particle strength ramp (one unit per frame)
	const u32 target = (u32)(g.rain * 110.0f);
	if (g.currentRainParticleStrength < target) g.currentRainParticleStrength++;
	else if (g.currentRainParticleStrength > 0) g.currentRainParticleStrength--;
}

const char* name(Type t) {
	static const char* const k[NUM_WEATHERS] = {
		"EXSUN_LA", "SUNNY_LA", "EXSUN_SMOG", "SUNNY_SMOG", "CLOUDY_LA",
		"SUNNY_SF", "EXSUN_SF", "CLOUDY_SF", "RAINY_SF", "FOGGY_SF",
		"SUNNY_LV", "EXSUN_LV", "CLOUDY_LV",
		"EXSUN_CS", "SUNNY_CS", "CLOUDY_CS", "RAINY_CS",
		"EXSUN_DES", "SUNNY_DES", "SANDSTORM",
		"UNDERWATER", "EXTRA_1", "EXTRA_2" };
	return t >= 0 && t < NUM_WEATHERS ? k[t] : "-";
}

const char* regionName(Region r) {
	static const char* const k[] = { "DEFAULT", "LA", "SF", "LV", "DESERT" };
	return r >= 0 && r <= REGION_DESERT ? k[r] : "?";
}

}
