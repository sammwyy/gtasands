#include "render/renderer.h"

#include <nds.h>
#include <math.h>

#include "game/cj.h"
#include "game/debugopts.h"
#include "game/player.h"
#include "game/timecycle.h"
#include "game/weather.h"
#include "game/world.h"
#include "render/sky.h"

namespace renderer {

// timecycle::worldLight for the placeholder geometry (u8 multiplier per channel)
static u8 s_lr = 255, s_lg = 255, s_lb = 255;
static inline u16 lit(int r5, int g5, int b5) { return RGB15(r5 * s_lr >> 8, g5 * s_lg >> 8, b5 * s_lb >> 8); }

// --- world geometry ---------------------------------------------------------
// Everything is emitted in player-local space: DS matrix registers are s20.12
// (+-2048), so absolute SA coords (~2500) overflow and clip the whole frame.
// The view transform only sees deltas, which stay tiny.

static void cellQuad(int cx, int cz, u16 color) {
	// CCW when seen from +y (above) so backface culling keeps it
	glColor(color);
	glTexCoord2t16(0, 0);
	glVertex3v16(inttov16(cx), 0, inttov16(cz));
	glVertex3v16(inttov16(cx), 0, inttov16(cz + 1));
	glVertex3v16(inttov16(cx + 1), 0, inttov16(cz + 1));
	glVertex3v16(inttov16(cx + 1), 0, inttov16(cz));
}

// A placeholder landmark: world-anchored box so motion reads. cx/cy are
// player-local SA deltas, s = half size, hgt = height. Vertices are emitted
// relative to the current anchor (adx, adz) at 1/8 of the scale(8) matrix.
static void box(float cx, float cy, float s, float hgt, float adx, float adz) {
	const float x0 = (cx - s - adx) * 0.125f, x1 = (cx + s - adx) * 0.125f;
	const float z0 = (-cy - s - adz) * 0.125f, z1 = (-cy + s - adz) * 0.125f; // SA y -> GL -z
	const float t = hgt * 0.125f;
	if (x1 < -7.5f || x0 > 7.5f || z1 < -7.5f || z0 > 7.5f) return; // outside v16 range
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_FOG);
	glBegin(GL_QUADS);
	glColor(lit(18, 17, 15));
	glTexCoord2t16(0, 0);
	glVertex3v16(floattov16(x0), 0, floattov16(z1)); glVertex3v16(floattov16(x1), 0, floattov16(z1));
	glVertex3v16(floattov16(x1), floattov16(t), floattov16(z1)); glVertex3v16(floattov16(x0), floattov16(t), floattov16(z1));
	glVertex3v16(floattov16(x1), 0, floattov16(z0)); glVertex3v16(floattov16(x0), 0, floattov16(z0));
	glVertex3v16(floattov16(x0), floattov16(t), floattov16(z0)); glVertex3v16(floattov16(x1), floattov16(t), floattov16(z0));
	glColor(lit(13, 12, 11));
	glVertex3v16(floattov16(x1), 0, floattov16(z1)); glVertex3v16(floattov16(x1), 0, floattov16(z0));
	glVertex3v16(floattov16(x1), floattov16(t), floattov16(z0)); glVertex3v16(floattov16(x1), floattov16(t), floattov16(z1));
	glVertex3v16(floattov16(x0), 0, floattov16(z0)); glVertex3v16(floattov16(x0), 0, floattov16(z1));
	glVertex3v16(floattov16(x0), floattov16(t), floattov16(z1)); glVertex3v16(floattov16(x0), floattov16(t), floattov16(z0));
	glColor(lit(25, 23, 21));
	glVertex3v16(floattov16(x0), floattov16(t), floattov16(z0)); glVertex3v16(floattov16(x1), floattov16(t), floattov16(z0));
	glVertex3v16(floattov16(x1), floattov16(t), floattov16(z1)); glVertex3v16(floattov16(x0), floattov16(t), floattov16(z1));
	glEnd();
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK | POLY_FOG);
}

