#include "game/sfx.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/ds_formats.h"
#include "hud/hud.h"
#include "mem/mem.h"

namespace sfx {

// DSND samples are played exactly as stored: the pipeline's IMA-ADPCM is the
// DS hardware format (4-byte header with the initial sample + step index,
// then nibbles low-first, same step table and clamping), so the mixer decodes
// it for free. No PCM16 expansion: a quarter of the RAM and no decode time.
struct Snd { const u8* data = nullptr; u32 bytes = 0; u16 rate = 0; u8 fmt = 0; };

// SND_BANK_GENRL_FRONTEND_MENU (60) -> menu beeps (PERM, loaded once).
static Snd s_menu[16];
// SND_BANK_GENRL_FRONTEND_GAME (59): only the start "whoosh" pair (25/26) is
// used; the other 38 sounds are not loaded until something plays them.
static Snd s_whoosh[2];
// SND_BANK_GENRL_LOADING (82): 4 stereo tune pairs, only the offset table is
// kept - the chosen pair is read (STREAM) when a tune starts.
static u32 s_tuneBankOff, s_tuneOffs[8];
static Snd s_tune[2];
static int s_tuneChan[2] = { -1, -1 };
static int s_tunePair = -1, s_tuneLoaded;
static bool s_ok, s_done;

static SoundFormat hwFormat(u8 fmt) {
	return fmt == ds::SND_ADPCM ? SoundFormat_ADPCM : fmt == ds::SND_PCM16 ? SoundFormat_16Bit : SoundFormat_8Bit;
}

static bool loadSnd(FILE* f, long pos, Snd& out, bool permanent) {
	ds::SoundHeader h;
	if (fseek(f, pos, SEEK_SET) != 0 || fread(&h, sizeof h, 1, f) != 1 || h.magic != DS_FOURCC('D', 'S', 'N', 'D'))
		return false;
	const u32 bytes = (h.dataBytes + 3) & ~3u;   // the blob is padded to a word
	u8* data = (u8*)(permanent ? mem::perm(bytes, mem::TAG_AUDIO) : mem::alloc(bytes, mem::TAG_AUDIO));
	if (!data) { hud::log("\x1b[31msfx: oom %u\x1b[39m", (unsigned)bytes); return false; }
	if (fread(data, 1, bytes, f) != bytes) {
		if (!permanent) mem::free(data);
		return false;
	}
	// the sound DMA reads main RAM: push the samples out of the D-cache
	DC_FlushRange(data, bytes);
	out.data = data;
	out.bytes = bytes;
	out.rate = h.sampleRate;
	out.fmt = h.format;
	return true;
}

// Reads a DSBK bank's offset table; returns its sound count (<= cap).
static int bankOffsets(FILE* f, u32 offset, u32* offs, int cap) {
	ds::BankHeader b;
	if (fseek(f, offset, SEEK_SET) != 0 || fread(&b, sizeof b, 1, f) != 1 || b.magic != DS_FOURCC('D', 'S', 'B', 'K'))
		return 0;
	const int n = b.numSounds < cap ? b.numSounds : cap;
	return (int)fread(offs, 4, n, f);
}

void init() {
	if (s_done) return;
	s_done = true;
	FILE* f = fopen("nitro:/audio/sfx/genrl.dsfx", "rb");
	if (!f) { hud::log("\x1b[33mgenrl.dsfx missing\x1b[39m"); return; }
	ds::SfxPakHeader ph;
	if (fread(&ph, sizeof ph, 1, f) != 1 || ph.magic != DS_FOURCC('D', 'S', 'F', 'X')) { fclose(f); return; }
	u32 offMenu = 0, offGame = 0;
	for (u16 b = 0; b < ph.numBanks; b++) {
		ds::SfxPakBank e;
		if (fread(&e, sizeof e, 1, f) != 1) break;
		if (e.bankId == 60) offMenu = e.offset;
		else if (e.bankId == 59) offGame = e.offset;
		else if (e.bankId == 82) s_tuneBankOff = e.offset;
	}
	u32 offs[40];
	int nMenu = 0;
	const int menuCount = offMenu ? bankOffsets(f, offMenu, offs, 16) : 0;
	for (int i = 0; i < menuCount; i++)
		if (loadSnd(f, offMenu + offs[i], s_menu[i], true)) nMenu = i + 1;
	if (offGame && bankOffsets(f, offGame, offs, 40) > 26) {
		loadSnd(f, offGame + offs[25], s_whoosh[0], true);
		loadSnd(f, offGame + offs[26], s_whoosh[1], true);
	}
	if (s_tuneBankOff && bankOffsets(f, s_tuneBankOff, s_tuneOffs, 8) < 8) s_tuneBankOff = 0;
	fclose(f);
	s_ok = nMenu > 0;
	if (!s_ok) { hud::log("\x1b[33msfx: frontend bank missing\x1b[39m"); return; }
	soundEnable();
	hud::log("sfx: %d menu sounds (adpcm)", nMenu);
}

static int play(const Snd& s, u8 vol, u8 pan, bool loop = false) {
	if (!s.data) return -1;
	// looping ADPCM restarts after the header word: the hardware saves the
	// decoder state there on the first pass and restores it on every loop
	const bool adpcm = s.fmt == ds::SND_ADPCM;
	const u16 loopPt = loop && adpcm ? 1 : 0;
	return soundPlaySample(s.data, hwFormat(s.fmt), s.bytes - loopPt * 4, s.rate, vol, pan, loop, loopPt);
}

static void playPair(const Snd& l, const Snd& r, u8 vol) {
	if (!s_ok) return;
	play(l, vol, 40);
	play(r, vol, 88);
}

void highlight() { playPair(s_menu[4], s_menu[5], 96); }
void select_() { playPair(s_menu[6], s_menu[7], 96); }
void back() { playPair(s_menu[0], s_menu[1], 96); }
void error() { playPair(s_menu[2], s_menu[3], 96); }
void noiseTest() { if (s_ok) play(s_menu[8], 96, 64); }
void start() { playPair(s_whoosh[0], s_whoosh[1], 96); }

// AEFrontendAudioEntity::ServiceLoadingTune: one random stereo pair from the
// LOADING bank, looped. Read one channel per call so the loading screen can
// spread the ~100 KB over two frames; both sides start together.
bool tuneStep() {
	if (!s_ok || !s_tuneBankOff || s_tuneChan[0] >= 0) return true;
	if (s_tunePair < 0) {
		s_tunePair = (rand() % 4) * 2;
		s_tuneLoaded = 0;
	}
	if (s_tuneLoaded < 2) {
		FILE* f = fopen("nitro:/audio/sfx/genrl.dsfx", "rb");
		if (!f) return true;
		loadSnd(f, s_tuneBankOff + s_tuneOffs[s_tunePair + s_tuneLoaded], s_tune[s_tuneLoaded], false);
		fclose(f);
		if (++s_tuneLoaded < 2) return false;
	}
	static const u8 pans[2] = { 40, 88 };
	for (int ch = 0; ch < 2; ch++) s_tuneChan[ch] = play(s_tune[ch], 100, pans[ch], true);
	hud::log("sfx: loading tune %d", s_tunePair / 2);
	return true;
}

void tuneStart() { while (!tuneStep()) {} }

void tuneStop() {
	for (int ch = 0; ch < 2; ch++) {
		if (s_tuneChan[ch] >= 0) soundKill(s_tuneChan[ch]);
		s_tuneChan[ch] = -1;
		mem::free((void*)s_tune[ch].data);
		s_tune[ch] = {};
	}
	s_tunePair = -1;
}

}
