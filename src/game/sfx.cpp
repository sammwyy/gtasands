#include "game/sfx.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/ds_formats.h"
#include "hud/hud.h"

namespace sfx {

static const int16_t STEP[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
	73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
	494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
	2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
	10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t IDX[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

// decode our DSND IMA-ADPCM (4-byte header + nibbles, low first) -> pcm16
static int16_t* ima_decode(const u8* data, u32 bytes, u32* outSamples) {
	int pred = (int16_t)(data[0] | (data[1] << 8));
	int idx = data[2];
	u32 n = (bytes - 4) * 2;
	int16_t* out = (int16_t*)malloc((n + 2) * 2);
	if (!out) { *outSamples = 0; return nullptr; }
	u32 w = 0;
	out[w++] = pred;
	for (u32 i = 4; i < bytes; i++) {
		for (int nib = 0; nib < 2; nib++) {
			int code = nib ? (data[i] >> 4) : (data[i] & 15);
			int step = STEP[idx];
			int vp = step >> 3;
			if (code & 4) vp += step;
			if (code & 2) vp += step >> 1;
			if (code & 1) vp += step >> 2;
			pred = (code & 8) ? pred - vp : pred + vp;
			if (pred > 32767) pred = 32767;
			if (pred < -32767) pred = -32767;
			idx += IDX[code & 7];
			if (idx < 0) idx = 0;
			if (idx > 88) idx = 88;
			out[w++] = (int16_t)pred;
		}
	}
	*outSamples = w;
	return out;
}

struct Snd { int16_t* pcm = nullptr; u32 samples = 0; u16 rate = 0; };
// SND_BANK_GENRL_FRONTEND_MENU (60) -> menu beeps, SND_BANK_GENRL_FRONTEND_GAME
// (59) -> the start "whoosh" + pickup/info sounds.
static Snd s_menu[16], s_game[40];
// SND_BANK_GENRL_LOADING (82): 4 stereo tune pairs, only the offset table is
// kept - the chosen pair is decoded when a tune actually starts (~250KB PCM).
static u32 s_tuneBankOff, s_tuneOffs[8];
static Snd s_tune[2];
static int s_tuneChan[2] = { -1, -1 };
static bool s_ok, s_done;

static bool loadSnd(FILE* f, long pos, Snd& out) {
	fseek(f, pos, SEEK_SET);
	u8 hdr[24];
	if (fread(hdr, 24, 1, f) != 1 || memcmp(hdr, "DSND", 4)) return false;
	u8 fmt = hdr[4];
	u16 rate = hdr[6] | (hdr[7] << 8);
	u32 want = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (hdr[11] << 24); // numSamples
	u32 dataBytes = hdr[20] | (hdr[21] << 8) | (hdr[22] << 16) | (hdr[23] << 24);
	u8* raw = (u8*)malloc(dataBytes);
	if (!raw) { hud::log("\x1b[31msfx: oom raw=%u\x1b[39m", (unsigned)dataBytes); return false; }
	if (fread(raw, 1, dataBytes, f) != dataBytes) { free(raw); return false; }
	int16_t* pcm = nullptr;
	u32 ns = 0;
	if (fmt == 2) {
		pcm = ima_decode(raw, dataBytes, &ns);
		if (!pcm) { free(raw); hud::log("\x1b[31msfx: oom pcm %u\x1b[39m", (unsigned)(dataBytes * 4)); return false; }
	} else if (fmt == 1) {
		pcm = (int16_t*)raw;
		ns = dataBytes / 2;
		raw = nullptr;
	} else {
		pcm = (int16_t*)malloc(dataBytes * 2);
		if (!pcm) { free(raw); hud::log("\x1b[31msfx: oom pcm8 %u\x1b[39m", (unsigned)dataBytes); return false; }
		for (u32 j = 0; j < dataBytes; j++) pcm[j] = (int16_t)((raw[j] ^ 0x80) << 8);
		ns = dataBytes;
	}
	free(raw);
	if (ns > want) ns = want; // adpcm nibbles may pad the last sample
	// the sound DMA reads main RAM: push decoded pcm out of the D-cache
	DC_FlushRange(pcm, ns * 2);
	out.pcm = pcm;
	out.samples = ns;
	out.rate = rate;
	return true;
}

static int loadBank(FILE* f, u32 offset, Snd* out, u16 cap) {
	fseek(f, offset, SEEK_SET);
	u32 magic;
	u16 ver, num;
	if (fread(&magic, 4, 1, f) != 1 || magic != DS_FOURCC('D', 'S', 'B', 'K')) return 0;
	fread(&ver, 2, 1, f);
	fread(&num, 2, 1, f);
	u32* offs = (u32*)malloc(num * 4);
	fread(offs, 4, num, f);
	if (num > cap) num = cap;
	u16 n = 0;
	for (u16 i = 0; i < num; i++)
		if (loadSnd(f, offset + offs[i], out[i])) n = i + 1;
	free(offs);
	return n;
}

void init() {
	if (s_done) return;
	s_done = true;
	FILE* f = fopen("nitro:/audio/sfx/genrl.dsfx", "rb");
	if (!f) { hud::log("\x1b[33mgenrl.dsfx missing\x1b[39m"); return; }
	u32 magic;
	u16 ver, nbanks;
	if (fread(&magic, 4, 1, f) != 1 || magic != DS_FOURCC('D', 'S', 'F', 'X')) { fclose(f); return; }
	fread(&ver, 2, 1, f);
	fread(&nbanks, 2, 1, f);
	// read the whole bank table first: loadBank fseeks around the file and
	// would corrupt the sequential fread position
	u32 offMenu = 0, offGame = 0;
	for (u16 b = 0; b < nbanks; b++) {
		u32 off, size;
		u16 bankId, pad;
		fread(&off, 4, 1, f);
		fread(&size, 4, 1, f);
		fread(&bankId, 2, 1, f);
		fread(&pad, 2, 1, f);
		if (bankId == 60) offMenu = off;
		else if (bankId == 59) offGame = off;
		else if (bankId == 82) s_tuneBankOff = off;
	}
	int nGame = offGame ? loadBank(f, offGame, s_game, 40) : 0;
	int nMenu = offMenu ? loadBank(f, offMenu, s_menu, 16) : 0;
	if (s_tuneBankOff) {
		fseek(f, s_tuneBankOff, SEEK_SET);
		u32 m2;
		u16 v2, n2;
		if (fread(&m2, 4, 1, f) == 1 && m2 == DS_FOURCC('D', 'S', 'B', 'K')) {
			fread(&v2, 2, 1, f);
			fread(&n2, 2, 1, f);
			fread(s_tuneOffs, 4, n2 < 8 ? n2 : 8, f);
		} else {
			s_tuneBankOff = 0;
		}
	}
	fclose(f);
	s_ok = nMenu > 0;
	if (!s_ok) { hud::log("\x1b[33msfx: frontend bank missing\x1b[39m"); return; }
	soundEnable();
	hud::log("sfx: %d menu + %d game sounds", nMenu, nGame);
}

static void play(const Snd& s, u8 vol, u8 pan) {
	if (s.pcm)
		soundPlaySample(s.pcm, SoundFormat_16Bit, s.samples * 2, s.rate, vol, pan, false, 0);
}

static void playPair(const Snd* bank, u8 l, u8 r, u8 vol) {
	if (!s_ok) return;
	play(bank[l], vol, 40);
	play(bank[r], vol, 88);
}

void highlight() { playPair(s_menu, 4, 5, 96); }
void select_() { playPair(s_menu, 6, 7, 96); }
void back() { playPair(s_menu, 0, 1, 96); }
void error() { playPair(s_menu, 2, 3, 96); }
void noiseTest() { if (s_ok) play(s_menu[8], 96, 64); }
void start() { playPair(s_game, 25, 26, 96); }

// AEFrontendAudioEntity::ServiceLoadingTune: one random stereo pair from the
// LOADING bank, looped. Decoded on demand (bank 82 is ~430KB ADPCM).
void tuneStart() {
	if (!s_ok || !s_tuneBankOff || s_tuneChan[0] >= 0) return;
	FILE* f = fopen("nitro:/audio/sfx/genrl.dsfx", "rb");
	if (!f) return;
	const int pair = (rand() % 4) * 2;
	for (int ch = 0; ch < 2; ch++)
		loadSnd(f, s_tuneBankOff + s_tuneOffs[pair + ch], s_tune[ch]);
	fclose(f);
	// decode both sides first, then start them back-to-back: decoding L then R
	// inline took ~100ms and the stereo pair started out of sync
	static const u8 pans[2] = { 40, 88 };
	for (int ch = 0; ch < 2; ch++) {
		if (!s_tune[ch].pcm) continue;
		s_tuneChan[ch] = soundPlaySample(s_tune[ch].pcm, SoundFormat_16Bit,
		                                s_tune[ch].samples * 2, s_tune[ch].rate, 100, pans[ch], true, 0);
	}
	hud::log("sfx: loading tune %d", pair / 2);
}

void tuneStop() {
	for (int ch = 0; ch < 2; ch++) {
		if (s_tuneChan[ch] >= 0) soundKill(s_tuneChan[ch]);
		s_tuneChan[ch] = -1;
		free(s_tune[ch].pcm);
		s_tune[ch].pcm = nullptr;
	}
}

}
