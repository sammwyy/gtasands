#include "render/sprite2d.h"

namespace sprite {

// 2D art goes through an identity projection; quads emit NDC directly
// (virtual 640x480 -> [-1,1]x[+1,-1]). Each quad is drawn slightly nearer so
// later primitives always win the depth test on the same layer.
static int s_z16;   // depth step in v16 units (per-quad decrement)
static u8 s_alpha;
static int s_bound; // last glBindTexture id; binds reconfigure VRAM banks (costly)

void beginFrame() {
	s_z16 = 0;
	s_alpha = 31;
	s_bound = -1;
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
}

void alpha(u8 a) { s_alpha = a > 31 ? 31 : a; }

void endFrame() {
	s_bound = -1;
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluPerspective(60, 256.0f / 192.0f, 0.1f, 200.0f);
	glMatrixMode(GL_MODELVIEW);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

static inline u16 rgb(const RGBA& c) { return RGB15(c.r >> 3, c.g >> 3, c.b >> 3); }

// same-texture runs (font glyphs) would otherwise rebind VRAM banks per quad
static inline void bind(int id) {
	if (id != s_bound) { glBindTexture(GL_TEXTURE_2D, id); s_bound = id; }
}

// virtual coords -> v16 NDC without soft-float: x*8 granularity (1/8px)
static inline v16 nx(int x8) { return (v16)((x8 - 2560) * 8 / 5); }
static inline v16 ny(int y8) { return (v16)((1920 - y8) * 32 / 15); }

static void quad(const Rect& rt, float u1, float v1, float u2, float v2, u16 c) {
	s_z16 -= 4; // -z is forward here (~0.001 in f32 terms)
	glPolyFmt(POLY_ALPHA(s_alpha) | POLY_CULL_NONE);
	glColor(c);
	glBegin(GL_QUADS);
	glTexCoord2t16((t16)(u1 * 4096), (t16)(v1 * 4096)); glVertex3v16(nx((int)(rt.left * 8)), ny((int)(rt.bottom * 8)), (v16)s_z16);
	glTexCoord2t16((t16)(u2 * 4096), (t16)(v1 * 4096)); glVertex3v16(nx((int)(rt.right * 8)), ny((int)(rt.bottom * 8)), (v16)s_z16);
	glTexCoord2t16((t16)(u2 * 4096), (t16)(v2 * 4096)); glVertex3v16(nx((int)(rt.right * 8)), ny((int)(rt.top * 8)), (v16)s_z16);
	glTexCoord2t16((t16)(u1 * 4096), (t16)(v2 * 4096)); glVertex3v16(nx((int)(rt.left * 8)), ny((int)(rt.top * 8)), (v16)s_z16);
	glEnd();
}

void draw(const tex::Texture* t, const Rect& rt, RGBA color) {
	if (!t) return;
	bind(t->id);
	// v=0 is the first texel row in memory = the image top on DS
	quad(rt, 0, 1, 1, 0, rgb(color));
}

void drawUV(const tex::Texture* t, const Rect& rt, float u1, float v1, float u2, float v2, RGBA color) {
	if (!t) return;
	bind(t->id);
	quad(rt, u1, v1, u2, v2, rgb(color));
}

void rect(const Rect& rt, RGBA color) {
	bind(0);
	quad(rt, 0, 0, 0, 0, rgb(color));
}

}
