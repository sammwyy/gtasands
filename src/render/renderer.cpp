#include "render/renderer.h"

#include <nds.h>

namespace renderer {

static void quad(float x0, float z0, float x1, float z1, float y) {
	glVertex3f(x0, y, z0);
	glVertex3f(x1, y, z0);
	glVertex3f(x1, y, z1);
	glVertex3f(x0, y, z1);
}

static void cube(float x, float z, float s, float h) {
	glBegin(GL_QUADS);
	glColor3b(200, 190, 170);
	quad(x - s, z - s, x + s, z + s, h);
	glColor3b(150, 140, 120);
	glVertex3f(x - s, 0, z + s); glVertex3f(x + s, 0, z + s); glVertex3f(x + s, h, z + s); glVertex3f(x - s, h, z + s);
	glVertex3f(x + s, 0, z - s); glVertex3f(x - s, 0, z - s); glVertex3f(x - s, h, z - s); glVertex3f(x + s, h, z - s);
	glColor3b(110, 100, 90);
	glVertex3f(x + s, 0, z + s); glVertex3f(x + s, 0, z - s); glVertex3f(x + s, h, z - s); glVertex3f(x + s, h, z + s);
	glVertex3f(x - s, 0, z - s); glVertex3f(x - s, 0, z + s); glVertex3f(x - s, h, z + s); glVertex3f(x - s, h, z - s);
	glEnd();
}

void init() {
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

void draw(float camYaw, float camPitch, float camDist) {
	glTranslatef(0, 0, -camDist);
	glRotatef(camPitch, 1, 0, 0);
	glRotatef(camYaw, 0, 1, 0);

	glBindTexture(GL_TEXTURE_2D, 0);
	glBegin(GL_QUADS);
	for (int i = -4; i < 4; i++)
		for (int j = -4; j < 4; j++) {
			if ((i + j) & 1) glColor3b(60, 110, 50); else glColor3b(70, 70, 75);
			quad(i * 4.0f, j * 4.0f, (i + 1) * 4.0f, (j + 1) * 4.0f, 0);
		}
	glEnd();

	cube(-6, -6, 1.5f, 6);
	cube(6, -4, 2.0f, 9);
	cube(0, 7, 1.0f, 3);
}

int polyCount() { int n = 0; glGetInt(GL_GET_POLYGON_RAM_COUNT, &n); return n; }
int vertCount() { int n = 0; glGetInt(GL_GET_VERTEX_RAM_COUNT, &n); return n; }

}
