#include "platform/video.h"

#include <nds.h>

namespace video {

void init() {
	powerOn(POWER_ALL);
	lcdMainOnTop();

	videoSetMode(MODE_0_3D);
	videoSetModeSub(MODE_0_2D);

	vramSetBankA(VRAM_A_TEXTURE);
	vramSetBankB(VRAM_B_TEXTURE);
	vramSetBankC(VRAM_C_SUB_BG);
	vramSetBankE(VRAM_E_TEX_PALETTE);

	glInit();
	glEnable(GL_TEXTURE_2D | GL_ANTIALIAS);
	glClearColor(10, 14, 22, 31);
	glClearPolyID(63);
	glClearDepth(GL_MAX_DEPTH);
	glViewport(0, 0, 255, 191);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(60, 256.0f / 192.0f, 0.1f, 200.0f);
	glMatrixMode(GL_MODELVIEW);
}

void beginFrame() {
	glLoadIdentity();
}

void endFrame() {
	glFlush(0);
	swiWaitForVBlank();
}

}