void init() {
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

void draw() {
	const float px = player::g.x, py = player::g.y;
	const float yaw = player::camYaw();
	const float pitch = player::camPitch();

	// Cam-on-a-string: the eye sits on a sphere around CJ's chest. yaw 0 looks
	// north, so the eye is south of him (behind a north-facing ped). pitch is
	// radians above the horizon, positive looking down. Player-local space
	// keeps the view matrix inside the s20.12 range.
	const float dist = dbg::opts.camDist, lookH = 0.85f;
	const float horiz = cosf(pitch) * dist;
	const float eyeH = lookH + sinf(pitch) * dist;
	const float elx = sinf(yaw) * horiz;
	const float ely = -cosf(yaw) * horiz;

	// sky first (constant far depth), it also latches the frame's fog/clear
	const sky::View view{ px + elx, py + ely, player::groundZ() + eyeH, yaw, pitch };
	sky::draw(view);
	timecycle::worldLight(s_lr, s_lg, s_lb);
	// wet ground reads darker (CWeather::WetRoads; road reflections not ported)
	const int wet = 256 - (int)(weather::g.wetRoads * 64.0f);
	s_lr = (u8)(s_lr * wet >> 8); s_lg = (u8)(s_lg * wet >> 8); s_lb = (u8)(s_lb * wet >> 8);
	u8 lr = 255, lg = 255, lb = 255;
	if (dbg::opts.pedLight) timecycle::pedLight(lr, lg, lb);
	cj::setLight(lr, lg, lb);

	glLoadIdentity();
	glRotatef(pitch * 57.2958f, 1.0f, 0.0f, 0.0f);
	glRotatef(-yaw * 57.2958f, 0.0f, 1.0f, 0.0f);
	glTranslatef(-elx, -eyeH, ely);

	// world patch: anchor on the cell grid nearest the player, scale 8 -> one
	// emitted unit is one ground cell (8 SA units). Anchor delta stays small.
	const int ax = (int)floorf(px * 0.125f), ay = (int)floorf(py * 0.125f);
	const float adx = ax * 8.0f - px, adz = -(ay * 8.0f - py);
	cj::draw(0.0f, 0.0f, player::g.z - player::groundZ(), player::g.heading);

	// map after CJ: the polygon budget drops far map objects, never CJ.
	// light0 = world light (see world.cpp compile(): ambient * light colour)
	glLight(0, RGB15(s_lr >> 3, s_lg >> 3, s_lb >> 3), 0, floattov10(-0.99f), 0);
	world::draw({ px + elx, py + ely, player::groundZ() + eyeH, yaw, pitch, px, py, player::groundZ() });

	if (dbg::opts.ground || !world::ready()) {
		glPushMatrix();
		glTranslatef(adx, 0.0f, adz);
		glScalef(8.0f, 8.0f, 8.0f);
		glDisable(GL_TEXTURE_2D);
		glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK | POLY_FOG);
		glBegin(GL_QUADS);
		for (int i = -6; i < 6; i++)
			for (int j = -6; j < 6; j++) {
				const int cx = ax + i, cyi = ay + j;    // absolute cell coords (colour only)
				cellQuad(i, -j - 1, (cx + cyi) & 1 ? lit(9, 15, 7) : lit(10, 10, 11));
			}
		glEnd();
		box(2488.0f - px, -1674.0f - py, 1.5f, 6.0f, adx, adz);
		box(2502.0f - px, -1652.0f - py, 2.0f, 9.0f, adx, adz);
		box(2495.0f - px, -1640.0f - py, 1.0f, 3.0f, adx, adz);
		glEnable(GL_TEXTURE_2D);
		glPopMatrix(1);
	}

	sky::drawWeatherFx(view);
}

int polyCount() { int n = 0; glGetInt(GL_GET_POLYGON_RAM_COUNT, &n); return n; }
int vertCount() { int n = 0; glGetInt(GL_GET_VERTEX_RAM_COUNT, &n); return n; }

}
