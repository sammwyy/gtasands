#include "render/sprite2d.h"

namespace sprite {

// quads are drawn at decreasing depth so later primitives always win the depth
// test (all 2D art shares one layer otherwise and text/rects get rejected)
static float s_z;

void beginFrame() {
	s_z = 0.0f;
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0f, 640.0f, 480.0f, 0.0f, -1.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
}

void endFrame() {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(60, 256.0f / 192.0f, 0.1f, 200.0f);
	glMatrixMode(GL_MODELVIEW);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

static inline u16 rgb(const RGBA& c) { return RGB15(c.r >> 3, c.g >> 3, c.b >> 3); }

static void quad(const Rect& rt, float u1, float v1, float u2, float v2, u16 c) {
	s_z += 0.001f; // z=+1 is nearest in this ortho; each quad slightly nearer
	glColor(c);
	glBegin(GL_QUADS);
	glTexCoord2f(u1, v2); glVertex3f(rt.left, rt.bottom, s_z);
	glTexCoord2f(u2, v2); glVertex3f(rt.right, rt.bottom, s_z);
	glTexCoord2f(u2, v1); glVertex3f(rt.right, rt.top, s_z);
	glTexCoord2f(u1, v1); glVertex3f(rt.left, rt.top, s_z);
	glEnd();
}

void draw(const tex::Texture* t, const Rect& rt, RGBA color) {
	if (!t) return;
	glBindTexture(GL_TEXTURE_2D, t->id);
	quad(rt, 0, 0, 1, 1, rgb(color));
}

void drawUV(const tex::Texture* t, const Rect& rt, float u1, float v1, float u2, float v2, RGBA color) {
	if (!t) return;
	glBindTexture(GL_TEXTURE_2D, t->id);
	quad(rt, u1, v1, u2, v2, rgb(color));
}

void rect(const Rect& rt, RGBA color) {
	glBindTexture(GL_TEXTURE_2D, 0);
	quad(rt, 0, 0, 0, 0, rgb(color));
}

}
