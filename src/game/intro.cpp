#include "game/intro.h"

#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/ds_formats.h"
#include "assets/texture.h"
#include "hud/hud.h"
#include "platform/input.h"
#include "render/sprite2d.h"

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

// DTEX scratch: 16B header + 64KB pixels + 512B palette
static u8 s_dtex[16 + 65536 + 512];

static bool playDvid(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	DvidHeader dh;
	if (fread(&dh, sizeof dh, 1, f) != 1 || memcmp(&dh.magic, "DVID", 4)) {
		fclose(f);
		return false;
	}
	hud::log("intro %s: %uf %ufps %uHz", path, (unsigned)dh.numFrames, (unsigned)dh.fps, (unsigned)dh.audioRate);
	const u32 frameBytes = (u32)dh.w * dh.h;
	u8* frame = (u8*)malloc(frameBytes);
	u8* ring = (u8*)calloc(1, 65536); // ~1.5s @ 22050Hz

	soundEnable();
	u8* wr = ring;
	SAD = (u32)ring;
	TMR = (u16)-(int)(16777216 / dh.audioRate);
	PNT = 0;
	LEN = (u16)(65536 / 4);
	CNT_H = 64 << 8;
	CNT_L = 127 | (1u << 10) | (1u << 12) | (1u << 15); // vol,loop,pcm16,enable

	// fabricate a pal256 DTEX around the frame buffer; DS textures need powers
	// of two -> video rows go into a 256x256 texture (bottom rows stay black)
	memset(s_dtex + 16, 0, 65536);
	ds::TexHeader* th = (ds::TexHeader*)s_dtex;
	th->magic = DS_FOURCC('D', 'T', 'E', 'X');
	th->width = dh.w;
	th->height = dh.h == dh.w ? dh.h : dh.w; // 192 -> 256
	th->format = 4; // GL_RGB256
	th->flags = 0;
	th->numColors = 256;
	th->texelBytes = (u32)th->width * th->height;
	memcpy(s_dtex + 16 + th->texelBytes, dh.palette, 512);

	const int framesPerVblank = 60 / (dh.fps ? dh.fps : 10);
	bool done = false;
	tex::Texture* t = nullptr;
	u32 audioLeft = 0;
	for (u32 i = 0; i < dh.numFrames && !done; i++) {
		if (fread(&audioLeft, 4, 1, f) != 1) audioLeft = 0;
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
		memcpy(s_dtex + 16, frame, frameBytes);
		tex::unload(t);
		t = tex::load(s_dtex);

		sprite::beginFrame();
		sprite::rect({ 0, 0, 640, 480 }, { 0, 0, 0, 255 });
		sprite::draw(t, { 0, 0, 640, 480 }, { 255, 255, 255, 255 });
		sprite::endFrame();
		glFlush(0);

		input::poll();
		if (i > 10 && (input::get().down & (KEY_A | KEY_B | KEY_START | KEY_X | KEY_Y |
		                                  KEY_L | KEY_R | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)))
			done = true;
		for (int v = 0; v < framesPerVblank; v++) swiWaitForVBlank();
	}

	CNT_L = 0;
	tex::unload(t);
	fclose(f);
	free(frame);
	free(ring);
	return true;
}

bool play(const char* path) { return playDvid(path); }

}
