#include "platform/video.h"

#include <nds.h>
#include <stdio.h>
#include <string.h>

namespace video {

static constexpr u16 DEFAULT_CLEAR = RGB15(10, 14, 22);

static Fog s_fog;
static bool s_fogOn;
static u16 s_clear = DEFAULT_CLEAR;
static bool s_manualSort;

void init() {
	powerOn(POWER_ALL);
	lcdMainOnTop();

	videoSetMode(MODE_0_3D);
	videoSetModeSub(MODE_0_2D);

	vramSetBankA(VRAM_A_TEXTURE);
	vramSetBankB(VRAM_B_TEXTURE);
	vramSetBankC(VRAM_C_SUB_BG);
	vramSetBankD(VRAM_D_TEXTURE);
	vramSetBankE(VRAM_E_TEX_PALETTE);

	glInit();
	glEnable(GL_TEXTURE_2D | GL_ANTIALIAS);
	glClearColor(10, 14, 22, 31);
	glClearPolyID(63);
	glClearDepth(GL_MAX_DEPTH);
	glViewport(0, 0, 255, 191);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(FOV_Y, 256.0f / 192.0f, NEAR_PLANE, FAR_PLANE);
	glMatrixMode(GL_MODELVIEW);
}

void beginFrame() {
	glLoadIdentity();
	s_fogOn = false;
	s_clear = DEFAULT_CLEAR;
	s_manualSort = false;
}

void setFog(const Fog* fog) {
	s_fogOn = fog != nullptr;
	if (fog) s_fog = *fog;
}
void setClearColor(u16 rgb15) { s_clear = rgb15; }
void setManualSort(bool on) { s_manualSort = on; }

static void applyEnv() {
	glClearColor(s_clear & 31, (s_clear >> 5) & 31, (s_clear >> 10) & 31, 31);
	if (!s_fogOn) {
		glDisable(GL_FOG);
		return;
	}
	glFogShift(s_fog.shift);
	glFogOffset(s_fog.offset);
	GFX_FOG_COLOR = s_fog.color | (31 << 16);
	for (int i = 0; i < 32; i++) GFX_FOG_TABLE[i] = s_fog.table[i];
	glEnable(GL_FOG);
}

#ifdef GTASADS_DEBUG
static char s_capPath[64];
static int s_capState;   // 0 idle, 1 requested, 2 capturing

void requestCapture(const char* path) {
	strncpy(s_capPath, path, sizeof s_capPath - 1);
	s_capState = 1;
}

static void stepCapture() {
	if (s_capState == 1) {
		vramSetBankD(VRAM_D_LCD);
		REG_DISPCAPCNT = DCAP_ENABLE | DCAP_MODE(DCAP_MODE_A) | DCAP_SRC_A(DCAP_SRC_A_3DONLY) | DCAP_SIZE(3) |
		                 DCAP_OFFSET(0) | DCAP_BANK(3);
		s_capState = 2;
	} else if (s_capState == 2 && !(REG_DISPCAPCNT & DCAP_ENABLE)) {
		FILE* f = fopen(s_capPath, "wb");
		if (f) {
			const u16 hdr[4] = { 'D' | 'S' << 8, 'C' | 'R' << 8, 256, 192 };
			fwrite(hdr, sizeof hdr, 1, f);
			fwrite(VRAM_D, 2, 256 * 192, f);
			fclose(f);
		}
		vramSetBankD(VRAM_D_TEXTURE);
		s_capState = 0;
	}
}
#endif

void endFrame() {
	glFlush(s_manualSort ? GL_TRANS_MANUALSORT : 0);
	swiWaitForVBlank();
	applyEnv();   // ~20 scanlines before the engine starts rendering the new frame
#ifdef GTASADS_DEBUG
	if (s_capState) stepCapture();
#endif
}

bool textureUploadWindow() {
	const u16 vc = REG_VCOUNT;
	return vc >= 192 && vc < 202;   // the last copy can take ~8 lines (DEBUG, map textures)
}

// z/w of the libnds frustum: (f+n)/(f-n) - 2fn / ((f-n) d); depth = z/w*0x4000 + 0x3FFF
int depth15(float dist) {
	constexpr float F = FAR_PLANE, N = NEAR_PLANE;
	if (dist < N) dist = N;
	const float ndc = (F + N) / (F - N) - 2.0f * F * N / ((F - N) * dist);
	int d = (int)(ndc * 16384.0f) + 0x3FFF;
	return d < 0 ? 0 : d > 0x7FFF ? 0x7FFF : d;
}

float distForDepth15(int depth) {
	constexpr float F = FAR_PLANE, N = NEAR_PLANE;
	const float ndc = (float)(depth - 0x3FFF) / 16384.0f;
	const float den = (F + N) - ndc * (F - N);
	return den <= 0.0f ? 1e9f : 2.0f * F * N / den;
}

}
