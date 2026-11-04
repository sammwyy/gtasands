#include "render/sprite2d.h"

namespace sprite {

// 2D art goes through an identity projection; quads emit NDC directly
// (virtual 640x480 -> [-1,1]x[+1,-1]). Each quad is drawn slightly nearer so
// later primitives always win the depth test on the same layer.
static float s_z;

void beginFrame() {
	s_z = 0.0f;
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
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

static inline float nx(float x) { return x * (2.0f / 640.0f) - 1.0f; }
static inline float ny(float y) { return 1.0f - y * (2.0f / 480.0f); }

static void quad(const Rect& rt, float u1, float v1, float u2, float v2, u16 c) {
	s_z -= 0.001f; // -z is forward here
	glColor(c);
	glBegin(GL_QUADS);
	glTexCoord2f(u1, v1); glVertex3f(nx(rt.left), ny(rt.bottom), s_z);
	glTexCoord2f(u2, v1); glVertex3f(nx(rt.right), ny(rt.bottom), s_z);
	glTexCoord2f(u2, v2); glVertex3f(nx(rt.right), ny(rt.top), s_z);
	glTexCoord2f(u1, v2); glVertex3f(nx(rt.left), ny(rt.top), s_z);
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
