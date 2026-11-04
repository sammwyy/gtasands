#include "audio/audioengine.h"

#include <nds.h>
#include <stdio.h>

#include "audio/aeambience.h"
#include "audio/aeaudio.h"
#include "audio/aebankloader.h"
#include "audio/aehardware.h"
#include "audio/aepedaudio.h"
#include "audio/aesoundmanager.h"
#include "audio/aestream.h"
#include "audio/aeweaponaudio.h"
#include "audio/aeweatheraudio.h"
#include "audio/audiozones.h"
#include "game/menu/menu.h"
#include "game/player.h"
#include "hud/hud.h"
#include "platform/timing.h"

namespace audio {

static bool s_init, s_game;
static int s_gameStep;
static float s_listenerZ;
static Stats s_stats;

Stats& stats() { return s_stats; }


void statsLine(char* buf, int n) {
	const aesound::Stats& ss = aesound::stats();
	const aebank::Stats& bs = aebank::stats();
	const aestream::Stats& st = aestream::stats();
	snprintf(buf, n,
	         "aud: snd=%u/%u start=%lu drop=%lu stolen=%lu miss=%lu lim=%u | cache=%u %luK pin=%u q=%u fail=%u loads=%lu "
	         "evict=%lu oom=%lu | steps=%lu land=%lu swing=%lu shots=%lu thunder=%lu | rain=%.0f trk=%ld dec=%lu under=%lu | "
	         "us=%lu (mgr %lu bank %lu strm %lu) hw=%04lx",
	         ss.active, ss.physical, (unsigned long)ss.started, (unsigned long)ss.dropped, (unsigned long)ss.stolen,
	         (unsigned long)ss.missing, ss.minGain * 100u / 4096u, bs.resident, (unsigned long)(bs.bytes >> 10), bs.pinned, bs.queued, bs.failed,
	         (unsigned long)bs.loads, (unsigned long)bs.evictions, (unsigned long)bs.oom, (unsigned long)s_stats.footsteps,
	         (unsigned long)s_stats.landings, (unsigned long)s_stats.swings, (unsigned long)s_stats.shots, (unsigned long)s_stats.thunders,
	         aeweather::rainVolume(), st.trackId == ~0u ? -1L : (long)st.trackId, (unsigned long)st.decoded,
	         (unsigned long)st.underruns, (unsigned long)s_stats.serviceUs, (unsigned long)ss.serviceUs,
	         (unsigned long)bs.serviceUs, (unsigned long)st.serviceUs, (unsigned long)aehw::activeMask());
}
float listenerZ() { return s_listenerZ; }

void init() {
	if (s_init) return;
	s_init = true;
	aehw::init();
	aesound::init();
	aebank::init();
	ae::loadEventVolumes();
}

// CAudioEngine::InitialisePostLoading: everything the first in-game frame
// plays is resident before the loading screen goes away
bool initGameStep() {
	switch (s_gameStep) {
	case 0: aeped::loadSurfaceAudio(); break;
	case 1: audiozones::load(); break;
	case 2:
		aeped::preload();
		aeweather::preload();
		aeweapon::prepare(player::g.weaponType);
		break;
	case 3:
		aebank::service(6000);
		if (!aebank::idle()) return false;
		hud::log("audio: %u sounds %luK resident", (unsigned)aebank::stats().resident,
		         (unsigned long)(aebank::stats().bytes >> 10));
		break;
	default: return true;
	}
	return ++s_gameStep > 3;
}

void setGameActive(bool on) {
	if (s_game == on) return;
	s_game = on;
	if (!on) {
		aeweather::reset();
		aeambience::reset();
		aesound::cancelOwner(aesound::OWNER_PED);
		aesound::cancelOwner(aesound::OWNER_WEAPON);
		aesound::cancelOwner(aesound::OWNER_SPEECH);
	}
}

// The chase camera of renderer::draw / player::updateCamera in SA axes
// (x east, y north, z up): yaw 0 looks north, pitch > 0 looks down.
static void updateListener() {
	const float y = player::camYaw(), p = player::camPitch(), d = player::camDist();
	// libnds' table sine (angle: 32768 per turn): soft-float sinf is ~2k cycles each
	const int ay = (int)(y * (32768.0f / 6.2831853f)), ap = (int)(p * (32768.0f / 6.2831853f));
	const float k = 1.0f / 4096.0f;
	const float sy = sinLerp((s16)ay) * k, cy = cosLerp((s16)ay) * k, sp = sinLerp((s16)ap) * k, cp = cosLerp((s16)ap) * k;
	const float fwd[3] = { -cp * sy, cp * cy, -sp };
	const float right[3] = { cy, sy, 0.0f };
	const float up[3] = { -sp * sy, sp * cy, cp };
	const float target[3] = { player::g.x, player::g.y, player::viewZ() + player::CAM_LOOK_H };
	const float eye[3] = { target[0] - fwd[0] * d, target[1] - fwd[1] * d, target[2] - fwd[2] * d };
	s_listenerZ = eye[2];
	// GetPositionRelativeToCamera's offset uses the distance to the ped origin
	aesound::setListener(eye, right, fwd, up, d);
}

void service() {
	if (!s_init) return;
	const u32 t0 = timing::ticks();
	aesound::setEffectsVolume(FrontEndMenuManager.m_nSfxVolume);
	aestream::setMusicVolume(FrontEndMenuManager.m_nRadioVolume);
	const bool paused = s_game && FrontEndMenuManager.m_bMenuActive;
	aesound::setPaused(paused);
	if (s_game) {
		updateListener();
		if (!paused) {
			audiozones::update(false, player::g.x, player::g.y, s_listenerZ);
			aeweather::service();
		}
		aeambience::service(paused);
	}
	aebank::service(s_game ? 1500 : 4000);
	aesound::service();
	aestream::service();
	s_stats.serviceUs = timing::usSince(t0);
}

}
