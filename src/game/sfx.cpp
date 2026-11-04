#include "game/sfx.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static Snd s_sounds[64];
static bool s_ok;

static void loadBank(FILE* f, long base, u32 offset) {
	fseek(f, base + offset, SEEK_SET);
	u32 magic;
	u16 ver, num;
	if (fread(&magic, 4, 1, f) != 1 || magic != 0x4443424B /*'DSBK'*/) return;
	fread(&ver, 2, 1, f);
	fread(&num, 2, 1, f);
	u32* offs = (u32*)malloc(num * 4);
	fread(offs, 4, num, f);
	for (u16 i = 0; i < num; i++) {
		fseek(f, base + offset + offs[i], SEEK_SET);
		u8 hdr[24];
		if (fread(hdr, 24, 1, f) != 1 || memcmp(hdr, "DSND", 4)) continue;
		u8 fmt = hdr[4];
		u16 rate = hdr[6] | (hdr[7] << 8);
		(void)hdr;
		u32 dataBytes = hdr[20] | (hdr[21] << 8) | (hdr[22] << 16) | (hdr[23] << 24);
		u8* raw = (u8*)malloc(dataBytes);
		fread(raw, 1, dataBytes, f);
		int16_t* pcm = nullptr;
		u32 ns = 0;
		if (fmt == 2) {
			pcm = ima_decode(raw, dataBytes, &ns);
		} else if (fmt == 1) {
			pcm = (int16_t*)raw;
			ns = dataBytes / 2;
			raw = nullptr;
		} else {
			pcm = (int16_t*)malloc(dataBytes * 2);
			for (u32 j = 0; j < dataBytes; j++) pcm[j] = (int16_t)((raw[j] ^ 0x80) << 8);
			ns = dataBytes;
		}
		free(raw);
		if (i < 64) {
			s_sounds[i].pcm = pcm;
			s_sounds[i].samples = ns;
			s_sounds[i].rate = rate;
		}
	}
	free(offs);
}

void init() {
	if (s_ok) return;
	FILE* f = fopen("nitro:/audio/sfx/genrl.dsfx", "rb");
	if (!f) return;
	u32 magic;
	u16 ver, nbanks;
	if (fread(&magic, 4, 1, f) != 1 || magic != 0x58465344 /*'DSFX'*/) { fclose(f); return; }
	fread(&ver, 2, 1, f);
	fread(&nbanks, 2, 1, f);
	for (u16 b = 0; b < nbanks; b++) {
		u32 off, size;
		u16 bankId, pad;
		fread(&off, 4, 1, f);
		fread(&size, 4, 1, f);
		fread(&bankId, 2, 1, f);
		fread(&pad, 2, 1, f);
		if (bankId == 60) { // SND_BANK_GENRL_FRONTEND_MENU
			loadBank(f, 0, off);
			break;
		}
	}
	fclose(f);
	soundEnable();
	s_ok = true;
}

static void playPair(u8 l, u8 r, u8 vol) {
	if (!s_ok) return;
	if (s_sounds[l].pcm)
		soundPlaySample(s_sounds[l].pcm, SoundFormat_16Bit, s_sounds[l].samples * 2,
		                s_sounds[l].rate, vol, 40, false, 0);
	if (s_sounds[r].pcm)
		soundPlaySample(s_sounds[r].pcm, SoundFormat_16Bit, s_sounds[r].samples * 2,
		                s_sounds[r].rate, vol, 88, false, 0);
}

void highlight() { playPair(4, 5, 96); }
void select_() { playPair(6, 7, 96); }
void back() { playPair(0, 1, 96); }
void start() { playPair(25, 26, 96); }

}
