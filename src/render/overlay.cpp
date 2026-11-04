#include "render/overlay.h"

#include <nds.h>

#include "platform/video.h"

namespace overlay {

// z/w of the passes (4096 = far, -4096 = near plane): the colour pass is
// nearer than its outline, both nearer than any scene depth that matters.
static constexpr s32 DEPTH_OUTLINE = -4000;
static constexpr s32 DEPTH_COLOR = -4060;
// one pixel in normalized device coordinates (2 / 256, 2 / 192), 20.12
static constexpr s32 PX_X = 32, PX_Y = 43;

static bool s_outline;

bool outlinePass() { return s_outline; }

void color(u16 rgb15) { glColor(s_outline ? RGB15(1, 1, 2) : rgb15); }

void polyFmt(int alpha) { glPolyFmt(POLY_ALPHA(alpha) | POLY_CULL_NONE | (1 << 13)); }

// Same perspective as the main projection, but z_clip = -depthMul * z_eye
// (constant z/w = depthMul / 4096) and an x/y shift of (ox, oy) in NDC
// (x_clip += ox * w, w = -z_eye).
static void projection(s32 ox, s32 oy, s32 depthMul) {
	const s32 fy = floattof32(1.0f / video::TAN_HALF_FOV_Y), fx = floattof32(1.0f / video::TAN_HALF_FOV_X);
	const m4x4 m = { { fx, 0, 0, 0, 0, fy, 0, 0, -ox, -oy, -depthMul, -4096, 0, 0, 0, 0 } };
	glMatrixMode(GL_PROJECTION);
	glLoadMatrix4x4(&m);
	glMatrixMode(GL_MODELVIEW);
}

void draw(EmitFn fn, void* ctx, bool outline) {
	static const s8 k_off[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
	glBindTexture(GL_TEXTURE_2D, 0);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	s_outline = true;
	for (int i = 0; i < 4 && outline; i++) {
		projection(k_off[i][0] * PX_X, k_off[i][1] * PX_Y, DEPTH_OUTLINE);
		fn(ctx);
	}
	s_outline = false;
	projection(0, 0, DEPTH_COLOR);
	fn(ctx);
	glMatrixMode(GL_PROJECTION);
	glPopMatrix(1);
	glMatrixMode(GL_MODELVIEW);
	glColor(RGB15(31, 31, 31));
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

}
