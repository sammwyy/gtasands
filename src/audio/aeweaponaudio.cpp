#include "audio/aeweaponaudio.h"

#include <math.h>
#include <stdint.h>

#include "audio/aeaudio.h"
#include "audio/aebankloader.h"
#include "audio/aesoundmanager.h"
#include "audio/audioengine.h"

namespace aeweapon {

using namespace ae;
using aesound::Params;

// CAEWeaponAudioEntity::WeaponFire's PlayGunSounds arguments per eWeaponType
struct Gun {
	uint8_t type;
	int16_t dry, lows, mainL, mainR, tail;
	float volume, speed, tailSpeed;
	int16_t reloadA, reloadB;   // WeaponReload
};
static const Gun k_guns[] = {
	{ 22, SND_WEAPONS_EAGLE_DRYFIRE, SND_WEAPONS_EAGLE_LOWS, SND_WEAPONS_EAGLE_SHOT_L, SND_WEAPONS_EAGLE_SHOT_R, SND_WEAPONS_EAGLE_TAIL_L, 0.0f, 1.41421f, 1.0f, 55, 66 },
	{ 23, SND_WEAPONS_SILENCED_DRYFIRE, SND_WEAPONS_SILENCED_LOWS, SND_WEAPONS_SILENCED_SHOT_L, SND_WEAPONS_SILENCED_SHOT_L, SND_WEAPONS_UNK, -7.0f, 1.0f, 1.0f, 55, 66 },
	{ 24, SND_WEAPONS_EAGLE_DRYFIRE, SND_WEAPONS_EAGLE_LOWS, SND_WEAPONS_EAGLE_SHOT_L, SND_WEAPONS_EAGLE_SHOT_R, SND_WEAPONS_EAGLE_TAIL_L, 0.0f, 0.94387001f, 1.0f, 55, 51 },
	{ 25, SND_WEAPONS_SHOTGUN_DRYFIRE, SND_WEAPONS_SHOTGUN_LOWS, SND_WEAPONS_SHOTGUN_SHOT_L, SND_WEAPONS_SHOTGUN_SHOT_R, SND_WEAPONS_SHOTGUN_TAIL_R, 0.0f, 1.0f, 1.0f, 72, 71 },
	{ 26, SND_WEAPONS_SHOTGUN_DRYFIRE, SND_WEAPONS_SHOTGUN_LOWS, SND_WEAPONS_SHOTGUN_SHOT_L, SND_WEAPONS_SHOTGUN_SHOT_R, SND_WEAPONS_SHOTGUN_TAIL_R, 0.0f, 0.79369998f, 0.93f, 70, 69 },
	{ 27, SND_WEAPONS_SHOTGUN_DRYFIRE, SND_WEAPONS_SHOTGUN_LOWS, SND_WEAPONS_SHOTGUN_SHOT_L, SND_WEAPONS_SHOTGUN_SHOT_R, SND_WEAPONS_SHOTGUN_TAIL_R, 0.0f, 1.0f, 1.0f, 72, 71 },
	{ 28, SND_WEAPONS_9MM1DRY, SND_WEAPONS_9MM1LOWS, SND_WEAPONS_9MM1SHOT_L, SND_WEAPONS_9MM1SHOT_R, SND_WEAPONS_9MM1TAIL, 0.0f, 1.0f, 1.0f, 85, 84 },
	{ 29, SND_WEAPONS_9MM1DRY, SND_WEAPONS_9MM1LOWS, SND_WEAPONS_MP5SHOT_L, SND_WEAPONS_MP5SHOT_R, SND_WEAPONS_9MM1TAIL, 0.0f, 1.0f, 1.0f, 85, 84 },
	{ 30, SND_WEAPONS_AK_DRYFIRE, SND_WEAPONS_EAGLE_LOWS, SND_WEAPONS_AK_SHOT_L, SND_WEAPONS_AK_SHOT_R, SND_WEAPONS_AK_TAIL_L, 0.0f, 1.0f, 1.0f, 32, 31 },
	{ 31, SND_WEAPONS_AK_DRYFIRE, SND_WEAPONS_EAGLE_LOWS, SND_WEAPONS_AK_SHOT_L, SND_WEAPONS_AK_SHOT_R, SND_WEAPONS_AK_TAIL_L, 0.0f, 1.0f, 1.0f, 32, 31 },
	{ 32, SND_WEAPONS_9MM1DRY, SND_WEAPONS_9MM1LOWS, SND_WEAPONS_9MM1SHOT_L, SND_WEAPONS_9MM1SHOT_R, SND_WEAPONS_9MM1TAIL, 0.0f, 1.25992f, 1.0f, 85, 84 },
	{ 33, SND_WEAPONS_EAGLE_DRYFIRE, SND_WEAPONS_EAGLE_LOWS, SND_WEAPONS_SNIPER_SHOT_L, SND_WEAPONS_SNIPER_SHOT_R, SND_WEAPONS_SHOTGUN_TAIL_R, 0.0f, 0.89f, 1.0f, 32, -1 },
	{ 34, SND_WEAPONS_EAGLE_DRYFIRE, SND_WEAPONS_EAGLE_LOWS, SND_WEAPONS_SNIPER_SHOT_L, SND_WEAPONS_SNIPER_SHOT_R, SND_WEAPONS_SHOTGUN_TAIL_R, 0.0f, 1.0f, 1.0f, 55, 32 },
	{ 40, SND_WEAPONS_DETONATOR_BEEP, SND_WEAPONS_UNK, SND_WEAPONS_UNK, SND_WEAPONS_UNK, SND_WEAPONS_UNK, -14.0f, 1.0f, 1.0f, -1, -1 },
};

static uint32_t s_lastFireMs;   // m_LastGunFireTimeMs

static const Gun* gunFor(int type) {
	for (const Gun& g : k_guns)
		if (g.type == type) return &g;
	return nullptr;
}

void prepare(int weaponType) {
	const Gun* g = gunFor(weaponType);
	if (!g) return;
	const int16_t ids[] = { g->dry, g->lows, g->mainL, g->mainR, g->tail, g->reloadA, g->reloadB };
	for (int16_t id : ids)
		if (id >= 0) aebank::find(BANK_GENRL_WEAPONS, (uint16_t)id);
}

static void play(int16_t id, float x, float y, float z, float volume, float rollOff, float speed, uint16_t flags) {
	if (id < 0) return;
	Params p;
	p.bank = BANK_GENRL_WEAPONS;
	p.id = (uint16_t)id;
	p.x = x; p.y = y; p.z = z;
	p.volume = volume;
	p.rollOff = rollOff;
	p.speed = speed;
	p.flags = flags;
	p.owner = aesound::OWNER_WEAPON;
	p.event = AE_WEAPON_FIRE;
	aesound::play(p);
}

void fire(int weaponType, float x, float y, float z) {
	const Gun* g = gunFor(weaponType);
	if (!g) return;
	prepare(weaponType);   // a miss queues the read; this shot goes without that part
	const uint32_t now = timeMs();
	if (now < s_lastFireMs + 25) return;
	s_lastFireMs = now;
	audio::stats().shots++;

	float base = defaultVolume(AE_WEAPON_FIRE) + g->volume;
	const float roll = 1.0f, speed = g->speed;
	play(g->dry, x, y, z, base, roll * (2.0f / 3.0f), speed, 0);
	play(g->lows, x, y, z, base, roll * 0.9f, speed, 0);

	// near the camera the stereo shot pair plays front-end (both ears),
	// farther away the positional copy takes over (12 m crossfade)
	const float d = aesound::distanceToListener(x, y, z) / (roll * 1.25f);
	float front = VOLUME_SILENCE;
	if (d <= 5.0f / roll) {
		base -= 3.0f;
		front = base + aesound::distanceAttenuation(d);
	} else if (d < 12.0f / roll) {
		const float t = (12.0f / roll - d) / (12.0f / roll - 5.0f / roll);
		front = base + aesound::distanceAttenuation(d) + log10f(t * 0.70710678f) * 20.0f;
		base += log10f((1.0f - t) * 0.2929f + 0.70710678f) * 20.0f;
	}
	const float mainSpeed = (randFloat(-0.02f, 0.02f) + 1.0f) * speed;
	const uint16_t fe = aesound::SOUND_FORCED_FRONT | aesound::SOUND_FRONT_END;
	for (int side = 0; side < 2; side++) {
		const int16_t id = side ? g->mainR : g->mainL;
		play(id, side ? 1.0f : -1.0f, 0.0f, 0.0f, front, roll * 1.25f, mainSpeed, fe);
		// the positional copy is what carries at range; next to the camera
		// it is 3 dB under the front-end one and only costs a channel
		if (d > 5.0f / roll) play(id, x, y, z, base, roll * 1.25f, mainSpeed, 0);
	}
	if (g->tail >= 0) {
		const float tailRoll = roll * 3.5f;
		const float tailVol = aesound::distanceAttenuation(aesound::distanceToListener(x, y, z) / tailRoll) + base - 20.0f;
		float a = 1.0f, b = 1.1892101f;
		if (chance(0.5f)) { const float t = a; a = b; b = t; }
		play(g->tail, -1.0f, 0.0f, 0.0f, tailVol, tailRoll, g->tailSpeed * a, fe | aesound::SOUND_ROLLED_OFF);
		play(g->tail, 1.0f, 0.0f, 0.0f, tailVol, tailRoll, g->tailSpeed * b, fe | aesound::SOUND_ROLLED_OFF);
	}
}

void reload(int weaponType, bool second, float x, float y, float z) {
	const Gun* g = gunFor(weaponType);
	if (!g) return;
	const int16_t id = second ? g->reloadB : g->reloadA;
	if (id < 0 || aebank::find(BANK_GENRL_WEAPONS, (uint16_t)id) < 0) return;
	Params p;
	p.bank = BANK_GENRL_WEAPONS;
	p.id = (uint16_t)id;
	p.x = x; p.y = y; p.z = z;
	p.volume = defaultVolume(second ? AE_WEAPON_RELOAD_B : AE_WEAPON_RELOAD_A) + (weaponType == 33 ? -6.0f : 0.0f);
	p.rollOff = 2.0f / 3.0f;
	p.owner = aesound::OWNER_WEAPON;
	p.event = second ? AE_WEAPON_RELOAD_B : AE_WEAPON_RELOAD_A;
	aesound::play(p);
}

}
