#include "game/loadscreen.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

#include "assets/texture.h"
#include "game/menu/menu.h"
#include "game/sfx.h"
#include "game/streaming.h"
#include "hud/hud.h"
#include "platform/input.h"
#include "platform/timing.h"
#include "render/sprite2d.h"

namespace loadscreen {

// LoadSplashes on PC grabs loadscs.txd and shuffles the splash order; on DS each
// loadscN.txd is a single texture, so we just pick one at random. The boot
// title uses title_pc_US from loadscs.txd (eDisplayedSplash::COPYRIGHT).
static constexpr int NUM_SPLASHES = 15;      // loadsc0..14
static constexpr u32 MIN_VBLANKS = 90;       // minimum time on screen (~1.5s)
static constexpr u32 TITLE_VBLANKS = 120;    // title screen hold (~2s, skippable)
static constexpr u32 FADE_VBLANKS = 15;      // fade in/out (~0.25s)
static const sprite::RGBA BAR_COLOR{ 225, 225, 225, 255 }; // HUD_COLOUR_LIGHT_GRAY

enum class Phase : uint8_t { FADE_IN, LOADING, HOLD, FADE_OUT };
enum class Mode : uint8_t { TITLE, LOAD };

static tex::Texture* s_splash;
static float s_progress;
static u32 s_t0, s_fadeT0;
static int s_step;
static Phase s_phase;
static Mode s_mode;

static void* readFile(const char* path) {
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

// The init steps the bar covers: the real assets the in-game state uses. The
// first one frees the frontend VRAM so the splash (and later the world) fits;
// heavier loads (IDE/IPL tables, the streaming grid) slot in here later.
static void stepFreeFrontend() {
	FrontEndMenuManager.unloadTextures();
	FrontEndMenuManager.m_bTexturesLoaded = false;
}
static void stepSplash() {
	char name[16], path[64];
	snprintf(name, sizeof name, "loadsc%d", rand() % NUM_SPLASHES);
	snprintf(path, sizeof path, "nitro:/models/txd/%s.txd", name);
	void* blob = readFile(path);
	if (blob) {
		s_splash = tex::loadFromTxd(blob, name);
		free(blob);
	}
	hud::log("loadscreen: %s %s", name, s_splash ? "ok" : "FAILED");
}
static void stepModels() {
	bool ok = streaming::g_models.open("nitro:/models/gta3.dspk");
	hud::log("gta3.dspk: %u entries", ok ? (unsigned)streaming::g_models.count() : 0u);
}
static void stepPlayer() { streaming::g_player.open("nitro:/models/player.dspk"); }
static void stepAnim() { streaming::g_anim.open("nitro:/anim/anim.dspk"); }
static void stepPedIfp() {  // base ped animations, meant to stay resident
	streaming::g_pedIfp = readFile("nitro:/anim/ped.ifp");
}
static void stepTune() {    // ~200ms decode: runs under the bar, not on New Game press
	sfx::tuneStart();
}

static void (*const s_steps[])() = {
	stepFreeFrontend,
	stepSplash,
	stepModels,
	stepPlayer,
	stepAnim,
	stepPedIfp,
	stepTune,
};
static constexpr int NUM_STEPS = sizeof(s_steps) / sizeof(s_steps[0]);

void beginTitle() {
	s_mode = Mode::TITLE;
	s_splash = nullptr;
	s_phase = Phase::FADE_IN;
	s_progress = 0.0f;
	s_t0 = timing::vblanks();
	void* blob = readFile("nitro:/models/txd/loadscs.txd");
	if (blob) {
		s_splash = tex::loadFromTxd(blob, "title_pc_US");
		free(blob);
	}
	hud::log("title: %s", s_splash ? "ok" : "FAILED");
}

void begin() {
	srand(timing::vblanks() * 1103515245u + timing::frameUs());
	s_mode = Mode::LOAD;
	s_splash = nullptr;
	s_phase = Phase::FADE_IN;
	s_step = 0;
	s_progress = 0.0f;
	s_t0 = timing::vblanks();
}

bool update() {
	const u32 now = timing::vblanks();
	if (s_mode == Mode::TITLE) {
		if (s_phase == Phase::FADE_IN && now - s_t0 >= FADE_VBLANKS)
			s_phase = Phase::HOLD;
		if (s_phase == Phase::HOLD && (now - s_t0 >= TITLE_VBLANKS || input::get().down)) {
			s_phase = Phase::FADE_OUT;
			s_fadeT0 = now;
		}
		return s_phase == Phase::FADE_OUT && now - s_fadeT0 >= FADE_VBLANKS;
	}

	if (s_step < NUM_STEPS) {
		s_steps[s_step++]();
	}
	if (s_phase == Phase::FADE_IN && now - s_t0 >= FADE_VBLANKS)
		s_phase = Phase::LOADING;

	// the bar follows whichever is slower: real steps or the minimum on-screen
	// time (keeps the artwork up for a moment like a console load would)
	const float stepF = (float)s_step / NUM_STEPS;
	const float timeF = std::min((float)(now - s_t0) / MIN_VBLANKS, 1.0f);
	s_progress = 100.0f * std::min(stepF, timeF);

	if (s_phase != Phase::FADE_OUT && s_step == NUM_STEPS && s_progress >= 100.0f) {
		s_phase = Phase::FADE_OUT;
		s_fadeT0 = now;
	}
	return s_phase == Phase::FADE_OUT && now - s_fadeT0 >= FADE_VBLANKS;
}

// CSprite2d::DrawBarChart: filled part in `color`, rest in color/2, black border
static void drawBar(float pct) {
	const float x = 50.0f, y = 480.0f - 40.0f, w = 180.0f, h = 10.0f, b = 2.0f;
	const float currX = x + w * std::clamp(pct, 0.0f, 100.0f) / 100.0f;
	sprite::rect({ x, y, currX, y + h }, BAR_COLOR);
	sprite::rect({ currX, y, x + w, y + h }, { 112, 112, 112, 255 });
	sprite::rect({ x, y, x + w, y + b }, { 0, 0, 0, 255 });
	sprite::rect({ x, y + h - b, x + w, y + h }, { 0, 0, 0, 255 });
	sprite::rect({ x, y, x + b, y + h }, { 0, 0, 0, 255 });
	sprite::rect({ x + w - b, y, x + w, y + h }, { 0, 0, 0, 255 });
}

void draw() {
	const sprite::Rect full{ -5.0f, -5.0f, 645.0f, 485.0f };
	sprite::beginFrame();
	if (s_splash) sprite::draw(s_splash, full, { 255, 255, 255, 255 });
	else sprite::rect(full, { 0, 0, 0, 255 });
	if (s_progress > 0.0f && s_progress < 100.0f)
		drawBar(s_progress);

	float a = 0.0f;
	if (s_phase == Phase::FADE_IN)
		a = 1.0f - (float)(timing::vblanks() - s_t0) / FADE_VBLANKS;
	else if (s_phase == Phase::FADE_OUT)
		a = (float)(timing::vblanks() - s_fadeT0) / FADE_VBLANKS;
	if (a > 0.0f) {
		sprite::alpha((u8)(31.0f * std::min(a, 1.0f)));
		sprite::rect(full, { 0, 0, 0, 255 });
		sprite::alpha(31);
	}
	sprite::endFrame();
}

void end() {
	tex::unload(s_splash);
	s_splash = nullptr;
	sfx::tuneStop();
}

}
