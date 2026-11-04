#include "game/intro.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/input.h"
#include "hud/hud.h"

namespace intro {

struct DvidHeader {
	u32 magic;
	u16 version, w, h, fps, reserved;
	u32 numFrames, audioRate;
	u16 palette[256];
};

// channel 0 raw regs: used for the streamed PCM16 track
#define SND0          0x04000400
#define CNT_L         (*(vu16*)(SND0 + 0))
#define CNT_H         (*(vu16*)(SND0 + 2))
#define SAD           (*(vu32*)(SND0 + 4))
#define TMR           (*(vu16*)(SND0 + 8))
#define PNT           (*(vu16*)(SND0 + 10))
#define LEN           (*(vu16*)(SND0 + 12))

static bool playDvid(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	DvidHeader dh;
	if (fread(&dh, sizeof dh, 1, f) != 1 || memcmp(&dh.magic, "DVID", 4)) {
		fclose(f);
		return false;
	}
	hud::log("intro %s: %uf %ufps %uHz", path, dh.numFrames, (unsigned)dh.fps, (unsigned)dh.audioRate);
	const u32 frameBytes = (u32)dh.w * dh.h;
	u8* frame = (u8*)malloc(frameBytes);
	u8* ring = (u8*)calloc(1, 65536); // ~1.5s @ 22050Hz, ring buffer

	videoSetMode(MODE_5_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	int bg = bgInit(2, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
	dmaCopy(dh.palette, BG_PALETTE, 512);
	dmaFillWords(0, bgGetGfxPtr(bg), 256 * 256);

	soundEnable();
	u8* wr = ring;
	SAD = (u32)ring;
	TMR = (u16)-(int)(16777216 / dh.audioRate);
	PNT = 0;
	LEN = (u16)(65536 / 4);
	CNT_H = 64 << 8;
	CNT_L = 127 | (1u << 10) | (1u << 12) | (1u << 15); // vol,loop,pcm16,enable

	const int framesPerVblank = 60 / (dh.fps ? dh.fps : 10);
	bool done = false;
	u32 audioLeft = 0;
	for (u32 i = 0; i < dh.numFrames && !done; i++) {
		if (fread(&audioLeft, 4, 1, f) != 1) audioLeft = 0;
		// write audio chunk into ring (wraps)
		u32 left = audioLeft;
		while (left) {
			u32 space = ring + 65536 - wr;
			u32 n = left < space ? left : space;
			if (fread(wr, 1, n, f) != n) memset(wr, 0, n);
			wr += n;
			if (wr >= ring + 65536) wr = ring;
			left -= n;
		}
		if (fread(frame, 1, frameBytes, f) != frameBytes) break;
		dmaCopy(frame, bgGetGfxPtr(bg), frameBytes);
		// skip on real button press only, after a warmup window
		input::poll();
		if (i > 10 && (input::get().down & (KEY_A | KEY_B | KEY_START | KEY_X | KEY_Y |
		                                  KEY_L | KEY_R | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)))
			done = true;
		for (int v = 0; v < framesPerVblank; v++) swiWaitForVBlank();
	}

	CNT_L = 0;
	fclose(f);
	free(frame);
	free(ring);

	// restore 3D mode for the menu/game
	videoSetMode(MODE_0_3D);
	vramSetBankA(VRAM_A_TEXTURE);
	glInit();
	glEnable(GL_TEXTURE_2D | GL_ANTIALIAS);
	glViewport(0, 0, 255, 191);
	glClearColor(10, 14, 22, 31);
	glClearPolyID(63);
	glClearDepth(GL_MAX_DEPTH);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(60, 256.0f / 192.0f, 0.1f, 200.0f);
	glMatrixMode(GL_MODELVIEW);
	return true;
}

bool play(const char* path) { return playDvid(path); }

}
