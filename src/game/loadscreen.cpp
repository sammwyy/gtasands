#include "game/loadscreen.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include "assets/texture.h"
#include "assets/txdfile.h"
#include "audio/audioengine.h"
#include "game/animstore.h"
#include "game/cj.h"
#include "game/collision.h"
#include "game/hud/chud.h"
#include "game/hud/radar.h"
#include "game/menu/menu.h"
#include "game/player.h"
#include "game/sfx.h"
#include "game/streaming.h"
#include "game/timecycle.h"
#include "game/vehicle.h"
#include "game/vehicledata.h"
#include "game/weather.h"
#include "game/world.h"
#include "game/zones.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/input.h"
#include "platform/timing.h"
#include "render/sky.h"
#include "render/sprite2d.h"

namespace loadscreen {

// LoadSplashes on PC grabs loadscs.txd and shuffles the splash order; on DS each
// loadscN.txd is a single texture, so we just pick one at random. The boot
// title uses title_pc_US from loadscs.txd (eDisplayedSplash::COPYRIGHT).
static constexpr int NUM_SPLASHES = 15;      // loadsc0..14
static constexpr u32 MIN_VBLANKS = 90;       // minimum time on screen (~1.5s)
static constexpr u32 TITLE_VBLANKS = 120;    // title screen hold (~2s, skippable)
static constexpr u32 FADE_VBLANKS = 15;      // fade in/out (~0.25s)
// CPU time the init jobs may take per frame. The rest of the frame draws the
// splash + bar, so the loading screen keeps animating (and the music keeps
// streaming) instead of freezing while the game loads.
static constexpr u32 WORK_US = 10000;
static const sprite::RGBA BAR_COLOR{ 225, 225, 225, 255 }; // HUD_COLOUR_LIGHT_GRAY

enum class Phase : uint8_t { FADE_IN, LOADING, HOLD, FADE_OUT };
enum class Mode : uint8_t { TITLE, LOAD };

static tex::Texture* s_splash;
static float s_progress;
static u32 s_t0, s_fadeT0;
static int s_step;
static Phase s_phase;
static Mode s_mode;

static tex::Texture* loadSplash(const char* path, const char* name) {
	TxdFile txd;
	if (!txd.open(path)) return nullptr;
	mem::Scratch scratch;
	return tex::load(txd.load(name, scratch), tex::GROUP_FRONTEND);
}

// --- init jobs ----------------------------------------------------------------
// Each job does a bounded slice of work and returns true when it is finished;
// update() keeps calling jobs until the frame's WORK_US is spent. Order is
// what the in-game state needs: VRAM first (frontend out, splash in), music,
// the streaming packs, the resident anim set, the HUD, CJ, the radar window.

static bool jobFreeFrontend() {
	FrontEndMenuManager.unloadTextures();
	FrontEndMenuManager.m_bTexturesLoaded = false;
	return true;
}
static bool jobSplash() {
	char name[16], path[64];
	snprintf(name, sizeof name, "loadsc%d", rand() % NUM_SPLASHES);
	snprintf(path, sizeof path, "nitro:/models/txd/%s.txd", name);
	s_splash = loadSplash(path, name);
	hud::log("loadscreen: %s %s", name, s_splash ? "ok" : "FAILED");
	return true;
}
static bool jobTune() { return sfx::tuneStep(); }
static bool jobOpenModels() {
	const bool ok = streaming::g_models.open("nitro:/models/gta3.dspk");
	hud::log("gta3.dspk: %u entries", ok ? (unsigned)streaming::g_models.count() : 0u);
	return true;
}
// gta3's TOC is 256 KB: only a sparse index is built, 16 KB per slice
static bool jobIndexModels() { return !streaming::g_models.isOpen() || streaming::g_models.indexStep(16); }
static bool jobOpenPlayer() { streaming::g_player.open("nitro:/models/player.dspk"); return true; }
static bool jobOpenAnim() { streaming::g_anim.open("nitro:/anim/anim.dspk"); return true; }
static bool jobPedBlock() {
	if (animstore::findBlock("ped") == animstore::NO_BLOCK &&
	    animstore::openFile("nitro:/anim/ped.ifp") == animstore::NO_BLOCK)
		hud::log("\x1b[31mped.ifp MISSING\x1b[39m");
	return true;
}
static bool jobHud() { return chud::initStep(); }
static bool jobZones() { zones::preload(); return true; }
static bool jobCj() { return cj::initStep(); }
static bool jobPlayer() { player::init(); return true; }
static bool jobRadar() { radar::prefetch(); return true; }
static bool jobTimeCycle() { return timecycle::loadStep(); }   // one weather (8 lines) per slice
static bool jobSky() {
	sky::loadStep();
	timecycle::update(player::g.x, player::g.y, player::g.z);   // first in-game frame has colours
	return true;
}
static bool jobWeather() { weather::init(); return true; }
static bool jobWorld() { return world::initStep(); }   // open world.dwld + LoadScene around the player
static bool jobCol() {                                 // collision index + records around the player
	if (!col::initStep()) return false;
	player::placeOnGround();
	return true;
}
static bool jobVehicles() { veh::init(); return vehdata::initStep(); }   // vehicles.dveh tables
static bool jobAudio() { return audio::initGameStep(); }   // surfaces, audio zones, in-game sounds
static bool jobReport() {
	char buf[96];
	mem::summary(buf, sizeof buf);
	hud::log("%s", buf);
	return true;
}

static bool (*const s_jobs[])() = {
	jobFreeFrontend,
	jobSplash,
	jobTune,
	jobOpenModels,
	jobIndexModels,
	jobOpenPlayer,
	jobOpenAnim,
	jobPedBlock,
	jobCj,
	jobPlayer,
	jobWeather,
	jobTimeCycle,
	jobSky,
	jobHud,
	jobZones,
	jobRadar,
	jobWorld,
	jobCol,
	jobVehicles,
	jobAudio,
	jobReport,
};
static constexpr int NUM_JOBS = sizeof(s_jobs) / sizeof(s_jobs[0]);

void beginTitle() {
	s_mode = Mode::TITLE;
	s_phase = Phase::FADE_IN;
	s_progress = 0.0f;
	s_t0 = timing::vblanks();
	s_splash = loadSplash("nitro:/models/txd/loadscs.txd", "title_pc_US");
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

	// run jobs until this frame's budget is spent (at least one slice/frame)
	const u32 start = timing::ticks();
	while (s_step < NUM_JOBS) {
		if (s_jobs[s_step]()) s_step++;
		if (timing::usSince(start) >= WORK_US) break;
	}
	if (s_phase == Phase::FADE_IN && now - s_t0 >= FADE_VBLANKS)
		s_phase = Phase::LOADING;

	// the bar follows whichever is slower: real jobs or the minimum on-screen
	// time (keeps the artwork up for a moment like a console load would)
	const float stepF = (float)s_step / NUM_JOBS;
	const float timeF = std::min((float)(now - s_t0) / MIN_VBLANKS, 1.0f);
	s_progress = 100.0f * std::min(stepF, timeF);

	if (s_phase != Phase::FADE_OUT && s_step == NUM_JOBS && s_progress >= 100.0f) {
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
