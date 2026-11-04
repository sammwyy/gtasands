#include "audio/aevehicleaudio.h"

#include <initializer_list>
#include <math.h>

#include "audio/aeaudio.h"
#include "audio/aebankloader.h"
#include "audio/aesoundmanager.h"

namespace aeveh {

using aesound::Sound;

// gVehicleAudioSettings (model 400 + i)
struct Settings {
	int8_t type;
	int16_t playerBank, dummyBank;
	float enginePitch;
	int8_t horn;
	float hornPitch;
	int8_t door;
	float volOffset;
};
static const Settings k_settings[] = {
#include "audio/aevehiclesettings.inc"
};
static constexpr int NUM_SETTINGS = sizeof k_settings / sizeof k_settings[0];

// eSoundBank / SoundIDs.h / eAudioEvents (original values)
enum : uint16_t { BANK_COLLISIONS = 39, BANK_EXPLOSIONS = 52, BANK_HORN = 74, BANK_VEHICLE_GEN = 138 };
enum : uint16_t {
	SND_GRASS_SKID_A = 0x06, SND_GRAVEL_SKID_A = 0x08, SND_TARSKIDTWIN1 = 0x18,
	SND_CLLIGHT = 0x20, SND_CLNEW = 0x21, SND_CLOLD = 0x22, SND_CLVAN = 0x23, SND_CTRUCK = 0x24,
	SND_GENERIC_START = 0x25, SND_OPLIGHT = 0x27, SND_OPNEW = 0x28, SND_OPOLD = 0x29, SND_OPVAN = 0x2A, SND_OTRUCK = 0x2B,
	SND_HOOD_FLY = 0x0C,
};
enum : uint16_t { SND_COL_METAL_SCRAPE = 0x02, SND_COL_COLCAR01 = 0x14, SND_COL_COLCAR12 = 0x1C, SND_COL_COLCARPED = 0x1D,
                  SND_COL_SOLID_METAL = 0x41, SND_COL_WINDSCREEN = 0x44 };
enum : uint16_t { SND_EXP_FIRE_LOOP = 0, SND_EXP_DISTANT_L = 1, SND_EXP_LOWBEE_L = 2, SND_EXP_MAIN_L = 3 };
enum : int16_t {
	AE_GENERAL_VEHICLE_SOUND = 77, AE_CAR_BONNET_OPEN = 78, AE_CAR_BONNET_CLOSE = 84, AE_WINDSCREEN_SHATTER = 91,
	AE_CAR_HORN = 95, AE_START_ENGINE = 103, AE_PED_KNOCK_DOWN = 121, AE_EXPLOSION = 129, AE_FIRE_CAR = 133,
};

// CAECollisionAudioEntity has no event volume: its base level
static constexpr float CRASH_VOL = 0.0f;

enum Snd { S_IDLE, S_REV, S_AC, S_CRZ, S_HORN, S_SKID, S_FIRE, S_COUNT };
enum State : uint8_t { ST_OFF, ST_ID, ST_CRZ_DUMMY, ST_AC, ST_CRZ, ST_SPIN };

struct Entity {
	bool used;
	const Settings* s;
	Sound* snd[S_COUNT];
	uint16_t skidId;
	State state;
	uint8_t auGear;
	float crzCount, fadeIn, fadeOut;
	float prevSpeed;
	uint32_t acStartMs, lastCrashMs;
	bool prepared;
};
static Entity s_e[8];

static void onUpdate(Sound& s, int pos) {
	if (pos >= 0) return;
	Entity& e = *(Entity*)s.p.client;
	for (Sound*& p : e.snd)
		if (p == &s) p = nullptr;
}

static void stopSnd(Entity& e, int k) {
	if (e.snd[k]) e.snd[k]->stop();
	e.snd[k] = nullptr;
}

static Sound* play(Entity& e, uint16_t bank, uint16_t id, float x, float y, float z, float vol, float speed, float roll,
                   uint16_t flags = 0, int16_t event = -1, float variance = 0.0f) {
	if (aebank::find(bank, id) < 0) return nullptr;
	aesound::Params p;
	p.bank = bank;
	p.id = id;
	p.x = x; p.y = y; p.z = z;
	p.volume = vol;
	p.speed = speed;
	p.rollOff = roll;
	p.freqVariance = variance;
	p.flags = flags;
	p.owner = aesound::OWNER_VEHICLE;
	p.event = event;
	p.update = onUpdate;
	p.client = &e;
	return aesound::play(p);
}

// a looped engine / horn sound: started once, then only position / speed / volume move
static void loop(Entity& e, int k, uint16_t bank, uint16_t id, const aeveh::Params& vp, float vol, float speed, float roll) {
	if (vol <= ae::VOLUME_SILENCE + 1.0f) { stopSnd(e, k); return; }
	if (!e.snd[k]) e.snd[k] = play(e, bank, id, vp.x, vp.y, vp.z, vol, speed, roll, aesound::SOUND_REQUEST_UPDATES);
	if (Sound* s = e.snd[k]) {
		s->setPosition(vp.x, vp.y, vp.z);
		s->setVolume(vol);
		s->setSpeed(speed);
	}
}

int create(int modelId) {
	for (int i = 0; i < 8; i++)
		if (!s_e[i].used) {
			Entity& e = s_e[i];
			e = {};
			e.used = true;
			const int si = modelId - 400;
			e.s = &k_settings[si >= 0 && si < NUM_SETTINGS ? si : 0];
			return i;
		}
	return -1;
}

void destroy(int h) {
	if ((unsigned)h >= 8 || !s_e[h].used) return;
	for (int k = 0; k < S_COUNT; k++) stopSnd(s_e[h], k);
	s_e[h].used = false;
}

static float lerp(float a, float b, float t) { return a + (b - a) * (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t); }

// CGeneral::GetPiecewiseLinear (GetFreqForIdle's table)
static float freqForIdle(float r) {
	static const float k[5][2] = { { 0.0f, 0.0f }, { 0.075f, 0.7f }, { 0.15f, 1.1f }, { 0.25f, 1.25f }, { 1.0f, 1.7f } };
	if (r <= 0.0f) return 0.0f;
	for (int i = 1; i < 5; i++)
		if (r <= k[i][0]) return k[i - 1][1] + (k[i][1] - k[i - 1][1]) * (r - k[i - 1][0]) / (k[i][0] - k[i - 1][0]);
	return 1.7f;
}

static void prepare(Entity& e) {
	if (e.prepared) return;
	e.prepared = true;
	for (int id = 0; id < 2; id++) if (e.s->dummyBank >= 0) aebank::find((uint16_t)e.s->dummyBank, (uint16_t)id);
	for (int id = 0; id < 3; id++) if (e.s->playerBank >= 0) aebank::find((uint16_t)e.s->playerBank, (uint16_t)id);
	if (e.s->horn >= 0) aebank::find(BANK_HORN, (uint16_t)e.s->horn);
	static const uint16_t k_gen[] = { SND_TARSKIDTWIN1, SND_GENERIC_START, SND_OPNEW, SND_CLNEW, SND_OPOLD, SND_CLOLD };
	for (uint16_t id : k_gen) aebank::find(BANK_VEHICLE_GEN, id);
}

// ProcessDummyVehicleEngine + ProcessDummyStateTransition (both loops, fades)
static void dummyEngine(Entity& e, const aeveh::Params& p, float base) {
	const uint16_t bank = (uint16_t)e.s->dummyBank;
	const float ratio = p.maxVelocity > 0.0f ? fminf(fabsf(p.speed) / p.maxVelocity, 1.0f) : 0.0f;
	const bool idling = ratio < (e.state == ST_CRZ_DUMMY ? 0.15f : 0.2f);
	const State next = idling ? ST_ID : ST_CRZ_DUMMY;
	if (next != e.state) { e.fadeIn = 0.0f; e.fadeOut = 0.0f; }
	if (e.state == ST_OFF) e.fadeIn = e.fadeOut = 1.0f;
	e.state = next;
	e.fadeIn = fminf(1.0f, e.fadeIn + (idling ? 0.1f : 0.05f));
	e.fadeOut = fminf(1.0f, e.fadeOut + (idling ? 0.1f : 0.05f));
	const float idP = fminf(ratio / 0.2f, 1.0f), revP = fmaxf(0.0f, fminf((ratio - 0.15f) / 0.85f, 1.0f));
	const float idVol = lerp(-3.0f, 0.0f, idP), idFrq = lerp(0.85f, 1.2f, idP);
	const float revVol = lerp(-4.5f, 0.0f, revP), revFrq = lerp(0.9f, 1.5f, revP);
	const float pitch = e.s->enginePitch > 0.1f ? e.s->enginePitch : 1.0f;
	const float a = idling ? e.fadeIn : 1.0f - e.fadeIn;      // idle share
	loop(e, S_IDLE, bank, 1, p, a > 0.01f ? base + idVol + ae::audioLog10(a) * 10.0f : ae::VOLUME_SILENCE, idFrq * pitch, 1.5f);
	loop(e, S_REV, bank, 0, p, a < 0.99f ? base + revVol + ae::audioLog10(1.0f - a) * 10.0f : ae::VOLUME_SILENCE,
	     revFrq * pitch, 1.5f);
}

// ProcessPlayerVehicleEngine, the ID / AC / CRZ / wheel spin states
static void playerEngine(Entity& e, const aeveh::Params& p, float base) {
	const uint16_t pb = (uint16_t)e.s->playerBank, db = (uint16_t)e.s->dummyBank;
	const float absSpeed = fabsf(p.speed);
	const float speedRatio = p.maxVelocity > 0.0f ? fminf(absSpeed / p.maxVelocity, 1.0f) : 0.0f;
	const bool reversing = p.speed < -0.001f;
	const float accel = p.gas;                       // ThisAccel / 255
	const bool spin = accel > 150.0f / 255.0f;
	static const float k_perGear[6] = { 0.4f, 0.22f, 0.13f, 0.075f, 0.0f, -0.075f };
	if (e.state == ST_OFF || e.state == ST_CRZ_DUMMY) {
		stopSnd(e, S_REV);
		e.state = ST_ID;
	}
	State want = e.state;
	if (reversing || accel <= 0.0f) want = reversing && spin ? ST_SPIN : ST_ID;
	else if (e.state == ST_ID || e.state == ST_SPIN) want = spin ? ST_AC : ST_ID;
	else if (!spin) want = ST_ID;
	if (want != e.state) {
		if (e.state == ST_AC || e.state == ST_CRZ) {   // lift off: the deceleration sweep
			stopSnd(e, S_AC);
			stopSnd(e, S_CRZ);
			if (Sound* s = play(e, pb, 2, p.x, p.y, p.z, base + lerp(-3.5f, -1.5f, speedRatio), lerp(0.75f, 1.25f, speedRatio), 2.0f))
				(void)s;
		}
		if (want == ST_AC) {
			e.auGear = (uint8_t)(p.gear < 1 ? 1 : p.gear > 4 ? 4 : p.gear);
			e.auGear = e.auGear < 1 ? 1 : e.auGear;
			if (absSpeed < 0.1f) e.auGear = 1;
			stopSnd(e, S_IDLE);
			stopSnd(e, S_AC);
		}
		if (want != ST_SPIN) stopSnd(e, S_REV);
		e.state = want;
		e.crzCount = 0.0f;
	}
	const float pitch = e.s->enginePitch > 0.1f ? e.s->enginePitch : 1.0f;
	switch (e.state) {
	case ST_AC: {
		// the sweep is a one-shot per audio gear: when it ends the next gear
		// starts lower (FrqPerGearFactor), past the last one it cruises
		if (!e.snd[S_AC]) {
			if (e.acStartMs && ae::timeMs() - e.acStartMs > 300) e.auGear++;
			if (e.auGear >= (p.numGears > 1 ? p.numGears : 2) || e.auGear > 5) {
				e.state = ST_CRZ;
				break;
			}
			e.snd[S_AC] = play(e, pb, 0, p.x, p.y, p.z, base, 1.0f, 2.0f, aesound::SOUND_REQUEST_UPDATES);
			e.acStartMs = e.snd[S_AC] ? ae::timeMs() : 0;
		}
		if (Sound* s = e.snd[S_AC]) {
			s->setPosition(p.x, p.y, p.z);
			s->setVolume(base + lerp(-2.0f, 0.0f, e.auGear / 5.0f));
			s->setSpeed((k_perGear[e.auGear < 6 ? e.auGear : 5] + 1.0f) * pitch);
		}
		break;
	}
	case ST_CRZ:
		e.crzCount = absSpeed <= e.prevSpeed - 0.05f ? fmaxf(0.0f, e.crzCount - 0.25f) : fminf(e.crzCount + 1.0f, 150.0f);
		loop(e, S_CRZ, pb, 1, p, base + lerp(0.0f, 2.0f, e.crzCount / 150.0f),
		     (0.925f + (1.25f - 0.925f) * e.crzCount / 150.0f) * pitch, 2.0f);
		break;
	case ST_SPIN:
		loop(e, S_REV, db, 0, p, base + lerp(-4.0f, 0.0f, p.revs), (1.0f + lerp(0.2f, 0.4f, p.revs)) * pitch, 2.0f);
		break;
	default:
		loop(e, S_IDLE, db, 1, p, base + lerp(-3.5f, -1.5f, speedRatio), lerp(0.85f, 1.5f, freqForIdle(speedRatio)) * pitch, 2.0f);
		break;
	}
}

void update(int h, const aeveh::Params& p) {
	if ((unsigned)h >= 8 || !s_e[h].used) return;
	Entity& e = s_e[h];
	prepare(e);
	if (p.wrecked || !p.engineOn || e.s->type != 0 || e.s->dummyBank < 0) {
		for (int k : { S_IDLE, S_REV, S_AC, S_CRZ, S_HORN, S_SKID }) stopSnd(e, k);
		e.state = ST_OFF;
	} else {
		const float base = ae::defaultVolume(AE_GENERAL_VEHICLE_SOUND) + e.s->volOffset - (p.player ? 0.0f : 3.0f);
		if (p.player && e.s->playerBank >= 0) playerEngine(e, p, base);
		else {
			stopSnd(e, S_AC);
			stopSnd(e, S_CRZ);
			dummyEngine(e, p, base);
		}
		// PlayHornOrSiren
		if (p.horn && e.s->horn >= 1)
			loop(e, S_HORN, BANK_HORN, (uint16_t)e.s->horn, p, ae::defaultVolume(AE_CAR_HORN) + e.s->volOffset, e.s->hornPitch, 2.5f);
		else stopSnd(e, S_HORN);
	}
	// ProcessVehicleSkidding: one loop, type by contact surface
	if (!p.wrecked && p.skid > 0.0f) {
		const uint16_t id = p.surface == 9 || p.surface == 10 || (p.surface >= 80 && p.surface <= 83) ? SND_GRASS_SKID_A
		                  : p.surface == 6 ? SND_GRAVEL_SKID_A : SND_TARSKIDTWIN1;
		if (e.skidId != id) stopSnd(e, S_SKID);
		e.skidId = id;
		const float vol = ae::audioLog10(p.skid / 4.0f) * 20.0f + (id == SND_TARSKIDTWIN1 ? 0.0f : -10.0f);
		loop(e, S_SKID, BANK_VEHICLE_GEN, id, p, vol, 0.8f + 0.125f * p.skid, 2.0f);
	} else {
		stopSnd(e, S_SKID);
	}
	// CAEFireAudioEntity for the burning engine / wreck
	if (p.onFire) loop(e, S_FIRE, BANK_EXPLOSIONS, SND_EXP_FIRE_LOOP, p, ae::defaultVolume(AE_FIRE_CAR), 1.0f, 2.0f);
	else stopSnd(e, S_FIRE);
	e.prevSpeed = fabsf(p.speed);
}

void event(int h, Event ev, float x, float y, float z, float intensity) {
	if ((unsigned)h >= 8 || !s_e[h].used) return;
	Entity& e = s_e[h];
	switch (ev) {
	case EV_DOOR_OPEN:
	case EV_DOOR_CLOSE: {
		static const uint16_t k_open[5] = { SND_OPLIGHT, SND_OPOLD, SND_OPNEW, SND_OTRUCK, SND_OPVAN };
		static const uint16_t k_close[5] = { SND_CLLIGHT, SND_CLOLD, SND_CLNEW, SND_CTRUCK, SND_CLVAN };
		const int d = e.s->door < 0 || e.s->door > 4 ? 2 : e.s->door;
		const bool open = ev == EV_DOOR_OPEN;
		play(e, BANK_VEHICLE_GEN, open ? k_open[d] : k_close[d], x, y, z,
		     ae::defaultVolume(open ? AE_CAR_BONNET_OPEN : AE_CAR_BONNET_CLOSE), 1.0f, 1.0f, 0, -1, 0.02f);
		break;
	}
	case EV_START_ENGINE:
		play(e, BANK_VEHICLE_GEN, SND_GENERIC_START, x, y, z, ae::defaultVolume(AE_START_ENGINE), 1.0f, 2.0f, 0, -1, 0.05f);
		break;
	case EV_CRASH: {
		// CAECollisionAudioEntity::PlayOneShotCollisionSound (car vs anything):
		// a colcarNN thump scaled by the impulse, metal on hard hits
		const uint32_t now = ae::timeMs();
		if (now - e.lastCrashMs < 120) break;
		e.lastCrashMs = now;
		const float t = fminf(intensity / 600.0f, 1.0f);
		const float vol = CRASH_VOL + ae::audioLog10(t > 0.02f ? t : 0.02f) * 20.0f;
		play(e, BANK_COLLISIONS, (uint16_t)ae::randInt(SND_COL_COLCAR01, SND_COL_COLCAR12), x, y, z, vol, 1.0f, 1.5f, 0, -1, 0.06f);
		if (t > 0.5f) play(e, BANK_COLLISIONS, SND_COL_SOLID_METAL, x, y, z, vol - 4.0f, 0.9f, 1.5f, 0, -1, 0.06f);
		break;
	}
	case EV_PED_HIT:
		play(e, BANK_COLLISIONS, SND_COL_COLCARPED, x, y, z, ae::defaultVolume(AE_PED_KNOCK_DOWN), 0.94f, 1.5f, 0, -1, 0.06f);
		break;
	case EV_WINDSCREEN:
		play(e, BANK_COLLISIONS, SND_COL_WINDSCREEN, x, y, z, ae::defaultVolume(AE_WINDSCREEN_SHATTER), 1.0f, 1.5f, 0, -1, 0.06f);
		break;
	case EV_PART_OFF:
		play(e, BANK_VEHICLE_GEN, SND_HOOD_FLY, x, y, z, CRASH_VOL - 6.0f, 1.0f, 1.5f, 0, -1, 0.06f);
		play(e, BANK_COLLISIONS, SND_COL_METAL_SCRAPE, x, y, z, CRASH_VOL - 6.0f, 1.2f, 1.5f, 0, -1, 0.06f);
		break;
	case EV_EXPLOSION: {
		// CAEExplosionAudioEntity::AddAudioEvent: main blast + low boom + a distant copy
		const float vol = ae::defaultVolume(AE_EXPLOSION);
		play(e, BANK_EXPLOSIONS, SND_EXP_MAIN_L, x, y, z, vol, ae::randFloat(0.9f, 1.1f), 4.0f);
		play(e, BANK_EXPLOSIONS, SND_EXP_LOWBEE_L, x, y, z, vol, 1.0f, 4.0f);
		play(e, BANK_EXPLOSIONS, SND_EXP_DISTANT_L, x, y, z, vol - 6.0f, 1.0f, 8.0f);
		break;
	}
	}
}

}
