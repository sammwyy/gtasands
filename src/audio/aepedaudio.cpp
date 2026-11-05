#include "audio/aepedaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/aeaudio.h"
#include "audio/aebankloader.h"
#include "audio/aesoundmanager.h"
#include "audio/audioengine.h"
#include "hud/hud.h"

namespace aeped {

using aesound::Params;

// surfaud.dat columns, in order
enum SurfaceAudio : uint16_t {
	SA_CONCRETE = 1 << 0, SA_GRASS = 1 << 1, SA_SAND = 1 << 2, SA_GRAVEL = 1 << 3, SA_WOOD = 1 << 4,
	SA_WATER = 1 << 5, SA_METAL = 1 << 6, SA_LONG_GRASS = 1 << 7, SA_TILE = 1 << 8,
};
static constexpr int MAX_SURFACES = 192;   // eSurfaceType: 179 + a few
static uint16_t s_surface[MAX_SURFACES];
static int s_numSurfaces;

bool loadSurfaceAudio() {
	FILE* f = fopen("nitro:/data/surfaud.dat", "rb");
	if (!f) {
		hud::log("\x1b[33msurfaud.dat missing\x1b[39m");
		return true;
	}
	// rows are in eSurfaceType order (the names match surfinfo.dat's)
	char line[128];
	while (fgets(line, sizeof line, f) && s_numSurfaces < MAX_SURFACES) {
		char* p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\r' || *p == '\n' || !*p) continue;
		while (*p && *p != ' ' && *p != '\t') p++;   // name
		uint16_t bits = 0;
		for (int c = 0; c < 9; c++) {
			char* end;
			const long v = strtol(p, &end, 10);
			if (end == p) break;
			if (v) bits |= (uint16_t)(1u << c);
			p = end;
		}
		s_surface[s_numSurfaces++] = bits;
	}
	fclose(f);
	hud::log("surfaud: %d surfaces", s_numSurfaces);
	return true;
}

static uint16_t audioOf(uint8_t surface) { return surface < s_numSurfaces ? s_surface[surface] : (uint16_t)SA_CONCRETE; }

void preload() {
	aebank::requestBank(ae::BANK_FEET_GENERIC, true);
	aebank::pin(ae::BANK_GENRL_WEAPONS, ae::SND_WEAPONS_WEAPON_SWIPE_FIST);
	for (uint16_t i = 0; i < 4; i++) aebank::find(ae::BANK_GENRL_WEAPONS, ae::SND_WEAPONS_BODY_PUNCH1 + i);
}

static void play(uint16_t bank, uint16_t id, const Pos& p, float volume, float speed, int16_t event) {
	Params s;
	s.bank = bank;
	s.id = id;
	s.x = p.x; s.y = p.y; s.z = p.z;
	s.volume = volume;
	s.speed = speed;
	s.freqVariance = 0.0588f;
	s.flags = aesound::SOUND_START_PERCENTAGE;
	s.owner = aesound::OWNER_PED;
	s.event = event;
	aesound::play(s);
}

void footstep(bool left, float volume, float speed, uint8_t surface, const Pos& p) {
	const int16_t event = left ? ae::AE_PED_FOOTSTEP_LEFT : ae::AE_PED_FOOTSTEP_RIGHT;
	audio::stats().footsteps++;
	volume += ae::defaultVolume(event) + (float)ae::randInt(-3, 3);
	auto generic = [&] { play(ae::BANK_FEET_GENERIC, (uint16_t)ae::randInt(1, 5), p, volume, speed, event); };
	// TryPlayPlayerFootstepSound: the surface bank if it is in, else the
	// generic step while it loads (one surface bank resident at a time on
	// PC; here the cache's LRU lets the old one go)
	auto surfaceBank = [&](uint16_t bank, int last) {
		if (aebank::bankReady(bank)) {
			const uint16_t id = (uint16_t)ae::randInt(0, last);
			if (aebank::find(bank, id) >= 0) {
				play(bank, id, p, volume, speed, event);
				return;
			}
		}
		aebank::requestBank(bank, false);
		generic();
	};
	const uint16_t a = audioOf(surface);
	if (a & SA_WATER) {
		if (aebank::find(ae::BANK_GENRL_SWIMMING, 0) >= 0) play(ae::BANK_GENRL_SWIMMING, (uint16_t)ae::randInt(0, 4), p, volume, speed, event);
		else aebank::requestBank(ae::BANK_GENRL_SWIMMING, false);
	} else if (a & SA_CONCRETE) {
		generic();
	} else if (a & (SA_GRASS | SA_LONG_GRASS)) {
		surfaceBank(ae::BANK_FEET_GRASS, 4);
	} else if (a & SA_SAND) {
		surfaceBank(ae::BANK_FEET_SAND, 3);
	} else if (a & SA_GRAVEL) {
		surfaceBank(ae::BANK_FEET_GRAVEL, 4);
	} else if (a & SA_WOOD) {
		surfaceBank(ae::BANK_FEET_WOOD, 4);
	} else if (a & SA_TILE) {
		surfaceBank(ae::BANK_FEET_TILE, 4);
	} else if (a & SA_METAL) {
		surfaceBank(ae::BANK_FEET_METAL, 4);
	} else {
		generic();
	}
}

void landing(bool collapse, uint8_t surface, const Pos& p) {
	const int16_t event = collapse ? ae::AE_PED_COLLAPSE_AFTER_FALL : ae::AE_PED_LAND_ON_FEET_AFTER_FALL;
	const float volume = ae::defaultVolume(event);
	audio::stats().landings++;
	if (audioOf(surface) & SA_WATER) {
		if (aebank::find(ae::BANK_GENRL_SWIMMING, 0) >= 0)
			play(ae::BANK_GENRL_SWIMMING, (uint16_t)ae::randInt(0, 4), p, volume > 0.0f ? volume : 0.0f, 1.0f, event);
		return;
	}
	play(ae::BANK_FEET_GENERIC, collapse ? 0 : 6, p, volume, 1.0f, event);
}

void swing(int weaponType, const Pos& p) {
	// eWeaponType: fists / knuckles swish, blades ring, the rest whoosh
	uint16_t id = ae::SND_WEAPONS_WEAPON_SWIPE;
	audio::stats().swings++;
	if (weaponType <= 1) id = ae::SND_WEAPONS_WEAPON_SWIPE_FIST;
	else if (weaponType == 4 || weaponType == 8 || weaponType == 9) id = ae::SND_WEAPONS_WEAPON_SWIPE_METAL;
	Params s;
	s.bank = ae::BANK_GENRL_WEAPONS;
	s.id = id;
	s.x = p.x; s.y = p.y; s.z = p.z;
	s.volume = ae::defaultVolume(ae::AE_PED_SWING) + (float)ae::randInt(-2, 2);
	s.freqVariance = 0.06f;
	s.owner = aesound::OWNER_PED;
	s.event = ae::AE_PED_SWING;
	aesound::play(s);
}

// gPainSpeechLookup[ctx][VOICE_PAIN_CARL] = first..last sound of PAIN_A_CARL,
// gSpeechContextLookup's repeat time per context
struct PainCtx { uint16_t first, last; uint16_t repeatMs; float volume; };
static const PainCtx k_pain[] = {
	{ 0, 13, 3000, 0.0f },    // CTX_PAIN_CJ_BOXING
	{ 64, 78, 3000, 0.0f },   // CTX_PAIN_CJ_HIGH_FALL
};
static uint32_t s_lastPain[sizeof k_pain / sizeof k_pain[0]];

void sayPain(Pain ctx, const Pos& p) {
	const PainCtx& c = k_pain[ctx];
	const uint32_t now = ae::timeMs();
	if (s_lastPain[ctx] && now - s_lastPain[ctx] < c.repeatMs) return;
	// one voice at a time (a speech slot)
	if (aesound::playing(ae::AE_UNDEFINED, aesound::OWNER_SPEECH)) return;
	const uint16_t id = (uint16_t)ae::randInt(c.first, c.last);
	if (aebank::find(ae::BANK_PAIN_A_CARL, id) < 0) {
		// speech is streamed in per line on PC too; the next one will be there
		for (uint16_t i = c.first; i <= c.last; i++) aebank::find(ae::BANK_PAIN_A_CARL, i);
		return;
	}
	s_lastPain[ctx] = now;
	Params s;
	s.bank = ae::BANK_PAIN_A_CARL;
	s.id = id;
	s.x = p.x; s.y = p.y; s.z = p.z;
	s.volume = c.volume;
	s.owner = aesound::OWNER_SPEECH;
	aesound::play(s);
}

// gPainSpeechLookup rows CTX_PAIN_PED_DEATH_HIGH / DEATH_LOW / GENRIC / PANIC,
// columns MALE (PAIN_A_MALE, 101 sounds) and FEMALE (PAIN_A_FEMALE, 131)
static const PainCtx k_pedPain[4][2] = {
	{ { 25, 35, 2000, 0.0f }, { 7, 18, 2000, 0.0f } },
	{ { 36, 45, 2000, 0.0f }, { 19, 35, 2000, 0.0f } },
	{ { 46, 87, 2000, 0.0f }, { 36, 68, 2000, 0.0f } },
	{ { 103, 119, 8000, 0.0f }, { 84, 94, 8000, 0.0f } },
};

void sayPedPain(bool female, PedPain ctx, const Pos& p) {
	const PainCtx& c = k_pedPain[ctx][female ? 1 : 0];
	const uint16_t bank = female ? ae::BANK_PAIN_A_FEMALE : ae::BANK_PAIN_A_MALE;
	if (aesound::playing(ae::AE_UNDEFINED, aesound::OWNER_SPEECH) >= 2) return;
	const uint16_t id = (uint16_t)ae::randInt(c.first, c.last);
	if (aebank::find(bank, id) < 0) return;   // queued: the next line of this context will be in
	Params s;
	s.bank = bank;
	s.id = id;
	s.x = p.x; s.y = p.y; s.z = p.z;
	s.volume = c.volume;
	s.freqVariance = 0.04f;
	s.owner = aesound::OWNER_SPEECH;
	aesound::play(s);
}

void pedHit(const Pos& p) {
	const uint16_t id = (uint16_t)(ae::SND_WEAPONS_BODY_PUNCH1 + ae::randInt(0, 3));
	if (aebank::find(ae::BANK_GENRL_WEAPONS, id) < 0) return;
	Params s;
	s.bank = ae::BANK_GENRL_WEAPONS;
	s.id = id;
	s.x = p.x; s.y = p.y; s.z = p.z;
	s.volume = ae::defaultVolume(ae::AE_PED_HIT_HIGH_UNARMED);
	s.freqVariance = 0.06f;
	s.owner = aesound::OWNER_PED;
	s.event = ae::AE_PED_HIT_HIGH_UNARMED;
	aesound::play(s);
}

void bulletHitPed(const Pos& p) {
	const uint16_t id = (uint16_t)ae::randInt(7, 9);   // BULPED1L..3L
	if (aebank::find(ae::BANK_GENRL_BULLET_HITS, id) < 0) return;
	Params s;
	s.bank = ae::BANK_GENRL_BULLET_HITS;
	s.id = id;
	s.x = p.x; s.y = p.y; s.z = p.z;
	s.volume = ae::defaultVolume(ae::AE_BULLET_HIT);
	s.freqVariance = 0.08f;
	s.owner = aesound::OWNER_PED;
	s.event = ae::AE_BULLET_HIT;
	aesound::play(s);
}

}
