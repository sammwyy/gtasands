#include "render/renderer.h"

#include <nds.h>
#include <math.h>

#include "game/cj.h"
#include "game/player.h"

namespace renderer {

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
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);
	glBegin(GL_QUADS);
	glColor3b(150, 140, 120);
	glTexCoord2t16(0, 0);
	glVertex3v16(floattov16(x0), 0, floattov16(z1)); glVertex3v16(floattov16(x1), 0, floattov16(z1));
	glVertex3v16(floattov16(x1), floattov16(t), floattov16(z1)); glVertex3v16(floattov16(x0), floattov16(t), floattov16(z1));
	glVertex3v16(floattov16(x1), 0, floattov16(z0)); glVertex3v16(floattov16(x0), 0, floattov16(z0));
	glVertex3v16(floattov16(x0), floattov16(t), floattov16(z0)); glVertex3v16(floattov16(x1), floattov16(t), floattov16(z0));
	glColor3b(110, 100, 90);
	glVertex3v16(floattov16(x1), 0, floattov16(z1)); glVertex3v16(floattov16(x1), 0, floattov16(z0));
	glVertex3v16(floattov16(x1), floattov16(t), floattov16(z0)); glVertex3v16(floattov16(x1), floattov16(t), floattov16(z1));
	glVertex3v16(floattov16(x0), 0, floattov16(z0)); glVertex3v16(floattov16(x0), 0, floattov16(z1));
	glVertex3v16(floattov16(x0), floattov16(t), floattov16(z1)); glVertex3v16(floattov16(x0), floattov16(t), floattov16(z0));
	glColor3b(200, 190, 170);
	glVertex3v16(floattov16(x0), floattov16(t), floattov16(z0)); glVertex3v16(floattov16(x1), floattov16(t), floattov16(z0));
	glVertex3v16(floattov16(x1), floattov16(t), floattov16(z1)); glVertex3v16(floattov16(x0), floattov16(t), floattov16(z1));
	glEnd();
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
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
	const float dist = 3.6f, lookH = 0.85f;
	const float horiz = cosf(pitch) * dist;
	const float eyeH = lookH + sinf(pitch) * dist;
	const float elx = sinf(yaw) * horiz;
	const float ely = -cosf(yaw) * horiz;
	glLoadIdentity();
	glRotatef(pitch * 57.2958f, 1.0f, 0.0f, 0.0f);
	glRotatef(-yaw * 57.2958f, 0.0f, 1.0f, 0.0f);
	glTranslatef(-elx, -eyeH, ely);

	// world patch: anchor on the cell grid nearest the player, scale 8 -> one
	// emitted unit is one ground cell (8 SA units). Anchor delta stays small.
	const int ax = (int)floorf(px * 0.125f), ay = (int)floorf(py * 0.125f);
	const float adx = ax * 8.0f - px, adz = -(ay * 8.0f - py);
	glPushMatrix();
	glTranslatef(adx, 0.0f, adz);
	glScalef(8.0f, 8.0f, 8.0f);
	glDisable(GL_TEXTURE_2D);
	glBegin(GL_QUADS);
	for (int i = -6; i < 6; i++)
		for (int j = -6; j < 6; j++) {
			const int cx = ax + i, cyi = ay + j;    // absolute cell coords (colour only)
			cellQuad(i, -j - 1, (cx + cyi) & 1 ? RGB15(9, 15, 7) : RGB15(10, 10, 11));
		}
	glEnd();
	box(2488.0f - px, -1674.0f - py, 1.5f, 6.0f, adx, adz);
	box(2502.0f - px, -1652.0f - py, 2.0f, 9.0f, adx, adz);
	box(2495.0f - px, -1640.0f - py, 1.0f, 3.0f, adx, adz);
	glEnable(GL_TEXTURE_2D);
	glPopMatrix(1);

	cj::draw(0.0f, 0.0f, player::g.z - player::groundZ(), player::g.heading);
}

int polyCount() { int n = 0; glGetInt(GL_GET_POLYGON_RAM_COUNT, &n); return n; }
int vertCount() { int n = 0; glGetInt(GL_GET_VERTEX_RAM_COUNT, &n); return n; }

}
