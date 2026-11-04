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
	// looped PCM16 ring on a libnds-managed channel; we refill behind the playhead
	int achan = soundPlaySample(ring, SoundFormat_16Bit, 65536, (u16)dh.audioRate,
	                            127, 64, true, 0);

	// frames are already 256x256 letterboxed -> texture is the frame verbatim
	ds::TexHeader* th = (ds::TexHeader*)s_dtex;
	th->magic = DS_FOURCC('D', 'T', 'E', 'X');
	th->width = dh.w;
	th->height = dh.h;
	th->format = 4; // GL_RGB256
	th->flags = 0;
	th->numColors = 256;
	th->texelBytes = frameBytes;
	memcpy(s_dtex + 16 + frameBytes, dh.palette, 512);

	const int framesPerVblank = 60 / (dh.fps ? dh.fps : 10);
	bool done = false;
	// rotate through 3 texture slots: the upload for frame i lands in a slot
	// that was last drawn 2 frames ago (fully rendered by then), and the slot
	// drawn last frame stays allocated so it can't be reused for the upload
	tex::Texture* t[3] = { nullptr, nullptr, nullptr };
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
		int cur = i % 3;
		tex::unload(t[(cur + 2) % 3]); // free the slot drawn 2 frames ago
		t[cur] = tex::load(s_dtex);

		sprite::beginFrame();
		sprite::rect({ 0, 0, 640, 480 }, { 0, 0, 0, 255 });
		sprite::draw(t[cur], { 0, 0, 640, 480 }, { 255, 255, 255, 255 });
		sprite::endFrame();
		glFlush(0);

		input::poll();
		if (i > 10 && (input::get().down & (KEY_A | KEY_B | KEY_START | KEY_X | KEY_Y |
		                                  KEY_L | KEY_R | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)))
			done = true;
		for (int v = 0; v < framesPerVblank; v++) swiWaitForVBlank();
	}

	soundKill(achan);
	tex::unload(t[0]);
	tex::unload(t[1]);
	tex::unload(t[2]);
	fclose(f);
	free(frame);
	free(ring);
	return true;
}

bool play(const char* path) { return playDvid(path); }

}
