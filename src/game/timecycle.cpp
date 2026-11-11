#include "game/timecycle.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "game/player.h"
#include "game/weather.h"
#include "hud/hud.h"
#include "mem/mem.h"

namespace timecycle {

// Blends treat everything before farClip as a byte array.
static constexpr int NUM_BYTES = offsetof(Colours, farClip);
static_assert(sizeof(Colours) == NUM_BYTES + 4, "Colours layout");

static Colours* s_tab;          // [hour * NUM_WEATHERS + weather], PERM
static Colours s_cur;
static float s_toSun[3] = { 0.0f, 0.0f, 1.0f };
static u8 s_grey[3];
static FILE* s_file;
static int s_loadWeather;       // next weather to parse; NUM_WEATHERS = done

static inline const Colours& sample(int h, int w) { return s_tab[h * weather::NUM_WEATHERS + w]; }

// ---- loading ----------------------------------------------------------------

// The original sscanf's into uninitialised stack locals, so a short line (the
// stock file has one, see TimeCycle.cpp) keeps the previous line's values.
// Static locals reproduce that.
static void parseLine(const char* line, Colours& c) {
	static int ambR, ambG, ambB, ambObjR, ambObjG, ambObjB, dirR, dirG, dirB;
	static int skyTopR, skyTopG, skyTopB, skyBotR, skyBotG, skyBotB;
	static int sunCoreR, sunCoreG, sunCoreB, sunCoronaR, sunCoronaG, sunCoronaB;
	static float sunSize, spriteSize, spriteBrightness;
	static int shadow, lightShadow, poleShadow;
	static float farClip, fogStart, lightOnGround;
	static int lowR, lowG, lowB, botR, botG, botB;
	static float waterR, waterG, waterB, waterA;
	static float fx1A, fx1R, fx1G, fx1B, fx2A, fx2R, fx2G, fx2B, cloudAlpha;
	static int highLight, waterFog;
	static float dirMult;
	const int n = sscanf(line,
		"%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d "
		"%f %f %f %d %d %d %f %f %f %d %d %d %d %d %d %f %f %f %f "
		"%f %f %f %f %f %f %f %f %f %d %d %f",
		&ambR, &ambG, &ambB, &ambObjR, &ambObjG, &ambObjB, &dirR, &dirG, &dirB,
		&skyTopR, &skyTopG, &skyTopB, &skyBotR, &skyBotG, &skyBotB,
		&sunCoreR, &sunCoreG, &sunCoreB, &sunCoronaR, &sunCoronaG, &sunCoronaB,
		&sunSize, &spriteSize, &spriteBrightness, &shadow, &lightShadow, &poleShadow,
		&farClip, &fogStart, &lightOnGround, &lowR, &lowG, &lowB, &botR, &botG, &botB,
		&waterR, &waterG, &waterB, &waterA, &fx1A, &fx1R, &fx1G, &fx1B, &fx2A, &fx2R, &fx2G, &fx2B,
		&cloudAlpha, &highLight, &waterFog, &dirMult);
	if (n < 51) hud::log("\x1b[33mtimecyc: short line (%d)\x1b[39m", n);
	c = Colours{
		(u8)ambR, (u8)ambG, (u8)ambB, (u8)ambObjR, (u8)ambObjG, (u8)ambObjB,
		(u8)skyTopR, (u8)skyTopG, (u8)skyTopB, (u8)skyBotR, (u8)skyBotG, (u8)skyBotB,
		(u8)sunCoreR, (u8)sunCoreG, (u8)sunCoreB, (u8)sunCoronaR, (u8)sunCoronaG, (u8)sunCoronaB,
		(u8)(int)(sunSize * 10.0f + 0.5f), (u8)(int)(spriteSize * 10.0f + 0.5f),
		(u8)(int)(spriteBrightness * 10.0f + 0.5f),
		(u8)shadow, (u8)lightShadow, (u8)poleShadow, (u8)(int)(lightOnGround * 10.0f + 0.5f),
		(u8)lowR, (u8)lowG, (u8)lowB, (u8)botR, (u8)botG, (u8)botB,
		(u8)waterR, (u8)waterG, (u8)waterB, (u8)waterA,
		(u8)fx1R, (u8)fx1G, (u8)fx1B, (u8)(fx1A * 2.0f), (u8)fx2R, (u8)fx2G, (u8)fx2B, (u8)(fx2A * 2.0f),
		(u8)cloudAlpha, (u8)highLight, (u8)waterFog,
		(u8)((u8)dirMult * 100), // sic: the original truncates dirMult before scaling
		0, (s16)farClip, (s16)fogStart };
	(void)dirR; (void)dirG; (void)dirB;
}

bool loaded() { return s_tab && s_loadWeather >= weather::NUM_WEATHERS; }

bool loadStep() {
	if (loaded()) return true;
	if (!s_tab) {
		s_tab = (Colours*)mem::perm(sizeof(Colours) * NUM_HOURS * weather::NUM_WEATHERS, mem::TAG_WORLD);
		if (!s_tab) return true;
		memset(s_tab, 0, sizeof(Colours) * NUM_HOURS * weather::NUM_WEATHERS);
	}
	if (!s_file && !(s_file = fopen("game:/data/timecyc.dat", "r"))) {
		hud::log("\x1b[31mtimecyc.dat missing\x1b[39m");
		s_loadWeather = weather::NUM_WEATHERS;   // all-zero table: black sky, no fog
		return true;
	}
	char line[384];
	for (int h = 0; h < NUM_HOURS; h++) {
		bool got = false;
		while (fgets(line, sizeof line, s_file))
			if (line[0] != '/' && line[0] != '\r' && line[0] != '\n' && line[0]) { got = true; break; }
		if (!got) break;
		parseLine(line, s_tab[h * weather::NUM_WEATHERS + s_loadWeather]);
	}
	if (++s_loadWeather < weather::NUM_WEATHERS) return false;
	fclose(s_file);
	s_file = nullptr;
	hud::log("timecyc: %d weathers x %d hours", (int)weather::NUM_WEATHERS, NUM_HOURS);
	return true;
}

// ---- CalcColoursForPoint ----------------------------------------------------

// CColourSet::Interpolate with weights b/256 (a gets 256-b).
static void blend(Colours& out, const Colours& a, const Colours& b, int t) {
	const u8* pa = (const u8*)&a;
	const u8* pb = (const u8*)&b;
	u8* po = (u8*)&out;
	for (int i = 0; i < NUM_BYTES; i++) po[i] = (u8)(pa[i] + (((int)pb[i] - (int)pa[i]) * t >> 8));
	out.farClip = (s16)(a.farClip + ((b.farClip - a.farClip) * t >> 8));
	out.fogStart = (s16)(a.fogStart + ((b.fogStart - a.fogStart) * t >> 8));
}

// One weather between two time samples; the LA smog types fade into their
// clear counterpart with camera altitude (TimeCycle.cpp 0x5606B7).
static void weatherSample(Colours& out, int w, int i0, int i1, int timeT, int smogT) {
	blend(out, sample(i0, w), sample(i1, w), timeT);
	const int base = w == weather::EXTRASUNNY_SMOG_LA ? weather::EXTRASUNNY_LA
	               : w == weather::SUNNY_SMOG_LA ? weather::SUNNY_LA : -1;
	if (base >= 0 && smogT > 0) {
		Colours clear;
		blend(clear, sample(i0, base), sample(i1, base), timeT);
		blend(out, out, clear, smogT);
	}
}

void update(float camX, float camY, float camZ) {
	(void)camX; (void)camY;   // timecycle boxes would be looked up here
	if (!loaded()) return;
	static const u8 k_samples[NUM_HOURS + 1] = { 0, 5, 6, 7, 12, 19, 20, 22, 24 };
	static const u8 k_grey[NUM_HOURS] = { 30, 30, 30, 50, 60, 60, 50, 35 };

	float hours = player::minutesToday() / 60.0f;
	if (hours > 23.999f) hours = 23.999f;
	int cur = 0;
	while (hours >= (float)k_samples[cur + 1]) cur++;
	const int next = (cur + 1) % NUM_HOURS;
	const int timeT = (int)((hours - k_samples[cur]) * 256.0f / (float)(k_samples[cur + 1] - k_samples[cur]));
	float f = (camZ - 20.0f) / 200.0f;
	const int smogT = (int)((f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f) * 256.0f);

	Colours a, b;
	weatherSample(a, weather::g.oldType, cur, next, timeT, smogT);
	weatherSample(b, weather::g.newType, cur, next, timeT, smogT);
	blend(s_cur, a, b, (int)(weather::g.interpolation * 256.0f));

	// m_VectorToSun: one full turn per day, noon overhead
	const float ang = player::minutesToday() * (3.14159265f / 720.0f);
	float v[3] = { 0.7f + sinf(ang), -0.7f, 0.2f - cosf(ang) };
	const float inv = 1.0f / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	for (int i = 0; i < 3; i++) s_toSun[i] = v[i] * inv;

	const u8 grey = (u8)(k_grey[cur] + ((k_grey[next] - k_grey[cur]) * timeT >> 8));
	s_grey[0] = s_grey[1] = s_grey[2] = grey;

	// app_game.cpp: a lightning flash turns the horizon/fog white
	if (weather::g.lightningFlash) s_cur.skyBotR = s_cur.skyBotG = s_cur.skyBotB = 255;
}

const Colours& current() { return s_cur; }
const float* vectorToSun() { return s_toSun; }
void belowHorizonGrey(u8& r, u8& g, u8& b) { r = s_grey[0]; g = s_grey[1]; b = s_grey[2]; }

float dayNightBalance() {
	const float m = player::minutesToday();
	if (m < 360.0f) return 1.0f;
	if (m < 420.0f) return (420.0f - m) / 60.0f;
	if (m < 1200.0f) return 0.0f;
	if (m >= 1260.0f) return 1.0f;
	return 1.0f - (1260.0f - m) / 60.0f;
}

// Collision lighting under the player's feet (tColLighting of the floor face,
// scale 0.5). Until a floor is found: a typical outdoor street, day 15 / night 5.
static float s_surfDay = 15.0f, s_surfNight = 5.0f;
// Average N.L of the (fixed) sun light over a ped seen from behind.
static constexpr float DIRECTIONAL_SHARE = 0.75f;

void setSurfaceLight(u8 light) {
	// eased: walking over faces with different lighting must not flicker
	s_surfDay += ((float)(light & 15) - s_surfDay) * 0.1f;
	s_surfNight += ((float)(light >> 4) - s_surfNight) * 0.1f;
}

void pedLight(u8& r, u8& g, u8& b) {
	if (weather::g.lightningFlash) { r = g = b = 255; return; }
	const Colours& c = s_cur;
	const float dn = dayNightBalance();
	// CPhysical::GetLightingFromCol: average ambient + contact surface brightness
	// (tColLighting::GetCurrentLighting(0.5))
	const float mult = (c.ambR + c.ambG + c.ambB) * (1.0f / (3.0f * 255.0f)) +
	                   (s_surfDay + (s_surfNight - s_surfDay) * dn) * (0.5f / 15.0f);
	const float dir = c.illumination * (DIRECTIONAL_SHARE * 255.0f / 100.0f);
	auto ch = [&](u8 amb) {
		const float v = mult * (amb + dir);
		return (u8)(v > 255.0f ? 255.0f : v);
	};
	r = ch(c.ambObjR); g = ch(c.ambObjG); b = ch(c.ambObjB);
}

void worldLight(u8& r, u8& g, u8& b) {
	if (weather::g.lightningFlash) { r = g = b = 255; return; }
	// CCustomBuildingDNPipeline: day prelit -> night prelit (dropped by the
	// pipeline, approximated as 40% of day) plus the static ambient
	const float base = 255.0f - 153.0f * dayNightBalance();
	const Colours& c = s_cur;
	auto ch = [&](u8 amb) { const float v = base + amb; return (u8)(v > 255.0f ? 255.0f : v); };
	r = ch(c.ambR); g = ch(c.ambG); b = ch(c.ambB);
}

}
